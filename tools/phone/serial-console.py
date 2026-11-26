#!/usr/bin/env python3
"""AkIOS (11-22): the Pixel 8a's USB serial console from the host: /dev/ttyACM* (Linux cdc_acm), the CDC-ACM function
of the phone's USB configurations 3 and 4 (drivers/usb/hakios.akita_usb.c, /dev/hakios-serial on the phone). The kernel's
console log streams on it by itself; userland/src/serial_console.c (job com.hakios.serial) answers typed commands.

usage: serial-console.py [--dev /dev/ttyACMn] [--log FILE] [--wait]     interactive: this terminal on the phone's
                                                                         console (Ctrl-] quits); --log keeps a copy
       serial-console.py --log FILE [--dev ...] --record                  only record everything into FILE (appended;
                                                                         waits for the port, reconnects after a
                                                                         phone restart; Ctrl-C ends it)
       serial-console.py --set-config [N]                                 no ttyACM: the phone is still in
                                                                         configuration 1 (usbmuxd picks 4; without
                                                                         it nobody does) -- set it to N (default 4)
                                                                         through sysfs (needs root)

The port is the phone's USB device 05ac:12a8; --dev picks another. After it is opened a CR is sent, so the command line
answers with a prompt. Only Linux (termios, sysfs).
"""
import glob
import os
import select
import sys
import termios
import time
import tty

VID, PID = "05ac", "12a8"
QUIT = b"\x1d"          # Ctrl-]


def usb_dir_of_tty(name):
    """The sysfs directory of the USB device a ttyACM belongs to (its interface's parent), or None."""
    path = os.path.realpath("/sys/class/tty/%s/device" % name)
    for _ in range(3):
        path = os.path.dirname(path)
        if os.path.exists(os.path.join(path, "idVendor")):
            return path
    return None


def read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return ""


def find_port():
    ports = sorted(glob.glob("/dev/ttyACM*"))
    for p in ports:
        d = usb_dir_of_tty(os.path.basename(p))
        if d and read(d + "/idVendor") == VID and read(d + "/idProduct") == PID:
            return p
    return None


def phones():
    return [d for d in glob.glob("/sys/bus/usb/devices/*")
            if read(d + "/idVendor") == VID and read(d + "/idProduct") == PID]


def set_config(n):
    found = phones()
    if not found:
        sys.exit("serial-console: no 05ac:12a8 device on the bus")
    for d in found:
        print("serial-console: %s: configuration %s -> %d" % (d, read(d + "/bConfigurationValue") or "?", n))
        try:
            with open(d + "/bConfigurationValue", "w") as f:
                f.write("%d\n" % n)
        except OSError as e:
            sys.exit("serial-console: %s (root needed: sudo %s --set-config %d)" % (e, sys.argv[0], n))


def open_port(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    attrs = termios.tcgetattr(fd)
    # raw: no echo, no line discipline, 8 bits (the speed means nothing over USB)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0
    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    return fd


def wait_port(dev, quiet=False):
    said = False
    while True:
        p = dev or find_port()
        if p and os.path.exists(p):
            try:
                return p, open_port(p)
            except OSError as e:
                if not said and not quiet:
                    print("serial-console: %s: %s; waiting" % (p, e), file=sys.stderr)
                    said = True
        elif not said and not quiet:
            hint = ""
            if phones():
                hint = " (the phone is on the bus without a ttyACM: configuration 1? try --set-config)"
            print("serial-console: no ttyACM of the phone yet%s; waiting" % hint, file=sys.stderr)
            said = True
        time.sleep(0.5)


def record(dev, log):
    """Everything the port sends into the log, across disconnects, until Ctrl-C."""
    with open(log, "ab", buffering=0) as out:
        while True:
            p, fd = wait_port(dev)
            print("serial-console: recording %s into %s" % (p, log), file=sys.stderr)
            try:
                while True:
                    data = os.read(fd, 65536)
                    if not data:
                        break
                    out.write(data)
            except OSError:
                pass
            except KeyboardInterrupt:
                return
            finally:
                os.close(fd)
            print("serial-console: %s went (the phone restarted?)" % p, file=sys.stderr)


def interactive(dev, log, wait):
    if wait:
        p, fd = wait_port(dev)
    else:
        p = dev or find_port()
        if not p:
            hint = " -- the phone is on the bus: configuration 1? (--set-config)" if phones() else ""
            sys.exit("serial-console: no ttyACM of the phone%s (--wait waits for one)" % hint)
        fd = open_port(p)
    out = open(log, "ab", buffering=0) if log else None
    stdin = sys.stdin.fileno()
    saved = termios.tcgetattr(stdin) if os.isatty(stdin) else None
    print("serial-console: %s (Ctrl-] quits)\r" % p, file=sys.stderr)
    try:
        if saved:
            tty.setraw(stdin)
        os.write(fd, b"\r")                     # a prompt
        while True:
            r, _, _ = select.select([fd, stdin], [], [])
            if fd in r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    data = b""
                if not data:
                    os.write(sys.stdout.fileno(), b"\r\nserial-console: the port went\r\n")
                    break
                os.write(sys.stdout.fileno(), data)
                if out:
                    out.write(data)
            if stdin in r:
                data = os.read(stdin, 1024)
                if not data or QUIT in data:
                    break
                os.write(fd, data)
    finally:
        if saved:
            termios.tcsetattr(stdin, termios.TCSADRAIN, saved)
        os.close(fd)
        if out:
            out.close()
    print("", file=sys.stderr)


def main():
    args = sys.argv[1:]
    dev = log = None
    rec = wait = False
    conf = None
    while args:
        a = args.pop(0)
        if a == "--dev" and args:
            dev = args.pop(0)
        elif a == "--log" and args:
            log = args.pop(0)
        elif a == "--record":
            rec = True
        elif a == "--wait":
            wait = True
        elif a == "--set-config":
            conf = int(args.pop(0)) if args and args[0].isdigit() else 4
        else:
            sys.exit(__doc__)
    if conf is not None:
        set_config(conf)
        return
    if rec or (log and not os.isatty(sys.stdin.fileno())):
        if not log:
            sys.exit("serial-console: --record needs --log FILE")
        record(dev, log)
    else:
        interactive(dev, log, wait)


if __name__ == "__main__":
    main()
