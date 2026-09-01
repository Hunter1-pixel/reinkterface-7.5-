# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

ESP32-S3 firmware for a standalone e-ink PC stats display. It is a fork of
[Valve's Inkterface](https://gitlab.steamos.cloud/SteamHardware/SteamMachine/inkterface/),
rewritten for non-Steam-Machine hardware: no battery, no soldering, wired
straight into a desktop PC's internal USB header. Built for a Bazzite Linux
desktop but works with any host running Valve's companion app.

This repo is **firmware only**. The other half of the system — the host-side
app that reads system stats and pushes them over Bluetooth — is Valve's
separate closed-source-ish app built from
[Valve's GitLab repo](https://gitlab.steamos.cloud/SteamHardware/SteamMachine/inkterface);
it is not part of this codebase and cannot be built or modified here.

Hardware:
- Board: Seeed XIAO ePaper Display Board (ESP32-S3), "EE04"
- Panel: Waveshare 5.83" e-ink, 648x480, SSD1677 controller

## Commands

Build system is PlatformIO (`platformio.ini`, single env `seeed_xiao_esp32s3`).

```
pio run                 # compile
pio run -t upload       # build and flash over USB
pio device monitor       # serial monitor (115200 baud)
```

There are no unit tests and no linter configured — `test/`, `lib/`, and
`include/` only contain PlatformIO's stock placeholder READMEs and are
otherwise unused. `src/main.cpp` is effectively the entire program.

## Architecture

Single-file firmware (`src/main.cpp`) with two responsibilities that meet in
one global `STATE` struct: a BLE GATT server that receives stats, and an
e-ink renderer that draws them.

**BLE transport (NimBLE-Arduino).** One GATT service
(`SERVICE_UUID`) exposes six characteristics that the host app writes to:
`TOPLINE`/`MIDLINE`/`BOTLINE` (plain strings for the header block),
`KEYVAL` (packed struct: `index + char key[32] + char val[32]`, fills the
9-slot `STATE.keyvals`), `VECTOR` (packed struct: `index + count + minVal +
maxVal + 64 bytes of point pairs`, fills the 6-slot `STATE.sparks` sparkline
data — point bytes are 0-255 and get normalized to 0.0-1.0 against
min/max), and `FLUSH` (any write signals "frame complete", triggers a
redraw and clears idle mode). **These UUIDs and packed struct layouts are a
compatibility contract with Valve's upstream host app — do not change them
without a corresponding change on the host side.**

**Render loop.** `loop()` runs a debounce-timer pattern, not immediate
redraws: `DISP_DEBOUNCE` and `CONN_DEBOUNCE` counters gate when
`redrawDashboard()` and BLE-advertising-state syncing actually fire,
coalescing bursts of characteristic writes into a single screen redraw.

**Partial refresh.** `redrawDashboard()` snapshots `STATE` under
`STATE_MUTEX` (NimBLE callbacks run on their own FreeRTOS task, genuinely
concurrent with `loop()` on the S3's other core — nothing may touch `STATE`
outside that lock) and diffs the snapshot against `RENDER_CACHE` to redraw
only the widgets whose data changed, via `MF_DISPLAY.displayWindow(x,y,w,h)`.
Two GxEPD2 gotchas that cost real debugging time and matter for any future
change here:
- `display()` always refreshes the *entire* panel regardless of any window
  set beforehand — `displayWindow(x,y,w,h)` is the actual per-rect primitive.
- **Never call `setPartialWindow()` before drawing.** It re-addresses
  `drawPixel()`'s coordinate frame (subtracts the window's x/y, strides by
  the window's width), which is incompatible with how `displayWindow()`
  reads the buffer back out in full-panel absolute addressing. The display
  must stay in `setFullWindow()` mode for all drawing, full or partial.

Between active redraws the panel is put in `powerOff()`, not `hibernate()`:
`hibernate()` forces a hardware reset on the next wake that wipes the panel
controller's own "previous frame" RAM, which the partial-refresh waveform
diffs against — corrupting the next partial update. Real `hibernate()` is
reserved for idle mode (`drawSleepScreen()`), which is always followed by a
forced full refresh on wake (`FORCE_FULL_REFRESH`), so it's unaffected.
`FORCE_FULL_REFRESH` is also set on connect/disconnect, and a full refresh
runs periodically (`FULL_REFRESH_INTERVAL`) regardless, to bound e-ink
ghosting from repeated partial updates. `HEADER_RECTS`/`DISCRETE_RECTS`/
`SPARK_RECTS`/`HOSTMSG_RECT` mirror `drawStatic()`'s layout math and must be
kept in sync with it by hand if that layout ever changes.

**Idle / sleep mode.** If no BLE client is connected for
`IDLE_TIMEOUT_MS` (5 min), `enterIdleMode()` draws a fixed TRMNL sleep
bitmap (`sleep_screen.h`) and hibernates the panel instead of the normal
dashboard. BLE advertising keeps running but switches to a slower interval
profile (`ADV_IDLE_*` vs `ADV_ACTIVE_*` in `setAdvertisingProfile()`) to cut
power while staying discoverable. Any new connection or `FLUSH` write calls
`exitIdleMode()` and restores the normal dashboard on the next debounce
tick.

**Drawing primitives** (`drawText`, `drawLogo`, `drawSparkbox`,
`drawDiscreteBox`) are hand-rolled on top of `GxEPD2_BW` — there is no
layout engine. `drawStatic()` lays out the whole screen procedurally by
threading an `x` cursor through the draw calls left to right, row by row
(logo + status lines, then a row of 3 plain `drawDiscreteBox` tiles, then
two rows of 3 `drawSparkbox` tiles with sparklines). `SPARKBOX_HEIGHT` /
`SPARKBOX_WIDTH` and the hardcoded x/y offsets in `drawStatic()` are all
coupled to the 648x480 panel size — changing tile counts or panel geometry
means re-deriving all of these by hand.

**Bitmaps** (`bazzite_logo.h`, `sleep_screen.h`) are raw `PROGMEM` byte
arrays, not meant to be hand-edited. They were generated from source images
via [image2cpp](https://javl.github.io/image2cpp/) (see README's "Logo"
section for the exact pipeline: source image → Boxy SVG cleanup → image2cpp
→ paste into a `.h` file).

`partitions.csv` is a custom single-app partition table (modified from
PlatformIO's `singleapp_large.csv`) — required because the default table
doesn't fit this build.

## Notes carried from README

- Changing the logo: source a 100x100px bitmap, run it through image2cpp,
  and replace `bazzite_logo_bitmap` in `src/bazzite_logo.h`.
- Fan RPM reporting requires a one-line patch on the **host app side**
  (`sysstats.hpp` in Valve's repo, not this repo) pointing
  `getFanRPM()` at the correct `hwmon` node for the target motherboard —
  Valve's stock app reads a Steam Deck-specific hwmon node that doesn't
  exist on normal PCs. See README's "Fan RPM" section for the walkthrough.
- Author has disclosed using an LLM to help with the rewrite.
