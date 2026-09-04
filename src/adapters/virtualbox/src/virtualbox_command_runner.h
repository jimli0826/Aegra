#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/process_launcher.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::virtualbox::detail {

struct VirtualBoxCommandResult final {
    std::uint32_t exit_code{0};
    std::string output;
};

class VirtualBoxCommandRunner final {
  public:
    VirtualBoxCommandRunner(ports::IProcessLauncher& launcher, std::string executable_path);

    [[nodiscard]] base::Result<VirtualBoxCommandResult>
    run(const std::vector<std::string>& arguments, std::string_view user_home,
        base::CancellationToken cancellation);

  private:
    ports::IProcessLauncher* launcher_{nullptr};
    std::string executable_path_;
};

} // namespace aegra::adapters::virtualbox::detail
