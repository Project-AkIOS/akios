"""The iOS Symbols supplement: its Mach-Os (one per dyld-cache image, with data but no usable code) and the
sqlite symbol database made from it, plus a reader for the device's dyld shared cache itself (for code).

The database has syms(addr, name, demangled, local, image) and images(id, path, base); addresses are the
dyld-cache addresses of iOS 27.2.1 (24B156).
"""
import glob, mmap, os, sqlite3, struct, subprocess

# arm64e iOS dyld cache: rebased pointers are offsets from the cache's base address
CACHE_BASE = 0x180000000


def decode(v):
    """a raw (chained) pointer read from cache data -> its address"""
    return 0 if not v else CACHE_BASE + (v & 0x3ffffffff)


class SymDB:
    def __init__(self, path):
        if not path or not os.path.exists(path):
            raise SystemExit('iogpu: symbol database %s not found; give --db or set akios.toml [iogpu] symbols_db' % path)
        self.db = sqlite3.connect('file:%s?mode=ro' % path, uri=True)
        self._name = {}

    def name(self, a):
        if a not in self._name:
            r = self.db.execute('select name from syms where addr=? limit 1', (a,)).fetchone()
            self._name[a] = r[0] if r else None
        return self._name[a]

    def names(self, a):
        return [r[0] for r in self.db.execute('select name from syms where addr=?', (a,))]

    def lookup(self, name):
        """(address, image path) of a symbol"""
        # the name index is COLLATE NOCASE: compare that way so it is used (else a scan of 5 GB), then exactly
        for a, n, path in self.db.execute('select s.addr, s.name, i.path from syms s left join images i on i.id = s.image '
                                          'where s.name = ? collate nocase', (name,)):
            if n == name:
                return a, path
        return None, None

    def like(self, pattern):
        """[(name, addr)] for a LIKE pattern; the caller escapes _ and % with backslash"""
        return self.db.execute("select name, addr from syms where name like ? escape '\\'", (pattern,)).fetchall()

    def image(self, lib):
        """an image path from a full path (System/Library/...) or just its file name (IOGPU, Metal ...)"""
        p = '/' + lib.lstrip('/')
        r = self.db.execute('select path from images where path = ?', (p,)).fetchone()
        if r:
            return r[0]
        r = self.db.execute("select path from images where path like ? escape '\\' order by length(path)",
                            ('%/' + esc(os.path.basename(lib)),)).fetchall()
        if not r:
            raise SystemExit('iogpu: no image %s in the symbol database' % lib)
        return r[0][0]


def esc(s):
    return s.replace('\\', '\\\\').replace('_', '\\_').replace('%', '\\%')


class SymbolFiles:
    """data of the Symbols supplement's Mach-Os, loaded image by image as needed"""

    def __init__(self, root):
        if not root or not os.path.isdir(os.path.join(root, 'System')):
            raise SystemExit('iogpu: Symbols supplement %s not found (want the directory holding System/); give --symbols' % root)
        self.root = root
        self.secs, self.loaded = [], set()

    def path(self, image):
        return os.path.join(self.root, image.lstrip('/'))

    def load(self, image):
        if image in self.loaded:
            return
        self.loaded.add(image)
        p = self.path(image)
        if not os.path.exists(p):
            return
        d = open(p, 'rb').read()
        n = struct.unpack_from('<I', d, 16)[0]
        o = 32
        for _ in range(n):
            c, s = struct.unpack_from('<II', d, o)
            if c == 0x19:
                ns = struct.unpack_from('<I', d, o + 64)[0]
                for i in range(ns):
                    so = o + 72 + 80 * i
                    a, sz, fo = struct.unpack_from('<QQI', d, so + 32)
                    if fo and sz:
                        self.secs.append((a, sz, fo, d))
            o += s

    def read(self, a, k):
        for s, sz, fo, d in self.secs:
            if s <= a < s + sz:
                return d[fo + a - s:fo + a - s + k]
        return None

    def q(self, a):
        b = self.read(a, 8)
        return struct.unpack('<Q', b)[0] if b and len(b) == 8 else None

    def text_symbols(self, image):
        """[(address, name)] of the image's code symbols, in address order"""
        out = []
        r = subprocess.run(['llvm-nm', '-n', '--defined-only', self.path(image)], capture_output=True, text=True)
        for l in r.stdout.splitlines():
            p = l.split(' ', 2)
            if len(p) == 3 and p[1] in 'tT':
                out.append((int(p[0], 16), p[2]))
        return out


class DyldCache:
    """the device's dyld shared cache (main file + numbered subcaches), memory-mapped, read by address.

    Only the code and text subcaches are trusted here: .dylddata subcaches (ObjC data) are not read correctly
    this way, so data comes from the Symbols Mach-Os instead."""

    def __init__(self, path):
        if not path or not os.path.exists(path):
            raise SystemExit('iogpu: dyld cache %s not found; give --dsc or set akios.toml [userland] ipsw' % path)
        self.maps = []
        for p in sorted(glob.glob(path + '*')):
            if p.endswith('.symbols') or p.endswith('.map'):
                continue
            f = open(p, 'rb')
            m = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            if m[:7] != b'dyld_v1':
                continue
            off, cnt = struct.unpack_from('<II', m, 0x10)
            for i in range(cnt):
                a, s, fo = struct.unpack_from('<QQQ', m, off + 32 * i)
                self.maps.append((a, s, fo, m))

    def read(self, a, n):
        for st, s, fo, m in self.maps:
            if st <= a < st + s:
                o = fo + a - st
                return m[o:o + n]
        return None
