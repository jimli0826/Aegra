#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <string>

namespace aegra::ports {

enum class FirmwareKind : std::uint8_t {
    kBios = 1,
    kUefi = 2,
};

/// Boot files for the one-time WinPE boot. Both paths must be absolute
/// drive-letter paths to existing files (`X:\...`); the implementation derives
/// the BCD ramdisk device from the drive component.
struct OneTimeBootRequest final {
    std::string wim_path_utf8;
    std::string sdi_path_utf8;
    /// Display name of the boot entry (menu text only; identity is internal).
    std::string entry_name;
};

/// One-time boot into a WinPE ramdisk image (ADR-0026 A).
///
/// Contract (frozen; the bcdedit implementation may later be replaced by a
/// WMI/BCD API implementation without changing these signatures):
/// - arm_once configures the *next* boot only (boot sequence), never the default
///   boot entry, display order, or timeout; it is idempotent (re-arming replaces
///   the previous product entry) and rolls its own objects back on failure.
/// - disarm removes the product boot entry and clears a boot sequence that still
///   references it; it succeeds when nothing is armed.
/// - is_armed reports whether the product entry exists AND the next-boot sequence
///   references it (a stale entry without a sequence reports false).
/// - Boot entry GUIDs never appear in this interface.
/// - All operations require administrator privileges and return kUnauthorized
///   without them. Implementations must not log boot passwords or secrets
///   (there are none in this flow; command lines and outputs are diagnostics).
class IOneTimeBootController {
  public:
    IOneTimeBootController() = default;
    virtual ~IOneTimeBootController() = default;
    IOneTimeBootController(const IOneTimeBootController&) = delete;
    IOneTimeBootController& operator=(const IOneTimeBootController&) = delete;
    IOneTimeBootController(IOneTimeBootController&&) = delete;
    IOneTimeBootController& operator=(IOneTimeBootController&&) = delete;

    [[nodiscard]] virtual base::Result<void>
    arm_once(const OneTimeBootRequest& request, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> disarm(base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<bool> is_armed(base::CancellationToken cancellation) = 0;

    /// Firmware of the running machine; selects the winload flavor for arm_once.
    [[nodiscard]] virtual base::Result<FirmwareKind> firmware_kind() = 0;
};

} // namespace aegra::ports
