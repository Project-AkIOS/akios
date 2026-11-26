"""kc-tables / kc-diff: the IOExternalMethodDispatch and IOExternalTrap tables of a kext in a kernelcache.

IOExternalMethodDispatch is 24 bytes: a signed function pointer, then u32 scalarIn, structIn, scalarOut,
structOut (0xffffffff = variable size). IOExternalTrap is 24 bytes too: object (0), then a pointer-to-member
function (pointer + adjustment 0). We look for runs of such entries in the kext's __const whose pointers go to
code. User clients keep a second, "restricted" copy right after the table, in which the closed entries all point
to one stub (s_restricted / t_restricted); we report which entries stay open.

kc-diff dumps the tables of a (new) kernelcache and compares them with a saved reference (data/*.json), so a new
iOS release is checked before anything is built on its IOGPU interface.
"""
import collections, difflib, hashlib, json, os

VAR = 0xffffffff


def _shape_ok(kc, a):
    try:
        return all(v < 0x10000 or v == VAR for v in (kc.u32(a + 8 + 4 * j) for j in range(4)))
    except KeyError:
        return False


def _zero(kc, a, n):
    try:
        return not any(kc.read(a, n))
    except KeyError:
        return False


def _q0(kc, a):
    try:
        return kc.q(a) == 0
    except KeyError:
        return False


def _split(kc, run, fn, shape):
    """a table and its restricted copy sit back to back and read as one run: split a run of 2N entries whose
    halves have the same shapes and in which the second half sends several entries to one stub"""
    n = len(run) // 2
    if len(run) % 2 or n < 2:
        return [run]
    a, b = run[:n], run[n:]
    if shape and any(shape(x) != shape(y) for x, y in zip(a, b)):
        return [run]
    same = sum(1 for x, y in zip(a, b) if fn(x) == fn(y))
    stub, k = collections.Counter(fn(y) for y in b).most_common(1)[0]
    if same and k > 1 and stub not in {fn(x) for x in a}:
        return [a, b]
    return [run]


def find(kc, bundle, min_entries=4):
    """[{'kind': 'methods'|'traps', 'addr', 'entries': [{'fn', ...}]}] in address order"""
    code = {p[0]: p[2] for p in kc.pointers(bundle) if p[1] == '__const' and p[4] == '__text'}
    tables, used = [], set()
    # traps first: their pointers sit at +8, which would otherwise also look like a method table shifted by 8
    for a in sorted(code):
        e = a - 8
        if a in used or not (_q0(kc, e) and _q0(kc, e + 16)):
            continue
        run = []
        while e + 8 in code and _q0(kc, e) and _q0(kc, e + 16):
            run.append(e)
            e += 24
        if len(run) >= min_entries:
            used.update(x + 8 for x in run)
            for part in _split(kc, run, lambda x: code[x + 8], None):
                tables.append({'kind': 'traps', 'addr': part[0], 'entries': [{'fn': code[x + 8]} for x in part]})
    for a in sorted(code):
        if a in used or not _shape_ok(kc, a):
            continue
        run, b = [a], a
        while True:
            nb = b + 24
            if nb in code and nb not in used and _shape_ok(kc, nb):
                run.append(nb)
            elif _zero(kc, nb, 24) and nb + 24 in code and _shape_ok(kc, nb + 24):     # a null entry inside a table
                run.append(nb)
            else:
                break
            b = nb
        used.update(run)
        if len(run) < min_entries:
            continue
        for part in _split(kc, run, lambda x: code.get(x, 0), lambda x: tuple(kc.u32(x + 8 + 4 * j) for j in range(4))):
            ents = []
            for e in part:
                si, sti, so, sto = (kc.u32(e + 8 + 4 * j) for j in range(4))
                ents.append({'fn': code.get(e, 0), 'scalar_in': si, 'struct_in': sti, 'scalar_out': so, 'struct_out': sto})
            tables.append({'kind': 'methods', 'addr': part[0], 'entries': ents})
    tables.sort(key=lambda t: t['addr'])
    # a table directly followed by one of the same kind and length is the restricted copy
    for t, u in zip(tables, tables[1:]):
        if t['kind'] == u['kind'] and len(t['entries']) == len(u['entries']) and \
                u['addr'] == t['addr'] + 24 * len(t['entries']) and 'restricted_of' not in t:
            fns = [e['fn'] for e in u['entries']]
            stub, n = collections.Counter(fns).most_common(1)[0]
            if n > 1:
                u['restricted_of'] = t['addr']
                u['stub'] = stub
                u['open'] = [i for i, (x, y) in enumerate(zip(t['entries'], u['entries'])) if x['fn'] == y['fn']]
    return tables


def shape_hash(entries):
    """a short hash over the argument shapes only (handlers move with every build, shapes should not)"""
    s = ';'.join('%d,%d,%d,%d' % (e['scalar_in'], e['struct_in'], e['scalar_out'], e['struct_out']) for e in entries)
    return hashlib.sha256(s.encode()).hexdigest()[:16]


def _sz(v):
    return 'var' if v == VAR else '%#x' % v if v > 9 else str(v)


def show(kc, bundle, tables):
    print('##', bundle)
    for t in tables:
        extra = ''
        if 'restricted_of' in t:
            extra = '  restricted copy of %#x; stub %s; open: %s' % (t['restricted_of'], kc.name(t['stub']), _ranges(t['open']))
        print('%s @%#x entries=%d%s' % (t['kind'], t['addr'], len(t['entries']), extra))
        if t['kind'] == 'methods' and 'restricted_of' not in t:
            print('  shape hash %s; columns: sel, handler, scalarIn, structIn, scalarOut, structOut' % shape_hash(t['entries']))
        if 'restricted_of' in t:
            continue
        for k, e in enumerate(t['entries']):
            nm = kc.syms.get(e['fn'], '')
            if t['kind'] == 'methods':
                print('  %3d %#x  %3s %5s %3s %5s  %s' % (k, e['fn'], _sz(e['scalar_in']), _sz(e['struct_in']),
                                                        _sz(e['scalar_out']), _sz(e['struct_out']), nm))
            else:
                print('  %3d %#x  %s' % (k, e['fn'], nm))


def _ranges(xs):
    out, i = [], 0
    while i < len(xs):
        j = i
        while j + 1 < len(xs) and xs[j + 1] == xs[j] + 1:
            j += 1
        out.append(str(xs[i]) if i == j else '%d-%d' % (xs[i], xs[j]))
        i = j + 1
    return '{' + ', '.join(out) + '}'


def to_json(tables):
    """tables as plain data, addresses as hex strings, var sizes as "var" """
    def ent(e):
        r = {'handler': '%#x' % e['fn']}
        for k in ('scalar_in', 'struct_in', 'scalar_out', 'struct_out'):
            if k in e:
                r[k] = 'var' if e[k] == VAR else e[k]
        return r
    out = []
    for t in tables:
        j = {'kind': t['kind'], 'addr': '%#x' % t['addr'], 'count': len(t['entries'])}
        if 'restricted_of' in t:
            j.update(restricted_of='%#x' % t['restricted_of'], stub='%#x' % t['stub'], open=t['open'])
        else:
            j['entries'] = [ent(e) for e in t['entries']]
            if t['kind'] == 'methods':
                j['shape_hash'] = shape_hash(t['entries'])
        out.append(j)
    return out


def run(kc, bundle, as_json=False):
    tables = find(kc, bundle)
    if as_json:
        print(json.dumps({'kext': bundle, 'tables': to_json(tables)}, indent=1))
    else:
        show(kc, bundle, tables)


# --- kc-diff -----------------------------------------------------------------------------------------------------

def _unvar(v):
    return VAR if v == 'var' else v


def _best(ref_entries, cands, kind):
    """the new table of KIND that agrees with the reference at the most indices"""
    def score(t):
        if kind == 'traps':
            return -abs(len(t['entries']) - len(ref_entries))
        return sum(1 for r, e in zip(ref_entries, t['entries'])
                   if all(_unvar(r[k]) == e[k] for k in ('scalar_in', 'struct_in', 'scalar_out', 'struct_out')))
    c = [t for t in cands if t['kind'] == kind and 'restricted_of' not in t]
    return max(c, key=score) if c else None


def diff(kc, ref_path, save=None):
    """print what changed against the reference; returns the number of differences"""
    ref = json.load(open(ref_path))
    bundle = ref['kext']
    tables = find(kc, bundle)
    print('reference: %s (%s, %s)' % (ref_path, ref.get('build'), ref.get('device')))
    print('kernelcache: %s, kext %s: %d tables' % (kc.path, bundle, len(tables)))
    bad = 0
    m, t = ref['methods'], ref['traps']
    nm = _best(m['entries'], tables, 'methods')
    if not nm:
        print('methods: NO method table found')
        return 1
    print('methods: reference %d @%s, now %d @%#x' % (len(m['entries']), m['addr'], len(nm['entries']), nm['addr']))
    keys = ('scalar_in', 'struct_in', 'scalar_out', 'struct_out')
    rs = [tuple(_unvar(r[k]) for k in keys) for r in m['entries']]
    es = [tuple(e[k] for k in keys) for e in nm['entries']]
    # align by shape, not by index: one selector added or removed would otherwise shift every later one.
    # Shapes repeat (many are (1 0 0 0)), so a match here is a lead; confirm moved selectors with the KDK names.
    for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, rs, es, autojunk=False).get_opcodes():
        if op == 'equal':
            if i1 != j1:
                bad += 1
                print('  sel %d-%d (%s .. %s) now at %d-%d' % (i1, i2 - 1, m['entries'][i1]['name'], m['entries'][i2 - 1]['name'], j1, j2 - 1))
            continue
        bad += 1
        for i in range(i1, i2):
            print('  sel %2d %-36s %s  %s' % (i, m['entries'][i]['name'], _fmt(rs[i]), 'removed' if op == 'delete' else 'replaced'))
        for j in range(j1, j2):
            print('  new sel %2d %s  %s' % (j, _fmt(es[j]), 'added' if op == 'insert' else 'in its place'))
    h = shape_hash(nm['entries'])
    print('  shape hash %s (reference %s)%s' % (h, m.get('shape_hash'), '' if h == m.get('shape_hash') else '  CHANGED'))
    rr = next((x for x in tables if x.get('restricted_of') == nm['addr']), None)
    if rr is None:
        print('  restricted copy: not found')
    elif rr['open'] != ref['methods_restricted']['open']:
        bad += 1
        print('  restricted copy: open %s, reference %s' % (_ranges(rr['open']), _ranges(ref['methods_restricted']['open'])))
    else:
        print('  restricted copy: same open set %s' % _ranges(rr['open']))
    nt = _best(t['entries'], tables, 'traps')
    if not nt:
        print('traps: NO trap table found')
        return bad + 1
    print('traps: reference %d @%s, now %d @%#x' % (len(t['entries']), t['addr'], len(nt['entries']), nt['addr']))
    if len(nt['entries']) != len(t['entries']):
        bad += 1
        print('  trap count changed')
    rt = next((x for x in tables if x.get('restricted_of') == nt['addr']), None)
    if rt is not None and rt['open'] != ref['traps_restricted']['open']:
        bad += 1
        print('  restricted copy: open %s, reference %s' % (_ranges(rt['open']), _ranges(ref['traps_restricted']['open'])))
    elif rt is not None:
        print('  restricted copy: same open set %s' % _ranges(rt['open']))
    print('result: %s' % ('same interface as the reference' if not bad else '%d differences' % bad))
    if save:
        _save(ref, nm, rr, nt, rt, save)
    return bad


def _save(ref, nm, rr, nt, rt, path):
    """the new tables as a reference file; names and callers carry over only where the shape is unchanged at the
    same index (a moved or new selector gets name null: match it by hand against the macOS KDK)"""
    keys = ('scalar_in', 'struct_in', 'scalar_out', 'struct_out')
    ents = []
    for i, e in enumerate(nm['entries']):
        r = ref['methods']['entries'][i] if i < len(ref['methods']['entries']) else None
        same = r is not None and all(_unvar(r[k]) == e[k] for k in keys)
        x = {'sel': i, 'name': r['name'] if same else None, 'macos_sel': r.get('macos_sel') if same else None}
        x.update({k: 'var' if e[k] == VAR else e[k] for k in keys})
        x.update(handler='%#x' % e['fn'], caller=r.get('caller') if same else None)
        ents.append(x)
    tents = []
    for i, e in enumerate(nt['entries']):
        r = ref['traps']['entries'][i] if i < len(ref['traps']['entries']) else {}
        tents.append({'trap': i, 'name': r.get('name'), 'args': r.get('args'), 'handler': '%#x' % e['fn'],
                      'caller': r.get('caller')})
    out = {'about': 'made by iogpu kc-diff --save from %s; names carried over from %s where shapes match: '
                    'check every null and every trap by hand' % (os.path.basename(ref.get('kernelcache', '')) or 'a kernelcache',
                                                                 ref.get('build')),
           'build': None, 'device': None, 'kext': ref['kext'],
           'methods': {'addr': '%#x' % nm['addr'], 'count': len(ents), 'shape_hash': shape_hash(nm['entries']), 'entries': ents},
           'methods_restricted': {'addr': '%#x' % rr['addr'], 'stub': '%#x' % rr['stub'], 'open': rr['open']} if rr else None,
           'traps': {'addr': '%#x' % nt['addr'], 'count': len(tents), 'entries': tents},
           'traps_restricted': {'addr': '%#x' % rt['addr'], 'stub': '%#x' % rt['stub'], 'open': rt['open']} if rt else None}
    open(path, 'w').write(dumps(out) + '\n')
    print('saved', path, '(set build and device in it)')


def dumps(o, ind=0):
    """JSON with one table entry a line: the reference files are read by people as much as by kc-diff"""
    p = ' ' * ind
    if isinstance(o, dict) and any(isinstance(x, (dict, list)) for x in o.values()):
        return '{\n' + ',\n'.join(p + ' ' + json.dumps(k) + ': ' + dumps(x, ind + 1) for k, x in o.items()) + '\n' + p + '}'
    if isinstance(o, list) and o and isinstance(o[0], dict):
        return '[\n' + ',\n'.join(p + ' ' + json.dumps(x) for x in o) + '\n' + p + ']'
    return json.dumps(o)


def _fmt(s):
    return '-' if s is None else '(%s)' % ' '.join(_sz(v) for v in s)
