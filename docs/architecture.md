# How AkIOS fits together

## The boot

```
Google's bootloader (ABL)           the phone's own, unlocked
  -> AkIOS firmware                 EDK2, in the boot slot ([flash] slot): display, UFS storage, A/B slot state
    -> UniXNU                       the boot manager, from the ESP partition (EFI/BOOT/BOOTAA64.EFI)
      -> DarwinBoot                 the XNU loader driver: the kernel, its Apple device tree, the RAM disk
        -> XNU                      our kernel, with the phone's drivers
          -> launchd (RAM disk)     mounts iOS's volumes: ios_system (APFS), ios_preboot (cryptexes), ios_data (/private/var)
            -> iOS                  Apple's user space as the IPSW has it, with Apple's own launchd jobs (few edits)
```

- The **firmware** (repository `firmware`) replaces U-Boot: ABL starts it from the boot partition as it would start
  Linux. Each device has its package (`Platforms/<vendor>/<device>Pkg`) on top of its SoC's (`Silicon/...`).
- The **ESP** is what `ak esp` makes: UniXNU, its `akios.toml` (the boot entry and its boot line), DarwinBoot, the
  kernel, `kernel.adt` and the RAM disk `akios.apfs`.
- **Darwin's device tree** (`tools/ak/adt.py`) is built on the PC: the iPhone's own nodes from the IPSW (who iOS
  thinks it runs on: `[device] iphone_tree`), the phone's numbers from its `[platform]`, and placeholders DarwinBoot
  fills at boot (memory window, random seed, RAM disk).

## The kernel (`xnu/`)

Apple's XNU at its release tag with our commits on top: a platform for the phone's SoC, and drivers for its
hardware, each a build option chosen by the device's `[kernel] drivers`. The drivers present the hardware as an
iPhone's drivers do, so iOS needs no changes:

- display: `IOMobileFramebuffer`, surfaces: `IOSurface`; the GPU: an `IOGPU` service with `IOGPUDeviceUserClient`
  (Apple's selector table), which iOS's IOGPU.framework talks to as on an iPhone
- touch and buttons: `IOHIDEventService`s (IOHIDFamily's, APSL)
- Wi-Fi: the `arm-io/wlan` device-tree node and `IO80211APIUserClient`; Bluetooth: `AppleBluetoothModule` on
  `arm-io/bluetooth` with the serial channel bluetoothd expects
- storage: UFS block devices, Apple's APFS and HFS; battery, thermal, power as `IOPMPowerSource` and kin; the USB
  side as `AppleUSBDeviceMux` (usbmux)

## User space (`userland/`, `metal/`)

`ak userland` builds the RAM disk from `userland/userland.toml`: our launchd (which mounts iOS's volumes and starts
Apple's), Apple's launchd jobs with a few edits (`userland/jobs/edits.toml`), the vendor files (`/fw`), and our
additions:

- **Metal**: two plug-ins over the Metal core of `metal/` (Metal's shader language AIR compiled to Mesa's NIR,
  then by Panfrost's back end for the Mali GPU). The new one, AkiosMetal, is built on Apple's IOGPU.framework as an
  iPhone's GPU driver is; the old one (`metal/ca-plugin`) is the default until AkiosMetal replaces it.
- bring-up aids, each named as such and switchable: e.g. `skip_setup=1` in SpringBoard.
- tools on the phone: the serial console, logging, test programs.

## The build (`tools/ak`)

`ak` reads `akios.toml` (the same for every device) with the device's `devices/<name>/device.toml` merged over it,
and writes into `out/<device>/`. The kernel's toolchain is `tools/toolchain` (built into `.cache/toolchain`), the
firmware's Mu-Silicium (`.cache/mu-silicium`), the loader's Project Mu (`.cache/mu_basecore`).
