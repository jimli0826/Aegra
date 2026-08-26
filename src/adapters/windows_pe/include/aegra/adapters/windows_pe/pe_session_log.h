#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace aegra::adapters::windows_pe {

/// Durable diagnostic log for the WinPE executor, written to the host volume at
/// `<data_dir>\pe\logs\pe_session_<timestamp>.log`. Every line is written through
/// the cache (FILE_FLAG_WRITE_THROUGH) so it survives the hard reset that is the
/// only way off a PE failure page. Never carries secrets.
class PeSessionLog final {
  public:
    ~PeSessionLog();
    PeSessionLog(const PeSessionLog&) = delete;
    PeSessionLog& operator=(const PeSessionLog&) = delete;
    PeSessionLog(PeSessionLog&&) = delete;
    PeSessionLog& operator=(PeSessionLog&&) = delete;

    /// nullptr on failure (diagnostics are best-effort by design).
    [[nodiscard]] static std::unique_ptr<PeSessionLog>
    open(const std::string& data_dir_utf8) noexcept;

    void line(std::string_view message) noexcept;

  private:
    struct Impl;
    explicit PeSessionLog(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::adapters::windows_pe
