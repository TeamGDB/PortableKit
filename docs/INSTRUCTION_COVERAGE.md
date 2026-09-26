# Instruction coverage

Which Allegrex instructions the decoder, the interpreter and the recompiler handle, and which of them a game has actually run. Keep it current: when a game runs an instruction from the unverified list, check its result against what the game expects, then move it up.

## How to tell

- The recompiler leaves what it cannot lower as `unsupported(pc, word, "<name> not lowered yet")` in the generated code, and the interpreter stops on the same words. [BRINGING_UP_A_GAME.md](BRINGING_UP_A_GAME.md) says how to count them.
- Every instruction in the unverified list below prints one line the first time it runs, in the interpreter or in recompiled code:
  `[instruction] first use of vsbn (0x61010002) at 0x08810000: UNVERIFIED, ...`
  That line in a game's log is the signal to check it.

## Verified by a game

Everything the decoder names outside the list below (`src/decoder.cpp`) was there before this page, for the games ported so far. Which game ran which one is not recorded instruction by instruction.

| Instruction | Game | What was checked |
| --- | --- | --- |
| `vcrs.t` | Monster Hunter Portable 2nd G (six sites in its executable) | Decoding against the neighbouring VFPU1 operations, and the result by hand in `psprecomp_tests`. The game's use of it (building quaternions from half-angle sines and cosines) has not been compared with a console |

## Implemented, not yet run by any game

`src/extra_instructions.cpp`, one function for both the interpreter and the recompiled code (`execute_extra_instruction`), so the two cannot disagree. Each has a hand-checked unit test in `psprecomp_tests` (`test_extra_instruction_semantics`), and none has been compared with a console. Every one carries the mark `UNVERIFIED: not yet exercised by a real game` in the source.

| Instruction | Encoding | What it does | Known simplifications |
| --- | --- | --- | --- |
| `ll` | op 0x30 | Load word, start a linked sequence | The paired `sc` always succeeds (below) |
| `sc` | op 0x38 | Store word, `rt = 1` | Always succeeds: one core, and guest threads switch only at system calls, so nothing can come between `ll` and `sc` |
| `lvl.q`, `lvr.q` | op 0x35, bit 1 | Load the left or right part of a quad at a word address | No prefixes, as `lv.q` |
| `svl.q`, `svr.q` | op 0x3D, bit 1 | Store the left or right part | |
| `vsbn` | VFPU0 op 2 | `s.x` with its exponent replaced by `127 + t.x` (integer) | Other lanes copied from `s` |
| `vdet.p` | VFPU1 op 6 | `s.x*t.y - s.y*t.x` | Ordinary prefixes; the hardware rewrites the t swizzle |
| `vcrs.t` | VFPU1 op 5 | See above | Ordinary prefixes; the hardware rewrites the s and t swizzles |
| `vwbn` | 0x34, bits 24-25 = 3 | Rescale `s.x` to the exponent in bits 16-23 | Shift limited to 15 places, as described |
| `vrnds` | VFPU7 op 0 | Seed the random generator | The framework's generator is not the console's, so the numbers after a seed differ anyway. The seed register is the vd field |
| `vsbz` | VFPU7 op 22 | `s.x`'s mantissa with the exponent of 1.0 | Sign dropped |
| `vlgb` | VFPU7 op 23 | `s.x`'s unbiased exponent as a float | `-inf` for zero and denormals |
| `vi2c` | VFPU7 op 29 | Top byte of each integer lane, packed | |
| `vi2us` | VFPU7 op 30 | Lanes clamped at zero, `>> 15`, two per word | |
| `vsrt1`–`vsrt4` | VFPU9 ops 0, 1, 8, 9 | Sorting-network steps on a quad | `fmin`/`fmax` for NaNs and signed zeros; ordinary prefixes |
| `vbfy2` | VFPU9 op 3 | `(x+z, y+w, x-z, y-w)` | Ordinary prefixes |
| `vmfvc`, `vmtvc` | VFPU9 ops 16, 17 | VFPU control register to or from a scalar | No write masks, as `mtv` to a control register has none either |
| `vt4444`, `vt5551` | VFPU9 ops 25, 26 | 8888 colours to 16 bits, two per word | |

The encodings and semantics come from public descriptions of the VFPU and from other emulators' documented behaviour, used as facts; no code was copied.

## Not implemented

| Instruction | Why |
| --- | --- |
| `teq`, `tge`, `tlt`, `tne` and their immediate forms | Trap instructions. No compiler-generated PSP code has been seen to use them; the decoder leaves them unsupported |
| `mfc2`, `mtc2`, `cfc2`, `ctc2` (as distinct from `mfv`/`mtv`) | No known use, and no public description of their behaviour on the Allegrex |
| `synci`, `rdhwr` | No known use on the PSP |
| `cop0` moves, `eret`, `halt`, `mfic`, `mtic`, `wait`, `deret`, `cache` beyond a no-op | Kernel mode: the runtime implements the kernel itself, and game code never runs these |
