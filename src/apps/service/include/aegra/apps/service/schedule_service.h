#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/file_browser.h"
#include "aegra/ports/random.h"

namespace aegra::application {
class FileBrowseService;
}

namespace aegra::apps::service {

// Personal-edition schedule CRUD over the control-plane schedule store.
class ScheduleService final {
  public:
    ScheduleService(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                    ports::IRandomSource& random,
                    application::FileBrowseService* file_browse = nullptr) noexcept;

    [[nodiscard]] base::Result<contracts::SchedulePage>
    list_schedules(const contracts::ScheduleListRequest& request,
                   base::CancellationToken cancellation);

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    upsert_schedule(const contracts::UpsertScheduleCommand& command,
                    std::string_view idempotency_key, const ports::FileBrowseSession& session,
                    base::CancellationToken cancellation);

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    delete_schedule(const contracts::ResourceRef& reference, std::string_view idempotency_key,
                    base::CancellationToken cancellation);

  private:
    ports::IControlPlaneDatabase& control_plane_;
    ports::IClock& clock_;
    ports::IRandomSource& random_;
    application::FileBrowseService* file_browse_;
};

} // namespace aegra::apps::service
