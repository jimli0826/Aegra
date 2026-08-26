#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <string>
#include <vector>

namespace aegra::ports {

/// One file to inject into the PE image payload directory
/// (`\Windows\System32\Aegra\` inside the mounted WIM).
struct PeImagePayloadFile final {
    /// Absolute path of the source file on the online system.
    std::string source_path_utf8;
    /// Plain file name inside the payload directory (no separators).
    std::string target_name;
};

struct PeImageBuildRequest final {
    /// Data directory; image files live under `<data_dir>\pe\image\`.
    std::string data_dir_utf8;
    /// Product version; part of the build cache key.
    std::string product_version;
    /// Payload closure: the PE executor and its runtime dependencies.
    /// Content hashes are part of the build cache key.
    std::vector<PeImagePayloadFile> payload;
    /// target_name of the executor `winpeshl.ini` launches after wpeinit;
    /// must match exactly one payload entry.
    std::string executor_target_name;
    /// Diagnostics: when true, winpeshl launches an interactive cmd.exe instead of
    /// the executor, so a technician can run the executor by hand and read any
    /// missing-DLL error. Part of the build cache key (toggling forces a rebuild).
    bool debug_shell{false};
};

/// Boot files produced by a successful build (inputs for IOneTimeBootController).
struct PeImagePaths final {
    std::string wim_path_utf8;
    std::string sdi_path_utf8;
};

/// Builds and caches the customized WinPE image from the local Windows RE.
///
/// Contract:
/// - ensure_ready is idempotent: a matching build id with existing boot files
///   returns immediately without DISM work; any change to the product version,
///   payload contents, or host OS build forces a rebuild.
/// - A failed build must never leave a mounted WIM behind (discard on failure,
///   self-heal a leftover mount from a crashed previous run) and must never
///   leave a stale build id that fakes a cache hit.
/// - Requires administrator privileges (DISM); returns kUnauthorized otherwise.
/// - The first build takes minutes; callers surface indeterminate progress.
class IPeImageBuilder {
  public:
    IPeImageBuilder() = default;
    virtual ~IPeImageBuilder() = default;
    IPeImageBuilder(const IPeImageBuilder&) = delete;
    IPeImageBuilder& operator=(const IPeImageBuilder&) = delete;
    IPeImageBuilder(IPeImageBuilder&&) = delete;
    IPeImageBuilder& operator=(IPeImageBuilder&&) = delete;

    [[nodiscard]] virtual base::Result<PeImagePaths>
    ensure_ready(const PeImageBuildRequest& request, base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
