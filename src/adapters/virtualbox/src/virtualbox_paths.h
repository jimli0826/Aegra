#pragma once

#include "aegra/base/result.h"
#include "aegra/virtualization/boot_check_provider.h"

#include <string>

namespace aegra::adapters::virtualbox::detail {

struct VirtualBoxJobLayout final {
    std::string job_root;
    std::string parent_vmdk;
    std::string user_home;
    std::string vm_base_directory;
    std::string child_medium;
    std::string vm_name;
};

/// When the check fails and reason is non-null, it receives a log-only
/// description of the failing sub-check.
[[nodiscard]] bool is_trusted_vbox_manage(std::string_view executable_path,
                                          std::string* reason = nullptr);
[[nodiscard]] base::Result<std::string>
prepare_capability_user_home(std::string_view requested_path);
/// When use_isolated_home is false, the layout omits the per-job vbox-home and
/// leaves user_home empty, so VBoxManage uses the invoking user's default
/// VirtualBox registry (the job VM becomes visible in their VirtualBox Manager).
[[nodiscard]] base::Result<VirtualBoxJobLayout>
prepare_job_layout(const virtualization::BootCheckVmRequest& request, bool use_isolated_home);

} // namespace aegra::adapters::virtualbox::detail
