#include "cli_args.h"

#include "aegra/base/error.h"

#include "cli_io.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <iterator>
#include <string_view>
#include <utility>

namespace aegra::apps::cli {
namespace {

[[nodiscard]] base::Error usage_error(const char* message) {
    return {base::ErrorCode::kInvalidArgument, message};
}

[[nodiscard]] base::Result<std::string> utf8_from_wide(const std::wstring_view wide) {
    if (wide.empty()) {
        return base::Result<std::string>::success(std::string{});
    }
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                              static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                              nullptr);
    if (required <= 0) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "command line is not valid UTF-16"});
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    const auto written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                                             static_cast<int>(wide.size()), utf8.data(), required,
                                             nullptr, nullptr);
    if (written != required) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "command line is not valid UTF-16"});
    }
    return base::Result<std::string>::success(std::move(utf8));
}

[[nodiscard]] base::Result<std::uint32_t> parse_u32(const std::string_view text) {
    std::uint32_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0) {
        return base::Result<std::uint32_t>::failure(
            usage_error("timeout must be a positive integer"));
    }
    return base::Result<std::uint32_t>::success(value);
}

[[nodiscard]] bool consume_flag(std::vector<std::string>& args, const std::string_view name) {
    const auto found = std::find(args.begin(), args.end(), name);
    if (found == args.end()) {
        return false;
    }
    args.erase(found);
    return true;
}

[[nodiscard]] base::Result<std::optional<std::string>>
take_option_value(std::vector<std::string>& args, const std::string_view name) {
    const auto found = std::find(args.begin(), args.end(), name);
    if (found == args.end()) {
        return base::Result<std::optional<std::string>>::success(std::nullopt);
    }
    const auto value = std::next(found);
    if (value == args.end()) {
        return base::Result<std::optional<std::string>>::failure(
            usage_error("missing value for option"));
    }
    std::string taken = *value;
    args.erase(found, std::next(value));
    return base::Result<std::optional<std::string>>::success(std::move(taken));
}

[[nodiscard]] base::Result<contracts::BackupType> parse_backup_type(const std::string_view text) {
    if (text == "full") {
        return base::Result<contracts::BackupType>::success(contracts::BackupType::kFull);
    }
    if (text == "incremental") {
        return base::Result<contracts::BackupType>::success(contracts::BackupType::kIncremental);
    }
    return base::Result<contracts::BackupType>::failure(
        usage_error("--type must be full or incremental"));
}

[[nodiscard]] base::Result<contracts::JobListScope> parse_scope(const std::string_view text) {
    if (text == "active") {
        return base::Result<contracts::JobListScope>::success(contracts::JobListScope::kActive);
    }
    if (text == "terminal") {
        return base::Result<contracts::JobListScope>::success(contracts::JobListScope::kTerminal);
    }
    if (text == "all") {
        return base::Result<contracts::JobListScope>::success(contracts::JobListScope::kAll);
    }
    return base::Result<contracts::JobListScope>::failure(
        usage_error("--scope must be active, terminal, or all"));
}

[[nodiscard]] base::Result<contracts::JobOperation>
parse_operation(const std::string_view text) {
    if (text == "backup") {
        return base::Result<contracts::JobOperation>::success(contracts::JobOperation::kBackup);
    }
    if (text == "restore") {
        return base::Result<contracts::JobOperation>::success(contracts::JobOperation::kRestore);
    }
    if (text == "verify") {
        return base::Result<contracts::JobOperation>::success(contracts::JobOperation::kVerify);
    }
    return base::Result<contracts::JobOperation>::failure(
        usage_error("--operation must be backup, restore, or verify"));
}

[[nodiscard]] base::Result<void> apply_timeouts(Options& options, std::vector<std::string>& args) {
    auto timeout = take_option_value(args, "--timeout-ms");
    if (!timeout) {
        return base::Result<void>::failure(timeout.error());
    }
    if (timeout.value()) {
        auto parsed = parse_u32(*timeout.value());
        if (!parsed) {
            return base::Result<void>::failure(parsed.error());
        }
        options.timeout_ms = parsed.value();
    }
    auto wait_timeout = take_option_value(args, "--wait-timeout-ms");
    if (!wait_timeout) {
        return base::Result<void>::failure(wait_timeout.error());
    }
    if (wait_timeout.value()) {
        auto parsed = parse_u32(*wait_timeout.value());
        if (!parsed) {
            return base::Result<void>::failure(parsed.error());
        }
        options.wait_timeout_ms = parsed.value();
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> apply_common_flags(Options& options,
                                                    std::vector<std::string>& args) {
    options.json = consume_flag(args, "--json");
    options.wait = consume_flag(args, "--wait");
    const auto enabled = consume_flag(args, "--enabled");
    const auto disabled = consume_flag(args, "--disabled");
    if (enabled && disabled) {
        return base::Result<void>::failure(usage_error("use either --enabled or --disabled"));
    }
    if (enabled) {
        options.enabled_filter = true;
    }
    if (disabled) {
        options.enabled_filter = false;
    }
    return apply_timeouts(options, args);
}

[[nodiscard]] base::Result<void> apply_named_values(Options& options,
                                                    std::vector<std::string>& args) {
    auto id = take_option_value(args, "--id");
    if (!id) {
        return base::Result<void>::failure(id.error());
    }
    options.id = std::move(id.value());
    auto type = take_option_value(args, "--type");
    if (!type) {
        return base::Result<void>::failure(type.error());
    }
    if (type.value()) {
        auto parsed = parse_backup_type(*type.value());
        if (!parsed) {
            return base::Result<void>::failure(parsed.error());
        }
        options.backup_type = parsed.value();
    }
    auto scope = take_option_value(args, "--scope");
    if (!scope) {
        return base::Result<void>::failure(scope.error());
    }
    if (scope.value()) {
        auto parsed = parse_scope(*scope.value());
        if (!parsed) {
            return base::Result<void>::failure(parsed.error());
        }
        options.job_scope = parsed.value();
    }
    auto operation = take_option_value(args, "--operation");
    if (!operation) {
        return base::Result<void>::failure(operation.error());
    }
    if (operation.value()) {
        auto parsed = parse_operation(*operation.value());
        if (!parsed) {
            return base::Result<void>::failure(parsed.error());
        }
        options.job_operation = parsed.value();
    }
    auto connection = take_option_value(args, "--connection");
    if (!connection) {
        return base::Result<void>::failure(connection.error());
    }
    options.connection_id = std::move(connection.value());
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<Command> parse_schedule_verb(const std::string_view verb) {
    if (verb == "list") {
        return base::Result<Command>::success(Command::kScheduleList);
    }
    if (verb == "run") {
        return base::Result<Command>::success(Command::kScheduleRun);
    }
    if (verb == "delete") {
        return base::Result<Command>::success(Command::kScheduleDelete);
    }
    return base::Result<Command>::failure(
        usage_error("schedule verb must be list, run, or delete"));
}

[[nodiscard]] base::Result<Command> parse_job_verb(const std::string_view verb) {
    if (verb == "list") {
        return base::Result<Command>::success(Command::kJobList);
    }
    if (verb == "cancel") {
        return base::Result<Command>::success(Command::kJobCancel);
    }
    if (verb == "wait") {
        return base::Result<Command>::success(Command::kJobWait);
    }
    return base::Result<Command>::failure(
        usage_error("job verb must be list, cancel, or wait"));
}

[[nodiscard]] base::Result<Command> parse_noun_verb(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "help") {
        return base::Result<Command>::success(Command::kHelp);
    }
    const auto& noun = args[0];
    if (noun == "status") {
        return base::Result<Command>::success(Command::kStatus);
    }
    if (noun == "inventory") {
        return args.size() <= 1 || args[1] == "list"
                   ? base::Result<Command>::success(Command::kInventoryList)
                   : base::Result<Command>::failure(usage_error("inventory verb must be list"));
    }
    if (noun == "repository") {
        return args.size() <= 1 || args[1] == "list"
                   ? base::Result<Command>::success(Command::kRepositoryList)
                   : base::Result<Command>::failure(usage_error("repository verb must be list"));
    }
    if (noun == "recovery-point") {
        return args.size() <= 1 || args[1] == "list"
                   ? base::Result<Command>::success(Command::kRecoveryPointList)
                   : base::Result<Command>::failure(
                         usage_error("recovery-point verb must be list"));
    }
    if (noun == "event") {
        return args.size() <= 1 || args[1] == "list"
                   ? base::Result<Command>::success(Command::kEventList)
                   : base::Result<Command>::failure(usage_error("event verb must be list"));
    }
    if (noun == "mount") {
        return args.size() <= 1 || args[1] == "list"
                   ? base::Result<Command>::success(Command::kMountList)
                   : base::Result<Command>::failure(usage_error("mount verb must be list"));
    }
    if (noun == "settings") {
        return args.size() <= 1 || args[1] == "get"
                   ? base::Result<Command>::success(Command::kSettingsGet)
                   : base::Result<Command>::failure(usage_error("settings verb must be get"));
    }
    if (args.size() < 2) {
        return base::Result<Command>::failure(usage_error("missing command verb"));
    }
    if (noun == "schedule") {
        return parse_schedule_verb(args[1]);
    }
    if (noun == "job") {
        return parse_job_verb(args[1]);
    }
    return base::Result<Command>::failure(usage_error("unknown command"));
}

[[nodiscard]] bool command_takes_positional_id(const Command command) noexcept {
    return command == Command::kScheduleRun || command == Command::kScheduleDelete ||
           command == Command::kJobCancel || command == Command::kJobWait;
}

} // namespace

void print_help() {
    write_line("AegraCli — local Aegra Service control client");
    write_line("");
    write_line("Usage:");
    write_line("  aegra_cli [global-options] <command> [command-options]");
    write_line("");
    write_line("Global options:");
    write_line("  --json                  Print Service V4 JSON responses");
    write_line("  --timeout-ms <n>        Per-request timeout in ms (default 30000)");
    write_line("  --wait-timeout-ms <n>   Job wait timeout in ms (default 3600000)");
    write_line("  -h, --help              Show this help");
    write_line("");
    write_line("Commands:");
    write_line("  status");
    write_line("  schedule list [--enabled|--disabled]");
    write_line("  schedule run --id <schedule-id> [--type full|incremental] [--wait]");
    write_line("  schedule delete --id <schedule-id>");
    write_line("  job list [--scope active|terminal|all] [--operation backup|restore|verify]");
    write_line("  job cancel --id <job-id>");
    write_line("  job wait --id <job-id>");
    write_line("  repository list");
    write_line("  recovery-point list [--connection <connection-id>]");
    write_line("  inventory list");
    write_line("  event list");
    write_line("  mount list");
    write_line("  settings get");
    write_line("");
    write_line("Talks to the running Aegra Service over \\\\.\\pipe\\aegra-service-control.");
    write_line("The CLI does not execute backup or restore data-plane work.");
}

base::Result<Options> parse_args(const int argc, wchar_t** argv) {
    Options options;
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
    for (int index = 1; index < argc; ++index) {
        auto converted = utf8_from_wide(argv[index] != nullptr ? argv[index] : L"");
        if (!converted) {
            return base::Result<Options>::failure(converted.error());
        }
        args.push_back(std::move(converted).value());
    }
    const auto help = consume_flag(args, "--help") || consume_flag(args, "-h") || args.empty();
    auto flags = apply_common_flags(options, args);
    if (!flags) {
        return base::Result<Options>::failure(flags.error());
    }
    auto named = apply_named_values(options, args);
    if (!named) {
        return base::Result<Options>::failure(named.error());
    }
    if (help) {
        options.command = Command::kHelp;
        return base::Result<Options>::success(std::move(options));
    }
    auto command = parse_noun_verb(args);
    if (!command) {
        return base::Result<Options>::failure(command.error());
    }
    options.command = command.value();
    std::size_t used = 1;
    if (options.command != Command::kStatus && args.size() >= 2 && !args[1].starts_with('-')) {
        used = 2;
    }
    if (!options.id && used < args.size() && command_takes_positional_id(options.command) &&
        !args[used].starts_with('-')) {
        options.id = args[used];
        ++used;
    }
    if (args.size() > used) {
        return base::Result<Options>::failure(usage_error("unexpected argument"));
    }
    return base::Result<Options>::success(std::move(options));
}

} // namespace aegra::apps::cli
