"""Resolve port numbers and DOS/DPMI function numbers used by the game."""
import sys, pickle, re
from collections import Counter, defaultdict
from capstone import *
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from le import LE
le = LE(sys.argv[1]); s = pickle.load(open(sys.argv[2], 'rb'))
code = le.objs[0]; cb = code['base']; mem = bytes(code['data'][:code['vsize']])
md = Cs(CS_ARCH_X86, CS_MODE_32)
addrs = s['insns']; idx = {a: k for k, a in enumerate(addrs)}
def dis(a): return next(md.disasm(mem[a-cb:a-cb+16], a))
ports = defaultdict(set); svc = defaultdict(Counter)
for k, a in enumerate(addrs):
    i = dis(a)
    if i.mnemonic in ('in', 'out') and 'dx' in i.op_str:
        for j in range(1, 12):
            if k-j < 0: break
            p = dis(addrs[k-j])
            mm = re.match(r'(e?dx|dl), (0x[0-9a-f]+|\d+)$', p.op_str)
            if p.mnemonic == 'mov' and mm: ports[int(mm.group(2), 0)].add(i.mnemonic); break
            if p.mnemonic in ('add', 'inc', 'dec', 'sub') and 'dx' in p.op_str: ports['dx-relative'].add(i.mnemonic); break
        else: ports['unknown'].add(i.mnemonic)
    if i.mnemonic == 'int' and i.op_str in ('0x21', '0x31', '0x2f', '0x10', '0x33', '0x16'):
        for j in range(1, 10):
            p = dis(addrs[k-j]); mm = re.match(r'(e?ax|ah), (0x[0-9a-f]+|\d+)$', p.op_str)
            if p.mnemonic == 'mov' and mm:
                v = int(mm.group(2), 0); svc[i.op_str][hex(v >> 8 if mm.group(1) == 'ah' or (i.op_str == '0x21' and v > 0xff) else v)] += 1; break
        else: svc[i.op_str]['?'] += 1
for p in sorted(ports, key=str): print('port', hex(p) if isinstance(p, int) else p, sorted(ports[p]))
for k, v in svc.items(): print('int', k, dict(v))
# Strings that hint at drivers / video modes
import re as r2
d3 = bytes(le.objs[2]['data'])
for m in r2.finditer(rb'[\x20-\x7e]{5,}', d3):
    t = m.group().decode()
    if r2.search(r'(?i)vesa|vga|mode|blaster|midi|hmi|joy|mouse|cd-?rom|mscdex|timer|xms|ems|svga|640|320', t): print('str', hex(0x80000+m.start()), t)
