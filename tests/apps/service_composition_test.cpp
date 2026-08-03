#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/base/cancellation.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/service_control.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <random>
#include <string>

namespace {

using namespace aegra;
using namespace aegra::apps::service;
using namespace aegra::adapters::sqlite;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        std::random_device entropy;
        path_ = std::filesystem::temp_directory_path() /
                ("aegra_comp_test_" + std::to_string(entropy()) + "_" + std::to_string(entropy()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

int run_tests() {
    TemporaryDirectory temp_dir;

    bool passed = true;

    // Open SQLite control plane database.
    SqliteControlPlaneOpenRequest req{temp_dir.path() / L"control-plane.db",
                                      SqliteOpenMode::kCreateIfMissing};
    auto db = SqliteControlPlaneDatabase::open(req);
    passed &= expect(db.has_value(), "sqlite control plane opens");
    if (!passed) {
        return EXIT_FAILURE;
    }

    // Insert a running job.
    {
        auto uow = db.value()->begin_unit_of_work({});
        passed &= expect(uow.has_value(), "begin_unit_of_work succeeds");
        if (!passed) {
            return EXIT_FAILURE;
        }

        ports::JobRecord job;
        job.job_id = "test-job-1";
        job.trace_id = "trace-1";
        job.operation = contracts::JobOperation::kBackup;
        job.state = contracts::ServiceJobState::kRunning;
        job.created_utc_ms = 1000;
        job.started_utc_ms = 1000;
        job.source_id = "source-1";
        job.backup_type = contracts::BackupType::kFull;
        job.message_code = "job.running";
        job.idempotency_key = "backup-1";

        auto inserted = uow.value()->jobs().insert(job, {});
        passed &= expect(inserted.has_value(), "job insert succeeds");
        if (!inserted) {
            uow.value()->rollback();
            return EXIT_FAILURE;
        }

        auto committed = uow.value()->commit({});
        passed &= expect(committed.has_value(), "commit succeeds after insert");
    }

    // Call mark_active_as_interrupted.
    {
        auto uow = db.value()->begin_unit_of_work({});
        passed &= expect(uow.has_value(), "begin_unit_of_work for interrupt");

        auto count = uow.value()->jobs().mark_active_as_interrupted(2000, {});
        passed &= expect(count.has_value(), "mark_active_as_interrupted succeeds");
        if (!count) {
            uow.value()->rollback();
            return EXIT_FAILURE;
        }
        passed &= expect(count.value() == 1, "mark_active_as_interrupted updates 1 row");

        auto committed = uow.value()->commit({});
        passed &= expect(committed.has_value(), "commit succeeds after interrupt");
    }

    // Verify job is now interrupted.
    {
        auto fetched = db.value()->get_job("test-job-1", {});
        passed &= expect(fetched.has_value(), "get_job succeeds");
        passed &= expect(fetched.value().has_value(), "job record found");
        passed &= expect(fetched.value()->state == contracts::ServiceJobState::kInterrupted,
                         "job state is interrupted");
        passed &=
            expect(fetched.value()->completed_utc_ms == 2000, "completed_utc_ms set correctly");
    }

    // Wire up ServiceRuntimeInfo.
    ServiceRuntimeInfo runtime;
    runtime.service_version = "1.0.0";
    runtime.capabilities = {"job.list"};
    runtime.control_plane = db.value().get();

    // GetServiceInfo.
    {
        contracts::ServiceRequest info_req;
        info_req.request_id = "info-1";
        info_req.kind = contracts::ServiceRequestKind::kGetServiceInfo;
        info_req.payload = contracts::ServiceVersionRange{contracts::kServiceApiVersion,
                                                          contracts::kServiceApiVersion};

        auto info_res = dispatch_service_request(info_req, runtime, {});
        passed &= expect(info_res.has_value(), "service info dispatch succeeds");
        passed &= expect(info_res.value().boundary_error_code == base::ErrorCode::kNone,
                         "service info no error");
        passed &= expect(info_res.value().kind == contracts::ServiceResponseKind::kQueryResult,
                         "service info returns query result");

        passed &= expect(std::holds_alternative<contracts::ServiceInfo>(info_res.value().payload),
                         "service info payload type matches");
        if (!passed) {
            return EXIT_FAILURE;
        }
        auto info = std::get<contracts::ServiceInfo>(info_res.value().payload);
        auto it = std::find(info.capabilities.begin(), info.capabilities.end(), "job.list");
        passed &= expect(it != info.capabilities.end(), "capabilities include job.list");
    }

    // ListJobs.
    {
        contracts::ServiceRequest list_req;
        list_req.request_id = "list-1";
        list_req.kind = contracts::ServiceRequestKind::kListJobs;
        contracts::JobListRequest list_payload;
        list_req.payload = list_payload;

        auto list_res = dispatch_service_request(list_req, runtime, {});
        passed &= expect(list_res.has_value(), "list jobs dispatch succeeds");
        passed &= expect(list_res.value().boundary_error_code == base::ErrorCode::kNone,
                         "list jobs no error");
        passed &= expect(list_res.value().kind == contracts::ServiceResponseKind::kQueryResult,
                         "list jobs returns query result");
        passed &= expect(list_res.value().message_code == "control_plane.ready",
                         "list jobs message code is control_plane.ready");

        passed &= expect(std::holds_alternative<contracts::JobPage>(list_res.value().payload),
                         "list jobs payload type matches");
        if (!passed) {
            return EXIT_FAILURE;
        }
        auto page = std::get<contracts::JobPage>(list_res.value().payload);
        passed &= expect(page.items.size() == 1, "list jobs returns 1 item");
        passed &= expect(page.items[0].job_id == "test-job-1", "listed job_id matches");
    }

    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[FAIL] unexpected exception: %s\n", exception.what());
        return EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
