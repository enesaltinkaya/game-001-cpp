# Screenshots & Frame Image Dumps

How to capture the final composited frame for visual verification. The happy
path is documented in `AGENTS.md` (Validation guidance); this file covers the
underlying env vars and extras.

## Basic flow

```bash
./scripts/run.sh play screenshot /tmp/screenshot.jpg   # gameplay (skips main menu)
./scripts/run.sh screenshot /tmp/menu.jpg              # main menu (no gameplay)
```

`run.sh` sets `ENGINE_SCREENSHOT=<path>` (default `/tmp/screenshot.jpg`).

## Behavior (debug builds only)

- The engine subscribes to the `gameLoaded` signal, waits an initial delay,
  then captures. It **exits automatically after the last capture**, so no log
  timeout is needed for screenshot runs.

| Env var                        | Default         | Description                                                        |
| ------------------------------ | --------------- | ------------------------------------------------------------------ |
| `ENGINE_SCREENSHOT`            | —               | Output path (`.jpg`). Unset = no capture.                         |
| `ENGINE_SCREENSHOT_COUNT`      | 1               | Shots taken as consecutive frames; saved as `<base>_1.jpg` … `<base>_N.jpg` (a `.jpg`/`.jpeg` suffix on the path is stripped before appending the index). |
| `ENGINE_SCREENSHOT_DELAY_MS`   | 5000            | Delay after game load before the first capture. Increase so the TAA accumulator converges and the camera settles for shimmer captures. |
| `ENGINE_DEBUG_DUMP_IMAGES`     | —               | Comma list of `velocity,depth,color,taa` — dumps those frame attachments as JPGs next to each screenshot (float formats auto-normalised per channel). |

Example multi-shot convergence capture (15 s settle, then 8 consecutive frames):

```bash
ENGINE_SCREENSHOT_COUNT=8 ENGINE_SCREENSHOT_DELAY_MS=15000 \
  ./scripts/run.sh play screenshot /tmp/shimmer.jpg
```

## Saving arbitrary render targets

- `vulkanScreenshot(path)` — saves the current swapchain image as JPG.
- `vulkanSaveImage(img, path)` — saves any `VulkanImage` (render targets, depth buffers, etc.) as JPG.
  - Float formats are auto-scaled to 0–255 per channel.
  - Integer formats get BGRA→RGBA swizzle for swapchain-like formats.