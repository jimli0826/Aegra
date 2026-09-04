#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/random_access.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace aegra::adapters::dokan {

struct VmdkPresentationInfo final {
    std::filesystem::path descriptor_path;
    std::filesystem::path flat_extent_path;
    std::uint64_t virtual_size_bytes{0};
};

struct VmdkImageIdentity final {
    std::uint32_t cid{0};
    std::array<std::uint8_t, 16> image_uuid{};
    std::array<std::uint8_t, 16> modification_uuid{};
};

// Presents a borrowed whole-disk reader as a read-only VMDK descriptor and flat
// extent. The reader must outlive this object. The mount directory must be an
// empty local NTFS directory; the presenter never removes pre-existing content.
class ReadOnlyVmdkPresentation final {
  public:
    ~ReadOnlyVmdkPresentation();

    ReadOnlyVmdkPresentation(const ReadOnlyVmdkPresentation&) = delete;
    ReadOnlyVmdkPresentation& operator=(const ReadOnlyVmdkPresentation&) = delete;
    ReadOnlyVmdkPresentation(ReadOnlyVmdkPresentation&&) = delete;
    ReadOnlyVmdkPresentation& operator=(ReadOnlyVmdkPresentation&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>
    create(ports::IRandomAccessReader& reader, const std::filesystem::path& mount_directory,
           const VmdkImageIdentity& identity, base::CancellationToken cancellation = {});

    [[nodiscard]] const VmdkPresentationInfo& info() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    void close() noexcept;

  private:
    struct Impl;
    explicit ReadOnlyVmdkPresentation(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

struct VhdxPresentationInfo final {
    std::filesystem::path vhdx_path;
    std::uint64_t virtual_size_bytes{0};
};

// Presents a borrowed whole-disk reader as one read-only VHDX file (disk.vhdx)
// for hypervisors that require VHDX parents (Hyper-V differencing children).
// Same reader-lifetime and empty-NTFS-mount-directory rules as the VMDK
// presentation.
class ReadOnlyVhdxPresentation final {
  public:
    ~ReadOnlyVhdxPresentation();

    ReadOnlyVhdxPresentation(const ReadOnlyVhdxPresentation&) = delete;
    ReadOnlyVhdxPresentation& operator=(const ReadOnlyVhdxPresentation&) = delete;
    ReadOnlyVhdxPresentation(ReadOnlyVhdxPresentation&&) = delete;
    ReadOnlyVhdxPresentation& operator=(ReadOnlyVhdxPresentation&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>
    create(ports::IRandomAccessReader& reader, const std::filesystem::path& mount_directory,
           base::CancellationToken cancellation = {});

    [[nodiscard]] const VhdxPresentationInfo& info() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    void close() noexcept;

  private:
    struct Impl;
    explicit ReadOnlyVhdxPresentation(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::adapters::dokan
