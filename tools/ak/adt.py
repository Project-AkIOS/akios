"""The Apple device tree (ADT) the kernel gets from DarwinBoot: \\EFI\\UniXNU\\kernel.adt on the ESP.

The old loader (xnuloader) built this tree at every start (run_kernel()); here it is built on the PC, the same tree
property for property, and DarwinBoot fills in at boot what only the boot can know. Those properties are here as
placeholders of the right size, all zero:

  /chosen/dram-base, dram-size        the kernel's memory window (DarwinBoot's choice)
  /chosen/random-seed                 256 bytes of seed (xnu-12377 wants 256)
  /chosen/el2-trampoline              where the kernel's other cores enter at EL2 (DarwinBoot's EL2 code)
  /chosen/boot-epoch                  the firmware's clock (0: the kernel asks the phone's RTC)
  /chosen/memory-map/RAMDisk          the RAM disk (the boot entry's initrd), { physical address, size }

and DarwinBoot reads /chosen/darwinboot-memory-size, the window it is to give the kernel.

Who Darwin's user space thinks it runs on (the root node's model and compatible, /product and its haptics, audio and
camera nodes) comes from the iPhone's own device tree in the IPSW (DeviceTree.<board>.im4p), as xnuloader's
mkidentity.py took it; the numbers iBoot would take from the factory data (serial number, chip id, the two
addresses) are made from the device's name and a hash of the phone's serial number, as xnuloader made them: the
same at every start, different from phone to phone, and the serial number itself is not in the tree.

  python3 -m tools.ak.adt out/<device>/kernel.adt       (prints a tree; ak esp calls build())
"""
import os, struct, subprocess, sys

# ---- the format: node := u32 nProperties, u32 nChildren, properties, children; property := char name[32],
# ---- u32 length, value padded to 4 bytes. "name" is the first property of every node.


class Node:
    def __init__(self, name, props=(), children=()):
        self.name, self.props, self.children = name, list(props), list(children)

    def prop(self, name, value):
        if len(name.encode()) > 31:
            raise ValueError('ADT property name too long: ' + name)
        self.props.append((name, bytes(value)))
        return self

    def child(self, node):
        self.children.append(node)
        return node

    def encode(self):
        props = [('name', self.name.encode() + b'\0')] + self.props
        out = struct.pack('<II', len(props), len(self.children))
        for name, value in props:
            out += name.encode().ljust(32, b'\0') + struct.pack('<I', len(value)) + value + b'\0' * (-len(value) % 4)
        return out + b''.join(c.encode() for c in self.children)


def u32(v):
    return struct.pack('<I', v)


def u64(*v):
    return struct.pack('<%dQ' % len(v), *v)


def cstr(s):
    return s.encode() + b'\0'


def padded(s, size):
    return s.encode().ljust(size, b'\0')[:size]


def parse(data, pos=0):
    """one node of a flattened ADT: ((properties, children), next position)"""
    nprops, nchildren = struct.unpack_from('<II', data, pos)
    pos += 8
    props = []
    for _ in range(nprops):
        name = data[pos:pos + 32].split(b'\0')[0].decode()
        length = struct.unpack_from('<I', data, pos + 32)[0] & 0x7FFFFFFF
        props.append((name, data[pos + 36:pos + 36 + length]))
        pos += 36 + (length + 3 & ~3)
    children = []
    for _ in range(nchildren):
        child, pos = parse(data, pos)
        children.append(child)
    return (props, children), pos


def name_of(node):
    return dict(node[0]).get('name', b'').split(b'\0')[0].decode()


# ---- the iPhone's identity, from its device tree in the IPSW -----------------------------------------------------
def im4p_payload(data):
    """the payload (OCTET STRING) of an IM4P: DER SEQUENCE { "IM4P", type, description, payload, ... }"""
    def tlv(pos):
        tag, length, pos = data[pos], data[pos + 1], pos + 2
        if length & 0x80:
            n = length & 0x7f
            length, pos = int.from_bytes(data[pos:pos + n], 'big'), pos + n
        return tag, pos, pos + length
    tag, pos, end = tlv(0)
    if tag != 0x30:
        raise ValueError('not an IM4P')
    strings = []
    while pos < end:
        tag, start, stop = tlv(pos)
        if tag == 0x16:
            strings.append(data[start:stop])
        elif tag == 0x04 and strings[:1] == [b'IM4P']:
            return data[start:stop]
        pos = stop
    raise ValueError('no IM4P payload')


def iphone_tree(im4p):
    payload = im4p_payload(open(im4p, 'rb').read())
    if payload[:3] == b'bvx':
        payload = subprocess.run(['lzfse', '-decode'], input=payload, stdout=subprocess.PIPE, check=True).stdout
    return parse(payload)[0]


def identity(im4p):
    """{root, product, haptics, camera, audio}: the properties Darwin knows its device by (mkidentity.py's choice)"""
    root = iphone_tree(im4p)
    kept = lambda props, skip: [(n, v) for n, v in props if n not in skip and
                                not v.startswith((b'syscfg/', b'macaddr/', b'zeroes/'))]
    own = ('name', 'AAPL,phandle')
    children = {name_of(c): c for c in root[1]}
    product = {name_of(c): c for c in children['product'][1]}
    return {
        # of the root: who we are; addresses, the phandle and what iBoot fills in stay ours
        'root': kept(root[0], own + ('#address-cells', '#size-cells', 'clock-frequency', 'platform-name', 'time-stamp',
                                     'secure-root-prefix')),
        'product': kept(children['product'][0], own),
        'haptics': kept(product['haptics'][0], own),
        'camera': kept(product['camera'][0], own),
        'audio': kept(product['audio'][0], own) if 'audio' in product else [],
        # (11-26) arm-io/wlan: the radio's settings wifid reads (wlan.*, bcom.*); its address is ours. launchd starts
        # wifid only on a device tree with a node of this name (its job's _LimitLoadToDeviceTree)
        'wlan': kept(next(c for c in children['arm-io'][1] if name_of(c) == 'wlan')[0],
                     own + ('local-mac-address', 'audio-protection-driver')),
        # (11-26) arm-io/bluetooth: the node bluetoothd and BlueTool find the controller by (IOServiceNameMatching
        # "bluetooth"; AppleBluetoothModule matches it by name). Without the iPhone's wiring: its interrupt, its
        # register and its GPIO functions (function-*) point into the iPhone's AOP and GPIO blocks; the transport is
        # build()'s to say, the address ours
        'bluetooth': [(n, v) for n, v in kept(next(c for c in children['arm-io'][1] if name_of(c) == 'bluetooth')[0],
                                              own + ('interrupt-parent', 'interrupts', 'reg', 'local-mac-address',
                                                     'transport-encoding', 'transport-speed'))
                      if not n.startswith('function-')],
    }


def phone_numbers(name, serial):
    """xnuloader's per-phone numbers: serial number, MLB, chip id, Wi-Fi and Bluetooth addresses, manifest hash"""
    mask = (1 << 64) - 1
    h = 0xcbf29ce484222325
    for c in name.encode() + serial.encode()[:32]:
        h = ((h ^ c) * 0x100000001b3) & mask
    g = h ^ (h >> 33)
    g = (g * 0xff51afd7ed558ccd) & mask
    g ^= g >> 33
    g = (g * 0xc4ceb9fe1a85ec53) & mask
    g ^= g >> 33
    digits = '0123456789ABCDEFGHJKLMNPQRSTUVWXYZ'
    first = (name + 'xxx')[:3].upper()
    v, rest = g, ''
    for _ in range(9):
        rest += digits[v % 34]
        v //= 34
    sn = first + rest
    wifi = bytes([0x02] + [(g >> (8 * i)) & 0xff for i in range(1, 6)])
    wifi = wifi[:5] + bytes([wifi[5] & 0xfe])
    bluetooth = wifi[:5] + bytes([wifi[5] | 1])
    n = min(3, len(name))
    manifest = bytes(ord(name[i]) if i < n else 0 if i < 8 else ((g if i & 8 else h) >> (8 * (i & 7))) & 0xff
                     for i in range(48))
    return {'serial': sn, 'mlb': sn + 'MLB00', 'chip': 0x0001000000000000 | (h & 0x0000ffffffffffff),
            'wifi': wifi, 'bluetooth': bluetooth, 'manifest': manifest}


# ---- what iBoot hands over that there is none of here ------------------------------------------------------------
NVRAM_BANK_SIZE = 0x2000


def nvram_image():
    """an empty, well-formed NVRAM image in CHRP format (IOKit refuses to start without one)"""
    def header(sig, size, name):
        blocks = size // 16
        h = bytearray(16)
        h[0], h[2], h[3] = sig, blocks & 0xff, blocks >> 8
        h[4:4 + len(name)] = name.encode()
        s = h[0] + sum(h[2:16])
        while s > 0xff:
            s = (s & 0xff) + (s >> 8)
        h[1] = s
        return h
    image = bytearray(NVRAM_BANK_SIZE)
    image[0:16] = header(0x5a, 0x20, 'nvram')
    image[0x20:0x30] = header(0x70, NVRAM_BANK_SIZE - 0x20, 'common')
    image[20:24] = u32(1)                       # generation
    lo, hi = 1, 0
    for b in image[20:]:
        lo = (lo + b) % 65521
        hi = (hi + lo) % 65521
    image[16:20] = u32((hi << 16) | lo)         # Adler-32 of everything after the checksum
    return bytes(image)


# ---- the device: [platform] of devices/<name>/device.toml (every number from the phone's own device tree) ---------
def platform(conf):
    """the platform numbers build() needs, from a device's merged configuration"""
    p = dict(conf['platform'])
    p['name'] = conf['device']['name']
    p['panic_log'] = tuple(p['panic_log'])
    return p


def word(line, prefix):
    return any(w.startswith(prefix) for w in line.split())


def boot_arg(line, name, default=0):
    """the number of boot-arg NAME=N in LINE, DEFAULT without one"""
    for w in line.split():
        if w.startswith(name + '=') and w[len(name) + 1:].isdigit():
            return int(w[len(name) + 1:])
    return default


def build(line, im4p, serial, plat, ramdisk=True, defaults=''):
    """kernel.adt for boot line LINE (it decides cores, torch, camera as for xnuloader); DEFAULTS: the kernel's built-in
    arguments (akios.toml [kernel] default_args), for what the line does not name (bt)"""
    ident = identity(im4p)
    more_cores = word(line, 'cpus=') or word(line, 'cpumask=')
    torch = any(w.startswith('torch=') and w[6:7] in '123456789' and w[6:7] != '' for w in line.split())
    camera = any(w in ('cam=5', 'camera=13', 'camera=14', 'camera=15', 'camera=16') for w in line.split())
    p3 = word(line, 'gamut=p3')
    ids = phone_numbers(plat['name'], serial)

    def from_iphone(node, props, only=None):
        for n, v in props:
            if only is not None and n != only:
                continue
            node.prop(n, b'sRGB\0' if n == 'artwork-display-gamut' and not p3 else v)

    root = Node('device-tree')
    from_iphone(root, ident['root'])
    root.prop('platform-name', cstr('t8015'))
    root.prop('serial-number', padded(ids['serial'], 32))
    root.prop('mlb-serial-number', padded(ids['mlb'], 32))
    root.prop('model-number', padded('MQAC2', 32))
    root.prop('region-info', padded('LL/A', 32))
    root.prop('regulatory-model-number', padded('A1865', 32))
    root.prop('#address-cells', u32(2)).prop('#size-cells', u32(2))

    chosen = root.child(Node('chosen'))
    chosen.prop('random-seed', bytes(256))              # DarwinBoot
    chosen.prop('dram-base', u64(0))                    # DarwinBoot
    chosen.prop('dram-size', u64(0))                    # DarwinBoot
    chosen.prop('debug-enabled', u32(1))
    chosen.prop('display-scale', u32(3))                # MobileGestalt's main screen scale
    chosen.prop('display-rotation', u32(0))
    chosen.prop('psci-method', cstr('smc'))             # the kernel runs at EL1 below an EL2 that answers no PSCI
    chosen.prop('el2-trampoline', u64(0))               # DarwinBoot (at EL2)
    chosen.prop('boot-epoch', u64(0))                   # DarwinBoot
    chosen.prop('unique-chip-id', u64(ids['chip']))
    chosen.prop('chip-id', u32(0x8015))
    chosen.prop('board-id', u32(0x06))
    chosen.prop('mac-address-wifi0', ids['wifi'])
    chosen.prop('mac-address-bluetooth0', ids['bluetooth'])
    chosen.prop('boot-manifest-hash', ids['manifest'])
    chosen.prop('software-behavior', bytes(16))
    chosen.prop('firmware-version', cstr('iBoot-8422.100.650'))
    for n in ('certificate-production-status', 'certificate-security-mode', 'effective-production-status-ap',
              'effective-security-mode-ap', 'secure-boot', 'production-cert'):
        chosen.prop(n, u32(1))
    chosen.prop('development-cert', u32(0))
    chosen.prop('embedded-panic-log-size', u32(plat['panic_log'][1]))
    chosen.prop('kernel-ctrr-to-be-enabled', u32(0))    # xnu-12377 asserts it is there
    chosen.prop('nvram-bank-size', u32(NVRAM_BANK_SIZE))
    chosen.prop('nvram-bank-count', u32(1))
    chosen.prop('nvram-current-bank', u32(0))
    chosen.prop('nvram-proxy-data', nvram_image())
    chosen.prop('darwinboot-memory-size', u64(plat['window']))
    memory_map = chosen.child(Node('memory-map'))
    if ramdisk:
        memory_map.prop('RAMDisk', u64(0, 0))           # DarwinBoot: md0, the root unless rd= says otherwise

    # what iBoot's tree tells the kernel unless told otherwise: the compressor as on a phone
    root.child(Node('defaults')).prop('kern.vm_compressor', u32(2))

    product = root.child(Node('product'))
    from_iphone(product, ident['product'])
    from_iphone(product.child(Node('haptics')), ident['haptics'])
    from_iphone(product.child(Node('audio')), ident['audio'])
    if torch or camera:
        cam = product.child(Node('camera'))
        if torch:
            from_iphone(cam, ident['camera'], only='flash')

    root.child(Node('buttons')).prop('home-button-type', u32(2))    # 2: no home button, the home gesture
    root.child(Node('options'))                                     # IOKit's NVRAM node
    root.child(Node('pram')).prop('reg', u64(*plat['panic_log']))

    cores = [plat['boot_core']]
    if more_cores:
        for distance in range(1, plat['cores']):
            for core in (plat['boot_core'] + distance, plat['boot_core'] - distance):
                if 0 <= core < plat['cores']:
                    cores.append(core)
    cpus = root.child(Node('cpus'))
    for i, core in enumerate(cores):
        cpu = cpus.child(Node('cpu%d' % i))
        cpu.prop('device_type', cstr('cpu'))
        cpu.prop('state', cstr('running' if i == 0 else 'waiting'))
        cpu.prop('reg', u32(core << plat['core_shift']))
        cpu.prop('cpu-id', u32(i))
        cpu.prop('timebase-frequency', u64(plat['timer_hz']))
        cpu.prop('fixed-frequency', u64(plat['timer_hz']))
        cpu.prop('clock-frequency', u64(1800000000))
        cpu.prop('bus-frequency', u64(100000000))
        cpu.prop('memory-frequency', u64(100000000))
        cpu.prop('peripheral-frequency', u64(24000000))
        if plat['small_cores'] & (1 << core):
            cpu.prop('small-core', u32(1))

    arm_io = root.child(Node('arm-io'))
    arm_io.prop('device_type', cstr(plat['name']))
    arm_io.prop('ranges', u64(0, plat['io_base'], plat['io_size']))
    arm_io.prop('#address-cells', u32(2)).prop('#size-cells', u32(2))
    if camera:
        isp = arm_io.child(Node('isp'))
        isp.prop('camera-rear', u32(1)).prop('camera-front', u32(1))
    wlan = arm_io.child(Node('wlan'))
    from_iphone(wlan, ident['wlan'])
    wlan.prop('local-mac-address', ids['wifi'])
    if boot_arg(line, 'bt', boot_arg(defaults, 'bt')) >= 2:
        # (11-26) only when the kernel gives the controller to iOS (bt=2): with the node there bluetoothd starts and
        # waits for a controller. transport-encoding 3 is H4BC (the kernel control com.apple.uart.sk.bluetooth and a
        # Skywalk pipe), a UART iPhone's transport: the BCM4383 really is on a UART (the iPhone 13's 7 is PCIe). No
        # transport-speed: the kernel already runs the UART at 4 Mbaud, and bluetoothd would try to set its own
        bluetooth = arm_io.child(Node('bluetooth'))
        from_iphone(bluetooth, ident['bluetooth'])
        bluetooth.prop('transport-encoding', u32(3))
        bluetooth.prop('local-mac-address', ids['bluetooth'])     # = /chosen/mac-address-bluetooth0
    gic = arm_io.child(Node('gic'))
    gic.prop('device_type', cstr('interrupt-controller'))
    gic.prop('compatible', cstr('arm,gic-v3'))
    gic.prop('reg', u64(plat['gic_dist'] - plat['io_base'], 0x10000, plat['gic_redist'] - plat['io_base'],
                        plat['gic_redist_size']))
    return root.encode()


def dump(data, out=sys.stdout):
    """the tree as text, for a look"""
    def show(node, depth):
        for n, v in node[0]:
            text = v.rstrip(b'\0').decode() if v and all(32 <= c < 127 for c in v.rstrip(b'\0')) and v.endswith(b'\0') \
                else v[:16].hex() + ('...' if len(v) > 16 else '')
            print('%s%s [%d] %s' % ('  ' * depth, n, len(v), text), file=out)
        for c in node[1]:
            show(c, depth + 1)
    show(parse(data)[0], 0)


if __name__ == '__main__':
    # (the tree is built by `ak esp`, for the device it works for: out/<device>/kernel.adt; this shows one)
    if len(sys.argv) != 2:
        sys.exit('usage: python3 -m tools.ak.adt KERNEL.ADT     (prints the tree)')
    dump(open(sys.argv[1], 'rb').read())
