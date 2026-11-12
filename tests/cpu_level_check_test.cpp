// The CPU level check (xenoforge/cpu_level_check.h) in an executable built as a
// port's is: cpu_level_check_game.cpp stands for the generated code, compiled
// at XF_GAME_CPU_LEVEL, with a static initializer of its own. On this CPU the
// program runs (the initializer, then main); the ctest entry that pretends a
// CPU without the level (XF_HOST_CPU_LEVEL=none) expects the check's message
// and neither of those lines.

#include <cstdio>

int cpu_level_check_game_value();

int main() {
    std::printf("main ran (%d)\n", cpu_level_check_game_value());
    return 0;
}
