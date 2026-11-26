"""Read an iOS arm64e kernelcache (MH_FILESET Mach-O, decompressed).

Kext sections come from the LC_FILESET_ENTRY headers. Pointers in a kernelcache are chained fixups in the
kernel-cache format (DYLD_CHAINED_PTR_64_KERNEL_CACHE): the low 30 bits are an offset from the cache's base.
We walk the chains only on pages that hold the kext we look at; the whole cache has millions of pointers.
"""
import bisect, json, os, struct

import capstone

LC_SEGMENT_64 = 0x19
LC_DYLD_CHAINED_FIXUPS = 0x80000034
LC_FILESET_ENTRY = 0x80000035


def _sections(d, mh):
    """(segment, section, va, size) of the Mach-O header at file offset MH"""
    out = []
    n = struct.unpack_from('<I', d, mh + 16)[0]
    o = mh + 32
    for _ in range(n):
        c, s = struct.unpack_from('<II', d, o)
        if c == LC_SEGMENT_64:
            sg = d[o + 8:o + 24].rstrip(b'\0').decode()
            ns = struct.unpack_from('<I', d, o + 64)[0]
            for i in range(ns):
                so = o + 72 + 80 * i
                sec = d[so:so + 16].rstrip(b'\0').decode()
                va, sz = struct.unpack_from('<QQ', d, so + 32)
                out.append((sg, sec, va, sz))
        o += s
    return out


class Kernelcache:
    def __init__(self, path, symbols=None):
        self.path = path
        self.d = d = open(path, 'rb').read()
        if struct.unpack_from('<I', d, 0)[0] != 0xfeedfacf:
            raise SystemExit('iogpu: %s is not a 64-bit Mach-O (decompress the kernelcache first)' % path)
        self.segs, self.ents, self.fixups = [], [], None
        n = struct.unpack_from('<I', d, 16)[0]
        o = 32
        for _ in range(n):
            c, s = struct.unpack_from('<II', d, o)
            if c == LC_SEGMENT_64:
                self.segs.append(struct.unpack_from('<QQQQ', d, o + 24))       # vmaddr, vmsize, fileoff, filesize
            elif c == LC_FILESET_ENTRY:
                vm, fo, no = struct.unpack_from('<QQI', d, o + 8)
                self.ents.append((d[o + no:o + s].split(b'\0')[0].decode(), vm, fo))
            elif c == LC_DYLD_CHAINED_FIXUPS:
                self.fixups = struct.unpack_from('<II', d, o + 8)
            o += s
        # chained targets are offsets from the cache's lowest address (its first segment)
        self.base = min(vm for vm, vs, fo, fs in self.segs if vs)
        # every kext section, sorted, to say who owns an address
        self.ranges = sorted((va, va + sz, name, sec) for name, vm, fo in self.ents
                             for sg, sec, va, sz in _sections(d, fo) if sz)
        self._lo = [r[0] for r in self.ranges]
        self._ptrs = {}
        # kernel symbols (address -> name), e.g. the .symbols.json next to the kernelcache
        if symbols is None and os.path.exists(path + '.symbols.json'):
            symbols = path + '.symbols.json'
        self.syms = {int(k): v for k, v in json.load(open(symbols)).items()} if symbols else {}
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.md.detail = True
        self.md.skipdata = True

    # --- memory -----------------------------------------------------------------------------------------------
    def off(self, va):
        for vm, vs, fo, fs in self.segs:
            if vm <= va < vm + fs:
                return fo + va - vm
        return None

    def read(self, va, n):
        o = self.off(va)
        if o is None:
            raise KeyError(hex(va))
        return self.d[o:o + n]

    def q(self, va):
        return struct.unpack('<Q', self.read(va, 8))[0]

    def u32(self, va):
        return struct.unpack('<I', self.read(va, 4))[0]

    def cstr(self, va):
        o = self.off(va)
        if o is None:
            return None
        return self.d[o:self.d.index(b'\0', o)].decode('latin1')

    def ptr(self, v):
        """a raw chained pointer -> the address it points to"""
        return self.base + (v & 0x3fffffff) if v else 0

    def dis(self, va, n):
        return list(self.md.disasm(self.read(va, n), va))

    # --- kexts ------------------------------------------------------------------------------------------------
    def kexts(self):
        return [e[0] for e in self.ents]

    def sections(self, bundle):
        """{'SEG,sect': (va, size)} of one kext"""
        for name, vm, fo in self.ents:
            if name == bundle:
                return {sg + ',' + sec: (va, sz) for sg, sec, va, sz in _sections(self.d, fo)}
        raise SystemExit('iogpu: no kext %s in %s' % (bundle, self.path))

    def text(self, bundle):
        return self.sections(bundle)['__TEXT_EXEC,__text']

    def owner(self, a):
        """(kext, section) that holds address A"""
        i = bisect.bisect_right(self._lo, a) - 1
        if i >= 0 and self.ranges[i][0] <= a < self.ranges[i][1]:
            return self.ranges[i][2], self.ranges[i][3]
        return '?', '?'

    def pointers(self, bundle):
        """every chained pointer that lives in BUNDLE:
        (where, its section, target, target kext, target section, diversity, addrDiv, key, isAuth)"""
        if bundle in self._ptrs:
            return self._ptrs[bundle]
        mine = [(va, va + sz) for sg, (va, sz) in self.sections(bundle).items() if sz]
        hdr = self.d[self.fixups[0]:self.fixups[0] + self.fixups[1]]
        starts = struct.unpack_from('<I', hdr, 4)[0]
        nseg = struct.unpack_from('<I', hdr, starts)[0]
        out = []
        for i, so in enumerate(struct.unpack_from('<%dI' % nseg, hdr, starts + 4)):
            if so == 0:
                continue
            s = starts + so
            size, page_size, fmt, seg_off, max_valid = struct.unpack_from('<IHHQI', hdr, s)
            page_count = struct.unpack_from('<H', hdr, s + 20)[0]
            page_starts = struct.unpack_from('<%dH' % page_count, hdr, s + 22)
            va0, vsz, fo0, fsz = self.segs[i]
            for p, st in enumerate(page_starts):
                if st == 0xffff:
                    continue
                pva = va0 + p * page_size
                if not any(lo < pva + page_size and pva < hi for lo, hi in mine):
                    continue
                a = fo0 + p * page_size + st
                while True:
                    v = struct.unpack_from('<Q', self.d, a)[0]
                    loc = va0 + (a - fo0)
                    ow, sec = self.owner(loc)
                    if ow == bundle:
                        t = self.ptr(v)
                        to, tsec = self.owner(t)
                        out.append((loc, sec, t, to, tsec, (v >> 32) & 0xffff, (v >> 48) & 1, (v >> 49) & 3, v >> 63))
                    nx = (v >> 51) & 0xfff
                    if not nx:
                        break
                    a += nx * 4
        out.sort()
        self._ptrs[bundle] = out
        return out

    def name(self, a):
        """a kernel symbol for A, or its hex address"""
        return self.syms.get(a) or '%#x' % a
