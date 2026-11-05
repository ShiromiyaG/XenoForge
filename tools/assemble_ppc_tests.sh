#!/usr/bin/env bash
# Assembles Xenia's PPC instruction tests (src/xenia/cpu/ppc/testing, BSD)
# as Xenia's `xb gentests` does: <name>.bin (the code, linked at 0x80000000),
# <name>.map (the test labels, from nm) and <name>.dis (objdump's listing,
# for checking the disassembler), with the VMX128 binutils.
#
#   tools/assemble_ppc_tests.sh <binutils bin dir> <xenia testing dir> <out dir>
#
# e.g. tools/assemble_ppc_tests.sh ../rexglue-sdk/tools/binutils \
#        ../xenia-canary/src/xenia/cpu/ppc/testing ../external/ppc_tests
set -euo pipefail
binutils=$(cd "$1" && pwd)
src=$(cd "$2" && pwd)
mkdir -p "$3"
out=$(cd "$3" && pwd)
export CYGWIN=nodosfilewarning
# the folder holds Linux and Windows builds side by side
exe=""
[ -f "$binutils/powerpc-none-elf-as.exe" ] && exe=".exe"

one() {
    local s=$1 name
    name=$(basename "$s" .s)
    "$binutils/powerpc-none-elf-as$exe" -a32 -be -mregnames -ma2 -maltivec -mvsx -mvmx128 -R \
        -o "$out/$name.o" "$s"
    "$binutils/powerpc-none-elf-objdump$exe" --adjust-vma=0x100000 -Ma2 -Mvmx128 -D -EB "$out/$name.o" |
        tail -n +5 >"$out/$name.dis"
    "$binutils/powerpc-none-elf-ld$exe" -A powerpc:common32 -melf32ppc -EB -nostdlib --oformat=binary \
        -Ttext=0x80000000 -e0x80000000 -o "$out/$name.bin" "$out/$name.o"
    "$binutils/powerpc-none-elf-nm$exe" --numeric-sort "$out/$name.o" >"$out/$name.map"
    rm "$out/$name.o"
    cp "$s" "$out/$name.s"
}
export -f one
export binutils out exe
ls "$src"/instr_*.s "$src"/seq_*.s 2>/dev/null | xargs -P 8 -I{} bash -c 'one "$@"' _ {}
cp "$src/skip.txt" "$out/skip.txt"
echo "$(ls "$out"/*.bin | wc -l) tests in $out"
