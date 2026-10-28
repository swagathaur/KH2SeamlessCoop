# Performance log

PC benchmark (`scratch/bench.ps1`): a new game with cutscenes and movies
skipped, circle mashed until Roxas stands in Twilight Town's Market Street
(guest frame ~2000), then the host profiler samples guest frames 2400-4200
(30 s at 60 fps). Busy % is of one core; the main (pump) thread spins in the
headless harness, so its busy % does not measure work. Two baseline runs agreed
within 0.6 points per thread; screenshots differ by 5-23 pixels between them.

| Change | DMA worker (VIF1/VU1) | EE thread | Notes |
| --- | --- | --- | --- |
| Baseline | 58.4 / 58.5 % | 33.5 / 33.5 % | |
| File-override scan (kh2files::tick) | - | - | 1-3 ms per 10 s measured, from a full search of ~5,000-entry tables every 4 ms |
| VIF1 UNPACK fast path (patch 0039) | 53.9 % | 32.8 % | processVIF1Data -37 %; 7.7 M UNPACKs verified identical (PS2_VIF_VERIFY=1) |
| Profiler records callers (new baseline: the stack copy slows sampled threads a little) | 53.2 % | 33.4 % | |
| DMA buffers reused, chain data handed over (patch 0040) | 52.3 % | 26.3 % | EE memmove/memchr 760 -> 146 samples, NtFreeVirtualMemory gone |
| EE timers applied at their next event; isSpecialAddress force-inlined (patch 0041) | 53.5 / 53.2 % | 27.1 / 26.7 % | both functions gone from the profile; within noise on this PC |
| Lone XGKICK finished without per-cycle pipeline checks (patch 0042); co-op check lock-free when no session; cutscene-skip check once per frame | 50.0 % | 26.3 % | screenshots within baseline noise |
| Masked UNPACKs on the fast path (patch 0044) | 50.2 % | 25.8 % | all but 3 of 11.8 M UNPACKs fast, verified identical; processVIF1Data 693 -> 512 samples |
| Render thread packet buffers recycled; tapPacket appends without zero-filling | - | - | render-thread mode only (Android); not measured on the PC |
| With --render-thread=1 (as on Android): render thread 18 % | 49.5 % | 25.5 % | its time is mostly the Vulkan driver and log writes; paraLLEl-GS's front end ~4 % |

Fixed along the way: the PC hung at exit with --render-thread=1 (paraLLEl-GS's
open command buffers were never submitted before Device::wait_idle); the
profiler now records callers (16 KB of each sampled stack, unwound after the
thread resumes, under SEH) and lists ten threads.

| VU lambdas force-inlined on MSVC (patch 0046) | 47.5 % | 25.8 % | PC only (clang already inlined them); the Frame still lives in memory under MSVC |

Not changed: the VU1 programs (~45 % of the DMA worker) are cycle-exact AOT
code already; the EE's top costs are the game's own functions.

## Why paraLLEl-GS ends ~12 render passes a frame

`[gs]` splits the texture-hazard ends by cause (texture 8.1, CLUT 0.5,
feedback 0.0 per frame) and `KH2_PGS_HAZARD_LOG=N` logs the first N with the
texture, the texels the draw samples and the pages drawn so far. Almost all
are real read-after-write hazards from KH2's screen effects:

- a 256x208 PSMCT24 buffer at page 320 drawn over itself with textures from
  the same buffer offset by 0-6 pixels (a feedback blur: each draw must see the
  previous one's result);
- a 32x417 strip read from pages drawn earlier in the pass (a post-process).

So the pass count is the game's, not over-cautious tracking; cutting it would
mean restructuring the renderer (a GPU-side split of the pass).
