"""ak: AkIOS's build, boot and flash tool.

  ak kernel                 configure and build the kernel from xnu/ (out/kernel, out/kernel.full)
  ak firmware               build our EDK2 firmware (firmware/ inside .cache/mu-silicium) into out/firmware.img
  ak fwboot NAME            RAM-boot out/firmware.img, wait for the phone to come back to Android, keep its log
  ak userland [--oslog L,..] build out/akios.apfs, the RAM disk the kernel starts from, out of userland/ (its
                            userland.toml; --oslog: those launchd jobs log to the console)
  ak loader                 build the UEFI loader from loader/ (out/loader: AkiosBoot.efi, DarwinBoot.efi, Fat.efi)
  ak image [--flash] [--new-loader] [ARGS]
                            make a boot image from out/kernel and the current userland (out/test.img, or with
                            --flash out/flash.img: no kill switches); ARGS are extra boot arguments
  ak boot NAME [ARGS]       RAM-boot out/test.img (built first) and keep the console and kernel log
                            (--stay: leave it running; --quiet: restarts into the bootloader; --speaker: sound on)
  ak flash                  build out/flash.img and write it to the flash slot (akios.toml [flash]), make it active
  ak log [--kernel]         the phone's console log (or kernel log) over the cable
  ak status                 where the phone is: fastboot, Android (adb) or iOS (usbmux)

Every run's output goes to [paths] logs/<command>-<time>/.
"""
import argparse, datetime, os, struct, subprocess, sys, time

from . import adt, fat, userland

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


def kernel_config():
    """the kernel's driver configuration, from akios.toml [kernel]: config/MASTER.hakios (one build option a driver,
    which the conf/files lists name) and bsd/dev/hakios.drivers.h (the list and the built-in default arguments)"""
    k, x = CONF['kernel'], path('xnu')
    drivers, defaults = k['drivers'], ' '.join(k['default_args'])
    master = ['# made by ak from akios.toml [kernel] drivers: the drivers built in (not committed)', '#',
              '#  HAKIOS_DRIVERS = [ %s ]' % ' '.join('hakios_' + d for d in drivers), '#']
    master += ['options\t\tHAKIOS_%s\t# <hakios_%s>' % (d.upper(), d) for d in drivers]
    header = ['/* made by ak from akios.toml [kernel] (not committed) */',
              '#define HAKIOS_DRIVERS_LIST     "%s"' % ' '.join(drivers),
              '#define HAKIOS_DRIVERS_COUNT    %d' % len(drivers),
              '#define HAKIOS_DRIVERS_CONFIG   "akios.toml"',
              '#define HAKIOS_DEFAULT_BOOTARGS "%s"' % defaults]
    for f, lines in ((os.path.join(x, 'config', 'MASTER.hakios'), master), (os.path.join(x, 'bsd', 'dev', 'hakios.drivers.h'), header)):
        text = '\n'.join(lines) + '\n'
        if not os.path.exists(f) or open(f).read() != text:      # unchanged files keep their time: no needless rebuild
            open(f, 'w').write(text)
    say('drivers: %d built in (akios.toml)' % len(drivers))


def cmd_kernel(a):
    log = os.path.join(rundir('kernel'), 'build.log')
    os.umask(0o022)
    kernel_config()
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


# ---- userland ---------------------------------------------------------------------------------------------------------
def cmd_userland(a):
    userland.run(ROOT, CONF, path, rundir, say, a)


# ---- firmware ---------------------------------------------------------------------------------------------------------
def cmd_firmware(a):
    mu, fw = os.path.join(ROOT, '.cache', 'mu-silicium'), os.path.join(ROOT, 'firmware')
    venv = os.path.join(ROOT, '.cache', 'mu-venv', 'bin', 'python3')
    if not os.path.exists(os.path.join(mu, 'build_uefi.py')) or not os.path.exists(venv):
        sys.exit('ak: no Mu-Silicium in .cache/mu-silicium (with its submodules) or no .cache/mu-venv')
    for link, target in (('Platforms/Google', 'Platforms/Google'), ('Silicon/Google', 'Silicon/Google'),
                         ('Resources/Configs/akita.toml', 'Resources/Configs/akita.toml')):
        dst = os.path.join(mu, link)
        if not os.path.islink(dst):
            os.symlink(os.path.join(fw, target), dst)
    log = os.path.join(rundir('firmware'), 'build.log')
    say('building the firmware (log:', log + ')')
    sh([venv, 'build_uefi.py', '-d', CONF['phone']['name'], '-r', 'DEBUG'], log=log, cwd=mu)
    kernel = bytearray(open(os.path.join(mu, 'Build', 'akitaPkg', 'akita-kernel'), 'rb').read())
    # ABL places an arm64 Image at the start of RAM + text_offset (Linux's rule); BootShim's header has the FD's
    # address there: 0, the size, and flags 0xa (little endian, 4K pages, anywhere). The shim copies the FD itself.
    struct.pack_into('<QQQ', kernel, 8, 0, (len(kernel) + 0xfff) & ~0xfff, 0xa)
    out = path('out')
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, 'firmware-kernel'), 'wb').write(kernel)
    img = os.path.join(out, 'firmware.img')
    sh(['python3', LEGACY + '/tools/mkubootimg.py', '-d', CONF['phone']['name'], '-u', os.path.join(out, 'firmware-kernel'),
        '--no-ramdisk', '-o', img], log=log)
    say('firmware image:', img)


def cmd_fwboot(a):
    img = os.path.join(path('out'), 'firmware.img')
    d = rundir('fwboot-' + a.name)
    to_bootloader()
    r = fastboot('boot', img, timeout=120)
    if r.returncode != 0:
        sys.exit('ak: fastboot boot failed: ' + r.stderr.strip()[-200:])
    t0 = time.time()
    say('started; waiting for the phone to come back to Android (the firmware log is read from pstore there)')
    time.sleep(10)
    while time.time() - t0 < a.wait and not in_android():
        time.sleep(4)
    if not in_android():
        sys.exit('ak: not back in Android after %d s (the phone may sit in the firmware or the bootloader)' % a.wait)
    time.sleep(5)
    for f in ('pmsg-ramoops-0', 'console-ramoops-0'):
        with open(os.path.join(d, f + '.txt'), 'w') as o:
            subprocess.run(['adb', 'shell', 'su', '-c', 'cat /sys/fs/pstore/' + f], stdout=o, stderr=subprocess.STDOUT)
    say('back after %d s; logs in %s' % (time.time() - t0, d))


# ---- loader -----------------------------------------------------------------------------------------------------------
def cmd_loader(a):
    log = os.path.join(rundir('loader'), 'build.log')
    edk2 = os.path.join(ROOT, '.cache', 'mu_basecore')
    if not os.path.exists(os.path.join(edk2, 'MdePkg', 'MdePkg.dec')):
        sys.exit('ak: no %s: git clone --depth 1 -b release/202608 https://github.com/microsoft/mu_basecore there, then\n'
                 '    git submodule update --init --depth 1 MdePkg/Library/BaseFdtLib/libfdt MdePkg/Library/MipiSysTLib/mipisyst '
                 'BaseTools/Source/C/BrotliCompress/brotli' % edk2)
    if not os.path.exists(os.path.join(edk2, 'BaseTools', 'Source', 'C', 'bin', 'GenFw')):
        sh(['make', '-C', os.path.join(edk2, 'BaseTools', 'Source', 'C'), '-j%d' % os.cpu_count()], log=log)
    work = os.path.join(path('out'), 'loader-build')
    os.makedirs(os.path.join(work, 'Build', 'Conf'), exist_ok=True)
    for f in ('target', 'tools_def', 'build_rule'):
        dst = os.path.join(work, 'Build', 'Conf', f + '.txt')
        if not os.path.exists(dst):
            sh(['cp', os.path.join(edk2, 'BaseTools', 'Conf', f + '.template'), dst])
    lo = os.path.join(ROOT, 'loader')
    e = dict(os.environ, WORKSPACE=work, EDK_TOOLS_PATH=os.path.join(edk2, 'BaseTools'), PYTHON_COMMAND='python3',
             CONF_PATH=os.path.join(work, 'Build', 'Conf'), CLANG_BIN='/usr/bin/',
             PACKAGES_PATH=':'.join([lo, lo + '/DarwinBoot', lo + '/UniXNU', edk2]))
    e['PATH'] = ':'.join([edk2 + '/BaseTools/BinWrappers/PosixLike', edk2 + '/BaseTools/Source/C/bin', e['PATH']])
    say('building the loader (log:', log + ')')
    sh(['build', '-p', 'AkitaPkg/AkitaPkg.dsc', '-a', 'AARCH64', '-b', 'DEBUG', '-t', 'CLANGPDB', '-n', str(os.cpu_count())],
       log=log, env=e, cwd=lo)
    out = os.path.join(path('out'), 'loader')
    os.makedirs(out, exist_ok=True)
    for f in ('AkiosBoot.efi', 'DarwinBoot.efi', 'Fat.efi', 'UniXNU.efi'):
        sh(['cp', os.path.join(work, 'Build', 'Akita', 'DEBUG_CLANGPDB', 'AARCH64', f), out])
    say('loader:', out)


def payload(items):
    """the boot payload U-Boot copies to 0xF0000000: "XNUP", then name[16]/size/reserved/data entries"""
    body = b''
    for name, data in items:
        body += struct.pack('<16sII', name.encode(), len(data), 0) + data + b'\0' * (-len(data) % 16)
    body += b'\0' * 32
    return struct.pack('<II', 0x50554E58, len(body)) + body


def akios_toml(line, failsafe, initrd=None):
    q = lambda v: '"%s"' % v.replace('\\', '\\\\').replace('"', '\\"')
    return '\n'.join([
        '# made by ak: the boot manager\'s config on the ESP', 'version = 1', '',
        '[boot]', 'timeout = 3', 'default = "akios"', '',
        '[ui]', 'title = "AkIOS"', '',
        '[[driver]]', 'path = "DarwinBoot.efi"', '',
        '[[entry]]', 'id = "akios"', 'title = "AkIOS"', 'type = "xnu"', 'path = "kernel"', 'args = ' + q(line)] +
        (['initrd = ' + q(initrd)] if initrd else []) + ['',
        '[[entry]]', 'id = "reset"', 'title = "Restart"', 'type = "action"', 'action = "reset"', ''])


def make_new_image(flash, line, name):
    """the new loader's image: U-Boot starts AkiosBoot.efi; the payload has Fat.efi and the ESP image"""
    out, lo = path('out'), os.path.join(path('out'), 'loader')
    for f in ('AkiosBoot.efi', 'DarwinBoot.efi', 'Fat.efi'):
        if not os.path.exists(os.path.join(lo, f)):
            sys.exit('ak: no out/loader/%s: run `ak loader` first' % f)
    failsafe = 0 if flash else CONF['test'].get('loader_failsafe', 120)
    rd = lambda f: open(f, 'rb').read()
    esp = fat.make_fat({
        'EFI/UniXNU/akios.toml': akios_toml(line, failsafe).encode(),
        'EFI/UniXNU/DarwinBoot.efi': rd(os.path.join(lo, 'DarwinBoot.efi')),
        'EFI/UniXNU/kernel': rd(os.path.join(out, 'kernel')),
    })
    open(os.path.join(out, name + '-esp.img'), 'wb').write(esp)
    items = [('fat', rd(os.path.join(lo, 'Fat.efi'))), ('esp', esp)]
    if failsafe:
        items.append(('failsafe', b'%d\0' % failsafe))
    pl = os.path.join(out, name + '-payload.bin')
    open(pl, 'wb').write(payload(items))
    img = os.path.join(out, name + '.img')
    sh(['python3', LEGACY + '/tools/mkubootimg.py', '-d', CONF['phone']['name'], '-l', os.path.join(lo, 'AkiosBoot.efi'),
        '-p', pl, '-o', img], log='/dev/null')
    return img


# ---- the ESP (our firmware's boot volume, the phone's "esp" partition) --------------------------------------------
ESP_BYTES = 512 << 20


ESP_RAMDISK = LEGACY + '/userland/root.apfs'            # the RAM disk the entry hands DarwinBoot (its initrd)
# (xnuloader had two: flashboot.apfs as md0, the root, whose /sbin/launchd mounts the Darwin volume on the flash, and
# root.apfs as md1; DarwinBoot places one: --initrd picks it)
IPHONE_TREE = LEGACY + '/sysimage-d16ap/DeviceTree.d16ap.im4p'   # whose identity Darwin's user space gets


def cmd_esp(a):
    """out/esp.img (and esp.simg for fastboot): UniXNU as the removable-media loader, its config, DarwinBoot, the
    kernel, its device tree (kernel.adt, tools/ak/adt.py) and the RAM disk"""
    out, lo = path('out'), os.path.join(path('out'), 'loader')
    for f in ('UniXNU.efi', 'DarwinBoot.efi'):
        if not os.path.exists(os.path.join(lo, f)):
            sys.exit('ak: no out/loader/%s: run `ak loader` first' % f)
    rd = lambda f: open(f, 'rb').read()
    line = cmdline(False, a.args, a.quiet or quiet_now(), a.speaker)
    # the RAM disk is the root (md0) unless the line names one, as xnuloader had it
    if not adt.word(line, 'rd='):
        line += ' rd=md0'
    serial = open(os.path.expanduser('~/akita/usb_serial')).readline().strip()
    ramdisk = os.path.expanduser(a.initrd or ESP_RAMDISK)
    tree = adt.build(line, IPHONE_TREE, serial, ramdisk=True)
    open(os.path.join(out, 'kernel.adt'), 'wb').write(tree)
    files = {
        'EFI/BOOT/BOOTAA64.EFI': rd(os.path.join(lo, 'UniXNU.efi')),
        'EFI/UniXNU/akios.toml': akios_toml(line, 0, initrd=os.path.basename(ramdisk)).encode(),
        'EFI/UniXNU/DarwinBoot.efi': rd(os.path.join(lo, 'DarwinBoot.efi')),
        'EFI/UniXNU/kernel': rd(os.path.join(out, 'kernel')),
        'EFI/UniXNU/kernel.adt': tree,
        'EFI/UniXNU/' + os.path.basename(ramdisk): rd(ramdisk),
    }
    img = os.path.join(out, 'esp.img')
    open(img, 'wb').write(fat.make_fat(files, label='AKIOS ESP', size=ESP_BYTES))
    sh(['img2simg', img, os.path.join(out, 'esp.simg')])
    say('ESP:', img, '(and esp.simg); `fastboot flash esp out/esp.simg` writes it')


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


def make_image(flash, extra, quiet, speaker=False, new_loader=False):
    out = path('out')
    kernel = os.path.join(out, 'kernel')
    if not os.path.exists(kernel):
        sys.exit('ak: no out/kernel: run `ak kernel` first')
    name = 'flash' if flash else 'test'
    payload, img = os.path.join(out, name + '-payload.bin'), os.path.join(out, name + '.img')
    line = cmdline(flash, extra, quiet, speaker)
    if new_loader:
        img = make_new_image(flash, line, name)
        open(os.path.join(out, name + '.cmdline'), 'w').write(line + '\n')
        say(name + ' image (new loader):', img)
        return img
    sh(['python3', LEGACY + '/xnuloader/mkpayload.py', '-o', payload, 'kernel=' + kernel,
        'ramdisk=' + LEGACY + '/userland/root.apfs', 'resident=' + LEGACY + '/userland/flashboot.apfs', '--cmdline', line],
       log='/dev/null')
    sh(['python3', LEGACY + '/tools/mkubootimg.py', '-d', CONF['phone']['name'], '-p', payload, '-o', img], log='/dev/null')
    open(os.path.join(out, name + '.cmdline'), 'w').write(line + '\n')
    say(name + ' image:', img)
    return img


def cmd_image(a):
    make_image(a.flash, a.args, a.quiet or (not a.flash and quiet_now()), a.speaker, a.new_loader)


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
    img = make_image(False, a.args, quiet, a.speaker, a.new_loader)
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
    s.add_parser('loader').set_defaults(f=cmd_loader)
    u = s.add_parser('userland'); u.add_argument('--oslog', help='launchd labels (comma-separated) whose logs go to the console')
    u.set_defaults(f=cmd_userland)
    s.add_parser('firmware').set_defaults(f=cmd_firmware)
    e = s.add_parser('esp'); e.add_argument('--quiet', action='store_true'); e.add_argument('--speaker', action='store_true')
    e.add_argument('--initrd', help='the RAM disk (default: the legacy tree\'s userland/root.apfs)')
    e.add_argument('args', nargs='*'); e.set_defaults(f=cmd_esp)
    w = s.add_parser('fwboot'); w.add_argument('name'); w.add_argument('--wait', type=int, default=300)
    w.set_defaults(f=cmd_fwboot)
    i = s.add_parser('image'); i.add_argument('--flash', action='store_true'); i.add_argument('--quiet', action='store_true')
    i.add_argument('--speaker', action='store_true'); i.add_argument('--new-loader', action='store_true')
    i.add_argument('args', nargs='*'); i.set_defaults(f=cmd_image)
    b = s.add_parser('boot'); b.add_argument('name'); b.add_argument('--stay', action='store_true')
    b.add_argument('--quiet', action='store_true'); b.add_argument('--wait', type=int, default=60)
    b.add_argument('--speaker', action='store_true'); b.add_argument('--new-loader', action='store_true')
    b.add_argument('args', nargs='*'); b.set_defaults(f=cmd_boot)
    f = s.add_parser('flash'); f.add_argument('args', nargs='*'); f.set_defaults(f=cmd_flash)
    l = s.add_parser('log'); l.add_argument('--kernel', action='store_true'); l.add_argument('--secs', type=int, default=15)
    l.set_defaults(f=cmd_log)
    s.add_parser('status').set_defaults(f=cmd_status)
    a = p.parse_args()
    a.f(a)


if __name__ == '__main__':
    main()
