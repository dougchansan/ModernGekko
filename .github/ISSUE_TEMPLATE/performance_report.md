---
name: Performance report
about: Report a game running slower than expected, with a diagnostics capture
title: "[perf] "
labels: performance
---

## What is slow

Which game, which section, and what happens.

## Expected versus observed

What frame rate or emulation speed you expected, and what you saw instead.
If the same build runs well on another machine, say so and attach that report
too — two reports compare far better than one.

## Diagnostics capture

Attach the `.mgdiag` file produced by:

```
moderngekko-run --game <extracted-root> --module <module> --diagnostics-level detailed
```

Play to the slow section, press `c` + Enter to start, play for 20-60 seconds,
then press `c` + Enter again. The runner prints where the report was written.

For a sudden hitch rather than a steady slowdown, press `h` + Enter right after
it happens to save the last 30 seconds of history instead.

Reports contain no game content, no save data and no personal paths. See
`docs/diagnostics.md` for exactly what is and is not captured.

- [ ] `.mgdiag` file attached

## Anything else

Driver version, unusual graphics settings, mods in use, netplay, overclocking.
