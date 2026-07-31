# rsDeck UI/Network Core Split — Handoff & Next Steps

**Status:** Stages 0–2 complete and compile-verified. The tree now runs with split-mode
support in place: UI loop on `loopTask` (core 1), network task on core 0 when
`RSDECK_UI_CORE_SPLIT=1`, and stock superloop behavior when the flag is `0`.

Build flag: `-DRSDECK_UI_CORE_SPLIT=1` in `platformio.ini`. Set it to `0` to get the
exact stock superloop back (every guard compiles to a no‑op). Flip this to A/B test.

> Build/flash needs **esptool v5** (`merge-bin` hyphen syntax). Already installed
> (5.3.1) in the PlatformIO penv. If you hit `invalid choice: 'merge-bin'`, run
> `python3 -m pip install --upgrade 'esptool>=5'`.

---

## Why

The UI goes sluggish/unresponsive under network load. Root cause: single‑threaded
superloop — LVGL render + input share `loop()` with `rns.loop()` (Reticulum crypto), so
UI latency == loop cycle time, which grows with traffic. Confirmed with the user it's
**load‑dependent**, not an idle leak.

**Fix:** move the Reticulum/radio/network stack to a FreeRTOS task pinned to **core 0**;
keep LVGL + input on the Arduino `loopTask` (**core 1**). Render never waits on crypto.

## Shared subsystems that cross the two cores (must be serialized)

1. **SPI bus** — display (LovyanGFX), SX1262 radio, and SD all share FSPI (SCK 40 / MOSI
   41 / MISO 38). Guarded by `CoreSync::spiBusMutex` (recursive). **DONE.**
2. **Reticulum/LXMF/AnnounceManager data + flash/SD persistence** — one "backend" lock,
   `CoreSync::rnsMutex` (recursive).
3. **Network → UI status** — lock‑free `CoreSync::netStatus` snapshot + a deferred‑toast
   bridge (`requestToast`/`takePendingToast`). **DONE (infra).**

**Lock ordering (keep it consistent to stay deadlock‑free): always `rnsMutex → spiBusMutex`.**
The display flush takes `spiBusMutex` only (never `rnsMutex`), so there's no inversion.

---

## What's DONE (committed to working tree, compiling)

### New module
- `src/platform/CoreSync.h` / `.cpp` — mutexes (`spiBusMutex`, `rnsMutex`), RAII guards
  (`SpiBusGuard`, `RnsGuard` blocking, `RnsTryGuard` try‑lock), the `NetStatus` atomic
  snapshot, and the toast bridge. All no‑ops when the flag is 0.
- `CoreSync::begin()` is called first thing in `setup()` (main.cpp).

### Stage 1 — SPI bus guarded (complete)
- `src/hal/Display.cpp` — `lvgl_flush_cb` wrapped in `SpiBusGuard`.
- `src/radio/SX1262.cpp` — all **5** SPI primitives wrapped: `singleTransfer`,
  `executeOpcode`, `executeOpcodeRead`, `writeBuffer`, `readBuffer` (guard taken *after*
  `waitOnBusy()`, around the transaction only).
- `src/storage/SDStore.cpp` — runtime methods guarded (recursive mutex handles the
  nesting: `writeString→writeAtomic`, `ensureDir` recursion, etc.). `begin()` is
  intentionally unguarded (boot, pre‑task).
- `src/ui/screens/LvSettingsScreen.cpp` — the identity‑import dir walk (the one runtime
  `openDir`+iterate site) wrapped in a `SpiBusGuard` scope.

### Stage 2 — complete
- `src/platform/CoreSync.*` — `NetStatus` + toast bridge added.
- `main.cpp` `announceWithName()` — refactored: guards `rns.announce` with `RnsGuard`,
  delivers flash/toast via the snapshot bridge (safe from either core).
- `main.cpp` loop split landed:
  - Added `networkLoopStep()` + `uiLoopStep()`.
  - Added `networkTask()` pinned to core 0 under `RSDECK_UI_CORE_SPLIT`.
  - `loop()` now dispatches split mode (`uiLoopStep()` only) vs stock mode
    (`uiLoopStep()` + `networkLoopStep()`).
  - Network-owned status updates now publish to `CoreSync::netStatus`.
  - UI status tick reads `CoreSync::netStatus` and refreshes UI under `RnsTryGuard`.
  - Added setup spawn after `bootComplete = true`:
    `xTaskCreatePinnedToCore(networkTask, "netstack", 16384, ..., 0)`.

---

## What REMAINS

Stage 2 implementation work is complete. Remaining work is optional Stage 3 refinement
plus on-device soak validation under sustained traffic.

### Stage 3 (optional refinement — removes choppy‑under‑flood)

Make input **lock‑free** by moving the backend guard out of the coarse dispatch block and
into only the handlers that touch Reticulum/LXMF, so pure‑LVGL nav/typing/scroll never
take `rnsMutex`. Sites to guard individually (wrap the backend call in `CoreSync::RnsGuard`):
- `LvMessageView` — message send (`_btnSend` CLICKED), `markRead`, open‑chat
  `getRecentMessages`.
- `LvMessagesScreen` — open conversation, `markRead`.
- `LvNodesScreen` — save/delete contact.
- `LvSettingsScreen` — `_rns->announce`, radio `set*`/`receive`/`sleep` apply, TCP reload.
- Home toggle callbacks — the `userConfig.save()` calls (SD/flash writes).
Then remove the blocking `RnsGuard` from the `uiLoopStep` dispatch block. Refresh reads
stay on the try‑lock.

---

## Build / flash / on‑device test plan

```bash
pio run                      # compile (flag on)
make flash PORT=/dev/tty...  # or use the Ratspeak web flasher with rsdeck-merged.bin
```

Watch the serial `[HEART]` line — it already prints `loop=<ms>` (max loop time) and
`heap=/min=`. Test sequence:

1. **Regression (flag on, before task spawn is even needed):** current tree — confirm it
   boots, UI works, radio RX/TX works, SD reads/writes, no display corruption. This
   validates Stage 1 (SPI guards) in isolation.
2. **After the split lands:** load the device (WiFi on + TCP hub + announce traffic).
   - UI should stay smooth (scroll/type) while `[HEART] loop=` on the **UI core** stays
     low. The heavy time now lives in the network task.
   - Confirm status bar (wifi/tcp/ble/peers) still updates — it's snapshot‑driven now.
   - Confirm announce (Enter on Home) still flashes the TX indicator + toasts, and that
     auto/boot announces work (they run on the network core via the bridge).
   - Send/receive an LXMF message; open a chat mid‑traffic.
   - Watch for **stack overflow** panic (raise `netstack` stack if so) and for any
     **`Interrupt wdt timeout`** (would indicate an SPI collision — check every radio/SD
     path is guarded).
3. Set flag to 0, rebuild — must behave exactly like today (sanity).

## Reference

- Full running context is in Claude project memory: `ui-core-split.md`,
  `esptool-v5-required.md`.
- LSP "file not found / Arduino.h" errors in the editor are noise (clangd lacks the
  PlatformIO `-Isrc` include path); the real `pio run` is the source of truth.
