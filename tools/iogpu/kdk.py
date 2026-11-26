"""kdk: read macOS kexts from a Kernel Debug Kit, which ships them with full symbols.

  extract   pull kexts out of the KDK's pkg payload (pbzx -> cpio) and thin them to arm64e (mac-NAME.arm64e)
  syms      defined symbols (demangled), optionally filtered by a regex
  imports   the chained-fixup imports (what the kext needs from the kernel and other kexts)
  dump      a table at a symbol: entry 0 is a pointer, the rest u32s (IOExternalMethodDispatch by default)
  vtable    the vtable slots of a class, named
  overrides the slots of a class's vtable that the kext implements itself (what a vendor subclass must provide)

A thin kext's pointers are chained fixups in arm64e userland format (DYLD_CHAINED_PTR_ARM64E): bit 63 = auth,
bit 62 = bind (the low 16 bits index the import list), else a rebase to a target offset.
"""
import bisect, glob, lzma, os, re, shutil, struct, subprocess, sys, tempfile

LC_SEGMENT_64 = 0x19
LC_DYLD_CHAINED_FIXUPS = 0x80000034
# the kexts the IOGPU research needs; `kdk extract` takes others by name
# (IOAcceleratorFamily2 is x86_64 only in KDK 26B131)
DEFAULT_KEXTS = ['IOGPUFamily', 'AppleParavirtGPUIOGPUFamily', 'AGXG13G']
# vtable slots inherited from the IOKit base classes: not interesting when listing a vendor's overrides
_BASES = ('OSObject::', 'OSMetaClassBase::', 'IOService::', 'IOUserClient::', 'IORegistryEntry::')
GENERIC = _BASES + tuple('import:' + b for b in _BASES)


class MK:
    """one thin arm64e kext Mach-O"""

    def __init__(self, path):
        self.path = path
        self.d = d = open(path, 'rb').read()
        if struct.unpack_from('<I', d, 0)[0] != 0xfeedfacf:
            raise SystemExit('iogpu: %s is not a thin 64-bit Mach-O (use `kdk extract`, it thins to arm64e)' % path)
        self.segs, self.imports = [], []
        n = struct.unpack_from('<I', d, 16)[0]
        o = 32
        for _ in range(n):
            c, s = struct.unpack_from('<II', d, o)
            if c == LC_SEGMENT_64:
                self.segs.append(struct.unpack_from('<QQQQ', d, o + 24))
            elif c == LC_DYLD_CHAINED_FIXUPS:
                do, ds = struct.unpack_from('<II', d, o + 8)
                v, starts, io, so, count, ifmt, sfmt = struct.unpack_from('<7I', d, do)
                for k in range(count):
                    # DYLD_CHAINED_IMPORT: lib_ordinal:8, weak:1, name_offset:23
                    no = struct.unpack_from('<I', d, do + io + 4 * k)[0] >> 9
                    e = d.index(b'\0', do + so + no)
                    self.imports.append(d[do + so + no:e].decode())
            o += s
        # imports are shown demangled (import:IOService::start(IOService*)); kdk imports lists them raw
        self.imports_dem = subprocess.run(['llvm-cxxfilt', '-_'], input='\n'.join(self.imports), capture_output=True,
                                          text=True).stdout.splitlines() if self.imports else []
        if len(self.imports_dem) != len(self.imports):
            self.imports_dem = list(self.imports)
        self.syms, self.byname = {}, {}
        raw = subprocess.run(['llvm-nm', '-n', '--defined-only', path], capture_output=True, text=True).stdout
        dem = subprocess.run(['llvm-nm', '-n', '-C', '--defined-only', path], capture_output=True, text=True).stdout
        for l, ld in zip(raw.splitlines(), dem.splitlines()):
            p, pd = l.split(' ', 2), ld.split(' ', 2)
            if len(p) == 3:
                a = int(p[0], 16)
                self.syms.setdefault(a, pd[2])
                self.byname[pd[2]] = a
                self.byname[p[2]] = a
        self.sa = sorted(self.syms)

    def off(self, va):
        for vm, vs, fo, fs in self.segs:
            if vm <= va < vm + vs:
                return fo + va - vm
        raise KeyError(hex(va))

    def q(self, va):
        return struct.unpack_from('<Q', self.d, self.off(va))[0]

    def u32(self, va):
        return struct.unpack_from('<I', self.d, self.off(va))[0]

    def ptr(self, v):
        if v >> 62 & 1:
            return ('bind', v & 0xffff)
        return v & 0x3fffffff if v >> 63 else v & 0x7ffffffffff

    def name(self, a):
        if isinstance(a, tuple):
            return 'import:' + (self.imports_dem[a[1]] if a[1] < len(self.imports) else str(a[1]))
        j = bisect.bisect_right(self.sa, a) - 1
        if j < 0:
            return hex(a)
        b = self.sa[j]
        return self.syms[b] + ('' if b == a else '+%#x' % (a - b))

    def addr(self, sym):
        if sym.startswith('0x'):
            return int(sym, 16)
        if sym not in self.byname:
            raise SystemExit('iogpu: no symbol %s in %s' % (sym, self.path))
        return self.byname[sym]

    def vtable(self, cls):
        """[(slot, target, name)] of `vtable for CLS`, up to the next vtable or a null"""
        a = self.byname.get('vtable for ' + cls)
        if a is None:
            return None
        a += 16                                  # past offset-to-top and the typeinfo pointer
        out = []
        while True:
            v = self.q(a)
            if v == 0:
                break
            t = self.ptr(v)
            nm = self.name(t)
            if nm.startswith(('vtable for', 'typeinfo')):
                break
            out.append((len(out), t, nm))
            a += 8
        return out

    def classes(self):
        return sorted(n[len('vtable for '):] for n in self.byname if n.startswith('vtable for '))


def find_kext(name, dirs):
    """NAME as a path, or mac-NAME.arm64e in one of DIRS"""
    if os.path.exists(name):
        return name
    for d in dirs:
        for p in (os.path.join(d, 'mac-%s.arm64e' % name), os.path.join(d, name)):
            if os.path.exists(p):
                return p
    raise SystemExit('iogpu: kext %s not found in %s' % (name, ', '.join(dirs)))


# --- commands ----------------------------------------------------------------------------------------------------

def cmd_syms(m, rx):
    r = re.compile(rx) if rx else None
    for a in m.sa:
        if not r or r.search(m.syms[a]):
            print('%#010x %s' % (a, m.syms[a]))


def cmd_imports(m, demangle):
    for k, n in enumerate(m.imports_dem if demangle else m.imports):
        print('%4d %s' % (k, n))


def cmd_dump(m, sym, es, count):
    base = m.addr(sym)
    if count is None:
        # up to the next symbol: the table's own size
        j = bisect.bisect_right(m.sa, base)
        count = ((m.sa[j] - base) // es) if j < len(m.sa) else 1
    for k in range(count):
        a = base + k * es
        v = m.q(a)
        fn = m.name(m.ptr(v)) if v else '(null)'
        rest = [m.u32(a + 8 + 4 * j) for j in range((es - 8) // 4)]
        print('%3d  %-70s %s' % (k, fn[:70], ' '.join('%#x' % r for r in rest)))


def cmd_vtable(m, cls):
    vt = m.vtable(cls)
    if vt is None:
        raise SystemExit('iogpu: no vtable for %s in %s' % (cls, m.path))
    for k, t, nm in vt:
        print('%4d %#06x %s' % (k, k * 8, nm))


def cmd_overrides(m, classes):
    for cls in classes or [c for c in m.classes() if not c.endswith('::MetaClass')]:
        vt = m.vtable(cls)
        if not vt:
            continue
        own = [(k, nm) for k, t, nm in vt if not isinstance(t, tuple) and not nm.startswith(GENERIC)
               and '::' in nm and nm.split('::')[0] == cls
               and '~' not in nm and 'getMetaClass' not in nm]       # every class has these: not news
        print('### %s (%d slots, %d its own)' % (cls, len(vt), len(own)))
        for k, nm in own:
            print('%4d  %s' % (k, nm))


def _pbzx(path, out):
    """an Apple pkg Payload (pbzx: chunks of xz or stored data) -> the cpio stream, written to OUT"""
    with open(path, 'rb') as f:
        if f.read(4) != b'pbzx':
            raise SystemExit('iogpu: %s is not a pbzx payload' % path)
        f.read(8)
        while True:
            h = f.read(16)
            if len(h) < 16:
                break
            flags, size = struct.unpack('>QQ', h)
            data = f.read(size)
            out.write(lzma.decompress(data) if data[:6] == b'\xfd7zXZ\x00' else data)


def cmd_extract(kdk, names, out):
    payload = os.path.join(kdk, 'pkg', 'KDK.pkg', 'Payload')
    if not os.path.exists(payload):
        hits = glob.glob(os.path.join(kdk, '**', 'KDK.pkg', 'Payload'), recursive=True)
        if not hits:
            raise SystemExit('iogpu: no KDK.pkg/Payload under %s (unpack the dmg and the pkg with 7z/xar first)' % kdk)
        payload = hits[0]
    for t in ('cpio', 'llvm-lipo'):
        if not shutil.which(t):
            raise SystemExit('iogpu: %s is needed for kdk extract' % t)
    os.makedirs(out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix='kdkx-', dir=out)
    pats = ['./System/Library/Extensions/%s.kext/*' % n for n in names]
    print('kdk: reading %s (a minute or two for a 1 GB payload)' % payload, file=sys.stderr)
    try:
        cp = subprocess.Popen(['cpio', '-idm', '--quiet', '--no-absolute-filenames'] + pats, stdin=subprocess.PIPE, cwd=tmp)
        try:
            _pbzx(payload, cp.stdin)
        except BrokenPipeError:
            pass
        cp.stdin.close()
        cp.wait()
        for n in names:
            src = os.path.join(tmp, 'System', 'Library', 'Extensions', n + '.kext')
            if not os.path.exists(src):
                print('kdk: %s.kext not in the KDK' % n, file=sys.stderr)
                continue
            kext = os.path.join(out, n + '.kext')
            if os.path.exists(kext):
                shutil.rmtree(kext)
            shutil.move(src, kext)
            binary = os.path.join(kext, 'Contents', 'MacOS', n)
            dst = os.path.join(out, 'mac-%s.arm64e' % n)
            magic, cpu, sub = struct.unpack('<III', open(binary, 'rb').read(12))
            if magic == 0xfeedfacf and cpu == 0x0100000c and (sub & 0xff) == 2:      # already thin arm64e
                shutil.copyfile(binary, dst)
            elif subprocess.run(['llvm-lipo', '-thin', 'arm64e', binary, '-output', dst], capture_output=True).returncode:
                print('kdk: %s has no arm64e slice (only the .kext is kept)' % n, file=sys.stderr)
                continue
            print('%s  (%s.kext beside it)' % (dst, n))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
