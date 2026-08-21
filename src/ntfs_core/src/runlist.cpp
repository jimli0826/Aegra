#include "aegra/ntfs_core/runlist.h"

#include "aegra/ntfs_core/binary.h"

#include <array>
#include <limits>
#include <utility>
#include <vector>

namespace aegra::ntfs_core {
namespace {

[[nodiscard]] base::Error runlist_corrupt() {
    return make_error(base::ErrorCode::kCorruptData, "ntfs.runlist_corrupt");
}

[[nodiscard]] std::size_t unsigned_le_width(const std::uint64_t value) noexcept {
    std::size_t width = 1;
    while (width < 8 && (value >> (8U * width)) != 0) {
        ++width;
    }
    return width;
}

[[nodiscard]] base::Result<std::size_t> signed_le_width(const std::int64_t value) {
    std::array<std::byte, 8> scratch{};
    for (std::size_t width = 1; width <= 8; ++width) {
        write_signed_le(std::span<std::byte>(scratch), 0, value, width);
        if (read_signed_le(std::span<const std::byte>(scratch), 0, width) == value) {
            return base::Result<std::size_t>::success(width);
        }
    }
    return base::Result<std::size_t>::failure(runlist_corrupt());
}

[[nodiscard]] base::Result<std::int64_t> lcn_delta(const std::uint64_t previous_lcn,
                                                   const std::uint64_t next_lcn) {
    if (next_lcn >= previous_lcn) {
        const auto magnitude = next_lcn - previous_lcn;
        if (magnitude >
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return base::Result<std::int64_t>::failure(runlist_corrupt());
        }
        return base::Result<std::int64_t>::success(static_cast<std::int64_t>(magnitude));
    }
    const auto magnitude = previous_lcn - next_lcn;
    const auto max_positive =
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
    if (magnitude > max_positive + 1ULL) {
        return base::Result<std::int64_t>::failure(runlist_corrupt());
    }
    if (magnitude == max_positive + 1ULL) {
        return base::Result<std::int64_t>::success((std::numeric_limits<std::int64_t>::min)());
    }
    return base::Result<std::int64_t>::success(-static_cast<std::int64_t>(magnitude));
}

struct RunEncodePlan final {
    std::size_t length_size{0};
    std::size_t offset_size{0};
    std::int64_t delta{0};
    bool sparse{false};
    std::uint64_t cluster_count{0};
};

[[nodiscard]] base::Result<RunEncodePlan> plan_run(const DataRun& run,
                                                   const std::uint64_t previous_lcn) {
    if (run.cluster_count.value == 0) {
        return base::Result<RunEncodePlan>::failure(runlist_corrupt());
    }
    RunEncodePlan plan;
    plan.cluster_count = run.cluster_count.value;
    plan.length_size = unsigned_le_width(plan.cluster_count);
    plan.sparse = run.sparse;
    if (run.sparse) {
        plan.offset_size = 0;
        plan.delta = 0;
        return base::Result<RunEncodePlan>::success(plan);
    }
    auto delta = lcn_delta(previous_lcn, run.first_lcn.value);
    if (!delta) {
        return base::Result<RunEncodePlan>::failure(delta.error());
    }
    plan.delta = delta.value();
    auto width = signed_le_width(plan.delta);
    if (!width) {
        return base::Result<RunEncodePlan>::failure(width.error());
    }
    plan.offset_size = width.value();
    return base::Result<RunEncodePlan>::success(plan);
}

[[nodiscard]] bool ranges_overlap(const std::uint64_t a0, const std::uint64_t a_count,
                                  const std::uint64_t b0, const std::uint64_t b_count) noexcept {
    std::uint64_t a1 = 0;
    std::uint64_t b1 = 0;
    if (!checked_add_u64(a0, a_count, a1) || !checked_add_u64(b0, b_count, b1)) {
        return true;
    }
    return a0 < b1 && b0 < a1;
}

[[nodiscard]] base::Result<DataRun>
parse_one_run(const std::span<const std::byte> runlist, std::size_t& offset,
              std::uint64_t& current_vcn, std::uint64_t& current_lcn) {
    const auto header = std::to_integer<std::uint8_t>(runlist[offset]);
    if (header == 0) {
        return base::Result<DataRun>::failure(runlist_corrupt());
    }
    ++offset;
    const auto length_size = static_cast<std::size_t>(header & 0x0FU);
    const auto offset_size = static_cast<std::size_t>((header >> 4) & 0x0FU);
    if (length_size == 0 || length_size > 8 || offset_size > 8 ||
        length_size > runlist.size() - offset ||
        offset_size > runlist.size() - offset - length_size) {
        return base::Result<DataRun>::failure(runlist_corrupt());
    }
    std::uint64_t cluster_count = 0;
    for (std::size_t i = 0; i < length_size; ++i) {
        cluster_count |=
            static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(runlist[offset + i]))
            << (8U * i);
    }
    offset += length_size;
    if (cluster_count == 0) {
        return base::Result<DataRun>::failure(runlist_corrupt());
    }
    DataRun run;
    run.first_vcn.value = current_vcn;
    run.cluster_count.value = cluster_count;
    if (offset_size == 0) {
        run.sparse = true;
        run.first_lcn.value = 0;
    } else {
        const auto delta = read_signed_le(runlist, offset, offset_size);
        offset += offset_size;
        std::uint64_t next_lcn = 0;
        if (delta >= 0) {
            if (!checked_add_u64(current_lcn, static_cast<std::uint64_t>(delta), next_lcn)) {
                return base::Result<DataRun>::failure(runlist_corrupt());
            }
        } else {
            const auto magnitude = std::uint64_t{0} - static_cast<std::uint64_t>(delta);
            if (magnitude > current_lcn) {
                return base::Result<DataRun>::failure(runlist_corrupt());
            }
            next_lcn = current_lcn - magnitude;
        }
        current_lcn = next_lcn;
        run.sparse = false;
        run.first_lcn.value = next_lcn;
    }
    std::uint64_t next_vcn = 0;
    if (!checked_add_u64(current_vcn, cluster_count, next_vcn)) {
        return base::Result<DataRun>::failure(runlist_corrupt());
    }
    current_vcn = next_vcn;
    return base::Result<DataRun>::success(std::move(run));
}

} // namespace

base::Result<void> validate_data_runs(const std::span<const DataRun> runs) {
    if (runs.size() > kMaxDataRuns) {
        return base::Result<void>::failure(runlist_corrupt());
    }
    std::uint64_t expected_vcn = runs.empty() ? 0 : runs.front().first_vcn.value;
    std::uint64_t previous_lcn = 0;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const auto& run = runs[i];
        if (run.first_vcn.value != expected_vcn || run.cluster_count.value == 0) {
            return base::Result<void>::failure(runlist_corrupt());
        }
        std::uint64_t next_vcn = 0;
        if (!checked_add_u64(run.first_vcn.value, run.cluster_count.value, next_vcn)) {
            return base::Result<void>::failure(runlist_corrupt());
        }
        if (!run.sparse) {
            auto planned = plan_run(run, previous_lcn);
            if (!planned) {
                return base::Result<void>::failure(planned.error());
            }
            for (std::size_t j = 0; j < i; ++j) {
                const auto& earlier = runs[j];
                if (earlier.sparse) {
                    continue;
                }
                if (ranges_overlap(earlier.first_lcn.value, earlier.cluster_count.value,
                                   run.first_lcn.value, run.cluster_count.value)) {
                    return base::Result<void>::failure(runlist_corrupt());
                }
            }
            previous_lcn = run.first_lcn.value;
        }
        expected_vcn = next_vcn;
    }
    return base::Result<void>::success();
}

base::Result<std::vector<DataRun>> parse_runlist(const std::span<const std::byte> runlist,
                                                 const VirtualClusterNumber first_vcn,
                                                 const VirtualClusterNumber last_vcn) {
    std::vector<DataRun> runs;
    std::size_t offset = 0;
    std::uint64_t current_vcn = first_vcn.value;
    std::uint64_t current_lcn = 0;
    bool terminated = false;

    while (offset < runlist.size()) {
        if (std::to_integer<std::uint8_t>(runlist[offset]) == 0) {
            terminated = true;
            break;
        }
        auto run = parse_one_run(runlist, offset, current_vcn, current_lcn);
        if (!run) {
            return base::Result<std::vector<DataRun>>::failure(run.error());
        }
        runs.push_back(std::move(run).value());
        if (runs.size() > kMaxDataRuns) {
            return base::Result<std::vector<DataRun>>::failure(runlist_corrupt());
        }
    }

    if (!terminated) {
        return base::Result<std::vector<DataRun>>::failure(runlist_corrupt());
    }

    if (!runs.empty() && last_vcn.value >= first_vcn.value) {
        std::uint64_t expected_end = 0;
        if (!checked_add_u64(last_vcn.value, 1, expected_end) || current_vcn != expected_end) {
            return base::Result<std::vector<DataRun>>::failure(runlist_corrupt());
        }
    }
    return base::Result<std::vector<DataRun>>::success(std::move(runs));
}

base::Result<std::size_t> measure_runlist_encoded_size(const std::span<const DataRun> runs) {
    auto valid = validate_data_runs(runs);
    if (!valid) {
        return base::Result<std::size_t>::failure(valid.error());
    }
    std::size_t total = 1; // terminator
    std::uint64_t previous_lcn = 0;
    for (const auto& run : runs) {
        auto plan = plan_run(run, previous_lcn);
        if (!plan) {
            return base::Result<std::size_t>::failure(plan.error());
        }
        std::uint64_t entry = 0;
        if (!checked_add_u64(1, plan.value().length_size, entry) ||
            !checked_add_u64(entry, plan.value().offset_size, entry) ||
            !checked_add_u64(total, entry, entry)) {
            return base::Result<std::size_t>::failure(runlist_corrupt());
        }
        total = static_cast<std::size_t>(entry);
        if (!run.sparse) {
            previous_lcn = run.first_lcn.value;
        }
        if (total > kMaxRunlistEncodedBytes) {
            return base::Result<std::size_t>::failure(runlist_corrupt());
        }
    }
    return base::Result<std::size_t>::success(total);
}

base::Result<std::size_t> encode_runlist(const std::span<const DataRun> runs,
                                         const std::span<std::byte> destination) {
    auto measured = measure_runlist_encoded_size(runs);
    if (!measured) {
        return measured;
    }
    if (measured.value() > destination.size()) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kInsufficientSpace, "ntfs.runlist_corrupt"));
    }
    std::size_t offset = 0;
    std::uint64_t previous_lcn = 0;
    for (const auto& run : runs) {
        auto plan = plan_run(run, previous_lcn);
        if (!plan) {
            return base::Result<std::size_t>::failure(plan.error());
        }
        const auto header = static_cast<std::uint8_t>((plan.value().offset_size << 4) |
                                                      (plan.value().length_size & 0x0FU));
        destination[offset] = static_cast<std::byte>(header);
        ++offset;
        write_unsigned_le(destination, offset, plan.value().cluster_count, plan.value().length_size);
        offset += plan.value().length_size;
        if (plan.value().offset_size != 0) {
            write_signed_le(destination, offset, plan.value().delta, plan.value().offset_size);
            offset += plan.value().offset_size;
            previous_lcn = run.first_lcn.value;
        }
    }
    destination[offset] = std::byte{0};
    ++offset;
    return base::Result<std::size_t>::success(offset);
}

base::Result<std::vector<std::byte>>
encode_runlist_bounded(const std::span<const DataRun> runs, const std::size_t maximum_bytes) {
    if (maximum_bytes == 0 || maximum_bytes > kMaxRunlistEncodedBytes) {
        return base::Result<std::vector<std::byte>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.runlist_corrupt"));
    }
    auto measured = measure_runlist_encoded_size(runs);
    if (!measured) {
        return base::Result<std::vector<std::byte>>::failure(measured.error());
    }
    if (measured.value() > maximum_bytes) {
        return base::Result<std::vector<std::byte>>::failure(
            make_error(base::ErrorCode::kInsufficientSpace, "ntfs.runlist_corrupt"));
    }
    std::vector<std::byte> buffer(measured.value());
    auto written = encode_runlist(runs, std::span<std::byte>(buffer));
    if (!written) {
        return base::Result<std::vector<std::byte>>::failure(written.error());
    }
    buffer.resize(written.value());
    return base::Result<std::vector<std::byte>>::success(std::move(buffer));
}

} // namespace aegra::ntfs_core
