#pragma once

#include "aegra/base/error.h"
#include "aegra/contracts/service.h"

#include <string_view>

namespace aegra::apps::cli {

void configure_console();
void write_line(std::string_view text);
void write_error(std::string_view text);
void write_failed_response(const contracts::ServiceResponse& response);
void write_error_object(const base::Error& error);

} // namespace aegra::apps::cli
