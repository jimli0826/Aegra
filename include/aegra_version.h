// Aegra Image product / file version (single source of truth).
// Edit numbers here only. EXE/DLL VERSIONINFO and CMake project() read this file.
// Format: Major.Minor.Patch.Build
#pragma once

#define AEGRI_VERSION_MAJOR 0
#define AEGRI_VERSION_MINOR 9
#define AEGRI_VERSION_PATCH 0
#define AEGRI_VERSION_BUILD 2

#define AEGRI_COMPANY_NAME     "Aegra"
#define AEGRI_PRODUCT_NAME     "Aegra Image"
#define AEGRI_COPYRIGHT        "Copyright (c) 2026 Aegra"
#define AEGRI_LEGAL_TRADEMARKS ""

#define AEGRI_VERSION_COMMA \
    AEGRI_VERSION_MAJOR, AEGRI_VERSION_MINOR, AEGRI_VERSION_PATCH, AEGRI_VERSION_BUILD

#define AEGRI_STRINGIFY_X(x) #x
#define AEGRI_STRINGIFY(x) AEGRI_STRINGIFY_X(x)

#define AEGRI_VERSION_STRING       \
    AEGRI_STRINGIFY(AEGRI_VERSION_MAJOR) "." \
    AEGRI_STRINGIFY(AEGRI_VERSION_MINOR) "." \
    AEGRI_STRINGIFY(AEGRI_VERSION_PATCH) "." \
    AEGRI_STRINGIFY(AEGRI_VERSION_BUILD)
