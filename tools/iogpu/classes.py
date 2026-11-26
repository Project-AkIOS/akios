"""kc-classes: the OSMetaClass of every C++ class in a kext: name, instance size, superclass, metaclass address.

Each class's static initialiser calls OSMetaClass::OSMetaClass(this=meta, name, super_meta, size), so we track
x0-x3 through adrp/add/mov/ldr and take every bl whose x1 points into the kext's __cstring and w3 is a constant.
The superclass is a metaclass in this kext, or (loaded through the GOT) one in the kernel or another kext.
"""
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG, ARM64_REG_X0, ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_W3


def scan(kc, bundle):
    """[(name, meta, super, size, call site)]; meta/super are ints, or ('ld', got) when loaded from memory"""
    s = kc.sections(bundle)
    ta, tl = s['__TEXT_EXEC,__text']
    ca, cl = s['__TEXT,__cstring']
    out, regs = [], {}
    for i in kc.md.disasm(kc.read(ta, tl), ta):
        if i.id == 0:                   # data inside __text (skipdata): no operands, forget what we knew
            regs = {}
            continue
        m, ops = i.mnemonic, i.operands
        if m == 'adrp':
            regs[ops[0].reg] = ops[1].imm & 0xffffffffffffffff
        elif m == 'add' and len(ops) == 3 and ops[2].type == ARM64_OP_IMM and isinstance(regs.get(ops[1].reg), int):
            regs[ops[0].reg] = regs[ops[1].reg] + ops[2].imm
        elif m in ('mov', 'movz') and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:
            regs[ops[0].reg] = ops[1].imm
        elif m == 'mov' and len(ops) == 2 and ops[1].type == ARM64_OP_REG:
            # register copies: some initialisers keep the metaclass address in a callee-saved register
            if ops[1].reg in regs:
                regs[ops[0].reg] = regs[ops[1].reg]
            else:
                regs.pop(ops[0].reg, None)
        elif m == 'ldr' and len(ops) == 2 and ops[1].type == ARM64_OP_MEM and isinstance(regs.get(ops[1].mem.base), int):
            regs[ops[0].reg] = ('ld', regs[ops[1].mem.base] + ops[1].mem.disp)
        elif m == 'bl':
            x0, x1, x2, w3 = (regs.get(r) for r in (ARM64_REG_X0, ARM64_REG_X1, ARM64_REG_X2, ARM64_REG_W3))
            if isinstance(x1, int) and ca <= x1 < ca + cl and isinstance(w3, int) and x2 is not None:
                out.append((kc.cstr(x1), x0, x2, w3, i.address))
            regs = {}       # a call clobbers the argument registers
        elif m in ('ret', 'retab', 'b', 'br', 'braa'):
            regs = {}
    return out


def classes(kc, bundles):
    """{bundle: [(name, size, super name, meta address or None)]}, one line a class.

    A class shows up more than once (the static initialiser and the MetaClass constructor both make the call);
    we keep one entry per name and prefer the one whose metaclass address is known."""
    raw = {b: scan(kc, b) for b in bundles}
    # some metaclass constructors get `this` from a caller we do not see; the kernel's symbols may still name
    # Class::gMetaClass, which is the address we want
    gmeta = {v[:-len('::gMetaClass')]: k for k, v in kc.syms.items() if v.endswith('::gMetaClass')}
    metas = {}
    for b in bundles:
        for n, x0, x2, sz, at in raw[b]:
            if isinstance(x0, int):
                metas[x0] = n
    res = {}
    for b in bundles:
        seen = {}
        for n, x0, x2, sz, at in raw[b]:
            if isinstance(x2, tuple):            # loaded from the GOT: a metaclass outside this kext
                t = kc.ptr(kc.q(x2[1]))
                sup = metas.get(t) or kc.syms.get(t) or '%#x' % t
            elif isinstance(x2, int):
                sup = metas.get(x2) or kc.syms.get(x2) or '%#x' % x2
            else:
                sup = str(x2)
            if sup.startswith('0x') and kc.off(int(sup, 16)) is None:
                continue                        # not a metaclass call (a string and a size by chance)
            sup = sup.replace('::gMetaClass', '')
            meta = x0 if isinstance(x0, int) else gmeta.get(n)
            if n not in seen or (seen[n][3] is None and meta is not None):
                seen[n] = (n, sz, sup, meta)
        res[b] = list(seen.values())
    return res


def run(kc, bundles, as_json=False):
    res = classes(kc, bundles)
    if as_json:
        import json
        print(json.dumps({b: [{'name': n, 'size': sz, 'super': sup, 'meta': meta} for n, sz, sup, meta in v]
                          for b, v in res.items()}, indent=1))
        return
    for b in bundles:
        print('##', b, '(%d classes)' % len(res[b]))
        for n, sz, sup, meta in res[b]:
            print('  %-44s size=%#7x  super=%-36s meta=%s' % (n, sz, sup, '%#x' % meta if meta else '-'))
