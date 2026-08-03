#include "aegra/ports/backup_session.h"
#include "aegra/ports/block_io.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/message_channel.h"
#include "aegra/ports/object_storage.h"
#include "aegra/ports/progress.h"
#include "aegra/ports/random.h"

#include <cstdlib>
#include <type_traits>

int main() {
    static_assert(aegra::ports::is_valid_object_key("archives/2026/08/archive.bkf"));
    static_assert(aegra::ports::is_valid_object_prefix("catalog/recovery-points/"));
    static_assert(!aegra::ports::is_valid_object_key("archives/../escape.bkf"));
    static_assert(!aegra::ports::is_valid_object_prefix("/absolute/"));
    static_assert(aegra::ports::kControlPlaneSchemaVersion == 1);
    static_assert(aegra::ports::is_valid_job_state_transition(
        aegra::contracts::ServiceJobState::kQueued, aegra::contracts::ServiceJobState::kRunning));
    static_assert(!aegra::ports::is_valid_job_state_transition(
        aegra::contracts::ServiceJobState::kSucceeded, aegra::contracts::ServiceJobState::kRunning));
    static_assert(aegra::ports::is_terminal_job_state(aegra::contracts::ServiceJobState::kInterrupted));
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBlockSource>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBlockSink>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBackupSession>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IRecoveryPointReader>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IClock>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::ICredentialResolver>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IResolvedSecret>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IProgressSink>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IRandomSource>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IMessageChannel>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IObjectReader>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IStagedObjectWriteSession>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IStagedObjectWriter>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IPrefixEnumerator>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IObjectPublisher>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IObjectDeleter>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IObjectStorageCapabilities>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IControlPlaneDatabase>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IControlPlaneUnitOfWork>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IRepositoryConnectionStore>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IJobStore>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IScheduleStore>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IAuditEventStore>);
    return EXIT_SUCCESS;
}
