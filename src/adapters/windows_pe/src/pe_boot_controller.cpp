#include "aegra/adapters/windows_pe/one_time_boot.h"

#include "pe_boot_internal.h"
#include "pe_pending_internal.h"

#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_pe {
namespace {

using detail::kBootEntryGuid;
using detail::kDeviceOptionsGuid;
using detail::pe_store_error;

inline constexpr std::size_t kMaximumEntryNameSize = 64;

struct SplitBootPath final {
    /// Drive component, e.g. "C:".
    std::string drive;
    /// Volume-relative component with a leading backslash, e.g. "\dir\boot.wim".
    std::string relative;
};

[[nodiscard]] base::Result<void> check_cancelled(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            pe_store_error(base::ErrorCode::kCancelled, "one-time boot operation cancelled"));
    }
    return base::Result<void>::success();
}

/// Splits an absolute drive-letter path and verifies the file exists.
[[nodiscard]] base::Result<SplitBootPath> split_boot_path(const std::string& path_utf8,
                                                          const char* what) {
    const bool drive_absolute =
        path_utf8.size() >= 4 &&
        ((path_utf8[0] >= 'A' && path_utf8[0] <= 'Z') ||
         (path_utf8[0] >= 'a' && path_utf8[0] <= 'z')) &&
        path_utf8[1] == ':' && path_utf8[2] == '\\';
    if (!drive_absolute || path_utf8.find('"') != std::string::npos) {
        std::string message = "boot file path must be an absolute drive-letter path: ";
        message += what;
        return base::Result<SplitBootPath>::failure(
            {base::ErrorCode::kInvalidArgument, std::move(message)});
    }
    auto wide = detail::utf8_to_wide(path_utf8);
    if (!wide) {
        return base::Result<SplitBootPath>::failure(wide.error());
    }
    if (!detail::file_exists(wide.value())) {
        std::string message = "boot file does not exist: ";
        message += what;
        return base::Result<SplitBootPath>::failure(
            {base::ErrorCode::kNotFound, std::move(message)});
    }
    SplitBootPath split;
    split.drive = path_utf8.substr(0, 2);
    split.relative = path_utf8.substr(2);
    return base::Result<SplitBootPath>::success(std::move(split));
}

[[nodiscard]] base::Result<void> validate_entry_name(const std::string& entry_name) {
    if (entry_name.empty() || entry_name.size() > kMaximumEntryNameSize ||
        entry_name.find('"') != std::string::npos) {
        return base::Result<void>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "boot entry name is invalid"));
    }
    return base::Result<void>::success();
}

class WindowsOneTimeBootController final : public ports::IOneTimeBootController {
  public:
    WindowsOneTimeBootController(ports::IProcessLauncher& launcher, std::string bcdedit_path_utf8,
                                OneTimeBootDiagnosticLog diagnostic_log)
        : launcher_(launcher), bcdedit_path_utf8_(std::move(bcdedit_path_utf8)),
          diagnostic_log_(std::move(diagnostic_log)) {}

    [[nodiscard]] base::Result<void>
    arm_once(const ports::OneTimeBootRequest& request,
             const base::CancellationToken cancellation) override {
        if (auto ready = prepare_operation(cancellation); !ready) {
            return ready;
        }
        if (auto valid = validate_entry_name(request.entry_name); !valid) {
            return valid;
        }
        auto wim = split_boot_path(request.wim_path_utf8, "boot.wim");
        auto sdi = split_boot_path(request.sdi_path_utf8, "boot.sdi");
        if (!wim || !sdi) {
            return base::Result<void>::failure(!wim ? wim.error() : sdi.error());
        }
        auto firmware = firmware_kind();
        if (!firmware) {
            return base::Result<void>::failure(firmware.error());
        }
        // Idempotent re-arm: fixed GUIDs must be recreated from a clean slate.
        if (auto cleaned = disarm_internal(); !cleaned) {
            return cleaned;
        }
        if (auto active = check_cancelled(cancellation); !active) {
            return active;
        }
        auto armed = configure_and_sequence(request.entry_name, wim.value(), sdi.value(),
                                            firmware.value());
        if (!armed) {
            // Never leave a half-built entry behind; rollback is best-effort.
            (void)disarm_internal();
            return armed;
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> disarm(const base::CancellationToken cancellation) override {
        if (auto ready = prepare_operation(cancellation); !ready) {
            return ready;
        }
        return disarm_internal();
    }

    [[nodiscard]] base::Result<bool>
    is_armed(const base::CancellationToken cancellation) override {
        if (auto ready = prepare_operation(cancellation); !ready) {
            return base::Result<bool>::failure(ready.error());
        }
        auto entry = object_exists(kBootEntryGuid);
        if (!entry) {
            return entry;
        }
        if (!entry.value()) {
            return base::Result<bool>::success(false);
        }
        return sequence_references_entry();
    }

    [[nodiscard]] base::Result<ports::FirmwareKind> firmware_kind() override {
        FIRMWARE_TYPE firmware = FirmwareTypeUnknown;
        if (!GetFirmwareType(&firmware)) {
            return base::Result<ports::FirmwareKind>::failure(
                detail::win32_error(GetLastError(), "query firmware type"));
        }
        if (firmware == FirmwareTypeBios) {
            return base::Result<ports::FirmwareKind>::success(ports::FirmwareKind::kBios);
        }
        if (firmware == FirmwareTypeUefi) {
            return base::Result<ports::FirmwareKind>::success(ports::FirmwareKind::kUefi);
        }
        return base::Result<ports::FirmwareKind>::failure(
            pe_store_error(base::ErrorCode::kConflict, "firmware type is unsupported"));
    }

  private:
    [[nodiscard]] base::Result<void>
    prepare_operation(const base::CancellationToken cancellation) const {
        if (auto active = check_cancelled(cancellation); !active) {
            return active;
        }
        return detail::require_elevated();
    }

    /// Runs one bcdedit command and emits a diagnostic line (args, exit, output
    /// excerpt) for field triage. Never logs secrets — this flow has none.
    [[nodiscard]] base::Result<detail::ConsoleToolResult>
    run_bcdedit(std::vector<std::string> arguments) {
        std::string joined = "bcdedit";
        for (const auto& argument : arguments) {
            joined += ' ';
            joined += argument;
        }
        auto result = detail::run_console_tool(launcher_, bcdedit_path_utf8_, std::move(arguments));
        if (diagnostic_log_) {
            if (result) {
                std::string line = joined + " -> exit=" + std::to_string(result.value().exit_code);
                if (result.value().terminated) {
                    line += " (terminated)";
                }
                line += "; out=" + detail::condense_console_output(result.value().output);
                diagnostic_log_(line);
            } else {
                diagnostic_log_(joined + " -> launch failed: " + result.error().message);
            }
        }
        return result;
    }

    [[nodiscard]] base::Result<void> run_expect(std::vector<std::string> arguments,
                                                const char* what) {
        auto result = run_bcdedit(std::move(arguments));
        if (!result) {
            return base::Result<void>::failure(result.error());
        }
        return detail::expect_tool_success(result.value(), what);
    }

    /// Existence probe: bcdedit's exit code is unreliable (it frequently returns 0
    /// while printing an error for a missing object), so require the enum output to
    /// echo the object's identifier GUID.
    [[nodiscard]] base::Result<bool> object_exists(const std::string_view guid) {
        auto result = run_bcdedit({"/enum", std::string(guid)});
        if (!result) {
            return base::Result<bool>::failure(result.error());
        }
        const bool present = !result.value().terminated && result.value().exit_code == 0 &&
                             detail::output_references_token(result.value().output, guid);
        return base::Result<bool>::success(present);
    }

    [[nodiscard]] base::Result<bool> sequence_references_entry() {
        auto result = run_bcdedit({"/enum", "{bootmgr}"});
        if (!result) {
            return base::Result<bool>::failure(result.error());
        }
        if (auto success = detail::expect_tool_success(result.value(), "enumerate {bootmgr}");
            !success) {
            return base::Result<bool>::failure(success.error());
        }
        return base::Result<bool>::success(
            detail::output_references_token(result.value().output, kBootEntryGuid));
    }

    /// Deletes one product object idempotently: `/f` is required for device-options
    /// objects, and an object that is already gone is treated as success (re-probe
    /// after a failed delete rather than trusting localized error text).
    [[nodiscard]] base::Result<void> delete_object_if_present(const std::string_view guid) {
        auto exists = object_exists(guid);
        if (!exists) {
            return base::Result<void>::failure(exists.error());
        }
        if (!exists.value()) {
            return base::Result<void>::success();
        }
        auto deleted = run_expect({"/delete", std::string(guid), "/f"}, "delete boot object");
        if (deleted) {
            return deleted;
        }
        auto still_present = object_exists(guid);
        if (still_present && !still_present.value()) {
            return base::Result<void>::success();
        }
        return deleted;
    }

    /// Removes the one-time sequence (when it references our entry) and both
    /// product objects. Succeeds when nothing exists.
    [[nodiscard]] base::Result<void> disarm_internal() {
        auto referenced = sequence_references_entry();
        if (!referenced) {
            return base::Result<void>::failure(referenced.error());
        }
        if (referenced.value()) {
            // bootsequence is a transient one-shot value; deleting it whole is the
            // supported bcdedit operation (there is no per-element removal).
            if (auto cleared = run_expect({"/deletevalue", "{bootmgr}", "bootsequence"},
                                          "clear boot sequence");
                !cleared) {
                return cleared;
            }
        }
        for (const auto guid : {kBootEntryGuid, kDeviceOptionsGuid}) {
            if (auto deleted = delete_object_if_present(guid); !deleted) {
                return deleted;
            }
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void>
    configure_and_sequence(const std::string& entry_name, const SplitBootPath& wim,
                           const SplitBootPath& sdi, const ports::FirmwareKind firmware) {
        const std::string options(kDeviceOptionsGuid);
        const std::string entry(kBootEntryGuid);
        const std::string ramdisk = "ramdisk=[" + wim.drive + "]" + wim.relative + "," + options;
        const std::string winload = firmware == ports::FirmwareKind::kUefi
                                        ? "\\windows\\system32\\winload.efi"
                                        : "\\windows\\system32\\winload.exe";
        const std::string options_name = entry_name + " Options";
        const struct {
            std::vector<std::string> arguments;
            const char* what;
        } steps[] = {
            {{"/create", options, "/d", options_name, "/device"}, "create device options"},
            {{"/set", options, "ramdisksdidevice", "partition=" + sdi.drive}, "set sdi device"},
            {{"/set", options, "ramdisksdipath", sdi.relative}, "set sdi path"},
            {{"/create", entry, "/d", entry_name, "/application", "osloader"},
             "create boot entry"},
            {{"/set", entry, "device", ramdisk}, "set boot device"},
            {{"/set", entry, "osdevice", ramdisk}, "set os device"},
            {{"/set", entry, "path", winload}, "set loader path"},
            {{"/set", entry, "systemroot", "\\windows"}, "set system root"},
            {{"/set", entry, "detecthal", "yes"}, "set detecthal"},
            {{"/set", entry, "winpe", "yes"}, "set winpe flag"},
            {{"/bootsequence", entry}, "set one-time boot sequence"},
        };
        for (const auto& step : steps) {
            if (auto executed = run_expect(step.arguments, step.what); !executed) {
                return executed;
            }
        }
        return base::Result<void>::success();
    }

    ports::IProcessLauncher& launcher_;
    std::string bcdedit_path_utf8_;
    OneTimeBootDiagnosticLog diagnostic_log_;
};

} // namespace

base::Result<std::unique_ptr<ports::IOneTimeBootController>>
open_one_time_boot_controller(const OneTimeBootControllerOpenRequest& request) {
    using Output = base::Result<std::unique_ptr<ports::IOneTimeBootController>>;
    if (request.process_launcher == nullptr) {
        return Output::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "process launcher is required"));
    }
    std::string bcdedit_path = request.bcdedit_path_utf8;
    if (bcdedit_path.empty()) {
        auto resolved = detail::resolve_system_tool_path("bcdedit.exe");
        if (!resolved) {
            return Output::failure(resolved.error());
        }
        bcdedit_path = std::move(resolved).value();
    }
    return Output::success(std::make_unique<WindowsOneTimeBootController>(
        *request.process_launcher, std::move(bcdedit_path), request.diagnostic_log));
}

} // namespace aegra::adapters::windows_pe
