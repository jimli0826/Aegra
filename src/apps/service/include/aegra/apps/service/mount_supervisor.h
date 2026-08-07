#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace aegra::ports {
class IClock;
class IControlPlaneDatabase;
class IProcessLauncher;
class IRandomSource;
class IRepositoryStorageFactory;
} // namespace aegra::ports

namespace aegra::apps::service {

struct MountSupervisorConfig final {
    std::string mount_host_executable_path;
    std::filesystem::path overlay_root;
    std::chrono::seconds mount_ready_timeout{120};
};

class MountSupervisor {
  public:
    MountSupervisor(MountSupervisorConfig config, ports::IProcessLauncher& launcher,
                    ports::IControlPlaneDatabase& control_plane,
                    ports::IRepositoryStorageFactory& storage_factory, ports::IClock& clock,
                    ports::IRandomSource& random);
    ~MountSupervisor();

    MountSupervisor(const MountSupervisor&) = delete;
    MountSupervisor& operator=(const MountSupervisor&) = delete;

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    mount(const contracts::MountRecoveryPointCommand& command, std::string_view idempotency_key,
          base::CancellationToken cancellation);

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    unmount(const contracts::ResourceRef& session, std::string_view idempotency_key,
            base::CancellationToken cancellation);

    [[nodiscard]] base::Result<contracts::MountSessionPage>
    list(const contracts::MountSessionListRequest& request,
         base::CancellationToken cancellation) const;

    void shutdown(const base::CancellationToken& cancellation);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::apps::service
