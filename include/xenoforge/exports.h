// The system's exports by ordinal: xboxkrnl, xam and xbdm, from Xenia's
// tables (src/exports/*.inc, BSD; see LICENSE-xenia).

#pragma once

#include <cstdint>
#include <string_view>

namespace xf {

enum class ExportKind : uint8_t { Function, Variable };

struct Export {
    uint16_t ordinal;
    const char *name;
    ExportKind kind;
};

// The library's export with that ordinal, by the import library's name
// ("xboxkrnl.exe", "xam.xex", "xbdm.xex"; case and extension ignored).
const Export *find_export(std::string_view library, uint16_t ordinal);

// Whether the library is one of the system's (as opposed to a game's DLL).
bool is_system_library(std::string_view library);

} // namespace xf
