"""gpt.py: read and change the phone's GUID partition table (LUN 0, 4096-byte blocks) on saved copies of its first and
last 8 blocks -- nothing here touches the phone. Every change keeps the other entries byte for byte, recomputes both
headers' and both arrays' CRCs, and is checked again after it is made.

    python3 -m ak.gpt show HEAD TAIL
    python3 -m ak.gpt carve HEAD TAIL FROM NAME MIB OUTDIR   shorten FROM by MIB at its end; a new partition NAME there
"""
import struct
import sys
import uuid
import zlib

BS = 4096
ESP_TYPE = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')


class Gpt:
    def __init__(self, head, tail):
        self.head, self.tail = bytearray(head), bytearray(tail)
        self.primary = self._header(self.head[BS:2 * BS])
        self.backup = self._header(self.tail[7 * BS:8 * BS])
        p, b = self.primary, self.backup
        if p['entries_lba'] != 2 or p['entry_size'] != 128:
            raise SystemExit('gpt: unexpected primary layout: %r' % p)
        self.count, self.size = p['count'], p['entry_size']
        self.blocks = p['backup_lba'] + 1
        self.tail_base = self.blocks - 8                       # the LBA of the tail copy's first block
        # room for the array: the blocks between the primary array's start and the first usable block (and as many
        # before the backup header); the array itself may be shorter (the phone's: 43 entries in 4 blocks)
        self.room = (p['first'] - p['entries_lba']) * BS // self.size
        if (b['my_lba'] != self.blocks - 1 or b['entries_lba'] != self.blocks - 1 - (p['first'] - p['entries_lba']) or
                b['entries_lba'] < self.tail_base or self.count > self.room or b['count'] != self.count):
            raise SystemExit('gpt: unexpected backup layout: %r' % b)
        self.parray = self.head[2 * BS:2 * BS + self.count * self.size]
        o = (b['entries_lba'] - self.tail_base) * BS
        self.barray = self.tail[o:o + self.count * self.size]
        for name, h, arr in (('primary', p, self.parray), ('backup', b, self.barray)):
            if zlib.crc32(arr) != h['array_crc']:
                raise SystemExit('gpt: the %s array CRC does not match' % name)
        if self.parray != self.barray:
            raise SystemExit('gpt: primary and backup arrays differ')

    @staticmethod
    def _header(block):
        (sig, rev, hsize, hcrc, _, my, backup, first, last, guid, elba, count, esize, acrc) = struct.unpack(
            '<8sIIIIQQQQ16sQIII', block[:92])
        if sig != b'EFI PART':
            raise SystemExit('gpt: no "EFI PART" header')
        h = bytearray(block[:hsize])
        h[16:20] = b'\0\0\0\0'
        if zlib.crc32(h) != hcrc:
            raise SystemExit('gpt: header CRC does not match')
        return dict(size=hsize, my_lba=my, backup_lba=backup, first=first, last=last, disk=guid, entries_lba=elba,
                    count=count, entry_size=esize, array_crc=acrc)

    def entries(self):
        out = []
        for i in range(self.count):
            e = self.parray[i * self.size:(i + 1) * self.size]
            tguid, pguid, first, last, attr = struct.unpack('<16s16sQQQ', e[:56])
            if tguid == b'\0' * 16:
                continue
            name = e[56:128].decode('utf-16-le').rstrip('\0')
            out.append(dict(index=i, type=uuid.UUID(bytes_le=tguid), guid=uuid.UUID(bytes_le=pguid), first=first,
                            last=last, attr=attr, name=name))
        return out

    def set_entry(self, i, tguid, pguid, first, last, attr, name):
        e = struct.pack('<16s16sQQQ', tguid.bytes_le, pguid.bytes_le, first, last, attr)
        e += name.encode('utf-16-le').ljust(72, b'\0')
        self.parray[i * self.size:(i + 1) * self.size] = e

    def write(self):
        """both arrays and headers, with fresh CRCs: (head, tail)"""
        acrc = zlib.crc32(self.parray)
        self.head[2 * BS:2 * BS + len(self.parray)] = self.parray
        o = (self.backup['entries_lba'] - self.tail_base) * BS
        self.tail[o:o + len(self.parray)] = self.parray
        for buf, off in ((self.head, BS), (self.tail, 7 * BS)):
            h = bytearray(buf[off:off + self.primary['size']])
            h[80:84] = struct.pack('<I', self.count)
            h[88:92] = struct.pack('<I', acrc)
            h[16:20] = b'\0\0\0\0'
            h[16:20] = struct.pack('<I', zlib.crc32(h))
            buf[off:off + len(h)] = h
        return bytes(self.head), bytes(self.tail)


def show(g):
    for e in g.entries():
        print('%3d %-24s %10d-%10d %8d MiB  %s' % (e['index'], e['name'], e['first'], e['last'],
                                                    (e['last'] - e['first'] + 1) * BS >> 20, e['type']))


def carve(g, src, name, mib):
    ents = g.entries()
    by = {e['name']: e for e in ents}
    if src not in by or name in by:
        raise SystemExit('gpt: %s missing or %s present already' % (src, name))
    s = by[src]
    blocks = mib * (1 << 20) // BS
    if s['last'] - s['first'] + 1 <= blocks * 2:
        raise SystemExit('gpt: %s too small to give %d MiB' % (src, mib))
    following = [e for e in ents if e['first'] > s['last']]
    nxt = min(following, key=lambda e: e['first']) if following else None
    free = next((i for i in range(g.count) if g.parray[i * g.size:i * g.size + 16] == b'\0' * 16), None)
    if free is None:                       # every entry used: the array one entry longer, inside its room
        if g.count + 1 > g.room:
            raise SystemExit('gpt: no room for another entry')
        free = g.count
        g.count += 1
        g.parray += b'\0' * g.size
    new_first, new_last = s['last'] - blocks + 1, s['last']
    g.set_entry(s['index'], s['type'], s['guid'], s['first'], new_first - 1, s['attr'], s['name'])
    g.set_entry(free, ESP_TYPE, uuid.uuid4(), new_first, new_last, 0, name)
    head, tail = g.write()
    # the result, read back with the same checks: only the two entries changed
    g2 = Gpt(head, tail)
    before = {e['name']: e for e in ents}
    after = {e['name']: e for e in g2.entries()}
    for n, e in before.items():
        if n != src and e != after[n]:
            raise SystemExit('gpt: %s changed unexpectedly' % n)
    print('%s: %d-%d -> %d-%d; %s: %d-%d (%d MiB, ESP), entry %d; next is %s at %d' % (
        src, s['first'], s['last'], s['first'], new_first - 1, name, new_first, new_last, mib, free,
        nxt['name'] if nxt else '-', nxt['first'] if nxt else 0))
    return head, tail


if __name__ == '__main__':
    a = sys.argv[1:]
    g = Gpt(open(a[1], 'rb').read(), open(a[2], 'rb').read())
    if a[0] == 'show':
        show(g)
    elif a[0] == 'carve':
        head, tail = carve(g, a[3], a[4], int(a[5]))
        open(a[6] + '/gpt-head.new', 'wb').write(head)
        open(a[6] + '/gpt-tail.new', 'wb').write(tail)
