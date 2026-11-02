#!/usr/bin/env python3
"""Differential test: every distinct (non-branch, non-I/O) instruction of SKIPIT is translated with
the recompiler's emitter, compiled into a harness, and run against Unicorn from identical random
states. Mismatches in registers, defined flags or touched memory are reported.

usage: tools/.venv/bin/python tools/difftest.py extracted/cd/SKIPIT.EXE [runs-per-insn]
"""
import sys, os, random, struct, subprocess, collections
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.argv = [sys.argv[0], sys.argv[1], os.path.join(ROOT, 'build', 'difftest_gen')] + sys.argv[2:]
RUNS = int(sys.argv[3]) if len(sys.argv) > 3 else 4

# reuse the recompiler's discovery + emitter (it also writes gen output to build/difftest_gen; harmless)
import io, contextlib
with contextlib.redirect_stdout(io.StringIO()):
    import importlib.util
    spec = importlib.util.spec_from_file_location('recomp', os.path.join(HERE, 'recomp.py'))
    rc = importlib.util.module_from_spec(spec); spec.loader.exec_module(rc)
from capstone.x86 import *
from unicorn import *
from unicorn.x86_const import *

SKIP = {'jmp', 'call', 'ret', 'retf', 'iretd', 'int', 'int3', 'into', 'in', 'out', 'hlt', 'cli', 'sti', 'loop', 'loope',
        'loopne', 'jecxz', 'jcxz', 'lcall', 'ljmp', 'les', 'lds', 'lfs', 'lgs', 'lss', 'lsl', 'lar', 'verr', 'smsw',
        'sidt', 'sldt', 'arpl', 'bound', 'aaa', 'aas', 'daa', 'das', 'popfd', 'pushfd', 'enter', 'leave', 'bsr',
        'lahf', 'sahf', 'insb', 'insd', 'outsb', 'outsd', 'nop', 'wait'}
UNDEF = {  # flags undefined after these (AF never compared)
    'imul': 0xc4, 'mul': 0xc4, 'div': 0x8c5, 'idiv': 0x8c5, 'aam': 0x801,
    'shl': 0x800, 'sal': 0x800, 'shr': 0x800, 'sar': 0x800, 'rol': 0x800, 'ror': 0x800, 'rcl': 0x800, 'rcr': 0x800,
    'shld': 0x801, 'shrd': 0x801,
}
FLAGMASK = 0x8c5  # CF PF ZF SF OF

def pick(i):
    m = i.mnemonic
    if m.startswith('notrack') or m.split()[-1] in SKIP or m.startswith('j') or m.startswith('f'): return False
    if m.split()[-1] in ('movsb', 'movsw', 'movsd', 'stosb', 'stosw', 'stosd', 'lodsb', 'lodsw', 'lodsd', 'scasb', 'cmpsd'):
        return True
    for op in i.operands:
        if op.type == X86_OP_REG and i.reg_name(op.reg) in rc.SREGS: return False
        if op.type == X86_OP_MEM:
            if op.mem.segment and i.reg_name(op.mem.segment) in ('fs', 'gs', 'cs'): return False
            for r in (op.mem.base, op.mem.index):
                if r and rc.REGMAP.get(i.reg_name(r), (0, 32))[1] == 16: return False
    return True

uniq = {}
for a in sorted(rc.seen):
    i = rc.dis(a)
    if i is None or not pick(i): continue
    key = bytes(i.bytes)
    if key not in uniq: uniq[key] = i
tests = list(uniq.values())
if os.environ.get('DIFF_ONLY'): tests = [t for t in tests if os.environ['DIFF_ONLY'] in (t.mnemonic + ' ' + t.op_str)]
print('distinct instructions under test:', len(tests))

# ---- emit harness
em = rc.Emitter.__new__(rc.Emitter)
em.name = 't'; em.eps = [0]; em.region = False; em.body = {}; em.targets = set()
src = ['#include "../../runtime/rt.h"']
ok_tests = []
for k, i in enumerate(tests):
    try:
        lines, _ = em.insn(i)
    except Exception:
        continue
    if any('rt_unimpl' in l or 'goto' in l or 'return' in l for l in lines): continue
    src.append('static void t_%d(void) { %s }' % (len(ok_tests), ' '.join(lines)))
    ok_tests.append(i)
src.append('void (*const TESTS[])(void) = {%s};' % ','.join('t_%d' % k for k in range(len(ok_tests))))
src.append('const int NTESTS = %d;' % len(ok_tests))
tdir = os.path.join(ROOT, 'build', 'difftest'); os.makedirs(tdir, exist_ok=True)
open(os.path.join(tdir, 'tests.c'), 'w').write('\n'.join(src) + '\n')
open(os.path.join(tdir, 'harness.c'), 'w').write(r'''
#include "../../runtime/rt.h"
#include <stdio.h>
#include <stdlib.h>
extern void (*const TESTS[])(void); extern const int NTESTS;
uint32_t R[8]; uint16_t S[6]; uint32_t SB[6]; uint32_t DF, IF = 1; uint8_t *MEM;
uint32_t fl_op, fl_sz, fl_a, fl_b, fl_res, fl_c, fl_expl; double FPR[8]; uint32_t FPU_TOP, FPU_CW = 0x37f, FPU_CC;
volatile int irq_pending; uint32_t SH[16]; int SHP = 1;
static inline uint8_t pat(uint32_t a) { return (uint8_t)((a * 2654435761u) >> 24); }
uint8_t vga_read(uint32_t a) { return MEM[a]; } void vga_write(uint32_t a, uint8_t v) { MEM[a] = v; }
void set_sreg(int s, uint16_t sel) { S[s] = sel; } _Noreturn void rt_fatal(const char *f, ...) { printf("FATAL %s\n", f); exit(1); }
void rt_unimpl(uint32_t a, const char *w) { rt_fatal(w); }
#define WIN 4
#define WLEN 96
int main(void) {
    MEM = malloc(MEM_SIZE + 64);
    for (uint32_t a = 0; a < MEM_SIZE; a++) MEM[a] = pat(a);
    struct { uint32_t idx, r[8], fl, win[WIN]; } in;
    while (fread(&in, sizeof in, 1, stdin) == 1) {
        for (int k = 0; k < 8; k++) R[k] = in.r[k];
        set_eflags(in.fl); DF = 0;
        TESTS[in.idx]();
        uint32_t out[9]; for (int k = 0; k < 8; k++) out[k] = R[k]; out[8] = get_eflags();
        fwrite(out, 4, 9, stdout);
        for (int w = 0; w < WIN; w++) fwrite(MEM + in.win[w], 1, WLEN, stdout);
        for (int w = 0; w < WIN; w++) for (uint32_t a = in.win[w]; a < in.win[w] + WLEN; a++) MEM[a] = pat(a);
    }
    return 0;
}
''')
for f in ('rt_dispatch', ):
    pass
open(os.path.join(tdir, 'stubs.c'), 'w').write(r'''
#include "../../runtime/rt.h"
#include <stdlib.h>
void rt_dispatch(uint32_t a) { rt_fatal("dispatch"); } void rt_dispatch_tail(uint32_t a) { rt_fatal("dispatch"); }
void rt_ret_jump(uint32_t a) { rt_fatal("retjump"); } void rt_irq(void) {}
void rt_int(int n) {} uint32_t rt_in8(uint32_t p) { return 0; } uint32_t rt_in16(uint32_t p) { return 0; } uint32_t rt_in32(uint32_t p) { return 0; }
void rt_out8(uint32_t p, uint32_t v) {} void rt_out16(uint32_t p, uint32_t v) {} void rt_out32(uint32_t p, uint32_t v) {}
void rt_retf(int n) {} void rt_iret(void) {} void rt_farjmp(uint32_t o, uint16_t s) {} void rt_farcall(uint32_t o, uint16_t s, uint32_t r) {}
int rt_lsl(uint32_t s, uint32_t *o) { return 0; } int rt_lar(uint32_t s, uint32_t *o) { return 0; }
double fpu_round(double v) { return v; } double fpu_ld_f80(uint32_t a) { return 0; } void fpu_st_f80(uint32_t a, double v) {}
void fpu_init(void) {} void fpu_save(uint32_t a) {} void fpu_restore(uint32_t a) {}
''')
# the real string-op and mul/div helpers live in rt_core.c; compile it with stubs for the rest
exe = os.path.join(tdir, 'harness')
cmd = ['clang', '-O1', '-std=c11', '-D_DEFAULT_SOURCE', '-w', '-o', exe, os.path.join(tdir, 'harness.c'),
       os.path.join(tdir, 'tests.c'), os.path.join(tdir, 'core_subset.c')]
core = open(os.path.join(ROOT, 'runtime', 'rt_core.c')).read()
# extract only mul/div + string-op helpers from rt_core.c
start = core.index('// ---------------------------------------------------------------- mul / div')
end = core.index('// ---------------------------------------------------------------- x87 extras')
open(os.path.join(tdir, 'core_subset.c'), 'w').write('#include "../../runtime/rt.h"\n#include <stdlib.h>\n' + core[start:end] +
    open(os.path.join(tdir, 'stubs.c')).read().split('\n', 2)[2])
subprocess.check_call(cmd)

# ---- vectors
rnd = random.Random(1234)
MEMBASE = 0x200000
vec = []; meta = []
def ea_of(i, regs):
    for op in i.operands:
        if op.type == X86_OP_MEM:
            m = op.mem; e = m.disp
            if m.base: e += regs[rc.REGMAP[i.reg_name(m.base)][0]]
            if m.index: e += regs[rc.REGMAP[i.reg_name(m.index)][0]] * m.scale
            return e & 0xffffffff
    return None
for k, i in enumerate(ok_tests):
    for run in range(RUNS):
        regs = [rnd.choice([rnd.getrandbits(32), rnd.getrandbits(8), rnd.getrandbits(16), 0, 0xffffffff, 0x80000000, 0x7fffffff, 1])
                for _ in range(8)]
        for op in i.operands:
            if op.type == X86_OP_MEM:
                if op.mem.base: regs[rc.REGMAP[i.reg_name(op.mem.base)][0]] = MEMBASE + rnd.randrange(0, 0x40000)
                if op.mem.index: regs[rc.REGMAP[i.reg_name(op.mem.index)][0]] = rnd.randrange(0, 0x400)
        regs[4] = 0x300000 + rnd.randrange(0, 64) * 4
        regs[6] = 0x220000 + rnd.randrange(0, 256); regs[7] = 0x240000 + rnd.randrange(0, 256)
        if i.mnemonic.startswith('rep'): regs[1] = rnd.randrange(0, 12)
        if any(op.type == X86_OP_REG and i.reg_name(op.reg) == 'cl' for op in i.operands[1:]):
            regs[1] = (regs[1] & ~0xff) | rnd.choice([0, 1, 2, 7, 8, 15, 16, 31, 33, rnd.randrange(256)])
        div_like = i.mnemonic in ('div', 'idiv')
        if div_like:   # avoid #DE: small dividend
            regs[2] = 0 if i.mnemonic == 'div' else (0xffffffff if regs[0] & 0x80000000 else 0)
            if i.operands[0].size == 1: regs[0] &= 0xff
            if i.operands[0].size == 2: regs[2] = 0; regs[0] &= 0xffff
            op0 = i.operands[0]
            if op0.type == X86_OP_REG:
                r = rc.REGMAP[i.reg_name(op0.reg)][0]
                if r not in (0, 2): regs[r] = (regs[r] & ~0xff) | rnd.randrange(1, 256)
                else: continue
        fl = 0x202 | (rnd.getrandbits(12) & FLAGMASK)
        e = ea_of(i, regs)
        if e is not None and (0xA0000 <= e < 0xB0000 or e >= rc.REBASE * 64 - 64): continue
        wins = [regs[4] - 48, regs[6] - 16, regs[7] - 16, (e - 16) if e is not None else 0x100000]
        wins = [w & 0xffffffff for w in wins]
        if any(w + 96 > (64 << 20) for w in wins): continue
        vec.append(struct.pack('<14I', k, *regs, fl, *wins)); meta.append((k, regs, fl, wins))
print('vectors:', len(vec))
# ---- unicorn reference
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0, 64 << 20)
pat = bytes(((a * 2654435761) >> 24) & 0xff for a in range(1 << 16))
# pattern is not periodic in 64K; generate per window lazily
def patwin(a, n): return bytes((((x * 2654435761) & 0xffffffff) >> 24) for x in range(a, a + n))
# fill full memory pattern (needed for absolute displacements); do it in chunks
CH = 1 << 20
for base in range(0, 64 << 20, CH):
    uc.mem_write(base, patwin(base, CH))
CODE = (64 << 20) - 0x1000
UREGS = [UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EBX, UC_X86_REG_ESP, UC_X86_REG_EBP, UC_X86_REG_ESI, UC_X86_REG_EDI]

wr_out = [False]
cur_wins = [[]]
def on_write(uc_, access, addr, size, value, ud):
    for w in cur_wins[0]:
        if w <= addr and addr + size <= w + 96: return
    wr_out[0] = True
uc.hook_add(UC_HOOK_MEM_WRITE, on_write)
ures = []; keep_vec = []; keep_meta = []
for n, (k, regs, fl, wins) in enumerate(meta):
    i = ok_tests[k]
    uc.mem_write(CODE, bytes(i.bytes))
    for r, v in zip(UREGS, regs): uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_EFLAGS, fl)
    cur_wins[0] = wins; wr_out[0] = False
    try:
        uc.emu_start(CODE, CODE + i.size, timeout=100000)
        bad = False
    except UcError:
        bad = True
    ur = [uc.reg_read(r) for r in UREGS]; uf = uc.reg_read(UC_X86_REG_EFLAGS)
    uw = [bytes(uc.mem_read(w, 96)) for w in wins]
    for w in wins: uc.mem_write(w, patwin(w, 96))
    if bad or wr_out[0]:
        if wr_out[0]:   # can't restore: rebuild memory around it
            for base in range(0, 64 << 20, CH): uc.mem_write(base, patwin(base, CH))
        continue
    ures.append((ur, uf, uw)); keep_vec.append(vec[n]); keep_meta.append(meta[n])
print('vectors kept:', len(keep_vec), 'of', len(vec))
vec, meta = keep_vec, keep_meta
pr = subprocess.run([exe], input=b''.join(vec), capture_output=True); res = pr.stdout
if pr.returncode: print('harness rc', pr.returncode, 'last test', meta[len(res)//(36+4*96)][0], ok_tests[meta[len(res)//(36+4*96)][0]].mnemonic, ok_tests[meta[len(res)//(36+4*96)][0]].op_str, [hex(x) for x in meta[len(res)//(36+4*96)][1]]); print(res[-200:].split(b'FATAL')[-1][:100] if b'FATAL' in res else '')
RECSZ = 36 + 4 * 96

fails = collections.defaultdict(list)
for n, (k, regs, fl, wins) in enumerate(meta):
    i = ok_tests[k]
    ur, uf, uw = ures[n]
    rec = res[n * RECSZ:(n + 1) * RECSZ]
    if len(rec) < RECSZ: print('harness output truncated at', n); break
    cr = list(struct.unpack_from('<8I', rec)); cf = struct.unpack_from('<I', rec, 32)[0]
    cw = [rec[36 + 96 * j:36 + 96 * (j + 1)] for j in range(4)]
    mask = FLAGMASK & ~UNDEF.get(i.mnemonic.split()[-1], 0)
    if i.mnemonic in ('shl', 'sal', 'shr', 'sar', 'rol', 'ror', 'rcl', 'rcr', 'shld', 'shrd'):
        cnt = (i.operands[-1].imm if i.operands[-1].type == X86_OP_IMM else (regs[1] & 0xff)) if len(i.operands) > 1 else 1
        if (cnt & 31) == 0: mask = FLAGMASK
        elif (cnt & 31) == 1: mask |= 0x800 if i.mnemonic not in ('shld', 'shrd') else 0
    diffs = []
    for j in range(8):
        if cr[j] != ur[j]: diffs.append('%s c=%08x u=%08x' % (rc.REGS32[j], cr[j], ur[j]))
    if (cf ^ uf) & mask: diffs.append('flags c=%03x u=%03x (mask %03x)' % (cf & mask, uf & mask, mask))
    for j in range(4):
        if cw[j] != uw[j]: diffs.append('mem@%08x' % wins[j])
    if diffs: fails[(i.mnemonic, i.op_str)].append((regs, fl, diffs))
print('instructions with mismatches: %d / %d' % (len(fails), len(ok_tests)))
by_mn = collections.Counter(m for m, _ in fails)
print('by mnemonic:', by_mn.most_common(30))
for (m, o), lst in list(fails.items())[:60]:
    regs, fl, d = lst[0]
    print('  %-8s %-40s %s' % (m, o, '; '.join(d)[:150]))
