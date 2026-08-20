## Validation guidance

When making code changes, prefer validating with:

1. `./scripts/build.sh`
2. `./scripts/run.sh` if runtime verification is needed

### Visual verification via screenshot

When a change affects rendering (shaders, passes, materials, lighting, etc.),
you can capture and inspect the final frame:

```bash
./scripts/run.sh screenshot              # saves to <project-root>/screenshot.jpg
./scripts/run.sh screenshot /tmp/out.jpg  # saves to a custom path
# note: game automatically takes screen shot and exists after a couple of seconds.
```

This sets `ENGINE_SCREENSHOT=<path>`, which makes the engine render 200 frames
(enough for resources to load), take a screenshot of the final composited frame,
and exit automatically. You can then read the resulting image to verify the
visual output is correct.
