"""Default inputs for the iogpu tools, read from akios.toml.

The tools only read their inputs. Everything they write goes to stdout or to an --out directory,
which defaults to [paths] out/iogpu/ (out/ is not in git).
"""
import glob, os

try:
    import tomllib
except ImportError:          # Python < 3.11
    import tomli as tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # tools/iogpu/ -> the repository
DATA = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data')

try:
    CONF = tomllib.load(open(os.path.join(ROOT, 'akios.toml'), 'rb'))
except OSError:
    CONF = {}


def _abs(p):
    p = os.path.expanduser(p)
    return p if os.path.isabs(p) else os.path.join(ROOT, p)


def get(section, key, default=None):
    """akios.toml [section] key as an absolute path, or DEFAULT when it is not set"""
    v = CONF.get(section, {}).get(key)
    return _abs(v) if v else default


def kernelcache():
    return get('iogpu', 'kernelcache')


def kdk():
    return get('iogpu', 'kdk')


def mac_kexts():
    return get('iogpu', 'mac_kexts')


def symbols():
    """the Symbols supplement's root: the directory that holds System/ (it sits one or two levels down)"""
    s = get('iogpu', 'symbols')
    if s and not os.path.isdir(os.path.join(s, 'System')):
        hits = sorted(glob.glob(os.path.join(s, '*', 'Symbols'))) + sorted(glob.glob(os.path.join(s, 'Symbols')))
        if hits:
            s = hits[0]
    return s


def symbols_db():
    return get('iogpu', 'symbols_db')


def dsc():
    """the dyld shared cache of the unpacked IPSW ([userland] ipsw), the file without a .NN suffix"""
    ipsw = get('userland', 'ipsw')
    if not ipsw:
        return None
    hits = sorted(glob.glob(os.path.join(ipsw, '*', 'dyld_shared_cache_arm64e')))
    return hits[0] if hits else None


def out(sub=''):
    return os.path.join(get('paths', 'out', os.path.join(ROOT, 'out')), 'iogpu', sub)


def need(path, what, flag):
    """stop with a clear message when an input is missing (no default in akios.toml and no flag given)"""
    if not path or not os.path.exists(path):
        raise SystemExit('iogpu: %s not found (%s); give %s or set it in akios.toml [iogpu]' % (what, path, flag))
    return path
