#include "aegra/adapters/windows_pe/pe_image_builder.h"

#include "pe_boot_internal.h"
#include "pe_image_internal.h"
#include "pe_pending_internal.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_pe {
namespace {

using detail::pe_store_error;

inline constexpr std::size_t kMaximumPayloadFiles = 64;

/// Wide and UTF-8 forms of every image path (UTF-8 feeds DISM arguments and the
/// returned PeImagePaths; wide feeds Win32 file operations).
struct ImageLayout final {
    std::wstring image_dir;
    std::wstring boot_wim;
    std::wstring boot_sdi;
    std::wstring build_id;
    std::wstring mount_dir;
    std::wstring legacy_base_wim;
    std::string boot_wim_utf8;
    std::string boot_sdi_utf8;
    std::string mount_dir_utf8;
};

[[nodiscard]] base::Result<void> check_cancelled(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            pe_store_error(base::ErrorCode::kCancelled, "pe image build cancelled"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] bool valid_target_name(const std::string& name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    return name.find_first_of("\\/:\"") == std::string::npos &&
           name.find("..") == std::string::npos;
}

[[nodiscard]] base::Result<void> validate_request(const ports::PeImageBuildRequest& request) {
    if (request.data_dir_utf8.empty() ||
        request.data_dir_utf8.find('"') != std::string::npos) {
        return base::Result<void>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "data directory is invalid"));
    }
    if (request.product_version.empty() || request.product_version.size() > 32) {
        return base::Result<void>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "product version is invalid"));
    }
    if (request.payload.empty() || request.payload.size() > kMaximumPayloadFiles) {
        return base::Result<void>::failure(pe_store_error(base::ErrorCode::kInvalidArgument,
                                                          "payload must contain 1..64 files"));
    }
    bool executor_found = false;
    for (const auto& file : request.payload) {
        if (!valid_target_name(file.target_name) || file.source_path_utf8.empty()) {
            return base::Result<void>::failure(pe_store_error(
                base::ErrorCode::kInvalidArgument, "payload entry is invalid"));
        }
        executor_found = executor_found || file.target_name == request.executor_target_name;
    }
    if (!executor_found) {
        return base::Result<void>::failure(pe_store_error(
            base::ErrorCode::kInvalidArgument,
            "executor_target_name must match exactly one payload entry"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<ImageLayout> layout_for(const std::string& data_dir_utf8) {
    auto wide = detail::utf8_to_wide(data_dir_utf8);
    if (!wide) {
        return base::Result<ImageLayout>::failure(wide.error());
    }
    std::wstring root = std::move(wide).value();
    while (!root.empty() && root.back() == L'\\') {
        root.pop_back();
    }
    std::string root_utf8 = data_dir_utf8;
    while (!root_utf8.empty() && root_utf8.back() == '\\') {
        root_utf8.pop_back();
    }
    if (root.empty() || root_utf8.empty()) {
        return base::Result<ImageLayout>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "data directory is invalid"));
    }
    ImageLayout layout;
    layout.image_dir = root + L"\\" + std::wstring(detail::kImageDirRelative);
    layout.legacy_base_wim =
        layout.image_dir + L"\\" + std::wstring(detail::kLegacyBaseWimFileName);
    layout.boot_wim = layout.image_dir + L"\\" + std::wstring(detail::kBootWimFileName);
    layout.boot_sdi = layout.image_dir + L"\\" + std::wstring(detail::kBootSdiFileName);
    layout.build_id = layout.image_dir + L"\\" + std::wstring(detail::kBuildIdFileName);
    layout.mount_dir = layout.image_dir + L"\\" + std::wstring(detail::kMountDirName);
    const std::string image_dir_utf8 = root_utf8 + "\\pe\\image";
    layout.boot_wim_utf8 = image_dir_utf8 + "\\boot.wim";
    layout.boot_sdi_utf8 = image_dir_utf8 + "\\boot.sdi";
    layout.mount_dir_utf8 = image_dir_utf8 + "\\mount";
    return base::Result<ImageLayout>::success(std::move(layout));
}

/// Deterministic build id document: nlohmann objects serialize with sorted keys,
/// so byte-equality of the dump is a stable cache comparison.
[[nodiscard]] base::Result<std::string>
compute_build_id(const ports::PeImageBuildRequest& request, const std::string& os_build) {
    nlohmann::json payload = nlohmann::json::array();
    for (const auto& file : request.payload) {
        auto wide = detail::utf8_to_wide(file.source_path_utf8);
        if (!wide) {
            return base::Result<std::string>::failure(wide.error());
        }
        auto digest = detail::sha256_file_hex(wide.value());
        if (!digest) {
            return base::Result<std::string>::failure(digest.error());
        }
        payload.push_back({{"name", file.target_name}, {"sha256", digest.value()}});
    }
    const nlohmann::json document = {
        {"job_schema_version", contracts::kPePendingJobSchemaVersion},
        {"os_build", os_build},
        {"payload", std::move(payload)},
        {"product_version", request.product_version},
        {"executor", request.executor_target_name},
        {"debug_shell", request.debug_shell},
    };
    return base::Result<std::string>::success(document.dump(2));
}

[[nodiscard]] std::string stored_build_id(const std::wstring& build_id_path) {
    auto content = detail::read_bounded_file(build_id_path, 64 * 1024);
    if (!content) {
        return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) UTF-8 text view of bytes.
    return std::string(reinterpret_cast<const char*>(content.value().data()),
                       content.value().size());
}

[[nodiscard]] std::string winpeshl_content(const std::string& executor_target_name,
                                           const bool debug_shell) {
    std::string content = "[LaunchApps]\r\n";
    content += "%SYSTEMROOT%\\System32\\wpeinit.exe\r\n";
    if (debug_shell) {
        // Interactive prompt for missing-DLL triage: the technician runs the
        // executor by hand (%SYSTEMROOT%\System32\Aegra\<executor>). PE reboots
        // when cmd.exe exits.
        content += "%SYSTEMROOT%\\System32\\cmd.exe\r\n";
        return content;
    }
    content += "%SYSTEMROOT%\\System32\\Aegra\\";
    content += executor_target_name;
    content += "\r\n";
    return content;
}

class WindowsPeImageBuilder final : public ports::IPeImageBuilder {
  public:
    explicit WindowsPeImageBuilder(ports::IProcessLauncher& launcher, std::string dism_path_utf8)
        : launcher_(launcher), dism_path_utf8_(std::move(dism_path_utf8)) {}

    [[nodiscard]] base::Result<ports::PeImagePaths>
    ensure_ready(const ports::PeImageBuildRequest& request,
                 const base::CancellationToken cancellation) override {
        using Output = base::Result<ports::PeImagePaths>;
        if (auto active = check_cancelled(cancellation); !active) {
            return Output::failure(active.error());
        }
        if (auto elevated = detail::require_elevated(); !elevated) {
            return Output::failure(elevated.error());
        }
        if (auto valid = validate_request(request); !valid) {
            return Output::failure(valid.error());
        }
        auto layout = layout_for(request.data_dir_utf8);
        if (!layout) {
            return Output::failure(layout.error());
        }
        auto os_build = detail::query_os_build_number();
        if (!os_build) {
            return Output::failure(os_build.error());
        }
        auto expected_id = compute_build_id(request, os_build.value());
        if (!expected_id) {
            return Output::failure(expected_id.error());
        }
        ports::PeImagePaths paths;
        paths.wim_path_utf8 = layout.value().boot_wim_utf8;
        paths.sdi_path_utf8 = layout.value().boot_sdi_utf8;
        (void)detail::delete_file_if_exists(layout.value().legacy_base_wim);
        const std::string previous_id = stored_build_id(layout.value().build_id);
        if (previous_id == expected_id.value() && detail::file_exists(layout.value().boot_wim) &&
            detail::file_exists(layout.value().boot_sdi)) {
            return Output::success(std::move(paths));
        }
        if (auto rebuilt = rebuild(request, layout.value(), expected_id.value(), cancellation);
            !rebuilt) {
            return Output::failure(rebuilt.error());
        }
        return Output::success(std::move(paths));
    }

  private:
    [[nodiscard]] base::Result<void> run_dism(std::vector<std::string> arguments,
                                              const char* what) {
        auto result = detail::run_console_tool(launcher_, dism_path_utf8_, std::move(arguments));
        if (!result) {
            return base::Result<void>::failure(result.error());
        }
        return detail::expect_tool_success(result.value(), what);
    }

    /// A crashed previous run can leave the WIM mounted; discard before reuse.
    void self_heal_mount(const ImageLayout& layout) {
        if (!detail::file_exists(layout.mount_dir)) {
            return;
        }
        (void)run_dism({"/Unmount-Wim", "/MountDir:" + layout.mount_dir_utf8, "/Discard"},
                       "discard leftover mount");
        RemoveDirectoryW(layout.mount_dir.c_str());
    }

    [[nodiscard]] base::Result<void> prepare_boot_files(const ImageLayout& layout) {
        auto sources = detail::locate_pe_image_sources(launcher_);
        if (!sources) {
            return base::Result<void>::failure(sources.error());
        }
        if (auto copied = detail::copy_file_writable(sources.value().winre_wim_path,
                                                     layout.boot_wim, "copy winre.wim");
            !copied) {
            return copied;
        }
        return detail::copy_file_writable(sources.value().boot_sdi_path, layout.boot_sdi,
                                          "copy boot.sdi");
    }

    [[nodiscard]] base::Result<void> inject_payload(const ports::PeImageBuildRequest& request,
                                                    const ImageLayout& layout) {
        const std::wstring payload_dir =
            layout.mount_dir + L"\\" + std::wstring(detail::kPayloadDirRelative);
        if (auto directories = detail::ensure_directory_exists(payload_dir); !directories) {
            return directories;
        }
        for (const auto& file : request.payload) {
            auto source = detail::utf8_to_wide(file.source_path_utf8);
            auto name = detail::utf8_to_wide(file.target_name);
            if (!source || !name) {
                return base::Result<void>::failure(!source ? source.error() : name.error());
            }
            if (auto copied = detail::copy_file_writable(
                    source.value(), payload_dir + L"\\" + name.value(), "inject payload file");
                !copied) {
                return copied;
            }
        }
        const std::wstring winpeshl =
            layout.mount_dir + L"\\" + std::wstring(detail::kWinpeshlRelative);
        return detail::write_document_atomically(
            winpeshl, winpeshl_content(request.executor_target_name, request.debug_shell),
            /*replace_existing=*/true);
    }

    [[nodiscard]] base::Result<void>
    rebuild(const ports::PeImageBuildRequest& request, const ImageLayout& layout,
            const std::string& expected_id, const base::CancellationToken cancellation) {
        // A failed rebuild must never fake a cache hit.
        if (auto cleared = detail::delete_file_if_exists(layout.build_id); !cleared) {
            return cleared;
        }
        self_heal_mount(layout);
        if (auto directories = detail::ensure_directory_exists(layout.image_dir); !directories) {
            return directories;
        }
        if (auto staged = prepare_boot_files(layout); !staged) {
            return staged;
        }
        if (auto active = check_cancelled(cancellation); !active) {
            return active;
        }
        if (auto directories = detail::ensure_directory_exists(layout.mount_dir); !directories) {
            return directories;
        }
        if (auto mounted = run_dism({"/Mount-Wim", "/WimFile:" + layout.boot_wim_utf8,
                                     "/Index:1", "/MountDir:" + layout.mount_dir_utf8},
                                    "mount boot.wim");
            !mounted) {
            return mounted;
        }
        auto injected = inject_payload(request, layout);
        if (!injected) {
            (void)run_dism({"/Unmount-Wim", "/MountDir:" + layout.mount_dir_utf8, "/Discard"},
                           "discard after inject failure");
            return injected;
        }
        if (auto committed = run_dism(
                {"/Unmount-Wim", "/MountDir:" + layout.mount_dir_utf8, "/Commit"},
                "commit boot.wim");
            !committed) {
            (void)run_dism({"/Unmount-Wim", "/MountDir:" + layout.mount_dir_utf8, "/Discard"},
                           "discard after commit failure");
            return committed;
        }
        return detail::write_document_atomically(layout.build_id, expected_id,
                                                 /*replace_existing=*/true);
    }

    ports::IProcessLauncher& launcher_;
    std::string dism_path_utf8_;
};

} // namespace

base::Result<std::unique_ptr<ports::IPeImageBuilder>>
open_pe_image_builder(const PeImageBuilderOpenRequest& request) {
    using Output = base::Result<std::unique_ptr<ports::IPeImageBuilder>>;
    if (request.process_launcher == nullptr) {
        return Output::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "process launcher is required"));
    }
    auto dism_path = detail::resolve_system_tool_path("dism.exe");
    if (!dism_path) {
        return Output::failure(dism_path.error());
    }
    return Output::success(std::make_unique<WindowsPeImageBuilder>(*request.process_launcher,
                                                                   std::move(dism_path).value()));
}

} // namespace aegra::adapters::windows_pe
