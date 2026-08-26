#include "pe_ui.h"

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*command_line*/,
                    int /*show_command*/) {
    try {
        return aegra::apps::pe_restore::run_pe_restore_ui();
    } catch (...) {
        return 1;
    }
}
