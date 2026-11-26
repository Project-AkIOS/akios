# Using ak and the tools

`./ak` builds, flashes and watches AkIOS. Every command works for one device: akios.toml's `device`, or
`./ak --device NAME ...`, or `AKIOS_DEVICE=NAME`. What it makes goes to `out/<device>/`; every run's log goes to
`[paths] logs/<command>-<time>/` (default `~/akita/logs/ak`).

## Commands

| Command | What it does |
|---|---|
| `ak devices` | the devices ak knows (`devices/*/device.toml`); `*` marks the one it works for |
| `ak toolchain` | (re)makes `.cache/toolchain` from `tools/toolchain` (the kernel's cross toolchain) |
| `ak kernel` | configures the kernel's drivers from the device's `[kernel] drivers`, builds XNU from `xnu/`: `kernel`, `kernel.full` |
| `ak loader` | builds the UEFI loader from `loader/`: `UniXNU.efi`, `DarwinBoot.efi`, `Fat.efi` |
| `ak firmware` | builds the device's EDK2 firmware (Mu-Silicium + `firmware/`) into `firmware.img`, as its bootloader wants it |
| `ak fwboot NAME` | RAM-boots `firmware.img` once (`fastboot boot`) and keeps its log |
| `ak userland [--oslog LABELS]` | builds `akios.apfs`, the RAM disk, from `userland/userland.toml`; `--oslog`: those launchd jobs' os_log goes to the console |
| `ak esp [--quiet] [--flash] [--speaker] [ARGS]` | builds `esp.img` / `esp.simg`: UniXNU, DarwinBoot, the kernel, its device tree, the RAM disk; ARGS are extra boot arguments |
| `ak log [--kernel] [--secs N]` | the phone's console log (or the kernel's) over the USB cable |
| `ak status` | where the phone is: fastboot, iOS (usbmux) or Android (adb) |

## Test and everyday images

- `ak esp` (a **test** image): the phone restarts by itself after `[test] test_reboot_after` seconds (20 min) and
  `usb_lost_reboot` seconds without a USB host; it never powers off (`halt_restarts=1`).
- `ak esp --quiet`: no sound, and a restart goes to the bootloader (`restart_fastboot=1`). The default between
  `[test] quiet_hours` (22:00-09:00).
- `ak esp --flash`: the **everyday** image, no kill switches; with `--quiet` no sound either.

Flash with `fastboot flash esp out/<device>/esp.simg`; to get to fastboot from a running iOS:
`python3 tools/phone/usbmux-reboot.py bootloader`.

## Boot arguments

The boot line is akios.toml's and the device's `[phone] cmdline`, plus the test switches, plus ARGS of `ak esp`
(at most `[phone] cmdline_max` characters: the phone cuts longer lines). The kernel's defaults for arguments the
line does not name are the device's `[kernel] default_args`. Some that are handy:

| Argument | Effect |
|---|---|
| `skip_setup=1` | SpringBoard skips the Setup Assistant (a bring-up aid until Setup works) |
| `metal_plugin=../../../akita/lib/AkiosMetal metal_class=AkiosMaliDevice` | the new IOGPU-based Metal plug-in instead of the old one |
| `touch_lat=1` | per-frame and per-touch timing lines (`tl:`) in the console log |
| `systrace=1`, `ustacks=NAME`, `exit_probe=NAME` | system-call traces; a process's thread stacks (`tools/phone/usbmux-touch.py stacks`); stacks at exit |
| `hid_monitor=1` | HID events as they reach backboardd |
| `bt_vsc=0` / `bt_vsc=2` | Bluetooth: pass every vendor command's failure through / the old blanket rewrite |

## The phone tools (`tools/phone/`)

All talk to the running phone over the USB cable (the kernel's usbmux; a usbmuxd on the PC):

| Tool | Use |
|---|---|
| `usbmux-shot.py OUT.png [SCALE]` | a screenshot of what the panel shows (`SHOT_PORT=9`: the newest frame handed to the display) |
| `usbmux-touch.py tap X Y` / `swipe X1 Y1 X2 Y2 STEPS` / `button lock` / `stacks N` | touch, buttons, thread stacks |
| `usbmux-log.py [-k]` | the console (or kernel) log, live |
| `usbmux-reboot.py [bootloader]` | restart, or restart into fastboot |
| `usbmux-install.py`, `usbmux-pull-*.py`, `usbmux-rec.py`, `usbmux-sound.py` | install an app, pull logs/caches/frames, record the screen or the sound |
| `serial-console.py`, `serial-run.py 'CMD' ...` | the shell on the USB serial console (`ls`, `cat` built in) |

## Other tools

- `tools/dsc/`: the iOS shared cache: `dsc-dis.py IMAGE SYMBOL` disassembles a function by name, `dsc-symdb.py`
  finds symbols and addresses (with the iOS Symbols supplement in `~/akita/ref`).
- `tools/apfs/`: our APFS writer (`mkapfs.py`) and an independent reader (`apfs.py`) that checks every image.
- `tools/ipsw/`: unpacking an IPSW into `akios-data`.
- `tools/iogpu`: reading Apple's GPU driver interfaces out of a kernelcache or a macOS KDK (research).
