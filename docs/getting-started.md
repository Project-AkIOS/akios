# Getting started

From a fresh Linux machine to iOS on the phone. The examples use the Pixel 8a (`akita`); for another device see
[../devices/README.md](../devices/README.md) and add `--device <name>` to the `ak` commands.

## 1. The machine

A 64-bit Linux (developed on Kali/Debian), about 60 GB free.

```
sudo apt install git curl tar xz-utils make clang lld flex bison m4 perl python3 python3-venv autoconf automake \
    libcrypt-dev rsync lz4 p7zip-full lzfse meson ninja-build android-sdk-libsparse-utils fastboot adb \
    python3-capstone python3-pil
```

- **theos** in `~/theos` (`THEOS` overrides): its Linux toolchain (`toolchain/linux/iphone`: the cctools linker,
  `ldid`) and two SDKs in `sdks/`: `iPhoneOS16.5.sdk` (the kernel is built against it) and `iPhoneOS27.2.sdk`
  (user space). See https://theos.dev and https://github.com/theos/sdks.
- **The kernel's toolchain**: `./ak toolchain` fetches and builds what the kernel build needs besides theos (Apple's
  open-source mig, AvailabilityVersions, libdispatch's firehose, libpthread; tcsh, unifdef; a macOS SDK for the
  DriverKit headers) into `.cache/toolchain`. It runs `tools/toolchain/setup.sh`; slow the first time.
- **The firmware's build system**: a [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium) checkout with its
  submodules in `.cache/mu-silicium`, and a Python venv with its requirements in `.cache/mu-venv`:
  ```
  git clone --recurse-submodules https://github.com/Project-Silicium/Mu-Silicium .cache/mu-silicium
  python3 -m venv .cache/mu-venv && .cache/mu-venv/bin/pip install -r .cache/mu-silicium/pip-requirements.txt
  ```
- **The loader's build system**: Project Mu's basecore in `.cache/mu_basecore`; `./ak loader` prints the exact
  `git clone` line when it is missing.
- Optional: [apfsprogs](https://github.com/linux-apfs/apfsprogs) built in `.cache/apfsprogs` (`ak userland` then
  checks every image with `apfsck`).

## 2. The data (never in git)

`~/akita/akios-data/` holds what may not or need not be in a repository:

```
akios-data/
  ipsw-d16ap/                    the unpacked IPSW (iOS 27.2.1 24B156 for iPhone14,4)
  images/ios_system.apfs         the system volume as written to the phone (optional copy)
  devices/akita/usb_serial       the phone's USB serial number (fastboot getvar serialno), one line
  devices/akita/firmware/fw-*/   the vendor's files the drivers load, copied from the phone's Android /vendor/firmware
```

- **The IPSW**: download the iPhone's IPSW from Apple yourself, then
  `OUT=~/akita/akios-data/ipsw-d16ap tools/ipsw/unpack.sh /path/to/the.ipsw`. (These scripts made today's
  `ipsw-d16ap`; their move into akios is recent: check the result.)
- **QuartzCore's metallib** for the Metal driver: `tools/ca-metallib.py` extracts it from the unpacked dyld cache into
  `~/ca-share` (`[userland] ca_share` in akios.toml).
- **The vendor files**: `[vendor_files]` of the device's `device.toml` lists every one the build takes.

## 3. The phone

- The bootloader must be **unlocked** (`fastboot flashing unlock`; this wipes Android).
- **Partitions**: iOS lives in its own partitions carved out of the phone's storage (`ios_system`, `ios_preboot`,
  `ios_data`, and `esp` for the boot volume), next to an untouched Android. Making them is not automated in this
  repository yet; back up the partition table before touching it.
- **Our firmware** goes to one boot slot (`[flash] slot`, `b` on the Pixel 8a); Android keeps the other:
  ```
  ./ak firmware
  fastboot flash boot_b out/akita/firmware.img && fastboot set_active b
  ```
  `fastboot set_active a` gives the phone back to Android.

## 4. The first build and boot

```
./ak kernel        # out/akita/kernel
./ak loader        # out/akita/loader: UniXNU.efi, DarwinBoot.efi, Fat.efi
./ak userland      # out/akita/akios.apfs: the RAM disk iOS starts from
./ak esp           # out/akita/esp.simg: the boot volume
fastboot flash esp out/akita/esp.simg && fastboot reboot
```

The phone starts our firmware, which starts UniXNU from the ESP, which loads XNU; iOS then runs from the
`ios_*` partitions. The first boot of a new data volume runs iOS's data migration (a progress bar, ~25 minutes).

`./ak esp` makes a **test** image by default (it restarts the phone after 20 minutes and when the USB cable is
gone, so a hung test never strands the phone); `./ak esp --flash` makes the everyday one. See
[usage.md](usage.md).
