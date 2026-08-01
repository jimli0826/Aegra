#pragma once

#include <cstdint>
#include <string>

namespace aegra::ports {

enum class TaskPhase : std::uint8_t {
    kPreparing = 1,
    kReading = 2,
    kTransforming = 3,
    kWriting = 4,
    kCommitting = 5,
    kCompleted = 6,
};

struct ProgressEvent final {
    std::uint32_t schema_version{1};
    std::string job_id;
    TaskPhase phase{TaskPhase::kPreparing};
    std::uint64_t logical_bytes{0};
    std::uint64_t processed_bytes{0};
    std::uint64_t stored_bytes{0};
    std::string message_code;
};

class IProgressSink {
public:
    IProgressSink() = default;
    virtual ~IProgressSink() = default;
    IProgressSink(const IProgressSink&) = delete;
    IProgressSink& operator=(const IProgressSink&) = delete;
    IProgressSink(IProgressSink&&) = delete;
    IProgressSink& operator=(IProgressSink&&) = delete;

    virtual void publish(const ProgressEvent& event) = 0;
};

} // namespace aegra::ports
