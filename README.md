# AP80 Pro-X custom player

A small replacement UI for the **Hidizs AP80 Pro-X** portable music player. It replaces the stock
HiBy player app with a simple one and keeps HiBy's Linux and drivers.

The goal is a minimal player. Plug in headphones, browse the SD card by folder, and play a song or
a whole folder. There is no Bluetooth, e-book reader, step counter or album art.

> **Warning:** this project involves flashing modified firmware and running unofficial software
> on the device. It is not affiliated with or endorsed by Hidizs or HiBy. Doing this may void your
> warranty, and it can brick the device or lose data on the SD card. You do it at your own risk.
> Make a full NAND backup first.

## What it does
- **Library:** a folder browser for the SD card. Tap a song to play it and the rest of its folder;
  "Play all" plays the folder recursively.
- **Player:** title, artist and album from FLAC/MP3/WAV tags, plus format, a seek bar,
  prev/play/next, and links to the library and settings.
- **Resume:** on boot it opens the player paused at the last position, or the library if nothing
  was played before.
- **Hardware controls:** play/pause, next/prev, the volume knob, and power (short press = screen,
  long press = power off).
- **Settings:** brightness, screen-off timeout, DAC filter, max volume, start paused or resume,
  switch to the stock player, power off.
- **USB storage mode:** exports the SD card to a computer so music can be copied over.

## How it runs on the device
The device is an Ingenic X1000 (MIPS32r2) with 64 MB RAM, Linux 3.10 and a 360×480 touch screen.

1. The device is flashed with stock firmware 1.1, modified in one place: the
   `S92_03_start_music_player` init script (`firmware/mod/overlay`).
2. That init hook runs `ap80dev/boot.sh` from the SD card. If `ap80dev/no_stock` exists, the
   stock player is skipped.
3. `boot.sh` (`sdcard/ap80dev/boot.sh`) keeps ADB on and runs `ap80dev/player/ap80player` in a
   loop. If the app crashes repeatedly, it falls back to the stock player.

The stock firmware 1.1 image (`.upt`) is needed to rebuild the patched firmware, but it isn't
included in this repo.

## Layout
| Path | Contents |
|---|---|
| `app/player` | The player: C, framebuffer UI, tinyalsa output, dr_flac/dr_mp3/dr_wav decoders, stb_truetype text |
| `app/fbtest`, `app/hello` | Early hardware experiments |
| `sdcard/ap80dev` | Files that go on the SD card (`boot.sh`, flag files) |
| `firmware/mod/overlay` | The one file changed in the stock rootfs |
| `tools/Dockerfile` | Cross toolchain image `ap80-tc` (Debian, mipsel gcc, UBI/ISO tools, qemu) |
| `tools/scripts` | Firmware unpack/repack (`roundtrip.sh`, `build_upt.sh`, `verify.sh`) and qemu helpers |
| `tools/deploy.sh`, `tools/shot.sh` | Build + push + restart over ADB; take a screenshot |
| `docs/hardware.md` | Hardware notes: display ioctls, inputs, ALSA controls, USB gadget, pitfalls |
| `docs/stock_features.md` | Inventory of stock firmware features |
| `screenshots` | UI screenshots |

## Build and deploy
```sh
git submodule update --init                      # tinyalsa
docker build -t ap80-tc tools                      # once
tools/deploy.sh                                     # build, push to the SD card over ADB, restart the app
tools/shot.sh player                                # saves screenshots/player.png
```
Running the app needs the patched firmware and the `ap80dev/adb` and `ap80dev/no_stock` flag
files on the card.

### Debug FIFO
While the app runs it reads commands from `/tmp/ap80p.cmd` on the device:
- `tap x y`
- `drag x1 y1 x2 y2`
- `key play|next|prev|up|down|power`
- `shot PATH`
- `restart`
- `quit`

## Things to know
- Volume is an attenuation scale: 0 is loudest and 255 is mute.
- Don't put the LCD controller to sleep (`SET_SLEEP` 1): it can hang the display driver. The app
  only turns the backlight off.
- Avoid hard resets while the SD card is mounted. One unclean reset wiped the `ap80dev` folder.
- To return to stock, remove `ap80dev/no_stock`, or flash the stock or rollback `.upt`.

## License
The code in this repo is MIT-licensed; see `LICENSE`. Third-party components keep their own
licenses:
- dr_flac, dr_mp3 and dr_wav: public domain or MIT-0
- stb_truetype: public domain or MIT
- tinyalsa: BSD-3-Clause; a git submodule
- DejaVu fonts: the Bitstream Vera / DejaVu license

Hidizs and HiBy firmware isn't included.
