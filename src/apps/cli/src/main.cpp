#include "cli_args.h"
#include "cli_commands.h"
#include "cli_exit.h"
#include "cli_io.h"

int wmain(int argc, wchar_t** argv) {
    aegra::apps::cli::configure_console();
    try {
        auto parsed = aegra::apps::cli::parse_args(argc, argv);
        if (!parsed) {
            aegra::apps::cli::write_error_object(parsed.error());
            aegra::apps::cli::write_error("Use AegraCLI --help for usage.");
            return aegra::apps::cli::kExitUsage;
        }
        return aegra::apps::cli::run_command(parsed.value());
    } catch (...) {
        aegra::apps::cli::write_error("AegraCLI failed with an unexpected error.");
        return aegra::apps::cli::kExitInternal;
    }
}
