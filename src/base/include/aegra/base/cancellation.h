#pragma once

#include <stop_token>

namespace aegra::base {

using CancellationSource = std::stop_source;
using CancellationToken = std::stop_token;

} // namespace aegra::base
