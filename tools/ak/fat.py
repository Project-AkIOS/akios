"""fat.py: a FAT16 image from a set of files, with long names -- the ESP ak puts in the boot payload (and later on
the phone's own ESP). No host tools needed (no mtools, no loop mounts).

    make_fat({'EFI/UniXNU/akios.toml': b'...', 'EFI/akios/kernel': b'...'}, label='AKIOS') -> bytes

FAT16 by the specification's rule: the type is the cluster count (4085..65524), so the cluster size is chosen to
land there; 512-byte sectors, two FATs, a 512-entry root directory.
"""
import struct

SECTOR = 512
ROOT_ENTRIES = 512


def _short_name(name, taken):
    """an 8.3 name for NAME: itself when it is one, else BASIS~N; and whether a long name is needed"""
    up = name.upper()
    base, _, ext = up.rpartition('.') if '.' in up[1:] else (up, '', '')
    ok = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$%\'-_@~`!(){}^#&')
    if name == up and 0 < len(base) <= 8 and len(ext) <= 3 and all(c in ok for c in base + ext) and up.count('.') <= 1:
        short = (base.ljust(8) + ext.ljust(3)).encode()
        if short not in taken:
            return short, False
    clean = lambda s: ''.join(c for c in s if c in ok)
    b, e = clean(base)[:6] or 'FILE', clean(ext)[:3]
    for n in range(1, 1000000):
        tail = '~%d' % n
        short = ((b[:8 - len(tail)] + tail).ljust(8) + e.ljust(3)).encode()
        if short not in taken:
            return short, True
    raise ValueError('no short name for ' + name)


def _checksum(short):
    s = 0
    for c in short:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xff
    return s


def _entries(name, short, needs_long, attr, cluster, size):
    """the directory entries for one name: long-name slots (last first), then the 8.3 entry"""
    out = b''
    if needs_long:
        units = list(name.encode('utf-16-le'))
        chars = [name[i] for i in range(len(name))]
        u = [ord(c) for c in chars] + [0]
        u += [0xffff] * (-len(u) % 13)
        slots = [u[i:i + 13] for i in range(0, len(u), 13)]
        ck = _checksum(short)
        for i in range(len(slots), 0, -1):
            s = slots[i - 1]
            seq = i | (0x40 if i == len(slots) else 0)
            out += struct.pack('<B10sBBB12sH4s', seq, struct.pack('<5H', *s[0:5]), 0x0f, 0, ck,
                               struct.pack('<6H', *s[5:11]), 0, struct.pack('<2H', *s[11:13]))
        del units
    # 2026-01-01 00:00 as the time stamps: reproducible images
    date = ((2026 - 1980) << 9) | (1 << 5) | 1
    out += struct.pack('<11sBBBHHHHHHHI', short, attr, 0, 0, 0, date, date, 0, 0, date, cluster, size)
    return out


def make_fat(files, label='AKIOS', size=None):
    # the tree: dirs map name -> dict, files name -> bytes
    tree = {}
    for path, data in files.items():
        parts = [p for p in path.replace('\\', '/').split('/') if p]
        d = tree
        for p in parts[:-1]:
            d = d.setdefault(p, {})
        d[parts[-1]] = bytes(data)

    payload = sum(len(v) for v in files.values())
    total = size or max(4 << 20, int(payload * 1.08) + (1 << 20))
    total = (total + SECTOR - 1) // SECTOR * SECTOR
    sectors = total // SECTOR
    for spc in (1, 2, 4, 8, 16, 32, 64):
        reserved, root_secs = 1, ROOT_ENTRIES * 32 // SECTOR
        fat_secs = 1
        for _ in range(8):   # the FAT's size and the cluster count depend on each other: settle
            clusters = (sectors - reserved - 2 * fat_secs - root_secs) // spc
            fat_secs = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR
        clusters = (sectors - reserved - 2 * fat_secs - root_secs) // spc
        if 4085 <= clusters <= 65524:
            break
    else:
        raise ValueError('no FAT16 geometry for %d bytes' % total)
    csize = spc * SECTOR
    data_start = reserved + 2 * fat_secs + root_secs
    fat = [0xfff8, 0xffff] + [0] * clusters
    heap = {}            # cluster -> bytes
    nxt = [2]

    def alloc(data):
        n = max(1, (len(data) + csize - 1) // csize) if data else 0
        if n == 0:
            return 0
        first = nxt[0]
        if first + n - 2 > clusters:
            raise ValueError('the image is too small for its files')
        for i in range(n):
            c = first + i
            fat[c] = c + 1 if i < n - 1 else 0xffff
            heap[c] = data[i * csize:(i + 1) * csize]
        nxt[0] += n
        return first

    def write_dir(d, self_cluster, parent_cluster):
        """the entries of directory D (its subdirectories written first, depth first)"""
        taken, body = set(), b''
        if self_cluster is not None:
            body += _entries('.', b'.          ', False, 0x10, self_cluster, 0)
            body += _entries('..', b'..         ', False, 0x10, parent_cluster, 0)
        for name in sorted(d):
            v = d[name]
            short, long_ = _short_name(name, taken)
            taken.add(short)
            if isinstance(v, dict):
                # reserve the subdirectory's clusters first, so its entries can name itself
                size_guess = (len(v) + 2) * 32 * 21
                n = max(1, (size_guess + csize - 1) // csize)
                first = nxt[0]
                for i in range(n):
                    fat[first + i] = first + i + 1 if i < n - 1 else 0xffff
                nxt[0] += n
                sub = write_dir(v, first, self_cluster or 0)
                if len(sub) > n * csize:
                    raise ValueError('directory %s too large' % name)
                for i in range(n):
                    heap[first + i] = sub[i * csize:(i + 1) * csize]
                body += _entries(name, short, long_, 0x10, first, 0)
            else:
                body += _entries(name, short, long_, 0x20, alloc(v), len(v))
        return body

    root = _entries(label, label.upper()[:11].ljust(11).encode(), False, 0x08, 0, 0) + write_dir(tree, None, 0)
    if len(root) > ROOT_ENTRIES * 32:
        raise ValueError('the root directory has too many entries')

    img = bytearray(total)
    bs = struct.pack('<3s8sHBHBHHBHHHII', b'\xeb\x3c\x90', b'AKIOS   ', SECTOR, spc, reserved, 2, ROOT_ENTRIES,
                     sectors if sectors < 65536 else 0, 0xf8, fat_secs, 63, 255, 0, sectors if sectors >= 65536 else 0)
    bs += struct.pack('<BBBI11s8s', 0x80, 0, 0x29, 0x414b494f, label.upper()[:11].ljust(11).encode(), b'FAT16   ')
    img[0:len(bs)] = bs
    img[510:512] = b'\x55\xaa'
    fat_bytes = struct.pack('<%dH' % len(fat), *fat)
    for k in range(2):
        o = (reserved + k * fat_secs) * SECTOR
        img[o:o + len(fat_bytes)] = fat_bytes
    o = (reserved + 2 * fat_secs) * SECTOR
    img[o:o + len(root)] = root
    for c, data in heap.items():
        o = (data_start + (c - 2) * spc) * SECTOR
        img[o:o + len(data)] = data
    return bytes(img)


if __name__ == '__main__':
    import sys
    out, pairs = sys.argv[1], sys.argv[2:]
    files = dict(p.split('=', 1) for p in pairs)
    open(out, 'wb').write(make_fat({k: open(v, 'rb').read() for k, v in files.items()}))
