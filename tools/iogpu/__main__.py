"""iogpu: read Apple's IOGPU interface out of iOS and macOS binaries (GPU plan, phase 0). Read-only on inputs.

  kc-classes KEXT...        OSMetaClass name, size and super of every class in kexts of a kernelcache
  kc-tables KEXT            IOExternalMethodDispatch and IOExternalTrap tables of a kext
  kc-diff [--ref JSON]      dump IOGPUFamily's tables from a (new) kernelcache and compare with the saved reference
  kc-vtables KEXT...        vtables of kexts, slots named from macOS kexts (--mac), vendor overrides (--pairs)
  kdk SUB ...               macOS KDK kexts: extract, syms, imports, dump, vtable, overrides
  objc-supers CLASS...      Objective-C superclass chains (+ method lists) from the Symbols supplement
  dsc-dis LIB REGEX         disassemble dyld-cache functions with named calls and strings

Defaults come from akios.toml [iogpu] (and [userland] ipsw for the dyld cache). Run as
`python3 tools/iogpu CMD ...` from the repository, or `python3 -m iogpu CMD ...` from tools/.
"""
import argparse, os, sys

if __package__ in (None, ''):          # run as `python3 tools/iogpu`: make the package importable
    sys.path[0] = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    __package__ = 'iogpu'

from iogpu import config  # noqa: E402
from iogpu.kdk import DEFAULT_KEXTS  # noqa: E402

REF = os.path.join(config.DATA, 'iogpu-24B156-selectors.json')


def _kc(a):
    from iogpu.kc import Kernelcache
    return Kernelcache(config.need(a.kc or config.kernelcache(), 'kernelcache', '--kc'), a.kc_symbols)


def _macs(a):
    """the --mac kexts, or by default the IOGPU ones in [iogpu] mac_kexts"""
    from iogpu.kdk import find_kext
    if a.mac:
        return [find_kext(m, [config.mac_kexts() or '.']) for m in a.mac]
    d = config.mac_kexts()
    if not d:
        return None
    return [os.path.join(d, 'mac-%s.arm64e' % n) for n in ('IOGPUFamily', 'AGXG13G', 'AppleParavirtGPUIOGPUFamily')
            if os.path.exists(os.path.join(d, 'mac-%s.arm64e' % n))] or None


def main():
    p = argparse.ArgumentParser(prog='iogpu', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)

    def kcargs(s):
        s.add_argument('--kc', help='decompressed kernelcache Mach-O (default: akios.toml [iogpu] kernelcache)')
        s.add_argument('--kc-symbols', help='JSON {address: name} of kernel symbols (default: KC.symbols.json if present)')

    s = sub.add_parser('kc-classes', help='OSMetaClass name/size/super per kext')
    kcargs(s)
    s.add_argument('kexts', nargs='+', metavar='KEXT', help='bundle id, e.g. com.apple.iokit.IOGPUFamily')
    s.add_argument('--json', action='store_true')

    s = sub.add_parser('kc-tables', help='IOExternalMethodDispatch + IOExternalTrap tables of a kext')
    kcargs(s)
    s.add_argument('kext', nargs='?', default='com.apple.iokit.IOGPUFamily')
    s.add_argument('--json', action='store_true')
    s.add_argument('--list-kexts', action='store_true', help='just list the kexts in the kernelcache')

    s = sub.add_parser('kc-diff', help='compare a kernelcache\'s IOGPUFamily tables with the saved reference')
    kcargs(s)
    s.add_argument('--ref', default=REF, help='reference JSON (default: data/iogpu-24B156-selectors.json)')
    s.add_argument('--save', metavar='JSON', help='also write the new tables as a reference file (names carried over where shapes match)')

    s = sub.add_parser('kc-vtables', help='iOS vtables, named from the macOS KDK')
    kcargs(s)
    s.add_argument('kexts', nargs='*', metavar='KEXT', default=['com.apple.iokit.IOGPUFamily'])
    s.add_argument('--mac', action='append', help='macOS kext (path or NAME for mac-NAME.arm64e in [iogpu] mac_kexts); repeatable')
    s.add_argument('--class', dest='cls', help='list the slots of one class, named')
    s.add_argument('--pairs', help='vendor/base class pairs file: list vendor overrides (e.g. data/pairs-AGXG14P.txt)')
    s.add_argument('--json', action='store_true', help='all vtables as JSON (addresses and slot targets)')

    s = sub.add_parser('kdk', help='macOS KDK kext reader')
    ks = s.add_subparsers(dest='sub', required=True)
    x = ks.add_parser('extract', help='extract kexts from the KDK pkg and thin them to arm64e')
    x.add_argument('names', nargs='*', help='kext names (default: %s)' % ' '.join(DEFAULT_KEXTS))
    x.add_argument('--kdk', help='KDK directory holding pkg/KDK.pkg/Payload (default: akios.toml [iogpu] kdk)')
    x.add_argument('--out', help='output directory (default: [paths] out/iogpu/kdk)')
    for name, h in (('syms', 'defined symbols'), ('imports', 'chained-fixup imports'), ('dump', 'a table at a symbol'),
                    ('vtable', 'vtable slots of a class'), ('overrides', 'slots a class implements itself')):
        y = ks.add_parser(name, help=h)
        y.add_argument('bin', metavar='KEXT', help='thin arm64e kext, or NAME for mac-NAME.arm64e in [iogpu] mac_kexts')
        if name == 'syms':
            y.add_argument('regex', nargs='?')
        elif name == 'imports':
            y.add_argument('-C', '--demangle', action='store_true')
        elif name == 'dump':
            y.add_argument('table', metavar='SYMBOL', help='symbol (mangled or demangled) or 0xADDRESS')
            y.add_argument('--entry', type=int, default=24, help='entry size (default 24, IOExternalMethodDispatch)')
            y.add_argument('--count', type=int, help='entries (default: up to the next symbol)')
        elif name == 'vtable':
            y.add_argument('cls', metavar='CLASS')
        else:
            y.add_argument('classes', nargs='*', metavar='CLASS', help='default: every class with a vtable')

    s = sub.add_parser('objc-supers', help='ObjC superclass chains and method lists')
    s.add_argument('classes', nargs='*', metavar='CLASS')
    s.add_argument('--prefix', help='also every class whose name starts with this')
    s.add_argument('--methods', action='store_true', help='list each class\'s own methods, marking overrides')
    s.add_argument('--vs', metavar='BASE', help='what the classes below BASE override of BASE and what they inherit')
    s.add_argument('--symbols', help='Symbols supplement (default: akios.toml [iogpu] symbols)')
    s.add_argument('--db', help='symbol database (default: akios.toml [iogpu] symbols_db)')

    s = sub.add_parser('dsc-dis', help='disassemble a dyld-cache library with names and strings')
    s.add_argument('lib', metavar='LIB', help='image path (System/Library/...) or file name (IOGPU, Metal)')
    s.add_argument('regex', nargs='?', metavar='FUNC_REGEX')
    s.add_argument('--calls', metavar='CALLEE_REGEX', help='list calls to matching callees with w1-w3 constants')
    s.add_argument('--symbols')
    s.add_argument('--db')
    s.add_argument('--dsc', help='dyld_shared_cache_arm64e (default: from akios.toml [userland] ipsw)')

    a = p.parse_args()
    try:
        dispatch(a, p)
    except BrokenPipeError:            # `| head` is normal use
        sys.stderr.close()


def dispatch(a, p):
    if a.cmd == 'kc-classes':
        from iogpu import classes
        classes.run(_kc(a), a.kexts, a.json)
    elif a.cmd == 'kc-tables':
        from iogpu import tables
        k = _kc(a)
        if a.list_kexts:
            print('\n'.join(k.kexts()))
        else:
            tables.run(k, a.kext, a.json)
    elif a.cmd == 'kc-diff':
        from iogpu import tables
        sys.exit(1 if tables.diff(_kc(a), a.ref, a.save) else 0)
    elif a.cmd == 'kc-vtables':
        from iogpu import vtables
        pairs = vtables.read_pairs(a.pairs) if a.pairs else None
        vtables.run(_kc(a), a.kexts, _macs(a), a.cls, pairs, a.json)
    elif a.cmd == 'kdk':
        from iogpu import kdk
        if a.sub == 'extract':
            kdk.cmd_extract(config.need(a.kdk or config.kdk(), 'KDK', '--kdk'), a.names or DEFAULT_KEXTS,
                            a.out or config.out('kdk'))
            return
        m = kdk.MK(kdk.find_kext(a.bin, [config.mac_kexts() or '.', config.out('kdk')]))
        if a.sub == 'syms':
            kdk.cmd_syms(m, a.regex)
        elif a.sub == 'imports':
            kdk.cmd_imports(m, a.demangle)
        elif a.sub == 'dump':
            kdk.cmd_dump(m, a.table, a.entry, a.count)
        elif a.sub == 'vtable':
            kdk.cmd_vtable(m, a.cls)
        else:
            kdk.cmd_overrides(m, a.classes)
    elif a.cmd == 'objc-supers':
        from iogpu import objc, symbols
        if not a.classes and not a.prefix:
            p.error('objc-supers: give CLASS names or --prefix')
        objc.run(symbols.SymDB(a.db or config.symbols_db()), symbols.SymbolFiles(a.symbols or config.symbols()),
                 a.classes, a.prefix, a.methods, a.vs)
    elif a.cmd == 'dsc-dis':
        from iogpu import dsc, symbols
        if not a.regex and not a.calls:
            p.error('dsc-dis: give FUNC_REGEX or --calls CALLEE_REGEX')
        db = symbols.SymDB(a.db or config.symbols_db())
        d = dsc.Dis(symbols.DyldCache(a.dsc or config.dsc()), db, symbols.SymbolFiles(a.symbols or config.symbols()),
                    db.image(a.lib))
        dsc.run(d, a.regex, a.calls)


if __name__ == '__main__':
    main()
