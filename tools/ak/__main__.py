"""ak: AkIOS's build, boot and flash tool.

  ak kernel                 configure and build the kernel from xnu/ (out/kernel, out/kernel.full)
  ak image [--flash] [ARGS] make a boot image from out/kernel and the current userland (out/test.img, or with
                            --flash out/flash.img: no kill switches); ARGS are extra boot arguments
  ak boot NAME [ARGS]       RAM-boot out/test.img (built first) and keep the console and kernel log
                            (--stay: leave it running; --quiet: restarts into the bootloader; --speaker: sound on)
  ak flash                  build out/flash.img and write it to the flash slot (akios.toml [flash]), make it active
  ak log [--kernel]         the phone's console log (or kernel log) over the cable
  ak status                 where the phone is: fastboot, Android (adb) or iOS (usbmux)

Every run's output goes to [paths] logs/<command>-<time>/.
"""
import argparse, datetime, os, subprocess, sys, time

try:
    import tomllib
except ImportError:          # Python < 3.11
    import tomli as tomllib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))   # tools/ak/ -> the repository
CONF = tomllib.load(open(os.path.join(ROOT, 'akios.toml'), 'rb'))


def path(key):
    p = os.path.expanduser(CONF['paths'][key])
    return p if os.path.isabs(p) else os.path.join(ROOT, p)


LEGACY = path('legacy')


def rundir(cmd):
    d = os.path.join(path('logs'), '%s-%s' % (cmd, datetime.datetime.now().strftime('%m%d-%H%M%S')))
    os.makedirs(d, exist_ok=True)
    return d


def sh(args, log=None, env=None, cwd=None, check=True, timeout=None):
    """run ARGS (a list); with LOG its output goes there, else to the terminal"""
    out = open(log, 'a') if log else None
    r = subprocess.run(args, stdout=out, stderr=subprocess.STDOUT if out else None, env=env, cwd=cwd, timeout=timeout)
    if out:
        out.close()
    if check and r.returncode != 0:
        sys.exit('ak: %s failed (%d)%s' % (args[0], r.returncode, ', see ' + log if log else ''))
    return r.returncode


def say(*a):
    print('ak:', *a, flush=True)


# ---- kernel -----------------------------------------------------------------------------------------------------------
def kernel_env():
    e = dict(os.environ)
    e.update(XNU_SRC=path('xnu'), RC_DARWIN_KERNEL_VERSION=CONF['kernel']['version'], XNU_IIG_SDK='MacOSX26.1.sdk',
             XNU_IIG_OVERRIDE=LEGACY + '/xnu-deps/compat-12377/iig', XNU_COMPAT_INC=LEGACY + '/xnu-deps/compat-12377/include')
    return e


def cmd_kernel(a):
    log = os.path.join(rundir('kernel'), 'build.log')
    os.umask(0o022)
    say('configuring the drivers (hakios-config.py kernel)')
    sh(['python3', 'tools/hakios-config.py', 'kernel', '--tree', path('xnu')], log=log, cwd=LEGACY)
    say('building XNU from', path('xnu'), '(log:', log + ')')
    k = os.path.join(path('xnu'), 'BUILD/obj/%s_ARM64_%s/mach.%s.%s' % (
        CONF['kernel']['config'], CONF['kernel']['machine'], CONF['kernel']['config'].lower(), CONF['kernel']['machine'].lower()))
    rc = sh(['./xnu-build.sh', '-k', '-j%d' % os.cpu_count()], log=log, env=kernel_env(), cwd=LEGACY, check=False)
    # the kext symbol-set checks fail on this configuration and make the status non-zero: make-kernel.sh's judge
    # accepts a build whose only failures are those
    if rc != 0:
        e = dict(os.environ, KERNEL=k)
        if sh(['tools/make-kernel.sh', '--judge', log], env=e, cwd=LEGACY, check=False) != 0:
            sys.exit('ak: the kernel build failed (status %d), see %s' % (rc, log))
    os.makedirs(path('out'), exist_ok=True)
    full, stripped = os.path.join(path('out'), 'kernel.full'), os.path.join(path('out'), 'kernel')
    sh(['cp', '-p', k, full])
    sh(['cp', '-p', k, stripped])
    sh([LEGACY + '/xnu-deps/host/bin/strip', '-x', stripped])
    say('kernel:', stripped, '(%d bytes)' % os.path.getsize(stripped))


# ---- images -----------------------------------------------------------------------------------------------------------
def quiet_now():
    a, b = CONF['test']['quiet_hours']
    h = datetime.datetime.now().hour
    return h >= a or h < b


def cmdline(flash, extra, quiet, speaker=False):
    serial = open(os.path.expanduser('~/akita/usb_serial')).readline().strip()
    k = CONF['flash'] if flash else CONF['test']
    args = list(CONF['phone']['cmdline'])
    args[1:1] = ['usb_lost_reboot=%d' % k['usb_lost_reboot'], 'test_reboot_after=%d' % k['test_reboot_after'],
                 'usb_serial=' + serial]
    # (speaker=0 by default as in liveboot.sh; a flashed image gets the owner's sound)
    args.append('speaker=1' if (speaker and not quiet) or flash else 'speaker=0')
    if quiet and not flash:
        args.append('restart_fastboot=1')
    args += extra
    line = ' '.join(args)
    if len(line) > CONF['phone']['cmdline_max']:
        sys.exit('ak: the boot line is %d characters, over %d: the phone would cut it (metal_plugin= went once)' %
                 (len(line), CONF['phone']['cmdline_max']))
    return line


def make_image(flash, extra, quiet, speaker=False):
    out = path('out')
    kernel = os.path.join(out, 'kernel')
    if not os.path.exists(kernel):
        sys.exit('ak: no out/kernel: run `ak kernel` first')
    name = 'flash' if flash else 'test'
    payload, img = os.path.join(out, name + '-payload.bin'), os.path.join(out, name + '.img')
    line = cmdline(flash, extra, quiet, speaker)
    sh(['python3', LEGACY + '/xnuloader/mkpayload.py', '-o', payload, 'kernel=' + kernel,
        'ramdisk=' + LEGACY + '/userland/root.apfs', 'resident=' + LEGACY + '/userland/flashboot.apfs', '--cmdline', line],
       log='/dev/null')
    sh(['python3', LEGACY + '/tools/mkubootimg.py', '-d', CONF['phone']['name'], '-p', payload, '-o', img], log='/dev/null')
    open(os.path.join(out, name + '.cmdline'), 'w').write(line + '\n')
    say(name + ' image:', img)
    return img


def cmd_image(a):
    make_image(a.flash, a.args, a.quiet or (not a.flash and quiet_now()), a.speaker)


# ---- the phone --------------------------------------------------------------------------------------------------------
def fastboot(*args, timeout=60):
    return subprocess.run(['fastboot'] + list(args), capture_output=True, text=True, timeout=timeout)


def in_fastboot():
    try:
        # (any listed device: some fastboot builds print a junk state instead of "fastboot")
        return any(l.strip() for l in fastboot('devices', timeout=5).stdout.splitlines())
    except subprocess.TimeoutExpired:
        return False


def in_ios():
    r = subprocess.run(['idevice_id', '-l'], capture_output=True, text=True)
    return r.returncode == 0 and r.stdout.strip() != ''


def in_android():
    r = subprocess.run(['adb', 'devices'], capture_output=True, text=True)
    return any(l.endswith('\tdevice') for l in r.stdout.splitlines())


def to_bootloader():
    if in_fastboot():
        return
    if in_android():
        subprocess.run(['adb', 'reboot', 'bootloader'])
    elif in_ios():
        subprocess.run(['python3', LEGACY + '/devices/akita/tools/usbmux-reboot.py', 'bootloader'])
    for _ in range(40):
        if in_fastboot():
            return
        time.sleep(3)
    sys.exit('ak: the phone did not reach the bootloader')


def cmd_status(a):
    say('fastboot' if in_fastboot() else 'iOS (usbmux)' if in_ios() else 'Android (adb)' if in_android() else 'not seen')
    if in_fastboot():
        print(fastboot('getvar', 'current-slot').stderr.strip().splitlines()[0])


def usbmux_log(dest, kernel=False, secs=15):
    with open(dest, 'w') as f:
        subprocess.run(['timeout', str(secs), 'python3', LEGACY + '/devices/akita/tools/usbmux-log.py'] + (['-k'] if kernel else []),
                       stdout=f, stderr=subprocess.STDOUT)


def cmd_boot(a):
    quiet = a.quiet or quiet_now()
    img = make_image(False, a.args, quiet, a.speaker)
    d = rundir('boot-' + a.name)
    to_bootloader()
    if fastboot('getvar', 'current-slot').stderr.find('current-slot: ' + CONF['flash']['slot']) >= 0:
        sys.exit('ak: slot %s is active, and the bootloader ignores RAM boots then (`fastboot set_active a`, with the owner\'s word)'
                 % CONF['flash']['slot'])
    r = fastboot('boot', img, timeout=120)
    if r.returncode != 0:
        sys.exit('ak: fastboot boot failed: ' + r.stderr.strip()[-200:])
    t0 = time.time()
    while time.time() - t0 < 180 and not in_ios():
        time.sleep(3)
    say('usbmux after %d s' % (time.time() - t0) if in_ios() else 'no usbmux in 180 s (is the PC\'s usbmuxd running?)')
    time.sleep(max(0, a.wait - (time.time() - t0)))
    usbmux_log(os.path.join(d, 'console.txt'))
    usbmux_log(os.path.join(d, 'klog.txt'), kernel=True, secs=12)
    say('logs in', d)
    if not a.stay:
        subprocess.run(['python3', LEGACY + '/devices/akita/tools/usbmux-reboot.py', 'bootloader'])


def cmd_flash(a):
    img = make_image(True, a.args, False)
    to_bootloader()
    slot = CONF['flash']['slot']
    for args in (['flash', 'boot_' + slot, img], ['set_active', slot]):
        r = fastboot(*args, timeout=180)
        if r.returncode != 0:
            sys.exit('ak: fastboot %s failed: %s' % (' '.join(args), r.stderr.strip()[-200:]))
    say('boot_%s written from %s and made active; `fastboot reboot` starts it' % (slot, img))


def cmd_log(a):
    d = rundir('log')
    dest = os.path.join(d, 'klog.txt' if a.kernel else 'console.txt')
    usbmux_log(dest, kernel=a.kernel, secs=a.secs)
    say(dest)


def main():
    p = argparse.ArgumentParser(prog='ak', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest='cmd', required=True)
    s.add_parser('kernel').set_defaults(f=cmd_kernel)
    i = s.add_parser('image'); i.add_argument('--flash', action='store_true'); i.add_argument('--quiet', action='store_true')
    i.add_argument('--speaker', action='store_true'); i.add_argument('args', nargs='*'); i.set_defaults(f=cmd_image)
    b = s.add_parser('boot'); b.add_argument('name'); b.add_argument('--stay', action='store_true')
    b.add_argument('--quiet', action='store_true'); b.add_argument('--wait', type=int, default=60)
    b.add_argument('--speaker', action='store_true')
    b.add_argument('args', nargs='*'); b.set_defaults(f=cmd_boot)
    f = s.add_parser('flash'); f.add_argument('args', nargs='*'); f.set_defaults(f=cmd_flash)
    l = s.add_parser('log'); l.add_argument('--kernel', action='store_true'); l.add_argument('--secs', type=int, default=15)
    l.set_defaults(f=cmd_log)
    s.add_parser('status').set_defaults(f=cmd_status)
    a = p.parse_args()
    a.f(a)


if __name__ == '__main__':
    main()
