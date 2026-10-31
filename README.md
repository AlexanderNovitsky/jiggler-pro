# Jiggler Pro

USB mouse jiggler for Flipper Zero. Once a second it moves the pointer sideways, then back by the same distance, so the cursor stays in place.

While the app is open, Flipper's USB port is a mouse. Hold **Back** to exit and restore the previous USB mode.

## Controls

| Button | Action |
|---|---|
| **OK** | Pause for the configured number of seconds. Press again to resume early. |
| **Back** | Open or close Settings. |
| **Back** (hold) | Exit. |

When the pause ends, the buzzer plays a short tune and jiggling continues. Opening Settings freezes a running countdown and restores it when you go back.

## Settings

Up and down select a row. Left and right change the value. Settings are saved on the SD card.

| Setting | Default | Limits |
|---|---|---|
| Range | 40 px | 1–120 |
| Pause | 10 s | 1–180 |

## Build

External app, built with [uFBT](https://github.com/flipperdevices/flipperzero-ufbt) for Flipper Zero (`f7`).

```bash
ufbt            # build dist/jiggler_pro.fap
ufbt launch     # build, install, and run over USB
```

On the device: **Apps → USB → Jiggler Pro**.
