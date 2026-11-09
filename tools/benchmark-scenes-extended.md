# Extended benchmark scenes

`benchmark-scenes-extended.txt` is a set of 12 MainField scenes in scene format 1, unchanged. Each scene puts the camera on a distinct GPU or render workload without a shrine arrival pad in the frame: Kakariko by day, Hateno and Riverside Stable at night, inside the Hateno Dye Shop, Hyrule Field grass, Korok Plaza's Deku Tree archway, South Akkala autumn foliage, Faron jungle, Zora's Domain plaza in heavy rain, Lake Hylia pier, Lurelin beach and the Kara Kara oasis. The starter set in `benchmark-scenes.txt` is unchanged.

X and Z were picked from object placements on radar.zeldamods.org and visually audited on device screenshots away from enemies and shrine pads. Y is the ground height measured on the device plus ~0.5, so the harness's height check (2 units) holds on flat ground and catches a fall, water or a roof.

Use game 1.9.0 and a normal-mode MainField save past the Great Plateau, with scratch data, save and settings paths (`LARECOMP_DATA_ROOT`, `LARECOMP_SAVE_DIR`, `LARECOMP_SETTINGS`) and `LARECOMP_BENCHMARK=<device path to this file>`. Back up the real profile first. No scene needs a Divine Beast, a completed quest, DLC or protective clothing. All are clear of heat and cold damage, water, cliff edges, shrine pads, guards and story triggers.

## Device validation

Measured on 2026-11-17 with an SM-X730 (MT6991, Mali-G925-Immortalis MC12, Android 16) and the stock Vulkan driver `v1.r49p1-03bet0.f8a98b506b89b21c80a508d70abfcb3f`. The engine was `v0.4.5-beta` (`6113c61a`) with the rebased PR #315 benchmark diagnostics and height-check commit, running recompiled game 1.9.0. Settings: 30 fps cap, 1x render scale, 1080 output, SGSR1, AA off, Original aspect. USB power stayed connected.

The whole file ran in one scratch root. A warm-up pass (one repeat per scene, `12/12 ok`) populated the shader/pipeline cache (`3047 -> 3246` SPIR-V files, `shader_cache_at_start = warm`). The measured pass then ran the file as given: two 45-second windows per scene and `thermal_max = 0`. All 24 windows returned `ok`. Link stayed within 0.52 units of each scene's Y and moved 0.0 units in every window.

GPU p50 values were recomputed from `frames.csv` and match `report.json`. The spread is `(max - min) / median * 100` over the two repeats. It is the range of two observations, not a confidence interval. These figures are for this device only, not thresholds for other devices.

| Scene | Workload | GPU p50, repeats 0 / 1 (ms) | Spread | Flags |
|---|---|---:|---:|---|
| `kakariko-day` | Kakariko at 09:00: houses, lanterns, trees, villagers. Heavy render thread (`20.36 ms`). | 28.376 / 28.375 | 0.00% | C,T |
| `hateno-night` | Hateno at 21:30: street lamps, lit windows, night lighting. | 27.256 / 27.182 | 0.27% | C,T |
| `riverside-stable-night` | Riverside Stable at 22:00: tent, lamps, a campfire in the open at night. | 27.227 / 27.233 | 0.02% | C,T |
| `hateno-dye-interior` | Inside Hateno Dye Shop at 12:00: beams, cloth banners, lamps, jars, NPC. | 27.271 / 27.122 | 0.55% | C,T |
| `field-grass` | Hyrule Field at 13:00: grass to the horizon, a large tree. | 26.517 / 26.496 | 0.08% | C |
| `korok-plaza` | Korok Plaza at 12:00: Deku Tree archway, volumetric sunbeams, fog, dense grass. | 27.915 / 27.595 | 1.16% | C |
| `akkala-foliage` | South Akkala at 08:00: autumn foliage, drifting leaf particles, ground decals. | 26.972 / 27.050 | 0.29% | C,T |
| `faron-jungle` | Faron jungle at 12:00: palm trunks, dense fern understory, layered foliage alpha. | 27.696 / 27.918 | 0.80% | C,T |
| `zora-plaza` | Zora's Domain plaza at 10:00 in heavy rain: wet flagstones, luminous Zora stone, fountain. | 28.242 / 27.395 | 3.05% | S,C,T |
| `lake-hylia-shore` | Lake Hylia pier at 17:00: sunlit lake water, raft, Bridge of Hylia pillars. | 27.139 / 27.224 | 0.31% | C,T |
| `lurelin-shore` | Lurelin beach at 07:00: sea bay, docks, palms, campfire, NPC (`Take` prompt). | 26.984 / 26.881 | 0.38% | T |
| `karakara-oasis` | Kara Kara at 06:30: oasis pond, palms, campfire, NPCs (`Talk` prompt). | 26.823 / 26.888 | 0.24% | C |

Flags:

- **S**: the GPU p50 spread is above 2%.
- **C**: a measured window contains shader compilation or skipped draws.
- **T**: a measured window began above thermal status 0.

## Caveats

- **30 fps cap compresses GPU differences on Mali DVFS:** Under `frame_rate = 30`, Mali-G925 scales clocks to the 33.33 ms vsync budget, keeping all 12 scenes within `26.50–28.38 ms` GPU p50 even while `render_thread_ms` p50 spans `13.50–20.36 ms`. Compare GPU optimization PRs at 60 fps or uncapped when evaluating true GPU headroom.
- **Kakariko villagers & Zora rain shaders can vary across runs:** `kakariko-day` showed `0.00%` spread here (but exceeded `3–5%` in earlier 11-scene runs as villagers walked through the camera), while `zora-plaza` showed `3.05%` here (`45` skipped builds in repeat 0 vs `0` in repeat 1, and `28.58 fps` vs `30.00 fps` on `kakariko-day` repeat 1). Compare across warm runs.
- **Some late shader misses occur past warmup:** Even after a warm-up pass, wandering NPC animations and distant weather particles trigger occasional late compiles (`C` in 11/12 scenes; `lurelin-shore` had `0` compiles and `0` skipped builds).
- **The cooling gate only applies at the title screen:** `thermal_max = 0` waits for status 0 at the title before every repeat; load and warmup can raise thermal status to `1` before the window opens (**T** rows).
- **The settle check reads the spawn height:** `y_settle` records the placed height; `y_window_start` and `y_window_end` verify Link stays within 2 units throughout the window (`<= 0.52` units delta across all 24 windows).

## Coverage boundaries

- **Night spawns:** No hearts were lost and no enemy appeared in any night-scene screenshot.
- **Rain and storms:** `zora-plaza` uses heavy rain (`weather = 3`), not a thunderstorm (lightning depends on metal gear).
- **Indoor lighting:** Covered by one MainField room (`hateno-dye-interior`, Hateno Dye Shop); shrine interiors require a stage change that format 1 cannot describe.
- **Static camera (`moved = 0.0`):** Because Link stands still, dynamic clipmap scrolling and streaming texture uploads are largely idle outside NPC motion and weather particles.
- **Saves & regions:** One post-Plateau save was used. Not covered: snow (`weather = 5` requires cold gear or Hebra stages outside mild areas), sandstorms, Death Mountain lava summit, Gerudo Town and Divine Beast interiors.

The raw reports, frame logs and screenshots stay local. This contribution contains no Nintendo files, screenshots or saves.
