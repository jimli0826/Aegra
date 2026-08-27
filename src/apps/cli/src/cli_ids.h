#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/random.h"

#include <string>
#include <string_view>

namespace aegra::apps::cli {

[[nodiscard]] base::Result<std::string>
make_random_uuid(ports::IRandomSource& random, const base::CancellationToken& cancellation);

[[nodiscard]] base::Result<std::string>
make_idempotency_key(std::string_view prefix, ports::IRandomSource& random,
                     const base::CancellationToken& cancellation);

} // namespace aegra::apps::cli
