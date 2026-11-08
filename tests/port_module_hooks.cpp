// port_debug_test's hook module (xenoforge/port_module.h): a port's hooks
// built as a DLL (a .so on POSIX) the test loads and reloads. Its own
// statics start over with each load; its persistent state does not.

#include "port_program.h"

#include "xenoforge/port.h"

using namespace xf::port;

namespace {

struct State {
    uint32_t calls;
};
uint32_t own_calls = 0; // this load's

} // namespace

// r30 says this load's count, r31 the count across loads
XF_BEFORE(port_program::ADD, module_before) {
    own_calls++;
    State &s = persistent<State>("port_debug_test");
    s.calls++;
    set_r(c, 30, own_calls);
    set_r(c, 31, s.calls);
}

XF_REPLACE(port_program::PLAIN, module_replace) {
    original(c);
    c.r[3] += 1000;
}
