"""objc-supers: Objective-C superclass chains and method lists from the Symbols supplement.

The superclass is the second word of class_t (_OBJC_CLASS_$_Name + 8), read from the Symbols Mach-O of the
image that defines the class. The dyld cache's own .dylddata subcaches are not read right by our cache reader,
so we never take ObjC data from there. Method lists come from the -[Class sel] / +[Class sel] symbols.
"""
import re

from .symbols import decode, esc


class ObjC:
    def __init__(self, db, files):
        self.db, self.files = db, files

    def superclass(self, cls):
        a, image = self.db.lookup('_OBJC_CLASS_$_' + cls)
        if a is None:
            return None
        self.files.load(image)
        v = self.files.q(a + 8)
        if v is None:
            return '?nodata'
        t = decode(v)
        n = [x for x in self.db.names(t) if x.startswith('_OBJC_CLASS_$_')]
        return n[0][len('_OBJC_CLASS_$_'):] if n else '%#x' % t

    def chain(self, cls, depth=16):
        out, x = [cls], cls
        for _ in range(depth):
            x = self.superclass(x)
            if x is None:
                break
            out.append(x)
            if x.startswith(('0x', '?')) or x == 'NSObject':
                break
        return out

    def classes(self, prefix):
        return sorted(n[len('_OBJC_CLASS_$_'):] for n, a in self.db.like(esc('_OBJC_CLASS_$_' + prefix) + '%'))

    def methods(self, cls):
        """{'-sel' / '+sel': address}; categories (Class(Cat)) included"""
        out = {}
        for kind in '-+':
            for pat in ('%s[%s %%' % (kind, esc(cls)), '%s[%s(%%' % (kind, esc(cls))):
                for n, a in self.db.like(pat):
                    m = re.match(r'[-+]\[\S+ (.*)\]$', n)
                    if m:
                        out[kind + m.group(1)] = a
        return out


def run(db, files, names, prefix=None, methods=False, vs=None):
    o = ObjC(db, files)
    if prefix:
        names = list(names) + o.classes(prefix)
    for c in names:
        ch = o.chain(c)
        print(' -> '.join(ch))
        if methods:
            up = {}
            for anc in ch[1:]:
                if anc.startswith(('0x', '?')):
                    break
                for s in o.methods(anc):
                    up.setdefault(s, anc)
            own = o.methods(c)
            for s in sorted(own, key=lambda s: s[1:]):
                print('    %s%s' % (s, '   (overrides %s)' % up[s] if s in up else ''))
        if vs:
            # what the classes below VS override of VS's own methods, and what they take from it as is
            if vs not in ch:
                print('    (%s is not in the chain)' % vs)
                continue
            below = set()
            for x in ch[:ch.index(vs)]:
                below.update(o.methods(x))
            base = o.methods(vs)
            ov = sorted(s for s in base if s in below)
            inh = sorted(s for s in base if s not in below)
            print('    vs %s: overrides %d, inherits %d' % (vs, len(ov), len(inh)))
            for s in ov:
                print('      override  %s' % s)
            for s in inh:
                print('      inherit   %s' % s)
