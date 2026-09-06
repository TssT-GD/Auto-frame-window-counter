# Auto Frame Window (Geode mod, GD 2.2081)

Measures the frame window of every input in a run **automatically** by re-simulating the
game itself: it replays your inputs from practice checkpoints and nudges one input at a time
until the player dies. No manual frame entry.

Untested — written against the Geode 5.10.1 / GD 2.2081 bindings without a build
environment. Expect to fix a few compile errors and verify the items under **Verify first**.

## Build (Windows)

```
geode sdk install        # once, sets GEODE_SDK
geode sdk install-binaries
geode build
```
The .geode file lands in `GeometryDash/geode/mods/`. Needs Geode CLI, clang-cl/LLVM, CMake 3.27+.

## Use

1. Play the level in **normal** mode (not practice). HUD top-left shows `REC n inputs`.
   Turn off the in-game *Click Between Frames* setting — injection is tick-granular.
2. Die or finish, then pause → **Frame Windows** (button at bottom centre of the pause menu).
   It analyses the last completed run if there is one, otherwise the last run.
3. Watch: replay pass (lays a checkpoint every 60 frames) → trials, last input first.
   Pause → **Stop FW** to cancel.
4. Popup summary + `frame_windows_<levelID>.csv` in the mod's save directory
   (`geode/mods/declan.auto_frame_window/` → Save folder, or via the mod's settings page).

CSV columns: `index, frame, button, player1, down, early, late, window, early_capped, late_capped`.
`window = early + late + 1`. `*_capped` = probing stopped at max-shift or at a neighbouring
input on the same key, not at a death.

## Verify first (in order)

1. **Frame convention.** Record a run, start analysis. If the replay pass dies on a run you
   know is deterministic, the recorded frame and the injected frame are off by one tick.
   Fix: set *Inject offset* to -1 or +1 in settings. If neither works, the record hook
   (`handleButton`) and inject hook (`processCommands`, post-original) need to move to the same
   point in the tick.
2. **`removeCheckpoint(bool first)`.** The mod assumes `false` removes the *newest* checkpoint.
   If analysis aborts with "removed the wrong checkpoint", flip *removeCheckpoint arg*.
3. **Hold state after respawn.** Practice checkpoints don't store held buttons, so the mod
   re-presses anything held at the checkpoint. If ship/wave sections desync only after a
   respawn, this is the spot (`startSegment`).
4. **Level-end during trials.** For the last few inputs the lookahead may hit the end flag;
   the mod skips `levelComplete` and resets. If that glitches, lower *Lookahead*.

## Known limits

- Non-platformer only. Random triggers break determinism (xdBot patches the RNG seed on
  Windows for this; not done here).
- Each input's window is measured with all other inputs fixed.
- Time ≈ (number of inputs) × (window + 2) × (trial length ≈ 0.5–1.2 s) ÷ analysis speed.
  Lower *Checkpoint interval* for shorter trials; raise *Analysis speed*.
- Death runs: inputs near the death frame can't be measured (no future to survive into).
