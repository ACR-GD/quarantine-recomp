"""Minimal LE (DOS/4GW) loader: parses header, objects, pages and fixups;
produces a relocated flat image with objects at their preferred bases."""
import struct, sys

class LE:
    def __init__(self, path, rebase=0):
        d = self.raw = open(path, 'rb').read()
        o = struct.unpack_from('<I', d, 0x3c)[0]
        if d[o:o+2] != b'LE':
            o = d.find(b'LE\x00\x00')
        self.off = o
        h = lambda f, x: struct.unpack_from(f, d, o + x)
        self.cpu, self.ostype = h('<HH', 0x08)
        self.npages = h('<I', 0x14)[0]
        self.eip_obj, self.eip = h('<II', 0x18)
        self.esp_obj, self.esp = h('<II', 0x20)
        self.pagesize, self.lastpage = h('<II', 0x28)
        self.objtab, self.nobj, self.pagemap = h('<III', 0x40)
        self.fixpage, self.fixrec = h('<II', 0x68)
        self.datapages = h('<I', 0x80)[0]
        self.objs = []
        for i in range(self.nobj):
            vsize, base, flags, pidx, pcnt, _ = struct.unpack_from('<6I', d, o + self.objtab + i*24)
            self.objs.append(dict(n=i+1, vsize=vsize, base=base + rebase, flags=flags, pidx=pidx, pcnt=pcnt))
        self.mem = {}
        for ob in self.objs:
            buf = bytearray(ob['vsize'] + 0x1000)
            for k in range(ob['pcnt']):
                p = ob['pidx'] + k  # 1-based page
                start = self.datapages + (p-1)*self.pagesize
                ln = self.lastpage if p == self.npages else self.pagesize
                buf[k*self.pagesize:k*self.pagesize+ln] = d[start:start+ln]
            ob['data'] = buf
        self.fixups = []
        self._fix()

    def obj_of_page(self, p):
        for ob in self.objs:
            if ob['pidx'] <= p < ob['pidx'] + ob['pcnt']: return ob

    def _fix(self):
        d, o = self.raw, self.off
        pt = [struct.unpack_from('<I', d, o + self.fixpage + 4*i)[0] for i in range(self.npages+1)]
        for p in range(self.npages):
            ob = self.obj_of_page(p+1)
            pbase = (p + 1 - ob['pidx']) * self.pagesize
            i, end = o + self.fixrec + pt[p], o + self.fixrec + pt[p+1]
            while i < end:
                src, flg = d[i], d[i+1]; i += 2
                cnt = None
                if src & 0x20: cnt = d[i]; i += 1
                else: srcoff = struct.unpack_from('<h', d, i)[0]; i += 2
                if flg & 3 != 0: raise ValueError('only internal fixups supported, flags %x' % flg)
                if flg & 0x40: tobj = struct.unpack_from('<H', d, i)[0]; i += 2
                else: tobj = d[i]; i += 1
                styp = src & 0xf
                tgt = 0
                if styp != 2:
                    if flg & 0x10: tgt = struct.unpack_from('<I', d, i)[0]; i += 4
                    else: tgt = struct.unpack_from('<H', d, i)[0]; i += 2
                offs = [srcoff] if cnt is None else []
                if cnt is not None:
                    for _ in range(cnt): offs.append(struct.unpack_from('<h', d, i)[0]); i += 2
                tb = self.objs[tobj-1]['base']
                for so in offs:
                    pos = pbase + so
                    self.fixups.append((ob['n'], pos, styp, tobj, tgt))
                    if pos < 0: continue
                    buf = ob['data']
                    if styp == 7:   # 32-bit offset
                        struct.pack_into('<I', buf, pos, tb + tgt)
                    elif styp == 8: # 32-bit self-relative
                        struct.pack_into('<I', buf, pos, (tb + tgt - (ob['base'] + pos + 4)) & 0xffffffff)
                    elif styp == 5: # 16-bit offset
                        struct.pack_into('<H', buf, pos, (tb + tgt) & 0xffff)
                    elif styp == 2: # 16-bit selector
                        struct.pack_into('<H', buf, pos, tobj)
                    elif styp == 6: # 16:32 pointer
                        struct.pack_into('<IH', buf, pos, tb + tgt, tobj)
                    elif styp == 3: # 16:16 pointer
                        struct.pack_into('<HH', buf, pos, (tb+tgt) & 0xffff, tobj)

    def read(self, addr, n):
        for ob in self.objs:
            if ob['base'] <= addr < ob['base'] + ob['vsize']:
                k = addr - ob['base']; return bytes(ob['data'][k:k+n])
        return None

    def entry(self):
        return self.objs[self.eip_obj-1]['base'] + self.eip

if __name__ == '__main__':
    le = LE(sys.argv[1])
    print('LE at 0x%x cpu %d pages %d pagesize %d' % (le.off, le.cpu, le.npages, le.pagesize))
    for ob in le.objs:
        f = ob['flags']
        print(' obj %d base %08x vsize %8x flags %04x %s%s%s%s pages %d' % (ob['n'], ob['base'], ob['vsize'], f,
              'R' if f&1 else '-', 'W' if f&2 else '-', 'X' if f&4 else '-', ' 32bit' if f&0x2000 else '', ob['pcnt']))
    print('entry %08x  stack obj %d esp %x' % (le.entry(), le.esp_obj, le.esp))
    from collections import Counter
    print('fixups', len(le.fixups), Counter(f[2] for f in le.fixups))
    if len(sys.argv) > 2:
        for ob in le.objs:
            open('%s.obj%d_%08x.bin' % (sys.argv[2], ob['n'], ob['base']), 'wb').write(ob['data'][:ob['vsize']])
