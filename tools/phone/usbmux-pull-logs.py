#!/usr/bin/env python3
"""Every program's log fetched over the USB cable (boot-arg log_all=1, 11-21): the phone's cache_send serves the *.log
files of /private/var/mobile/Media/hakios-log on its 127.0.0.1:31415 (job line: /akita/bin/cache_send 900
/private/var/mobile/Media/hakios-log), the same protocol as usbmux-pull-cache.py; no lockdownd needed.

usage: usbmux-pull-logs.py OUTDIR       OUTDIR/NAME-PID.log for every one (the directory emptied of older *.log first)
"""
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
pull = __import__('usbmux-pull-cache')


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    s = None
    for _ in range(60):
        s = pull.mux_connect(pull.PORT)
        if s is not None:
            break
        time.sleep(1)
    if s is None:
        sys.exit('usbmux-pull-logs: no answer on the phone\'s port %d (is cache_send running?)' % pull.PORT)
    if pull.read_exact(s, 8) != b'HKMC0002':
        sys.exit('usbmux-pull-logs: not cache_send\'s greeting')
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        if f.endswith('.log'):
            os.unlink(os.path.join(out, f))
    files = total = 0
    while True:
        (length,) = pull.struct.unpack('<I', pull.read_exact(s, 4))
        if length == 0:
            break
        name = pull.read_exact(s, length).decode('utf-8', 'replace')
        size, _ = pull.struct.unpack('<QQ', pull.read_exact(s, 16))
        data = pull.read_exact(s, size)
        if '/' in name or not name.endswith('.log'):
            continue
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
        files += 1
        total += size
    s.close()
    print('usbmux-pull-logs: %d logs, %.1f MB into %s' % (files, total / 1e6, out))


if __name__ == '__main__':
    main()
