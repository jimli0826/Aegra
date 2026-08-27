#include "cli_io.h"

#include "aegra/base/error.h"

#include <Windows.h>

#include <iostream>
#include <string>

namespace aegra::apps::cli {
namespace {

void write_stream(std::ostream& stream, const std::string_view text) {
    stream << text << '\n';
}

} // namespace

void configure_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void write_line(const std::string_view text) {
    write_stream(std::cout, text);
}

void write_error(const std::string_view text) {
    write_stream(std::cerr, text);
}

void write_error_object(const base::Error& error) {
    std::string line = "error: ";
    line += base::error_code_name(error.code);
    if (!error.message.empty()) {
        line += ": ";
        line += error.message;
    }
    write_error(line);
}

void write_failed_response(const contracts::ServiceResponse& response) {
    std::string line = "request failed: ";
    line += response.message_code;
    line += " (";
    line += base::error_code_name(response.boundary_error_code);
    line += ')';
    write_error(line);
    for (const auto& argument : response.message_arguments) {
        std::string detail = "  ";
        detail += argument.name;
        detail += '=';
        detail += argument.value;
        write_error(detail);
    }
}

} // namespace aegra::apps::cli
