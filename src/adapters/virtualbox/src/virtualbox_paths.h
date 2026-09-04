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

[[nodiscard]] bool is_trusted_vbox_manage(std::string_view executable_path);
[[nodiscard]] base::Result<std::string>
prepare_capability_user_home(std::string_view requested_path);
[[nodiscard]] base::Result<VirtualBoxJobLayout>
prepare_job_layout(const virtualization::BootCheckVmRequest& request);

} // namespace aegra::adapters::virtualbox::detail
