# Notice

BOTWRecompAndroid's source is MIT ([LICENSE](LICENSE)), except the folder below.

**The Android build that includes frame generation is GPL-3.0-or-later.** By default the app links Lossless Scaling frame
generation (LSFG), in [`runtime/nvn/vulkan/lsfg`](runtime/nvn/vulkan/lsfg/LICENSE.md): a C port, by Swap2-Native, of
[Eden](https://git.eden-emu.dev/eden-emu/eden)'s implementation (itself based on [lsfg-vk](https://github.com/PancakeTAS/lsfg-vk)), licensed
GPL-3.0-or-later. An APK built with it is therefore distributed as a whole under the GNU General Public License, version 3
or later ([COPYING](runtime/nvn/vulkan/lsfg/COPYING)); whoever passes such an APK on must offer its complete source (this
repository at that commit). The source files outside that folder stay MIT on their own.

To build an APK without it: CMake `-DBOTW_LSFG=OFF` (the app then reports frame generation as "not in this build").

`Lossless.dll` (Lossless Scaling for Windows, proprietary) is never in this repository or in any APK: the player supplies
their own copy, and the app reads its shaders at run time.

**Ideas and layouts from [totk-android](https://github.com/AurelioB/totk-android)** (AurelioB, MIT): the 3D reverb and the
auxiliary buffer in `runtime/services/audio_reverb3d.inc` are adapted from its `games/totk/runtime/audio_reverb.c` and
`audio_mixer.c` (Copyright (c) 2026 MacNativeGames contributors, MIT), whose reverb network is based on Ryujinx.Audio's
Reverb3d (Copyright (c) Ryujinx Team and Contributors, MIT).

**From [peanuts-native](https://github.com/igawa6/peanuts-native)** (MIT, Copyright (c) seedds, Swap2-Native contributors,
EddizEge): the lock test in `runtime/native.c` (`lock_without_acquire`, native mode) is a C port of its
`dm/tools/patch_nso.py` `acquire_lock_sites`.

**From [Savage Wind Recomp](https://github.com/Revivedx/SavageWindRecomp)** (MIT, Copyright (c) 2026 Revivedx and the
Savage Wind Recomp contributors; Copyright (c) 2026 Peiikai): the low bits of 16 / 8-bit integer colour targets on Mali
(`rt_kind_key` in `runtime/nvn/vulkan/exec.c` and the narrow outputs in `runtime/nvn/maxwell/translate.c`) follow its
commit 9950b9e (branch `mali-fixes`, StonedEdge).

**From Swap2-Native's Banana Native** (MIT): the write-fault run in `runtime/platform/android.c` (`wt_frun`) follows its
`fault_run` (6624426f).
