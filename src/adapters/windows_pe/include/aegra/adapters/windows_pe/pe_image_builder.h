#pragma once

#include "aegra/base/result.h"
#include "aegra/ports/pe_image_builder.h"
#include "aegra/ports/process_launcher.h"

#include <memory>

namespace aegra::adapters::windows_pe {

struct PeImageBuilderOpenRequest final {
    /// Required. Non-owning: the launcher must outlive the builder. The
    /// composition root wires the windows_process implementation.
    ports::IProcessLauncher* process_launcher{nullptr};
};

/// DISM/WinRE-based PE image builder (design §6). Locates the host `Winre.wim`
/// (reagentc → recovery partition scan → `System32\Recovery`) and `boot.sdi`,
/// injects the payload and `winpeshl.ini`, and caches the result keyed by a
/// build id (product version, payload SHA-256 set, host OS build, job schema).
[[nodiscard]] base::Result<std::unique_ptr<ports::IPeImageBuilder>>
open_pe_image_builder(const PeImageBuilderOpenRequest& request);

} // namespace aegra::adapters::windows_pe
