"""dsc-dis: disassemble functions of a dyld-cache library, with call targets and string references named.

Code is read from the device's dyld shared cache; function bounds come from the Symbols supplement Mach-O of the
library; names from the symbol database. Calls into other images go through branch islands (adrp/add/br), which
we follow one step. adrp+add/ldr pairs that land on a C string or a CFString get the text as a comment.

  dsc-dis LIB FUNC_REGEX             print the matching functions
  dsc-dis LIB --calls CALLEE_REGEX   every call to a matching callee, with the constants last put into w1-w3
                                     (e.g. IOConnectCall* selectors and IOConnectTrap numbers)
"""
import re, struct

import capstone
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG

from .symbols import decode


class Dis:
    def __init__(self, cache, db, files, image):
        self.c, self.db, self.files, self.image = cache, db, files, image
        files.load(image)
        self.md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.md.detail = True
        self._name = {}

    def name(self, a):
        if a in self._name:
            return self._name[a]
        n = self.db.name(a)
        if not n:
            b = self.c.read(a, 12)
            if b:
                ins = list(self.md.disasm(b, a))
                if len(ins) >= 3 and [i.mnemonic for i in ins[:3]] == ['adrp', 'add', 'br']:
                    t = ins[0].operands[1].imm + ins[1].operands[2].imm
                    n = self.db.name(t) or hex(t)
        self._name[a] = n
        return n

    def funcs(self):
        s = self.files.text_symbols(self.image)
        out = []
        for i, (a, n) in enumerate(s):
            e = s[i + 1][0] if i + 1 < len(s) else a + 0x400
            out.append((a, min(e, a + 0x8000), n))
        return out

    def dis(self, a, e):
        b = self.c.read(a, e - a)
        return list(self.md.disasm(b, a)) if b else []

    def _string(self, a, cf):
        b = self.files.read(a, 64) or self.c.read(a, 64)
        if not b:
            return None
        s = b.split(b'\0')[0]
        if len(s) > 3 and all(32 <= c < 127 for c in s):
            return '"%s"' % s.decode()
        if cf and len(b) >= 24:
            # CFString: isa, flags, then the pointer to the characters at +16
            t = decode(struct.unpack_from('<Q', b, 16)[0])
            bb = self.files.read(t, 64) or self.c.read(t, 64)
            if bb:
                ss = bb.split(b'\0')[0]
                if len(ss) > 2 and all(32 <= c < 127 for c in ss):
                    return 'CF"%s"' % ss.decode()
        return '@%#x' % a

    def listing(self, a, e):
        page = {}
        for i in self.dis(a, e):
            s = '%x: %-8s %s' % (i.address, i.mnemonic, i.op_str)
            ops = i.operands
            if i.mnemonic in ('bl', 'b') and ops and ops[0].type == ARM64_OP_IMM:
                n = self.name(ops[0].imm)
                if n:
                    s += '   ; ' + n
            elif i.mnemonic == 'adrp':
                page[ops[0].reg] = ops[1].imm & 0xffffffffffffffff
            elif i.mnemonic == 'add' and len(ops) == 3 and ops[2].type == ARM64_OP_IMM and ops[1].reg in page:
                t = self._string(page[ops[1].reg] + ops[2].imm, True)
                if t:
                    s += '   ; ' + t
            elif i.mnemonic == 'ldr' and len(ops) == 2 and ops[1].type == ARM64_OP_MEM and ops[1].mem.base in page:
                at = page[ops[1].mem.base] + ops[1].mem.disp
                s += '   ; @%#x' % at
                # a selector reference or GOT slot: name what it points to
                v = self.files.q(at)
                if v:
                    t = decode(v)
                    n = self.db.name(t)
                    if n:
                        s += ' -> ' + n
                    else:
                        txt = self._string(t, False)
                        if txt and txt.startswith('"'):
                            s += ' -> ' + txt
            # a register written by anything but adrp no longer holds a page address
            if i.mnemonic != 'adrp' and ops and ops[0].type == ARM64_OP_REG:
                page.pop(ops[0].reg, None)
            yield s


def run(dis, regex=None, calls=None):
    if calls:
        rx = re.compile(calls)
        for a, e, n in dis.funcs():
            regs = {}
            for i in dis.dis(a, e):
                ops = i.operands
                if i.mnemonic in ('mov', 'movz', 'orr') and len(ops) == 2 and ops[1].type == ARM64_OP_IMM:
                    regs[i.reg_name(ops[0].reg).replace('x', 'w')] = ops[1].imm
                if i.mnemonic == 'bl':
                    t = dis.name(ops[0].imm) or ''
                    if rx.search(t):
                        print('%-60s %x %-28s w1=%s w2=%s w3=%s' % (n[:60], i.address, t, regs.get('w1'), regs.get('w2'), regs.get('w3')))
                    regs = {}
        return
    rx = re.compile(regex)
    for a, e, n in dis.funcs():
        if rx.search(n):
            print('====', n, hex(a))
            for l in dis.listing(a, e):
                print(l)
