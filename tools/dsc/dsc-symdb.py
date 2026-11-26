#!/usr/bin/env python3
"""A searchable registry of every symbol of an iOS build's dyld shared cache (local ones too), made once from a "Symbols"
directory of the kind Xcode's DeviceSupport keeps (one Mach-O per library, symbol tables at the cache's addresses), into
an SQLite file: for naming addresses from backtraces and disassembly, and for finding functions, vtables, ivars and
selectors by name -- without the cache's .symbols file, which sysimage/unpack.sh leaves out.

usage: dsc-symdb.py build SYMBOLS_DIR DB         make DB (every 64-bit Mach-O under SYMBOLS_DIR; names demangled)
       dsc-symdb.py find DB PATTERN [LIMIT]       symbols whose demangled (or raw) name matches PATTERN (SQL LIKE,
                                                  * and ? as wildcards; case-insensitive), with library and address
       dsc-symdb.py re DB REGEX [LIMIT]           the same with a Python regular expression (slower: scans names)
       dsc-symdb.py at DB ADDRESS...              the symbol at or below each address (+distance), and its library
       dsc-symdb.py image DB NAME [PATTERN]       one library's symbols (NAME: last path component or a LIKE pattern)
       dsc-symdb.py vtable DB CLASS               a C++ class's vtable symbol(s) and address(es) (_ZTV...)
       dsc-symdb.py stats DB                      how many libraries and symbols

DB default for the iOS 27.2.1 (24B156) build: ~/akita/ref/symbols-24B156.sqlite (env HAKIOS_SYMDB). Nothing of
Apple's is kept in the repository: it reads files you have.
"""
import os
import re
import sqlite3
import struct
import subprocess
import sys

LC_SYMTAB = 0x2
DEFAULT_DB = os.environ.get('HAKIOS_SYMDB', os.path.expanduser('~/akita/ref/symbols-24B156.sqlite'))


def macho_symbols(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 32 or struct.unpack_from('<I', data, 0)[0] != 0xfeedfacf:
        return None
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, at)
        if cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', data, at + 8)
            out = []
            for i in range(nsyms):
                strx, n_type, n_sect, _, value = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
                if n_type & 0xe0 or n_sect == 0 or value == 0:
                    continue
                end = data.index(b'\0', stroff + strx)
                out.append((value, data[stroff + strx:end].decode('utf-8', 'replace'), 0 if n_type & 0x01 else 1))
            return out
        if size == 0:
            break
        at += size
    return []


def demangle(names):
    """c++filt over a batch (names that are not C++ come back as they are); Swift names are left alone"""
    try:
        p = subprocess.run(['c++filt', '-_'], input='\n'.join(names) + '\n', capture_output=True, text=True, check=True)
        out = p.stdout.split('\n')
        if len(out) >= len(names):
            return out[:len(names)]
    except (OSError, subprocess.CalledProcessError):
        pass
    return names


def build(root, db):
    if os.path.exists(db):
        os.remove(db)
    con = sqlite3.connect(db)
    con.executescript('''
        CREATE TABLE images (id INTEGER PRIMARY KEY, path TEXT, base TEXT);
        CREATE TABLE syms (addr INTEGER, name TEXT, demangled TEXT, local INTEGER, image INTEGER);
    ''')
    nimg = nsym = 0
    for dirpath, _, files in os.walk(root):
        for fn in files:
            path = os.path.join(dirpath, fn)
            if os.path.islink(path):
                continue
            try:
                syms = macho_symbols(path)
            except (OSError, struct.error, ValueError):
                syms = None
            if not syms:
                continue
            rel = '/' + os.path.relpath(path, root)
            cur = con.execute('INSERT INTO images (path, base) VALUES (?, ?)', (rel, fn))
            img = cur.lastrowid
            names = [n for _, n, _ in syms]
            dem = []
            for i in range(0, len(names), 20000):
                dem += demangle(names[i:i + 20000])
            con.executemany('INSERT INTO syms VALUES (?, ?, ?, ?, ?)',
                            [(a, n, d if d != n else None, l, img) for (a, n, l), d in zip(syms, dem)])
            nimg += 1
            nsym += len(syms)
            if nimg % 250 == 0:
                print('dsc-symdb: %d libraries, %d symbols...' % (nimg, nsym), file=sys.stderr)
    con.executescript('''
        CREATE INDEX syms_addr ON syms (addr);
        CREATE INDEX syms_name ON syms (name COLLATE NOCASE);
        CREATE INDEX syms_dem ON syms (demangled COLLATE NOCASE);
        CREATE INDEX images_base ON images (base);
    ''')
    con.commit()
    print('dsc-symdb: %s: %d libraries, %d symbols' % (db, nimg, nsym))


def show(rows):
    for addr, name, dem, local, path in rows:
        print('%#x %s %s  [%s]' % (addr, 'l' if local else 'g', dem or name, path))


def like(pattern):
    p = pattern.replace('*', '%').replace('?', '_')
    return p if '%' in p or '_' in p else '%' + p + '%'


def main():
    a = sys.argv[1:]
    if not a:
        sys.exit(__doc__)
    cmd = a[0]
    if cmd == 'build':
        if len(a) < 2:
            sys.exit(__doc__)
        build(a[1], a[2] if len(a) > 2 else DEFAULT_DB)
        return
    # the DB argument may be left out when the next one is not a file
    db = DEFAULT_DB
    if len(a) > 1 and (a[1].endswith('.sqlite') or os.path.isfile(a[1])):
        db = a[1]
        a = [a[0]] + a[2:]
    con = sqlite3.connect(db)
    q = 'SELECT s.addr, s.name, s.demangled, s.local, i.path FROM syms s JOIN images i ON i.id = s.image '
    if cmd == 'find':
        limit = int(a[2]) if len(a) > 2 else 200
        p = like(a[1])
        show(con.execute(q + 'WHERE s.demangled LIKE ? OR s.name LIKE ? ORDER BY s.addr LIMIT ?', (p, p, limit)))
    elif cmd == 're':
        limit = int(a[2]) if len(a) > 2 else 200
        r = re.compile(a[1])
        n = 0
        for row in con.execute(q + 'ORDER BY s.addr'):
            if r.search(row[2] or row[1]):
                show([row])
                n += 1
                if n >= limit:
                    break
    elif cmd == 'at':
        for word in a[1:]:
            addr = int(word, 0)
            row = con.execute(q + 'WHERE s.addr <= ? ORDER BY s.addr DESC LIMIT 1', (addr,)).fetchone()
            if row is None:
                print('%#x ?' % addr)
            else:
                print('%#x %s+%#x  [%s]' % (addr, row[2] or row[1], addr - row[0], row[4]))
    elif cmd == 'image':
        img = a[1]
        ids = [r[0] for r in con.execute('SELECT id FROM images WHERE base = ? OR path LIKE ?', (img, like(img)))]
        if not ids:
            sys.exit('dsc-symdb: no library %s' % img)
        p = like(a[2]) if len(a) > 2 else '%'
        show(con.execute(q + 'WHERE s.image IN (%s) AND (s.demangled LIKE ? OR s.name LIKE ?) ORDER BY s.addr'
                         % ','.join('?' * len(ids)), ids + [p, p]))
    elif cmd == 'vtable':
        show(con.execute(q + "WHERE s.demangled LIKE ? ORDER BY s.addr", ('vtable for %' + a[1] + '%',)))
    elif cmd == 'stats':
        print('%d libraries, %d symbols' % (con.execute('SELECT COUNT(*) FROM images').fetchone()[0],
                                            con.execute('SELECT COUNT(*) FROM syms').fetchone()[0]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
