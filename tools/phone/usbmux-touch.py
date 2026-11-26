#!/usr/bin/env python3
"""Tap, swipe and press buttons on the Pixel 8a while HakiOS runs, over the USB cable (usbmux=1), with nobody at the phone:
each action is a connection to one of the kernel's touch ports (drivers/usb/hakios.akita_usbmux.c), which does it and
refuses the connection. Coordinates are the panel's pixels, 1080 x 2400 (as usbmux-shot.py's picture at scale 1); the
port carries them in 64 x 128 steps (~17 x 19 px).

usage: usbmux-touch.py tap X Y
       usbmux-touch.py swipe X0 Y0 X1 Y1 [STEPS]      (down, STEPS moves (default 12), up)
       usbmux-touch.py button lock|volup|voldown|home
       usbmux-touch.py stacks [N] (the threads of the Nth process of boot-arg ustacks=A,B,C, printed by the kernel)
       several actions: separate them with "--", e.g.  tap 540 1200 -- button home
"""
import plistlib
import socket
import struct
import sys
import time

W, H = 1080, 2400


def mux_port(port):
    """connect to the phone's PORT through usbmuxd; True when the phone answered (a touch port always refuses)"""
    s = socket.socket(socket.AF_UNIX)
    s.connect('/var/run/usbmuxd')
    b = plistlib.dumps({'MessageType': 'ListDevices'})
    s.sendall(struct.pack('<IIII', 16 + len(b), 1, 8, 1) + b)
    n = struct.unpack('<I', s.recv(16)[:4])[0]
    data = b''
    while len(data) < n - 16:
        data += s.recv(n - 16 - len(data))
    devices = plistlib.loads(data)['DeviceList']
    if not devices:
        sys.exit('usbmux-touch: no device (is usbmuxd running, the phone attached?)')
    dev = devices[0]['DeviceID']
    s.close()
    s = socket.socket(socket.AF_UNIX)
    s.connect('/var/run/usbmuxd')
    b = plistlib.dumps({'MessageType': 'Connect', 'DeviceID': dev, 'PortNumber': socket.htons(port)})
    s.sendall(struct.pack('<IIII', 16 + len(b), 1, 8, 2) + b)
    s.recv(4096)
    s.close()


def at(kind, x, y):
    gx = max(0, min(63, round(x * 63 / (W - 1))))
    gy = max(0, min(127, round(y * 127 / (H - 1))))
    mux_port(0x8000 | kind << 13 | gx << 7 | gy)


def run(args):
    if args[0] == 'tap' and len(args) == 3:
        at(0, float(args[1]), float(args[2]))
    elif args[0] == 'swipe' and len(args) in (5, 6):
        x0, y0, x1, y1 = map(float, args[1:5])
        steps = int(args[5]) if len(args) == 6 else 12
        at(1, x0, y0)
        for i in range(1, steps + 1):
            time.sleep(0.02)
            at(2, x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
        at(3, x1, y1)
    elif args[0] == 'button' and len(args) == 2:
        mux_port(0x7f00 + {'lock': 1, 'volup': 2, 'voldown': 3, 'home': 4}[args[1]])
    elif args[0] == 'stacks' and len(args) <= 2:
        # every thread of a process boot-arg ustacks=A,B,C names (the Nth, from 0), into the kernel log
        mux_port(0x7f10 + (int(args[1]) if len(args) == 2 else 0))
    else:
        sys.exit(__doc__)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    action = []
    for a in sys.argv[1:] + ['--']:
        if a == '--':
            if action:
                run(action)
                time.sleep(0.3)
            action = []
        else:
            action.append(a)


main()
