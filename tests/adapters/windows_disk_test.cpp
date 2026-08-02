#include "aegra/adapters/windows_disk/windows_disk.h"

#include "aegra/base/error.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <span>
#include <string>

namespace {

namespace windows_disk = aegra::adapters::windows_disk;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

class TemporaryFile final {
  public:
    TemporaryFile() {
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-windows-disk-test-" + std::to_string(GetCurrentProcessId()) + ".bin");
        std::ofstream stream(path_, std::ios::binary | std::ios::trunc);
        stream.write(kContents.data(), static_cast<std::streamsize>(kContents.size()));
    }

    ~TemporaryFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;
    TemporaryFile(TemporaryFile&&) = delete;
    TemporaryFile& operator=(TemporaryFile&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    inline static constexpr std::string_view kContents = "0123456789abcdef";

  private:
    std::filesystem::path path_;
};

bool test_path_policy() {
    using windows_disk::WindowsBlockSource;
    bool passed = expect(WindowsBlockSource::is_vss_snapshot_device_path(
                             LR"(\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy42)"),
                         "canonical VSS snapshot path is accepted");
    passed &= expect(!WindowsBlockSource::is_vss_snapshot_device_path(
                         LR"(\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy)"),
                     "VSS snapshot number is required");
    passed &= expect(!WindowsBlockSource::is_vss_snapshot_device_path(
                         LR"(\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy42\)"),
                     "VSS snapshot suffix is rejected");
    passed &= expect(!WindowsBlockSource::is_vss_snapshot_device_path(
                         LR"(\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy4x)"),
                     "VSS snapshot number must be decimal");
    passed &= expect(!WindowsBlockSource::is_vss_snapshot_device_path(
                         LR"(\\?\GLOBALROOT\Device\HarddiskVolume1)"),
                     "ordinary volume device is not a VSS snapshot");
    passed &= expect(windows_disk::WindowsBlockSink::is_canonical_volume_guid_path(
                         LR"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdef}\)"),
                     "canonical Volume GUID sink path is accepted");
    passed &= expect(!windows_disk::WindowsBlockSink::is_canonical_volume_guid_path(LR"(C:\)"),
                     "drive letter sink path is rejected");
    passed &= expect(!windows_disk::WindowsBlockSink::is_canonical_volume_guid_path(
                         LR"(\\?\Volume{01234567-89ab-cdef-0123-456789abcdef})"),
                     "Volume GUID sink requires a trailing slash");
    return passed;
}

bool test_open_validation(const TemporaryFile& file) {
    using windows_disk::WindowsBlockSource;
    using windows_disk::WindowsBlockSourceKind;
    using windows_disk::WindowsBlockSourceOpenRequest;

    auto missing = WindowsBlockSource::open(
        WindowsBlockSourceOpenRequest{file.path().wstring() + L".missing"});
    bool passed = expect(!missing && missing.error().code == aegra::base::ErrorCode::kNotFound,
                         "missing stable file reports not found");
    auto physical =
        WindowsBlockSource::open(WindowsBlockSourceOpenRequest{LR"(\\.\PhysicalDrive0)"});
    passed &= expect(!physical && physical.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "stable mode rejects physical device paths");
    auto online_volume = WindowsBlockSource::open(
        WindowsBlockSourceOpenRequest{LR"(\\?\GLOBALROOT\Device\HarddiskVolume1)"});
    passed &= expect(!online_volume &&
                         online_volume.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "stable mode rejects online volume device paths");
    auto incomplete_snapshot = WindowsBlockSource::open(WindowsBlockSourceOpenRequest{
        LR"(\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy42)",
        WindowsBlockSourceKind::kVssSnapshot,
    });
    passed &= expect(!incomplete_snapshot && incomplete_snapshot.error().code ==
                                                 aegra::base::ErrorCode::kInvalidArgument,
                     "VSS source requires its captured volume size");
    auto mismatch = WindowsBlockSource::open(WindowsBlockSourceOpenRequest{
        file.path(), WindowsBlockSourceKind::kStableFile, TemporaryFile::kContents.size() + 1U});
    passed &= expect(!mismatch && mismatch.error().code == aegra::base::ErrorCode::kConflict,
                     "stable file expected size mismatch is rejected");
    return passed;
}

bool test_read_contract(const TemporaryFile& file) {
    auto opened = windows_disk::WindowsBlockSource::open({file.path()});
    bool passed = expect(opened.has_value(), "stable file opens for overlapped reads");
    if (!opened) {
        return false;
    }
    auto& source = *opened.value();
    passed &= expect(source.size_bytes() == TemporaryFile::kContents.size(),
                     "stable file size is reported");

    std::array<std::byte, 4> buffer{};
    auto read = source.read(3, buffer, {});
    passed &= expect(read && read.value() == buffer.size(), "nonzero offset read succeeds");
    passed &= expect(buffer[0] == std::byte{'3'} && buffer[3] == std::byte{'6'},
                     "read returns bytes from the requested offset");
    auto eof = source.read(source.size_bytes(), buffer, {});
    passed &= expect(eof && eof.value() == 0, "read at EOF returns zero");
    auto short_read = source.read(source.size_bytes() - 2U, buffer, {});
    passed &=
        expect(short_read && short_read.value() == 2U, "read is shortened at the source boundary");
    auto invalid = source.read(source.size_bytes() + 1U, buffer, {});
    passed &= expect(!invalid && invalid.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "read beyond EOF is rejected");
    return passed;
}

bool test_cancellation_and_concurrency(const TemporaryFile& file) {
    auto opened = windows_disk::WindowsBlockSource::open({file.path()});
    if (!opened) {
        return false;
    }
    auto& source = *opened.value();
    std::array<std::byte, 2> cancelled_buffer{};
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled = source.read(0, cancelled_buffer, cancellation.get_token());
    bool passed = expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled,
                         "pre-cancelled read reports cancellation");

    auto reader = [&source](const std::uint64_t offset) {
        std::array<std::byte, 2> buffer{};
        auto result = source.read(offset, buffer, {});
        return result && result.value() == buffer.size() &&
               buffer[0] == static_cast<std::byte>('0' + offset);
    };
    auto first = std::async(std::launch::async, reader, 1U);
    auto second = std::async(std::launch::async, reader, 7U);
    passed &=
        expect(first.get() && second.get(), "independent overlapped reads may run concurrently");
    return passed;
}

bool test_sink_contract(const TemporaryFile& file) {
    auto opened = windows_disk::WindowsBlockSink::open({file.path()});
    bool passed = expect(opened && opened.value()->capacity_bytes() ==
                                      TemporaryFile::kContents.size(),
                         "stable file sink opens with its fixed capacity");
    if (!opened) {
        return false;
    }
    const std::array<std::byte, 4> replacement{std::byte{'A'}, std::byte{'E'}, std::byte{'G'},
                                                std::byte{'R'}};
    auto written = opened.value()->write(4, replacement, {});
    passed &= expect(written.has_value() && opened.value()->flush({}).has_value(),
                     "stable file sink writes and flushes by offset");
    std::ifstream input(file.path(), std::ios::binary);
    std::array<char, 4> actual{};
    input.seekg(4);
    input.read(actual.data(), static_cast<std::streamsize>(actual.size()));
    passed &= expect(std::string_view(actual.data(), actual.size()) == "AEGR",
                     "stable file sink writes the expected bytes");
    auto overflow = opened.value()->write(opened.value()->capacity_bytes() - 1, replacement, {});
    passed &= expect(!overflow && overflow.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "stable file sink rejects writes beyond capacity");
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled = opened.value()->write(0, replacement, cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled,
                     "stable file sink observes pre-cancellation");
    return passed;
}

int run_tests() {
    const TemporaryFile file;
    const bool passed = test_path_policy() && test_open_validation(file) &&
                        test_read_contract(file) && test_cancellation_and_concurrency(file) &&
                        test_sink_contract(file);
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
