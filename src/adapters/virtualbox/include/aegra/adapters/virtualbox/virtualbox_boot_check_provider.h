#pragma once

#include "aegra/virtualization/boot_check_provider.h"

#include <memory>
#include <string>

namespace aegra::ports {
class IProcessLauncher;
}

namespace aegra::adapters::virtualbox {

struct VirtualBoxProviderOptions final {
    std::string vbox_manage_path;
    std::string capability_user_home;
    /// When true (LocalSystem path), the job VM is registered in a per-job
    /// isolated VBOX_USER_HOME so it never touches a user's global registry.
    /// When false (host running under the logged-on user's token), the job VM
    /// is registered in that user's default VirtualBox registry so it shows up
    /// in their VirtualBox Manager. The capability probe always stays isolated.
    bool use_isolated_home{true};
};

// The injected process launcher must outlive the provider and all sessions
// returned by it. A provider instance is not thread-safe; separate BootCheck
// Hosts construct separate instances.
class VirtualBoxBootCheckProvider final : public virtualization::IBootCheckProvider {
  public:
    VirtualBoxBootCheckProvider(ports::IProcessLauncher& launcher,
                                VirtualBoxProviderOptions options);
    ~VirtualBoxBootCheckProvider() override;

    VirtualBoxBootCheckProvider(const VirtualBoxBootCheckProvider&) = delete;
    VirtualBoxBootCheckProvider& operator=(const VirtualBoxBootCheckProvider&) = delete;
    VirtualBoxBootCheckProvider(VirtualBoxBootCheckProvider&&) = delete;
    VirtualBoxBootCheckProvider& operator=(VirtualBoxBootCheckProvider&&) = delete;

    [[nodiscard]] base::Result<virtualization::BootCheckProviderInfo>
    inspect(base::CancellationToken cancellation) override;
    [[nodiscard]] virtualization::BootCheckParentDiskFormat
    parent_disk_format() const noexcept override {
        return virtualization::BootCheckParentDiskFormat::kVmdk;
    }
    [[nodiscard]] base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>
    create(const virtualization::BootCheckVmRequest& request,
           base::CancellationToken cancellation) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::adapters::virtualbox
