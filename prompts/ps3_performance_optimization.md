# PS3 performance optimization

Optimize the Extreme Tux Racer PS3 port for real hardware, concentrating on
the terrain and object rendering paths. Preserve rendering correctness and
measure each optimization independently. Explore the codebase to answer
implementation questions rather than asking the user when the answer is
available locally.

## Current evidence

### 1. The PS3 build is unoptimized

`src/Makefile.ps3` currently supplies no optimization level. The effective PPU
compile command is therefore `-O0`, despite compiling recursive quadtree code,
physics, per-index decoding, and many small rendering helpers.

The PS3 build now enables `-O2` while retaining useful warnings and debug
symbols. It deliberately does not use `-ffast-math`, which could alter physics
behavior. Measure this change separately before restructuring rendering.

### 2. Terrain is de-indexed on the PPU

`quadsquare::DrawTris()` generates a 32-bit index list and calls
`glDrawElements`, but the PS3 implementation in `src/ps3/source/ps3_gl.cpp`
does not submit indexed geometry. It currently:

- reads every index on the PPU;
- fetches position, normal, and color components individually;
- expands each index into a 48-byte `ImmVtx`;
- duplicates shared vertices for every triangle;
- copies expanded vertices into RSX memory;
- splits terrain into approximately 510-vertex batches.

Each batch repeats render-state setup, texture configuration, shader and
uniform uploads, vertex-cache invalidation, label submission, and
`rsxFlushBuffer`.

The preferred long-term solution is a native indexed RSX terrain path:

- upload static terrain vertex attributes once;
- upload only changing index lists or material data each frame;
- submit with `rsxDrawIndexArray`;
- retain 32-bit indices because large courses exceed 65,535 vertices;
- fence dynamic buffers correctly so the PPU never overwrites in-flight RSX
  data.

As a lower-risk intermediate step, `PS3_VTX_SLOT_MAX` and its corresponding
ring allocation have been increased from 512 to 4096 vertices. Triangle
batches now contain up to 4095 vertices while remaining on the fenced ring
path; sending a batch larger than a ring slot would take the oversize path,
which explicitly waits for RSX idle. Cache unchanged RSX state and texture
bindings where safe.

### 3. Terrain is traversed once per loaded material

`quadsquare::Render()` calls `RenderAux()` independently for every terrain
texture loaded by the course. Each pass repeats quadtree traversal, frustum
checks, vertex lookup, index-list construction, and shared alpha writes.

Bronze Set is a 100 x 3000 grid: 300,000 source vertices. Its terrain map
resolves to six loaded materials (`snow`, `ice1`, `rock`, `rock04`, `rock06`,
and `rock01`). Bunny Hill is 90 x 260, or 23,400 vertices, and resolves to
three materials. Visibility culling prevents the entire grid from being sent
each frame, but the larger tree and six material traversals still create much
more PPU and cache work.

Potential improvements, in increasing order of complexity:

1. Store a terrain-material bit mask on every quadtree node and reject
   branches that cannot contribute to the current pass.
2. Generate all per-material index lists during one visible-quadtree
   traversal, then bind and draw each nonempty list.
3. Consider a PS3-specific material/shader strategy that reduces terrain
   passes without increasing fragment overdraw excessively.

Preserve the existing PS3 workaround that disables only the legacy
three-terrain additive junction pass. Re-enabling it caused black/white
triangle flashes on RSX.

### 4. Trees and items generate excessive draw calls

`DrawTrees()` scans the complete `Course.CollArr` and `Course.NocollArr` every
frame, performs only Z-distance rejection, and submits one draw call per
visible tree or item. Bronze Set's object map contains roughly 8,300 objects.

The configured `tree_detail_distance` is currently unused; tree clipping uses
the general 75-unit forward and 20-unit backward course clip distances.

Optimize this path by:

- spatially partitioning or sorting objects so only the visible Z range is
  scanned;
- adding actual frustum and lateral rejection;
- applying `tree_detail_distance`;
- batching visible tree billboards by texture;
- batching herrings and other compatible billboard items;
- avoiding a full shader/state upload per individual object.

### 5. The generic fragment shader is expensive for terrain

`src/ps3/shaders/etr3d.fcg` performs per-fragment normalizations, half-vector
construction, and a specular `pow()` whenever lighting is enabled. Terrain
uses a black specular material, but it still takes the general lighting path.

Evaluate a dedicated terrain shader that performs directional diffuse
lighting per vertex, interpolates the result, omits specular work, and retains
texture modulation, vertex alpha, and fog. Compare visuals before accepting
the change.

### 6. Conservative PS3 quality defaults

Current defaults include:

- `course_detail_level = 75`
- `forward_clip_distance = 75`
- `backward_clip_distance = 20`
- `perf_level = 3`

After no-visual-change optimizations, test PS3-specific defaults around:

- course detail 30-40;
- forward clip 50-60;
- tree distance 20-30;
- reduced snow, track marks, and particles at lower performance levels.

These settings have visible consequences and should not be the first fix.
Remember that an existing `/dev_hdd0/game/EXTR00001/USRDIR/options.txt` retains
old values unless explicitly migrated or overridden.

## Implementation order

1. Measure the completed `-O2` and 4096-vertex ring changes on real hardware.
2. Add lightweight counters/timers for CPU frame time, terrain indices,
   terrain batches, quadtree material passes, and object draw calls. Do not use
   per-frame TTY logging; aggregate results or expose them in the HUD.
3. Eliminate redundant RSX state submission now that the vertex-ring batch
   size has been increased.
4. Spatially cull and batch trees/items.
5. Implement native indexed RSX terrain submission.
6. Collapse or prune repeated per-material quadtree traversals.
7. Add a lightweight terrain shader.
8. Tune PS3 quality defaults only after measuring the preceding changes.

## Existing related changes

- The PS3 display preference is being changed to 720 x 480 for NTSC or 720 x
  576 for PAL, with higher modes as fallbacks. HDMI will use 480p/576p; analog
  outputs may use interlaced SD.
- `sysTtyTrace` is disabled centrally in `src/ps3/include/ps3_tty.h`. Its calls
  were startup/error-only and were not a meaningful frame-rate cost.
- The problematic three-terrain additive junction pass remains disabled on
  PS3 while ordinary two-terrain blending remains active.

## Verification requirements

- Build with `make -f Makefile.ps3` from `src/` after every isolated change.
- Test Bronze Set and at least one small three-material course.
- Check terrain seams, mixed-material boundaries, mirrored courses, fog,
  lighting, track marks, trees, and collectible billboards.
- Confirm that 32-bit terrain indices above 65,535 render correctly.
- Watch for RSX ring-buffer overwrite artifacts, flashes, or stalls.
- Compare real-hardware frame time and draw/batch counts before and after each
  optimization.
- Preserve unrelated user changes and commit optimizations in reviewable,
  independently testable steps.
