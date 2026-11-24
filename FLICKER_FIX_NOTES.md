# PAPEROS flicker, GUI, storage and driver notes (v0.9)

## Summary

v0.4 was stable and had networking, but it still used a fixed 320x200 logical
desktop stretched into larger modes. v0.5 moved the GUI to native framebuffer
coordinates, added a millisecond timer driver, tracked damage rectangles, and
published only changed pixels. v0.6 expanded the picker to 11 modes through
3840x2160 and replaced VBE mode switching with a validated Bochs/QEMU DISPI
path. v0.7 added the Start menu and embedded `cursor.cur`. v0.8 added real
persistent storage, an installer, TTY mode, four new apps, and a boot menu.
v0.9 adds retro Windows styling, real z-order changes, four more apps, three
games, and a real text-only browser, still without changing the lightweight
polling design.

## Boot menu and boot modes

Stage 1 saves the BIOS boot drive at `0x501E`, shows text mode, and loads
stage 2. Stage 2 shows the boot menu and writes:

- `0x501A`: kernel sector count (patched by `build.py`)
- `0x501C`: boot mode (`0=live`, `1=TTY`, `2=install`)
- `0x501E`: boot drive
- `0x5020`: menu choice marker

`L` is the default live GUI. `T` enters a text-only shell; `I` runs the
text-mode installer. Resolution keys `1`-`B` boot the GUI directly in that
mode. `sendkey t` / `sendkey i` are used by the QEMU test harnesses.

Stage 1 keeps `sti` enabled because BIOS and El Torito disk calls need IRQs.
Stage 2 executes `cli` before entering protected mode because PAPEROS polls
all devices and installs no IDT.

Hard-disk boots use BIOS extended reads (`AH=42h`) with a disk address packet.
Floppy and El Torito floppy-emulation boots keep the verified CHS path with
18 sectors and 2 heads. This lets the same image boot live from floppy/ISO and
installed from IDE.

## Video mode setting

Stage 2 always boots VBE mode `0x0103` (800x600, 8bpp, linear framebuffer).
That known-good mode provides the LFB and the BIOS font before the final mode
is chosen. The selected width, height and native flag are stored at
`0x5014`, `0x5016` and `0x5018`; the kernel applies the requested mode.

VBE modes below 1024 pixels wide can still be set through the BIOS. Wider
modes use the Bochs VBE DISPI registers at `0x1CE`/`0x1CF`. The kernel reads
width, height, pitch and bpp back after setting DISPI and falls back to smaller
valid modes if the requested mode is rejected. The serial line reports the
accepted geometry, for example:

```text
PAPEROS VIDEO 1920x1080 PITCH 1920 NATIVE
```

The DAC mask is explicitly set to `0xFF` and the full 256-entry PAPEROS palette
is loaded through ports `0x3C8`/`0x3C9`.

## Resolution picker

| Choice | Resolution |
| ------ | ---------- |
| 1      | 640x400    |
| 2      | 640x480    |
| 3      | 800x600 (default) |
| 4      | 1024x768   |
| 5      | 1280x720   |
| 6      | 1360x768   |
| 7      | 1280x1024  |
| 8      | 1600x1200  |
| 9      | 1920x1080  |
| A      | 2560x1440  |
| B      | 3840x2160  |

1360x768 replaces 1366/1368x768 because the QEMU DISPI path rejects the
1368-wide mode and falls back to 1280x720. All 11 entries were booted and
logged their expected `PAPEROS VIDEO WxH PITCH W NATIVE` line.

## Memory layout

- Kernel loads at `0x10000`; stage 2 loads up to 768 sectors and bumps ES when
  BX wraps every 128 sectors.
- Stack is at `0x80000`.
- Hidden back buffer `BACK` is at `0x100000` (1 MiB).
- Static wallpaper layer `DESKTOP` is at `0x1000000` (16 MiB).
- Paint's canvas is allocated at `0x1A00000` (26 MiB).
- Pristine kernel copy for the installer is at `0x1C00000` (28 MiB).
- BIOS 8x16 font is copied to `0x6000` while still in real mode.
- Guest RAM is 64 MiB, so all high buffers fit with room to spare.

`build.py` rejects a kernel that would overlap the stack or high buffers.

## Damage tracking and speed

The old single dirty bounding box was replaced with a 192-entry list of small
rectangles. Touching rectangles are merged; if the list overflows, the kernel
falls back to a full-screen redraw. Only listed rectangles are restored from
`DESKTOP`, redrawn for windows/taskbar/cursor, and copied to the LFB.

Fills are clipped per row with `rep stosb`. Window, taskbar and cursor redraws
are skipped when they do not intersect a dirty rectangle. Frame publishing
waits for vertical retrace once per frame rather than per rectangle. Idle time
sleeps about 1 ms through the PIT instead of busy-spinning.

## PIT timer driver

Channel 2 of the 8253/8254 PIT is configured in mode 3 with a 65536 reload.
`pit_ms()` returns a monotonic millisecond count, used for:

- ~60 Hz redraw pacing
- ~150 ms Snake steps
- ~120-520 ms Brick Drop gravity
- ~500 ms System Monitor refresh
- NE2000 reset/initialisation timeouts
- 2-second DHCP discovery/request timeouts
- 1-second ping timeout

## GUI layout, clipping and apps

The BIOS 8x16 font is used at native pixel size. Windows, taskbar and all app
content sizes are derived from the current framebuffer size. Drawing is
clipped through `set_clip()`/`restrict_clip()` and bounded text helpers
(`draw_text_n()`), so terminal, notepad, taskbar, settings and monitor labels
stay inside their widgets at every resolution.

The twelve Start menu/taskbar apps are Terminal, Paint, Notepad, Snake,
Settings, File Manager, Calculator, System Monitor, Minesweeper, 2048,
Brick Drop and Internet Browser. `MAXW` is 12, and task-button width is
computed from the framebuffer so all twelve buttons fit at 800x600.

- Notepad has an editable filename and working Save, Load and New buttons.
- File Manager lists real PAPEROSFS entries and implements Open, Delete,
  Format and Refresh.
- Calculator supports both the button grid and keyboard input. Shifted keys
  are mapped in `shifted_key()`, so `+` and `*` work. Starting a new operand
  no longer clears the pending accumulator/operator.
- System Monitor shows uptime, video geometry, window count, storage use and
  network state, and refreshes twice a second.
- Settings has Appearance, System, Network and About tabs. Appearance changes
  wallpaper colour, accent, wallpaper visibility and 3-D edges; Network can
  renew DHCP and ping the gateway.
- Minesweeper has flood reveal, flags, a 28-mine 16x12 board and safe first
  click. 2048 has merge/score logic, spawn, game-over detection and arrow/WASD
  input. Brick Drop has seven shapes, rotation, soft/hard drop, line clearing,
  score/lines and a next-piece preview.
- Internet Browser resolves DNS through the DHCP server, connects with TCP,
  sends HTTP/1.0 `GET`, reassembles the reply, strips tags/entities, lists links
  and supports Back, Load, Home, link clicks and scrolling.

Native coordinates removed the old `UI_SCALE` division, so cursor movement
matches visible pixels at every resolution.

## Start menu and custom cursor

The taskbar has a START button followed by twelve app buttons. Clicking START
opens a popup menu above the taskbar; clicking an item opens that app. Esc
toggles the menu, and clicking outside it closes it. The menu is drawn after
the taskbar but before the cursor, so it stays above windows and does not
flicker.

Windows are stored in z-order from bottom to top. Clicking a window moves its
`Win` slot to the end of the array and updates `focus`/`drag_win` indices; the
middle title-bar button moves it to index 0 (send behind). The close path now
compacts the array instead of leaving holes, so repeated stacking stays
consistent with the redraw and hit-test loops.

`cursor.cur` is converted by `make_cursor.py` into `cursor.h`: a 13x21
transparency mask plus one 8-bpp palette index per visible pixel. The kernel
draws it at 1x below 2000 pixels wide and 2x at 2000+ pixels. Cursor damage
uses the full scaled cursor rectangle. Menu hover changes mark the old and new
item rectangles, so moving the cursor across the Start menu leaves no trails.

## ATA and PAPEROSFS

PAPEROSFS is a small real filesystem on the primary IDE disk:

```text
LBA 0..1023     reserved for stage 1, stage 2 and kernel
LBA 1024        PAPEROS1 superblock
LBA 1025-1026   directory (32 x 32-byte entries)
LBA 1027+       contiguous file data
```

`FS_BASE` moved from 400 to 1024 because the grown kernel would otherwise be
overwritten by filesystem metadata. `FS_TOTAL` is 31744 sectors. Implemented
operations are format, load, find, touch, write, read, delete, list and df.

The ATA driver uses 28-bit LBA PIO. Two timing bugs were fixed: `ata_wait(0)`
now waits for DRQ to clear after a completed transfer instead of accepting a
stale DRQ, and a short delay after issuing the command avoids sampling
pre-command status. Installer writes read each sector back and retry up to
four times. `ata_copy_sectors()` takes a `u32` count; the old `(u8)ksec` cast
truncated counts above 255 and caused missing kernel sectors.

## Installer

The installer runs in text mode from the ISO. It:

1. Probes the primary IDE disk.
2. Writes pristine stage 1, stage 2 and kernel sectors.
3. Creates PAPEROSFS at LBA 1024.
4. Verifies the boot signature and filesystem magic.

`build.py` embeds pristine boot sectors in `boot_images.h` using a two-pass
kernel build, so patched boot images for KSEC do not get copied into the
installed disk. The pristine kernel is kept at `0x1C00000`. `build.py` also
zeroes `disk.img` on every build, preventing stale superblocks at the old
LBA 400 from hiding mistakes.

Verified installer output ends with `INSTALL COMPLETE`, and `install_diff.py`
reports zero bad sectors versus `kernel.bin`.

## TTY mode

TTY mode boots the same kernel without graphics and uses COM1 plus text output.
It implements `help`, `ver`, `clear`, `about`, `echo`, `date`, `format`, `ls`,
`touch`, `rm`, `cat`, `df`, `install` and `reboot`. Files persist in PAPEROSFS;
`hello` survived a reboot during testing. `tty_puts()` writes through
`serial_putc()` character by character so COM1 output is correct.

## Networking

The NE2000 PCI driver, DHCP client and ICMP echo from v0.4 are preserved. Reset,
DHCP and ping waits use `pit_ms()` rather than busy-loop counters. v0.9 adds
real DNS over UDP and a minimal TCP stack for the browser: correct outbound TCP
checksums, SYN/SYN-ACK/ACK handshake, PSH+ACK `GET`, inbound payload
reassembly, FIN acknowledgement and timeouts. There is still no ARP, TLS,
sockets or standard library; the browser is deliberately text-only.

QEMU 11.1's `filter-dump` captures one direction per object, so `run.bat`
records `net_rx.pcap` and `net_tx.pcap`. The delivered `net.pcap` is a merged
capture with 6 packets: DHCP DISCOVER, OFFER, REQUEST and ACK, plus ICMP Echo
Request and Reply. Serial output ends with
`PAPEROS NET: IP 10.0.2.15 GW 10.0.2.2`.

## QEMU runtime

Launchers use QEMU 11.1 with 64 MiB RAM and 32 MiB VGA memory:

```text
run.bat           floppy live, monitor 4444, pcap filters
run_iso.bat       ISO live/install/TTY, monitor 4445
run_install.bat   ISO install target, monitor 4446
run_installed.bat installed IDE boot, monitor 4447
```

Serial syntax is `-serial file:...`. QEMU monitor HMP mouse commands
`mouse_move dx dy` and `mouse_button 1|0` are used by the GUI state tests.
Boot selection uses `sendkey t`, `sendkey i`, or `sendkey <resolution>`.

## Verification

All temporary `PAPEROS DBG ...` serial messages were removed before final
builds. Clean GUI serial output is:

```text
PAPEROS VIDEO 800x600 PITCH 800 NATIVE
PAPEROS NET: init
PAPEROS NET: ne2k_pci at 0xC000
PAPEROS NET: MAC 52:54:00:12:34:56
PAPEROS NET: IP 10.0.2.15 GW 10.0.2.2
```

Verified boot paths after the final code changes:

- Live floppy: GUI + DHCP/ping success serial line
- ISO live: GUI + DHCP/ping success serial line
- ISO TTY: `PAPEROS 0.9 - TTY MODE`, format/touch/ls/echo/df commands
- ISO installer: four steps OK, `INSTALL COMPLETE`
- Installed IDE disk: GUI + DHCP/ping success serial line

Final build:

```text
stage 1    : 512 bytes
stage 2    : 2048 bytes (1288 raw)
kernel     : 274304 bytes (536 sectors)
os.img     : 1474560 bytes (1440 KiB)
paperos.iso: 1523712 bytes (744 ISO sectors)
disk.img   : 16777216 bytes (install target)
```

GUI app state was checked through QEMU monitor memory reads instead of
screenshots: all twelve Start menu items, Settings controls, Calculator button
and keyboard arithmetic (`7+8=15`, `12+3*4=60`), Notepad Save/Load/New,
File Manager Open/Delete/Format/Refresh, the three new games, window stacking,
and the browser's real DNS/TCP/HTTP result.
