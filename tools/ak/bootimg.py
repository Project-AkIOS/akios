"""The phone's boot image for our firmware: what `ak firmware` writes to out/firmware.img.

Google's bootloader on the Pixel 8a (akita) starts what is in the boot partition (or what `fastboot boot` sends) as an
arm64 Linux kernel. It wants the image as the phone's own boot partition has it (read off the phone 2026-10-31,
bootloader akita-16.4-14540574):

  - Android boot image header version 4, page size 4096; no ramdisk worth the name (the bootloader takes init_boot's
    and vendor_boot's ramdisks, never boot's): one zero byte
  - the kernel in LZ4's legacy frame (02 21 4c 18), as `lz4 -l -12` makes it; the device tree is the bootloader's own
  - an unsigned AVB hash footer, the image made as long as the boot partition (0x4000000): zuma's vbmeta chains boot,
    and an image with no footer is refused even unlocked, while an unlocked bootloader allows an unsigned one

The header is written here directly (byte for byte what AOSP's mkbootimg and avbtool made for these inputs: checked 11-26 against
the old tools' out/firmware.img).
"""
import hashlib
import struct
import subprocess
import sys

PAGE = 4096
BOOT_PARTITION = 0x4000000          # fastboot getvar partition-size:boot_a


def lz4_legacy(data):
    """DATA in LZ4's legacy frame, as the phone's own kernel is (lz4 -l -12)"""
    return subprocess.run(['lz4', '-l', '-12', '-f', '-q', '-c', '-'], input=data, stdout=subprocess.PIPE,
                          check=True).stdout


def boot_image_v4(kernel, ramdisk=b'\0', cmdline=b''):
    """an Android boot image, header version 4 (bootimg.h boot_img_hdr_v4): header, kernel, ramdisk, and the boot
    signature section mkbootimg reserves (one page of zeros: GKI's place for a signature, unused here), page-aligned"""
    header = struct.pack('<8sIIII4II1536sI', b'ANDROID!', len(kernel), len(ramdisk), 0, 1584, 0, 0, 0, 0, 4,
                         cmdline, PAGE)
    pad = lambda b: b + b'\0' * (-len(b) % PAGE)
    return pad(header) + pad(kernel) + pad(ramdisk) + b'\0' * PAGE


def avb_hash_footer(image, partition_size=BOOT_PARTITION, partition_name='boot'):
    """IMAGE with an unsigned AVB hash footer, PARTITION_SIZE bytes long: what avbtool add_hash_footer --algorithm NONE
    makes (libavb's avb_footer.h, avb_vbmeta_image.h, avb_hash_descriptor.h; big-endian): the image padded to 4 KiB,
    the vbmeta image (a 256-byte header, no authentication block, an auxiliary block with one hash descriptor), zeros,
    and the 64-byte footer in the partition's last bytes"""
    salt = b''
    digest = hashlib.sha256(salt + image).digest()
    name = partition_name.encode()
    body = struct.pack('>Q32sLLLL60s', len(image), b'sha256', len(name), len(salt), len(digest), 0, b'')
    body += name + salt + digest
    body += b'\0' * (-(16 + len(body)) % 8)
    descriptor = struct.pack('>QQ', 2, len(body)) + body
    aux = descriptor + b'\0' * (-len(descriptor) % 64)
    header = struct.pack('>4sLLQQL' 'QQQQQQQQQQ' 'QLL48s80s',
                         b'AVB0', 1, 0,             # required libavb 1.0
                         0, len(aux),               # authentication block (none: unsigned), auxiliary block
                         0,                         # algorithm NONE
                         0, 0, 0, 0,                # hash, signature: none
                         0, 0, 0, 0,                # public key and its metadata: none
                         0, len(descriptor),        # the descriptors, at the start of the auxiliary block
                         0, 0, 0,                   # rollback index, flags, rollback index location
                         b'hakios mkubootimg', b'') # (the release string the images on the phone carry)
    assert len(header) == 256
    vbmeta = header + aux
    vbmeta_offset = len(image) + (-len(image) % PAGE)
    end = vbmeta_offset + len(vbmeta)
    if end + 64 > partition_size:
        sys.exit('ak: the boot image and its vbmeta (%d bytes) do not fit a partition of %d' % (end, partition_size))
    footer = struct.pack('>4sLLQQQ28s', b'AVBf', 1, 0, len(image), vbmeta_offset, len(vbmeta), b'')
    return image + b'\0' * (vbmeta_offset - len(image)) + vbmeta + b'\0' * (partition_size - 64 - end) + footer


def firmware_image(kernel):
    """out/firmware.img from the firmware's arm64 Image (BootShim + the FD)"""
    if kernel[0x38:0x3c] != b'ARM\x64':
        sys.exit('ak: the firmware has no arm64 Image header')
    return avb_hash_footer(boot_image_v4(lz4_legacy(kernel)))
