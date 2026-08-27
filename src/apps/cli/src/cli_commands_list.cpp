#include "cli_commands.h"

#include "cli_commands_internal.h"
#include "cli_exit.h"
#include "cli_format.h"
#include "cli_io.h"

#include <utility>
#include <variant>
#include <vector>

namespace aegra::apps::cli {
namespace {

template <typename Page>
[[nodiscard]] const Page* query_page(const contracts::ServiceResponse& response) {
    return std::get_if<Page>(&response.payload);
}

} // namespace

int run_schedule_list(ServiceSession& session, const Options& options) {
    contracts::ScheduleListRequest request;
    request.page.maximum_results = kCliPageSize;
    request.enabled = options.enabled_filter;
    std::vector<contracts::ScheduleSummary> items;
    std::optional<std::string> token;
    do {
        request.page.continuation_token = token;
        auto response = session.transact(contracts::ServiceRequestKind::kListSchedules, request,
                                         false);
        if (!response) {
            return fail_request(response.error());
        }
        const auto printed = emit_query(session, options, response.value());
        if (printed != kExitOk) {
            return printed;
        }
        const auto* page = query_page<contracts::SchedulePage>(response.value());
        if (page == nullptr) {
            write_error("schedule list payload is missing");
            return kExitRequest;
        }
        items.insert(items.end(), page->items.begin(), page->items.end());
        auto next = advance_continuation(token, page->continuation_token, items.size());
        if (!next) {
            write_error_object(next.error());
            return kExitRequest;
        }
        token = std::move(next).value();
    } while (token);
    if (!options.json) {
        print_schedules(items);
    }
    return kExitOk;
}

int run_job_list(ServiceSession& session, const Options& options) {
    contracts::JobListRequest request;
    request.page.maximum_results = kCliPageSize;
    request.scope = options.job_scope;
    request.operation = options.job_operation;
    std::vector<contracts::JobSummary> items;
    std::optional<std::string> token;
    do {
        request.page.continuation_token = token;
        auto response =
            session.transact(contracts::ServiceRequestKind::kListJobs, request, false);
        if (!response) {
            return fail_request(response.error());
        }
        const auto printed = emit_query(session, options, response.value());
        if (printed != kExitOk) {
            return printed;
        }
        const auto* page = query_page<contracts::JobPage>(response.value());
        if (page == nullptr) {
            write_error("job list payload is missing");
            return kExitRequest;
        }
        items.insert(items.end(), page->items.begin(), page->items.end());
        auto next = advance_continuation(token, page->continuation_token, items.size());
        if (!next) {
            write_error_object(next.error());
            return kExitRequest;
        }
        token = std::move(next).value();
    } while (token);
    if (!options.json) {
        print_jobs(items);
    }
    return kExitOk;
}

int run_repository_list(ServiceSession& session, const Options& options) {
    contracts::RepositoryConnectionListRequest request;
    request.page.maximum_results = kCliPageSize;
    std::vector<contracts::RepositoryConnectionSummary> items;
    std::optional<std::string> token;
    do {
        request.page.continuation_token = token;
        auto response = session.transact(contracts::ServiceRequestKind::kListRepositoryConnections,
                                         request, false);
        if (!response) {
            return fail_request(response.error());
        }
        const auto printed = emit_query(session, options, response.value());
        if (printed != kExitOk) {
            return printed;
        }
        const auto* page = query_page<contracts::RepositoryConnectionPage>(response.value());
        if (page == nullptr) {
            write_error("repository list payload is missing");
            return kExitRequest;
        }
        items.insert(items.end(), page->items.begin(), page->items.end());
        auto next = advance_continuation(token, page->continuation_token, items.size());
        if (!next) {
            write_error_object(next.error());
            return kExitRequest;
        }
        token = std::move(next).value();
    } while (token);
    if (!options.json) {
        print_repositories(items);
    }
    return kExitOk;
}

int run_inventory_list(ServiceSession& session, const Options& options) {
    contracts::SourceInventoryListRequest request;
    request.page.maximum_results = kCliPageSize;
    std::vector<contracts::SourceInventoryItem> items;
    std::optional<std::string> token;
    do {
        request.page.continuation_token = token;
        auto response = session.transact(contracts::ServiceRequestKind::kListSourceInventory,
                                         request, false);
        if (!response) {
            return fail_request(response.error());
        }
        const auto printed = emit_query(session, options, response.value());
        if (printed != kExitOk) {
            return printed;
        }
        const auto* page = query_page<contracts::SourceInventoryPage>(response.value());
        if (page == nullptr) {
            write_error("inventory list payload is missing");
            return kExitRequest;
        }
        items.insert(items.end(), page->items.begin(), page->items.end());
        auto next = advance_continuation(token, page->continuation_token, items.size());
        if (!next) {
            write_error_object(next.error());
            return kExitRequest;
        }
        token = std::move(next).value();
    } while (token);
    if (!options.json) {
        print_inventory(items);
    }
    return kExitOk;
}

int run_event_list(ServiceSession& session, const Options& options) {
    contracts::AuditEventListRequest request;
    request.page.maximum_results = kCliPageSize;
    std::vector<contracts::AuditEventSummary> items;
    std::optional<std::string> token;
    do {
        request.page.continuation_token = token;
        auto response =
            session.transact(contracts::ServiceRequestKind::kListEvents, request, false);
        if (!response) {
            return fail_request(response.error());
        }
        const auto printed = emit_query(session, options, response.value());
        if (printed != kExitOk) {
            return printed;
        }
        const auto* page = query_page<contracts::AuditEventPage>(response.value());
        if (page == nullptr) {
            write_error("event list payload is missing");
            return kExitRequest;
        }
        items.insert(items.end(), page->items.begin(), page->items.end());
        auto next = advance_continuation(token, page->continuation_token, items.size());
        if (!next) {
            write_error_object(next.error());
            return kExitRequest;
        }
        token = std::move(next).value();
    } while (token);
    if (!options.json) {
        print_events(items);
    }
    return kExitOk;
}

int run_mount_list(ServiceSession& session, const Options& options) {
    contracts::MountSessionListRequest request;
    request.page.maximum_results = kCliPageSize;
    std::vector<contracts::MountSessionSummary> items;
    std::optional<std::string> token;
    do {
        request.page.continuation_token = token;
        auto response = session.transact(contracts::ServiceRequestKind::kListMountSessions, request,
                                         false);
        if (!response) {
            return fail_request(response.error());
        }
        const auto printed = emit_query(session, options, response.value());
        if (printed != kExitOk) {
            return printed;
        }
        const auto* page = query_page<contracts::MountSessionPage>(response.value());
        if (page == nullptr) {
            write_error("mount list payload is missing");
            return kExitRequest;
        }
        items.insert(items.end(), page->items.begin(), page->items.end());
        auto next = advance_continuation(token, page->continuation_token, items.size());
        if (!next) {
            write_error_object(next.error());
            return kExitRequest;
        }
        token = std::move(next).value();
    } while (token);
    if (!options.json) {
        print_mounts(items);
    }
    return kExitOk;
}

int run_recovery_point_list(ServiceSession& session, const Options& options) {
    contracts::ServiceRecoveryPointListRequest request;
    request.repository_connection_id = options.connection_id;
    request.page.maximum_results = kCliPageSize;
    std::vector<contracts::RecoveryPointSummary> items;
    std::optional<std::string> token;
    do {
        request.page.continuation_token = token;
        auto response = session.transact(contracts::ServiceRequestKind::kListRecoveryPoints,
                                         request, false);
        if (!response) {
            return fail_request(response.error());
        }
        const auto printed = emit_query(session, options, response.value());
        if (printed != kExitOk) {
            return printed;
        }
        const auto* page = query_page<contracts::ServiceRecoveryPointPage>(response.value());
        if (page == nullptr) {
            write_error("recovery point list payload is missing");
            return kExitRequest;
        }
        items.insert(items.end(), page->catalog.items.begin(), page->catalog.items.end());
        auto next =
            advance_continuation(token, page->catalog.continuation_token, items.size());
        if (!next) {
            write_error_object(next.error());
            return kExitRequest;
        }
        token = std::move(next).value();
    } while (token);
    if (!options.json) {
        print_recovery_points(items);
    }
    return kExitOk;
}

} // namespace aegra::apps::cli
