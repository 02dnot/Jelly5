<p align="center">
  <img src="docs/media/banner.jpg" alt="Jelly5 — Jellyfin for PlayStation 5" width="100%">
</p>

<p align="center">
  <img src="docs/media/icon.png" alt="" width="96"><br>
  <b>A native Jellyfin client for jailbroken PS5 consoles.</b><br>
  Its own GPU-drawn interface in the spirit of Netflix and Apple TV, hardware-decoded 4K HDR playback, and no browser in between.
</p>

<p align="center">
  <img alt="Platform: PS5" src="https://img.shields.io/badge/platform-PS5%20(jailbroken)-4b5bdc">
  <img alt="Jellyfin" src="https://img.shields.io/badge/Jellyfin-client-7b5cd6">
  <img alt="License: GPL-3.0-or-later" src="https://img.shields.io/badge/license-GPL--3.0--or--later-555">
  <img alt="Status: early" src="https://img.shields.io/badge/status-early%20release-c06">
</p>

---

## What it is

Jelly5 is an app that appears on the PS5 home screen under **Media**, next to
Netflix and friends. Everything you see is drawn directly on the GPU at the
panel's own resolution: large backdrops, rows that slide and lift, blur and
glass, artwork that fades in over its BlurHash. Video goes through the console's
own hardware decoder.

It speaks only Jellyfin. There is no extra backend and no account other than
your Jellyfin server.

## Features

**An interface made for the TV**
- Liquid glass throughout: controls on frosted, light-bending glass, and one springy glass drop that marks the focus wherever you are
- Home with a hero, *Continue watching*, *Next up*, *Recently added* per library, recommendations and genres, in the order you set in Jellyfin
- Detail pages with logo art, cast, seasons and episodes, trailers, extras and *More like this*
- Libraries for movies, shows and music: sort, filter (unwatched, favourites, genre, decade) and jump A–Å by letter
- Search across movies, shows, episodes, music and people
- Several users and servers with profile pictures, and a screensaver drawn from your own backdrops
- Norwegian and English, following the PS5's system language

**Signing in**
- Finds Jellyfin servers on your network by itself
- **Quick Connect** by default: scan the QR code with your phone, tap *Authorize*, done
- Or user name and password with the PS5's system keyboard

**Playback**
- Hardware decoding of H.264 and HEVC up to 4K, HDR10 and HLG (Dolby Vision plays its HDR10 base layer)
- Direct play wherever the PS5 can, server transcoding where it can't, through a PS5 device profile
- Audio and subtitle tracks (SRT, ASS/SSA, PGS and more), subtitle styling and online subtitle search
- Trickplay thumbnails, a chapter menu, *Skip intro* from Jellyfin's media segments, auto-play of the next episode
- Choice between versions when a title has several
- An audio delay setting for soundbars and receivers
- Playback info on L3: how the server serves it, codecs, decoder, bitrate and buffer
- Keeps going through network hiccups: a stream that breaks off resumes where it stopped
- Progress, resume and watched state synced with Jellyfin; mark whole seasons or series as watched

**Music**
- Albums, artists, playlists and Instant Mix
- Background playback while you browse, a mini player and a full *Now playing* view with time-synced lyrics (word by word when the lyrics have it)

**PS5 touches**
- Adaptive triggers: L2/R2 scrub against a resistance, faster the harder you press
- The DualSense light bar takes the colour of what is playing
- A 120 Hz interface on displays that support it, and its own background on the PS5 home screen
- *Watch together* (Jellyfin SyncPlay) and remote control from other Jellyfin apps

## Formats

| | Plays on the PS5 | Notes |
| --- | --- | --- |
| Video | H.264, HEVC (Main, Main 10), VP9 and older formats | AV1 is transcoded by the server |
| HDR | HDR10, HLG, HDR10+ (as HDR10), Dolby Vision (its HDR10 base layer) | Dolby Vision profile 5 is transcoded |
| Audio | AAC, AC3, E-AC3, TrueHD, DTS (incl. DTS-HD MA), FLAC, Opus, MP3 and more | Decoded to multichannel PCM |
| Subtitles | SRT, ASS/SSA, PGS, DVD and DVB subtitles, WebVTT | Embedded or external |
| Containers | MKV, MP4, TS/M2TS, AVI and more | Blu-ray folders and ISO files are not supported |
| 3D | — | Side-by-side and top-and-bottom files are refused; 3D Blu-ray (MVC) plays in 2D |

## Requirements

- A jailbroken PS5 with **ShadowMount+** and an FTP server (for example ftpsrv or etaHEN's). Tested on firmware **11.60**; other firmware with ShadowMount+ should work but is untested.
- A Jellyfin server (developed and tested against 12.1) on the same network or reachable from the console.

## Install

1. Download `Jelly5-<version>.zip` from the [latest release](../../releases/latest) and unzip it.
2. Over FTP, copy the `PPSA99505` folder to `/data/homebrew/` on the console,
   so that you have `/data/homebrew/PPSA99505/eboot.bin`.
3. ShadowMount+ mounts it and adds the **Jelly5** tile under Media. If the tile
   doesn't show up, rerun your payloads or reboot and jailbreak again.
4. Open Jelly5. It looks for Jellyfin servers on your network; pick yours, or
   type its address, and sign in with Quick Connect.

The zip also has `PPSA99505.ffpfsc`, a PFS image of the same app for loaders
that mount images. The folder route above is the tested one.

**Updating:** close Jelly5 completely first (PS button → close the app), then
overwrite the files in `/data/homebrew/PPSA99505/`. Don't delete the folder and
copy a new one, and never keep a second folder with the same title ID anywhere
under `/data/homebrew`: ShadowMount+ bind-mounts the folder, and replacing it
breaks the mount. Replacing files while the app is running can crash the
console.

## Controls

| Button | In the menus | In the player |
| --- | --- | --- |
| ✕ | Select | Play / pause, select |
| ○ | Back one level at a time | Hide the controls, then leave the player |
| D-pad | Move | Show the controls; left/right seek in 10 s steps |
| L1 / R1 | Previous / next tab | Previous / next chapter |
| L2 / R2 | Previous / next letter (libraries sorted A–Å) | Rewind / fast forward, faster the harder you press |
| △ | Search | Episodes (a film: its chapters) |
| □ | Sort & filter (libraries) | Audio and subtitles |
| Options | Options for the selected title (watched, favourite, …) | The controls |
| Touchpad | *Now playing*, while music plays | The controls |
| L3 | | Playback info |

## Known limits

These come from the platform, not from Jelly5:

- No bitstream passthrough: Dolby Atmos and DTS:X play as 7.1 PCM.
- No true 24p output: the console runs the display at 60 or 120 Hz.
- Dolby Vision plays its HDR10 base layer. Profile 5, which has none, is transcoded by the server.
- AV1 is transcoded by the server for now.
- No 3D output, so side-by-side and top-and-bottom 3D files are not played.
- The app can't quit itself. Close it with the PS button.

## Privacy

Jelly5 talks to your Jellyfin server and nothing else. It keeps its accounts,
settings and a 96 MB image cache in its own folder, `/download0/jelly5`, and
writes nowhere else on the console. Release builds send no logs anywhere.

## Reporting problems

Open an issue with your firmware, your Jellyfin version, what you did and what
happened. For playback problems, the L3 playback info for the title helps a lot.

## Building

Jelly5 builds natively on macOS (Apple silicon) with the
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) and pacbrew.

```sh
scripts/setup-toolchain.sh                  # once: SDK, pacbrew sysroot, host tools
cd app
eval "$(../scripts/setup-toolchain.sh --env)"
"$(brew --prefix)/bin/bash" scripts/build.sh --release   # → build/app/Jelly5-<version>.zip
```

A plain `scripts/build.sh` is the development build. It reads the git-ignored
`.env.local` (`JF_URL`, `PS5_HOST`, …), uses `JF_URL` as the default server,
and sends a debug log over UDP to your Mac (`scripts/log.sh`). `scripts/deploy.py`
uploads it to the console over FTP. `--release` leaves all of that out.

The project's layout and its hard-won rules are in [`CLAUDE.md`](CLAUDE.md), and
the decisions and design plan are in [`PLAN.md`](PLAN.md). `concept/` holds the
interactive HTML prototype the interface is built from.

## Credits

Jelly5 stands on the work of others in the PS5 scene and beyond:

- **[Nuvio PS5](https://github.com/theghostonline/Nuvio-PS5)** by Husam Osman: the native player, its session and subtitle handling, and the app packaging toolkit
- **[EVO Player](https://github.com/sainsaji/EVO-PLAYER-PS5)**: the media engine (demuxing, `sceVideodec2`, the AGC renderer, audio, HDR), shader pipeline tooling and much of what is known about the PS5's GPU from user space
- **[ps5-payload-dev SDK](https://github.com/ps5-payload-dev/sdk)** by John Törnblom and contributors, plus the pacbrew sysroot
- **[Switchfin](https://github.com/dragonflylee/switchfin)**, used as a reference for the Jellyfin API
- **[OverShifted/LiquidGlass](https://github.com/OverShifted/LiquidGlass)** (MIT), whose refraction profile the glass shader follows
- [FFmpeg](https://ffmpeg.org), [libass](https://github.com/libass/libass), FreeType, HarfBuzz, [cJSON](https://github.com/DaveGamble/cJSON), [NanoSVG](https://github.com/memononen/nanosvg), [QR Code generator](https://www.nayuki.io/page/qr-code-generator-library) by Project Nayuki, and the Inter, Roboto and Noto typefaces
- The [Jellyfin](https://jellyfin.org) project

Third-party licences are listed in
[`toolkit/NUVIO_THIRD_PARTY_NOTICES.md`](toolkit/NUVIO_THIRD_PARTY_NOTICES.md) and
next to the bundled fonts in `app/assets/fonts/`.

## License

Jelly5 is free software under the **GNU General Public License v3.0 or later**
(see [`LICENSE`](LICENSE)). Code taken from Nuvio PS5, EVO Player and other
projects keeps its original notices.

Jelly5 is not affiliated with Sony Interactive Entertainment or the Jellyfin
project. *PlayStation* and *PS5* are trademarks of Sony Interactive
Entertainment. Use it only with media you are entitled to play.
