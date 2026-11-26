# tools/iogpu

These tools read Apple's IOGPU interface out of iOS and macOS binaries. They are phase 0 of the GPU plan: our
kernel will speak `IOGPUDeviceUserClient`, so we need to read that interface, and read it again for each new
iOS release.

The tools only read their inputs. Output goes to stdout, or to `--out` (default `out/iogpu/`, which is not in git).

## Running

From the repository:

    python3 tools/iogpu CMD ...

or from `tools/`: `python3 -m iogpu CMD ...`. `python3 tools/iogpu CMD --help` shows the options of a command.

## Inputs

The defaults come from `akios.toml` `[iogpu]`. Each one can also be given on the command line.

| key | what | flag |
|---|---|---|
| `kernelcache` | iOS kernelcache, decompressed (MH_FILESET Mach-O). A `KC.symbols.json` beside it (`{address: name}`) names kernel symbols. | `--kc`, `--kc-symbols` |
| `kdk` | macOS Kernel Debug Kit, unpacked so that `pkg/KDK.pkg/Payload` exists | `--kdk` |
| `mac_kexts` | thin arm64e macOS kexts named `mac-NAME.arm64e` (what `kdk extract` writes) | `--mac` |
| `symbols` | iOS Symbols supplement: the directory that holds `System/`, or one level above | `--symbols` |
| `symbols_db` | the supplement's sqlite symbol database: `syms(addr, name, ...)`, `images(path, ...)` | `--db` |
| `[userland] ipsw` | the unpacked IPSW; its `*/dyld_shared_cache_arm64e` is the dyld cache | `--dsc` |

Current references: kernelcache 24B156 (iPhone14,4), KDK 26B131 (macOS 27.2.1), Symbols 24B156.

## Commands

### kc-classes KEXT...
Every C++ class in the kexts: name, instance size, superclass, metaclass address. It finds the
`OSMetaClass::OSMetaClass(meta, name, super, size)` calls in the kext's code.

    python3 tools/iogpu kc-classes com.apple.iokit.IOGPUFamily com.apple.AGXG14P

### kc-tables [KEXT]
The `IOExternalMethodDispatch` and `IOExternalTrap` tables of a kext (default IOGPUFamily). For each table it
gives the handler and the argument shape (scalarIn, structIn, scalarOut, structOut; `var` = variable size). A
user client keeps a "restricted" copy of each table right after it. For that copy the tool lists only the
entries that stay open. `--json` prints machine-readable output. `--list-kexts` lists the bundle ids.

    python3 tools/iogpu kc-tables
    python3 tools/iogpu kc-tables com.apple.AGXG14P

### kc-diff [--kc NEW] [--ref JSON] [--save JSON]
Run this first on every new iOS release. It dumps IOGPUFamily's tables from a kernelcache and compares them with
the saved reference, `data/iogpu-24B156-selectors.json`. It compares:

- every selector's shape, aligned by shape, so one added or removed selector shows as one change;
- the shape hash;
- the trap count;
- the open sets of both restricted copies.

The exit status is 0 when nothing changed. `--save` writes the new tables as a reference file. Names carry over
only where the shape at that index is unchanged. Check every `null`, and the traps, by hand against the macOS KDK
(`kdk dump IOGPUFamily IOGPUDeviceUserClient::sDeviceMethods`).

    python3 tools/iogpu kc-diff --kc ~/new/kernelcache.macho --save /tmp/iogpu-NEW.json

### kc-vtables [KEXT...]
The vtables of the kexts. Slot 7 (`getMetaClass`) identifies the class. The slots are named from the macOS kexts
(`--mac`, default: IOGPUFamily, AGXG13G and AppleParavirtGPUIOGPUFamily from `mac_kexts`).

- With no options it lists each class and says whether its slots match macOS one for one (`same`).
- `--class C` lists one vtable with names.
- `--pairs FILE` names the base part of each vendor class's vtable through its IOGPU base. It then lists the
  slots that the vendor kext implements itself. `data/pairs-AGXG14P.txt` holds the pairs for AGX.
- `--json` prints the vtable addresses and slot targets.

iOS vtables are the macOS ones without the `_RESERVED` slots, but not always one for one. For example, iOS
`IOGPU` has 217 slots and macOS has 218 (see the findings below). When the counts differ, the slots that the
kernel's symbols name, and the pure virtuals, anchor an alignment. The output then says which macOS slot is
missing. If unnamed slots leave the gap's position open, it says that too. Pure virtual slots in a macOS base
class have no name. They take the name that a macOS subclass gives the same slot, marked `[pure; named from X]`.

    python3 tools/iogpu kc-vtables com.apple.iokit.IOGPUFamily com.apple.AGXG14P
    python3 tools/iogpu kc-vtables --class IOGPUCommandQueue
    python3 tools/iogpu kc-vtables com.apple.AGXG14P --pairs tools/iogpu/data/pairs-AGXG14P.txt

### kdk SUB
macOS KDK kexts, which come with full symbols. KEXT is a path, or NAME for `mac-NAME.arm64e` in `mac_kexts`.

- `kdk extract [NAME...] [--out DIR]`: unpacks the kexts from the KDK pkg payload (pbzx, then cpio) and thins
  each one to arm64e as `mac-NAME.arm64e` (with the `.kext` beside it). The default set is IOGPUFamily,
  AppleParavirtGPUIOGPUFamily and AGXG13G. This reads the whole 1.2 GB payload and takes about two minutes.
- `kdk syms KEXT [REGEX]`: the defined symbols, demangled.
- `kdk imports KEXT [-C]`: the chained-fixup imports. `-C` demangles them.
- `kdk dump KEXT SYMBOL [--entry 24] [--count N]`: dumps a table. Entry 0 is a pointer and the rest are u32s.
  By default it stops at the next symbol.
- `kdk vtable KEXT CLASS`: the vtable slots of CLASS, with names.
- `kdk overrides KEXT [CLASS...]`: the slots that a class implements itself. Used on
  AppleParavirtGPUIOGPUFamily, this is the list of what a vendor IOGPU driver must provide.

      python3 tools/iogpu kdk extract --out /tmp/kdk
      python3 tools/iogpu kdk dump IOGPUFamily IOGPUDeviceUserClient::sDeviceMethods
      python3 tools/iogpu kdk overrides AppleParavirtGPUIOGPUFamily AppleParavirtGPU

### objc-supers CLASS... [--prefix P] [--methods] [--vs BASE]
Objective-C superclass chains, read from the Symbols Mach-Os (`class_t.superclass`).

- `--prefix` adds every class whose name starts with P.
- `--methods` lists each class's own methods and marks overrides.
- `--vs BASE` splits BASE's methods into those that the classes below it override and those they inherit.

      python3 tools/iogpu objc-supers --prefix AGXA14
      python3 tools/iogpu objc-supers AGXA14FamilyDevice --vs IOGPUMetalDevice

### dsc-dis LIB FUNC_REGEX | --calls CALLEE_REGEX
Disassembles functions of a dyld-cache library. Code comes from the device's dyld cache, function bounds from the
Symbols Mach-O, and names from the database. The output names calls (through branch islands), C strings,
CFStrings and the GOT slots it can resolve. LIB is an image path or just its file name. `--calls` lists every
call to a matching callee, with the constants last loaded into w1-w3. For IOConnect calls those are the selector
numbers.

    python3 tools/iogpu dsc-dis IOGPU '^_IOGPUDeviceCreateWithOptions$'
    python3 tools/iogpu dsc-dis IOGPU --calls '_IOConnect'

## Data

- `data/iogpu-24B156-selectors.json`: the reference interface of iOS 27.2.1 (24B156, iPhone14,4). It holds
  the 56 external methods (name, macOS selector, shape, handler, IOGPU.framework caller) and the 14 traps (name,
  argument count, handler, caller), plus the open sets of both restricted copies.
  - Regenerated with `kc-tables --json`: shapes, handlers and restricted sets.
  - From the research notes (KDK names matched by position and shape): names and callers.
- `data/pairs-AGXG14P.txt`: each AGX vendor class and its IOGPU base, for `kc-vtables --pairs`.

## Findings made while building these tools

- iOS `IOGPU` has 217 vtable slots and macOS has 218 without the reserved slots. macOS slot 200,
  `IOGPU::newSurface()`, is not on iOS. So iOS slots 200-216 are macOS 201-217. Slots 198 and 199 are
  `newDevice()` and `newDeviceUserClient()`: each allocates IOGPUDevice or IOGPUDeviceUserClient through its
  metaclass. Every other IOGPUFamily class that has a macOS twin matches one for one.
- `IOAcceleratorFamily2.kext` in KDK 26B131 is x86_64 only.

## Requirements

- python3 (3.11 or later for `tomllib`; on older versions, `tomli`), with `sqlite3` and `lzma` from the standard
  library.
- capstone (Python bindings, tested with 5.0.7): `apt install python3-capstone` or `pip install capstone`.
- llvm tools `llvm-nm`, `llvm-cxxfilt` and `llvm-lipo` (Debian/Kali package `llvm`).
- `cpio`, for `kdk extract`.

## Limits

- The kernelcache reader knows only the kernel-cache chained pointer format (arm64e iOS 15 and later).
- kc-classes finds metaclasses by pattern. A class whose constructor gets its metaclass from a caller it cannot
  see gets its address from the kernel symbols, if they name `Class::gMetaClass`.
- dsc-dis reads ObjC data (selector references) from the Symbols Mach-Os, not from the cache's `.dylddata`
  subcaches, because our cache reader reads those wrong. Some GOT and selector-reference slots therefore stay
  shown as plain addresses.
