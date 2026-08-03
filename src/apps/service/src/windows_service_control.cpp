#include "aegra/apps/service/windows_service_control.h"

#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] bool valid_service_name(const std::string& name) noexcept {
    if (name.empty() || name.size() > 256) {
        return false;
    }
    for (const unsigned char character : name) {
        const bool alpha = (character >= 'A' && character <= 'Z') ||
                           (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if (!alpha && !digit && character != '-' && character != '_' && character != ' ') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_install_request(const WindowsServiceInstallRequest& request) noexcept {
    return valid_service_name(request.service_name) && !request.display_name.empty() &&
           request.display_name.size() <= 256 && !request.binary_path.empty() &&
           request.binary_path.size() <= 32'767 && request.recovery_delay_ms > 0 &&
           request.recovery_reset_period_seconds > 0;
}

} // namespace

base::Result<void> install_windows_service(IWindowsServiceControlManager& manager,
                                           const WindowsServiceInstallRequest& request) {
    if (!valid_install_request(request)) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "windows service install request is invalid"});
    }
    auto created = manager.create_service(request);
    if (!created) {
        return created;
    }
    auto recovery = manager.configure_recovery(request.service_name, request.recovery_delay_ms,
                                               request.recovery_reset_period_seconds);
    if (!recovery) {
        auto rolled_back = manager.delete_service(request.service_name);
        if (!rolled_back) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kOutcomeUnknown,
                "windows service recovery configuration failed and rollback failed"});
        }
        return base::Result<void>::failure(recovery.error());
    }
    return base::Result<void>::success();
}

base::Result<void> uninstall_windows_service(IWindowsServiceControlManager& manager,
                                             const std::string& service_name) {
    if (!valid_service_name(service_name)) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "windows service name is invalid"});
    }
    auto stopped = manager.stop_service(service_name);
    if (!stopped && stopped.error().code != base::ErrorCode::kNotFound &&
        stopped.error().code != base::ErrorCode::kConflict) {
        // Conflict maps to "already stopped" in the fake and production adapters.
        return stopped;
    }
    return manager.delete_service(service_name);
}

base::Result<void> restart_windows_service(IWindowsServiceControlManager& manager,
                                           const std::string& service_name) {
    if (!valid_service_name(service_name)) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "windows service name is invalid"});
    }
    auto stopped = manager.stop_service(service_name);
    if (!stopped && stopped.error().code != base::ErrorCode::kConflict) {
        return stopped;
    }
    return manager.start_service(service_name);
}

FakeWindowsServiceControlManager::Entry*
FakeWindowsServiceControlManager::find(const std::string& service_name) {
    for (auto& entry : services_) {
        if (entry.identity.service_name == service_name) {
            return &entry;
        }
    }
    return nullptr;
}

const FakeWindowsServiceControlManager::Entry*
FakeWindowsServiceControlManager::find(const std::string& service_name) const {
    for (const auto& entry : services_) {
        if (entry.identity.service_name == service_name) {
            return &entry;
        }
    }
    return nullptr;
}

base::Result<void>
FakeWindowsServiceControlManager::create_service(const WindowsServiceInstallRequest& request) {
    operations_.push_back("create");
    if (fail_create_) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kIoFailure, "fake create_service failed"});
    }
    if (find(request.service_name) != nullptr) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kConflict, "service already exists"});
    }
    Entry entry;
    entry.identity.service_name = request.service_name;
    entry.identity.display_name = request.display_name;
    entry.identity.binary_path = request.binary_path;
    services_.push_back(std::move(entry));
    return base::Result<void>::success();
}

base::Result<void>
FakeWindowsServiceControlManager::delete_service(const std::string& service_name) {
    operations_.push_back("delete");
    if (fail_delete_) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kIoFailure, "fake delete_service failed"});
    }
    for (auto iterator = services_.begin(); iterator != services_.end(); ++iterator) {
        if (iterator->identity.service_name == service_name) {
            if (iterator->running) {
                return base::Result<void>::failure(
                    base::Error{base::ErrorCode::kConflict, "service is still running"});
            }
            services_.erase(iterator);
            return base::Result<void>::success();
        }
    }
    return base::Result<void>::failure(
        base::Error{base::ErrorCode::kNotFound, "service was not found"});
}

base::Result<void> FakeWindowsServiceControlManager::configure_recovery(
    const std::string& service_name, const std::uint32_t recovery_delay_ms,
    const std::uint32_t recovery_reset_period_seconds) {
    operations_.push_back("configure_recovery");
    if (fail_recovery_) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kIoFailure, "fake configure_recovery failed"});
    }
    auto* entry = find(service_name);
    if (entry == nullptr) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kNotFound, "service was not found"});
    }
    if (recovery_delay_ms == 0 || recovery_reset_period_seconds == 0) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "recovery policy is invalid"});
    }
    entry->identity.recovery_enabled = true;
    entry->identity.recovery_delay_ms = recovery_delay_ms;
    return base::Result<void>::success();
}

base::Result<void>
FakeWindowsServiceControlManager::start_service(const std::string& service_name) {
    operations_.push_back("start");
    if (fail_start_) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kIoFailure, "fake start_service failed"});
    }
    auto* entry = find(service_name);
    if (entry == nullptr) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kNotFound, "service was not found"});
    }
    if (entry->running) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kConflict, "service is already running"});
    }
    entry->running = true;
    return base::Result<void>::success();
}

base::Result<void>
FakeWindowsServiceControlManager::stop_service(const std::string& service_name) {
    operations_.push_back("stop");
    if (fail_stop_) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kIoFailure, "fake stop_service failed"});
    }
    auto* entry = find(service_name);
    if (entry == nullptr) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kNotFound, "service was not found"});
    }
    if (!entry->running) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kConflict, "service is already stopped"});
    }
    entry->running = false;
    return base::Result<void>::success();
}

base::Result<WindowsServiceIdentity>
FakeWindowsServiceControlManager::query_service(const std::string& service_name) const {
    const auto* entry = find(service_name);
    if (entry == nullptr) {
        return base::Result<WindowsServiceIdentity>::failure(
            base::Error{base::ErrorCode::kNotFound, "service was not found"});
    }
    return base::Result<WindowsServiceIdentity>::success(entry->identity);
}

} // namespace aegra::apps::service
