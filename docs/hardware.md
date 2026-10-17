# AP80 Pro-X — hardware notes for the app rewrite

Verified on the device on 2026-10-16 with stock firmware 1.1 and the ap80dev hook.

## System
- Ingenic X1000 (MIPS32r2), 64 MB RAM (~40 MB free with no player running), Linux 3.10.14, glibc 2.16, busybox.
- NAND partitions (mtd): uboot 512K, logo 2.5M, kernel 6M, recovery 7M, rootfs 64M (UBIFS), data 48M (UBIFS, mounted at `/data`).
- Full flash backup: `backup/device_flash/` (nanddump + dd of every mtd, plus `data.tar`; all checksums verified).
- Build: `docker run --rm -v $PWD:/p ap80-tc mipsel-linux-gnu-gcc -march=mips32r2 -O2 -static ...`. Static binaries run fine.

## Display — `/dev/fb0` (jzfb, smart LCD)
- 360×480 portrait, 32bpp XRGB8888 (R16 G8 B0), 1440-byte stride.
- **The panel is landscape internally.** The buffer is scanned as 480-px lines running top→bottom on screen, and the first line is the RIGHT edge.
  Portrait pixel (x, y) → `fb[(359 - x) * 480 + y]`.
- The panel needs explicit refresh calls. Stock does this every frame:
  - once at start: `ioctl(fd, 0x80044620 /*SET_SLEEP*/, &0)` to wake the panel
  - each frame: `ioctl(fd, 0x80044660 /*GET_EMPTY_BUF*/, &idx)` → draw → `ioctl(fd, 0x80044661 /*START_DMA*/, 0)`
  - two buffers in the mmap (`smem_len` 1384448), **page-aligned**: buffer `idx` starts at `idx * 0xA9000`, not at `idx * 360*480*4` (0xA8C00). Physical addresses are in jzfb's table at kernel `0x805E84B8` (0x2200000, 0x22A9000).
  - to sleep: SET_SLEEP with 1 — **don't use it**: suspending with a frame in flight leaves that buffer marked busy, and a later GET_EMPTY_BUF spins in the kernel forever while holding the fb mutex (unkillable, needs a reset). Use backlight 0 instead. Backlight: `/sys/class/backlight/pwm-backlight.0/brightness` (0–255).

## Input (evdev)
| Device | Node | Codes |
|---|---|---|
| gpio-keys | event5 | 116 POWER (also the knob press?), 164 PLAYPAUSE, 165 = physical **Next**, 163 = physical **Previous** (labels swapped) |
| sa-ring-keys (volume knob) | event0 | 106 RIGHT = up, 105 LEFT = down |
| GT9XX-TS touch | event6 | MT protocol B, ABS_MT_POSITION_X/Y already in portrait screen coordinates (0–359, 0–479) |
| earpods_wire (in-line remote) | event1 | not tested |
| kx126 accel / step detector / step counter | event2–4 | |
- Headset jack: `/sys/class/switch/headset/state`.

## Accelerometer — kx126 on i2c-2 @ 0x1e
- `/sys/class/input/input2/{enable,delay}` switch it on, but **event2 delivers nothing useful**: the
  data-ready interrupt (GPIO 54) never fires, and the polled path (delay = 5 ms) only reports when X
  crosses a threshold. `orientation_status` is just that X-flip flag.
- The chip works (WHO_AM_I 0x11 = 0x38). Read registers through the driver's debug hook:
  `echo "func kx126_sysfs_reg_read +p" > /sys/kernel/debug/dynamic_debug/control`, then
  `echo 0x09 > /sys/class/input/input2/reg_read` logs `kx126_sysfs_reg_read reg 0x9 val 0xe2`.
- XOUT/YOUT/ZOUT = 0x08..0x0D (16-bit LE, ±4 g, ~8192 per g; high bytes ~32 per g).
- Axes: held upright, gravity reads chip −X. Turned with the top edge to the left (left edge down),
  it reads chip −Y. So screen +x (toward the right edge) = chip +Y and screen +y (down) = chip −X.

## Audio — ALSA card 0 "ap80pro" (ES9218/9219 driver)
- PCM `hw:0,0`: S16_LE / S24_LE, 8 k–384 kHz, 1–2 channels.
- Mixer controls: `Left/Right Playback Volume` (0–255), `Digital Filter` (0–7), `Hardware Mute`, `DOP_EN`, `isDSD`, `Output Port Switch` (0–6).
- `Output Port Switch`: **5 = off** (AMP_PWR regulator disabled; playback fails with a FIFO-underrun I/O error). **0 = 3.5 mm** (verified by ear). 1–4 and 6 play without errors but weren't heard on 3.5 mm (presumably 4.4 mm balanced / line out / gain variants — still to map).
- **Volume = `Left/Right Playback Volume`. It's an ATTENUATION scale: 0 = LOUDEST (0 dB), 255 = mute** (ES92xx, about 0.5 dB per step).
  Seen with stock running: UI 0 → 255, UI 1 → 188, then 178, 168, 158, 154, 150 … (a non-linear table). The app must clamp volume and start quiet, and must never write 0 by default.
- Stock uses **Output Port 2 for 3.5 mm** during playback. Port 0 was also audible on 3.5 mm in testing. The port meanings (balanced, gain, line out) are not mapped yet.
- Stock state lives in `/data/user.ini` (binary), with `eq.ini`, `usrlocal_media.db` (the SQLite library) and `lastused.txt` alongside.
- Card 1 "sa_spdif": SPDIF dummy.
- Battery: `/sys/class/power_supply/battery/{capacity,status,voltage_now}`.

## Bluetooth — CSR8811 on ttyS0, BlueZ 4
- `S40bt_init` → `/usr/bin/bt_init` starts dbus, `hciattach`, `bluetoothd` (BlueZ 4, HiBy-patched), `bt-agent`, `bt-monitor` and `bt-media` at every boot, and leaves the adapter Powered Off.
- Pairings persist in `/data/lib/bluetooth/<adapter MAC>/` (linkkeys).
- Tools: `bt-adapter --set Powered 1|0`, `bt-adapter -i`, `bt-device -l` (JSON after some uuid lines), `bt-device -c MAC` (pair), `-d` (disconnect), `-r` (remove), `--set MAC Trusted 1`, `hcitool scan`, `hcitool con` (cheap way to see live ACL links).
- Audio: ALSA `pcm.NAME { type bluetooth bdaddr MAC profile a2dp }` (`libasound_module_pcm_bluetooth.so`). Opening it connects A2DP by itself if the device is reachable; the codec (LDAC/aptX/AAC/SBC) comes from `/etc/bluetooth/audio.conf`. It accepts only **S16_LE**, any rate up to 192 kHz; stream start takes 1–2 s. `BT_GET_CAPABILITIES failed: Input/output error` = device off, out of range or busy with another source.
- ap80player writes an `.asoundrc` into a per-device HOME under `/tmp` and pipes 16-bit PCM to `aplay -D ap80bt -t raw` (software volume). `bt-monitor` reports events over HiBy's IPC, not stdout, so connection state is polled with `hcitool con`.

## Stock player behaviour
- `strace` of `hiby_player` stalls during the library scan (it was writing to the SD card), so a future trace should log to `/tmp` and trace fewer syscalls.
- Starting stock from adb needs `start-stop-daemon -S -b`; otherwise it gets a SIGHUP when the adb shell exits.
