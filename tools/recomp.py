#!/usr/bin/env python3
"""Static recompiler: Quarantine SKIPIT.EXE (Watcom LE, DOS/4GW) -> C.

Model
  * Registers, flags (lazy), segment bases and x87 state live in globals (runtime/rt.h).
  * Guest memory is one flat byte array MEM; objects are loaded 0x100000 above their
    preferred bases (so the data object no longer overlaps the VGA window).
  * Every discovered function entry becomes `void f_XXXXXXXX(void)`.  Calls push a
    return address on the guest stack and call the C function; `ret` pops and returns.
    Jumps to another entry are tail calls.  Indirect branches go through rt_dispatch().
  * The self-modifying span loops are emitted as multi-entry "region" functions that
    test, at every possible patch point, whether the byte there has become 0xC3 (ret).
"""
import sys, os, struct, collections
from capstone import *
from capstone.x86 import *

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from le import LE

REBASE = 0x100000
le = LE(sys.argv[1], REBASE)
OUT = sys.argv[2]
os.makedirs(OUT, exist_ok=True)

code = le.objs[0]
CB, CE = code['base'], code['base'] + code['vsize']
MEM = bytes(code['data'][:code['vsize']])
OBJ2 = le.objs[1]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True

DOBJ = le.objs[2]
DB, DE = DOBJ['base'], DOBJ['base'] + DOBJ['vsize']
DMEM = bytes(DOBJ['data'][:DOBJ['vsize']])
# code may also live in the data object (hand-written asm modules); only reached via branches
def incode(a): return CB <= a < CE or DB <= a < DE
def raw(a, n):
    if CB <= a < CE: return MEM[a-CB:a-CB+n]
    return DMEM[a-DB:a-DB+n]

_cache = {}
def dis(a):
    if a in _cache: return _cache[a]
    try: i = next(md.disasm(raw(a, 16), a))
    except StopIteration: i = None
    _cache[a] = i
    return i

# ---------------------------------------------------------------- fixup classes
code_fix = {}     # position in code object -> target address (32-bit offset fixups)
data_code_ptrs = []  # (pos, target) data-object pointers into code
for (so, pos, st, to, tg) in le.fixups:
    if st != 7 or to != 1: continue
    t = le.objs[0]['base'] + tg
    if so == 1: code_fix[CB + pos] = t
    elif so == 3: data_code_ptrs.append((le.objs[2]['base'] + pos, t))

# ---------------------------------------------------------------- SMC regions
# pointer tables in data whose targets are unrolled span fragments (mov al,[esi+ebx])
SMC_SIG = b'\x8a\x04\x1e'
smc_targets = set(t for _, t in data_code_ptrs if MEM[t-CB:t-CB+3] == SMC_SIG)

# ---------------------------------------------------------------- recursive descent
jump_tables = {}          # insn addr -> list of targets
table_bytes = set()
def table_at(tbl):
    out = []; p = tbl
    while p in code_fix and incode(code_fix[p]):
        out.append(code_fix[p]); table_bytes.update(range(p, p+4)); p += 4
    return out

seen = set(); direct_calls = set(); branch_targets = set()
def explore(starts):
    work = list(starts)
    while work:
        a = work.pop()
        while incode(a) and a not in seen and a not in table_bytes:
            i = dis(a)
            if i is None: break
            seen.add(a)
            g = set(i.groups); m = i.mnemonic
            if X86_GRP_JUMP in g or X86_GRP_CALL in g:
                op = i.operands[0]
                if op.type == X86_OP_IMM:
                    t = op.imm & 0xffffffff
                    if X86_GRP_CALL in g: direct_calls.add(t)
                    else: branch_targets.add(t)
                    work.append(t)
                elif m.endswith('jmp') and op.type == X86_OP_MEM and op.mem.index and op.mem.scale == 4 \
                        and op.mem.base == 0 and incode(op.mem.disp & 0xffffffff):
                    tg = table_at(op.mem.disp & 0xffffffff)
                    jump_tables[a] = tg; branch_targets.update(tg); work.extend(tg)
                if m.endswith('jmp') or m == 'ljmp': break
            if m in ('ret', 'retf', 'iretd', 'hlt'): break
            a += i.size

entry = le.entry()
fptr_seeds = set(t for _, t in data_code_ptrs if t not in smc_targets)
explore([entry] + sorted(fptr_seeds))
# code-object fixups that are not jump-table entries are function pointers (push offset f, etc.)
for p, t in code_fix.items():
    if p in table_bytes or t in table_bytes: continue
    i_at = None
    fptr_seeds.add(t)
explore(sorted(fptr_seeds))
# prologue scan in still-unexplored bytes
import re
prologue_seeds = set()
for mt in re.finditer(rb'(?:[\x50-\x57]){1,7}\x89\xe5', MEM):
    a = CB + mt.start()
    if a not in seen and a not in table_bytes and not any(x in seen for x in range(a, a + 4)):
        prologue_seeds.add(a)
explore(sorted(prologue_seeds))

# Watcom int386x stub table: 256 x (int N; ret), reached by push+ret
int_stubs = set()
k = MEM.find(b'\xcd\x00\xc3\xcd\x01\xc3\xcd\x02\xc3')
if k >= 0:
    for n in range(256): int_stubs.add(CB + k + 3 * n)
    explore(sorted(int_stubs))
entries = set([entry]) | direct_calls | fptr_seeds | prologue_seeds | int_stubs
entries = set(e for e in entries if incode(e) and e in seen and e not in smc_targets)
# smc region: explore separately (not part of normal function bodies)
smc_seen_before = set(seen)

# ---------------------------------------------------------------- C emission helpers
REGS32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi']
REGMAP = {}
for k, r in enumerate(REGS32):
    REGMAP[r] = (k, 32)
    REGMAP[r[1:]] = (k, 16)
for k, r in enumerate(['al', 'cl', 'dl', 'bl']): REGMAP[r] = (k, 8)
for k, r in enumerate(['ah', 'ch', 'dh', 'bh']): REGMAP[r] = (k, 'h')
SREGS = {'es': 0, 'cs': 1, 'ss': 2, 'ds': 3, 'fs': 4, 'gs': 5}

class Unsupported(Exception): pass

def regname(i, r): return i.reg_name(r)

def rreg(name):
    if name in SREGS: return 'S[%d]' % SREGS[name]
    if name not in REGMAP: raise Unsupported('reg ' + name)
    k, sz = REGMAP[name]
    if sz == 32: return 'R[%d]' % k
    if sz == 16: return '((uint16_t)R[%d])' % k
    if sz == 8: return '((uint8_t)R[%d])' % k
    return '((uint8_t)(R[%d]>>8))' % k

def wreg(name, v):
    if name in SREGS: return 'set_sreg(%d, (uint16_t)(%s));' % (SREGS[name], v)
    if name not in REGMAP: raise Unsupported('reg ' + name)
    k, sz = REGMAP[name]
    if sz == 32: return 'R[%d] = (uint32_t)(%s);' % (k, v)
    if sz == 16: return 'R[%d] = (R[%d] & 0xffff0000u) | (uint16_t)(%s);' % (k, k, v)
    if sz == 8: return 'R[%d] = (R[%d] & 0xffffff00u) | (uint8_t)(%s);' % (k, k, v)
    return 'R[%d] = (R[%d] & 0xffff00ffu) | ((uint32_t)(uint8_t)(%s) << 8);' % (k, k, v)

def ea(i, op, with_seg=True):
    m = op.mem
    parts = []
    addr16 = False
    if m.base:
        bn = regname(i, m.base)
        if bn in ('eip',): parts.append('0x%xu' % (i.address + i.size))
        else:
            if REGMAP[bn][1] == 16: addr16 = True
            parts.append(rreg(bn))
    if m.index:
        xn = regname(i, m.index)
        if REGMAP[xn][1] == 16: addr16 = True
        parts.append('%s*%d' % (rreg(xn), m.scale) if m.scale != 1 else rreg(xn))
    if m.disp or not parts: parts.append('0x%xu' % (m.disp & 0xffffffff))
    e = '(uint32_t)(' + ' + '.join(parts) + ')'
    if addr16: e = '(uint32_t)(uint16_t)(' + ' + '.join(parts) + ')'
    if not with_seg: return e
    if m.segment:
        seg = regname(i, m.segment)
    else:
        seg = 'ss' if m.base and regname(i, m.base) in ('esp', 'ebp', 'sp', 'bp') else 'ds'
    return '(SB[%d] + %s)' % (SREGS[seg], e)

def opsize(op): return op.size * 8

def rd(i, op, sz=None):
    if op.type == X86_OP_REG: return rreg(regname(i, op.reg))
    if op.type == X86_OP_IMM:
        s = sz or opsize(op)
        return '0x%xu' % (op.imm & ((1 << s) - 1))
    if op.type == X86_OP_MEM:
        s = sz or opsize(op)
        return 'RD%d(%s)' % (s, ea(i, op))
    raise Unsupported('op type')

def wr(i, op, v, sz=None):
    if op.type == X86_OP_REG: return wreg(regname(i, op.reg), v)
    if op.type == X86_OP_MEM:
        s = sz or opsize(op)
        return 'WR%d(%s, %s);' % (s, ea(i, op), v)
    raise Unsupported('write op')

CONDS = {
    'o': 'get_of()', 'no': '!get_of()', 'b': 'cond_b()', 'ae': '!cond_b()', 'e': 'get_zf()', 'ne': '!get_zf()',
    'be': 'cond_be()', 'a': '!cond_be()', 's': 'get_sf()', 'ns': '!get_sf()', 'p': 'get_pf()', 'np': '!get_pf()',
    'l': 'cond_l()', 'ge': '!cond_l()', 'le': 'cond_le()', 'g': '!cond_le()',
}

ALU2 = {'add': 'alu_add', 'or': 'alu_or', 'adc': 'alu_adc', 'sbb': 'alu_sbb', 'and': 'alu_and',
        'sub': 'alu_sub', 'xor': 'alu_xor', 'cmp': 'alu_sub', 'test': 'alu_and'}
SHIFTS = {'shl': 'alu_shl', 'sal': 'alu_shl', 'shr': 'alu_shr', 'sar': 'alu_sar', 'rol': 'alu_rol',
          'ror': 'alu_ror', 'rcl': 'alu_rcl', 'rcr': 'alu_rcr'}

def is_far_indirect(i):
    b = bytes(i.bytes)
    k = 0
    while b[k] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2, 0xf3): k += 1
    return b[k] == 0xff and ((b[k+1] >> 3) & 7) in (3, 5)

# Entry hooks: C inserted at the top of a function, before its first instruction.
HOOKS = {
    # Manual-lookup copy protection screen ("flesh ratio factor"). Returns EAX=1 when the
    # answer is right; the caller (0x11024e) then continues into the game. Skipped unless
    # QUAR_COPYPROT=1.
    0x147c44: 'if (rt_skip_copyprot) { R[0] = 1; RET(0); return; }',
}
# Idle points: heads of loops that only poll a timer tick counter, the keyboard or the voice stream.
# The generated code sleeps briefly there (rt_idle) instead of spinning a host core.
IDLE = {0x110784, 0x1133c9,           # wait for a key (kbhit loop)
        0x12ae8e,                     # wait for a key or a tick timeout
        0x14a37e,                     # wait 3 ticks
        0x14b289,                     # wait for the streamed voice to finish
        0x14b5fa, 0x14b6a0, 0x14b75b} # FLI players: wait for the next frame's tick

# HMI SOS API entry points, emulated in runtime/rt_snd.c (value = bytes the callee pops).
for _a, _n in {0x155194: 0, 0x15532d: 8, 0x155b9c: 0, 0x1552c2: 0, 0x1561d5: 0, 0x15623d: 0,
               0x15626b: 0x10, 0x156acf: 0, 0x156fb8: 0, 0x1575d1: 0, 0x157637: 0, 0x157e7d: 0,
               0x154208: 8, 0x15452a: 0}.items():
    HOOKS[_a] = 'if (snd_hle(0x%x)) { RET(%d); return; }' % (_a, _n)

class Emitter:
    """Emits one function (or SMC region) body."""
    def __init__(self, name, entry_points, is_region=False):
        self.name = name; self.eps = entry_points; self.region = is_region
        self.body = {}; self.out = []; self.targets = set()
        work = list(entry_points)
        while work:
            a = work.pop()
            while incode(a) and a not in self.body and a not in table_bytes:
                if a != self.eps[0] and a in entries and not is_region: break
                i = dis(a)
                if i is None: break
                self.body[a] = i
                m = i.mnemonic; g = set(i.groups)
                if X86_GRP_JUMP in g:
                    op = i.operands[0]
                    if op.type == X86_OP_IMM:
                        t = op.imm & 0xffffffff
                        if self.local(t): self.targets.add(t); work.append(t)
                    elif a in jump_tables:
                        for t in jump_tables[a]:
                            if self.local(t): self.targets.add(t); work.append(t)
                    if m in ('jmp', 'ljmp', 'notrack jmp'): break
                if m in ('ret', 'retf', 'iretd', 'hlt'): break
                a += i.size

    def local(self, t):
        if not incode(t) or t in table_bytes: return False
        if self.region: return True
        return t == self.eps[0] or t not in entries

    def emit(self):
        o = self.out
        addrs = sorted(self.body)
        if self.region:
            o.append('void %s(uint32_t ep) {' % self.name)
            o.append('  switch (ep) {')
            for e in sorted(self.eps): o.append('  case 0x%x: goto L_%x;' % (e, e))
            o.append('  default: rt_fatal("bad region entry %%08x", ep);\n  }')
        else:
            o.append('void %s(void) {' % self.name)
            o.append('  FN_ENTER(0x%x);' % self.eps[0])
            if self.eps[0] in HOOKS: o.append('  ' + HOOKS[self.eps[0]])
        if not self.region and addrs and addrs[0] != self.eps[0]:
            o.append('  goto L_%x;' % self.eps[0])
        for k, a in enumerate(addrs):
            i = self.body[a]
            o.append('L_%x:' % a)
            if a in IDLE: o.append('  rt_idle();')
            if self.region and a in smc_targets:
                o.append('  if (MEM[0x%x] == 0xC3) { ESP += 4; return; }' % a)
            try:
                lines, falls = self.insn(i)
            except (Unsupported, KeyError, IndexError) as ex:
                lines, falls = ['rt_unimpl(0x%x, "%s %s (%s)");' % (a, i.mnemonic, i.op_str, ex)], False
            for l in lines: o.append('  ' + l)
            nxt = a + i.size
            if falls and not (k + 1 < len(addrs) and addrs[k + 1] == nxt):
                o.append('  ' + self.goto(nxt, a))
        o.append('}')
        # labels that are never jumped to are harmless but noisy -> keep (compiler ignores)
        return o

    def goto(self, t, cur):
        if t in self.body and (self.region or t == self.eps[0] or t not in entries):
            if t <= cur: return '{ IRQCHK(); goto L_%x; }' % t
            return 'goto L_%x;' % t
        if incode(t) and (t in entries):
            return '{ f_%08x(); return; }' % t
        if incode(t):
            return 'rt_unimpl(0x%x, "jump outside function to %08x");' % (cur, t)
        return 'rt_unimpl(0x%x, "jump to non-code %08x");' % (cur, t)

    def call_direct(self, t, nxt):
        if incode(t) and t in entries:
            return ['CALLF(f_%08x, 0x%xu);' % (t, nxt)]
        return ['CALLD(0x%xu, 0x%xu);' % (t, nxt)]

    def insn(self, i):
        m = i.mnemonic; ops = i.operands; a = i.address; nxt = a + i.size
        L = []
        if m.startswith('notrack '): m = m[8:]
        if m == 'nop' or m == 'wait' or m == 'fwait': return [], True
        if m == 'mov':
            d, s = ops
            if d.type == X86_OP_REG and regname(i, d.reg) in SREGS:
                return [wr(i, d, rd(i, s, 16))], True
            if s.type == X86_OP_REG and regname(i, s.reg) in SREGS:
                return [wr(i, d, rd(i, s), 16 if d.type == X86_OP_MEM else None)], True
            return [wr(i, d, rd(i, s))], True
        if m in ('movsx', 'movzx'):
            d, s = ops; ss, ds = opsize(s), opsize(d)
            if m == 'movzx': v = rd(i, s)
            else: v = '(uint32_t)(int32_t)(int%d_t)%s' % (ss, rd(i, s))
            return [wr(i, d, v)], True
        if m == 'lea':
            d, s = ops
            return [wr(i, d, ea(i, s, with_seg=False))], True
        if m == 'push':
            s = ops[0]
            if s.type == X86_OP_REG and regname(i, s.reg) in SREGS:
                return ['PUSH32(%s);' % rd(i, s)], True
            if s.type == X86_OP_IMM:
                if s.size == 2: return ['PUSH16(0x%xu);' % (s.imm & 0xffff)], True
                return ['PUSH32(0x%xu);' % (s.imm & 0xffffffff)], True
            if opsize(s) == 16: return ['PUSH16(%s);' % rd(i, s)], True
            if s.type == X86_OP_REG and regname(i, s.reg) == 'esp':
                return ['{ uint32_t t = ESP; PUSH32(t); }'], True
            return ['{ uint32_t t = %s; PUSH32(t); }' % rd(i, s)], True
        if m == 'pop':
            d = ops[0]
            if d.type == X86_OP_REG and regname(i, d.reg) in SREGS:
                return ['{ uint32_t t = POP32(); %s }' % wr(i, d, 't')], True
            if opsize(d) == 16: return ['{ uint32_t t = POP16(); %s }' % wr(i, d, 't')], True
            if d.type == X86_OP_MEM:  # address computed after esp increment
                return ['{ uint32_t t = POP32(); %s }' % wr(i, d, 't')], True
            return ['{ uint32_t t = POP32(); %s }' % wr(i, d, 't')], True
        if m in ALU2:
            d, s = ops; sz = opsize(d)
            fn = ALU2[m]
            sv = rd(i, s, sz) if s.type == X86_OP_IMM else rd(i, s)
            if s.type == X86_OP_IMM:
                sv = '0x%xu' % (s.imm & ((1 << sz) - 1))
            if m in ('cmp', 'test'):
                return ['%s(%s, %s, %d);' % (fn, rd(i, d), sv, sz)], True
            if m == 'xor' and d.type == X86_OP_REG and s.type == X86_OP_REG and d.reg == s.reg:
                return [wr(i, d, '0'), 'alu_xor(0, 0, %d);' % sz], True
            return ['{ uint32_t r = %s(%s, %s, %d); %s }' % (fn, rd(i, d), sv, sz, wr(i, d, 'r'))], True
        if m in ('inc', 'dec', 'neg', 'not'):
            d = ops[0]; sz = opsize(d)
            return ['{ uint32_t r = alu_%s(%s, %d); %s }' % (m, rd(i, d), sz, wr(i, d, 'r'))], True
        if m in SHIFTS:
            d = ops[0]; sz = opsize(d)
            cnt = '1' if len(ops) == 1 else rd(i, ops[1], 8)
            return ['{ uint32_t r = %s(%s, (%s) & 31, %d); %s }' % (SHIFTS[m], rd(i, d), cnt, sz, wr(i, d, 'r'))], True
        if m in ('shld', 'shrd'):
            d, s, c = ops; sz = opsize(d)
            return ['{ uint32_t r = alu_%s(%s, %s, (%s) & 31, %d); %s }' % (m, rd(i, d), rd(i, s), rd(i, c, 8), sz, wr(i, d, 'r'))], True
        if m == 'imul':
            if len(ops) == 1:
                return ['rt_imul1(%s, %d);' % (rd(i, ops[0]), opsize(ops[0]))], True
            if len(ops) == 2:
                d, s = ops; sz = opsize(d)
                return ['{ uint32_t r = alu_imul(%s, %s, %d); %s }' % (rd(i, d), rd(i, s), sz, wr(i, d, 'r'))], True
            d, s, c = ops; sz = opsize(d)
            return ['{ uint32_t r = alu_imul(%s, 0x%xu, %d); %s }' % (rd(i, s), c.imm & 0xffffffff, sz, wr(i, d, 'r'))], True
        if m in ('mul', 'div', 'idiv'):
            return ['rt_%s1(%s, %d);' % (m, rd(i, ops[0]), opsize(ops[0]))], True
        if m == 'cwde': return ['R[0] = (uint32_t)(int32_t)(int16_t)R[0];'], True
        if m == 'cbw': return [wreg('ax', '(uint16_t)(int16_t)(int8_t)R[0]')], True
        if m == 'cwd': return [wreg('dx', '((int16_t)R[0] < 0) ? 0xffff : 0')], True
        if m == 'cdq': return ['R[2] = ((int32_t)R[0] < 0) ? 0xffffffffu : 0;'], True
        if m == 'xchg':
            d, s = ops
            return ['{ uint32_t t1 = %s, t2 = %s; %s %s }' % (rd(i, d), rd(i, s), wr(i, d, 't2'), wr(i, s, 't1'))], True
        if m.startswith('set'):
            c = CONDS[m[3:]]
            return [wr(i, ops[0], '(%s) ? 1 : 0' % c)], True
        if m == 'bsr':
            d, s = ops
            return ['{ uint32_t v = %s; if (v) { %s set_zf(0); } else set_zf(1); }' % (rd(i, s), wr(i, d, '31 - __builtin_clz(v)'))], True
        # ---- control flow
        if m in ('jmp', 'call') and is_far_indirect(i):
            e = ea(i, ops[0])
            if m == 'jmp':
                return ['{ uint32_t p = %s; rt_farjmp(RD32(p), RD16(p + 4)); return; }' % e], False
            return ['{ uint32_t p = %s; rt_farcall(RD32(p), RD16(p + 4), 0x%xu); }' % (e, nxt)], True
        if m == 'jmp':
            op = ops[0]
            if op.type == X86_OP_IMM:
                return [self.goto(op.imm & 0xffffffff, a)], False
            if a in jump_tables:
                idx = rd(i, op).replace('RD32(', 'RD32(')
                L = ['switch (%s) {' % rd(i, op)]
                for t in sorted(set(jump_tables[a])):
                    L.append('  case 0x%xu: %s' % (t, self.goto(t, a)))
                L.append('  default: rt_dispatch_tail(%s); return; }' % rd(i, op))
                return L, False
            return ['rt_dispatch_tail(%s); return;' % rd(i, op)], False
        if m.startswith('j') and m[1:] in CONDS:
            t = ops[0].imm & 0xffffffff
            return ['if (%s) %s' % (CONDS[m[1:]], self.goto(t, a))], True
        if m in ('jecxz', 'jcxz'):
            t = ops[0].imm & 0xffffffff
            r = 'R[1]' if m == 'jecxz' else '(uint16_t)R[1]'
            return ['if (%s == 0) %s' % (r, self.goto(t, a))], True
        if m in ('loop', 'loope', 'loopne'):
            t = ops[0].imm & 0xffffffff
            c = {'loop': '', 'loope': ' && get_zf()', 'loopne': ' && !get_zf()'}[m]
            return ['if (--R[1] != 0%s) %s' % (c, self.goto(t, a))], True
        if m == 'call':
            op = ops[0]
            if op.type == X86_OP_IMM:
                return self.call_direct(op.imm & 0xffffffff, nxt), True
            return ['{ uint32_t t = %s; CALLD(t, 0x%xu); }' % (rd(i, op), nxt)], True
        if m == 'ret':
            n = ops[0].imm if ops else 0
            return ['RET(%d); return;' % n], False
        if m == 'retf':
            n = ops[0].imm if ops else 0
            return ['rt_retf(%d); return;' % n], False
        if m == 'iretd':
            return ['rt_iret(); return;'], False
        if m == 'leave': return ['ESP = EBP; EBP = POP32();'], True
        if m == 'enter':
            sz, lvl = ops[0].imm, ops[1].imm
            if lvl: raise Unsupported('enter level')
            return ['PUSH32(EBP); EBP = ESP; ESP -= %d;' % sz], True
        if m == 'pushal':
            return ['{ uint32_t t = ESP; PUSH32(R[0]); PUSH32(R[1]); PUSH32(R[2]); PUSH32(R[3]); PUSH32(t); PUSH32(R[5]); PUSH32(R[6]); PUSH32(R[7]); }'], True
        if m == 'popal':
            return ['R[7] = POP32(); R[6] = POP32(); R[5] = POP32(); ESP += 4; R[3] = POP32(); R[2] = POP32(); R[1] = POP32(); R[0] = POP32();'], True
        if m == 'pushfd': return ['PUSH32(get_eflags());'], True
        if m == 'popfd': return ['set_eflags(POP32());'], True
        if m == 'lahf': return [wreg('ah', 'get_eflags() & 0xd5')], True
        if m == 'sahf': return ['set_eflags((get_eflags() & ~0xd5u) | (%s & 0xd5));' % rreg('ah')], True
        if m == 'cmc': return ['set_cf(!get_cf());'], True
        if m == 'stc': return ['set_cf(1);'], True
        if m == 'clc': return ['set_cf(0);'], True
        if m == 'cld': return ['DF = 0;'], True
        if m == 'std': return ['DF = 1;'], True
        if m == 'cli': return ['IF = 0;'], True
        if m == 'sti': return ['IF = 1;'], True
        if m in ('les', 'lds', 'lfs', 'lgs', 'lss'):
            d, s = ops
            seg = SREGS[m[1:]]
            e = ea(i, s)
            return ['{ uint32_t p = %s; uint32_t o = RD32(p); uint16_t sel = RD16(p + 4); %s set_sreg(%d, sel); }' % (e, wr(i, d, 'o'), seg)], True
        # ---- string ops
        base = m.split()[-1]
        rep = m.split()[0] if ' ' in m else ''
        if base in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd',
                    'scasb', 'scasw', 'scasd', 'cmpsb', 'cmpsw', 'cmpsd', 'insb', 'insw', 'insd',
                    'outsb', 'outsw', 'outsd'):
            if base == 'movsd' and ops and ops[0].size == 8: raise Unsupported('sse movsd')
            sz = {'b': 8, 'w': 16, 'd': 32}[base[-1]]
            srcseg = 3
            for op in ops:
                if op.type == X86_OP_MEM and op.mem.segment and regname(i, op.mem.base) in ('esi', 'si'):
                    srcseg = SREGS[regname(i, op.mem.segment)]
            kind = base[:-1]
            pfx = bytes(i.bytes)[:4]
            k0 = 0
            while pfx[k0] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2, 0xf3): k0 += 1
            if not rep and 0xf3 in pfx[:k0]: rep = 'repe' if kind in ('scas', 'cmps') else 'rep'
            if not rep and 0xf2 in pfx[:k0]: rep = 'repne' if kind in ('scas', 'cmps') else 'rep'
            if kind in ('movs', 'stos', 'lods', 'ins', 'outs') and rep in ('rep', 'repne', 'repe'):
                rep = 'rep'
            return ['rt_str_%s(%d, %d, "%s"[0]);' % (kind, sz, srcseg, {'rep': 'r', 'repe': 'e', 'repne': 'n', '': '-'}[rep])], True
        if m in ('in', 'out'):
            if m == 'in':
                d, s = ops; sz = opsize(d)
                port = rd(i, s, 16) if s.type == X86_OP_REG else '0x%x' % s.imm
                return [wr(i, d, 'rt_in%d(%s)' % (sz, port))], True
            d, s = ops; sz = opsize(s)
            port = rd(i, d, 16) if d.type == X86_OP_REG else '0x%x' % d.imm
            return ['rt_out%d(%s, %s);' % (sz, port, rd(i, s))], True
        if m == 'int':
            return ['rt_int(0x%x);' % ops[0].imm], True
        if m in ('ljmp', 'lcall'):
            op = ops[0]
            if op.type == X86_OP_MEM:
                e = ea(i, op)
                if m == 'ljmp':
                    return ['{ uint32_t p = %s; rt_farjmp(RD32(p), RD16(p + 4)); return; }' % e], False
                return ['{ uint32_t p = %s; rt_farcall(RD32(p), RD16(p + 4), 0x%xu); }' % (e, nxt)], True
            raise Unsupported('direct far branch')
        if m == 'lsl':
            d, s = ops
            return ['{ uint32_t r; if (rt_lsl(%s, &r)) { %s set_zf(1); } else set_zf(0); }' % (rd(i, s, 16), wr(i, d, 'r'))], True
        if m == 'lar':
            d, s = ops
            return ['{ uint32_t r; if (rt_lar(%s, &r)) { %s set_zf(1); } else set_zf(0); }' % (rd(i, s, 16), wr(i, d, 'r'))], True
        if m == 'verr': return ['set_zf(rt_lsl(%s, 0));' % rd(i, ops[0], 16)], True
        if m == 'smsw': return [wr(i, ops[0], '0x0011')], True
        if m == 'aam':
            b = ops[0].imm if ops else 10
            return ['{ uint8_t al = (uint8_t)R[0]; uint8_t ah = al / %d; al %%= %d; R[0] = (R[0] & 0xffff0000u) | (ah << 8) | al; alu_or(al, 0, 8); }' % (b, b)], True
        if m.startswith('f'):
            return self.fpu(i, m, ops), True
        raise Unsupported('mnemonic')

    def fpu(self, i, m, ops):
        def memop(op):
            return ea(i, op), op.size
        if m in ('fld', 'fild'):
            op = ops[0]
            if op.type == X86_OP_REG: return ['fpu_push(ST(%d));' % (op.reg - X86_REG_ST0)]
            e, s = memop(op)
            k = {('fld', 4): 'f32', ('fld', 8): 'f64', ('fld', 10): 'f80', ('fild', 2): 'i16', ('fild', 4): 'i32', ('fild', 8): 'i64'}[(m, s)]
            return ['fpu_push(fpu_ld_%s(%s));' % (k, e)]
        if m in ('fst', 'fstp', 'fist', 'fistp'):
            op = ops[0]; pop = m.endswith('p')
            if op.type == X86_OP_REG:
                L = ['ST(%d) = ST(0);' % (op.reg - X86_REG_ST0)]
            else:
                e, s = memop(op)
                k = {('fst', 4): 'f32', ('fst', 8): 'f64', ('fst', 10): 'f80', ('fis', 2): 'i16', ('fis', 4): 'i32', ('fis', 8): 'i64'}[(m[:3] if m.startswith('fis') else 'fst', s)]
                L = ['fpu_st_%s(%s, ST(0));' % (k, e)]
            if pop: L.append('fpu_pop();')
            return L
        arith = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/', 'fsubr': 'r-', 'fdivr': 'r/'}
        bm = m.rstrip('p') if m not in ('fsubp', 'fsubrp') else m[:-1]
        if m in ('faddp', 'fsubp', 'fmulp', 'fdivp', 'fsubrp', 'fdivrp'): bm = m[:-1]
        if bm in arith or m in ('fiadd', 'fisub', 'fimul', 'fidiv'):
            pop = m in ('faddp', 'fsubp', 'fmulp', 'fdivp', 'fsubrp', 'fdivrp')
            opn = arith.get(bm) or {'fiadd': '+', 'fisub': '-', 'fimul': '*', 'fidiv': '/'}[m]
            def comb(x, y):
                if opn.startswith('r'): return '(%s) %s (%s)' % (y, opn[1], x)
                return '(%s) %s (%s)' % (x, opn, y)
            if m.startswith('fi'):
                e, s = memop(ops[0])
                return ['ST(0) = %s;' % comb('ST(0)', 'fpu_ld_i%d(%s)' % (s*8, e))]
            if len(ops) == 1 and ops[0].type == X86_OP_MEM:
                e, s = memop(ops[0])
                return ['ST(0) = %s;' % comb('ST(0)', 'fpu_ld_f%d(%s)' % (s*8, e))]
            if len(ops) == 0:
                d, s = 1, 0
            elif len(ops) == 1:
                d, s = 0, ops[0].reg - X86_REG_ST0
            else:
                d, s = ops[0].reg - X86_REG_ST0, ops[1].reg - X86_REG_ST0
            L = ['ST(%d) = %s;' % (d, comb('ST(%d)' % d, 'ST(%d)' % s))]
            if pop: L.append('fpu_pop();')
            return L
        if m in ('fcom', 'fcomp', 'fcompp', 'fucom', 'fucomp', 'fucompp'):
            if ops and ops[0].type == X86_OP_MEM:
                e, s = memop(ops[0]); other = 'fpu_ld_f%d(%s)' % (s*8, e)
            elif ops: other = 'ST(%d)' % (ops[0].reg - X86_REG_ST0)
            else: other = 'ST(1)'
            L = ['fpu_compare(ST(0), %s);' % other]
            if m.endswith('pp'): L += ['fpu_pop();', 'fpu_pop();']
            elif m.endswith('p'): L.append('fpu_pop();')
            return L
        if m == 'fnstsw':
            if ops[0].type == X86_OP_REG: return [wreg('ax', 'fpu_sw()')]
            return ['WR16(%s, fpu_sw());' % ea(i, ops[0])]
        if m == 'fnstcw': return ['WR16(%s, FPU_CW);' % ea(i, ops[0])]
        if m == 'fldcw': return ['FPU_CW = RD16(%s);' % ea(i, ops[0])]
        if m == 'fldz': return ['fpu_push(0.0);']
        if m == 'fld1': return ['fpu_push(1.0);']
        if m == 'fldlg2': return ['fpu_push(0.301029995663981195);']
        if m == 'fchs': return ['ST(0) = -ST(0);']
        if m == 'fabs': return ['ST(0) = fabs(ST(0));']
        if m == 'fsqrt': return ['ST(0) = sqrt(ST(0));']
        if m == 'frndint': return ['ST(0) = fpu_round(ST(0));']
        if m == 'fyl2x': return ['ST(1) = ST(1) * log2(ST(0)); fpu_pop();']
        if m == 'fxch':
            r = ops[0].reg - X86_REG_ST0 if ops else 1
            return ['{ double t = ST(0); ST(0) = ST(%d); ST(%d) = t; }' % (r, r)]
        if m in ('fninit', 'finit'): return ['fpu_init();']
        if m in ('fnsave', 'fsave'): return ['fpu_save(%s);' % ea(i, ops[0])]
        if m == 'frstor': return ['fpu_restore(%s);' % ea(i, ops[0])]
        raise Unsupported('fpu')

# ---------------------------------------------------------------- generate
funcs = sorted(entries)
chunks = collections.defaultdict(list)
CHUNK = 60
unimpl = collections.Counter()
for k, e in enumerate(funcs):
    em = Emitter('f_%08x' % e, [e])
    lines = em.emit()
    for l in lines:
        if 'rt_unimpl' in l: unimpl[l.split('"')[1].split(' ')[0]] += 1
    chunks[k // CHUNK].append('\n'.join(lines))

# SMC regions: one per pointer table
regions = []
tbls = collections.defaultdict(list)
ptrs = sorted((p, t) for p, t in data_code_ptrs if t in smc_targets)
cur = None
for p, t in ptrs:
    if cur is None or p != cur[-1][0] + 4: cur = []; tbls[p] = cur
    cur.append((p, t))
region_eps = sorted(set(t for _, t in ptrs))
# group fragments into contiguous code ranges -> regions
groups = []
for t in region_eps:
    if groups and t - groups[-1][-1] < 0x200: groups[-1].append(t)
    else: groups.append([t])
region_src = []
for g in groups:
    name = 'r_%08x' % g[0]
    em = Emitter(name, g, is_region=True)
    region_src.append('\n'.join(em.emit()))
    regions.append((min(em.body), max(a + em.body[a].size for a in em.body), name))

hdr = '#include "../runtime/rt.h"\n#include "funcs.h"\n'
for k, src in chunks.items():
    open(os.path.join(OUT, 'code_%03d.c' % k), 'w').write(hdr + '\n\n'.join(src) + '\n')
open(os.path.join(OUT, 'regions.c'), 'w').write(hdr + '\n\n'.join(region_src) + '\n')
with open(os.path.join(OUT, 'funcs.h'), 'w') as f:
    f.write('#pragma once\n')
    for e in funcs: f.write('void f_%08x(void);\n' % e)
    for lo, hi, n in regions: f.write('void %s(uint32_t ep);\n' % n)
with open(os.path.join(OUT, 'table.c'), 'w') as f:
    f.write(hdr)
    f.write('const struct fentry FUNCS[] = {\n')
    for e in funcs: f.write('  {0x%x, f_%08x},\n' % (e, e))
    f.write('};\nconst int NFUNCS = %d;\n' % len(funcs))
    f.write('const struct rentry REGIONS[] = {\n')
    for lo, hi, n in regions: f.write('  {0x%x, 0x%x, %s},\n' % (lo, hi, n))
    f.write('};\nconst int NREGIONS = %d;\n' % len(regions))
    f.write('const uint32_t GUEST_ENTRY = 0x%x;\n' % entry)
# memory image
with open(os.path.join(OUT, 'image.bin'), 'wb') as f:
    for ob in le.objs:
        f.write(struct.pack('<II', ob['base'], ob['vsize']))
        f.write(bytes(ob['data'][:ob['vsize']]))
with open(os.path.join(OUT, 'image_info.h'), 'w') as f:
    f.write('#define GUEST_ESP 0x%x\n' % (le.objs[le.esp_obj-1]['base'] + le.esp))
    f.write('#define OBJ2_BASE 0x%x\n#define OBJ3_BASE 0x%x\n' % (le.objs[1]['base'], le.objs[2]['base']))

print('functions %d, regions %d (%s), jump tables %d' % (len(funcs), len(regions), ', '.join('%s %x-%x' % (n, lo, hi) for lo, hi, n in regions), len(jump_tables)))
print('instructions covered: %d' % len(seen))
print('unimplemented sites:', unimpl.most_common(40))
