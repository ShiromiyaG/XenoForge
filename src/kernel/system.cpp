// The kernel's interfaces to the system's own modules (docs/runtime.md, "XAM's
// oracle"): exports only the console's XAM calls, which no game imports and
// Xenia does not implement. Each answers as a console with no hardware event
// pending would; what is known of each comes from how XAM uses it (its
// arguments and how it reads the answer), never from its code.

#include "objects.h"
#include "xenoforge/kernel.h"

#include <cstdint>

namespace xf::kernel {
namespace {

constexpr uint32_t X_STATUS_NO_MORE_ENTRIES = 0x8000001A;

// (notification, ?): the next of the content storage's notifications (a
// device's storage arriving or leaving), copied out; XAM takes them until
// the answer is an error. None are queued here: no storage device comes or
// goes.
XF_EXPORT(xboxkrnl, DrvGetContentStorageNotification) { result(c, X_STATUS_NO_MORE_ENTRIES); }

} // namespace
} // namespace xf::kernel
