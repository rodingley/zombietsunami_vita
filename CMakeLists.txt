cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(zombietsunami_vita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Zombie Tsunami")
set(VITA_TITLEID "ZMBT00001")
set(VITA_VERSION "01.00")
# extended memory mode, as the other so-loader ports use
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

# GCC 15 defaults to C23; use C17 like the rest of the Vita ecosystem still expects
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=gnu17 -g -Wl,-q -D_GNU_SOURCE -Wall -O2")

# direct_imports.c is generated from what your SDK actually provides
set(DIRECT_C "${CMAKE_BINARY_DIR}/direct_imports.c")
option(REAL_GL "draw with vitaGL (needs vitaGL + vitaShaRK installed)" ON)
if(REAL_GL)
  set(GL_SRC "${CMAKE_SOURCE_DIR}/src/gl_real.c")
  set(GEN_ENV "REAL_GL=1")
else()
  set(GL_SRC "${CMAKE_SOURCE_DIR}/src/gl_stubs.c")
  set(GEN_ENV "REAL_GL=")
endif()
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env ${GEN_ENV} bash "${CMAKE_SOURCE_DIR}/tools/gen_direct.sh"
          "${CMAKE_SOURCE_DIR}/tools/needed_symbols.txt"
          "${CMAKE_SOURCE_DIR}/src/imports.c" "${GL_SRC}" "${DIRECT_C}"
  RESULT_VARIABLE GEN_RESULT)
if(NOT GEN_RESULT EQUAL 0)
  message(FATAL_ERROR "tools/gen_direct.sh failed (is VITASDK set?)")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/imports.c" "${GL_SRC}"
  "${CMAKE_SOURCE_DIR}/tools/needed_symbols.txt")

# zlib 1.3.1 is bundled (src/zlib) so the game's inflate/deflate always resolve
file(GLOB ZLIB_SRC "${CMAKE_SOURCE_DIR}/src/zlib/*.c")
set_source_files_properties(${ZLIB_SRC} PROPERTIES COMPILE_FLAGS "-w")

option(CRASH_HANDLER "log CPU registers on a crash (needs a recent kubridge)" OFF)
set(EXTRA_SRC "")
if(CRASH_HANDLER)
  list(APPEND EXTRA_SRC src/crash.c)
  add_definitions(-DCRASH_HANDLER=1)
endif()

add_executable(zombietsunami_loader.elf
  src/main.c
  src/platform_vita.c
  src/so_util.c
  src/stage1.c
  src/imports.c
  src/asset_io.c
  src/asset_cache.c
  src/art_patch.c
  src/assets.c
  src/ime.c
  ${GL_SRC}
  src/gfx_vita.c
  src/jni.c
  src/opensl_vita.c
  src/fmod_log.c
  src/patches.c
  src/popup.c
  src/updater.c
  src/update_util.c
  src/game_art.c
  src/png_load.c
  src/stage2b.c
  ${DIRECT_C}
  ${ZLIB_SRC}
  ${EXTRA_SRC}
)
target_include_directories(zombietsunami_loader.elf PRIVATE src)

set(VITA_LIBS
  pthread
  m
  taihen_stub
  kubridge_stub
  SceVshBridge_stub
  SceCtrl_stub
  SceTouch_stub SceMotion_stub
  ScePower_stub
  # auto-updater (updater.c): network, HTTPS, pop-ups, restart
  SceNet_stub
  SceNetCtl_stub
  SceHttp_stub
  SceSsl_stub
  SceSysmodule_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceProcessmgr_stub
)
if(REAL_GL)
  add_definitions(-DREAL_GL=1)
  list(APPEND VITA_LIBS
    vitaGL
    vitashark
    SceShaccCgExt
    mathneon
    SceAppMgr_stub
    SceAppUtil_stub
    SceAudio_stub
    SceCommonDialog_stub
    SceDisplay_stub
    SceKernelDmacMgr_stub
    SceGxm_stub
    SceShaccCg_stub
    SceSysmodule_stub
    ScePower_stub
    stdc++
    m)
endif()
target_link_libraries(zombietsunami_loader.elf ${VITA_LIBS})

vita_create_self(eboot.bin zombietsunami_loader.elf UNSAFE)
vita_create_vpk(zombietsunami.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME "${VITA_APP_NAME}"
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startgame.png sce_sys/livearea/contents/startgame.png
  FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE ${CMAKE_SOURCE_DIR}/popup/popup_text.png popup_text.png
  FILE ${CMAKE_SOURCE_DIR}/popup/credit.png credit.png
)
