// XXH3 64-bit, seed 0, default secret: the hash Xenia's patcher names a module
// by (UserModule::CalculateHash). Written from the XXH3 algorithm's description
// (xxHash, Yann Collet, BSD-2-Clause); checked against the reference library in
// the tests' reference values.

#pragma once

#include <cstddef>
#include <cstdint>

namespace xf {

uint64_t xxh3_64(const void *data, size_t size);

} // namespace xf
