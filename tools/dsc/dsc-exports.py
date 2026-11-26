#!/usr/bin/env python3
"""Exported symbols of one library in a dyld shared cache, from its export trie -- without the cache's .symbols file
(which sysimage/unpack.sh leaves out). The addresses are the cache's own, as tools/dsc-disasm.py takes them.

usage: dsc-exports.py CACHE_DIR IMAGE [REGEX]      IMAGE: a path's last component or a regex on the path

Nothing of Apple's is kept: it reads the cache you extracted from your own IPSW.
"""
import glob
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
Cache = __import__('dsc-disasm').Cache

LC_SEGMENT_64, LC_DYLD_INFO_ONLY, LC_DYLD_EXPORTS_TRIE = 0x19, 0x80000022, 0x80000033


def uleb(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1
        v |= (c & 0x7f) << s; s += 7
        if c < 0x80:
            return v, i


def images(directory):
    main = sorted(p for p in glob.glob(os.path.join(directory, 'dyld_shared_cache_arm64*')) if not re.search(r'\.\w+$', p))[0]
    m = open(main, 'rb').read(64 << 20)
    off, count = struct.unpack_from('<II', m, 0x1c0)
    if not count:
        off, count = struct.unpack_from('<II', m, 0x18)
    for i in range(count):
        addr, _, _, path_off, _ = struct.unpack_from('<QQQII', m, off + 32 * i)
        yield addr, m[path_off:m.index(b'\0', path_off)].decode()


def exports(cache, base):
    hdr = cache.read(base, 32)
    ncmds, sizeofcmds = struct.unpack_from('<II', hdr, 16)
    cmds = cache.read(base + 32, sizeofcmds)
    linkedit = trie = None
    i = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', cmds, i)
        if cmd == LC_SEGMENT_64 and cmds[i + 8:i + 24].rstrip(b'\0') == b'__LINKEDIT':
            linkedit = struct.unpack_from('<QQQ', cmds, i + 24)    # vmaddr, vmsize, fileoff
        elif cmd == LC_DYLD_EXPORTS_TRIE:
            trie = struct.unpack_from('<II', cmds, i + 8)
        elif cmd == LC_DYLD_INFO_ONLY and trie is None:
            t = struct.unpack_from('<10I', cmds, i + 8)
            if t[9]:
                trie = (t[8], t[9])
        i += size
    vmaddr, _, fileoff = linkedit
    data = cache.read(vmaddr + trie[0] - fileoff, trie[1])
    out = []

    def walk(node, prefix):
        size, i = uleb(data, node)
        if size:
            flags, j = uleb(data, i)
            if not flags & 0x08:                                    # not a re-export
                off, _ = uleb(data, j)
                out.append((base + off, prefix))
        i += size
        n = data[i]; i += 1
        for _ in range(n):
            e = data.index(b'\0', i)
            edge = data[i:e].decode(); i = e + 1
            child, i = uleb(data, i)
            walk(child, prefix + edge)
    walk(0, '')
    return sorted(out)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    directory, want = sys.argv[1], sys.argv[2]
    pat = re.compile(sys.argv[3]) if len(sys.argv) > 3 else None
    cache = Cache(directory)
    for addr, path in images(directory):
        if os.path.basename(path) == want or (not os.sep in want and False) or re.search(want + '$', path):
            print('# %s at %#x' % (path, addr))
            for a, name in exports(cache, addr):
                if not pat or pat.search(name):
                    print('%#x %s' % (a, name))
            return
    sys.exit('dsc-exports: no image %s' % want)


if __name__ == '__main__':
    main()
