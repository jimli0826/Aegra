#include "aegra/ports/backup_session.h"
#include "aegra/ports/block_io.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/progress.h"
#include "aegra/ports/random.h"

#include <cstdlib>
#include <type_traits>

int main() {
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBlockSource>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBlockSink>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBackupSession>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IRecoveryPointReader>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IClock>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::ICredentialResolver>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IResolvedSecret>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IProgressSink>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IRandomSource>);
    return EXIT_SUCCESS;
}
