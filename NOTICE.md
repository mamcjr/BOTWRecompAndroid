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
