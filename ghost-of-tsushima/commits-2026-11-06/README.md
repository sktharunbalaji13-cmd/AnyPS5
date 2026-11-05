# Ghost of Tsushima (PPSA02225), clean-commit comparison, 2026-11-06

Machine: Windows 11 26200, AMD Ryzen 7 7840HS, 16 GB RAM, NVIDIA GeForce RTX 4060 Laptop GPU (8 GB), driver 617.42.
Every run: 145 s, `APS5_TRACE_SECONDS=60:25`, `APS5_PROFILE_DRAW=1`, `APS5_SKIP_REFUSED_DRAWS=1`, `APS5_SOFT_DEPTH_CHECKS=1`, game window hidden (click-through, no focus change) because the laptop was in use.

| label | commit | what the build is |
|---|---|---|
| C0a, C0b | 3fe8829a8abf | local scratch tree without the joined-import check (#16130) and without the byte comparison (#16188) |
| C1a, C1b | d842941d04e3 | C0 plus the joined-import check (#16130) |
| C3a, C3b | f3222bb7c416 | C1 plus the byte comparison for blocks of watched heap mirrors (#16188, both commits) |

The scratch tree also holds the known-zero scan fix (#15967), trace counters and unmerged depth-surface experiments. The commits are local scratch commits.

- `traces/` the `aps5-trace` captures (window 60:25)
- `summaries/` the `aps5-summary.jsonl` line of each run (the `trace` field names the file in `traces/`)
- `digests/` full `tools/trace_digest.py` output of each run
- `logs/` the `[address]` lines and per-window fps of each run
- `ctest/ctest-gate.log` focused ctest log of #16188 with both commits (22/22 passed, no VUID- lines)
- `frames/` `f059.png` is the first presented frame with scene content after the PlayStation Studios montage in a longer run of a build with the depth experiments; `f059_amp.png` is the same frame brightened 8x
