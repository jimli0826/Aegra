#include "aegra/adapters/hyperv/hyperv_boot_check_provider.h"

#include "hyperv_powershell.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace aegra::adapters::hyperv {
namespace {

using detail::PowerShellRunner;
using detail::quote_powershell;
using virtualization::BootCheckVmState;

constexpr const char* kProviderUnavailable = "bootcheck.provider_unavailable";
constexpr const char* kVmCreateFailed = "bootcheck.vm_create_failed";
constexpr const char* kVmStartFailed = "bootcheck.vm_start_failed";
constexpr const char* kScreenshotFailed = "bootcheck.screenshot_failed";
constexpr const char* kCleanupIncomplete = "bootcheck.cleanup_incomplete";
constexpr std::string_view kReadyMarker = "aegra-hyperv-ready";
constexpr std::uint16_t kScreenshotWidth = 640;
constexpr std::uint16_t kScreenshotHeight = 480;

[[nodiscard]] base::Error stable_error(const base::ErrorCode code, const char* message) {
    return base::Error{code, message};
}

/// Job identity mirrors the VirtualBox layout: everything lives in the private
/// job directory; the parent VHDX must sit at <job>/present/disk.vhdx.
struct HyperVJobLayout final {
    std::string parent_vhdx;
    std::string vm_directory;
    std::string child_vhdx;
    std::string vm_name;
    std::string notes_marker;
    bool uefi{true};
};

[[nodiscard]] bool safe_job_id(const std::string_view job_id) noexcept {
    if (job_id.empty() || job_id.size() > 64) {
        return false;
    }
    return std::ranges::all_of(
        job_id, [](const unsigned char value) { return std::isalnum(value) != 0 || value == '-'; });
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

[[nodiscard]] base::Result<HyperVJobLayout>
prepare_job_layout(const virtualization::BootCheckVmRequest& request) {
    if (!safe_job_id(request.job_id) || request.cpu_count == 0 || request.cpu_count > 32 ||
        request.memory_mib < 1024 || request.memory_mib > 32768 ||
        request.overlay_limit_bytes == 0) {
        return base::Result<HyperVJobLayout>::failure(
            stable_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    std::error_code error;
    const auto root = std::filesystem::path(request.job_directory).lexically_normal();
    const auto parent = std::filesystem::path(request.parent_disk_path).lexically_normal();
    const auto expected_parent = (root / "present" / "disk.vhdx").lexically_normal();
    if (!root.is_absolute() || !std::filesystem::is_directory(root, error) || error ||
        parent != expected_parent || !std::filesystem::is_regular_file(parent, error) || error) {
        return base::Result<HyperVJobLayout>::failure(
            stable_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    const auto vm_directory = root / "vm";
    std::filesystem::create_directories(vm_directory, error);
    const auto child = root / "child.vhdx";
    if (error || std::filesystem::exists(child, error) || error) {
        return base::Result<HyperVJobLayout>::failure(
            stable_error(base::ErrorCode::kConflict, kVmCreateFailed));
    }
    HyperVJobLayout layout;
    layout.parent_vhdx = path_utf8(parent);
    layout.vm_directory = path_utf8(vm_directory);
    layout.child_vhdx = path_utf8(child);
    layout.vm_name = "Aegra-BootCheck-" + request.job_id;
    layout.notes_marker = "aegra-bootcheck:" + request.job_id;
    layout.uefi = request.firmware == virtualization::BootFirmware::kUefi;
    return base::Result<HyperVJobLayout>::success(std::move(layout));
}

[[nodiscard]] base::Result<void> run_required(PowerShellRunner& runner, const std::string& script,
                                              const char* const message_code,
                                              const base::CancellationToken cancellation) {
    auto result = runner.run(script, cancellation);
    if (!result) {
        if (result.error().code == base::ErrorCode::kCancelled) {
            return base::Result<void>::failure(result.error());
        }
        return base::Result<void>::failure(stable_error(base::ErrorCode::kIoFailure, message_code));
    }
    if (result.value().exit_code != 0) {
        return base::Result<void>::failure(stable_error(base::ErrorCode::kIoFailure, message_code));
    }
    return base::Result<void>::success();
}

[[nodiscard]] BootCheckVmState parse_vm_state(const std::string_view output) {
    if (output.find("Running") != std::string_view::npos) {
        return BootCheckVmState::kRunning;
    }
    if (output.find("Paused") != std::string_view::npos ||
        output.find("Saved") != std::string_view::npos) {
        return BootCheckVmState::kPaused;
    }
    if (output.find("OffCritical") != std::string_view::npos) {
        return BootCheckVmState::kAborted;
    }
    if (output.find("Off") != std::string_view::npos) {
        return BootCheckVmState::kPoweredOff;
    }
    return BootCheckVmState::kUnknown;
}

[[nodiscard]] std::string clipped_powershell_output(const std::string_view output) {
    std::string clipped;
    clipped.reserve((std::min)(output.size(), static_cast<std::size_t>(240)));
    for (std::size_t index = 0; index < output.size() && clipped.size() < 240; ++index) {
        const auto character = static_cast<unsigned char>(output[index]);
        if (character == '\r' || character == '\n' || character == '\t') {
            if (!clipped.empty() && clipped.back() != ' ') {
                clipped.push_back(' ');
            }
            continue;
        }
        if (character >= 0x20 && character != 0x7F) {
            clipped.push_back(static_cast<char>(character));
        }
    }
    return clipped;
}

[[nodiscard]] base::Error screenshot_io_error(const std::string_view output) {
    auto message = std::string(kScreenshotFailed);
    if (const auto clipped = clipped_powershell_output(output); !clipped.empty()) {
        message.append(": ");
        message.append(clipped);
    }
    return {base::ErrorCode::kIoFailure, std::move(message)};
}

/// TargetSystem is Msvm_ComputerSystem (CIM REF), not VirtualSystemSettingData.
/// $input is a PowerShell automatic variable and cannot carry method parameters.
/// The WQL filter value must itself be quoted (ElementName='name'); vm_name is
/// restricted to [A-Za-z0-9-] by safe_job_id, so no WQL escaping is needed.
/// ImageData may carry a few trailing bytes beyond width*height*2 (observed +4 on
/// Hyper-V 10.0.26100); only the leading RGB565 payload is copied into the bitmap.
[[nodiscard]] std::string make_screenshot_script(const std::string& vm_name,
                                                 const std::string& destination_path) {
    const auto vm = quote_powershell(vm_name);
    const auto destination = quote_powershell(destination_path);
    const auto expected_bytes =
        std::to_string(static_cast<unsigned>(kScreenshotWidth) * kScreenshotHeight * 2U);
    return "$ProgressPreference='SilentlyContinue'; Add-Type -AssemblyName System.Drawing; "
           "$ns='root\\virtualization\\v2'; "
           "$vm=Get-WmiObject -Namespace $ns -Class Msvm_ComputerSystem -Filter "
           "(\"ElementName='\" + " +
           vm +
           " + \"'\"); if ($null -eq $vm) { throw 'VM missing' }; "
           "$service=Get-WmiObject -Namespace $ns -Class "
           "Msvm_VirtualSystemManagementService; "
           "$params=$service.GetMethodParameters('GetVirtualSystemThumbnailImage'); "
           "$params.TargetSystem=$vm.__PATH; "
           "$params.WidthPixels=[uint16]" +
           std::to_string(kScreenshotWidth) +
           "; $params.HeightPixels=[uint16]" + std::to_string(kScreenshotHeight) +
           "; $result=$service.InvokeMethod('GetVirtualSystemThumbnailImage',$params,$null); "
           "if ($result.ReturnValue -ne 0) { throw ('Thumbnail failed: ' + "
           "$result.ReturnValue) }; [byte[]]$pixels=$result.ImageData; "
           "if ($null -eq $pixels) { throw 'Empty thumbnail' }; "
           "if ($pixels.Length -lt " +
           expected_bytes +
           ") { throw ('Thumbnail too small ' + $pixels.Length) }; "
           "$bitmap=New-Object System.Drawing.Bitmap(" +
           std::to_string(kScreenshotWidth) + "," + std::to_string(kScreenshotHeight) +
           ",[System.Drawing.Imaging.PixelFormat]::Format16bppRgb565); "
           "$rect=New-Object System.Drawing.Rectangle(0,0,$bitmap.Width,$bitmap.Height); "
           "$bits=$bitmap.LockBits($rect,[System.Drawing.Imaging.ImageLockMode]::WriteOnly,"
           "$bitmap.PixelFormat); try { [Runtime.InteropServices.Marshal]::Copy("
           "$pixels,0,$bits.Scan0," +
           expected_bytes + ") } finally { $bitmap.UnlockBits($bits) }; "
           "try { $bitmap.Save(" +
           destination +
           ",[System.Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }";
}

class HyperVVmSession final : public virtualization::IBootCheckVmSession {
  public:
    HyperVVmSession(ports::IProcessLauncher& launcher, std::string powershell_path,
                    HyperVJobLayout layout, std::string provider_version)
        : runner_(launcher, std::move(powershell_path)), layout_(std::move(layout)) {
        info_.vm_name = layout_.vm_name;
        info_.child_medium_path = layout_.child_vhdx;
        info_.provider_version = std::move(provider_version);
    }

    ~HyperVVmSession() override {
        base::CancellationSource bounded;
        std::jthread watchdog([&bounded](const std::stop_token stopped) {
            std::mutex mutex;
            std::condition_variable_any changed;
            std::unique_lock lock(mutex);
            changed.wait_for(lock, stopped, std::chrono::seconds(30), [] { return false; });
            if (!stopped.stop_requested()) {
                bounded.request_stop();
            }
        });
        (void)cleanup(bounded.get_token());
    }

    [[nodiscard]] const virtualization::BootCheckVmInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] base::Result<void> initialize(const virtualization::BootCheckVmRequest& request,
                                                const base::CancellationToken cancellation) {
        const auto vm = quote_powershell(layout_.vm_name);
        auto child =
            run_required(runner_,
                         "New-VHD -Path " + quote_powershell(layout_.child_vhdx) + " -ParentPath " +
                             quote_powershell(layout_.parent_vhdx) + " -Differencing | Out-Null",
                         kVmCreateFailed, cancellation);
        if (!child) {
            return child;
        }
        child_created_ = true;
        auto created = run_required(runner_,
                                    "New-VM -Name " + vm + " -Generation " +
                                        (layout_.uefi ? "2" : "1") + " -MemoryStartupBytes " +
                                        std::to_string(request.memory_mib) + "MB -NoVHD -Path " +
                                        quote_powershell(layout_.vm_directory) + " | Out-Null",
                                    kVmCreateFailed, cancellation);
        if (!created) {
            return created;
        }
        vm_created_ = true;
        auto configured = run_required(
            runner_,
            "Remove-VMNetworkAdapter -VMName " + vm + " -ErrorAction SilentlyContinue; " +
                "Set-VMProcessor -VMName " + vm + " -Count " + std::to_string(request.cpu_count) +
                "; " + "Set-VM -Name " + vm +
                " -StaticMemory -CheckpointType Disabled -AutomaticStartAction Nothing" +
                " -AutomaticStopAction TurnOff -Notes " + quote_powershell(layout_.notes_marker) +
                "; " +
                (layout_.uefi ? "Set-VMFirmware -VMName " + vm + " -EnableSecureBoot Off; "
                              : std::string()) +
                "Add-VMHardDiskDrive -VMName " + vm + " -Path " +
                quote_powershell(layout_.child_vhdx) +
                (layout_.uefi ? "; Set-VMFirmware -VMName " + vm +
                                    " -FirstBootDevice (Get-VMHardDiskDrive -VMName " + vm + ")"
                              : std::string()),
            kVmCreateFailed, cancellation);
        if (!configured) {
            return configured;
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> start(const base::CancellationToken cancellation) override {
        if (!vm_created_) {
            return base::Result<void>::failure(
                stable_error(base::ErrorCode::kConflict, kVmStartFailed));
        }
        return run_required(runner_, "Start-VM -Name " + quote_powershell(layout_.vm_name),
                            kVmStartFailed, cancellation);
    }

    [[nodiscard]] base::Result<BootCheckVmState>
    state(const base::CancellationToken cancellation) override {
        if (!vm_created_) {
            return base::Result<BootCheckVmState>::success(BootCheckVmState::kPoweredOff);
        }
        auto result =
            runner_.run("(Get-VM -Name " + quote_powershell(layout_.vm_name) + ").State.ToString()",
                        cancellation);
        if (!result) {
            return base::Result<BootCheckVmState>::failure(result.error());
        }
        if (result.value().exit_code != 0) {
            return base::Result<BootCheckVmState>::failure(
                stable_error(base::ErrorCode::kIoFailure, kVmStartFailed));
        }
        return base::Result<BootCheckVmState>::success(parse_vm_state(result.value().output));
    }

    [[nodiscard]] base::Result<bool>
    guest_heartbeat_ok(const base::CancellationToken cancellation) override {
        if (!vm_created_) {
            return base::Result<bool>::success(false);
        }
        // Integration Services heartbeat: "Ok*" means the guest OS is up and the
        // heartbeat component is responding. "NoContact" covers both a still-
        // booting guest and one without the service, so it is not OK yet. Guard
        // the null (services disabled) case so ToString never throws.
        auto result = runner_.run("$h = (Get-VM -Name " + quote_powershell(layout_.vm_name) +
                                      ").Heartbeat; if ($h) { $h.ToString() } else { 'NoContact' }",
                                  cancellation);
        if (!result) {
            if (result.error().code == base::ErrorCode::kCancelled) {
                return base::Result<bool>::failure(result.error());
            }
            return base::Result<bool>::success(false);
        }
        if (result.value().exit_code != 0) {
            return base::Result<bool>::success(false);
        }
        return base::Result<bool>::success(result.value().output.find("Ok") != std::string::npos);
    }

    [[nodiscard]] base::Result<void>
    capture_screenshot(const std::string& destination_path,
                       const base::CancellationToken cancellation) override {
        if (!vm_created_ || destination_path.empty()) {
            return base::Result<void>::failure(
                stable_error(base::ErrorCode::kConflict, kScreenshotFailed));
        }
        auto ran = runner_.run(make_screenshot_script(layout_.vm_name, destination_path),
                               cancellation);
        if (!ran) {
            if (ran.error().code == base::ErrorCode::kCancelled) {
                return base::Result<void>::failure(ran.error());
            }
            return base::Result<void>::failure(screenshot_io_error(ran.error().message));
        }
        if (ran.value().exit_code != 0) {
            return base::Result<void>::failure(screenshot_io_error(ran.value().output));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void>
    power_off(const base::CancellationToken cancellation) override {
        if (!vm_created_) {
            return base::Result<void>::success();
        }
        return run_required(runner_,
                            "Stop-VM -Name " + quote_powershell(layout_.vm_name) +
                                " -TurnOff -Force -ErrorAction SilentlyContinue",
                            kCleanupIncomplete, cancellation);
    }

    [[nodiscard]] base::Result<void> cleanup(const base::CancellationToken cancellation) override {
        bool failed = false;
        if (vm_created_) {
            if (!power_off(cancellation)) {
                failed = true;
            }
            auto removed = run_required(
                runner_, "Remove-VM -Name " + quote_powershell(layout_.vm_name) + " -Force",
                kCleanupIncomplete, cancellation);
            if (removed) {
                vm_created_ = false;
            } else {
                failed = true;
            }
        }
        if (child_created_) {
            std::error_code error;
            std::filesystem::remove(std::filesystem::path(layout_.child_vhdx), error);
            if (error) {
                failed = true;
            } else {
                child_created_ = false;
            }
        }
        if (failed) {
            return base::Result<void>::failure(
                stable_error(base::ErrorCode::kIoFailure, kCleanupIncomplete));
        }
        return base::Result<void>::success();
    }

  private:
    PowerShellRunner runner_;
    HyperVJobLayout layout_;
    virtualization::BootCheckVmInfo info_;
    bool child_created_{false};
    bool vm_created_{false};
};

} // namespace

struct HyperVBootCheckProvider::Impl final {
    Impl(ports::IProcessLauncher& process_launcher, HyperVProviderOptions value)
        : launcher(&process_launcher), options(std::move(value)),
          runner(process_launcher, options.powershell_path) {}

    ports::IProcessLauncher* launcher{nullptr};
    HyperVProviderOptions options;
    PowerShellRunner runner;
    std::string version;
};

HyperVBootCheckProvider::HyperVBootCheckProvider(ports::IProcessLauncher& launcher,
                                                 HyperVProviderOptions options)
    : impl_(std::make_unique<Impl>(launcher, std::move(options))) {}

HyperVBootCheckProvider::~HyperVBootCheckProvider() = default;

base::Result<virtualization::BootCheckProviderInfo>
HyperVBootCheckProvider::inspect(const base::CancellationToken cancellation) {
    virtualization::BootCheckProviderInfo info;
    info.provider_name = "Microsoft Hyper-V";
    info.message_code = kProviderUnavailable;
    if (impl_->options.powershell_path.empty()) {
        info.diagnostic = "powershell.exe not discovered";
        return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    // Available = vmms running + Hyper-V module resolvable. Real create errors
    // surface per job; no throwaway probe VM (Hyper-V creation is heavyweight).
    auto probed = impl_->runner.run("$s = Get-Service -Name vmms -ErrorAction SilentlyContinue; "
                                    "$c = Get-Command Get-VM -ErrorAction SilentlyContinue; "
                                    "if ($s -and $s.Status -eq 'Running' -and $c) "
                                    "{ 'aegra-hyperv-ready ' + $c.Version.ToString() }",
                                    cancellation);
    if (!probed) {
        if (probed.error().code == base::ErrorCode::kCancelled) {
            return base::Result<virtualization::BootCheckProviderInfo>::failure(probed.error());
        }
        info.diagnostic = "PowerShell probe did not run: " + probed.error().message;
        return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    const auto marker = probed.value().output.find(kReadyMarker);
    if (probed.value().exit_code != 0 || marker == std::string::npos) {
        info.diagnostic =
            "vmms service or Hyper-V module unavailable, probe exit=" +
            std::to_string(probed.value().exit_code) + ": " +
            probed.value().output.substr(0, (std::min)(probed.value().output.size(),
                                                       static_cast<std::size_t>(300)));
        return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    auto version = probed.value().output.substr(marker + kReadyMarker.size());
    const auto end = version.find_first_of("\r\n");
    impl_->version = "hyperv" + (end == std::string::npos ? version : version.substr(0, end));
    info.available = true;
    info.provider_version = impl_->version;
    info.message_code.clear();
    return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
}

base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>
HyperVBootCheckProvider::create(const virtualization::BootCheckVmRequest& request,
                                const base::CancellationToken cancellation) {
    auto provider = inspect(cancellation);
    if (!provider) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            provider.error());
    }
    if (!provider.value().available) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            stable_error(base::ErrorCode::kNotFound, kProviderUnavailable));
    }
    auto layout = prepare_job_layout(request);
    if (!layout) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            layout.error());
    }
    auto session =
        std::make_unique<HyperVVmSession>(*impl_->launcher, impl_->options.powershell_path,
                                          std::move(layout).value(), impl_->version);
    auto initialized = session->initialize(request, cancellation);
    if (!initialized) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            initialized.error());
    }
    return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::success(
        std::move(session));
}

} // namespace aegra::adapters::hyperv
