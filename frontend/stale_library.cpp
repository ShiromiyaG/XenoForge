#include "stale_library.h"

namespace xf::frontend {

// The stub: connect to game_build.h's "new entries since built" when it exists.
bool library_has_new_entries(const std::filesystem::path &, std::string_view) { return false; }

} // namespace xf::frontend
