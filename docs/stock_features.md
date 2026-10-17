# AP80 Pro-X stock firmware 1.1 — feature inventory

Taken from the stock firmware's UI layouts (`usr/resource/layout/theme1`), its English strings (`usr/resource/str/english/*.ini`) and the `hiby_player` binary.
Mark each line **K** (keep), **S** (strip) or **L** (later).

Hardware: 360×480 touch screen, volume knob, physical buttons, Ingenic X1000, 2× ES9219C DAC + HBC3000 FPGA ("hm100"), Bluetooth (CSR8811), pedometer sensor, vibrator, status LED.

## Library / browsing
- [ ] Categories: All songs, Albums, Artists, Genres, Files (folder browser), Format
- [ ] A–Z quick-scroll index bar
- [ ] Search
- [ ] Media database (SQLite): manual or auto update, CUE sheets, ReplayGain tags, cover art
- [ ] Album cover lists (`/list_cover`), custom covers
- [ ] My Favorites ("My Collection")
- [ ] Playlists: create, delete, add/remove tracks, load/save as .m3u / .hiby
- [ ] History / recently played
- [ ] Track info / properties (format, sample rate, bit depth, bitrate, channels, size, path)
- [ ] Delete file / folder from the device
- [ ] Lyrics (.lrc)

## Playback
- [ ] Now playing screen: cover, progress/seek, track info, now-playing list
- [ ] Play menu: add to playlist, EQ, view album, properties, list now playing
- [ ] Play modes: loop single, loop list, play through list, shuffle; play through folders / albums
- [ ] Resume from last (track / position)
- [ ] Gapless playback
- [ ] Crossfade
- [ ] Gain (low/high)
- [ ] Balance L/R
- [ ] ReplayGain (off / track / album)
- [ ] Max volume, volume at power-on (memory / fixed)
- [ ] Lock volume knob
- [ ] Formats: FLAC, APE, WAV, AIFF, ALAC/AAC (FDK), MP3, OGG, Opus, WMA, DSD (DSF/DFF, DoP, native, DSD→PCM), CUE

## Sound
- [ ] Graphic EQ: presets (Rock, Classic, Jazz, Pop, Dance, Voice, Blues, Metal) + Custom
- [ ] MSEB (HiBy's tuning sliders: temperature, bass, vocals, sibilance, air...) + presets
- [ ] DAC digital filter (8 ES9219 filters: linear/minimum/apodizing fast/slow, brick wall)
- [ ] DSD output mode (PCM / DoP / Native), DSD gain compensation
- [ ] Soundfield
- [ ] Output selection / Line out mode (fixed volume)
- [ ] SPDIF output

## Bluetooth (transmitter)
- [ ] Scan, pair, connect, unpair headphones
- [ ] Codecs: LDAC (quality modes), aptX, AAC, SBC, UAT (600K/900K/1.2M/TWS)
- [ ] Bluetooth volume control, HiBy Link (phone remote)
- [ ] Device name

## USB
- [ ] USB mode: Storage (mass storage), Audio (USB DAC, UAC2 with DSD), Dock
- [ ] USB charge current limit
- [ ] OTG / USB audio output to an external DAC

## Extra apps
- [ ] FM radio (scan, presets, sensitivity) — not supported: no FM tuner on I2C and no driver on this hardware
- [ ] Pedometer / step counter (sensitivity, date, reset)
- [ ] E-book reader (`/book`: bookmarks, favorites, recents, page turn by key)
- [ ] Gallery / screensaver (album cover, dynamic cover, photos in `/screensavers`)
- [ ] Themes / UI theme / color theme

## System
- [ ] Brightness, backlight timeout, font size, language
- [ ] Clock: 12/24h, set date/time, clock display
- [ ] Idle shutdown / sleep timer
- [ ] Button behavior with the screen off, in-line remote support
- [ ] Battery percentage display, status LED, charging screen
- [ ] Format microSD, restore factory settings, firmware update, about / licence
- [ ] Pull-down quick menu (status bar toggles)
