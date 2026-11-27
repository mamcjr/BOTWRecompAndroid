# botwgame.so for Android: the game code generated from the player's own game files with gen.py --dyn-base
# (android/build_gamelib.sh runs the generator and this build). The runtime functions it calls are libmain.so's: it
# names libmain.so as a dependency, which is how Android's linker finds them when the runtime loads it (dlopen).
#   LAS_GEN: the generated code (sdk/, main/)   LAS_LIBMAIN: the app's libmain.so (to link against)
cmake_minimum_required(VERSION 3.22)
project(botwgame C)

set(R ${CMAKE_CURRENT_SOURCE_DIR}/../..)
get_filename_component(R ${R} ABSOLUTE)
include(${R}/android/gamelib_id.cmake)
message(STATUS "GAMELIB_ID ${GAMELIB_ID}")

file(GLOB GEN_SRC ${LAS_GEN}/main/*.c ${LAS_GEN}/sdk/*.c ${LAS_GEN}/subsdk*/*.c)  # (subsdk*: code mods, runtime/codemods.h)
list(LENGTH GEN_SRC n)
if(n EQUAL 0)
    message(FATAL_ERROR "no generated code in ${LAS_GEN}")
endif()
if(NOT EXISTS ${LAS_LIBMAIN})
    message(FATAL_ERROR "LAS_LIBMAIN (the app's libmain.so) not found: ${LAS_LIBMAIN}")
endif()

add_library(main SHARED IMPORTED)
set_target_properties(main PROPERTIES IMPORTED_LOCATION ${LAS_LIBMAIN} IMPORTED_NO_SONAME FALSE)

add_library(botwgame SHARED ${GEN_SRC} ${R}/tools/gamelib_entry.c)
target_include_directories(botwgame PRIVATE ${R}/runtime)
target_compile_options(botwgame PRIVATE ${GAMELIB_FLAGS})
set_source_files_properties(${R}/tools/gamelib_entry.c PROPERTIES COMPILE_DEFINITIONS "GAMELIB_ID=\"${GAMELIB_ID}\"")
# Files with a gigantic function (heavy.txt, android/build_gamelib.sh: over 15,000 labels - a jump table's function
# gets one per instruction) at -O0: clang spent hours on each at -O1 / -O2 (v1.6.0), seconds at -O0
if(EXISTS ${LAS_GEN}/heavy.txt)
    file(STRINGS ${LAS_GEN}/heavy.txt HEAVY)
    foreach(f ${HEAVY})
        set_source_files_properties(${LAS_GEN}/${f} PROPERTIES COMPILE_OPTIONS "-O0")
    endforeach()
endif()
target_link_libraries(botwgame PRIVATE main)
set_target_properties(botwgame PROPERTIES PREFIX "")   # botwgame.so (runtime/gamelib.h GAMELIB_NAME)
file(WRITE ${CMAKE_BINARY_DIR}/botwgame.so.id "${GAMELIB_ID}\n")
