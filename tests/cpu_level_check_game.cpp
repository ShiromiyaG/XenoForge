// Stands for generated code in cpu_level_check_test: compiled at
// XF_GAME_CPU_LEVEL, with a static initializer that must run only after the
// CPU level check has passed.

#include <cstdio>

namespace {
int initialize() {
    std::printf("the game's initializer ran\n");
    return 42;
}
const int value = initialize();
} // namespace

int cpu_level_check_game_value() { return value; }
