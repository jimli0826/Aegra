#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <memory>
#include <string>

namespace aegra::virtualization {

enum class BootFirmware : std::uint8_t { kBios = 1, kUefi = 2 };

/// Read-only parent disk container the provider consumes. VirtualBox mounts a
/// monolithicFlat VMDK descriptor; Hyper-V requires VHDX (both generations).
enum class BootCheckParentDiskFormat : std::uint8_t { kVmdk = 1, kVhdx = 2 };

enum class BootCheckVmState : std::uint8_t {
    kUnknown = 0,
    kPoweredOff = 1,
    kRunning = 2,
    kPaused = 3,
    kAborted = 4,
};

struct BootCheckProviderInfo final {
    bool available{false};
    std::string provider_name;
    std::string provider_version;
    std::string message_code;
};

struct BootCheckVmRequest final {
    std::string job_id;
    std::string job_directory;
    /// Read-only parent disk in the provider's parent_disk_format(), inside
    /// <job_directory>/present.
    std::string parent_disk_path;
    BootFirmware firmware{BootFirmware::kBios};
    std::uint32_t cpu_count{2};
    std::uint32_t memory_mib{2048};
    // The BootCheck Host must monitor BootCheckVmInfo::child_medium_path and
    // stop the session before this limit is exceeded. Providers validate the
    // policy value but do not implement filesystem quotas.
    std::uint64_t overlay_limit_bytes{8ULL * 1024 * 1024 * 1024};
};

struct BootCheckVmInfo final {
    std::string vm_name;
    std::string child_medium_path;
    std::string provider_version;
};

class IBootCheckVmSession {
  public:
    IBootCheckVmSession() = default;
    virtual ~IBootCheckVmSession() = default;
    IBootCheckVmSession(const IBootCheckVmSession&) = delete;
    IBootCheckVmSession& operator=(const IBootCheckVmSession&) = delete;
    IBootCheckVmSession(IBootCheckVmSession&&) = delete;
    IBootCheckVmSession& operator=(IBootCheckVmSession&&) = delete;

    // A session is single-caller and not thread-safe. The caller must invoke
    // cleanup after every successful create, including failed or cancelled
    // starts. Destruction performs only bounded best-effort cleanup.
    // Boot confirmation is owned by the caller: it monitors the growth of
    // BootCheckVmInfo::child_medium_path and this session's state; providers
    // expose no guest channel.
    [[nodiscard]] virtual const BootCheckVmInfo& info() const noexcept = 0;
    [[nodiscard]] virtual base::Result<void> start(base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<BootCheckVmState>
    state(base::CancellationToken cancellation) = 0;
    // True once the guest OS is confirmed running through the hypervisor's own
    // guest channel (Hyper-V Integration Services heartbeat). This is the
    // authoritative "the OS booted" signal and needs no agent inside the image.
    // Providers without such a channel (VirtualBox, no Guest Additions) return
    // false so the caller falls back to differencing-overlay growth. A query
    // error is reported as false, not a failure: the caller keeps polling.
    [[nodiscard]] virtual base::Result<bool>
    guest_heartbeat_ok(base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<void> power_off(base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<void> cleanup(base::CancellationToken cancellation) = 0;
};

class IBootCheckProvider {
  public:
    IBootCheckProvider() = default;
    virtual ~IBootCheckProvider() = default;
    IBootCheckProvider(const IBootCheckProvider&) = delete;
    IBootCheckProvider& operator=(const IBootCheckProvider&) = delete;
    IBootCheckProvider(IBootCheckProvider&&) = delete;
    IBootCheckProvider& operator=(IBootCheckProvider&&) = delete;

    // inspect returns available=false for an absent or unusable provider and
    // reserves Result failure for cancellation or an internal contract error.
    // Provider instances are single-caller unless an implementation documents
    // stronger thread-safety.
    [[nodiscard]] virtual base::Result<BootCheckProviderInfo>
    inspect(base::CancellationToken cancellation) = 0;
    /// Container format the read-only parent disk must use for this provider.
    [[nodiscard]] virtual BootCheckParentDiskFormat parent_disk_format() const noexcept = 0;
    [[nodiscard]] virtual base::Result<std::unique_ptr<IBootCheckVmSession>>
    create(const BootCheckVmRequest& request, base::CancellationToken cancellation) = 0;
};

} // namespace aegra::virtualization
