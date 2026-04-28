# paks

Homebrew paks for the **TrimUI Brick** running [NextUI](https://github.com/LoveRetro/NextUI).

---

## PlexMusic.pak

A native Plex music client for NextUI. Browse your Plex music library, stream tracks over your local network, and download albums for offline listening.

### Features

**Browsing**
- Browse by **Artists → Albums → Tracks** or jump straight to a flat **Albums** view sorted by release year
- Album art displayed alongside every list
- Animated loading screen with instant B-button cancel on any in-progress load

**Playback**
- Streams MP3, FLAC, WAV, M4A, AAC, OGG, and Opus
- Set transcode quality for Opus conversions.
- Audio starts after a small buffer — no waiting for the full track to download
- - Note: Only when Original quality is set - WIP on getting this working for transcodes.
- Scrobbles plays back to your Plex server


**Offline**
- Download albums from the browse screen (Y button on any album)
- Set download quality targets (original or Opus transcode)
- Full offline mode: browse and play downloaded albums without a network connection
- Offline Artists and Albums views mirror the online experience

**Auth & setup**
- Sign in once via the standard Plex PIN flow (plex.tv/link)
- Supports multiple Plex servers — pick one on first launch
- Auth and settings saved to SD card; no re-login needed after reboot

**Quality of life**
- Configurable screen sleep timeout
- MENU+SELECT wakes the screen from sleep
- MENU+START quits from anywhere instantly
- Settings: switch server, sign out, change library, adjust screen timeout, transcode options

**Planned Features**
- Bug hunting + more testing
- Playlists
- Custom sort options
- Other QoL stuff and features as I think of them (or as requested)

**Disclaimer**
This code was written with good vibes only (i.e Claude). I'm not a fullstack dev, just a person who wanted to listen to Plex on his TrimUI Brick.
