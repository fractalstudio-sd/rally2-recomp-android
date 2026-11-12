cmake_minimum_required(VERSION 3.16)
project(srally2_native C CXX)
set(M3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
if(ANDROID)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# The lifted guest code is produced by scripts/setup.sh from YOUR ROMs and is never committed.
set(M3_GENERATED_DIR "${M3_ROOT}/build/out" CACHE PATH "directory with recomp_funcs_*.c, recomp_dispatch.c, recomp.h")
# Native translator: no lifted code at all in the build; the program is translated to AArch64 on the device from the
# user's own ROM when the game starts (xlat/).
option(M3_XLAT "translate the game program on the device (AArch64 / ARM) instead of linking lifted C" OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(XL_SRC ${M3_ROOT}/xlat/frontend.cpp ${M3_ROOT}/xlat/backend_a64.cpp ${M3_ROOT}/xlat/runtime.cpp)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
  set(XL_SRC ${M3_ROOT}/xlat/frontend.cpp ${M3_ROOT}/xlat/backend_a32.cpp ${M3_ROOT}/xlat/runtime.cpp)
endif()
if(NOT M3_XLAT AND NOT EXISTS "${M3_GENERATED_DIR}/recomp_dispatch.c")
  message(FATAL_ERROR "No lifted code in ${M3_GENERATED_DIR}. Run scripts/setup.sh /path/to/srally2.zip first.")
endif()

option(M3_GLES "Render through OpenGL ES 3.2 instead of desktop GL 4.1 (Android, or Mesa GLES for testing)" OFF)
option(M3_HEADLESS "maintainers: build only dev/headless (no video/audio/SDL; lockstep tests, also under qemu-user)" OFF)
if(NOT ANDROID AND NOT M3_HEADLESS)
  find_package(SDL2 REQUIRED)
endif()
if(NOT M3_GLES AND NOT M3_HEADLESS)
  find_package(OpenGL REQUIRED)
endif()
find_package(Threads REQUIRED)

set(HW ${M3_ROOT}/runtime/hw)

# ---- Musashi 68000 core: its opcode tables are generated at build time by its own m68kmake tool
set(MUSASHI_DIR ${HW}/CPU/68K/Musashi)
set(MUSASHI_GEN ${CMAKE_BINARY_DIR}/musashi)
file(MAKE_DIRECTORY ${MUSASHI_GEN})
# When cross-compiling (Android) the generator cannot run on the build machine's target: pass tables generated on the
# host with -DM3_MUSASHI_PREGEN=<dir containing m68kops.c m68kops.h m68kopac.c m68kopdm.c m68kopnz.c>.
set(M3_MUSASHI_PREGEN "" CACHE PATH "pre-generated Musashi tables (cross builds)")
if(M3_MUSASHI_PREGEN)
  file(COPY ${M3_MUSASHI_PREGEN}/ DESTINATION ${MUSASHI_GEN} FILES_MATCHING PATTERN "m68kop*.[ch]")
else()
  add_executable(m68kmake ${MUSASHI_DIR}/m68kmake.c)
  set_target_properties(m68kmake PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${MUSASHI_GEN})
  target_compile_options(m68kmake PRIVATE -w)
  add_custom_command(
    OUTPUT ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kops.h ${MUSASHI_GEN}/m68kopac.c ${MUSASHI_GEN}/m68kopdm.c ${MUSASHI_GEN}/m68kopnz.c
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${MUSASHI_DIR} ${MUSASHI_GEN}/src
    COMMAND m68kmake ${MUSASHI_GEN} ${MUSASHI_GEN}/src/m68k_in.c
    DEPENDS m68kmake ${MUSASHI_DIR}/m68k_in.c
    WORKING_DIRECTORY ${MUSASHI_GEN})
endif()
add_library(musashi STATIC
  ${MUSASHI_DIR}/m68kcpu.c ${MUSASHI_DIR}/m68kdasm.c
  ${MUSASHI_GEN}/m68kops.c ${MUSASHI_GEN}/m68kopac.c ${MUSASHI_GEN}/m68kopdm.c ${MUSASHI_GEN}/m68kopnz.c)
target_include_directories(musashi PRIVATE ${MUSASHI_GEN} ${MUSASHI_DIR} ${HW}/CPU/68K ${HW} ${HW}/OSD)
target_compile_options(musashi PRIVATE -w)
if(ANDROID)
  # bionic declares 'typedef unsigned int uint' in <sys/types.h>; Musashi #defines uint first. Include it before.
  target_compile_options(musashi PRIVATE -include sys/types.h)
endif()

# ---- lifted guest (C)
file(GLOB LIFTED_SRC ${M3_GENERATED_DIR}/recomp_funcs_*.c)
if(M3_XLAT AND NOT M3_HEADLESS)
  add_library(guest STATIC ${XL_SRC} ${M3_ROOT}/xlat/guest/hooks.c ${M3_ROOT}/runtime/app/menu_guest.c ${M3_ROOT}/runtime/app/leaderboard_guest.c)
  target_include_directories(guest BEFORE PRIVATE ${M3_ROOT}/xlat/guest)
  target_include_directories(guest PRIVATE ${M3_ROOT}/xlat ${M3_ROOT}/runtime)
  target_compile_options(guest PRIVATE -w -O2)
elseif(NOT M3_HEADLESS)
# a few lifted functions are replaced by the runtime (touch menus): route them, idempotent
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} -I ${M3_ROOT}/tools/apply_overrides.py ${M3_GENERATED_DIR} RESULT_VARIABLE M3_OVR_RES)
else()
  execute_process(COMMAND python3 -I ${M3_ROOT}/tools/apply_overrides.py ${M3_GENERATED_DIR} RESULT_VARIABLE M3_OVR_RES)
endif()
if(NOT M3_OVR_RES EQUAL 0)
  message(FATAL_ERROR "tools/apply_overrides.py failed on ${M3_GENERATED_DIR}")
endif()
add_library(guest STATIC ${LIFTED_SRC} ${M3_GENERATED_DIR}/recomp_dispatch.c ${M3_ROOT}/runtime/app/menu_guest.c ${M3_ROOT}/runtime/app/leaderboard_guest.c)
target_include_directories(guest PRIVATE ${M3_GENERATED_DIR} ${M3_ROOT}/runtime)
target_compile_options(guest PRIVATE -w -O1)
endif()

# ---- headless lockstep harness (dev/headless): same hardware sources, no renderer / audio device / SDL
if(M3_HEADLESS)
  file(GLOB HL_SRC
    ${HW}/Model3/*.cpp ${HW}/Model3/DriveBoard/*.cpp
    ${HW}/Sound/SCSP.cpp ${HW}/Sound/SCSPDSP.cpp ${HW}/Sound/MPEG/*.cpp
    ${HW}/CPU/68K/68K.cpp ${HW}/CPU/Z80/Z80.cpp
    ${HW}/OSD/Logger.cpp ${HW}/OSD/Outputs.cpp ${HW}/OSD/Thread.cpp
    ${HW}/Util/*.cpp ${HW}/Inputs/*.cpp ${HW}/BlockFile.cpp ${HW}/ROMSet.cpp)
  list(FILTER HL_SRC EXCLUDE REGEX "53C810Disasm")
  add_executable(m3_headless ${HL_SRC} ${M3_ROOT}/runtime/cpu_glue.cpp ${M3_ROOT}/runtime/app/game_srally2.cpp ${M3_ROOT}/runtime/app/rom_images.cpp
    ${M3_ROOT}/dev/headless/headless_main.cpp ${M3_ROOT}/dev/headless/stubs.cpp ${M3_ROOT}/dev/headless/shim_sdl.cpp)
  target_include_directories(m3_headless BEFORE PRIVATE ${M3_ROOT}/dev/headless/shim ${M3_ROOT}/runtime/gles_shim)
  target_include_directories(m3_headless PRIVATE ${HW} ${HW}/OSD ${HW}/Inputs ${HW}/Network ${HW}/Pkgs ${HW}/Graphics ${HW}/Graphics/New3D
    ${M3_ROOT}/runtime ${M3_ROOT}/dev/headless ${MUSASHI_GEN})
  target_compile_definitions(m3_headless PRIVATE SUPERMODEL_UNIX M3_GLES=1 M3_HEADLESS=1)
  target_compile_options(m3_headless PRIVATE -w)
  # reference: the lifted C exactly as the lifter wrote it (no runtime overrides)
  add_library(guest_ref STATIC ${LIFTED_SRC} ${M3_GENERATED_DIR}/recomp_dispatch.c)
  target_include_directories(guest_ref PRIVATE ${M3_GENERATED_DIR} ${M3_ROOT}/runtime)
  target_compile_options(guest_ref PRIVATE -w -O1)
  target_link_libraries(m3_headless PRIVATE guest_ref musashi Threads::Threads)
  # native translator (AArch64 hosts): same harness, guest code translated at start-up
  if(XL_SRC)
    get_target_property(HL_SOURCES m3_headless SOURCES)
    add_executable(m3_headless_native ${HL_SOURCES} ${XL_SRC})
    get_target_property(HL_INC m3_headless INCLUDE_DIRECTORIES)
    target_include_directories(m3_headless_native BEFORE PRIVATE ${HL_INC})
    target_include_directories(m3_headless_native PRIVATE ${M3_ROOT}/xlat)
    target_compile_definitions(m3_headless_native PRIVATE SUPERMODEL_UNIX M3_GLES=1 M3_HEADLESS=1 XL_NATIVE=1)
    target_compile_options(m3_headless_native PRIVATE -w)
    target_link_libraries(m3_headless_native PRIVATE musashi Threads::Threads)
    # same with the touch-menu routines hooked in (they pass through when no touch input arrives)
    add_executable(m3_headless_native_hooks ${HL_SOURCES} ${XL_SRC} ${M3_ROOT}/xlat/guest/hooks.c ${M3_ROOT}/runtime/app/menu_guest.c ${M3_ROOT}/runtime/app/leaderboard_guest.c ${M3_ROOT}/runtime/app/leaderboard.cpp)
    target_include_directories(m3_headless_native_hooks BEFORE PRIVATE ${M3_ROOT}/xlat/guest ${HL_INC})
    target_include_directories(m3_headless_native_hooks PRIVATE ${M3_ROOT}/xlat)
    target_compile_definitions(m3_headless_native_hooks PRIVATE SUPERMODEL_UNIX M3_GLES=1 M3_HEADLESS=1 XL_NATIVE=1)
    target_compile_options(m3_headless_native_hooks PRIVATE -w)
    target_link_libraries(m3_headless_native_hooks PRIVATE musashi Threads::Threads)
  endif()
  return()
endif()

# ---- OpenGL loader
if(NOT M3_GLES)
  add_library(glew_static STATIC ${HW}/Pkgs/glew.c)
  target_include_directories(glew_static PUBLIC ${HW}/Pkgs)
  target_compile_definitions(glew_static PUBLIC GLEW_STATIC)
  target_compile_options(glew_static PRIVATE -w)
endif()

# ---- hardware + runtime + player
file(GLOB HW_SRC
  ${HW}/Model3/*.cpp ${HW}/Model3/DriveBoard/*.cpp
  ${HW}/Sound/SCSP.cpp ${HW}/Sound/SCSPDSP.cpp ${HW}/Sound/MPEG/*.cpp
  ${HW}/CPU/68K/68K.cpp ${HW}/CPU/Z80/Z80.cpp
  ${HW}/OSD/Logger.cpp ${HW}/OSD/Outputs.cpp ${HW}/OSD/Thread.cpp ${HW}/OSD/Audio.cpp ${HW}/OSD/SDLInputSystem.cpp
  ${HW}/Util/*.cpp ${HW}/Inputs/*.cpp ${HW}/BlockFile.cpp ${HW}/ROMSet.cpp
  ${HW}/Graphics/*.cpp ${HW}/Graphics/New3D/*.cpp)
list(FILTER HW_SRC EXCLUDE REGEX "53C810Disasm")
set(M3_APP_SRC ${HW_SRC} ${M3_ROOT}/runtime/cpu_glue.cpp ${M3_ROOT}/runtime/pack/pack_util.cpp ${M3_ROOT}/runtime/pack/tex3d.cpp
    runtime/pack/tex2d.cpp
  ${M3_ROOT}/runtime/app/touch.cpp
  ${M3_ROOT}/runtime/app/pad.cpp
  ${M3_ROOT}/runtime/app/settings.cpp
  ${M3_ROOT}/runtime/app/leaderboard.cpp
  ${M3_ROOT}/runtime/app/play_main.cpp ${M3_ROOT}/runtime/app/game_srally2.cpp ${M3_ROOT}/runtime/app/rom_images.cpp)
if(ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
  list(APPEND M3_APP_SRC ${M3_ROOT}/runtime/emutls_arm.c)   # 32-bit Android: emulated TLS
endif()
if(ANDROID)
  # SDLActivity loads libmain.so and calls SDL_main (play_main.cpp's main, renamed by <SDL.h>)
  add_library(srally2 SHARED ${M3_APP_SRC})
  set_target_properties(srally2 PROPERTIES OUTPUT_NAME main)
else()
  add_executable(srally2 ${M3_APP_SRC})
endif()
if(M3_GLES)
  # <GL/glew.h> resolves to a GLES 3.2 shim (must come first)
  target_include_directories(srally2 BEFORE PRIVATE ${M3_ROOT}/runtime/gles_shim)
  target_compile_definitions(srally2 PRIVATE M3_GLES=1)
endif()
target_include_directories(srally2 PRIVATE
  ${HW} ${HW}/OSD ${HW}/Inputs ${HW}/Network ${HW}/Pkgs ${HW}/Graphics ${HW}/Graphics/New3D
  ${M3_ROOT}/runtime ${MUSASHI_GEN} ${SDL2_INCLUDE_DIRS})
target_compile_definitions(srally2 PRIVATE SUPERMODEL_UNIX)
if(M3_XLAT)
  target_compile_definitions(srally2 PRIVATE XL_NATIVE=1)
endif()
target_compile_options(srally2 PRIVATE -w)
target_link_libraries(srally2 PRIVATE guest musashi Threads::Threads)
if(M3_GLES)
  find_library(GLES_LIB NAMES GLESv3 GLESv2 REQUIRED)
  target_link_libraries(srally2 PRIVATE ${GLES_LIB})
else()
  target_link_libraries(srally2 PRIVATE glew_static OpenGL::GL)
endif()
if(TARGET SDL2::SDL2)
  target_link_libraries(srally2 PRIVATE SDL2::SDL2)
else()
  target_link_libraries(srally2 PRIVATE ${SDL2_LIBRARIES})
endif()
if(APPLE)
  target_link_libraries(srally2 PRIVATE "-framework Cocoa")
endif()
if(ANDROID)
  target_link_libraries(srally2 PRIVATE log android EGL)
endif()
