#include "vmdk_disk_image.h"

#include "cow_backing_store.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <utility>

namespace aegra::adapters::dokan::detail {
namespace {

constexpr std::uint64_t kSectorSize = 512;
constexpr std::uint64_t kLegacyHeads = 255;
constexpr std::uint64_t kLegacySectors = 63;

std::uint64_t capacity_sectors(const std::uint64_t bytes) noexcept {
    return (bytes + kSectorSize - 1) / kSectorSize;
}

std::string uuid_text(const std::array<std::uint8_t, 16>& uuid) {
    std::array<char, 37> text{};
    std::snprintf(text.data(), text.size(),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8],
                  uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
    return text.data();
}

std::string build_descriptor(const std::uint64_t raw_size_bytes,
                             const VmdkDescriptorIdentity& identity) {
    const std::uint64_t sectors = capacity_sectors(raw_size_bytes);
    const std::uint64_t sectors_per_cylinder = kLegacyHeads * kLegacySectors;
    const std::uint64_t cylinders =
        (std::min)((std::max)(sectors / sectors_per_cylinder, 1ULL), 65535ULL);

    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "# Disk DescriptorFile\n"
        << "version=1\n"
        << "encoding=\"UTF-8\"\n"
        << "CID=" << std::hex << std::setfill('0') << std::setw(8) << identity.cid << std::dec
        << "\n"
        << "parentCID=ffffffff\n"
        << "createType=\"monolithicFlat\"\n\n"
        << "# Extent description\n"
        << "RW " << sectors << " FLAT \"base-flat.vmdk\" 0\n\n"
        << "# The Disk Data Base\n"
        << "#DDB\n\n"
        << "ddb.virtualHWVersion = \"21\"\n"
        << "ddb.geometry.cylinders = \"" << cylinders << "\"\n"
        << "ddb.geometry.heads = \"255\"\n"
        << "ddb.geometry.sectors = \"63\"\n"
        << "ddb.adapterType = \"lsilogic\"\n"
        << "ddb.uuid.image = \"" << uuid_text(identity.image_uuid) << "\"\n"
        << "ddb.uuid.parent = \"00000000-0000-0000-0000-000000000000\"\n"
        << "ddb.uuid.modification = \"" << uuid_text(identity.modification_uuid) << "\"\n"
        << "ddb.uuid.parentmodification = "
           "\"00000000-0000-0000-0000-000000000000\"\n";
    return out.str();
}

} // namespace

VmdkDescriptorImage::VmdkDescriptorImage(CowBackingStore& backing,
                                         const std::uint64_t raw_size_bytes,
                                         VmdkDescriptorIdentity identity)
    : DiskImage(backing), raw_size_bytes_(raw_size_bytes), identity_(std::move(identity)) {}

void VmdkDescriptorImage::rebuild_locked() {
    const std::string text = build_descriptor(raw_size_bytes_, identity_);
    descriptor_.assign(text.begin(), text.end());
}

NTSTATUS VmdkDescriptorImage::read_locked(const std::uint64_t offset, void* buffer,
                                          DWORD buffer_len, LPDWORD bytes_read) {
    *bytes_read = 0;
    if (offset >= descriptor_.size()) {
        return STATUS_END_OF_FILE;
    }

    const std::uint64_t available = descriptor_.size() - offset;
    if (buffer_len > available) {
        buffer_len = static_cast<DWORD>(available);
    }
    std::memcpy(buffer, descriptor_.data() + offset, buffer_len);
    *bytes_read = buffer_len;
    return STATUS_SUCCESS;
}

NTSTATUS VmdkDescriptorImage::write_locked(std::uint64_t, const void*, DWORD,
                                           LPDWORD bytes_written) {
    *bytes_written = 0;
    return STATUS_MEDIA_WRITE_PROTECTED;
}

std::uint64_t VmdkDescriptorImage::file_size_locked() const { return descriptor_.size(); }

std::uint64_t VmdkDescriptorImage::data_offset_locked() const { return descriptor_.size(); }

std::uint64_t VmdkDescriptorImage::unit_size_locked() const { return kSectorSize; }

void VmdkFlatExtentImage::rebuild_locked() {
    const std::uint64_t raw_size = static_cast<std::uint64_t>(backing_.size());
    if (raw_size > (std::numeric_limits<std::uint64_t>::max)() - kSectorSize + 1) {
        extent_size_bytes_ = 0;
        return;
    }
    extent_size_bytes_ = capacity_sectors(raw_size) * kSectorSize;
}

NTSTATUS VmdkFlatExtentImage::read_locked(const std::uint64_t offset, void* buffer,
                                          DWORD buffer_len, LPDWORD bytes_read) {
    *bytes_read = 0;
    if (offset >= extent_size_bytes_) {
        return STATUS_END_OF_FILE;
    }

    const std::uint64_t available = extent_size_bytes_ - offset;
    if (buffer_len > available) {
        buffer_len = static_cast<DWORD>(available);
    }
    return backing_.read(buffer, buffer_len, bytes_read, static_cast<LONGLONG>(offset));
}

NTSTATUS VmdkFlatExtentImage::write_locked(std::uint64_t, const void*, DWORD,
                                           LPDWORD bytes_written) {
    *bytes_written = 0;
    return STATUS_MEDIA_WRITE_PROTECTED;
}

} // namespace aegra::adapters::dokan::detail
