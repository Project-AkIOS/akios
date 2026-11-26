"""kc-vtables: find the C++ vtables of kexts in an iOS kernelcache and name their slots from the macOS KDK.

A vtable in a kext is a run of (at least 8) chained pointers in __const. Slot 7 is getMetaClass(), which just
returns its class's metaclass (adrp/add x0), and kc-classes maps metaclasses to names.

iOS vtables are the macOS ones without the _RESERVED padding slots (checked for IOGPU, IOGPUCommandQueue,
IOGPUResource, IOGPUDeviceUserClient ... in 24B156 vs KDK 26B131), so dropping those names from the macOS
vtable names the iOS slots one for one. A vendor class (AGX...) has no macOS twin; a pairs file names its IOGPU
base class, and the slots that point into the vendor kext are its overrides.
"""
import json, os, subprocess

from capstone.arm64 import ARM64_REG_X0

from . import classes as kclasses
from .kdk import GENERIC, MK


def _meta_of(kc, fn):
    try:
        ins = kc.dis(fn, 24)
    except KeyError:
        return None
    r = {}
    for i in ins:
        if i.mnemonic == 'adrp':
            r[i.operands[0].reg] = i.operands[1].imm & 0xffffffffffffffff
        elif i.mnemonic == 'add' and len(i.operands) == 3 and i.operands[1].reg in r:
            r[i.operands[0].reg] = r[i.operands[1].reg] + i.operands[2].imm
        elif i.mnemonic in ('ret', 'retab'):
            return r.get(ARM64_REG_X0)
    return None


def find(kc, bundles):
    """{class: (address, [slot targets])} for every vtable in BUNDLES"""
    meta = {}
    for b, cl in kclasses.classes(kc, bundles).items():
        for n, sz, sup, m in cl:
            if m:
                meta[m] = n
    vt = {}
    for b in bundles:
        locs = {p[0]: p[2] for p in kc.pointers(b) if p[1] == '__const'}
        for a in sorted(locs):
            if a - 8 in locs:
                continue                       # not the start of a run
            run, x = [], a
            while x in locs:
                run.append(locs[x])
                x += 8
            if len(run) < 8:
                continue
            m = _meta_of(kc, run[7])
            if m in meta:
                vt[meta[m]] = (a, run)
    return vt


class MacNames:
    """vtable slot names from one or more macOS kexts, without the reserved slots"""

    def __init__(self, paths):
        self.mks = [MK(p) for p in paths]
        self._cache = {}

    def _raw(self, m, cls):
        k = (m.path, cls)
        if k not in self._cache:
            v = m.vtable(cls)
            self._cache[k] = [nm for i, t, nm in v if 'RESERVED' not in nm] if v else None
        return self._cache[k]

    def names(self, cls):
        """slot names of CLS; its pure virtual slots get the name a macOS subclass gives the same slot"""
        for m in self.mks:
            base = self._raw(m, cls)
            if base:
                break
        else:
            return None
        pure = [k for k, n in enumerate(base) if _key(n) == 'cxa_pure_virtual']
        if pure:
            filled = self._from_subclass(cls, base, pure)
            base = [filled.get(k, n) for k, n in enumerate(base)]
        return base

    def _from_subclass(self, cls, base, pure):
        """a class whose vtable agrees with BASE's method signatures on the slots BASE implements is a subclass
        (kexts carry no RTTI to say so); its names for BASE's pure slots name them"""
        def sig(n):
            n = n[len('import:'):] if n.startswith('import:') else n
            return n.split('::', 1)[-1]
        impl = [k for k in range(len(base)) if k not in pure]
        out = {}
        for m in self.mks:
            for c in m.classes():
                if c == cls or c.endswith('::MetaClass'):
                    continue
                v = self._raw(m, c)
                if not v or len(v) < len(base):
                    continue
                agree = sum(1 for k in impl if sig(v[k]) == sig(base[k]) or (sig(v[k]).startswith('~') and sig(base[k]).startswith('~')))
                if agree < 0.9 * len(impl):
                    continue
                for k in pure:
                    if k not in out and _key(v[k]) != 'cxa_pure_virtual':
                        out[k] = '%s::%s  [pure; named from %s]' % (cls, sig(v[k]), c)
            if len(out) == len(pure):
                break
        return out


def _demangle(names):
    out = subprocess.run(['llvm-cxxfilt', '-_'], input='\n'.join(names), capture_output=True, text=True).stdout.splitlines()
    return dict(zip(names, out)) if len(out) == len(names) else {n: n for n in names}


def _key(name):
    """compare names without the import: prefix, the leading underscore of C symbols and the argument list"""
    n = name[len('import:'):] if name.startswith('import:') else name
    return n.lstrip('_').split('(')[0]


def align(kc, slots, names):
    """map iOS slots to macOS names: [macOS index or None] per iOS slot, plus the macOS indices iOS lacks.

    Usually the two are equal one for one. Where they are not (iOS IOGPU has no newSurface(), so from slot 200
    every later name would be off by one), the slots the kernel's own symbols name, and the pure virtuals,
    anchor a global alignment (Needleman-Wunsch) that finds where macOS has a slot iOS does not, or the reverse."""
    pure = {a for a, n in kc.syms.items() if n.lstrip('_') == 'cxa_pure_virtual'}
    dem = _demangle(sorted({kc.syms[t] for t in slots if t in kc.syms}))
    ios = ['PURE' if t in pure else _key(dem[kc.syms[t]]) if t in kc.syms else None for t in slots]
    mac = ['PURE' if _key(n) == 'cxa_pure_virtual' or '[pure;' in n else _key(n) for n in names]
    if len(ios) == len(mac) and all(a is None or a == b for a, b in zip(ios, mac)):
        return list(range(len(ios))), []

    def score(a, b):
        if a is None:
            return -1.0 if b == 'PURE' else 0.5        # an unnamed iOS slot: anything but a pure virtual
        if a == b:
            return 3.0
        return -3.0
    gap = -2.0
    n, m = len(ios), len(mac)
    S = [[0.0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        S[i][0] = i * gap
    for j in range(1, m + 1):
        S[0][j] = j * gap
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            S[i][j] = max(S[i - 1][j - 1] + score(ios[i - 1], mac[j - 1]), S[i - 1][j] + gap, S[i][j - 1] + gap)
    # walking back from the end, take a gap before a diagonal step on ties: a gap lands as late as the anchors
    # allow. Between two anchors it could sit anywhere; align_note() says so.
    res, missing, i, j = [None] * n, [], n, m
    while i or j:
        if j and S[i][j] == S[i][j - 1] + gap:
            missing.append(j - 1)                     # a macOS-only slot
            j -= 1
        elif i and S[i][j] == S[i - 1][j] + gap:
            i -= 1                                    # an iOS-only slot
        else:
            res[i - 1] = j - 1
            i, j = i - 1, j - 1
    return res, sorted(missing)


def align_note(kc, slots, names, idx, j):
    """the macOS slots a missing slot J could equally be: unnamed iOS slots that are not pure virtuals"""
    pure = {a for a, n in kc.syms.items() if n.lstrip('_') == 'cxa_pure_virtual'}
    back = {v: k for k, v in enumerate(idx) if v is not None}
    lo = j
    while lo - 1 in back and slots[back[lo - 1]] not in kc.syms and slots[back[lo - 1]] not in pure \
            and _key(names[lo - 1]) != 'cxa_pure_virtual' and '[pure;' not in names[lo - 1]:
        lo -= 1
    return '' if lo == j else '; macOS %d-%d have no iOS names or pure virtuals to tell, so it may be any of them' % (lo, j)


def read_pairs(path):
    out = []
    for l in open(path):
        l = l.split('#')[0].split()
        if len(l) == 2:
            out.append(tuple(l))
    return out


def run(kc, bundles, mac=None, cls=None, pairs=None, as_json=False):
    if pairs and 'com.apple.iokit.IOGPUFamily' not in bundles:
        bundles = list(bundles) + ['com.apple.iokit.IOGPUFamily']    # the base classes' own vtables
    vt = find(kc, bundles)
    mn = MacNames(mac) if mac else None
    if as_json:
        print(json.dumps({k: {'addr': '%#x' % a, 'slots': ['%#x' % t for t in r]} for k, (a, r) in sorted(vt.items())}, indent=1))
        return
    if cls:
        if cls not in vt:
            raise SystemExit('iogpu: no vtable for %s in %s' % (cls, ', '.join(bundles)))
        a, r = vt[cls]
        names = mn.names(cls) if mn else None
        print('## %s @%#x slots=%d%s' % (cls, a, len(r), '' if names is None else ', macOS slots=%d' % len(names)))
        idx, missing = align(kc, r, names) if names else ([None] * len(r), [])
        for j in missing:
            print('  (macOS slot %d %s is not in the iOS vtable%s)' % (j, names[j], align_note(kc, r, names, idx, j)))
        for k, t in enumerate(r):
            nm = names[idx[k]] if idx[k] is not None else '(iOS only) ' + kc.name(t) if names else kc.name(t)
            shift = '' if idx[k] in (None, k) else '  [macOS %d]' % idx[k]
            print('  %3d %#06x %#x  %s%s' % (k, k * 8, t, nm, shift))
        return
    if pairs:
        _overrides(kc, vt, mn, pairs)
        return
    if mn:
        print('# macOS: same = the slots match one for one; DIFFERENT = see --class CLASS for where they differ')
    for k, (a, r) in sorted(vt.items()):
        extra = ''
        if mn:
            names = mn.names(k)
            if names is not None:
                idx, missing = align(kc, r, names)
                same = not missing and idx == list(range(len(r)))
                extra = '  macOS %d %s' % (len(names), 'same' if same else 'DIFFERENT (%d macOS-only, %d iOS-only)' % (
                    len(missing), idx.count(None)))
        print('%-40s %#x slots=%d%s' % (k, a, len(r), extra))


def _overrides(kc, vt, mn, pairs):
    """for each (vendor class, IOGPU base): which base slots the vendor kext implements itself"""
    if not mn:
        raise SystemExit('iogpu: --pairs needs --mac (the macOS kexts that name the base classes)')
    for sub, base in pairs:
        if sub not in vt:
            print('##', sub, '(vtable not found)')
            continue
        a, r = vt[sub]
        own_kext = kc.owner(a)[0]
        ta, tl = kc.text(own_kext)
        names = mn.names(base) or []
        # name the base part of the vendor vtable through the iOS base vtable's alignment with macOS
        if base in vt:
            idx, missing = align(kc, vt[base][1], names)
            nbase = len(vt[base][1])
        else:
            idx, missing, nbase = list(range(len(names))), [], len(names)
        print('## %s : %s   iOS slots=%d, base slots=%d (macOS %d%s)' % (sub, base, len(r), nbase, len(names),
              ', not on iOS: ' + ', '.join(str(j) for j in missing) if missing else ''))
        for k, t in enumerate(r):
            if not (ta <= t < ta + tl) or k >= nbase or idx[k] is None:
                continue
            nm = names[idx[k]]
            generic = nm.startswith(GENERIC) or '~' in nm or 'getMetaClass' in nm
            print('   %3d %#x  %s%s' % (k, t, '[generic] ' if generic else '', nm[:150]))
        if len(r) > nbase:
            print('   + %d slots introduced by %s' % (len(r) - nbase, sub))
