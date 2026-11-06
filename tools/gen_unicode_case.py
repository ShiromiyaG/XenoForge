#!/usr/bin/env python3
"""Writes src/kernel/unicode_case.inc: the console's case table for RtlUpcaseUnicodeChar and
RtlDowncaseUnicodeChar, from the Unicode standard's UnicodeData.txt.

    python tools/gen_unicode_case.py <UnicodeData-2.1.2.txt> [src/kernel/unicode_case.inc]

The data file is the Unicode Consortium's (version 2.1.2, 1998; the file is kept in refs/docs/kernel/unicode).
The simple case mappings of the Basic Multilingual Plane are taken from its fields 12 (uppercase) and 13
(lowercase). The kernel of system software 2.0.16202 was run over all 65536 characters (tools/xfkrnl's sweep
in XenoForge-private, the oracle) and its table is that of Unicode 2.1.2 with these changes, which the oracle
showed:

- a mapping from or to a titlecase letter (general category Lt: U+01C5, U+01C8, U+01CB, U+01F2, and the capital
  Greek letters with a prosgegrammeni, U+1F88..U+1F8F, U+1F98..U+1F9F, U+1FA8..U+1FAF, U+1FBC, U+1FCC, U+1FFC,
  which this edition lists as Lu) does not exist, in either direction (so U+1F80 stays as it is, and U+01C5
  maps neither way);
- the uppercase of letters that are only variants of another letter is the letter itself: dotless i (U+0131),
  long s (U+017F), the Greek symbol forms U+03D0, U+03D1, U+03D5, U+03D6, U+03F0, U+03F1, and U+1E9B;
  U+0130 (I with a dot) does not become a small i;
- Unicode 2.1.2 gives U+0275 a lowercase mapping to U+019F, where it is U+019F that has the lowercase mapping
  to U+0275 and U+0275 that has the uppercase one (a mistake of that edition, corrected in 2.1.9): the
  mapping is the pair's.

What the oracle's sweep gave (the whole table: 636 uppercase and 673 lowercase mappings) is the result.
"""

import sys

UPPER_NOT_MAPPED = {0x0131, 0x017F, 0x03D0, 0x03D1, 0x03D5, 0x03D6, 0x03F0, 0x03F1, 0x1E9B}
LOWER_NOT_MAPPED = {0x0130}


def tables(path):
    upper, lower, titlecase = {}, {}, set()
    with open(path, encoding='latin-1') as f:
        for line in f:
            fields = line.rstrip('\n').split(';')
            if len(fields) < 14:
                continue
            cp = int(fields[0], 16)
            if cp > 0xFFFF:
                continue
            # the titlecase letters: Lt, and the capital Greek letters with a prosgegrammeni, which this edition
            # lists as Lu and later ones (3.0) as Lt
            if fields[2] == 'Lt' or (fields[2] == 'Lu' and 'PROSGEGRAMMENI' in fields[1]):
                titlecase.add(cp)
            if fields[12]:
                upper[cp] = int(fields[12], 16)
            if fields[13]:
                lower[cp] = int(fields[13], 16)
    return upper, lower, titlecase


def console_tables(upper, lower, titlecase):
    # the 2.1.2 mistake: U+0275's mapping is the uppercase one
    lower.pop(0x0275, None)
    upper[0x0275] = 0x019F
    up = {c: m for c, m in upper.items() if c not in titlecase and m not in titlecase and c not in UPPER_NOT_MAPPED}
    down = {c: m for c, m in lower.items() if c not in titlecase and m not in titlecase and c not in LOWER_NOT_MAPPED}
    return up, down


def runs(table):
    """(first, last, stride, delta): c + delta for c in first..last step stride; stride 1 or 2."""
    out = []
    keys = sorted(table)
    i = 0
    while i < len(keys):
        first = keys[i]
        delta = table[first] - first
        best = (first, 1, 1)  # last, stride, count
        for stride in (1, 2):
            n = 1
            while (i + n < len(keys) and keys[i + n] == first + stride * n
                   and table[keys[i + n]] - keys[i + n] == delta):
                n += 1
            if n > best[2]:
                best = (first + stride * (n - 1), stride, n)
        last, stride, n = best
        out.append((first, last, stride if n > 1 else 1, delta))
        i += n
    return out


def emit(name, table, f):
    f.write('constexpr CaseRun %s[] = {\n' % name)
    for first, last, stride, delta in runs(table):
        f.write('    {0x%04X, 0x%04X, %d, %d},\n' % (first, last, stride, delta))
    f.write('};\n')


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out_path = sys.argv[2] if len(sys.argv) > 2 else 'src/kernel/unicode_case.inc'
    upper, lower, titlecase = tables(sys.argv[1])
    up, down = console_tables(upper, lower, titlecase)
    with open(out_path, 'w', newline='\n') as f:
        f.write("// Written by tools/gen_unicode_case.py from UnicodeData-2.1.2.txt (the Unicode Consortium's).\n"
                "// Do not edit. Runs of simple case mappings: c + delta for c in first..last step stride.\n"
                "// clang-format off\n")
        emit('upcase_runs', up, f)
        emit('downcase_runs', down, f)
    print('%d uppercase and %d lowercase mappings' % (len(up), len(down)))


main()
