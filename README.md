# PAPEROS

A custom x86 GUI OS built from scratch on this PC. No Linux, no other OS base,
no downloads, and no toolchain beyond the MinGW, Python, and QEMU already
installed here.

## Run

| Script | What it boots |
| ------ | ------------- |
| `run.bat` | Live desktop from `os.img` as a 1.44 MB floppy; `disk.img` is attached as primary IDE storage |
| `run_iso.bat` | Live desktop from the generated `paperos.iso` |
| `run_install.bat` | ISO boot menu with `I` selected to install to `disk.img` |
| `run_installed.bat` | Installed PAPEROS from `disk.img` as the primary IDE disk |

All launchers use QEMU 11.1 with `-m 64 -vga none -device VGA,vgamem_mb=32`.
The monitor is exposed on TCP ports 4444-4447 for scripts and inspection.

At the boot menu:

```text
L = live desktop (default after timeout)
I = install to primary IDE disk
T = TTY text mode
1 = 640x400       2 = 640x480       3 = 800x600 (default)
4 = 1024x768      5 = 1280x720      6 = 1360x768
7 = 1280x1024     8 = 1600x1200     9 = 1920x1080
A = 2560x1440     B = 3840x2160
```

Press `t` or `i` for the text modes, or a resolution key to boot straight into
the GUI. All GUI modes are native 8-bpp VESA linear framebuffers; there is no
logical-to-physical UI scaling. `1360x768` replaces 1366/1368x768 because QEMU's
DISPI path rejects the 1368-wide mode.

## What is in it (v0.9)

- Two-stage 16-bit BIOS bootloader with an 11-entry resolution picker and
  boot menu (`L`, `I`, `T`)
- BIOS LBA kernel/stage-2 loads on installed hard disks; the verified CHS
  18-sector/2-head path remains for floppy and El Torito boots
- VESA VBE 2.0 8-bpp linear framebuffers up to 3840x2160
- BIOS 8x16 VGA font copied by stage 2 before switching video modes
- Bochs/QEMU VBE DISPI mode setting with read-back validation and fallbacks
- 32-bit flat protected-mode C kernel
- PIT channel 2 millisecond timer driver
- Damage-tracked double buffering with a 192-entry dirty-rectangle list and one
  vblank blit per frame
- PS/2 keyboard and mouse with click latching, packet resynchronisation, and
  shifted-symbol support for `+`, `*`, `_`, etc.
- Retro Windows-style window manager: raised bevels, 3-D title buttons, click
  to raise, minimise, send-behind and close; Start menu and taskbar adapt to
  twelve apps; custom cursor from `cursor.cur` with no menu hover trails
- Apps: Terminal, Paint, Notepad, Snake, Settings, File Manager, Calculator,
  System Monitor, Minesweeper, 2048, Brick Drop and Internet Browser
- Notepad filename editing with working Save/Load/New buttons
- File Manager list, Open, Delete, Format and Refresh backed by the real disk
- Calculator with button and keyboard input, including `+` and `*`
- System Monitor with uptime, video mode, window count, storage use and network
  state
- Settings tabs: Appearance (wallpaper, accent, wallpaper toggle, flat/3-D
  edges, reset), System, Network (real DHCP and gateway ping) and About
- Games: Minesweeper with flood reveal and flags, 2048 with keyboard controls,
  and Brick Drop, a complete falling-block game with rotation, soft/hard drop,
  line clears and a next-piece preview
- Terminal commands: `help ver clear echo color wall date about win`,
  `format ls cat touch rm df save load`, `net ip ping web [url]`, and app
  launchers `edit files calc mon snake paint note settings mines g2048 tetris
  games web`
- Real TTY text mode with persistent filesystem commands
- Text-mode installer with verification and an installed-disk boot path
- Polled NE2000 PCI driver, DHCP client and ICMP echo; preserved QEMU
  wallpaper/network behaviour

## Storage

PAPEROSFS lives on the primary IDE disk (`disk.img`, 16 MiB):

```text
LBA 0..1023     reserved for stage 1, stage 2 and kernel
LBA 1024        PAPEROS1 superblock
LBA 1025-1026   directory (32 entries, 32 bytes each)
LBA 1027+       contiguous file data
```

All reads and writes use ATA PIO 28-bit LBA. The installer writes pristine
boot sectors from `boot_images.h` and a pristine kernel copy held at
`0x1C00000` in guest RAM, then formats PAPEROSFS. Installed writes are
read back and retried before the installer reports success.

`build.py` always recreates `disk.img` as a blank 16 MiB target, so stale
filesystem metadata cannot mask installer bugs.

## ISO and installer

`build.py` is pure Python and needs no external ISO tool. It builds an El
Torito bootable ISO with a 1.44 MB floppy-emulation boot image (boot catalog at
LBA 17, boot image at LBA 20). The ISO supports all three boot choices: live
GUI, TTY and installer.

The installer writes the boot image and kernel to LBAs 0-1023, creates
PAPEROSFS at LBA 1024, verifies the result and prints `INSTALL COMPLETE`.
Reboot with `run_installed.bat` to boot the installed IDE disk.

## Network scope

The network code is raw Ethernet + IPv4 + DHCP + ICMP + UDP/DNS + TCP. The
browser is a real text-only HTTP/1.0 client: it resolves names through the
DHCP-provided DNS server, performs a TCP handshake, sends `GET`, reassembles
the response, strips HTML to text, and exposes links. No images, scripts,
cookies, TLS or sockets are implemented. QEMU user networking normally gives
PAPEROS `10.0.2.15`, gateway `10.0.2.2` and DNS `10.0.2.3`.

For a local test page, run `python -m http.server 8000` on the host and open
`http://10.0.2.2:8000/` in PAPEROS Web.

QEMU 11.1's `filter-dump` captures one direction per object, so `run.bat`
writes `net_rx.pcap` and `net_tx.pcap`. The delivered `net.pcap` is a merged
capture from both directions.

## Build

```bat
python build.py
```

Outputs:

```text
boot.bin / boot2.bin / kernel.bin   build intermediates (kernel 274,304 bytes)
os.img       1,474,560 bytes        1.44 MB live floppy
paperos.iso  1,523,712 bytes        El Torito live/install/TTY ISO
disk.img     16,777,216 bytes       blank IDE install target
```

`build.py` patches the kernel sector count into stage 2, regenerates
`boot_images.h` in a two-pass build, checks high-buffer boundaries, and zeroes
`disk.img` on every build.

## Verification

Serial output on a clean GUI boot is:

```text
PAPEROS VIDEO 800x600 PITCH 800 NATIVE
PAPEROS NET: init
PAPEROS NET: ne2k_pci at 0xC000
PAPEROS NET: MAC 52:54:00:12:34:56
PAPEROS NET: IP 10.0.2.15 GW 10.0.2.2
```

The same success pattern is verified from the live floppy, the ISO and the
installed IDE disk. TTY and installer logs are checked separately:

```text
PAPEROS 0.9 - TTY MODE
FS: formatted
FS: created
hello             0
papa
used 0 / total 31744

INSTALL COMPLETE
```

`install_diff.py` compares the installed kernel with `kernel.bin` and reports
zero bad sectors. GUI app state is checked through QEMU monitor memory reads
rather than screenshots: Start menu items, Settings controls, Calculator
button/keyboard arithmetic, Notepad Save/Load/New, File Manager actions, the
three new games, window stacking and the browser's DNS/TCP/HTTP result.

`net.pcap` has 6 packets: DHCP DISCOVER, OFFER, REQUEST and ACK, plus ICMP Echo
Request and Reply. DHCP types `(op, type)` are `(1,1)`, `(2,2)`, `(1,3)` and
`(2,5)`; ICMP types are `8` and `0`.

## Flicker, layout and driver details

See `FLICKER_FIX_NOTES.md`.

## Release

v0.9: retro Windows-style UI, real window stacking with send-behind, twelve
apps, three new games, a real DNS/TCP/HTTP browser, richer Settings, and the
verified floppy/ISO/installed boot paths from v0.8.
