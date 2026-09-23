#!/usr/bin/env python3
# Differential test ROM for the instruction forms of a SeaVGABIOS build.
#
# This file may be distributed under the terms of the GNU GPLv3 license.
#
# Every distinct instruction form (prefixes, opcode, ModRM mod/rm, group reg
# field) found in the given objects runs once, from a fixed register, flag
# and memory state; the resulting state goes out through port 0x3DA.  Run the
# ROM on an x86 CPU and under the interpreter under test (QEMU:
# -trace vga_std_write_io), then compare the two traces with "diff".
#
#   x86compat_testrom.py gen OBJDUMP OUTDIR OBJ... [--control OBJ...]
#                            [--skip ID,...]
#   x86compat_testrom.py diff MANIFEST REFERENCE.log TEST.log

import json
import os
import re
import subprocess
import sys

SCR = 0x5000        # test segment: the 4 KiB window, stack and planted values
SCR2 = 0x5100       # what the segment-load tests load
SP0 = 0x0E00
WIN = 0x1000
CSBUF_SIZE = 0x100
FLAGS0 = 0x0003     # CF; IF, TF and DF clear
MARK_OFF = 0x0010
# The base registers are not a multiple of 0x100 apart, and the window
# pattern does not repeat every 0x100 bytes: a read through the wrong base
# register returns other data.
REG0 = {'eax': 0x9A5C01A1, 'ecx': 0x00000003, 'edx': 0x00000002,
        'ebx': 0x12340230, 'ebp': 0xDEF0056C, 'esi': 0x56780344,
        'edi': 0x9ABC0458}
REGNUM = {'eax': 0, 'ecx': 1, 'edx': 2, 'ebx': 3, 'esp': 4, 'ebp': 5,
          'esi': 6, 'edi': 7}
R16 = ['ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di']
RM16 = {0: ('bx', 'si'), 1: ('bx', 'di'), 2: ('bp', 'si'), 3: ('bp', 'di'),
        4: ('si',), 5: ('di',), 6: ('bp',), 7: ('bx',)}
PREFIXES = {0x66, 0x67, 0xF2, 0xF3, 0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65}
SEGPFX = {0x26: 'es', 0x2E: 'cs', 0x36: 'ss', 0x3E: 'ds', 0x64: 'fs',
          0x65: 'gs'}
MODRM1 = ({b + o for b in range(0, 0x40, 8) for o in range(4)}
          | set(range(0x80, 0x90)) | set(range(0xD8, 0xE0))
          | {0x62, 0x63, 0x69, 0x6B, 0xC0, 0xC1, 0xC4, 0xC5, 0xC6, 0xC7,
             0xD0, 0xD1, 0xD2, 0xD3, 0xF6, 0xF7, 0xFE, 0xFF})
# Opcodes whose ModRM reg field selects the operation or a segment register.
GROUPS1 = {0x80, 0x81, 0x83, 0x8C, 0x8E, 0x8F, 0xC0, 0xC1, 0xC6, 0xC7, 0xD0,
           0xD1, 0xD2, 0xD3, 0xF6, 0xF7, 0xFE, 0xFF}
NOMODRM2 = set(range(0x80, 0x90)) | {0xA0, 0xA1, 0xA2, 0xA8, 0xA9} \
    | set(range(0xC8, 0xD0))
SKIP = re.compile(r'(j\w*|call\w*|ret\w*|lret\w*|iret\w*|loop\w*|int\w*|hlt|'
                  r'in[sbwl]?|ins[bwld]|out[sbwl]?|outs[bwld]|lcall\w*|'
                  r'ljmp\w*|sti|bound|lss|lfs|lgs|enter\w*)$')
FLAGS_ALL = 0x0CD5  # CF PF AF ZF SF DF OF


def decode(b):
    """Split an instruction into prefixes, opcode, and the ModRM fields."""
    i = 0
    while i < len(b) and b[i] in PREFIXES:
        i += 1
    pre = b[:i]
    if b[i] == 0x0F:
        op = b[i:i + 2]
        hasm = op[1] not in NOMODRM2
    else:
        op = b[i:i + 1]
        hasm = op[0] in MODRM1
    j = i + len(op)
    d = {'pre': pre, 'op': op, 'modrm_at': j if hasm else None}
    if hasm:
        mr = b[j]
        d['mod'], d['reg'], d['rm'] = mr >> 6, (mr >> 3) & 7, mr & 7
        if d['mod'] == 0 and d['rm'] == 6:
            d['disp'] = (j + 1, 2)
        elif d['mod'] == 1:
            d['disp'] = (j + 1, 1)
        elif d['mod'] == 2:
            d['disp'] = (j + 1, 2)
    elif len(op) == 1 and 0xA0 <= op[0] <= 0xA3:
        d['disp'] = (j, 2)          # moffs16
    return d


def formkey(d):
    k = [tuple(sorted(d['pre'])), tuple(d['op'])]
    if d['modrm_at'] is not None:
        grp = d['op'][0] in GROUPS1 if len(d['op']) == 1 else True
        k += [d['mod'], d['rm'] if d['mod'] != 3 else 'r',
              d['reg'] if grp else '-']
    return repr(k)


def inventory(objdump, objs):
    re_ins = re.compile(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)\s*(\S*)\s*(.*)$')
    re_fn = re.compile(r'^[0-9a-f]+ <(.*)>:')
    out = []
    for obj in objs:
        text = subprocess.run([objdump, '-d', '--insn-width=16', '-M', 'i8086',
                               obj],
                              capture_output=True, text=True,
                              check=True).stdout
        fn = '?'
        for l in text.splitlines():
            m = re_fn.match(l)
            if m:
                fn = m.group(1)
                continue
            m = re_ins.match(l)
            if m and m.group(2):
                out.append({'bytes': [int(x, 16) for x in m.group(1).split()],
                            'mnem': m.group(2), 'ops': m.group(3).strip(),
                            'src': '%s:%s' % (os.path.basename(obj), fn)})
    return out


FLAGOPS = ('and', 'or', 'xor', 'test', 'shl', 'sal', 'shr', 'sar', 'shld',
           'shrd', 'rol', 'ror', 'rcl', 'rcr', 'mul', 'imul', 'div', 'idiv',
           'bsf', 'bsr', 'bt', 'bts', 'btr', 'btc')


def flagmask(mnem):
    m = mnem if mnem in FLAGOPS else mnem[:-1]
    mask = FLAGS_ALL
    if m in ('and', 'or', 'xor', 'test'):
        mask &= ~0x0010
    elif m in ('shl', 'sal', 'shr', 'sar', 'shld', 'shrd'):
        mask &= ~0x0810
    elif m in ('rol', 'ror', 'rcl', 'rcr'):
        mask &= ~0x0800
    elif m in ('mul', 'imul'):
        mask &= ~0x00D4
    elif m in ('div', 'idiv'):
        mask &= ~0x08D5
    elif m in ('bsf', 'bsr'):
        mask &= ~0x0895
    elif m in ('bt', 'bts', 'btr', 'btc'):
        mask &= ~0x0894
    return mask


class Test:
    def __init__(self, tid, kind, src, mnem, ops, code, orig=None):
        self.id, self.kind, self.src = tid, kind, src
        self.mnem, self.ops = mnem, ops
        self.code, self.orig = code, orig
        self.over = {}      # register -> 32-bit value after setregs
        self.plants = []    # (segment override, base registers, offset, word)
        self.follow = []    # bytes after the instruction
        self.mask = flagmask(mnem)


def ea_of(d, code, regs):
    base = RM16[d['rm']] if not (d['mod'] == 0 and d['rm'] == 6) else ()
    disp = 0
    if 'disp' in d:
        at, n = d['disp']
        disp = code[at] | (code[at + 1] << 8 if n == 2 else 0)
        if n == 1 and disp & 0x80:
            disp -= 0x100
    return (sum(regs[r] & 0xFFFF for r in base) + disp) & 0xFFFF, base


def build_test(tid, ins, csbuf):
    b = list(ins['bytes'])
    d = decode(b)
    t = Test(tid, 'generic', ins['src'], ins['mnem'], ins['ops'], b,
             list(ins['bytes']))
    regs16 = {R16[REGNUM[k]]: v & 0xFFFF for k, v in REG0.items()}
    seg = next((SEGPFX[p] for p in d['pre'] if p in SEGPFX), None)
    op0 = d['op'][0] if len(d['op']) == 1 else None
    mem = 'disp' in d or (d['modrm_at'] is not None and d['mod'] != 3)
    if mem and 'disp' in d:
        at, n = d['disp']
        old = b[at] | (b[at + 1] << 8 if n == 2 else 0)
        absolute = d['modrm_at'] is None or (d['mod'] == 0 and d['rm'] == 6)
        if absolute:
            new = csbuf + (old & 0x7F) if seg == 'cs' else 0x0C00 + (old & 0xFF)
        elif n == 1:
            new = ((old & 0x3F) - 0x20) & 0xFF
        else:
            new = 0x0100 + (old & 0x7F)
        b[at] = new & 0xFF
        if n == 2:
            b[at + 1] = new >> 8
    if mem and seg == 'cs' and d['modrm_at'] is not None \
            and not (d['mod'] == 0 and d['rm'] == 6):
        # CS-relative through registers: aim the base at the scratch buffer.
        ea, base = ea_of(d, b, regs16)
        disp = (ea - sum(regs16[r] for r in base)) & 0xFFFF
        if len(base) == 2:
            t.over[base[1]] = 0x10
            regs16[base[1]] = 0x10
        t.over[base[0]] = (csbuf + 0x40 - disp - (0x10 if len(base) == 2
                                                   else 0)) & 0xFFFF
        regs16[base[0]] = t.over[base[0]]
    t.code = b
    # Divisions: operands that cannot overflow.
    if op0 in (0xF6, 0xF7) and d['reg'] in (6, 7):
        if op0 == 0xF6:
            t.over['ax'] = 0x0011
        else:
            t.over['edx32'] = 0
            t.over['eax32'] = 0x00001234
    # Segment loads: load SCR2, then store a marker through the segment.
    sreg = None
    if op0 == 0x8E:
        sreg = {0: 'es', 3: 'ds'}.get(d['reg'])
        if sreg is None:
            return None
        if d['mod'] == 3:
            t.over[R16[d['rm']]] = SCR2
        else:
            ea, base = ea_of(d, b, regs16)
            t.plants.append((seg, base, ea, SCR2))
    elif op0 in (0x07, 0x1F):
        sreg = 'es' if op0 == 0x07 else 'ds'
        t.plants.append(('ss', (), SP0, SCR2))
    elif op0 in (0xC4, 0xC5):
        sreg = 'es' if op0 == 0xC4 else 'ds'
        ea, base = ea_of(d, b, regs16)
        t.plants.append((seg, base, ea, 0x0020))
        t.plants.append((seg, base, (ea + 2) & 0xFFFF, SCR2))
    if sreg:
        t.kind = 'segload'
        t.follow = [0x26 if sreg == 'es' else 0x3E, 0xC6, 0x06,
                    MARK_OFF & 0xFF, MARK_OFF >> 8, 0x5A]
    if op0 == 0x9D:                 # popf / popfl: no TF from the pattern
        t.kind = 'popf'
        t.plants.append(('ss', (), SP0, 0x0046))
        t.plants.append(('ss', (), SP0 + 2, 0x0000))
    return t


R32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']


def decode32(b):
    """Displacement and registers of a ModRM operand behind an 0x67 prefix."""
    d = decode(b)
    j = d['modrm_at']
    if j is None:
        return None
    mod, rm = d['mod'], d['rm']
    if mod == 3:
        return None
    k = j + 1
    base, idx, scale = rm, None, 1
    if rm == 4:
        sib = b[k]
        k += 1
        scale, idx, base = 1 << (sib >> 6), (sib >> 3) & 7, sib & 7
        if idx == 4:
            idx = None
    if mod == 0 and base == 5:
        return {'disp': (k, 4), 'base': None, 'idx': idx, 'scale': scale}
    if mod == 1:
        return {'disp': (k, 1), 'base': base, 'idx': idx, 'scale': scale}
    if mod == 2:
        return {'disp': (k, 4), 'base': base, 'idx': idx, 'scale': scale}
    return {'disp': None, 'base': base, 'idx': idx, 'scale': scale}


def controls(objdump, objs, tid):
    """Forms the rewrite removes, from an unrewritten build: every distinct
    0x67 ModRM form and 66 6A.  They show which of them the interpreter
    under test gets wrong."""
    out, seen = [], set()
    for ins in inventory(objdump, objs):
        b = ins['bytes']
        d = decode(b)
        if SKIP.match(ins['mnem']) or re.search(r',%(ss|fs|gs)$', ins['ops']):
            continue
        is67 = 0x67 in d['pre'] and d['modrm_at'] is not None
        if not (is67 or b[:2] == [0x66, 0x6A]):
            continue
        a = decode32(b) if is67 else None
        if is67 and a is None:
            continue
        key = formkey(d) + repr(a and (a['disp'] and a['disp'][1],
                                       a['base'], a['idx'], a['scale']))
        if key in seen:
            continue
        seen.add(key)
        c = list(b)
        t = Test(tid, 'control', ins['src'], ins['mnem'], ins['ops'], c,
                 list(b))
        if is67:
            # Upper register halves 0: a real CPU faults on offsets above
            # 64 KiB.  Addresses stay inside the window.
            regs = {r: REG0.get(r, SP0) & 0xFFFF for r in R32}
            for r in R32:
                if r != 'esp':
                    t.over[r + '32'] = regs[r]
            if a['idx'] is not None:
                regs[R32[a['idx']]] = 0x10
                t.over[R32[a['idx']] + '32'] = 0x10
            if a['disp']:
                at, n = a['disp']
                old = int.from_bytes(bytes(c[at:at + n]), 'little')
                if a['base'] is None and a['idx'] is None:
                    new = 0x0C00 + (old & 0xFF)
                elif n == 1:
                    new = ((old & 0x3F) - 0x20) & 0xFF
                else:
                    new = 0x0100 + (old & 0x7F)
                c[at:at + n] = list(new.to_bytes(n, 'little'))
            if 0x2E in d['pre'] or (d['op'] and d['op'][0] in (0x8E, 0xC4,
                                                                0xC5)):
                continue            # CS stores and segment loads: not here
        t.code = c
        # 16-bit decoding keeps the length only for mod 0 with rm 0-3 or 7,
        # and mod 1 without SIB; a longer 32-bit form desynchronises the
        # interpreter, so those run last.
        t.samelen = not is67 or (d['mod'] == 0 and d['rm'] in (0, 1, 2, 3, 7)) \
            or (d['mod'] == 1 and d['rm'] != 4)
        out.append(t)
    out.sort(key=lambda t: not t.samelen)
    for t in out:
        t.id = tid
        tid += 1
    return out


# Control transfers, written out: each leaves a path value in %ax.
TEMPLATES = [
    ('jc short taken', [0xF9, 0x72, 0x05, 0xB8, 0x01, 0x00, 0xEB, 0x03,
                        0xB8, 0x02, 0x00]),
    ('jc short not taken', [0xF8, 0x72, 0x05, 0xB8, 0x01, 0x00, 0xEB, 0x03,
                            0xB8, 0x02, 0x00]),
    ('jc near taken', [0xF9, 0x0F, 0x82, 0x05, 0x00, 0xB8, 0x01, 0x00, 0xEB,
                       0x03, 0xB8, 0x02, 0x00]),
    ('jmp near', [0xE9, 0x03, 0x00, 0xB8, 0x01, 0x00, 0xB8, 0x02, 0x00]),
    ('call near + ret', [0xE8, 0x05, 0x00, 0xB8, 0x01, 0x00, 0xEB, 0x04,
                         0xB8, 0x02, 0x00, 0xC3]),
    ('pushw + call near + retw $2', [0x50, 0xE8, 0x05, 0x00, 0xB8, 0x01, 0x00,
                                     0xEB, 0x06, 0xB8, 0x02, 0x00, 0xC2,
                                     0x02, 0x00]),
    ('call *%si + ret', 'call_reg'),
    ('push cs, push ip, lret', 'lret'),
    # 16-bit effective addresses wrap at 64 KiB (x86compat.py relies on it).
    ('wrap: store -0x10(%bp), bp=8', 'wrap_bp_w'),
    ('wrap: store (%bx,%si), sum 0x10010', 'wrap_bxsi_w'),
    ('wrap: store 0x12(%bx), bx=0xfffe', 'wrap_bx_w'),
    ('wrap: load 0x120(%si), si=0xff00', 'wrap_si_r'),
    ('wrap: load (%bp,%si), sum 0x10020', 'wrap_bpsi_r'),
    ('wrap: leaw 0x20(%si), si=0xfff0', 'wrap_lea_si'),
    ('wrap: leaw 0x10(%bp,%si), sum 0x10020', 'wrap_lea_bpsi'),
    ('wrap: leaw 0x20(%bx), bx=0xfff0', 'wrap_lea_bx'),
]


def emit(tests):
    a = []
    for t in tests:
        a.append('t_%04x:' % t.id)
        a.append('\tmovw $0x%04x, %%ax' % t.id)
        a.append('\tcall x_pre')
        a.append('\tmovw %ss, %ax')
        a.append('\tmovw %ax, %cs:h_ss')
        a.append('\tmovw %sp, %cs:h_sp')
        a.append('\tmovw $0x%04x, %%ax' % SCR)
        a.append('\tmovw %ax, %ds')
        a.append('\tmovw %ax, %es')
        a.append('\tmovw %ax, %ss')
        a.append('\t.byte 0x66, 0xbc\n\t.long 0x%04x' % SP0)
        a.append('\tcall x_setregs')
        for r, v in t.over.items():
            if r.endswith('32'):
                a.append('\t.byte 0x66, 0x%02x\n\t.long 0x%08x'
                         % (0xB8 + REGNUM[r[:-2]], v))
            else:
                a.append('\tmovw $0x%04x, %%%s' % (v, r))
        for seg, base, off, val in t.plants:
            # ds, es and ss all hold the test segment here.
            p = '%cs:' if seg == 'cs' else '%ds:'
            a.append('\tmovw $0x%04x, %s0x%04x' % (val, p, off))
        a.append('t_%04x_i:' % t.id)
        if isinstance(t.code, str):
            a.append(TEMPLATE_ASM[t.code] % {'id': t.id})
        else:
            a.append('\t.byte ' + ', '.join('0x%02x' % x for x in t.code))
        if t.follow:
            a.append('\t.byte ' + ', '.join('0x%02x' % x for x in t.follow))
        a.append('t_%04x_e:' % t.id)
        a.append('\tcall x_capture')
        a.append('\tcall x_post')
    return '\n'.join(a)


TEMPLATE_ASM = {
    'wrap_lea_si': '\tmovw $0xfff0, %%si\n\tleaw 0x20(%%si), %%bx',
    'wrap_lea_bpsi': '\tmovw $0xfff0, %%bp\n\tmovw $0x0020, %%si\n'
                     '\tleaw 0x10(%%bp,%%si), %%bx',
    'wrap_lea_bx': '\tmovw $0xfff0, %%bx\n\tleaw 0x20(%%bx), %%bx',
    'wrap_bp_w': '\tmovb $0, %%ss:0xfff8\n\tmovw $0x0008, %%bp\n'
                 '\tmovb $0x5a, -0x10(%%bp)\n\tmovb %%ss:0xfff8, %%al',
    'wrap_bxsi_w': '\tmovw $0xfff0, %%bx\n\tmovw $0x0020, %%si\n'
                   '\tmovb $0x5a, (%%bx,%%si)',
    'wrap_bx_w': '\tmovw $0xfffe, %%bx\n\tmovb $0x5a, 0x12(%%bx)',
    'wrap_si_r': '\tmovw $0xff00, %%si\n\t.byte 0x8a, 0x84, 0x20, 0x01',
    'wrap_bpsi_r': '\tmovw $0xfff0, %%bp\n\tmovw $0x0030, %%si\n'
                   '\tmovb (%%bp,%%si), %%al',
    'call_reg': '\tmovw $t_%(id)04x_f, %%si\n\tcall *%%si\n\tjmp t_%(id)04x_d\n'
                't_%(id)04x_f:\n\tmovw $3, %%ax\n\tret\nt_%(id)04x_d:',
    'lret': '\tpushw %%cs\n\tpushw $t_%(id)04x_d\n\tlretw\n\tmovw $1, %%ax\n'
            't_%(id)04x_d:\n\tmovw $4, %%ax',
}

HARNESS = r'''
	.code16
	.section .text
	.globl _start
_start:
	.word 0xaa55
	.byte 0
	jmp x_post_entry
	.org 0x18
	.word pcir
	.word 0
	.org 0x20
	.ascii "X86COMPAT TEST ROM"
	.balign 4
pcir:
	.ascii "PCIR"
	.word 0x%(vid)04x, 0x%(did)04x, 0, 0x18
	.byte 0, 0, 0, 3
	.word 0, 1
	.byte 0, 0x80
	.word 0

	# Written at run time: keep it in a page without code, or an x86 TCG
	# reference run retranslates on every store.
	.org 0x1000
h_ss:	.word 0
h_sp:	.word 0
r_eax:	.long 0
r_ecx:	.long 0
r_edx:	.long 0
r_ebx:	.long 0
r_ebp:	.long 0
r_esi:	.long 0
r_edi:	.long 0
r_sp:	.word 0
r_fl:	.word 0
r_ds:	.word 0
r_es:	.word 0
	.balign 16
csbuf:	.fill 0x%(csbuf_size)x, 1, 0
x_entry_sp:	.word 0
x_entry_ss:	.word 0

	.org 0x2000
x_post_entry:
	pushfw
	cli
	cld
	.byte 0x66, 0x50, 0x66, 0x51, 0x66, 0x52, 0x66, 0x53
	.byte 0x66, 0x55, 0x66, 0x56, 0x66, 0x57
	pushw %%ds
	pushw %%es
	movw %%sp, %%cs:x_entry_sp
	movw %%ss, %%ax
	movw %%ax, %%cs:x_entry_ss
	# QEMU drops port 0x3da unless the misc register selects colour.
	movw $0x3cc, %%dx
	inb %%dx, %%al
	orb $1, %%al
	movw $0x3c2, %%dx
	outb %%al, %%dx
	movw $x_s_begin, %%si
	call x_puts
%(tests)s
	movw $x_s_end, %%si
	call x_puts
	movw %%cs:x_entry_ss, %%ax
	movw %%ax, %%ss
	movw %%cs:x_entry_sp, %%sp
	popw %%es
	popw %%ds
	.byte 0x66, 0x5f, 0x66, 0x5e, 0x66, 0x5d, 0x66, 0x5b
	.byte 0x66, 0x5a, 0x66, 0x59, 0x66, 0x58
	popfw
	lretw

x_s_begin:	.asciz "X86C-BEGIN\n"
x_s_end:	.asciz "X86C-END\n"

# %%al -> port 0x3da
x_putc:
	pushw %%dx
	movw $0x3da, %%dx
	outb %%al, %%dx
	popw %%dx
	ret

# %%cs:%%si -> port, NUL-terminated
x_puts:
	pushw %%ax
1:	movb %%cs:(%%si), %%al
	testb %%al, %%al
	jz 2f
	call x_putc
	incw %%si
	jmp 1b
2:	popw %%ax
	ret

# %%ax as four hex digits
x_hex4:
	pushw %%cx
	pushw %%bx
	movw $4, %%cx
1:	rolw $1, %%ax
	rolw $1, %%ax
	rolw $1, %%ax
	rolw $1, %%ax
	movw %%ax, %%bx
	andw $0xf, %%bx
	pushw %%ax
	movb %%cs:x_digits(%%bx), %%al
	call x_putc
	popw %%ax
	decw %%cx
	jnz 1b
	popw %%bx
	popw %%cx
	ret
x_digits:	.ascii "0123456789abcdef"

x_space:
	pushw %%ax
	movb $0x20, %%al
	call x_putc
	popw %%ax
	ret

# Print the id, refill the window and clear the other scratch areas.
x_pre:
	pushw %%ax
	movb $0x54, %%al
	call x_putc
	popw %%ax
	call x_hex4
	movw $0x%(scr)04x, %%ax
	movw %%ax, %%es
	xorw %%di, %%di
	xorb %%cl, %%cl
1:	movb %%cl, %%al
	orb $0x40, %%al
	movb %%al, %%es:(%%di)
	addb $0x25, %%cl
	incw %%di
	testw $0xff, %%di
	jnz 3f
	addb $0x3d, %%cl
3:	cmpw $0x%(win)04x, %%di
	jne 1b
	movw $0x%(scr2)04x, %%ax
	movw %%ax, %%es
	movb $0, %%es:0x%(mark)04x
	xorw %%si, %%si
2:	movb $0, %%cs:csbuf(%%si)
	incw %%si
	cmpw $0x%(csbuf_size)x, %%si
	jne 2b
	ret

x_setregs:
	pushw $0x%(flags0)04x
	popfw
%(setregs)s
	ret

# Called right after the test: save the state, then return on the harness
# stack.
x_capture:
	movw %%ax, %%cs:r_eax
	pushfw
	popw %%ax
	movw %%ax, %%cs:r_fl
	pushw $0
	popw %%ax
	.byte 0x66, 0xc1, 0xc8, 0x10
	movw %%ax, %%cs:r_eax+2
%(saveregs)s
	movw %%ds, %%ax
	movw %%ax, %%cs:r_ds
	movw %%es, %%ax
	movw %%ax, %%cs:r_es
	popw %%ax
	movw %%sp, %%cs:r_sp
	movw %%cs:h_ss, %%dx
	movw %%dx, %%ss
	movw %%cs:h_sp, %%sp
	jmp *%%ax

# Window checksum, bytes that differ from the pattern, the first of them,
# the scratch-buffer checksum and the marker; then print the record.
x_post:
	movw $0x%(scr)04x, %%ax
	movw %%ax, %%ds
	xorw %%si, %%si
	xorw %%bx, %%bx
	xorw %%dx, %%dx
	movw $0xffff, %%bp
	xorb %%cl, %%cl
1:	movb (%%si), %%al
	rolw $1, %%bx
	xorb %%ah, %%ah
	addw %%ax, %%bx
	movb %%cl, %%ah
	orb $0x40, %%ah
	cmpb %%ah, %%al
	je 2f
	incw %%dx
	cmpw $0xffff, %%bp
	jne 2f
	movw %%si, %%bp
2:	addb $0x25, %%cl
	incw %%si
	testw $0xff, %%si
	jnz 4f
	addb $0x3d, %%cl
4:	cmpw $0x%(win)04x, %%si
	jne 1b
	pushw %%dx
	xorw %%si, %%si
	xorw %%di, %%di
3:	movb %%cs:csbuf(%%si), %%al
	rolw $1, %%di
	xorb %%ah, %%ah
	addw %%ax, %%di
	incw %%si
	cmpw $0x%(csbuf_size)x, %%si
	jne 3b
	movw $0x%(scr2)04x, %%ax
	movw %%ax, %%ds
	movb 0x%(mark)04x, %%al
	xorb %%ah, %%ah
	pushw %%ax
%(print)s
	popw %%ax
	call x_space
	call x_hex4
	popw %%ax
	call x_space
	call x_hex4
	movw %%bx, %%ax
	call x_space
	call x_hex4
	movw %%bp, %%ax
	call x_space
	call x_hex4
	movw %%di, %%ax
	call x_space
	call x_hex4
	movb $0x0a, %%al
	call x_putc
	ret
'''

FIELDS = ['eax', 'ecx', 'edx', 'ebx', 'ebp', 'esi', 'edi', 'sp', 'fl', 'ds',
          'es', 'mark', 'ndiff', 'wsum', 'first', 'cssum']


def harness(tests_asm, vid, did):
    setregs = '\n'.join('\t.byte 0x66, 0x%02x\n\t.long 0x%08x'
                        % (0xB8 + REGNUM[r], v) for r, v in REG0.items())
    save = []
    for r in ('ecx', 'edx', 'ebx', 'ebp', 'esi', 'edi'):
        n = REGNUM[r]
        save.append('\tmovw %%%s, %%cs:r_%s' % (R16[n], r))
        save.append('\t.byte 0x66, 0xc1, 0x%02x, 0x10' % (0xC8 + n))
        save.append('\tmovw %%%s, %%cs:r_%s+2' % (R16[n], r))
    prn = []
    for r in ('eax', 'ecx', 'edx', 'ebx', 'ebp', 'esi', 'edi'):
        prn += ['\tcall x_space', '\tmovw %%cs:r_%s+2, %%ax' % r,
                '\tcall x_hex4', '\tmovw %%cs:r_%s, %%ax' % r, '\tcall x_hex4']
    for f in ('sp', 'fl', 'ds', 'es'):
        prn += ['\tcall x_space', '\tmovw %%cs:r_%s, %%ax' % f,
                '\tcall x_hex4']
    return HARNESS % {'tests': tests_asm, 'vid': vid, 'did': did,
                      'csbuf_size': CSBUF_SIZE, 'scr': SCR, 'scr2': SCR2,
                      'win': WIN, 'mark': MARK_OFF, 'flags0': FLAGS0,
                      'setregs': setregs, 'saveregs': '\n'.join(save),
                      'print': '\n'.join(prn)}


def shape(ins_text):
    return re.sub(r'-?0x[0-9a-f]+|\b[0-9a-f]+\b', 'N',
                  ins_text).replace(' ', '')


def gen(objdump, outdir, objs, ctl_objs, skip, vid=0x1234, did=0x1111):
    os.makedirs(outdir, exist_ok=True)
    seen, tests = set(), []
    tests.append(Test(0, 'baseline', '-', 'nop', '', [0x90]))
    tid = 1
    for ins in inventory(objdump, objs):
        if SKIP.match(ins['mnem']) or not ins['mnem']:
            continue
        d = decode(ins['bytes'])
        key = formkey(d)
        if key in seen:
            continue
        # %ss and %sp loads other than through the stack instructions
        # would move the harness stack; segment registers other than
        # es/ds are never loaded by the VGA code.
        if re.search(r',%(ss|fs|gs)$', ins['ops']) or \
                (d['op'] and d['op'][0] == 0x17):
            continue
        seen.add(key)
        t = build_test(tid, ins, 0)
        if t is None:
            continue
        t.key = key
        tests.append(t)
        tid += 1
    for name, code in TEMPLATES:
        t = Test(tid, 'template', '-', name, '', code)
        tests.append(t)
        tid += 1
    tests += controls(objdump, ctl_objs, tid) if ctl_objs else []
    tests = [t for t in tests if t.id not in skip]
    # csbuf is placed by the assembler; fix CS-relative displacements and
    # base overrides with its real offset in a second pass.
    for rnd in (0, 1):
        asm = harness(emit(tests), vid, did)
        src = os.path.join(outdir, 'testrom.S')
        obj = os.path.join(outdir, 'testrom.o')
        out = os.path.join(outdir, 'testrom.bin')
        open(src, 'w').write(asm)
        subprocess.run(['as', '--32', '-o', obj, src], check=True)
        syms = subprocess.run(['nm', obj], capture_output=True, text=True,
                              check=True).stdout
        addr = {l.split()[2]: int(l.split()[0], 16)
                for l in syms.splitlines() if len(l.split()) == 3}
        if rnd == 0:
            csbuf = addr['csbuf']
            for i, t in enumerate(tests):
                if t.kind in ('generic', 'segload', 'popf') and \
                        isinstance(t.orig, list):
                    ins = {'bytes': t.orig, 'mnem': t.mnem, 'ops': t.ops,
                           'src': t.src}
                    nt = build_test(t.id, ins, csbuf)
                    nt.key = t.key
                    tests[i] = nt
    subprocess.run(['objcopy', '-O', 'binary', '-j', '.text', obj, out],
                   check=True)
    rom = bytearray(open(out, 'rb').read())
    rom += bytes((-len(rom)) % 512)
    if len(rom) > 0x10000:
        sys.exit('test ROM too large: %d bytes' % len(rom))
    rom[2] = len(rom) // 512
    p = addr['pcir']
    rom[p + 16] = len(rom) // 512 & 0xFF
    rom[p + 17] = len(rom) // 512 >> 8
    rom[-1] = (-sum(rom[:-1])) & 0xFF
    open(out, 'wb').write(rom)
    # Only the displacements and immediates may differ from the original.
    bad = 0
    for t in tests:
        if not isinstance(t.orig, list):
            continue
        s, e = addr['t_%04x_i' % t.id], addr['t_%04x_i' % t.id] + len(t.code)
        dis = subprocess.run([objdump, '-D', '-b', 'binary', '-m', 'i8086',
                              '--start-address=%d' % s,
                              '--stop-address=%d' % e, out],
                             capture_output=True, text=True).stdout
        m = re.findall(r'^\s*[0-9a-f]+:\s+(?:[0-9a-f]{2} )+\s*(.*)$', dis,
                       re.M)
        got = shape(m[0]) if m else ''
        want = shape((t.mnem + ' ' + t.ops).strip())
        if got.split('<')[0].strip() != want.split('<')[0].strip():
            bad += 1
            sys.stderr.write('shape mismatch T%04x: %s | %s\n'
                             % (t.id, want, got))
    man = [{'id': t.id, 'kind': t.kind, 'src': t.src, 'mnem': t.mnem,
            'ops': t.ops, 'code': ' '.join('%02x' % x for x in t.code)
            if isinstance(t.code, list) else t.code,
            'orig': ' '.join('%02x' % x for x in t.orig) if t.orig else '',
            'mask': t.mask, 'key': getattr(t, 'key', '')} for t in tests]
    json.dump({'fields': FIELDS, 'tests': man},
              open(os.path.join(outdir, 'manifest.json'), 'w'), indent=1)
    sys.stderr.write('x86compat_testrom: %d tests, %d bytes, %d shape '
                     'mismatches\n' % (len(tests), len(rom), bad))


def parse_trace(path):
    text = ''
    for l in open(path, errors='replace'):
        m = re.search(r'vga_std_write_io addr 0x3da, val 0x([0-9a-f]+)', l)
        if m:
            text += chr(int(m.group(1), 16))
    recs = {}
    for l in text.split('\n'):
        m = re.match(r'T([0-9a-f]{4})((?: [0-9a-f]{4,8}){16})$', l.strip())
        if m:
            recs[int(m.group(1), 16)] = [int(x, 16) for x in m.group(2).split()]
    return recs, 'X86C-END' in text


def diff(manifest, ref, test):
    man = json.load(open(manifest))
    fields = man['fields']
    r, rend = parse_trace(ref)
    t, tend = parse_trace(test)
    print('reference: %d records%s; test: %d records%s'
          % (len(r), '' if rend else ' (no END)', len(t),
             '' if tend else ' (no END)'))
    bad = 0
    for e in man['tests']:
        i = e['id']
        if i not in r:
            print('T%04x missing in the reference' % i)
            continue
        if i not in t:
            print('T%04x missing in the test run (first missing test: the '
                  'run stopped here): %s %s [%s] %s'
                  % (i, e['mnem'], e['ops'], e['code'], e['src']))
            bad += 1
            break
        a, b = list(r[i]), list(t[i])
        fl = fields.index('fl')
        a[fl] &= e['mask']
        b[fl] &= e['mask']
        d = [f for f, x, y in zip(fields, a, b) if x != y]
        if d:
            bad += 1
            print('T%04x %-8s %-30s [%s] %s: %s' % (
                i, e['kind'], (e['mnem'] + ' ' + e['ops'])[:30], e['code'],
                e['src'], ', '.join('%s %x/%x' % (f, a[fields.index(f)],
                                                  b[fields.index(f)])
                                    for f in d)))
    print('%d of %d tests differ' % (bad, len(man['tests'])))


def main():
    a = sys.argv[1:]
    if a[:1] == ['gen']:
        skip = set()
        if '--skip' in a:
            k = a.index('--skip')
            skip = {int(x, 16) for x in a[k + 1].split(',')}
            del a[k:k + 2]
        ctl = []
        if '--control' in a:
            k = a.index('--control')
            ctl, a = a[k + 1:], a[:k]
        gen(a[1], a[2], a[3:], ctl, skip)
    elif a[:1] == ['diff'] and len(a) == 4:
        diff(a[1], a[2], a[3])
    else:
        sys.exit('usage: see the file header')


if __name__ == '__main__':
    main()
