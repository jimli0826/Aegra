// In-memory overlay for auxiliary (side-car) files next to the synthetic VHDX.
#pragma once

#include "dokan_ntstatus.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace aegra::adapters::dokan::detail {

struct AuxEntryInfo {
    bool is_directory = false;
    std::uint64_t size = 0;
    FILETIME creation{};
    FILETIME last_access{};
    FILETIME last_write{};
    std::wstring display_name;
};

class AuxFileStore final {
  public:
    [[nodiscard]] NTSTATUS create(LPCWSTR file_name, ULONG create_disposition,
                                  ULONG create_options, bool* out_is_directory);

    void cleanup(LPCWSTR file_name, bool delete_pending);

    [[nodiscard]] NTSTATUS read(LPCWSTR file_name, void* buffer, DWORD buffer_len,
                                LPDWORD bytes_read, LONGLONG offset);
    [[nodiscard]] NTSTATUS write(LPCWSTR file_name, const void* buffer,
                                 DWORD bytes_to_write, LPDWORD bytes_written,
                                 LONGLONG offset, BOOL write_to_end);

    [[nodiscard]] bool get_info(LPCWSTR file_name, AuxEntryInfo* out);

    [[nodiscard]] NTSTATUS set_end_of_file(LPCWSTR file_name, LONGLONG byte_offset);
    void set_times(LPCWSTR file_name, const FILETIME* creation,
                   const FILETIME* last_access, const FILETIME* last_write);

    [[nodiscard]] NTSTATUS remove_file(LPCWSTR file_name);
    [[nodiscard]] NTSTATUS remove_directory(LPCWSTR file_name);
    [[nodiscard]] NTSTATUS move(LPCWSTR file_name, LPCWSTR new_file_name,
                                BOOL replace_if_existing);

    void enumerate_children(LPCWSTR dir_path,
                            const std::function<void(const AuxEntryInfo&)>& fn);

  private:
    struct MemEntry {
        bool is_directory = false;
        std::wstring display_name;
        std::vector<std::uint8_t> data;
        FILETIME creation{};
        FILETIME last_access{};
        FILETIME last_write{};
    };

    [[nodiscard]] static std::wstring key(LPCWSTR path);
    [[nodiscard]] static std::wstring leaf_name(LPCWSTR path);
    [[nodiscard]] static std::wstring parent_key(const std::wstring& key);
    [[nodiscard]] bool parent_exists(const std::wstring& key);

    std::mutex mutex_;
    std::map<std::wstring, MemEntry> files_;
};

} // namespace aegra::adapters::dokan::detail
