#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aegra::apps::service {

// Portable service registration description. Win32 types stay inside the implementation.
struct WindowsServiceInstallRequest final {
    std::string service_name{"AegraService"};
    std::string display_name{"Aegra Management Service"};
    std::string binary_path;
    // Restart delay applied by recovery policy after unexpected termination.
    std::uint32_t recovery_delay_ms{5'000};
    std::uint32_t recovery_reset_period_seconds{86'400};
};

struct WindowsServiceIdentity final {
    std::string service_name;
    std::string display_name;
    std::string binary_path;
    bool recovery_enabled{false};
    std::uint32_t recovery_delay_ms{0};
};

// Testable SCM port. Production code uses open_windows_service_control_manager().
class IWindowsServiceControlManager {
  public:
    IWindowsServiceControlManager() = default;
    virtual ~IWindowsServiceControlManager() = default;
    IWindowsServiceControlManager(const IWindowsServiceControlManager&) = delete;
    IWindowsServiceControlManager& operator=(const IWindowsServiceControlManager&) = delete;
    IWindowsServiceControlManager(IWindowsServiceControlManager&&) = delete;
    IWindowsServiceControlManager& operator=(IWindowsServiceControlManager&&) = delete;

    [[nodiscard]] virtual base::Result<void>
    create_service(const WindowsServiceInstallRequest& request) = 0;
    [[nodiscard]] virtual base::Result<void> delete_service(const std::string& service_name) = 0;
    [[nodiscard]] virtual base::Result<void>
    configure_recovery(const std::string& service_name, std::uint32_t recovery_delay_ms,
                       std::uint32_t recovery_reset_period_seconds) = 0;
    [[nodiscard]] virtual base::Result<void> start_service(const std::string& service_name) = 0;
    [[nodiscard]] virtual base::Result<void> stop_service(const std::string& service_name) = 0;
    [[nodiscard]] virtual base::Result<WindowsServiceIdentity>
    query_service(const std::string& service_name) const = 0;
};

// Install creates the service then configures recovery. On recovery failure the service is deleted
// so partial installer state is rolled back.
[[nodiscard]] base::Result<void>
install_windows_service(IWindowsServiceControlManager& manager,
                        const WindowsServiceInstallRequest& request);

[[nodiscard]] base::Result<void> uninstall_windows_service(IWindowsServiceControlManager& manager,
                                                           const std::string& service_name);

// Stops if running, then starts. Used by restart and installer repair paths.
[[nodiscard]] base::Result<void> restart_windows_service(IWindowsServiceControlManager& manager,
                                                         const std::string& service_name);

// Opens the local SCM with create/enumerate rights. Requires an elevated administrator process.
[[nodiscard]] base::Result<std::unique_ptr<IWindowsServiceControlManager>>
open_windows_service_control_manager();

// In-memory manager for deterministic unit tests. Not used by production composition roots.
class FakeWindowsServiceControlManager final : public IWindowsServiceControlManager {
  public:
    [[nodiscard]] base::Result<void>
    create_service(const WindowsServiceInstallRequest& request) override;
    [[nodiscard]] base::Result<void> delete_service(const std::string& service_name) override;
    [[nodiscard]] base::Result<void>
    configure_recovery(const std::string& service_name, std::uint32_t recovery_delay_ms,
                       std::uint32_t recovery_reset_period_seconds) override;
    [[nodiscard]] base::Result<void> start_service(const std::string& service_name) override;
    [[nodiscard]] base::Result<void> stop_service(const std::string& service_name) override;
    [[nodiscard]] base::Result<WindowsServiceIdentity>
    query_service(const std::string& service_name) const override;

    void set_fail_create(const bool value) noexcept { fail_create_ = value; }
    void set_fail_recovery(const bool value) noexcept { fail_recovery_ = value; }
    void set_fail_start(const bool value) noexcept { fail_start_ = value; }
    void set_fail_stop(const bool value) noexcept { fail_stop_ = value; }
    void set_fail_delete(const bool value) noexcept { fail_delete_ = value; }

    [[nodiscard]] const std::vector<std::string>& operations() const noexcept {
        return operations_;
    }

  private:
    struct Entry final {
        WindowsServiceIdentity identity;
        bool running{false};
    };

    [[nodiscard]] Entry* find(const std::string& service_name);
    [[nodiscard]] const Entry* find(const std::string& service_name) const;

    std::vector<Entry> services_;
    std::vector<std::string> operations_;
    bool fail_create_{false};
    bool fail_recovery_{false};
    bool fail_start_{false};
    bool fail_stop_{false};
    bool fail_delete_{false};
};

} // namespace aegra::apps::service
