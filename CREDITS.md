# Credits

AkIOS stands on other people's work. This lists what it is built from, what it contains, and what it learned from.
Licences are given as the projects state them; each repository's own LICENSE/NOTICE files are authoritative.

## The project AkIOS grew from

- **HakiOS** — [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) and
  [oosexgang/hakios-metal](https://github.com/oosexgang/hakios-metal), by the HakiOS authors (among them vcvkk and
  shimoku74). AkIOS began as HakiOS's port to the Pixel 8a and keeps much of its groundwork: the way XNU is built on
  Linux, the kernel's platform and driver framework, the APFS tools, the user-space build, and the Metal driver,
  whose repository (`metal`) has hakios-metal's history up to 5974ea9 as its base (MIT).

## Contained in AkIOS

| Work | Where | Licence |
|---|---|---|
| [XNU](https://github.com/apple-oss-distributions/xnu), Apple | `xnu/` (our kernel is XNU with our commits on top) | APSL 2.0 |
| [IOHIDFamily](https://github.com/apple-oss-distributions/IOHIDFamily) 2238.100.59, Apple | `xnu/iokit/Kernel/IOHIDFamily` (the event service and its user client, ported into the kernel) | APSL 2.0 |
| [UniXNU](https://unixnu.com) and DarwinBoot, the UniXNU contributors | `loader/` (the boot manager and the XNU loader, as released, with our changes) | BSD-2-Clause-Patent |
| [Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium), Project Silicium | `firmware/` (our device and SoC packages started from its Exynos 2400 ones; built inside it) | BSD-2-Clause-Patent |
| [Project Mu](https://github.com/microsoft/mu_basecore) / [TianoCore EDK2](https://github.com/tianocore/edk2) | the firmware's and the loader's base (fetched at build time) | BSD-2-Clause-Patent |
| [Mesa](https://gitlab.freedesktop.org/mesa/mesa): NIR, the Panfrost compiler | the Metal core's Mali back end (built from Mesa 26 at build time); Adreno facts in `metal/` (see its NOTICE) | MIT |
| [libzbitmap](https://github.com/eafer/libzbitmap), Ernesto A. Fernández / Corellium | `xnu/bsd/miscfs/apfs/libzbitmap` (decompression only) | GPL-2.0+ or MIT, taken under MIT |
| [ios-sapphire](https://github.com/Angelpro09xd/ios-sapphire) | the Wi-Fi registry entries MobileGestalt needs (`xnu/iokit/Kernel/hakios.wlan.cpp`, from its patches 0157/0158) | APSL 2.0 |

## Used to build AkIOS (fetched, not contained)

- Apple's open source: [libdispatch](https://github.com/apple-oss-distributions/libdispatch) (the firehose
  buffer), [libpthread](https://github.com/apple-oss-distributions/libpthread),
  [AvailabilityVersions](https://github.com/apple-oss-distributions/AvailabilityVersions), and mig from
  [bootstrap_cmds](https://github.com/markmentovai/bootstrap_cmds) (APSL 2.0) — `tools/toolchain/setup.sh`
- [theos](https://theos.dev) and [theos/sdks](https://github.com/theos/sdks): the Linux iOS toolchain (cctools,
  ld64, ldid) and the iOS SDKs; [macosx-sdks](https://github.com/joseluisq/macosx-sdks): the macOS SDK whose
  DriverKit headers stand in for Apple's iig
- [tcsh](https://github.com/tcsh-org/tcsh), [unifdef](https://github.com/fanf2/unifdef), LLVM/Clang
- [apfsprogs](https://github.com/linux-apfs/apfsprogs): `apfsck` checks every image `ak userland` makes
- [usbmuxd](https://github.com/libimobiledevice/usbmuxd) (libimobiledevice, GPL-2.0/GPL-3.0): the PC side of the
  USB connection; AkIOS uses a fork with fixes for this phone
- AOSP's boot image and AVB formats (mkbootimg, avbtool; Apache-2.0): `tools/ak/bootimg.py` writes the same formats

## Read as documentation (no code taken)

- The Linux kernel and Google's Pixel kernel sources (GPL): the phone's hardware (PCIe, UFS, the Mali GPU's CSF
  interface, Broadcom Wi-Fi/Bluetooth, Cirrus amplifiers and haptics, the modem's boot). Where our drivers follow
  their sequences, the comments name the source; none of that code is in AkIOS.
- AOSP and the phone's own Android: register values and boot sequences read on the running phone.

## Learned from (other ports and projects, consulted for ideas)

- [UniXNU](https://github.com/UniXNU) (iOS/macOS on non-Apple ARM64), including its AdrenoMetal and iOS 26 work
- [iOSPlus10](https://github.com/akshaynexus/iOSPlus10) (iOS 26 on the OnePlus 10 Pro; MIT),
  [PixelIOS](https://github.com/PixOS-Project/pixelios-kernel) (Pixel 6a), OneiOS, Scion
- [Darling](https://github.com/darlinghq)'s Indium and Iridium, darling-metal, metal2vulkan (Metal implementations)
- [mapple_gpu_mali](https://github.com/open-mapple/mapple_gpu_mali) (Metal on Mali Valhall; GPL-3.0: ideas only, no code)
- [blacktop/ipsw](https://github.com/blacktop/ipsw) and its symbolicator signatures (reading kernelcaches)

## Apple and Google

iOS, its frameworks and daemons are Apple's and are not part of AkIOS: each user supplies an IPSW from Apple. The
phone's vendor firmware (GPU, Wi-Fi, audio, camera, modem) is Google's and its suppliers', copied from the phone's
own Android and never published here. AkIOS is not affiliated with Apple or Google.
