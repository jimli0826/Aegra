#include "aegra/adapters/windows_pe/pe_pending_store.h"

#include "pe_pending_internal.h"

#include <optional>
#include <utility>

namespace aegra::adapters::windows_pe {
namespace {

using detail::pe_store_error;

[[nodiscard]] base::Result<void> check_cancelled(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            pe_store_error(base::ErrorCode::kCancelled, "pe pending store operation cancelled"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_job_key_for_mode(const contracts::PePendingJobV1& job,
                          const std::span<const std::byte> job_key) {
    const bool sealed = job.envelope.mode == contracts::PeEnvelopeMode::kSealed;
    if (sealed && job_key.size() != contracts::kPeEnvelopeKeySize) {
        return base::Result<void>::failure(pe_store_error(
            base::ErrorCode::kInvalidArgument, "sealed pe job requires a 32-byte job key"));
    }
    if (!sealed && !job_key.empty()) {
        return base::Result<void>::failure(pe_store_error(
            base::ErrorCode::kInvalidArgument, "job key is only valid for sealed pe jobs"));
    }
    return base::Result<void>::success();
}

class WindowsPePendingStore final : public ports::IPePendingJobStore {
  public:
    explicit WindowsPePendingStore(std::wstring pending_directory)
        : pending_directory_(std::move(pending_directory)),
          job_path_(pending_directory_ + L"\\" + std::wstring(detail::kPendingJobFileName)),
          key_path_(pending_directory_ + L"\\" + std::wstring(detail::kPendingKeyFileName)),
          result_path_(pending_directory_ + L"\\" +
                       std::wstring(detail::kPendingResultFileName)) {}

    [[nodiscard]] base::Result<void>
    write_pending(const contracts::PePendingJobV1& job, const std::span<const std::byte> job_key,
                  const base::CancellationToken cancellation) override {
        if (auto active = check_cancelled(cancellation); !active) {
            return active;
        }
        if (auto key_shape = validate_job_key_for_mode(job, job_key); !key_shape) {
            return key_shape;
        }
        auto encoded = detail::encode_pe_pending_job(job);
        if (!encoded) {
            return base::Result<void>::failure(encoded.error());
        }
        if (detail::file_exists(job_path_)) {
            return base::Result<void>::failure(
                pe_store_error(base::ErrorCode::kConflict, "a pe pending job already exists"));
        }
        if (auto directories = detail::ensure_directory_exists(pending_directory_); !directories) {
            return directories;
        }
        const bool sealed = job.envelope.mode == contracts::PeEnvelopeMode::kSealed;
        if (sealed) {
            if (auto key_written = detail::write_key_file(key_path_, job_key); !key_written) {
                return key_written;
            }
        }
        auto published = detail::write_document_atomically(job_path_, encoded.value(),
                                                           /*replace_existing=*/false);
        if (!published && sealed) {
            // Never leave an orphan key behind a failed job publish.
            (void)detail::scrub_and_delete_key_file(key_path_);
        }
        return published;
    }

    [[nodiscard]] base::Result<contracts::PePendingJobV1>
    read_pending(const base::CancellationToken cancellation) override {
        if (auto active = check_cancelled(cancellation); !active) {
            return base::Result<contracts::PePendingJobV1>::failure(active.error());
        }
        auto content = detail::read_bounded_file(job_path_, detail::kMaximumJobDocumentSize);
        if (!content) {
            return base::Result<contracts::PePendingJobV1>::failure(content.error());
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) UTF-8 text view of bytes.
        const std::string_view text(reinterpret_cast<const char*>(content.value().data()),
                                    content.value().size());
        return detail::decode_pe_pending_job(text);
    }

    [[nodiscard]] base::Result<std::vector<std::byte>>
    read_job_key(const base::CancellationToken cancellation) override {
        if (auto active = check_cancelled(cancellation); !active) {
            return base::Result<std::vector<std::byte>>::failure(active.error());
        }
        auto content = detail::read_bounded_file(key_path_, 4096);
        if (!content) {
            return content;
        }
        if (content.value().size() != contracts::kPeEnvelopeKeySize) {
            SecureZeroMemory(content.value().data(), content.value().size());
            return base::Result<std::vector<std::byte>>::failure(
                pe_store_error(base::ErrorCode::kCorruptData, "pe job key size is invalid"));
        }
        return content;
    }

    [[nodiscard]] base::Result<void> consume_job_key() override {
        return detail::scrub_and_delete_key_file(key_path_);
    }

    [[nodiscard]] base::Result<void> clear_pending() override {
        auto key_cleared = detail::scrub_and_delete_key_file(key_path_);
        auto job_cleared = detail::delete_file_if_exists(job_path_);
        if (!key_cleared) {
            return key_cleared;
        }
        return job_cleared;
    }

    [[nodiscard]] base::Result<void>
    write_result(const contracts::PeRestoreResultV1& result,
                 const base::CancellationToken cancellation) override {
        if (auto active = check_cancelled(cancellation); !active) {
            return active;
        }
        auto encoded = detail::encode_pe_restore_result(result);
        if (!encoded) {
            return base::Result<void>::failure(encoded.error());
        }
        if (auto directories = detail::ensure_directory_exists(pending_directory_); !directories) {
            return directories;
        }
        return detail::write_document_atomically(result_path_, encoded.value(),
                                                 /*replace_existing=*/true);
    }

    [[nodiscard]] base::Result<std::optional<contracts::PeRestoreResultV1>>
    read_result(const base::CancellationToken cancellation) override {
        using Output = base::Result<std::optional<contracts::PeRestoreResultV1>>;
        if (auto active = check_cancelled(cancellation); !active) {
            return Output::failure(active.error());
        }
        auto content = detail::read_bounded_file(result_path_, detail::kMaximumResultDocumentSize);
        if (!content) {
            if (content.error().code == base::ErrorCode::kNotFound) {
                return Output::success(std::nullopt);
            }
            return Output::failure(content.error());
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) UTF-8 text view of bytes.
        const std::string_view text(reinterpret_cast<const char*>(content.value().data()),
                                    content.value().size());
        auto decoded = detail::decode_pe_restore_result(text);
        if (!decoded) {
            return Output::failure(decoded.error());
        }
        return Output::success(std::move(decoded).value());
    }

    [[nodiscard]] base::Result<void> clear_result() override {
        return detail::delete_file_if_exists(result_path_);
    }

  private:
    std::wstring pending_directory_;
    std::wstring job_path_;
    std::wstring key_path_;
    std::wstring result_path_;
};

} // namespace

base::Result<std::unique_ptr<ports::IPePendingJobStore>>
open_pe_pending_store(const PePendingStoreOpenRequest& request) {
    using Output = base::Result<std::unique_ptr<ports::IPePendingJobStore>>;
    if (request.data_dir_utf8.empty()) {
        return Output::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "data directory is required"));
    }
    auto data_dir = detail::utf8_to_wide(request.data_dir_utf8);
    if (!data_dir) {
        return Output::failure(data_dir.error());
    }
    std::wstring pending_directory = std::move(data_dir).value();
    while (!pending_directory.empty() && pending_directory.back() == L'\\') {
        pending_directory.pop_back();
    }
    if (pending_directory.empty()) {
        return Output::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "data directory is invalid"));
    }
    pending_directory += L'\\';
    pending_directory += detail::kPendingDirRelative;
    return Output::success(std::make_unique<WindowsPePendingStore>(std::move(pending_directory)));
}

namespace detail {

std::unique_ptr<ports::IPePendingJobStore>
make_pe_pending_store_for_directory(std::wstring pending_directory) {
    return std::make_unique<WindowsPePendingStore>(std::move(pending_directory));
}

} // namespace detail
} // namespace aegra::adapters::windows_pe
