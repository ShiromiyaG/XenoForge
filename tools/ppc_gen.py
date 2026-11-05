#!/usr/bin/env python3
"""Writes the PPC tables from src/ppc/ppc-instructions.xml (Xenia's, BSD).

Translated from Xenia's tools/ppc-table-gen.py (BSD; see LICENSE-xenia,
itself after shuffle2's generator): the same decoder (a switch on the primary
opcode, then one on each form's extended opcode bits, in the same order) and
the same disassembly, so both match Xenia's byte for byte (docs/recompiler.md).

    python tools/ppc_gen.py

writes include/xenoforge/ppc_opcodes.inc (the list), src/ppc/decode.inc (the
decoder's body) and src/ppc/disasm.inc (a function per instruction that has a
disassembly pattern).
"""

import os
from xml.etree.ElementTree import ElementTree

HERE = os.path.dirname(os.path.abspath(__file__))
PPC = os.path.join(HERE, '..', 'src', 'ppc')
CUSTOM_DISASM = ['bcx']
HEADER = '// Written by tools/ppc_gen.py from ppc-instructions.xml (Xenia\'s, BSD). Do not edit.\n// clang-format off\n'


def bit_extract(x, leftmost, rightmost):
    return (x >> (32 - 1 - rightmost)) & ((1 << (rightmost - leftmost + 1)) - 1)


EXTENDED_OPCODE_BITS = {
    'X': [(21, 30)],
    'XL': [(21, 30)],
    'XFX': [(21, 30)],
    'XFL': [(21, 30)],
    'VX': [(21, 31)],
    'VX128': [(22, 25), (27, 27)],
    'VX128_1': [(21, 27), (30, 31)],
    'VX128_2': [(22, 22), (27, 27)],
    'VX128_3': [(21, 27)],
    'VX128_4': [(21, 23), (26, 27)],
    'VX128_5': [(27, 27)],
    'VX128_R': [(22, 24), (27, 27)],
    'VX128_P': [(21, 22), (26, 27)],
    'VC': [(22, 31)],
    'VA': [(26, 31)],
    'XO': [(22, 30)],
    'XW': [(25, 30)],
    'A': [(26, 30)],
    'DS': [(30, 31)],
    'MD': [(27, 29)],
    'MDS': [(27, 30)],
    'XS': [(21, 29)],
    'DCBZ': [(6, 10), (21, 30)],  # like X
}


def opcode_extended(insn, form):
    if form not in EXTENDED_OPCODE_BITS:
        return -1
    parts = EXTENDED_OPCODE_BITS[form]
    shift = max(part[1] for part in parts)
    value = 0
    for part in parts:
        value |= bit_extract(insn, part[0], part[1]) << (shift - part[1])
    return value


class Insn:
    pass


def parse(filename):
    insns = []
    for e in ElementTree(file=filename).findall('.//insn'):
        i = Insn()
        i.opcode = int(e.attrib['opcode'], 16)
        i.mnem = e.attrib['mnem'].replace('.', 'x')
        i.form = e.attrib['form']
        i.group = e.attrib['group']
        i.desc = e.attrib['desc']
        i.op_primary = bit_extract(i.opcode, 0, 5)
        i.op_extended = opcode_extended(i.opcode, i.form)
        i.disasm = None
        for d in e.findall('.//disasm'):
            i.disasm = d.text
        insns.append(i)
    return sorted(insns, key=lambda i: i.mnem)


def write_list(insns):
    lines = [HEADER, '// XF_PPC_OP(name, opcode, form, group, description), sorted by name, as Xenia\'s PPCOpcode']
    for i in insns:
        lines.append('XF_PPC_OP(%s, 0x%08x, %s, %s, "%s")' % (i.mnem, i.opcode, i.form, i.group, i.desc))
    return '\n'.join(lines) + '\n'


def write_decode(insns):
    lines = [HEADER, '// The body of decode(code): HIT(name) returns that opcode, MISS returns Invalid.']
    lines.append('switch (bits(code, 0, 5)) {')
    subtables = {}
    for i in sorted(insns, key=lambda i: i.op_primary):
        subtables.setdefault(i.op_primary, []).append(i)
    for pri in sorted(subtables):
        if len(subtables[pri]) == 1:
            lines.append('case %i: HIT(%s);' % (pri, subtables[pri][0].mnem))
            continue
        groups = {}
        for i in subtables[pri]:
            parts = EXTENDED_OPCODE_BITS[i.form]
            shift = max(part[1] for part in parts)
            expression = '|'.join('(bits(code, %s, %s) << %s)' % (p[0], p[1], shift - p[1]) for p in parts)
            groups.setdefault(expression, (i.form, []))[1].append(i)
        lines.append('case %i:' % pri)
        for expression in sorted(groups):
            form, members = groups[expression]
            parts = EXTENDED_OPCODE_BITS[form]
            count = max(p[1] for p in parts) - min(p[0] for p in parts) + 1
            lines.append('    switch (%s) {' % expression)
            for i in sorted(members, key=lambda i: i.op_extended):
                lines.append('    case 0b%s: HIT(%s);' % (('{:0' + str(count) + 'b}').format(i.op_extended), i.mnem))
            lines.append('    }')
        lines.append('    MISS;')
    lines.append('default: MISS;')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def token_code(form, token):
    f = 'd.%s.%s()' % (form, token)
    if token == 'Rc': return 'if (d.%s.Rc()) out += \'.\';' % form
    if token == 'OE': return 'if (d.%s.OE()) out += \'o\';' % form
    if token == 'LK': return 'if (d.%s.LK()) out += \'l\';' % form
    if token == 'AA': return 'if (d.%s.AA()) out += \'a\';' % form
    if token in ['RA', 'RA0', 'RB', 'RC', 'RT', 'RS', 'RD']: return 'out += "r" + std::to_string(%s);' % f
    if token in ['FA', 'FB', 'FC', 'FT', 'FS', 'FD']: return 'out += "fr" + std::to_string(%s);' % f
    if token in ['VA', 'VB', 'VC', 'VT', 'VS', 'VD']: return 'out += "vr" + std::to_string(%s);' % f
    if token in ['CRFD', 'CRFS']: return 'out += "crf" + std::to_string(%s);' % f
    if token in ['CRBA', 'CRBB', 'CRBD']: return 'out += "crb" + std::to_string(%s);' % f
    if token in ['BO', 'BI', 'TO', 'SPR', 'TBR', 'L', 'FM', 'MB', 'ME', 'SH', 'IMM', 'z']:
        return 'dec(out, %s);' % f
    if token in ['UIMM', 'ADDR']: return 'hex(out, %s);' % f
    if token in ['d', 'ds', 'SIMM']: return 'signed_hex(out, %s);' % f
    if token == 'LEV': return 'if (%s) dec(out, %s);' % (f, f)
    return 'out += "(UNHANDLED %s)";' % token


def write_disasm(insns):
    lines = [HEADER]
    for i in insns:
        if not i.disasm or i.mnem in CUSTOM_DISASM:
            continue
        lines.append('void disasm_%s([[maybe_unused]] const Data &d, std::string &out) {' % i.mnem)
        lines.append('    // ' + i.disasm)
        lines.append('    size_t start = out.size();')
        current, first_space, j, s = '', False, 0, i.disasm
        while j < len(s):
            c = s[j]
            if c == '[':
                if current:
                    lines.append('    out += "%s";' % current)
                    current = ''
                token = s[j + 1:s.index(']', j)]
                j += len(token) + 1
                lines.append('    ' + token_code(i.form, token))
            elif c == ' ' and not first_space:
                if current:
                    lines.append('    out += "%s";' % current)
                    current = ''
                lines.append('    pad(out, start);')
                first_space = True
            else:
                current += c
            j += 1
        if current:
            lines.append('    out += "%s";' % current)
        if not first_space:
            lines.append('    pad(out, start);')
        lines.append('}')
    lines.append('')
    lines.append('// by opcode, null where the name alone is printed')
    lines.append('constexpr DisasmFn DISASM[] = {')
    for i in insns:
        # the custom ones are written by hand in ppc.cpp under the same name
        lines.append('    %s,' % ('disasm_' + i.mnem if i.disasm else 'nullptr'))
    lines.append('};')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    insns = parse(os.path.join(PPC, 'ppc-instructions.xml'))
    include = os.path.join(HERE, '..', 'include', 'xenoforge')
    for path, text in [(os.path.join(include, 'ppc_opcodes.inc'), write_list(insns)),
                       (os.path.join(PPC, 'decode.inc'), write_decode(insns)),
                       (os.path.join(PPC, 'disasm.inc'), write_disasm(insns))]:
        with open(path, 'w', newline='\n') as f:
            f.write(text)
    print('%d instructions' % len(insns))
