# Zombie Tsunami for PS Vita

An unofficial fan port of the Android version of **Zombie Tsunami (4.7.3)** to the PS Vita / PS TV. PS Vita port by rodi.

The port is a loader (an "so-loader"): it loads the game's own Android libraries on the Vita and supplies everything they expect from Android (Java calls, OpenGL ES, sound, files, touch, the keyboard). **This repository and the release downloads contain no game code and no game data** - players use the files from their own copy of the game.

## Download and install

Get `ZombieTsunami_Vita_v1.0.zip` from the
[Releases](../../releases) page. Its `README.txt` has the full step-by-step
install guide, the controls and the promo codes. In short:

1. Install `kubridge.skprx` (under `*KERNEL` in your taiHEN `config.txt`) and
   have `libshacccg.suprx` in `ur0:data/` (the ShaRKBR33D app puts it there).
2. From your own copy of Zombie Tsunami **4.7.3** for Android (build 224,
   `net.mobigame.zombietsunami`), copy
   - `lib/armeabi-v7a/libc++_shared.so`, `libfmod.so`, `libfmodstudio.so`,
     `libcgame.so` to `ux0:data/zombietsunami/lib/`
   - the folder `assets/res` to `ux0:data/zombietsunami/assets/res/`
3. Copy `zombietsunami.vpk` to `ux0:data/` on the Vita, then in VitaShell go to
   `ux0:data/`, highlight the file, press X and choose Yes to install.

The Vita has a 32-bit ARM processor, so the game's **32-bit** files
(`armeabi-v7a`) are needed. A copy taken from a modern phone often only has
the 64-bit files (`arm64-v8a`) and will not work; you need a full bundle of
4.7.3 that includes `split_config.armeabi_v7a.apk`.

### Controls

| Button | Action |
| --- | --- |
| Touch screen | Plays like the phone version |
| X | Jump during a run (tap = hop, hold = long jump, fast taps = glide) |
| START | Pause / resume a run |
| Square | Shake (earthquake mission); shaking the Vita works too |

### Promo codes

In the game's **Market**, open the **Redeem code** box: `RODI` gives 100,000 diamonds,
`VITA` gives 100,000 coins. Any letter case, usable any number of times.

### Updates

When the game starts and the Vita is on Wi-Fi, it checks this repository for a
newer release. If there is one, a Vita pop-up asks whether to install it; the
game then downloads it, installs it and restarts. Saves are kept. Without Wi-Fi
nothing happens.

A running app cannot install a new version of itself, so the install is done
by a tiny second app, "Zombie Tsunami Updater" (`ZTUPDATER`, shipped inside the
VPK), the same way VitaShell updates itself: the game installs it and starts
it, it installs the update with the Vita's package installer and starts the
game again, and the game removes it.

## Building from source

You need [VitaSDK](https://vitasdk.org) (`nightly-softfp` channel) with these
packages installed through `vdpm`: `taihen`, `kubridge`, `vitaGL`,
`vitaShaRK`.

```
export VITASDK=$HOME/vitasdk
export PATH=$VITASDK/bin:$PATH
mkdir build && cd build
cmake ..
make
```

The result is `build/zombietsunami.vpk`.

Options: `-DCRASH_HANDLER=ON` writes CPU registers and a call trace to
`log.txt` on a crash (needs a recent kubridge); `-DREAL_GL=OFF` builds without
graphics (for debugging start-up only).

`tools/extract_libs.sh` and `tools/extract_assets.sh` pull the needed files
out of an APKMirror `.apkm` bundle of the game.

### Publishing a new version

1. Raise `PORT_VERSION` in `src/config.h` and `VITA_VERSION` in `CMakeLists.txt`
   (`"1.1"` and `"01.10"`, ...).
2. Build, then copy `build/zombietsunami.vpk` to `release/zombietsunami.vpk`.
3. Commit, tag it (`v1.1`, ...) and publish a GitHub release with that tag and
   the release zip.

The auto-updater reads the latest release's tag and downloads
`release/zombietsunami.vpk` at that tag through api.github.com (the Vita cannot
make secure connections to GitHub's release file servers).

## How it works

| File | What it does |
| --- | --- |
| `src/so_util.c`, `src/stage1.c` | Load, relocate and link the four Android libraries |
| `src/imports.c`, `src/stage2b.c` | Android/libc functions the game needs; start-up and the frame loop |
| `src/jni.c` | A fake Java VM: answers every Java call the game makes |
| `src/gfx_vita.c`, `src/gl_real.c` | Screen through vitaGL; touch, buttons and motion sensors |
| `src/opensl_vita.c` | Sound (OpenSL ES for FMOD) |
| `src/assets.c`, `src/asset_io.c`, `src/asset_cache.c` | The game's files read from `ux0:` (memory card, SD2Vita or internal storage), with caching |
| `src/patches.c` | Small fixes to the game's code (social buttons, Facebook pop-up, loading counter...) |
| `src/ime.c` | The Vita keyboard for the promo code box, and the port's promo codes |
| `src/updater.c`, `src/update_util.c`, `src/update_certs.h` | The auto-updater: checks GitHub, asks, downloads and prepares a new version |
| `src/updater_helper.c`, `src/update_promoter.h` | The "Zombie Tsunami Updater" app that installs it and starts the game again |
| `src/popup.c`, `src/game_art.c`, `src/art_patch.c` | The "report a bug" button and pop-up, built at runtime from the game's own sprite sheets |

The report-bug pop-up and the menu's warning-sign button are cut out of the
player's own game files while the port runs, so no game art is needed to
build. Only our own text is shipped (`popup/`, made by `tools/make_popup.py`
and `tools/make_credit.py`).

## Legal notice

This is an unofficial, non-commercial fan project. It is not made, endorsed,
sponsored or approved by Mobigame or by Sony Interactive Entertainment, and is
not affiliated with them in any way.

"Zombie Tsunami" and all related names, characters, artwork, music and other
content are the property of Mobigame. "PlayStation" and "PS Vita" are
trademarks of Sony Interactive Entertainment. All trademarks belong to their
respective owners and are used only to describe what this software works with.

No game code or game data is included in this repository or its releases. To
use the port you must supply the game's files yourself, from a copy of the game
you are legally entitled to use. Do not open issues asking for game files, and
do not share them.

The software is free and must not be sold. It is provided "as is", without
warranty of any kind; use it at your own risk.

If you are a rights holder and have a concern about this project, please open
an issue or contact the author and it will be dealt with promptly, including
removal if you ask.

## Credits and licences

- PS Vita port: rodi - Instagram `@rodingley`, Discord `rodingley`
- Zombie Tsunami (c) Mobigame. All rights reserved.
- so-loader code adapted from [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita)
  by Andy Nguyen (MIT licence, see `LICENSE-so_util-AndyNguyen`)
- [vitaGL](https://github.com/Rinnegatamante/vitaGL) and
  [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK) by Rinnegatamante
  (GNU LGPL v3), linked into the port
- zlib by Jean-loup Gailly and Mark Adler (`src/zlib/LICENSE`)
- kubridge (plugin, installed separately)

The port's own code is released under the MIT licence (`LICENSE`).
