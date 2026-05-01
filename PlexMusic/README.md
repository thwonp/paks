# PlexMusic.pak

## What is this?

PlexMusic.pak is a music player for the TrimUI Brick that streams music from a Plex Media Server. It runs natively (SDL2, no interpreter), cross-compiled for ARM aarch64. Installed as a NextUI pak, it appears in the Apps menu.

## Features

- PIN-based Plex authentication (plex.tv/link, no password entry needed)
- Browse Music libraries → Artists → Albums → Tracks
- Streams MP3, FLAC, WAV, M4A; progressive playback (audio starts within ~2–3 s)
- Background playback — press B to return to browsing while music keeps playing
- Now Playing screen: shuffle (L2), repeat All/One/Off (R2), favorite (Y)
- Favorite Tracks list (online and offline)
- Offline mode: download albums to SD card, play without network
- Offline favorites sync: downloads favorite tracks for offline use
- Settings: stream quality (Original or Opus transcode at selectable bitrate)

## Installation

```
Roms/APPS/PlexMusic.pak/
  launch.sh
  pak.json
  res/
    font.ttf
  bin/tg5040/
    plexmusic.elf
    libmsettings.so
```

1. Copy the above directory structure to your SD card under `Roms/APPS/`.
2. The pak will appear in NextUI's Apps menu.

## Controls

**Browsing**

| Button | Action |
|---|---|
| D-pad | Navigate lists |
| A | Select / confirm |
| B | Back |
| L1 / R1 | Jump to previous/next letter or year |
| SELECT | Toggle offline mode |
| Y (on album) | Download album for offline use |
| Y (on Favorite Tracks, online) | Sync favorites for offline use |

**Now Playing**

| Button | Action |
|---|---|
| A | Play / Pause |
| B | Back to browse (music keeps playing) |
| D-pad left/right (tap) | Previous / Next track |
| D-pad left/right (hold) | Seek ±5 s |
| D-pad up/down | Seek ±30 s |
| L2 | Toggle shuffle |
| R2 | Cycle repeat (Off → All → One) |
| Y | Toggle favorite |

**Global**

| Button | Action |
|---|---|
| MENU + START | Quit |
| START (short press) | Help |
| START (long press) | Quit confirm |

## First Launch

1. Launch PlexMusic.pak from the Apps menu.
2. A 4-character PIN is displayed. On another device, go to plex.tv/link and enter it.
3. After signing in, select your Plex server.
4. Browse your music library.

Auth config is saved to SD card — subsequent launches go straight to browse.

## Log and Config Files

- **Log:** `/mnt/SDCARD/.userdata/tg5040/logs/plexmusic.txt`
- **Config:** `/mnt/SDCARD/.userdata/shared/plexmusic/config.json`
  - Contains token, server URL, stream quality setting
- **Favorites:** `/mnt/SDCARD/.userdata/shared/plexmusic/favorites.json`
- **Downloads:** `/mnt/SDCARD/.userdata/shared/plexmusic/downloads/`
- **Art cache:** `/mnt/SDCARD/.userdata/shared/plexmusic/art/`

To sign out or switch servers, delete `config.json`.

## Building from Source

Requires the `ghcr.io/loveretro/tg5040-toolchain` Docker image (run via Podman).

```bash
# Build libmsettings.so (once)
podman run --rm \
  -v /path/to/repo:/path/to/repo \
  ghcr.io/loveretro/tg5040-toolchain:latest \
  sh -c 'cd /path/to/repo/NextUI/workspace/tg5040/libmsettings && \
    ${CROSS_COMPILE}gcc -c -fpic msettings.c && \
    ${CROSS_COMPILE}gcc -shared -o libmsettings.so msettings.o -ltinyalsa -ldl -lrt && \
    cp libmsettings.so /path/to/repo/git/paks/PlexMusic/bin/tg5040/'

# Build the ELF
podman run --rm \
  -v /path/to/repo:/path/to/repo \
  ghcr.io/loveretro/tg5040-toolchain:latest \
  sh -c 'cd /path/to/repo/git/paks/PlexMusic/src && make 2>&1'
```

Output: `bin/tg5040/plexmusic.elf`
