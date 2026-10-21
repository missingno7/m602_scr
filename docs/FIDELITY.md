# Fidelity

Reference: the [m602_recon](https://github.com/missingno7/m602_recon) matching
decompilation, `src/SAVER.PAS`, which is relocation-equivalent to overlay 61 of
`M602X.EXE`, and its notes `docs/notes/saver.md` and `saver_handoff.md`.
Addresses are in its `res:SEG:OFS` / `ovl:N:OFS` notation.

## Translated from the original (`src/saver.cpp`)

| Original | Notes |
|---|---|
| `ScreenSaver` `ovl:61:06D8` | same statements and order; `Integer` = `int16_t`, `Word` = `uint16_t`, `Boolean` = byte |
| `Draw` `ovl:61:0121` | erase the trailing column/row of every box, then draw boxes 1, 2, 3; date and time re-read every frame |
| `DrawString` `ovl:61:0000` | `Byte` X/Y, 16-bit offset `(Y*ScreenWidth+X)*2`, `lodsb/stosw/loop` |
| `Pause` `ovl:61:0073` | `(Ticks-FirstTick)*55` in LongInt, both tick reads, midnight exit |
| TP 6.01 `Random`, `Randomize` | `RandSeed*134775813+1`, `(RandSeed shr 16) mod n`, seed from INT 21h AH=2Ch |
| `res:119C:0201`, `Maximum`/`Minimum`, `Drivers.ClearScreen` | digit formatting, signed LongInt clamps, fill with `0720h` |
| `DS:539C`, `DS:53D8`, `DS:53DE` | box lines, initial directions |

The screen is an emulated 32 KB B800h text buffer of character/attribute
words. The saver writes it as the original did and never clears it between
frames; the renderer only reads it.

## Replaced machine dependencies

| Original | Replacement |
|---|---|
| BIOS tick count `0:046C` (18.2065 Hz, wraps at 1800B0h) | computed from the monotonic clock, phase from the time of day (`src/biostime.h`; `winmachine.h`, `posixmachine.h`) |
| INT 28h in Pause's busy-wait | sleep until the next tick; Pause's exit depends only on the tick count, so frames happen at the same ticks without using the CPU |
| other INT 28h calls | omitted (no observable effect) |
| DOS GetDate / GetTime | local time |
| `Application^.GetEvent` | input events from the window on Windows; on macOS the system ends the saver |
| VGA scan-out | `src/renderer.cpp` |
| cursor parking, mouse hiding | no text cursor drawn; mouse pointer hidden |

## Adaptations

- **Text width.** `ScreenWidth` is `round(60 × width/height)` (80 on 4:3, 96 on
  16:10, 107 on 16:9, 140 on 21:9), clamped to 41..255. The original movement
  rules handle any width via `MaxX = ScreenWidth - 11`. Rows, box size, start
  column 30 and speed are unchanged. The text area is fitted with the original
  proportions, which can leave margins of a few pixels. The *80×25* setting
  keeps 80 columns.
- **Uninitialized locals.** `StepY` and `Cycle` are never set in the original;
  their values are leftover stack contents. The port starts with `False` and
  `1`, so all boxes keep their initial directions until box 1's first periodic
  re-roll at frame 5.
- **Exit.** Key press, mouse button or wheel, or moving the mouse more than
  4 px ends the saver at its next event check. Turbo Vision also ignored
  movement within one text cell.
- **Not ported:** idle activation, mouse-corner and modem triggers, and saving
  and restoring the underlying screen.
- **Multiple monitors:** each monitor runs its own saver, sized to its shape.
- **Speed setting:** the emulated tick rate is scaled. Pause still waits
  2 ticks and the sequence of frames is identical.
- **Color setting:** replaces palette entry 7 (`#AAAAAA`), the only colour
  the saver uses.

## Display

- 9×16 cells with the VGA line-graphics rule: the 9th column repeats the 8th
  for C0h–DFh, so box lines join.
- 720×400 is shown as 4:3, so raster pixels are 20:27, not square.
- Area-weighted resampling in linear light keeps raster pixels equal in size
  at fractional scales.
- Optional scanlines follow the 400 logical raster lines.
- Font: M602 loads its own KeybCS set by default (help text: "Nezavádět font"
  turns this off). Its `f` and `7` differ from the IBM ROM font. The block at
  file offset BEF9Fh of `M602X.EXE` was identified by content: KeybCS layout,
  87h = č, 9Fh = ť.

## Verification (`tests/`)

- **DOS reference:** `gen_reference.py` compiles the unmodified `SAVER.PAS`
  with Turbo Pascal 6.01 and runs it under MS-DOS Player with:
  - a scripted DOS clock;
  - INT 28h advancing the tick count;
  - `StepY`/`Cycle` preset in their stack slots.

  It logs the seed, positions, directions, `Cycle`, `StepY` and a screen hash
  after every frame. The C++ port matches all 6,300 frames: widths 80, 107,
  140 and 41, and initial `(StepY, Cycle)` of `(F,1)`, `(T,0)` and `(T,3)`.
- **Screen:** after every frame the screen equals a blank screen with boxes
  1, 2, 3 drawn in order, so no stale cells are left behind.
- **PRNG:** a register-level transcription of `res:1570:1726` matches the
  port.
- **Timing:**
  - With a simulated clock, frames come exactly 2 ticks apart, and the wait
    is 1 tick across midnight.
  - In real time, frames measured 109.86 ms apart (expected 109.85 ms) with
    about 0 % CPU.

## Open points

- The initial `StepY`/`Cycle` values (see Adaptations).
- Palette shades and screen mode can be changed in M602; the defaults are
  assumed.
- The code path that loads the font block was not traced.
- Not modelled: the original read the tick count in two word reads, which can
  tear once an hour, and a frame being scanned out while half drawn.
