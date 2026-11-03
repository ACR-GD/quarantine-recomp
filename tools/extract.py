#!/usr/bin/env python3
"""Extract the Quarantine CD image into the layout the runtime expects.

    python3 tools/extract.py [--exe-only] <image> [outdir]     (outdir defaults to "extracted")

--exe-only writes just outdir/cd/SKIPIT.EXE, which is all the build needs; the game itself reads the
disc image directly (choose it in the launcher).

<image> is either a Nero image (.nrg) or a cue sheet (.cue, with its .bin file(s)).
Produces:
    outdir/cd/...            files of the data track (ISO 9660)
    outdir/track01.iso       the data track itself (2048-byte sectors)
    outdir/trackNN.wav       CD-audio tracks (44.1 kHz, 16-bit stereo)

Only the standard library is used.
"""
import array, os, re, struct, sys, wave

EXE_ONLY = False


# ---------------------------------------------------------------- ISO 9660
def extract_iso(iso_path, out):
    with open(iso_path, 'rb') as f:
        d = f.read()
    if d[0x8001:0x8006] != b'CD001':
        sys.exit('%s: no ISO 9660 volume descriptor' % iso_path)
    count = 0

    def walk(lba, length, path):
        nonlocal count
        os.makedirs(path, exist_ok=True)
        buf = d[lba * 2048:lba * 2048 + length]
        i = 0
        while i < len(buf):
            rl = buf[i]
            if rl == 0:                          # records never cross a sector: skip to the next one
                i = (i // 2048 + 1) * 2048
                continue
            r = buf[i:i + rl]
            i += rl
            ext, size, flags = struct.unpack_from('<I', r, 2)[0], struct.unpack_from('<I', r, 10)[0], r[25]
            name = r[33:33 + r[32]]
            if name in (b'\x00', b'\x01'):       # "." and ".."
                continue
            name = name.decode('ascii', 'replace').split(';')[0].rstrip('.')
            p = os.path.join(path, name)
            if EXE_ONLY and not (flags & 2) and name.upper() != 'SKIPIT.EXE':
                continue
            if flags & 2:
                walk(ext, size, p)
            else:
                with open(p, 'wb') as o:
                    o.write(d[ext * 2048:ext * 2048 + size])
                count += 1

    root = d[0x8000 + 156:0x8000 + 190]
    walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], os.path.join(out, 'cd'))
    print('data track: %d files -> %s' % (count, os.path.join(out, 'cd')))


# ---------------------------------------------------------------- helpers
def roughness(pcm, swap):
    """Sum of sample-to-sample differences (left channel); smaller = right byte order."""
    a = array.array('h', pcm[:len(pcm) // 4 * 4])
    if swap:
        a.byteswap()
    return sum(abs(a[i] - a[i - 2]) for i in range(2, len(a), 2))


def copy_audio(f, start, nbytes, wav_path, swap):
    if EXE_ONLY:
        return
    w = wave.open(wav_path, 'wb')
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
    f.seek(start)
    while nbytes > 0:
        c = f.read(min(nbytes, 2352 * 1000))
        if not c:
            break
        nbytes -= len(c)
        if swap:
            a = array.array('h', c); a.byteswap(); c = a.tobytes()
        w.writeframes(c)
    w.close()


def copy_data(f, start, nsectors, sector_size, data_off, iso_path):
    """Write the 2048-byte user data of each sector."""
    with open(iso_path, 'wb') as o:
        f.seek(start)
        for _ in range(nsectors):
            s = f.read(sector_size)
            if len(s) < sector_size:
                break
            o.write(s[data_off:data_off + 2048])


# ---------------------------------------------------------------- Nero (.nrg)
def extract_nrg(src, out):
    f = open(src, 'rb')
    f.seek(-12, 2)
    tag = f.read(4)
    if tag == b'NER5':
        off = struct.unpack('>Q', f.read(8))[0]
    else:
        f.seek(-8, 2)
        if f.read(4) != b'NERO':
            sys.exit('%s: not a Nero image (no NER5/NERO footer)' % src)
        off = struct.unpack('>I', f.read(4))[0]
    v2 = tag == b'NER5'
    f.seek(off)
    tracks = []
    while True:
        cid = f.read(4)
        ln = struct.unpack('>I', f.read(4))[0]
        body = f.read(ln)
        if cid in (b'END!', b''):
            break
        if cid in (b'DAOX', b'DAOI'):
            first, last = body[20], body[21]
            p, esz = 22, (42 if cid == b'DAOX' else 30)
            for t in range(first, last + 1):
                ssz, mode = struct.unpack('>HI', body[p + 12:p + 18])
                if cid == b'DAOX':
                    i0, i1, end = struct.unpack('>QQQ', body[p + 18:p + 42])
                else:
                    i0, i1, end = struct.unpack('>III', body[p + 18:p + 30])
                tracks.append((t, ssz, mode, i1, end))
                p += esz
    if not tracks:
        sys.exit('%s: no disc-at-once track table found' % src)
    os.makedirs(out, exist_ok=True)
    for t, ssz, mode, i1, end in tracks:
        n = end - i1
        if ssz == 2048:                          # cooked data track
            iso = os.path.join(out, 'track%02d.iso' % t)
            f.seek(i1)
            with open(iso, 'wb') as o:
                while n:
                    c = f.read(min(n, 1 << 24)); o.write(c); n -= len(c)
            extract_iso(iso, out)
            if EXE_ONLY:
                os.remove(iso)
        elif ssz == 2352:
            # audio; some Nero versions store it big-endian, so pick the byte order that sounds smooth
            f.seek(i1 + n // 2)
            probe = f.read(2352 * 50)
            swap = roughness(probe, True) < roughness(probe, False)
            copy_audio(f, i1, n, os.path.join(out, 'track%02d.wav' % t), swap)
            if not EXE_ONLY:
                print('track %02d: audio, %d sectors%s' % (t, n // ssz, ' (byte-swapped)' if swap else ''))
        else:
            sys.exit('track %d: unsupported sector size %d / mode %08x' % (t, ssz, mode))
    f.close()


# ---------------------------------------------------------------- cue/bin
def msf(s):
    m, sec, fr = (int(x) for x in s.split(':'))
    return (m * 60 + sec) * 75 + fr


def extract_cue(cue, out):
    base = os.path.dirname(os.path.abspath(cue))
    tracks, cur_file = [], None
    for line in open(cue, encoding='latin-1'):
        line = line.strip()
        m = re.match(r'FILE\s+"?(.+?)"?\s+(\w+)$', line, re.I)
        if m:
            cur_file = os.path.join(base, m.group(1))
            continue
        m = re.match(r'TRACK\s+(\d+)\s+(\S+)', line, re.I)
        if m:
            tracks.append({'n': int(m.group(1)), 'type': m.group(2).upper(), 'file': cur_file, 'idx': {}})
            continue
        m = re.match(r'INDEX\s+(\d+)\s+(\d+:\d+:\d+)', line, re.I)
        if m and tracks:
            tracks[-1]['idx'][int(m.group(1))] = msf(m.group(2))
    if not tracks:
        sys.exit('%s: no tracks' % cue)
    os.makedirs(out, exist_ok=True)
    for k, t in enumerate(tracks):
        ssz = 2352 if t['type'] in ('AUDIO', 'MODE1/2352', 'MODE2/2352') else 2048 if t['type'] == 'MODE1/2048' else None
        if ssz is None:
            sys.exit('track %d: unsupported type %s' % (t['n'], t['type']))
        start = t['idx'].get(1, t['idx'].get(0, 0))
        # the track ends where the next one (in the same file) begins, else at the end of the file
        nxt = tracks[k + 1] if k + 1 < len(tracks) else None
        if nxt and nxt['file'] == t['file']:
            end = nxt['idx'].get(0, nxt['idx'].get(1))
        else:
            end = os.path.getsize(t['file']) // ssz
        f = open(t['file'], 'rb')
        if t['type'] == 'AUDIO':
            copy_audio(f, start * ssz, (end - start) * ssz, os.path.join(out, 'track%02d.wav' % t['n']), False)
            if not EXE_ONLY:
                print('track %02d: audio, %d sectors' % (t['n'], end - start))
        else:
            iso = os.path.join(out, 'track%02d.iso' % t['n'])
            data_off = {2048: 0, 2352: 16 if t['type'] == 'MODE1/2352' else 24}[ssz]
            copy_data(f, start * ssz, end - start, ssz, data_off, iso)
            extract_iso(iso, out)
            if EXE_ONLY:
                os.remove(iso)
        f.close()


def main():
    global EXE_ONLY
    args = sys.argv[1:]
    if args and args[0] == '--exe-only':
        EXE_ONLY = True
        args = args[1:]
    if len(args) not in (1, 2):
        sys.exit(__doc__)
    src = args[0]
    out = args[1] if len(args) == 2 else 'extracted'
    ext = os.path.splitext(src)[1].lower()
    if ext == '.cue':
        extract_cue(src, out)
    elif ext == '.nrg':
        extract_nrg(src, out)
    else:
        sys.exit('unsupported image type %r (use .nrg or .cue/.bin)' % ext)
    exe = os.path.join(out, 'cd', 'SKIPIT.EXE')
    if not os.path.exists(exe):
        print('warning: %s not found; is this the Quarantine CD?' % exe)
    waves = [n for n in os.listdir(out) if re.match(r'track\d+\.wav$', n)]
    print('done: %d audio tracks, game files in %s' % (len(waves), os.path.join(out, 'cd')))


if __name__ == '__main__':
    main()
