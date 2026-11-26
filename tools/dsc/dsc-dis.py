#!/usr/bin/env python3
"""Disassemble a function of the iOS 27 shared cache by name, at its true address.

usage: dsc-dis.py IMAGE SYMBOL [BYTES]     e.g. dsc-dis.py CoreServices '-[FSNode getResourceValue:forKey:options:error:]'

The symbol registry (dsc-symdb.py) takes addresses from the symbols supplement as they are, and for images in the
cache's later subcaches those are off (CarbonCore: 0x2c6000). Here an address is taken as an offset from the image's
own __TEXT in its symbol file and put at the image's base as the cache header lists it. Calls and branches inside
the image are named the same way; a string loaded with adrp+add is shown when it is one.
"""
import bisect
import glob
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
# the unpacked IPSW (akios-data, outside every repo): AKIOS_DATA overrides
DATA = os.environ.get('AKIOS_DATA', os.path.expanduser('~/akita/akios-data'))
ex = __import__('dsc-exports')
Cache = __import__('dsc-disasm').Cache
ms = __import__('dsc-machosyms')
import capstone

CACHE = os.path.join(DATA, 'ipsw-d16ap', '24B156__iPhone14,4') + '/'
SUPP = glob.glob(os.path.expanduser('~/akita/ref/symbols-24B156/*/Symbols'))[0]


def text_base(data):
    n = struct.unpack_from('<I', data, 16)[0]
    off = 32
    for _ in range(n):
        cmd, sz = struct.unpack_from('<II', data, off)
        if cmd == 0x19 and data[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            return struct.unpack_from('<Q', data, off + 24)[0]
        off += sz
    raise SystemExit('no __TEXT')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    image, want = sys.argv[1], sys.argv[2]
    nbytes = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0x400
    imgs = sorted(ex.images(CACHE))
    hits = [(a, p) for a, p in imgs if (p.endswith('/' + image) if '/' in image else os.path.basename(p) == image)]
    if not hits:
        sys.exit(f'dsc-dis.py: no image {image} in the cache')
    cbase, path = hits[0]
    supp = SUPP + path
    data = open(supp, 'rb').read()
    sbase = text_base(data)
    syms = sorted(ms.symbols(supp))
    keys = [a for a, _ in syms]
    match = [(a, n) for a, n in syms if n == want] or [(a, n) for a, n in syms if want in n]
    if not match:
        sys.exit(f'dsc-dis.py: no symbol like {want} in {image}')
    start, name = match[0]
    i = keys.index(start)
    end = keys[i + 1] if i + 1 < len(keys) else start + nbytes
    end = min(end, start + nbytes)

    def nm(cache_addr):
        a = cache_addr - cbase + sbase
        j = bisect.bisect_right(keys, a) - 1
        if j >= 0 and 0 <= a - syms[j][0] < 0x200000:
            return '%s+%#x' % (syms[j][1], a - syms[j][0])
        return None

    c = Cache(CACHE)
    at = start - sbase + cbase
    code = bytes(c.read(at, end - start))
    print(f'{name} in {image}: cache {at:#x} (image base {cbase:#x})')
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    page = {}
    for ins in md.disasm(code, at):
        note = ''
        ops = ins.op_str
        if ins.mnemonic == 'adrp':
            page[ops.split(',')[0]] = int(ops.split('#')[-1], 0)
        elif ins.mnemonic == 'add' and '#0x' in ops:
            r = [x.strip() for x in ops.split(',')]
            if r[1] in page:
                a = page[r[1]] + int(r[2].split('#')[-1], 0)
                try:
                    s = bytes(c.read(a, 120)).split(b'\0')[0]
                    if 4 <= len(s) and all(32 <= ch < 127 for ch in s):
                        note = '  ; "%s"' % s.decode()
                except BaseException:
                    pass
        elif ins.mnemonic == 'ldr' and '#0x' in ops and ops.split(',')[1].strip().lstrip('[') in page:
            # a pointer loaded from a table: a CFString constant (isa, flags, chars, length) is shown by its text
            r = [x.strip() for x in ops.replace('[', '').replace(']', '').split(',')]
            a = page[r[1]] + int(r[2].split('#')[-1], 0)
            try:
                ptr = struct.unpack('<Q', bytes(c.read(a, 8)))[0] & 0xfffffffff
                ptr = ptr + 0x180000000 if ptr < 0x180000000 else ptr
                cf = bytes(c.read(ptr, 32))
                chars = struct.unpack_from('<Q', cf, 16)[0] & 0xfffffffff
                chars = chars + 0x180000000 if chars < 0x180000000 else chars
                txt = bytes(c.read(chars, 80)).split(b'\0')[0]
                if 3 <= len(txt) and all(32 <= ch < 127 for ch in txt):
                    note = '  ; @"%s"' % txt.decode()
            except BaseException:
                pass
        elif ins.mnemonic in ('bl', 'b') or ins.mnemonic.startswith(('b.', 'cb', 'tb')):
            try:
                tgt = int(ops.split('#')[-1], 0)
                n2 = nm(tgt)
                if n2 and not (at <= tgt < at + len(code)):
                    note = '  ; ' + n2
            except ValueError:
                pass
        print('%#x  %-7s %s%s' % (ins.address - cbase, ins.mnemonic, ops, note))


if __name__ == '__main__':
    main()
