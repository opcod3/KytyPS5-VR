# PSVR2: command-processor bottleneck fixes

The game runs at 5–7 fps. This note explains where that time goes and the three fixes proposed.

## How we got here

Three Tracy captures, each adding zones where the previous one left time unaccounted for.

1. **Capture 1 (`horizon_profile.tracy`)**: the GPU thread runs `CommandProcessor::Process` and was busy **98%** of the time between guest flips. That is about 170 ms per frame. The XR thread holds a steady 60 Hz and `PresentHmd` takes about 0.8 ms. So the frame rate is limited by CPU time on the command-processor thread, not by the GPU or presentation.
   - There are about 4,570 indexed draws per frame, costing 33 µs each.
   - Most of that was time with no zone covering it: `DrawIndex` self time plus `CommitBindings` self time.
   - The cost is bimodal. Early-frame draws take about 0.65 µs; the rest take about 19 µs.
   - Tiny CPU-only zones such as `ResolveRenderDepthTarget` cost the same in both groups, which rules out CPU contention. Real work on a code path only some draws take was the cause.
2. **Capture 2**: zones throughout the draw path. Two stood out:
   - `PipelineCache::GetGraphicsPrograms`: 17 µs per draw.
   - `CommitBindings::CheckSpecializationReads`: 5 µs per call.

   These zones were small and ruled out:
   - waiting on the render mutex
   - descriptor updates and pushes
   - image transitions
3. **Capture 3**: zones inside `ProgramCache::Get`, plus Tracy plots of the sizes involved.
   - Cache lookup, permutation search and compilation are close to zero. Every shader has exactly one permutation.
   - Almost all the time is `MaterializeResources`, which re-runs on every cache **hit**.

## Capture 3, per frame (~183 ms on the CP thread, 5.5 fps)

| Zone | ms/frame | per call |
|---|---|---|
| `ProgramCache::MaterializeResources` (pixel shaders) | **38** | 10.9 µs |
| `CommitBindings::CheckSpecializationReads` | **33** | 5.1 µs |
| `ResolveRenderColorTarget` | 24 | 1.3 µs, 3.6 per draw |
| `PrepareGraphicsBindings` + `NativeStorageBuffer` + `ResolveTexture` | ~30 | |
| `ProgramCache::MaterializeResources` (vertex shaders) | 11 | 2.2 µs |
| Program lookup, permutation search, compilation | ~0 | |

Plots:
- Tracked guest-memory reads (`specialization_reads`) per shader: median 15, p90 149, max 269.
- Bound images per draw: average 18, max 33.

## Fix 1: skip `MaterializeResources` when nothing changed

**Why:** on every cache hit, `ProgramCache::Get` (`pipelineCache.cpp`) re-walks the shader's resource tables (the SRT) in guest memory from scratch. That can mean up to ~270 reads, each going through `TryReadBufferBacking`. Each read checks the GPU address range, queries the buffer cache's dirty-byte tracking, then reads from guest memory. Across about 4,900 draws, that is 49 ms per frame.

**How:** the result depends only on the shader's user data and the bytes behind the tracked reads. Keep both from the last run. On a hit, compare the user data, then re-read only those ranges and compare them with the saved bytes. If everything matches, reuse the previous snapshot and specialization instead of walking the tables again.

**Risk:**
- This is the correctness-sensitive one. Each check still has to trigger the GPU-dirty readback, the same as a real read.
- It only applies to plans that track their reads (`capture_specialization_reads`); other plans keep the full walk.
- Compute shaders also depend on `workgroup_counts`, so those must be part of the comparison.

## Fix 2: sort-and-sweep overlap check

**Why:** `CheckSpecializationReads` (inside `CommitBindings`) does two sets of comparisons:
- every tracked read (about 61 on average) against every bound image's three written ranges, fetching the image from the slot table again on every iteration;
- the reads against every written buffer.

That is about 3,000+ overlap tests per draw, or 33 ms per frame, just to check something that is almost never true.

**How:**
- Sort the reads and merge them into intervals. They are mostly neighbouring words in the same tables, so they should collapse to a handful.
- Collect the image and buffer write ranges once per draw and sort them.
- Do one pass over both sorted lists.

The result is the same, at a fraction of the cost. This is low-risk and self-contained.

## Fix 3: cache `ResolveRenderColorTarget`

**Why:** it costs 1.3 µs per call but runs about 3.6 times per draw. The callers are the special-operation checks and `PrepareDrawRenderState`, and each call re-derives the same target from unchanged CB registers. Together with its `TileGetTextureSize` calls, that is about 24 ms per frame.

**How:** keep the result for each slot, keyed on the render-target registers and the slice offset. Drop it when those registers or the texture cache's image state change.

## Plan

- Order: fix 2 first (smallest, safest), then fix 1 (biggest win), then fix 3.
- Expected saving: about 70–90 ms of the ~180 ms frame.
- After each fix, recapture the same scene with the current zones and compare per-frame totals.
- Remove the extra zones once the fixes have landed.
