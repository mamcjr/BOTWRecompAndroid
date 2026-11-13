# License of this folder: GPL-3.0-or-later

The files here are Lossless Scaling's frame generation (LSFG) for BOTWRecompAndroid's present. `lsfg_chain.c`, `lsfg_dll.c`,
`lsfg_pacer.c` and `lsfg.h` come from Swap2-Native (`dm/gfx/vk_android/lsfg`), a C port of [Eden](https://git.eden-emu.dev/eden-emu/eden)'s
implementation (`src/video_core/frame_gen/{lossless_dll,lsfg_translate}.cpp`, `src/video_core/renderer_vulkan/present/{frame_gen_pacer,lsfg_*}.cpp`),
which builds on [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk). All are licensed GPL-3.0-or-later, and so is this folder: each file
keeps its SPDX copyright lines (Eden Emulator Project 2026, lsfg-vk 2025). The full license text is [COPYING](COPYING).

Changes for BOTWRecompAndroid: the device, queue, memory types, barriers and log come from `lsfg_host.h` (this renderer's
`vk.h` and volk) instead of Swap2-Native's `vka.h`; `lsfg_present.c` is new (after Swap2-Native's `vka_framegen.c`): it feeds
the game's final image to the chain and hands the generated frames to the present (`../device.c`).

The rest of BOTWRecompAndroid is MIT ([LICENSE](../../../../LICENSE)). The Android app links this folder by default
(CMake option `BOTW_LSFG`), and an APK built with it is a combined work under **GPL-3.0-or-later as a whole**; see
[NOTICE.md](../../../../NOTICE.md). Built with `-DBOTW_LSFG=OFF`, none of this folder is in it (`../nolsfg/lsfg_stub.c` stands in).

The shaders are not here and never are: they are read at run time from the user's own copy of `Lossless.dll` (Lossless
Scaling, proprietary), which is not part of this repository or of any APK.
