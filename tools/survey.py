"""Recursive-descent survey of an LE executable: coverage, FPU use,
port I/O, interrupts, indirect branches, writes into code."""
import sys, struct
from collections import Counter, defaultdict
from capstone import *
from capstone.x86 import *
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from le import LE

le = LE(sys.argv[1])
code = le.objs[0]; cb, ce = code['base'], code['base'] + code['vsize']
data = le.objs[2]
md = Cs(CS_ARCH_X86, CS_MODE_32); md.detail = True
mem = bytes(code['data'][:code['vsize']])
def incode(a): return cb <= a < ce

# seeds: entry + every 32-bit fixup whose target lands in the code object
seeds = {le.entry()}
ptr_in_data = set()
for (sobj, pos, styp, tobj, tgt) in le.fixups:
    if tobj == 1 and styp in (7,):
        a = cb + tgt
        if sobj == 3: ptr_in_data.add(a)
        seeds.add(a)
calls = set(); visited = {}; insns = {}
work = list(seeds); jumptables = 0
stats = Counter(); ports = Counter(); ints = Counter(); fpu = Counter()
indirect = Counter(); codewrites = []
while work:
    a = work.pop()
    while incode(a) and a not in insns:
        try: i = next(md.disasm(mem[a-cb:a-cb+16], a))
        except StopIteration: stats['bad'] += 1; break
        insns[a] = i
        g = set(i.groups); m = i.mnemonic
        if X86_GRP_FPU in g or m.startswith('f'): fpu[m] += 1
        if m in ('in','out','ins','outs','insb','insw','insd','outsb','outsw','outsd'):
            ports[i.op_str] += 1
        if m == 'int': ints[i.op_str] += 1
        # writes to code addresses
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 and incode(op.mem.disp & 0xffffffff) \
               and (op.access & CS_AC_WRITE):
                codewrites.append((a, i.mnemonic, i.op_str))
        nxt = a + i.size
        if X86_GRP_JUMP in g or X86_GRP_CALL in g:
            op = i.operands[0]
            if op.type == X86_OP_IMM:
                t = op.imm & 0xffffffff
                if X86_GRP_CALL in g: calls.add(t)
                work.append(t)
            else:
                kind = 'call' if X86_GRP_CALL in g else 'jmp'
                if op.type == X86_OP_MEM and op.mem.index and op.mem.scale == 4 and incode(op.mem.disp & 0xffffffff):
                    jumptables += 1; kind += '-table'
                    # walk table entries while they look like code pointers
                    t = op.mem.disp & 0xffffffff
                    while incode(t):
                        e = struct.unpack_from('<I', mem, t-cb)[0]
                        if not incode(e): break
                        work.append(e); t += 4
                indirect[kind + (' mem' if op.type == X86_OP_MEM else ' reg')] += 1
            if m == 'jmp': break
        if m in ('ret', 'retf', 'iret', 'iretd', 'hlt'): break
        a = nxt
covered = sum(i.size for i in insns.values())
print('code object %d bytes; decoded %d insns covering %d bytes (%.1f%%)' % (code['vsize'], len(insns), covered, 100*covered/code['vsize']))
print('direct call targets (functions):', len(calls), ' code ptrs stored in data:', len(ptr_in_data), ' jump tables:', jumptables)
print('indirect branches:', dict(indirect))
print('FPU insns:', sum(fpu.values()), fpu.most_common(12))
print('port I/O:', ports.most_common(30))
print('int:', ints.most_common())
print('writes to absolute code addresses:', len(codewrites)); [print('  %08x %s %s' % w) for w in codewrites[:20]]
print('decode errors:', stats['bad'])
# uncovered gaps
gaps = []; a = cb
cov = bytearray(code['vsize'])
for i in insns.values(): cov[i.address-cb:i.address-cb+i.size] = b'\1'*i.size
run = 0
for k, c in enumerate(cov):
    if not c: run += 1
    else:
        if run >= 64: gaps.append((cb+k-run, run))
        run = 0
print('uncovered gaps >=64 bytes:', len(gaps), 'total', sum(g[1] for g in gaps))
for g in sorted(gaps, key=lambda x: -x[1])[:10]: print('  %08x len %d' % g)
import pickle
pickle.dump(dict(calls=sorted(calls), insns=sorted(insns)), open(sys.argv[2], 'wb')) if len(sys.argv) > 2 else None
