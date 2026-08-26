#pragma once

namespace aegra::apps::pe_restore {

/// Creates the fullscreen recovery window, runs the restore flow on a background
/// thread, pumps the message loop, and returns the process exit code.
[[nodiscard]] int run_pe_restore_ui();

} // namespace aegra::apps::pe_restore
