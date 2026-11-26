# Devices

AkIOS is built per device. Everything that is the same for every phone is in `../akios.toml`; each phone has a
directory here with its `device.toml`, which `ak` merges over akios.toml (the device's values win, table by table).

```
./ak devices                      # the devices ak knows; * = the one it works for (akios.toml's device)
./ak --device akita esp --quiet   # any command for a given device (or AKIOS_DEVICE=akita ./ak ...)
```

What ak makes for a device goes to `out/<device>/`; its private data (never in git) is in its `[device] data`
directory: `usb_serial` (the phone's USB serial number, one line) and `firmware/` (the vendor's files, `fw-*/`).

| Device | Phone | SoC | State |
|---|---|---|---|
| `akita` | Google Pixel 8a | Tensor G3 (zuma) | boots iOS 27.2.1 |

## Adding a Pixel

1. **The directory.** `devices/<codename>/device.toml`, from `_template/device.toml.example`. `[device] name` is the
   codename (what fastboot and the firmware call it: `shiba`, `husky`, `lynx`, ...).
2. **Its data.** `~/akita/akios-data/devices/<codename>/usb_serial` (`fastboot getvar serialno`) and `firmware/` with
   the vendor's files the drivers need, copied from the phone's Android (`/vendor/firmware`); list them in
   `[vendor_files]`.
3. **The boot image** (`[boot_image]`): read the phone's own boot partition (`fastboot getvar partition-size:boot_a`,
   AOSP's `unpack_bootimg` on a pulled `boot.img`): header version, how the kernel is packed (`lz4` legacy frame or
   `raw`), and whether its vbmeta chains boot (then `avb_footer` = the partition's size). `tools/ak/bootimg.py`
   makes header version 4 so far.
4. **The firmware** (`[firmware]`): a Mu-Silicium device package in the firmware repo,
   `firmware/Platforms/<vendor>/<package>` with its `Resources/Configs/<config>`; `ak firmware` builds it into
   `out/<codename>/firmware.img` for the boot slot (`[flash] slot`; Android keeps the other).
5. **Darwin's device tree** (`[platform]`): the numbers from the phone's Android device tree
   (`/sys/firmware/devicetree`, or a dtb dump): the peripheral window, the GIC's distributor and redistributors, the
   cores (count, MPIDR shift, which are the small ones), the arch timer's frequency, a reserved RAM zone for the panic
   log, the memory window to give the kernel. `[device] iphone_tree` names the iPhone whose identity iOS gets.
6. **The kernel's drivers** (`[kernel] drivers`): the generic ones (`guard`, `bt_host`, `battery`, `ufs`,
   `wifi_station`) and the phone's. A phone on a SoC that already has drivers reuses them (the Pixel 8 and 8 Pro are
   zuma like the 8a: today's `akita_*` drivers are zuma's, named after the first phone); a new SoC (gs101 for the
   Pixel 6 family, gs201 for the 7, ...) needs its own, under the same rules as every driver here: presented the
   way an iPhone presents its hardware (Apple-shaped IOKit services, iPhone device-tree nodes). Each driver is a
   build option in xnu's conf files; `[kernel] default_args` are the kernel's defaults for that phone.
7. **The boot line** (`[phone] cmdline`): the display (`fbwidth`, `fbheight`, `fbscale`, `display_hz`), the cores
   (`cpus=`), and the drivers' switches.
8. **Build and boot**: `./ak --device <codename> kernel`, `loader`, `firmware`, `userland`, `esp`; flash the firmware
   to the boot slot once and the ESP (`fastboot flash esp out/<codename>/esp.simg`) after every build.
