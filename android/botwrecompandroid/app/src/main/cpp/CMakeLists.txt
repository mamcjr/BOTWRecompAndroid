# libmain.so: the BOTWRecompAndroid runtime for Android, with the
# Vulkan backend and the ImGui UI. The game code is not in it: botwgame.so, generated with gen.py --dyn-base and built
# from the player's own game files, is loaded at run time (docs/las/ANDROID-PORT.md).
cmake_minimum_required(VERSION 3.22)
project(botwrecompandroid C CXX ASM)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)   # the repository
get_filename_component(R ${R} ABSOLUTE)
set(DEPS ${R}/android/deps/out)                        # android/deps/build_deps.sh
if(NOT EXISTS ${DEPS}/lib/libSDL3.so)
    message(FATAL_ERROR "run android/deps/build_deps.sh first")
endif()
if(NOT BOTW_GEN_DIR)
    message(FATAL_ERROR "BOTW_GEN_DIR (the Gradle build's generated files: smaa_hlsl.inc) is not set")
endif()

include(${R}/android/gamelib_id.cmake)  # (GAMELIB_ID: the same as botwgame.so's, android/gamelib)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

add_subdirectory(${R}/third_party/libadrenotools ${CMAKE_BINARY_DIR}/adrenotools)

add_library(SDL3 SHARED IMPORTED)
set_target_properties(SDL3 PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libSDL3.so)
add_library(shaderc_shared SHARED IMPORTED)
set_target_properties(shaderc_shared PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/libshaderc_shared.so)

file(GLOB RT_SRC CONFIGURE_DEPENDS ${R}/runtime/*.c ${R}/runtime/hle/*.c ${R}/runtime/services/*.c ${R}/runtime/nvn/*.c
     ${R}/runtime/nvn/maxwell/*.c)
# FFmpeg (android/deps/build_ffmpeg.sh): the game's movies and the start screen's background; without it, neither
set(HAVE_FFMPEG FALSE)
if(EXISTS ${DEPS}/lib/libavformat.so)
    set(HAVE_FFMPEG TRUE)
    foreach(l avcodec avformat avutil swscale)
        add_library(${l} SHARED IMPORTED)
        set_target_properties(${l} PROPERTIES IMPORTED_LOCATION ${DEPS}/lib/lib${l}.so)
    endforeach()
else()
    list(REMOVE_ITEM RT_SRC ${R}/runtime/hle/movie.c)
endif()
file(GLOB VK_SRC CONFIGURE_DEPENDS ${R}/runtime/nvn/vulkan/*.c ${R}/runtime/nvn/vulkan/*.cpp)
# Frame generation (Lossless Scaling's LSFG, runtime/nvn/vulkan/lsfg, GPL-3.0-or-later: an APK built with it is
# GPL-3.0-or-later as a whole - NOTICE.md). -DBOTW_LSFG=OFF (Gradle -PbotwLsfg=false): left out, a stub in its place.
option(BOTW_LSFG "Lossless Scaling frame generation (GPL-3.0-or-later)" ON)
if(BOTW_LSFG)
    file(GLOB LSFG_SRC CONFIGURE_DEPENDS ${R}/runtime/nvn/vulkan/lsfg/*.c)
    list(APPEND VK_SRC ${LSFG_SRC})
else()
    list(APPEND VK_SRC ${R}/runtime/nvn/vulkan/nolsfg/lsfg_stub.c)
endif()
file(GLOB UI_SRC CONFIGURE_DEPENDS ${R}/runtime/ui/*.mm)
set(IMGUI ${R}/third_party/imgui)
set(IMGUI_SRC ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp ${IMGUI}/backends/imgui_impl_vulkan.cpp)

add_library(main SHARED ${RT_SRC} ${R}/runtime/platform/android.c ${VK_SRC} ${UI_SRC} ${IMGUI_SRC}
            ${DEPS}/volk/volk.c ${R}/android/common/guest_window.c ${R}/runtime/native_gate.S)
# the UI sources are plain C++ apart from __APPLE__ blocks (compiled as C++ on Android)
set_source_files_properties(${UI_SRC} PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-xc++")
set_source_files_properties(${R}/runtime/gamelib.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
set_source_files_properties(${R}/runtime/gamebuild.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
# the benchmark's report names the commit this build came from (asked on every build, benchmark_commit.cmake)
add_custom_target(benchmark_commit BYPRODUCTS ${CMAKE_BINARY_DIR}/benchmark/benchmark_commit.h
    COMMAND ${CMAKE_COMMAND} -DR=${R} -DOUT=${CMAKE_BINARY_DIR}/benchmark/benchmark_commit.h -P ${CMAKE_CURRENT_LIST_DIR}/benchmark_commit.cmake VERBATIM)
add_dependencies(main benchmark_commit)
target_include_directories(main PRIVATE ${CMAKE_BINARY_DIR}/benchmark)

target_include_directories(main PRIVATE ${R}/runtime ${R}/runtime/nvn ${IMGUI} ${IMGUI}/backends ${DEPS}/include
                           ${DEPS}/volk ${BOTW_GEN_DIR} ${R}/third_party/smaa/Textures ${R}/android/common)
target_compile_definitions(main PRIVATE
    VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR IMGUI_IMPL_VULKAN_USE_VOLK   # Vulkan from the loaded driver (volk)
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
if(HAVE_FFMPEG)
    target_link_libraries(main PRIVATE avcodec avformat avutil swscale)
else()
    target_compile_definitions(main PRIVATE LARECOMP_NO_FFMPEG)
endif()
# -O2 -g whatever the Gradle build type (the Android runtime optimization flags): a debug build left the runtime at -O0, about half of the
# game thread's time
target_compile_options(main PRIVATE -O2 -g -fno-strict-aliasing -Wall -Wno-unused-function -Wno-format  # (as on Linux: %llx for uint64_t)
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>)
target_link_libraries(main PRIVATE SDL3 shaderc_shared adrenotools android log dl m)
# (runtime/hle.h, ipc.h, main.c: tables in named sections found through __start_ / __stop_ symbols)
target_link_options(main PRIVATE -Wl,-z,nostart-stop-gc)
