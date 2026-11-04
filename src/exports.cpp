// The system's exports by ordinal. The tables are Xenia's own files
// (src/exports/*.inc, copied unchanged from xenia-canary 838850fed, BSD; see
// LICENSE-xenia), read through XE_EXPORT.

#include "xenoforge/exports.h"

#include <algorithm>
#include <span>
#include <string>

namespace xf {
namespace {

constexpr ExportKind kFunction = ExportKind::Function;
constexpr ExportKind kVariable = ExportKind::Variable;

#define XE_EXPORT(module, ordinal, name, kind) Export{uint16_t(ordinal), #name, kind}

constexpr Export XBOXKRNL[] = {
#include "exports/xboxkrnl_table.inc"
};

constexpr Export XAM[] = {
#include "exports/xam_table.inc"
};

constexpr Export XBDM[] = {
#include "exports/xbdm_table.inc"
};

#undef XE_EXPORT

// "xboxkrnl.exe" -> "xboxkrnl", lower case
std::string base_name(std::string_view library) {
    std::string name(library.substr(0, library.find('.')));
    for (char &c : name)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return name;
}

std::span<const Export> table(std::string_view library) {
    std::string name = base_name(library);
    if (name == "xboxkrnl") return XBOXKRNL;
    if (name == "xam") return XAM;
    if (name == "xbdm") return XBDM;
    return {};
}

} // namespace

const Export *find_export(std::string_view library, uint16_t ordinal) {
    std::span<const Export> exports = table(library);
    auto it = std::find_if(exports.begin(), exports.end(), [&](const Export &e) { return e.ordinal == ordinal; });
    return it == exports.end() ? nullptr : &*it;
}

bool is_system_library(std::string_view library) { return !table(library).empty(); }

} // namespace xf
