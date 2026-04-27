# PlexMusic.pak — Session Resume Document

Last updated: 2026-04-26

---

## Project Goal

Build a native Plex music client for the **TrimUI Brick** handheld running **NextUI**. The pak is a self-contained SDL2 application (no runtime interpreter, no shell scripting) that:

1. Authenticates with Plex via PIN flow (plex.tv/link)
2. Browses Music libraries → Artists → Albums → Tracks
3. Streams and plays audio (MP3, FLAC, OGG, M4A, AAC, OPUS, WAV)
4. Saves auth config to SD card for persistent login

The pak follows NextUI's pak conventions: `launch.sh` runs `plexmusic.elf`, which is cross-compiled for ARM aarch64.

---

## Repo Layout

```
paks/PlexMusic.pak/
  bin/tg5040/
    plexmusic.elf         ← compiled binary (deploy to SD card)
    libmsettings.so       ← bundled (NOT on device's library path)
  res/
    font.ttf              ← app font (relative path, must match CWD at launch)
  src/                    ← all C source
    main.c
    module_auth.c         ← PIN flow state machine
    module_browse.c       ← library/artist/album/track browsing
    module_player.c       ← playback UI
    module_settings.c
    module_common.c       ← shared input/overlay logic
    plex_auth.c           ← plex.tv PIN API calls
    plex_api.c            ← Plex server REST API
    plex_net.c            ← HTTP/HTTPS client (mbedTLS)
    plex_config.c         ← config.json load/save
    plex_art.c            ← async cover art fetch (pthread)
    plex_queue.c
    player.c              ← audio decoding/playback (large file)
    ui_fonts.c
    ui_utils.c
    ui_icons.c
    Makefile
    include/
      mbedtls/            ← mbedTLS 3.x headers (copied from nextui-music-player)
      mbedtls_lib/        ← mbedTLS 3.x C sources
      psa/                ← PSA crypto headers (copied from nextui-music-player)
      parson/             ← JSON parser
      libogg/, libopus/, opusfile/, fdk-aac/
      mbedtls_config.h
      mbedtls_entropy_alt.c
```

---

## Build System

**Toolchain:** `ghcr.io/loveretro/tg5040-toolchain:latest` (Docker image, run via Podman on Fedora).

**Build command:**
```bash
podman run --rm \
  -v /home/thwonp/opencode:/home/thwonp/opencode \
  ghcr.io/loveretro/tg5040-toolchain:latest \
  sh -c 'cd /home/thwonp/opencode/paks/PlexMusic.pak/src && make 2>&1'
```

**Notes:**
- Mount the entire repo at the same absolute path — the Makefile uses `../../../NextUI/` relative paths that must resolve correctly inside the container
- The toolchain image sets all cross-compile env vars as Docker `ENV` — no bashrc sourcing needed
- Output binary: `paks/PlexMusic.pak/bin/tg5040/plexmusic.elf`

**libmsettings.so** must be built from source before the first build (it's NOT pre-built in the toolchain):
```bash
podman run --rm \
  -v /home/thwonp/opencode:/home/thwonp/opencode \
  ghcr.io/loveretro/tg5040-toolchain:latest \
  sh -c 'cd /home/thwonp/opencode/NextUI/workspace/tg5040/libmsettings && \
    ${CROSS_COMPILE}gcc -c -fpic msettings.c && \
    ${CROSS_COMPILE}gcc -shared -o libmsettings.so msettings.o -ltinyalsa -ldl -lrt && \
    cp libmsettings.so /home/thwonp/opencode/paks/PlexMusic.pak/bin/tg5040/'
```

---

## Deploy to Device

The device is a **TrimUI Brick** running NextUI. Files on SD card:

```
/mnt/SDCARD/
  Roms/APPS/PlexMusic.pak/   ← pak directory visible in NextUI apps menu
    launch.sh
    pak.json
    res/
      font.ttf
    bin/tg5040/
      plexmusic.elf
      libmsettings.so
```

Deploy steps (assuming device mounted or SSH access):
1. Copy `bin/tg5040/plexmusic.elf` to the pak's `bin/tg5040/` on SD card
2. Ensure `libmsettings.so` is also there (copy once; doesn't change)

**Log file location:** `$LOGS_PATH/plexmusic.txt`  
On the Brick this is `/mnt/SDCARD/.userdata/tg5040/logs/plexmusic.txt`

**Config file location:** `$SHARED_USERDATA_PATH/plexmusic/config.json`  
On the Brick: `/mnt/SDCARD/.userdata/shared/plexmusic/config.json`  
Contains: `{ "token": "...", "server_url": "...", "server_name": "...", "server_id": "..." }`

---

## Current Status

### What works (confirmed on device)
- App launches without crashing
- PIN auth screen displays a 4-character PIN with 120s countdown
- Successful auth transitions to server selection; config saved to SD card
- Browse flow: Music libraries → Artists → Albums → Tracks
- Now Playing screen — layout correct on Brick (1024×768)
- Audio playback — MP3, FLAC, WAV, M4A confirmed; OGG/AAC/Opus untested
- Track load latency reduced from ~60–90 s to ~2–3 s (progressive playback)

### No active bugs

### Remaining deferred work
- Remove `[DIAG] fprintf` logging from `main.c` and `module_browse.c` (deferred by user — leave for now)

---

## Bugs Fixed (History)

| Symptom | Root Cause | Fix |
|---|---|---|
| Black screen ~5s then crash | `Fonts_load()` not called in `main.c` | Added `Fonts_load()` after `GFX_init()`, `Fonts_unload()` in shutdown |
| SSL handshake failed `-0x7780` | mbedTLS 3.x TLS 1.3 path requires `psa_crypto_init()` | Added `plex_net_psa_init()` (once-guard), called at top of `ssl_ctx_connect()` |
| SSL still failing after PSA fix | TLS 1.3 PSA dependency too complex | Forced TLS 1.2 max via `mbedtls_ssl_conf_max_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2)` |
| 7-character PIN (wrong) | `strong=true` in POST body returns 8-char alphanumeric PIN | Changed to `strong=false` for 4-char PIN compatible with plex.tv/link |
| App auto-closes / "auth timeout" on successful auth | `check_pin` returning -1 on transient network error → AUTH_STATE_ERROR immediately | Removed `-1 → error` branch; only 120s SDL timer expires auth |
| Audio never plays | `Player_init()` not called in `main.c`; `Player_load()` checks `audio_initialized` and returns -1 | Added `Player_init()` after `plex_art_init()`, `Player_quit()` before `Fonts_unload()` in `main.c` |
| UI cut off on Brick | `COVER_SIZE = SCALE1(160) = 480px` on Brick leaves no room for text | `COVER_SIZE = is_brick ? SCALE1(100) : SCALE1(160)`; title uses `Fonts_getLarge()` on Brick |
| `[Paused]` overlapping hints bar | Fixed layout leaves only 30px gap between paused indicator and hints | Removed `[Paused]` text; hints line is now state-aware (`[A] Play` vs `[A] Pause`) |
| Temp file not cleaned up | Missing `remove(temp_path)` on Player_load failure and B-press from ERROR state | Added `remove()` in both paths in `module_player.c` |
| 60–90 s track load time | Full file downloaded before `Player_load` called; FLAC 30–50 MB at 5 Mbps WiFi | Progressive playback: `Player_load` after 512 KB buffered; stream thread retries EOF while `file_growing` is true |

---

## Key Gotchas

### Build
- **Makefile paths**: `../../../NextUI/` (3 levels up from `src/`). Mount the whole repo at same absolute path inside the container.
- **`BTN_ID_COUNT` undeclared**: `api.h` uses it but doesn't include `defines.h`. Fixed with `-include $(NEXTUI_COMMON)/defines.h` in `MY_CFLAGS`.
- **`NULL` undeclared in `album_art.c`**: Stub file was missing `#include <stddef.h>`.
- **mbedTLS headers missing**: `include/mbedtls/` and `include/psa/` directories must be copied from `paks/nextui-music-player/src/include/`.

### SSL / Network
- **TLS 1.2 max required**: mbedTLS 3.x compiled in this toolchain requires `psa_crypto_init()` for TLS 1.3. Even with PSA init, TLS 1.3 was unstable. Forcing TLS 1.2 max is the working solution.
- **`mbedtls_ssl_conf_max_tls_version` must be called BEFORE `mbedtls_ssl_setup`**: The call order in `ssl_ctx_connect` matters.
- **Transient check_pin failures**: The poll interval (2s) may elapse during a 15s SSL timeout. This is fine — the 120s SDL timer is the correct expiry mechanism.

### Auth API
- **`strong=true` vs `strong=false`**: `POST /api/v2/pins` with `strong=true` returns an 8-char code (not compatible with plex.tv/link which expects 4 chars). Always use `strong=false`.
- **Server URL selection**: `plex_auth_get_servers` picks the first connection URI from the resources list. May be plex.direct HTTPS, local HTTP, or relay URL.

### Font system
- **Font path is relative**: `ui_fonts.c` opens `"res/font.ttf"`. `launch.sh` must `cd` to the pak directory before running the ELF.
- **All font sizes must load**: Any NULL font pointer causes a segfault on the next render call.

### Platform / NextUI
- **`LEDS_applyRules called before InitSettings`**: Benign warning from the platform library. Noise, not a problem.
- **`libmsettings.so` must be bundled**: Not on device's standard library path. Goes in `bin/tg5040/` alongside the ELF; `launch.sh` sets `LD_LIBRARY_PATH`.
- **`PWR_pinToCores(CPU_CORE_PERFORMANCE)`**: Called in `main.c` to run on performance cores.

### Progressive playback
- **Early-start restricted to MP3/FLAC/WAV/M4A**: AAC, OGG, and Opus compute `total_frames` by seeking to EOF at decoder open time — on a 512 KB partial file this gives a wrong (short) value and causes premature track termination. These formats fall through to full-download behavior.
- **`file_growing` order**: `Player_setFileGrowing(false)` must be called BEFORE `Player_stop()` at all exit points, so the stream thread exits its retry loop cleanly before the join.
- **M4A moov-at-end**: If `Player_load` fails on the partial file (moov atom not yet downloaded), it falls through to full-download. Worst case is the original 60–90 s wait.

---

## Architecture Summary

```
main.c
  → plex_config_load()         load saved token+server
  → Player_init()              open SDL audio device
  → MODULE_AUTH or MODULE_BROWSE (depending on config validity)

module_auth_run()
  → plex_auth_create_pin()     POST /api/v2/pins (strong=false)
  → poll plex_auth_check_pin() every 2s for up to 120s
  → plex_auth_get_servers()    GET /api/v2/resources?includeHttps=1
  → plex_config_save()         write token+server_url to config.json
  → return MODULE_BROWSE

module_browse_run()            static state, persists across player round-trips
  → plex_api_get_libraries()   GET /library/sections  (filters type=="artist")
  → plex_api_get_artists()     paginated, up to PLEX_MAX_ITEMS=200 per page
  → plex_api_get_albums()      GET /library/metadata/{id}/children
  → plex_api_get_tracks()      GET /library/metadata/{id}/children
  → plex_queue_set()           load tracks into queue
  → return MODULE_PLAYER

module_player_run()            playback UI
  → downloads track to temp file (plex_net_download_file)
  → early-start after 512 KB buffered (MP3/FLAC/WAV/M4A only)
  → player.c                   decodes audio; stream thread retries EOF while file_growing
  → plex_api_timeline()        scrobble progress to Plex server
  → return MODULE_BROWSE

plex_net.c                     HTTP/HTTPS client
  → ssl_ctx_connect()          mbedTLS, TLS 1.2 max, VERIFY_NONE
  → redirect handling (up to 5 hops)
  → gzip decompression (zlib)

plex_art.c                     async cover art
  → pthread worker fetches art from /photo/:/transcode
  → disk cache at $SHARED_USERDATA_PATH/plexmusic/art/
```

---

## Related Projects (Reference Material)

These live under `/home/thwonp/opencode/` and were actively consulted during development.

---

### `paks/nextui-music-player/` — Primary reference implementation

The NextUI Music Player pak (by loveretro) is the closest existing SDL2 pak on this platform. PlexMusic.pak was bootstrapped by borrowing its build infrastructure, headers, and patterns.

**What we copied directly from it:**
- `src/include/mbedtls/` (74 header files) — mbedTLS 3.x headers required by mbedTLS lib sources
- `src/include/psa/` (23 header files) — PSA crypto API headers
- `src/include/mbedtls_config.h` — mbedTLS compile-time config
- `src/include/mbedtls_entropy_alt.c` — entropy source for the TrimUI platform
- `src/include/mbedtls_lib/` — all mbedTLS 3.x C source files
- `res/font.ttf` — the font used for all UI text (same font, same relative path)
- General Makefile structure (INCDIR, LDFLAGS, how to link against platform libs)

**Useful for:** understanding any NextUI API call, GFX functions, PAD input, PWR/LEDS behavior.

---

### `htpcstation/` — Sibling project (same author)

HTPC Station is a fullscreen, gamepad-first Linux interface for a home theater PC. Written in Python/QML.

**Relevance to PlexMusic.pak:** Both projects share the goal of Plex integration. HTPC Station's Plex integration code is a useful reference for API behavior, response formats, and edge cases.

**Path:** `/home/thwonp/opencode/htpcstation/`  
Key files: `main.py`, `backend/`, `qml/`
