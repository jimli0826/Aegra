#include "aegra/ports/backup_session.h"
#include "aegra/ports/block_io.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/progress.h"

#include <cstdlib>
#include <type_traits>

int main() {
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBlockSource>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBlockSink>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IBackupSession>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IRecoveryPointReader>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IClock>);
    static_assert(std::has_virtual_destructor_v<aegra::ports::IProgressSink>);
    return EXIT_SUCCESS;
}
