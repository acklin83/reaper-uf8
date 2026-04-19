# Installing reaper_uf8 on macOS

## Prerequisites
- Homebrew libusb: `brew install libusb`
- REAPER (any recent version)
- UF8 plugged into the Mac
- **SSL 360° on macOS must be quit** — it exclusively claims the UF8's vendor interface, so our extension can't open it while 360° is running

## Build
```bash
cd reaper-uf8/extension
cmake -B build -G "Unix Makefiles"
cmake --build build -j$(sysctl -n hw.ncpu)
```
Output: `build/reaper_uf8.dylib`

## Install into REAPER
Symlink (preferred during development — `cmake --build build` rebuilds are live):
```bash
ln -sf "$PWD/build/reaper_uf8.dylib" ~/Library/Application\ Support/REAPER/UserPlugins/reaper_uf8.dylib
```

Or copy:
```bash
cp build/reaper_uf8.dylib ~/Library/Application\ Support/REAPER/UserPlugins/
```

Restart REAPER. On first load:
- **Success:** nothing visible in REAPER itself; the UF8 scribble strips should start showing REAPER's track colors (for tracks 1..8; bank-scroll is the next feature).
- **Failure:** REAPER's Console (View → Console) shows `reaper_uf8: <reason>`. Most common reason is `SSL360Core owns the device` — quit SSL 360° and re-launch REAPER.

## Uninstall
```bash
rm ~/Library/Application\ Support/REAPER/UserPlugins/reaper_uf8.dylib
```
Restart REAPER.

## Known gotchas (first-milestone scope)

- **Tracks 1..8 only** — no bank-shift hook yet. If your project has more than 8 tracks, strips 9+ aren't visible. Coming next.
- **Palette still incomplete** — only 5 of 16 palette indices are measured. Off-palette REAPER colors snap to the closest known entry. Do a systematic palette sweep (see `docs/protocol-notes.md`) to fill the rest.
- **libusb runtime path** — the .dylib links against `/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib`. On the machine that built it this is fine. For redistribution to other Macs, either static-link libusb or bundle it with `install_name_tool`.
- **SSL 360° mutex** — if SSL 360° is needed for other reasons (UC1, meters), we may later look at coexistence (probably impossible for the color-path USB interface; 360° would have to release the claim).
