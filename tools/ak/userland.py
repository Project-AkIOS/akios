"""ak userland: out/akios.apfs, the RAM disk the kernel starts from, out of the userland repository.

userland/userland.toml says what goes in; akios.toml [userland] says where the tools, the SDK, the unpacked IPSW and
the firmware are. The steps: compile (akinit static against libmini, the rest against the iOS SDK), sign (ldid),
Apple's launchd jobs with userland/jobs/ applied and ours added, the data volume's skeleton, the inject list, the boot
plan (checked by a host build of akinit), the firmware; everything staged in work/userland/image/ and made into an
APFS image; read back by an independent reader and compared with the staging. Logs: [paths] logs/userland-*/.
"""
import copy, glob, os, plistlib, re, shutil, subprocess, sys, time

try:
    import tomllib
except ImportError:          # Python < 3.11
    import tomli as tomllib


class Build:
    def __init__(self, root, conf, path, rundir, say, args):
        self.root, self.conf, self.say, self.args = root, conf, say, args
        u = conf['userland']
        ex = lambda p: os.path.expanduser(p) if os.path.isabs(os.path.expanduser(p)) else os.path.join(root, p)
        self.repo = ex(u['repo'])
        self.man = tomllib.load(open(os.path.join(self.repo, 'userland.toml'), 'rb'))
        self.vars = {k: ex(v) for k, v in u.items() if isinstance(v, str) and k not in ('arch', 'sdk_target')}
        self.arch = u.get('arch', 'arm64e')
        self.work = os.path.join(path('work') if 'work' in conf['paths'] else os.path.join(root, 'work'), 'userland')
        self.out = path('out')
        self.logdir = rundir('userland')
        self.log = os.path.join(self.logdir, 'build.log')
        self.obj = os.path.join(self.work, 'obj')
        self.img = os.path.join(self.work, 'image')
        tc = self.vars['toolchain']
        self.env = dict(os.environ, PATH=tc + ':' + os.environ['PATH'], XNU_SDK_VERSION=u.get('sdk_target', '16.5'))
        self.notes = []

    # ---- helpers ----
    def x(self, s):
        """@NAME -> akios.toml [userland] NAME"""
        return re.sub(r'@([a-z_]+)', lambda m: self.vars[m.group(1)], s)

    def src(self, s):
        s = self.x(s)
        return s if os.path.isabs(s) else os.path.join(self.repo, s)

    def run(self, args, cwd=None, check=True):
        with open(self.log, 'a') as f:
            f.write('$ ' + ' '.join(args) + '\n')
            f.flush()
            r = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, env=self.env, cwd=cwd)
        if check and r.returncode != 0:
            sys.exit('ak: %s failed (%d), see %s' % (os.path.basename(args[0]), r.returncode, self.log))
        return r.returncode

    def note(self, *a):
        line = ' '.join(str(x) for x in a)
        self.notes.append(line)
        with open(self.log, 'a') as f:
            f.write('# ' + line + '\n')

    def staged(self, rel):
        p = os.path.join(self.img, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        return p

    # ---- compiling ----
    def cc_static(self):
        a = self.man['akinit']
        tc = self.vars['toolchain']
        cc = [tc + '/xnu-clang', '-arch', self.arch, '-Os', '-static', '-nostdlib', '-ffreestanding', '-fno-stack-protector',
              '-fno-builtin', '-Wall', '-Wextra', '-I' + self.src('akinit/libmini')]
        out = os.path.join(self.obj, 'akinit')
        self.run(cc + ['-Wl,-e,_start', '-Wl,-pagezero_size,0x100000000', '-o', out] +
                 [self.src(s) for s in a['src'] + a['libmini']])
        shutil.copy(out, self.staged(a['out']))
        os.chmod(self.staged(a['out']), 0o755)

    def host_akinit(self):
        """akinit on the PC against test/fake.c: checks the plan here and runs the host test"""
        a = self.man['akinit']
        out = os.path.join(self.obj, 'akinit-host')
        self.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Wextra', '-fno-builtin', '-I' + self.src('akinit/test'), '-o', out] +
                 [self.src(s) for s in a['src']] + [self.src('akinit/test/fake.c')])
        return out

    def cc_dynamic(self, b):
        tc = self.vars['toolchain']
        sdk = self.vars['sdk']
        if b.get('make'):
            self.run([self.x(w) for w in b['make']])
        kind = b.get('kind', 'program')
        out = os.path.join(self.obj, os.path.basename(b['out']))
        args = [tc + '/xnu-clang', '-arch', self.arch, '-isysroot', sdk, '-O1', '-I' + os.path.join(self.obj, 'include'),
                '-I' + self.src('src/include')]
        if kind == 'dylib':
            args += ['-dynamiclib', '-install_name', '/' + b['out']]
        elif kind == 'bundle':
            args += ['-bundle']
        args += [self.x(f) for f in b.get('flags', [])]
        for fw in b.get('frameworks', []):
            args += ['-framework', fw]
        args += ['-l' + l for l in b.get('libs', [])]
        args += ['-o', out] + [self.src(s) for s in b['src']]
        self.run(args)
        if b.get('entitlements'):
            self.run([self.vars['ldid'], '-S' + self.src(b['entitlements']), out])
        dst = self.staged(b['out'])
        shutil.copy(out, dst)
        os.chmod(dst, 0o755)
        d = os.path.dirname(dst)
        if b.get('plist'):
            shutil.copy(self.src(b['plist']), os.path.join(d, 'Info.plist'))
            os.chmod(os.path.join(d, 'Info.plist'), 0o644)
        if b.get('pkginfo'):
            open(os.path.join(d, 'PkgInfo'), 'w').write(b['pkginfo'])

    def sign_all(self):
        """every Mach-O without a signature signed ad hoc (an entitlements signature is kept): iOS 27's dyld refuses an
        inserted library without one, and the kernel takes ad-hoc signed code as the platform's"""
        n = 0
        otool = os.path.join(self.vars['toolchain'], 'otool')
        for dp, dn, fn in os.walk(self.img):
            for f in fn:
                p = os.path.join(dp, f)
                if os.path.islink(p):
                    continue
                head = open(p, 'rb').read(4)
                if head not in (b'\xcf\xfa\xed\xfe', b'\xca\xfe\xba\xbe'):
                    continue
                r = subprocess.run([otool, '-l', p], capture_output=True, text=True, env=self.env)
                if 'LC_CODE_SIGNATURE' in r.stdout:
                    continue
                self.run([self.vars['ldid'], '-S', p])
                n += 1
        return n

    def normalize_modes(self):
        """directories 755, files 755 or 644 (by their owner's x bit), whatever the checkout's umask left: launchd
        refuses a group-writable plist, and the image takes the modes as they are"""
        for dp, dn, fn in os.walk(self.img):
            for n in dn:
                os.chmod(os.path.join(dp, n), 0o755)
            for n in fn:
                p = os.path.join(dp, n)
                if not os.path.islink(p):
                    os.chmod(p, 0o755 if os.stat(p).st_mode & 0o100 else 0o644)

    # ---- Apple's jobs ----
    def jobs(self, oslog):
        lib = os.path.join(self.vars['ipsw'], 'rootfs', 'System', 'Library')
        e = tomllib.load(open(self.src(self.man['jobs']['edits']), 'rb'))
        deny = set(e.get('deny', []))
        sets = {s['label']: s['keys'] for s in e.get('set', [])}
        envs = {s['label']: s['vars'] for s in e.get('env', [])}
        osl = e.get('oslog', {})
        changed = set()

        def edit(job):
            label = job.get('Label')
            if label in deny:
                return None
            if label in oslog:
                job['StandardErrorPath'] = osl.get('stderr', '/dev/console')
                job.setdefault('EnvironmentVariables', {}).update(osl.get('vars', {}))
                changed.add('%s: log to the console' % label)
            if label in sets:
                job.update(copy.deepcopy(sets[label]))
                changed.add('%s: %s' % (label, ' '.join(sets[label])))
            if label in envs:
                job.setdefault('EnvironmentVariables', {}).update(envs[label])
                changed.add('%s: %s' % (label, ' '.join('%s=%s' % kv for kv in envs[label].items())))
            return job

        xpc = os.path.join(self.img, 'akita', 'xpc')
        ld = os.path.join(self.img, 'akita', 'LaunchDaemons')
        os.makedirs(xpc, exist_ok=True)
        os.makedirs(ld, exist_ok=True)
        for f in sorted(os.listdir(os.path.join(lib, 'xpc'))):
            if f not in ('launchd.plist', 'launchd.plist.sig'):
                shutil.copy(os.path.join(lib, 'xpc', f), xpc)
        d = plistlib.load(open(os.path.join(lib, 'xpc', 'launchd.plist'), 'rb'))
        for key in list(d['LaunchDaemons']):
            if edit(d['LaunchDaemons'][key]) is None:
                del d['LaunchDaemons'][key]
        copied = 0
        for name in sorted(os.listdir(os.path.join(lib, 'LaunchDaemons'))):
            if not name.endswith('.plist'):
                continue
            try:
                job = plistlib.load(open(os.path.join(lib, 'LaunchDaemons', name), 'rb'))
            except Exception:
                continue
            if not isinstance(job, dict) or edit(job) is None:
                continue
            plistlib.dump(job, open(os.path.join(ld, name), 'wb'), fmt=plistlib.FMT_BINARY)
            copied += 1
        # the angels are read from the volume's own directory: a copy over it only when an edit reached one
        angels, out, any_edit = os.path.join(lib, 'LaunchAngels'), {}, False
        for name in sorted(os.listdir(angels)) if os.path.isdir(angels) else []:
            if not name.endswith('.plist'):
                continue
            try:
                job = plistlib.load(open(os.path.join(angels, name), 'rb'))
            except Exception:
                continue
            if isinstance(job, dict):
                was = copy.deepcopy(job)
                out[name] = edit(job)
                any_edit |= out[name] != was
        if any_edit:
            os.makedirs(os.path.join(self.img, 'akita', 'LaunchAngels'), exist_ok=True)
            for name, job in out.items():
                if job is not None:
                    plistlib.dump(job, open(os.path.join(self.img, 'akita', 'LaunchAngels', name), 'wb'), fmt=plistlib.FMT_BINARY)
        added = []
        ours = self.src(self.man['jobs']['ours'])
        for name in sorted(os.listdir(ours)):
            if name.startswith('com.akios.') and name.endswith('.plist'):
                job = plistlib.load(open(os.path.join(ours, name), 'rb'))
                d['LaunchDaemons']['/System/Library/LaunchDaemons/' + name] = job
                shutil.copy(os.path.join(ours, name), ld)
                added.append(job['Label'])
        plistlib.dump(d, open(os.path.join(xpc, 'launchd.plist'), 'wb'), fmt=plistlib.FMT_BINARY)
        self.note('jobs: %d in launchd.plist, %d of Apple\'s LaunchDaemons, left out: %s; changed: %s; ours: %s%s' % (
            len(d['LaunchDaemons']), copied, ' '.join(sorted(deny)), '; '.join(sorted(changed)), ' '.join(added),
            '; LaunchAngels copied (an edit among them)' if any_edit else ''))
        return added

    # ---- the data volume's skeleton ----
    def var_skel(self):
        v = self.man['var']
        skel = open(os.path.join(self.vars['ipsw'], 'var.skel')).read()
        zone = v.get('timezone', 'auto')
        if zone == 'auto':
            r = subprocess.run(['timedatectl', 'show', '-p', 'Timezone', '--value'], capture_output=True, text=True)
            zone = r.stdout.strip() or 'UTC'
        # the template's zoneinfo links go in a circle until the time zone daemon has run: the data the system ships
        skel = re.sub(r'(?m)^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$',
                      'l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo', skel)
        skel = re.sub(r'(?m)^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime$',
                      'l\t/var/db/timezone/zoneinfo/%s\t/private/var/db/timezone/localtime' % zone, skel)
        lines = [skel.rstrip('\n')]
        top = self.src(v['files_from'])
        files = []
        for dp, dn, fn in os.walk(top):
            files += [os.path.relpath(os.path.join(dp, f), top) for f in fn]
        for f in sorted(files, key=str.lower):      # (the old build's order: sort(1)'s)
            dst = self.staged(os.path.join('akita', 'var', f))
            shutil.copy(os.path.join(top, f), dst)
            os.chmod(dst, 0o644)
            owner = 501 if f.startswith('mobile/') else 0
            lines.append('f\t644\t%d\t%d\t/private/var/%s\t/var/%s' % (owner, owner, f, f))
        for f in v.get('retired', []):
            if f not in files:
                lines.append('x\t/private/var/%s' % f)
        for d in v.get('dirs', []):
            lines.append('d\t%s\t%d\t%d\t%s' % (d['mode'], d['uid'], d['gid'], d['path']))
        for f in v.get('own_files', []):
            rel = f['path'][len('/private/var/'):]
            dst = self.staged(os.path.join('akita', 'var', rel))
            shutil.copy(self.src(f['from']), dst)
            os.chmod(dst, 0o644)
            lines.append('f\t%s\t%d\t%d\t%s\t/var/%s' % (f['mode'], f['uid'], f['gid'], f['path'], rel))
        text = '\n'.join(lines) + '\n'
        if len(text) >= 16383:
            sys.exit('ak: var.skel is %d bytes: akinit reads 16 KB of it' % len(text))
        open(self.staged('akita/etc/var.skel'), 'w').write(text)
        self.note('var.skel: %d lines, time zone %s' % (text.count('\n'), zone))

    # ---- the plan ----
    def plan(self, host):
        b = self.man['boot']
        log_lib = [x['out'] for x in self.man['build'] if x.get('log_all')]
        vols = []
        for v in b.get('ios_volumes', []):
            c = 'if exists:/dev/%s ' % v['dev']
            if v['at'].startswith('/private/var/'):
                vols.append(c + 'mkdirp %s 0755' % v['at'])
            vols.append(c + 'hfs /dev/%s %s %s %d %d' % (v['dev'], v['at'], v['mode'], v['uid'], v['gid']))
        sub = {
            'IMAGE_ID': self.image_id,
            'IOS_VOLUMES': '\n'.join(vols),
            'PERSONA_TIMEOUT': str(b.get('persona_timeout', 20)),
            'MAX_VNODES': str(b.get('max_vnodes', 32768)),
            'LAUNCHD_STDERR': 'stderr /dev/null' if b.get('launchd_stderr') == 'null' else '# (launchd\'s standard error: the console)',
            'LOG_ALL_LINE': ('/%s * ' % log_lib[0]) + ' '.join('!' + p for p in b.get('log_all_skip', [])) if log_lib else '',
        }
        text = open(self.src(b['plan'])).read()
        text = re.sub(r'@([A-Z_]+)@', lambda m: sub[m.group(1)], text)
        dst = self.staged('etc/akios/boot.plan')
        open(dst, 'w').write(text)
        r = subprocess.run([host, '--check', '/plan/boot.plan'], capture_output=True, text=True,
                           env=dict(os.environ, FAKE_PID='5', FAKE_MAP='/plan=' + os.path.dirname(dst)))
        with open(self.log, 'a') as f:
            f.write(r.stdout)
        if r.returncode != 0:
            sys.exit('ak: the boot plan does not check out:\n' + r.stdout)
        return text

    def inject_conf(self):
        lines = []
        for b in self.man['build']:
            if b.get('inject'):
                if not os.path.exists(os.path.join(self.img, b['out'])):
                    sys.exit('ak: %s is to be injected but was not built' % b['out'])
                lines.append('/%s %s' % (b['out'], ' '.join(b['inject'])))
        text = '\n'.join(lines) + '\n'
        if len(text) >= 4000:
            sys.exit('ak: inject.conf is %d bytes: over the kernel\'s 4 KB' % len(text))
        open(self.staged('akita/etc/inject.conf'), 'w').write(text)
        return lines

    def firmware(self):
        fw = self.man['firmware']
        top = self.vars['firmware']
        n = 0
        os.makedirs(os.path.join(self.img, 'fw'), exist_ok=True)
        for pat in fw.get('files', []):
            got = sorted(glob.glob(os.path.join(top, pat)))
            if not got:
                self.note('firmware: nothing for %s (not there)' % pat)
            for f in got:
                dst = os.path.join(self.img, 'fw', os.path.basename(f))
                shutil.copy(f, dst)
                os.chmod(dst, 0o644)
                n += 1
        cc = fw.get('carrierconfig')
        if cc and os.path.isfile(os.path.join(top, cc, 'cfg.db')):
            tool = self.vars['carrierpack']
            self.run(['python3', tool, os.path.join(top, cc), os.path.join(self.img, 'fw', 'carrierconfig.pack')])
            os.chmod(os.path.join(self.img, 'fw', 'carrierconfig.pack'), 0o644)
            n += 1
        return n

    # ---- the image ----
    def check_image(self, image):
        """read the image back with an independent reader (the old tree's tools/apfs/apfs.py, not mkapfs) and compare
        every entry with the staging: kind, mode bits, owner (root), size, link target and the bytes of every file"""
        import importlib.util
        spec = importlib.util.spec_from_file_location('apfs_reader', self.vars['apfs_reader'])
        rd = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rd)
        c = rd.Container(image)
        v = rd.pick_volume(c, None)
        got = {}
        for p, _name, fid, dt in rd.walk(v, 2, ''):
            got[p] = v.inode(fid)
        want = {}
        for dp, dn, fn in os.walk(self.img):
            for f in dn + fn:
                want['/' + os.path.relpath(os.path.join(dp, f), self.img)] = os.path.join(dp, f)
        missing, extra = sorted(set(want) - set(got)), sorted(set(got) - set(want))
        bad = []
        for p in sorted(set(want) & set(got)):
            local, ino = want[p], got[p]
            st = os.lstat(local)
            why = []
            if ino.mode != st.st_mode:
                why.append('mode %o, staged %o' % (ino.mode, st.st_mode))
            if ino.owner != 0 or ino.group != 0:
                why.append('owner %d:%d' % (ino.owner, ino.group))
            if os.path.islink(local):
                if ino.contents().split(b'\0')[0].decode() != os.readlink(local):
                    why.append('link target')
            elif not os.path.isdir(local):
                if ino.size != st.st_size or ino.contents() != open(local, 'rb').read():
                    why.append('contents')
                if any(p.startswith('/' + d + '/') for d in self.man['image'].get('plain', [])) and ino.decmpfs is not None:
                    why.append('compressed, but it is to be stored as it is')
            if why:
                bad.append('%s: %s' % (p, ', '.join(why)))
        c.close()
        self.note('image check (apfs.py): %d entries read back, %d staged; missing %d, extra %d, different %d' % (
            len(got), len(want), len(missing), len(extra), len(bad)))
        for p in missing[:20] + extra[:20] + bad[:20]:
            self.note('  ' + p)
        return not missing and not extra and not bad

    def apfsck(self, image):
        ck = os.path.join(self.root, '.cache', 'apfsprogs', 'apfsck', 'apfsck')
        if not os.path.exists(ck):
            self.note('apfsck: not built (.cache/apfsprogs/apfsck): skipped')
            return None
        r = subprocess.run([ck, '-cw', image], capture_output=True, text=True)
        self.note('apfsck -cw: status %d %s' % (r.returncode, (r.stdout + r.stderr).strip()[-300:]))
        return r.returncode == 0


def run(root, conf, path, rundir, say, args):
    os.umask(0o022)          # launchd refuses group-writable plists; mkapfs takes the modes as they are
    b = Build(root, conf, path, rundir, say, args)
    t0 = time.time()
    say('userland from', b.repo, '(log:', b.log + ')')
    if os.path.exists(b.work):
        shutil.rmtree(b.work)
    os.makedirs(b.obj)
    os.makedirs(os.path.join(b.obj, 'include'))
    os.makedirs(b.img)
    rev = subprocess.run(['git', '-C', b.repo, 'describe', '--always', '--dirty'], capture_output=True, text=True).stdout.strip()
    b.image_id = '%s (%s)' % (rev or 'no git', time.strftime('%Y-%m-%d %H:%M'))
    # the iOS build the hooks are for (src/inject/*.c, audio: ios_build.h)
    bc = dict(re.findall(r'^(\w+)=(.*)$', open(os.path.join(b.vars['ipsw'], 'build.conf')).read(), re.M))
    open(os.path.join(b.obj, 'include', 'ios_build.h'), 'w').write(
        '#define AKIOS_IOS_%s 1        /* ak userland: the unpacked IPSW\'s build.conf */\n' % bc.get('BUILD', 'unknown'))

    for d in b.man['image'].get('dirs', []):
        os.makedirs(os.path.join(b.img, d), exist_ok=True)
    say('akinit (process 1)')
    b.cc_static()
    host = b.host_akinit()
    for x in b.man['build']:
        say('build', x['out'])
        b.cc_dynamic(x)
    # the audio server's plug-in directory: our device, and Bluetooth's own beside it
    hal = os.path.join(b.img, 'akita', 'hal')
    os.makedirs(hal, exist_ok=True)
    os.symlink('/akita/lib/HakiosAudio.driver', os.path.join(hal, 'HakiosAudio.driver'))
    bt = os.path.join(b.vars['ipsw'], 'rootfs', 'System', 'Library', 'Audio', 'Plug-Ins', 'HAL', 'BTAudioHALPlugin.driver')
    if os.path.isdir(bt):
        shutil.copytree(bt, os.path.join(hal, 'BTAudioHALPlugin.driver'), symlinks=True)
    for s in b.man.get('share', []):
        shutil.copytree(b.x(s['from']), os.path.join(b.img, 'akita', 'share', s['name']), symlinks=False)
    say('jobs')
    ours = b.jobs([l for l in (args.oslog or '').split(',') if l])
    say('the data volume\'s skeleton, the inject list, the firmware, the plan')
    b.var_skel()
    inject = b.inject_conf()
    nfw = b.firmware()
    signed = b.sign_all()
    b.normalize_modes()
    b.plan(host)

    image = os.path.join(b.out, b.man['image']['file'])
    os.makedirs(b.out, exist_ok=True)
    tmp = image + '.new'
    say('the image')
    plain = sum((['--plain', p] for p in b.man['image'].get('plain', [])), [])
    b.run(['python3', b.vars['mkapfs'], '-z'] + plain + ['-o', tmp, '%s=%s' % (b.man['image']['volume'], b.img)])
    ok = b.check_image(tmp)
    ck = b.apfsck(tmp)
    if not ok or ck is False:
        sys.exit('ak: %s does not read back as it was staged (see %s): left as %s' % (image, b.log, tmp))
    os.replace(tmp, image)

    # what is in it, for the next comparison
    listing = os.path.join(b.logdir, 'image-files.txt')
    with open(listing, 'w') as f:
        for dp, dn, fn in os.walk(b.img):
            for n in sorted(dn + fn):
                p = os.path.join(dp, n)
                st = os.lstat(p)
                f.write('%o %d /%s%s\n' % (st.st_mode, st.st_size if not os.path.isdir(p) else 0,
                                           os.path.relpath(p, b.img), ' -> ' + os.readlink(p) if os.path.islink(p) else ''))
    shutil.copy(os.path.join(b.img, 'etc', 'akios', 'boot.plan'), os.path.join(b.logdir, 'boot.plan'))
    size = os.path.getsize(image)
    say('done in %d s:' % (time.time() - t0))
    print('  image      %s (%.1f MB), volume %s, %s' % (image, size / 1048576, b.man['image']['volume'], b.image_id))
    print('  built      akinit + %d programs and libraries (%d signed ad hoc here)' % (len(b.man['build']), signed))
    print('  inject     ' + '; '.join(inject))
    print('  jobs       ours: ' + ' '.join(ours))
    print('  firmware   %d files in /fw' % nfw)
    for n in b.notes:
        print('  ' + n)
    print('  apfsck     ' + ('clean' if ck else 'not run' if ck is None else 'FAILED'))
    print('  logs       ' + b.logdir)
