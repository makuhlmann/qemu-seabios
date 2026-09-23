#!/usr/bin/env python3
# Rewrite gcc -m16 output for x86 interpreters that decode some forms wrongly.
#
# This file may be distributed under the terms of the GNU GPLv3 license.
#
# The option ROM interpreter of the HP zx1 (Itanium 2) system firmware decodes
# ModRM operands with 16-bit rules even behind an 0x67 prefix, pushes only
# two bytes for "pushl $imm8" (66 6A), and writes to the memory operand of
# "bt $imm8" (0F BA /4).  The Windows IA-64 HAL emulator (x86new)
# mis-decodes SIB operands without an index, which covers every ESP-based one.
#
#   x86compat.py rewrite IN.s OUT.s
#   x86compat.py lint OBJDUMP OBJ...

import re
import subprocess
import sys

R16 = {'%eax': '%ax', '%ecx': '%cx', '%edx': '%dx', '%ebx': '%bx',
       '%esp': '%sp', '%ebp': '%bp', '%esi': '%si', '%edi': '%di'}

# SeaBIOS writes segment overrides in upper case in inline asm (%CS:).
re_mem = re.compile(r'(?P<seg>%[cdefgsCDEFGS][sS]:)?(?P<disp>[^,()\s%]*)'
                    r'\((?P<base>%e\w\w)?(?:,(?P<idx>%e\w\w)(?:,(?P<sc>\d))?)?\)')
# The address size of a string instruction also selects its count register.
re_string = re.compile(r'\s*(rep\w*\s+)?(movs|stos|lods|cmps|scas|ins|outs)'
                       r'[bwl]?(?!\w)')
re_pushimm = re.compile(r'\s*pushl\s+\$(-?(?:0x[0-9a-fA-F]+|\d+))\s*$')
re_btimm = re.compile(r'\s*bt([wl])\s+(\$[^,]+),\s*(\S*\(.*\))\s*$')


def fail(line, why):
    sys.stderr.write('x86compat: cannot rewrite (%s): %s' % (why, line))
    sys.exit(1)


# In real mode every effective address is below 64 KiB, or a real CPU
# faults, and the low 16 bits of a sum depend only on the low 16 bits of its
# terms, so a 16-bit form computes the same address - provided the sum wraps
# at 64 KiB.  The zx1 interpreter does not wrap it.  So every address except
# a frame-pointer offset (%ebp is always a valid stack address) is computed
# into %bx, which the C code never uses (-ffixed-ebx), by 16-bit arithmetic
# and "leaw", whose 16-bit register write wraps; the access is then (%bx).
def lower(line, helper_ok=False):
    code = line.partition('#')[0]
    if re_string.match(code):
        return [line]
    ms = [m for m in re_mem.finditer(code) if m.group('base') or m.group('idx')]
    if not ms:
        return [line]
    if len(ms) > 1:
        fail(line, 'two memory operands')
    if not helper_ok and re.search(r'%e?bx\b|%b[lh]\b', code):
        fail(line, 'instruction uses bx')
    m = ms[0]
    seg, disp, base, idx, sc = m.group('seg', 'disp', 'base', 'idx', 'sc')
    if sc and sc != '1':
        fail(line, 'scaled index')
    if not base:
        base, idx = idx, None
    if base == '%esp' and code.split()[0].startswith('pop'):
        fail(line, 'pop to an esp-based address')
    if base == '%ebp' and idx is None:
        new = code[:m.start()] + (seg or '') + disp + '(%bp)' + code[m.end():]
        return [new.rstrip() + '\n']
    b = R16[base]
    i = R16[idx] if idx else None
    pre = []
    if i is None:
        if b in ('%si', '%di'):
            ea = '(%s)' % b
        else:
            pre, ea = ['movw %s, %%bx' % b], '(%bx)'
    elif {b, i} in ({'%bp', '%si'}, {'%bp', '%di'}):
        ea = '(%%bp,%s)' % ({'%si', '%di'} & {b, i}).pop()
    elif i in ('%si', '%di'):
        pre, ea = ['movw %s, %%bx' % b], '(%%bx,%s)' % i
    elif b in ('%si', '%di'):
        pre, ea = ['movw %s, %%bx' % i], '(%%bx,%s)' % b
    else:
        pre = ['movw %s, %%bx' % b, 'pushfw', 'addw %s, %%bx' % i, 'popfw']
        ea = '(%bx)'
    if disp or ea != '(%bx)':
        pre.append('leaw %s%s, %%bx' % (disp, ea))
    # 32-bit forms default to SS for an EBP or ESP base; (%bx) to DS.
    if not seg and base in ('%ebp', '%esp'):
        seg = '%ss:'
    new = code[:m.start()] + (seg or '') + '(%bx)' + code[m.end():]
    return ['\t%s\n' % p for p in pre] + [new.rstrip() + '\n']


def rewrite(infile, outfile):
    out = []
    for line in open(infile):
        s = line.strip()
        if not s or s[0] in '.#' or s.endswith(':'):
            out.append(line)
            continue
        code = line.partition('#')[0]
        m = re_pushimm.match(code)
        if m and -128 <= int(m.group(1), 0) <= 127:
            out.append('\t.byte 0x66, 0x68\n\t.long %s\n' % m.group(1))
            continue
        m = re_btimm.match(code)
        if m:
            # Test the bit in the scratch register: same CF, no store.
            sfx, bit, mem = m.groups()
            reg = '%ebx' if sfx == 'l' else '%bx'
            out.extend(lower('\tmov%s\t%s, %s\n' % (sfx, mem, reg),
                             helper_ok=True))
            out.append('\tbt%s\t%s, %s\n' % (sfx, bit, reg))
            continue
        if re.match(r'\s*bt[swrc]?[wl]?\s.*\(', code):
            fail(line, 'bit test on memory')
        out.extend(lower(line))
    open(outfile, 'w').writelines(out)


re_insn = re.compile(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)\s*(.*)$')
re_func = re.compile(r'^[0-9a-f]+ <(.*)>:')
re_addr32 = re.compile(r'\(%e\w\w|\(,%e\w\w')
re_spbased = re.compile(r'\(%e?sp[,)]')

# Violations of what the rewrite guarantees stop the build; the rest are
# environment problems that are known and not yet removed.
# x86new, the Windows IA-64 emulator, pushes 16 bits for a segment register
# whatever the operand size, and keys jecxz on 0x66 instead of 0x67.
ERRORS = ('32-bit address', '66 6A push', 'SP-based operand', '0F 1F nop',
          'bt imm8 on memory', '66 push/pop of a segment register', 'jecxz')


def lint(objdump, objs):
    found = {}
    for obj in objs:
        text = subprocess.run([objdump, '-d', '--insn-width=16', '-M', 'i8086',
                               obj],
                              capture_output=True, text=True,
                              check=True).stdout
        func = '?'
        for l in text.splitlines():
            m = re_func.match(l)
            if m:
                func = m.group(1)
                continue
            m = re_insn.match(l)
            if not m:
                continue
            byts, insn = m.group(1).split(), m.group(2)
            kind = None
            if re_addr32.search(insn) and not re_string.match(insn):
                kind = '32-bit address'
            elif byts[:2] == ['66', '6a']:
                kind = '66 6A push'
            elif re_spbased.search(insn):
                kind = 'SP-based operand'
            elif re.match(r'bt[wl]?\s+\$[^,]+,.*\(', insn):
                kind = 'bt imm8 on memory'
            elif byts[0] == '66' and byts[1] in ('06', '07', '0e', '16',
                                                  '17', '1e', '1f'):
                kind = '66 push/pop of a segment register'
            elif insn.startswith('jecxz'):
                kind = 'jecxz'
            elif '0f 1f' in ' '.join(byts[:4]):
                kind = '0F 1F nop'
            elif insn.startswith(('int ', 'hlt')):
                kind = insn.split(' ')[0] + (' ' + insn.split()[1]
                                              if insn.startswith('int ') else '')
            if kind:
                found.setdefault(kind, []).append('%s: %s' % (func, insn))
    errors = 0
    for kind, where in sorted(found.items()):
        level = 'error' if kind in ERRORS else 'warning'
        errors += level == 'error'
        sys.stderr.write('x86compat: %s: %s (%d), e.g. %s\n'
                         % (level, kind, len(where), where[0]))
    if errors:
        sys.exit(1)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'rewrite':
        rewrite(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 4 and sys.argv[1] == 'lint':
        lint(sys.argv[2], sys.argv[3:])
    else:
        sys.stderr.write(__doc__ or 'usage: see the file header\n')
        sys.exit(2)


if __name__ == '__main__':
    main()
