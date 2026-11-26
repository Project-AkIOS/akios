#!/usr/bin/env python3
"""Run commands on the phone through its USB serial console (serial_console on /dev/hakios-serial) and print what they say.

usage: serial-run.py [--tty /dev/ttyACM0] [--timeout S] 'command args' ...   each one sent and read up to the next prompt
The port is put in raw mode first (a cooked tty echoes the phone's log back to it as typed input). 'klog off' is sent first
unless --klog: the kernel log would interleave with the answers.
"""
import os, sys, termios, time, select, glob, re

args = sys.argv[1:]
tty = None
timeout = 30.0
klog = False
while args and args[0].startswith('--'):
    a = args.pop(0)
    if a == '--tty':
        tty = args.pop(0)
    elif a == '--timeout':
        timeout = float(args.pop(0))
    elif a == '--klog':
        klog = True
tty = tty or (sorted(glob.glob('/dev/ttyACM*')) or [None])[0]
if not tty:
    sys.exit('serial-run: no /dev/ttyACM*')
fd = os.open(tty, os.O_RDWR | os.O_NOCTTY)
t = termios.tcgetattr(fd)
t[0] = 0; t[1] = 0; t[2] = t[2] | termios.CS8 | termios.CREAD | termios.CLOCAL; t[3] = 0
t[6][termios.VMIN] = 0; t[6][termios.VTIME] = 0
termios.tcsetattr(fd, termios.TCSANOW, t)
termios.tcflush(fd, termios.TCIOFLUSH)
PROMPT = re.compile(rb'akios:[^\r\n#]*# $')


def drain(limit):
    buf = b''
    end = time.time() + limit
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            chunk = os.read(fd, 65536)
            buf += chunk
            if PROMPT.search(buf[-200:]):
                return buf, True
    return buf, False


def run(cmd, show=True):
    os.write(fd, b'\x15')                       # ^U: drop anything half typed
    os.write(fd, cmd.encode() + b'\r')
    out, ok = drain(timeout)
    text = out.decode(errors='replace').replace('\r', '')
    lines = text.split('\n')
    if lines and lines[0].strip().endswith(cmd.strip()):
        lines = lines[1:]
    if show:
        body = '\n'.join(l for l in lines if not PROMPT.search(l.encode() + b' ') and not l.startswith('akios:'))
        print('$ ' + cmd + '\n' + body.rstrip() + ('' if ok else '\n[no prompt within %d s]' % timeout))
    return ok


os.write(fd, b'\r')
drain(3)
if not klog:
    run('klog off', show=False)
    drain(2)
for c in args:
    run(c)
os.close(fd)
