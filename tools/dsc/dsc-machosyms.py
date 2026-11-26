#!/usr/bin/env python3
"""Every symbol (local ones too) of a library as it is in a dyld shared cache, from a "Symbols" directory of the kind
Xcode's DeviceSupport keeps for an iOS build (one Mach-O per library, its symbol table at the cache's addresses) --
what the cache's own .symbols file would give, which sysimage/unpack.sh leaves out.

usage: dsc-machosyms.py SYMBOLS_FILE [REGEX...]       its symbols (matching one REGEX), "address name", sorted
       dsc-machosyms.py SYMBOLS_FILE --at ADDRESS...  the nearest symbol at or below each address, and the distance

SYMBOLS_FILE e.g. ~/akita/ref/symbols-24B156/27.2.1 (24B156) arm64e/Symbols/System/Library/Frameworks/QuartzCore.framework/QuartzCore
Nothing of Apple's is kept in the repository: it reads files you have.
"""
import bisect
import re
import struct
import sys

LC_SYMTAB = 0x2


def symbols(path):
    data = open(path, 'rb').read()
    if struct.unpack_from('<I', data, 0)[0] != 0xfeedfacf:
        sys.exit('dsc-machosyms: %s is not a 64-bit Mach-O' % path)
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, strsize = struct.unpack_from('<IIII', data, at + 8)
            out = []
            for i in range(nsyms):
                strx, n_type, n_sect, n_desc, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
                if n_type & 0xe0 or n_sect == 0 or value == 0:         # stabs, undefined
                    continue
                end = data.index(b'\0', stroff + strx)
                out.append((value, data[stroff + strx:end].decode('utf-8', 'replace')))
            return sorted(out)
        at += size
    return []


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    syms = symbols(sys.argv[1])
    if len(sys.argv) > 2 and sys.argv[2] == '--at':
        keys = [a for a, _ in syms]
        for word in sys.argv[3:]:
            a = int(word, 0)
            i = bisect.bisect_right(keys, a) - 1
            print('%#x %s+%#x' % (a, syms[i][1], a - syms[i][0]) if i >= 0 else '%#x ?' % a)
        return
    pats = [re.compile(p) for p in sys.argv[2:]]
    for a, name in syms:
        if not pats or any(p.search(name) for p in pats):
            print('%#x %s' % (a, name))


if __name__ == '__main__':
    main()
