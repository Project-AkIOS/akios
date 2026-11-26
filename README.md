# AkIOS

AkIOS runs Apple's iOS on Google Pixel phones: our own build of XNU (Apple's open-source kernel) with drivers for
the phone's hardware, booted by our own UEFI firmware, under iOS's unmodified user space taken from Apple's IPSW.
The first device is the **Google Pixel 8a** (`akita`, Tensor G3), running **iOS 27.2.1** with the identity of an
iPhone 13 mini.

The rule for every driver: the hardware is presented the way an iPhone presents its own (Apple-shaped IOKit
services, iPhone device-tree nodes, Apple's user clients), so iOS's own frameworks and daemons drive it unchanged.
Where that is not done yet, the code says it is a stopgap.

## What works (Pixel 8a)

Boot to the lock screen and home screen; touch and buttons; display at 120 Hz; the GPU through an IOGPU service
with a Metal driver (Mali-G715); Wi-Fi (scan, join from Settings, DHCP); Bluetooth (scan); storage (UFS, Apple's
APFS); battery, thermal, power management. Being worked on: the GPU's last rendering bugs, camera, audio routing,
cellular, Setup Assistant (skipped for now). Details per area are in the repositories below.

## Repositories

This repository is the umbrella: the build tool `ak`, the device descriptions, and the others as submodules.

| Path | Repository | What it is |
|---|---|---|
| `xnu/` | [xnu](https://github.com/Project-AkIOS/xnu) | the kernel: Apple's XNU at its release tag, our platform and drivers on top |
| `userland/` | [userland](https://github.com/Project-AkIOS/userland) | what AkIOS adds to iOS's user space: the RAM disk, launchd jobs, the AkiosMetal plug-in, tools |
| `metal/` | [metal](https://github.com/Project-AkIOS/metal) | the Metal core: AIR -> NIR -> Mali compiler, command buffers, the old CoreAnimation plug-in |
| `loader/` | [loader](https://github.com/Project-AkIOS/loader) | the UEFI boot manager (UniXNU) and XNU loader (DarwinBoot) on the phone's ESP |
| `firmware/` | [firmware](https://github.com/Project-AkIOS/firmware) | the EDK2 firmware for the phone's boot slot (built inside Mu-Silicium) |
| `devices/` | (here) | one `device.toml` per phone: [devices/README.md](devices/README.md) |
| `tools/` | (here) | `ak` (tools/ak), the kernel toolchain's sources, APFS writer/reader, IPSW unpacking, phone and shared-cache tools |

## Quick start

```
git clone --recurse-submodules https://github.com/Project-AkIOS/akios && cd akios
./ak toolchain                 # once: the kernel's cross toolchain (needs theos; see docs/getting-started.md)
./ak kernel && ./ak loader && ./ak firmware && ./ak userland && ./ak esp
fastboot flash esp out/akita/esp.simg
```

Setting up a machine and a phone the first time takes more than that: read
[docs/getting-started.md](docs/getting-started.md). Then:

- [docs/usage.md](docs/usage.md): every `ak` command, test and everyday images, boot arguments, logs, the phone tools
- [docs/architecture.md](docs/architecture.md): how AkIOS boots and how its pieces fit together
- [devices/README.md](devices/README.md): adding another Pixel
- [CREDITS.md](CREDITS.md): the work AkIOS is built on, and its licences

## Legal

AkIOS contains no Apple software beyond Apple's own open-source releases: iOS itself comes from an IPSW you provide,
and the vendor firmware the phone's chips need comes from the phone's own Android. Each repository states its
licence; the third-party parts and their licences are listed in [CREDITS.md](CREDITS.md). AkIOS is not affiliated
with Apple or Google.
