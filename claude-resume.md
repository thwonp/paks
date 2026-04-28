# PlexMusic.pak — Session Resume Document

Last updated: 2026-04-27 (session 2)

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

Repo lives at `git/paks/` (remote: `git@github.com:thwonp/paks.git`).

```
git/paks/
  claude-resume.md
  PlexMusic/                ← becomes PlexMusic.pak on device
    bin/tg5040/             ← gitignored
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
  sh -c 'cd /home/thwonp/opencode/git/paks/PlexMusic/src && make 2>&1'
```

**Notes:**
- Mount the entire opencode repo at the same absolute path — the Makefile uses `../../../../NextUI/` relative paths that must resolve correctly inside the container
- The toolchain image sets all cross-compile env vars as Docker `ENV` — no bashrc sourcing needed
- Output binary: `git/paks/PlexMusic/bin/tg5040/plexmusic.elf`
- Three pre-existing build warnings (not errors): `PADDING` redefined in `module_player.c`, `MAX` in `parson/parson.c`, `strncpy` in NextUI's `generic_wifi.c` — ignore

**libmsettings.so** must be built from source before the first build (it's NOT pre-built in the toolchain):
```bash
podman run --rm \
  -v /home/thwonp/opencode:/home/thwonp/opencode \
  ghcr.io/loveretro/tg5040-toolchain:latest \
  sh -c 'cd /home/thwonp/opencode/NextUI/workspace/tg5040/libmsettings && \
    ${CROSS_COMPILE}gcc -c -fpic msettings.c && \
    ${CROSS_COMPILE}gcc -shared -o libmsettings.so msettings.o -ltinyalsa -ldl -lrt && \
    cp libmsettings.so /home/thwonp/opencode/git/paks/PlexMusic/bin/tg5040/'
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
Contains: `{ "token": "...", "server_url": "...", "server_name": "...", "server_id": "...", "screen_timeout": 30, "library_id": 12345, "library_name": "Music" }`

---

## Current Status

### What works (confirmed on device)
- App launches without crashing
- PIN auth screen displays a 4-character PIN with 120s countdown
- Successful auth transitions to server selection; config saved to SD card
- Browse flow: Home menu → Artists → Albums → Tracks
- Home screen: static menu [Artists / Now Playing (conditional) / Settings]
- First launch: library picker shown; auto-selects if only one music library; saves to config
- Library selection persisted to config.json; accessible via Settings → Library
- Now Playing screen — layout correct on Brick (1024×768)
- Audio playback — MP3, FLAC, WAV, M4A confirmed; OGG/AAC/Opus untested
- Track load latency ~2–3 s (progressive playback)
- Browse data loads are fully async — animated loading screen, no frozen UI
- B-press during album/track/artist load cancels and returns to parent immediately
- Scrobble and timeline calls are fire-and-forget — no stall during playback
- B from home screen shows quit confirmation dialog; A confirms, B cancels
- B from Artists in offline mode shows quit confirmation dialog (same as online)
- MENU+START from any screen quits immediately (no confirmation)
- Screen auto-sleep: configurable timeout (Off/15s/30s/1min/2min/5min) in Settings; MENU+SELECT wakes
- Screen timeout setting persisted to config.json
- Startup splash rendered before relay URL probe (no more black screen on launch)
- Right sidebar (art panel) shown only on Albums screen; all other browse screens full-width
- Artist thumbnail fetches suppressed (artist art has no consumer without the sidebar)

### Active bugs / under investigation
- **Offline album click crash** — app crashes when selecting an album in offline mode. Root cause unknown; diagnostic logging added. Next reproduction will capture the crash site.

### Deferred / known noise
- Remove `[DIAG] fprintf` logging from `main.c` and `module_browse.c` (deferred by user)

---

## Key Gotchas

### Build
- **Makefile paths**: `../../../NextUI/` (3 levels up from `src/`). Mount the whole repo at same absolute path inside the container.
- **`BTN_ID_COUNT` undeclared**: `api.h` uses it but doesn't include `defines.h`. Fixed with `-include $(NEXTUI_COMMON)/defines.h` in `MY_CFLAGS`.
- **mbedTLS headers missing**: `include/mbedtls/` and `include/psa/` directories must be copied from `paks/nextui-music-player/src/include/`.

### SSL / Network
- **TLS 1.2 max required**: forcing TLS 1.2 via `mbedtls_ssl_conf_max_tls_version` is the working solution. TLS 1.3 was unstable even with PSA init.
- **`mbedtls_ssl_conf_max_tls_version` must be called BEFORE `mbedtls_ssl_setup`**.

### Auth API
- **`strong=false`**: `POST /api/v2/pins` with `strong=true` returns an 8-char code (plex.tv/link expects 4 chars). Always use `strong=false`.

### Font system
- **Font path is relative**: `launch.sh` must `cd` to the pak directory before running the ELF.

### Platform / NextUI
- **`libmsettings.so` must be bundled**: Not on device's standard library path.
- **`PWR_pinToCores(CPU_CORE_PERFORMANCE)`**: Called in `main.c` to run on performance cores.

### Screen sleep (`module_player.c`)
- **Use `PLAT_enableBacklight(1)` to restore backlight**, NOT `SetBrightness(GetBrightness())`. On the Brick, `PLAT_enableBacklight(1)` calls `SetRawBrightness(8)` first as a priming step before `SetBrightness`; without this, the backlight silently stays off.
- **`extern void PLAT_enableBacklight(int enable);`** — declare it in the file. The function is defined in `platform.c` which is compiled directly into the binary via `NEXTUI_PLATFORM_SRC` in the Makefile — no additional linking needed.
- **`ModuleCommon_handleGlobalInput` must still be called while sleeping** — it runs `PWR_update` which handles the power button and platform power events. Skipping it causes the app to appear crashed after device sleep/wake via the power button.

### State machine / render loop pattern (module_browse.c, module_settings.c)
- **Every `state = X; dirty = 1;` in an input block MUST be followed by `GFX_sync(); continue;`**. Without it, the current state's render block fires on the same frame, resets `dirty = 0`, and the next state appears frozen until the user presses a button.
- **Dialog flags set in the input block must be handled in the SAME render block** (not in a separate pre-input check block). If a flag like `quit_confirm_active` is set in the input section but checked in a block that runs before the input section, it's invisible on the frame it's set — the dialog won't appear until the next frame, causing a one-frame freeze that looks like input is needed to trigger it.
- **Secondary button hints must use `align_right=0`** (left side). `render_browse_screen` already renders A/B hints right-aligned; a second `GFX_blitButtonGroup` call with `align_right=1` overlaps them. Use `align_right=0` for any additional hint (Y/DOWNLOAD, SELECT/OFFLINE, etc.).

### Progressive playback
- **Early-start restricted to MP3/FLAC/WAV/M4A**: AAC, OGG, Opus compute `total_frames` by seeking to EOF — wrong value on a partial file → premature track end.
- **`Player_setFileGrowing(false)` before `Player_stop()`** at all exit points.
- **M4A moov-at-end**: falls through to full-download if moov not in first 512 KB.

### Home screen / library picker (`module_browse.c`)
- **`BROWSE_LIBRARIES`** is now a static home menu — it does NOT fetch libraries. Do not add async load logic there.
- **`BROWSE_LIBRARY_PICKER`** contains the async library fetch. It is entered on first launch (`cfg->library_id == 0`) or when `module_browse_request_library_pick()` has been called (e.g. from Settings).
- **`s_library_pick_requested` flag** must be checked OUTSIDE the `!s_browse_initialized` block so it fires on every call to `module_browse_run()`, not just the first.
- **Sidebar**: right art panel only renders for `BROWSE_ALBUMS`. All other states pass `show_panel=0` to `render_browse_screen`. Artist thumbnail fetches (`plex_art_fetch` with `artists[...].thumb`) are fully removed.
- **Startup black screen**: `SDL_FillRect` + `GFX_flip` is called before each `apply_relay_fallback()` in `main.c` so the screen is dark (not black/unrendered) during the blocking DNS probe.

### Browse async workers (`module_browse.c`)
- **Single shared worker context `s_load`**: Only one load in flight at a time.
- **Soft cancel**: `s_load.cancel = true`; old thread joined on next `browse_load_kick()`.
- **B-cancel for BROWSE_ARTISTS**: Does NOT reset `libs_ls` — library data stays valid to avoid triggering a re-kick on the next frame.

### Scrobble fire-and-forget (`module_player.c`)
- **`fire_scrobble()` helper**: Heap-allocates ctx, spawns detached pthread. Five call sites: periodic (every 5s), 90% mark, quit, prev, next.

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
  → BROWSE_LIBRARIES           static home menu [Artists / Now Playing / Settings]
  → BROWSE_LIBRARY_PICKER      async library fetch; shown on first launch or via Settings
                               auto-selects + saves if only one music library
  → BROWSE_ARTISTS / BROWSE_ALBUMS / BROWSE_TRACKS
  → browse_load_worker()       background pthread for all data loads
  → main loop polls LoadState each frame; renders animated loading screen
  → B at home = quit confirmation dialog (A confirms, B cancels)
  → MENU+START = immediate quit (handled in ModuleCommon_handleGlobalInput)
  → plex_queue_set()           load tracks into queue
  → return MODULE_PLAYER

module_player_run()            playback UI
  → downloads track to temp file (plex_net_download_file) — async pthread
  → early-start after 512 KB buffered (MP3/FLAC/WAV/M4A only)
  → player.c                   decodes audio; stream thread retries EOF while file_growing
  → fire_scrobble()            detached pthreads for timeline/scrobble
  → screen auto-sleep: idle timer → SetRawBrightness(0); MENU+SELECT → PLAT_enableBacklight(1)
  → return MODULE_BROWSE

module_settings_run()
  → Switch Server, Sign Out, Screen timeout (Off/15s/30s/1min/2min/5min)
  → screen_timeout persisted to config.json

plex_net.c                     HTTP/HTTPS client
  → mbedTLS, TLS 1.2 max, VERIFY_NONE
  → redirect handling, gzip decompression

plex_art.c                     async cover art
  → disk cache at $SHARED_USERDATA_PATH/plexmusic/art/
```

---

## Related Projects (Reference Material)

### `paks/nextui-music-player/` — Primary reference implementation
What we copied: mbedTLS headers, PSA headers, mbedtls_config.h, entropy alt, font.ttf, Makefile structure.  
**Useful for:** any NextUI API call, GFX functions, PAD input, PWR/LEDS behavior.

### `htpcstation/` — Sibling project (same author)
HTPC Station is a fullscreen gamepad-first Linux interface for a home theater PC (Python/QML).  
**Relevance:** Plex integration reference for API behavior, response formats, edge cases.  
**Path:** `/home/thwonp/opencode/htpcstation/`
