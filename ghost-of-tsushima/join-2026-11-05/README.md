# Joined host imports kept across address-based builds (Ghost of Tsushima, PPSA02225)

Machine: AMD Ryzen 7 7840HS, 16 GB RAM, NVIDIA GeForce RTX 4060 Laptop GPU (8 GB), driver 617.42, Windows 11 (26200), 3840x2160 guest output.

Both sides are the same local build apart from the change under test: a tree with the known-zero scan fix (boykopovar/AnyPS5#15967), the depth capture touch and trace counters. `main-*` has the joined-import check disabled, `join-*` has it.

Scene: same launch, `APS5_TRACE_SECONDS=45:25` (25 s window starting 45 s after the first present), `APS5_PROFILE_DRAW=1`, `APS5_SKIP_REFUSED_DRAWS=1`.

- `traces/main-aps5-trace-45-25.json.gz`, `traces/join-aps5-trace-45-25.json.gz` and their `*-aps5-summary-45-25.jsonl` lines
- `main-digest-45-25.txt`, `join-digest-45-25.txt`: `python3 tools/trace_digest.py <trace> --summary <summary>`
- `runs-45-25-fps-and-address-profile.txt`: fps per 4 s window and the `[address]` profile lines of both full runs
- `logs/ctest-without-fix.log`, `logs/ctest-with-fix.log`: `ctest -R "agc_driver_(address_draw_space_reuse|host_import_windows|resident_heap_mirror|graphics|address_space_teardown)" -V` with `ANYPS5_REQUIRE_VULKAN=1 ANYPS5_NO_SHADER_CACHE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`
