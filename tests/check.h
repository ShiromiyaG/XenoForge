// The tests' one assertion: report and count, keep going.

#pragma once

#include <cstdio>

inline int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                     \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

inline int finish(const char *name) {
    if (failures) std::printf("%s: %d failed\n", name, failures);
    else std::printf("%s: ok\n", name);
    return failures ? 1 : 0;
}
