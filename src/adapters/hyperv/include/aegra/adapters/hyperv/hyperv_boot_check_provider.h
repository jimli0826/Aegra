#pragma once

#include "aegra/virtualization/boot_check_provider.h"

#include <memory>
#include <string>

namespace aegra::ports {
class IProcessLauncher;
}

namespace aegra::adapters::hyperv {

struct HyperVProviderOptions final {
    /// Trusted absolute powershell.exe path (System32); empty = unavailable.
    std::string powershell_path;
};

/// Hyper-V BootCheck provider driven through the Hyper-V PowerShell module.
/// Requires the vmms service and a running hypervisor. VMs are created in the
/// job-private directory with the Aegra-BootCheck name prefix and an
/// `aegra-bootcheck:<job_id>` Notes marker (Hyper-V has no per-job registry, so
/// cleanup and scavenging are gated on prefix + marker, never broad names).
/// The injected launcher must outlive the provider and its sessions; instances
/// are single-caller.
class HyperVBootCheckProvider final : public virtualization::IBootCheckProvider {
  public:
    HyperVBootCheckProvider(ports::IProcessLauncher& launcher, HyperVProviderOptions options);
    ~HyperVBootCheckProvider() override;

    HyperVBootCheckProvider(const HyperVBootCheckProvider&) = delete;
    HyperVBootCheckProvider& operator=(const HyperVBootCheckProvider&) = delete;
    HyperVBootCheckProvider(HyperVBootCheckProvider&&) = delete;
    HyperVBootCheckProvider& operator=(HyperVBootCheckProvider&&) = delete;

    [[nodiscard]] base::Result<virtualization::BootCheckProviderInfo>
    inspect(base::CancellationToken cancellation) override;
    [[nodiscard]] virtualization::BootCheckParentDiskFormat
    parent_disk_format() const noexcept override {
        return virtualization::BootCheckParentDiskFormat::kVhdx;
    }
    [[nodiscard]] base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>
    create(const virtualization::BootCheckVmRequest& request,
           base::CancellationToken cancellation) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::adapters::hyperv
