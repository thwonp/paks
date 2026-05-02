# PlexMusic.pak

**NOTE: I highly recommend updating [stock firmware](https://trimui.com/pages/trimui-brick-firmware) to the latest update. I noticed greatly improved wifi stability on v1.1.1**

Plex Pass is required for transcoding (which is set by default). If you do not have Plex Pass, you can change the quality presets to "Original" in Settings.

## What is this?

PlexMusic.pak is a music player for the TrimUI Brick that streams music from a Plex Media Server. It runs natively (SDL3, no interpreter), cross-compiled for ARM aarch64. Installed as a NextUI pak, it appears in the Apps->Tools menu.

## Features

**Browsing**
- Browse by Artists → Albums → Tracks or jump straight to a flat Albums view sorted by release year
- Album art displayed alongside every list
- Animated loading screen with instant B-button cancel on any in-progress load

**Playback**
- Streams MP3, FLAC, WAV, M4A, AAC, OGG, and Opus
- Set transcode quality for Opus conversions
- Progressive playback — audio starts within ~2–3 s; no waiting for the full track to download
- Scrobbles plays back to your Plex server

**Offline**
- Full offline mode: browse and play downloaded albums and favorites list without a network connection
- Set download quality targets (original or Opus transcode)
- Offline Artists and Albums views mirror the online experience
- Warning prompt if you try to switch to offline while a download is in progress

**Favorites**
- Press Y on any track (online or offline) to toggle it as a favorite — a ♥ appears in the track listing
- "Favorite Tracks" on the home menu shows all favorites; A queues the whole list, Y removes a track
- Press Y on "Favorite Tracks" in online mode to sync: missing tracks are downloaded, removed favorites are deleted from local storage
- Offline edits (add/remove favorites) are automatically picked up by the next online sync — no extra step needed

**Auth & setup**
- PIN-based Plex authentication (plex.tv/link, no password entry needed)
- Supports multiple Plex servers — pick one on first launch
- Auth and settings saved to SD card; no re-login needed after reboot

**Quality of life**
- Background playback — press B from Now Playing to return to browse while audio keeps playing
- Now Playing screen: shuffle (L2), repeat All/One/Off (R2), favorite (Y)
- D-pad left/right: tap = skip track, hold = seek ±5 s
- Queue auto-advances through an album during background playback
- Configurable screen sleep timeout
- MENU+SELECT wakes the screen from sleep when pocket lockout is enabled
- MENU+START quits from anywhere instantly
- Settings: switch server, sign out, change library, seek interval, transcode options

## Installation

```
Tools/tg5040/PlexMusic.pak/
  launch.sh
  pak.json
  res/
    font.ttf
  bin/tg5040/
    plexmusic.elf
    libmsettings.so
```

1. Copy the above directory structure to your SD card under `Tools/tg5040/`.
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

## Disclaimer

This code was written with good vibes only (i.e Claude). I'm not a fullstack dev, just a person who wanted to listen to Plex on his TrimUI Brick.
