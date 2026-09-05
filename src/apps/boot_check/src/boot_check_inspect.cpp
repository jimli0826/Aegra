#include "aegra/apps/boot_check/boot_check_host.h"

#include "aegra/adapters/hyperv/hyperv_boot_check_provider.h"
#include "aegra/adapters/virtualbox/virtualbox_boot_check_provider.h"
#include "aegra/apps/worker/worker_task_log.h"
#include "aegra/virtualization/boot_check_provider.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::apps::boot_check {
namespace {

using apps::worker::WorkerTaskLog;

constexpr const char* kProviderUnavailable = "bootcheck.provider_unavailable";

[[nodiscard]] std::unique_ptr<virtualization::IBootCheckProvider>
make_provider(const std::string_view hypervisor_name, const BootCheckHostOptions& options,
              const BootCheckHostContext& context) {
    if (hypervisor_name == "virtualbox" && !options.vbox_manage_path.empty()) {
        adapters::virtualbox::VirtualBoxProviderOptions provider_options;
        provider_options.vbox_manage_path = options.vbox_manage_path;
        provider_options.capability_user_home = options.capability_user_home;
        return std::make_unique<adapters::virtualbox::VirtualBoxBootCheckProvider>(
            context.launcher, std::move(provider_options));
    }
    if (hypervisor_name == "hyperv" && !options.powershell_path.empty()) {
        adapters::hyperv::HyperVProviderOptions provider_options;
        provider_options.powershell_path = options.powershell_path;
        return std::make_unique<adapters::hyperv::HyperVBootCheckProvider>(
            context.launcher, std::move(provider_options));
    }
    return nullptr;
}

} // namespace

base::Result<EncodedBootCheckResult>
run_boot_check_inspect(const std::string_view hypervisor_name, const BootCheckHostOptions& options,
                       const BootCheckHostContext& context) {
    if (hypervisor_name != "virtualbox" && hypervisor_name != "hyperv") {
        return base::Result<EncodedBootCheckResult>::failure(
            {base::ErrorCode::kInvalidArgument, "bootcheck.inspect_rejected"});
    }
    auto log = WorkerTaskLog::open("bootcheck", "inspect");
    if (log != nullptr) {
        log->section("Inspect");
        log->field("hypervisor", std::string(hypervisor_name));
    }
    virtualization::BootCheckProviderInfo info;
    info.message_code = kProviderUnavailable;
    auto provider = make_provider(hypervisor_name, options, context);
    if (provider == nullptr) {
        info.diagnostic = "hypervisor tooling not discovered on this host";
    } else if (auto inspected = provider->inspect({}); inspected) {
        info = std::move(inspected).value();
        if (!info.available && info.message_code.empty()) {
            info.message_code = kProviderUnavailable;
        }
    } else {
        info.diagnostic = "inspect failed: " + inspected.error().message;
    }
    if (log != nullptr) {
        log->field("available", info.available ? "true" : "false");
        log->field("provider_version", info.provider_version);
        log->field("message_code", info.message_code);
        log->field("diagnostic", info.diagnostic);
    }
    const nlohmann::json root{{"schema_version", 1},
                              {"kind", "inspect"},
                              {"hypervisor", std::string(hypervisor_name)},
                              {"available", info.available},
                              {"message_code", info.available ? std::string{} : info.message_code},
                              {"provider_version", info.provider_version},
                              {"diagnostic", info.diagnostic}};
    EncodedBootCheckResult result;
    result.exit_code = BootCheckExitCode::kSucceeded;
    result.response_json = root.dump();
    return base::Result<EncodedBootCheckResult>::success(std::move(result));
}

} // namespace aegra::apps::boot_check
