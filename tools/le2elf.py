"""Wrap relocated LE objects in a minimal ELF32/i386 so Ghidra can import it."""
import sys, struct
sys.path.insert(0, __file__.rsplit('/', 1)[0])
from le import LE
le = LE(sys.argv[1]); out = sys.argv[2]
objs = le.objs; n = len(objs)
hdr = 52 + 32*n; off = (hdr + 0xfff) & ~0xfff
phs, blobs = [], []
for ob in objs:
    data = bytes(ob['data'][:ob['vsize']])
    f = ob['flags']; pf = (4 if f&1 else 0) | (2 if f&2 else 0) | (1 if f&4 else 0)
    phs.append(struct.pack('<8I', 1, off, ob['base'], ob['base'], len(data), ob['vsize'], pf, 0x1000))
    blobs.append((off, data)); off = (off + len(data) + 0xfff) & ~0xfff
eh = b'\x7fELF\x01\x01\x01' + b'\0'*9 + struct.pack('<HHIIIIIHHHHHH', 2, 3, 1, le.entry(), 52, 0, 0, 52, 32, n, 40, 0, 0)
buf = bytearray(off); buf[:52] = eh; buf[52:52+32*n] = b''.join(phs)
for o, d in blobs: buf[o:o+len(d)] = d
open(out, 'wb').write(buf); print('wrote', out, len(buf))
