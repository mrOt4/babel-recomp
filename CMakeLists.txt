cmake_minimum_required(VERSION 3.20)
project(IllusionOfGaiaUsaSNESRecomp C CXX)

set(CMAKE_C_STANDARD 11)

# Release pin for lobby matching. Release builds stamp the VERSION file so two
# peers can prove they are the same build; everything else stamps "dev".
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _GAME_VER_RAW)
string(STRIP "${_GAME_VER_RAW}" GAME_RELEASE_VERSION)
set(SNESRECOMP_BUILD_VERSION "" CACHE STRING
    "Lobby release pin (empty = Release uses VERSION, else dev)")
if(SNESRECOMP_BUILD_VERSION STREQUAL "")
    get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_is_multi)
        set(SNESRECOMP_BUILD_VERSION "$<IF:$<CONFIG:Release>,${GAME_RELEASE_VERSION},dev>")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(SNESRECOMP_BUILD_VERSION "${GAME_RELEASE_VERSION}")
    else()
        set(SNESRECOMP_BUILD_VERSION "dev")
    endif()
endif()

set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the snesrecomp checkout used by this game")
if(NOT EXISTS "${SNESRECOMP_ROOT}/runner/runner.cmake")
    message(FATAL_ERROR
        "snesrecomp submodule missing at ${SNESRECOMP_ROOT}.\n"
        "Run: git submodule update --init --recursive")
endif()
# Mod packages are part of every project, not an option. The launcher's Mods
# page, the .snesmod loader, the trusted-plugin registry and netplay's mod-set
# gate all hang off this one switch; a build without it cannot exchange mods
# with a peer that has them, and the framework says so at every lobby init.
# Must precede the include: runner.cmake reads it to add the loader sources.
set(SNESRECOMP_ENABLE_MODS ON CACHE BOOL
    "Mod package loader + trusted plugins (required for every title)" FORCE)
# Interpreter-only by default: the game runs from the player's own ROM at
# run time and the binary carries nothing derived from it, so it can be
# distributed and builds without any regeneration step. Measured on this game:
# the same frames and the same speed as with recompiled code (it runs almost
# entirely in the interpreter tier either way). OFF + tools/regen.sh links the
# recompiled code instead.
set(SNESRECOMP_INTERPRETER_ONLY ON CACHE BOOL "Interpreter-only build (no ROM-derived code)")
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

# Nintendo Switch: an optional Mesa for Switch SDK (danfromtico/mesa-switch,
# Mesa 26 nvc0 / Zink+NVK) takes precedence over devkitPro's switch-mesa
# (Mesa 20.1) for every -lEGL / -lGLESv2 / -lglapi, including SDL2's own.
set(SNESRECOMP_SWITCH_MESA_DIR "" CACHE PATH
    "mesa-switch SDK prefix (…/mesa-install/opt/devkitpro/portlibs/switch); empty = devkitPro switch-mesa")
if(NINTENDO_SWITCH AND SNESRECOMP_SWITCH_MESA_DIR)
    set(CMAKE_EXE_LINKER_FLAGS "-L${SNESRECOMP_SWITCH_MESA_DIR}/lib ${CMAKE_EXE_LINKER_FLAGS}")
    include_directories(BEFORE SYSTEM "${SNESRECOMP_SWITCH_MESA_DIR}/include")
    message(STATUS "Switch: Mesa from ${SNESRECOMP_SWITCH_MESA_DIR}")
endif()

set(IOG_SOURCES
    ${SNESRECOMP_RUNNER_SOURCES}
    src/main.c
    src/host_contract.c
    src/game_rtl.c
    src/iog_widescreen.c
    src/iog_i18n.c
    src/iog_mods.c
    src/iog_msu.c
    src/iog_inventory.c
    src/iog_ips.c
    src/iog_ngplus.c
    src/iog_randomizer.c
    src/iog_settings.c
    src/iog_companion.c
    src/iog_text.c
    src/gen_stubs.c
)
if(ANDROID)
    # Android: the game is the "main" library SDL's activity loads.
    add_library(IllusionOfGaiaUsaSNESRecomp SHARED ${IOG_SOURCES})
else()
    add_executable(IllusionOfGaiaUsaSNESRecomp ${IOG_SOURCES})
endif()

# ROM identity. rom_identity.txt is the one place the digests live: the build
# turns it into snesrecomp_rom_identity.h (SNESRECOMP_ROM_EXPECTED_SHA256 and
# friends) for src/main.c, and tools/regen.sh, scripts/package_release.sh and
# the release workflow read the same file. A revision bump is one edit there.
snesrecomp_rom_identity(IllusionOfGaiaUsaSNESRecomp ${CMAKE_SOURCE_DIR}/rom_identity.txt)

# The game ships as "babel-recomp" (babel-recomp, babel-recomp.exe,
# babel-recomp.nro); the CMake target keeps its internal name. The Vita's is
# eboot.bin inside the .vpk, made from the ELF under its target name.
if(ANDROID)
    set_target_properties(IllusionOfGaiaUsaSNESRecomp PROPERTIES OUTPUT_NAME main)
elseif(NOT VITA)
    set_target_properties(IllusionOfGaiaUsaSNESRecomp PROPERTIES OUTPUT_NAME babel-recomp)
endif()

# Generated recompiler output. Not committed: it is derived from a copyrighted
# ROM, so every developer regenerates it locally from their own copy -- and a
# release does not ship it either. With -DSNESRECOMP_SETUP_HOST=ON and an empty
# src/gen/, this builds the SETUP HOST instead: the same executable minus the
# recompiled code, whose only path is the launcher's Generate & rebuild wizard.
# That is what release zips contain; the player's machine produces the
# playable binary from their own ROM. Without the option an empty src/gen/ is
# still a hard error (the framework owns the message).
snesrecomp_target_generated_code(IllusionOfGaiaUsaSNESRecomp ${CMAKE_SOURCE_DIR}/src/gen)

# The mod catalog. This names WHERE the packages are; the framework owns the
# layout they are staged into beside the executable, and stages them on every
# build OF THE EXECUTABLE -- including the launcher's own Generate & rebuild on
# a player's machine, which builds only this target and would skip a sibling
# ALL target. mods/preloaded/packages/ holds reviewed, default-disabled
# packages; players install .snesmod archives through the launcher.
# snesrecomp/docs/MOD_PACKAGES.md.
#
# It used to be `snesrecomp_target_stage_dir(... ${CMAKE_SOURCE_DIR}/mods mods)`
# -- the destination spelled per title, which is how three ports in the same
# workspace ended up shipping an empty Mods page and nothing said so. Declaring
# it is now also enforced: a project with packages that never calls this fails
# to configure.
# Text layout for runtime localization (no text: the player's ROMs supply it).
add_custom_command(TARGET IllusionOfGaiaUsaSNESRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_SOURCE_DIR}/i18n/structure.txt
        $<TARGET_FILE_DIR:IllusionOfGaiaUsaSNESRecomp>/i18n/structure.txt
    COMMENT "Staging i18n/structure.txt"
    VERBATIM)

snesrecomp_target_mod_catalog(IllusionOfGaiaUsaSNESRecomp ${CMAKE_SOURCE_DIR}/mods/preloaded)
# Any other directory the game reads exe-relative (a translations/ table, ...)
# is staged with snesrecomp_target_stage_dir, never with a separate ALL target.

target_include_directories(IllusionOfGaiaUsaSNESRecomp PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    # common_rtl.c includes "config.h": the desktop key-map / Config contract
    # the framework host supplies (snesrecomp_target_desktop_host below).
    ${SNESRECOMP_ROOT}/runner/src/desktop
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/recomp
)
target_link_libraries(IllusionOfGaiaUsaSNESRecomp PRIVATE ${SNESRECOMP_RUNNER_LIBRARIES})
target_compile_definitions(IllusionOfGaiaUsaSNESRecomp PRIVATE
    SNES_GAME_VERSION="${SNESRECOMP_BUILD_VERSION}"
    SNES_GAME_TITLE="Illusion Of Gaia Usa"
)
snesrecomp_target_sdl(IllusionOfGaiaUsaSNESRecomp)

# The desktop host: launcher flow, ROM resolution, config.ini + [KeyMap],
# window + SDL/OpenGL presenters, audio, gamepads, the save-state browser and
# rewind filmstrip, the pacing clock, crash handlers and the post-mortem
# report. src/main.c is the shim that names this title and supplies its
# hooks; the host is the framework's and improves on a submodule pull.
# Add TIER2 once this port reads the interpreter-gap manifest after each run.
snesrecomp_target_desktop_host(IllusionOfGaiaUsaSNESRecomp)
# rom.cfg (the ROM the wizard set this project up from; git-ignored) is
# staged beside the executable when none is there yet, so the first launch
# opens on that dump rather than asking for one.
snesrecomp_target_rom_cache(IllusionOfGaiaUsaSNESRecomp)

# Shared Dear ImGui pre-boot launcher (ROM picker, verification, display /
# audio / input settings). Self-contained: it brings its own ImGui, GL link and
# staged assets, and defines RECOMP_LAUNCHER so src/main.c compiles its GUI path.
# The 3DS and the Vita have no OpenGL for it: there the ROM is read from the
# game's folder and the in-game settings menu covers the options.
if(NOT NINTENDO_3DS AND NOT VITA)
    set(RECOMP_UI_ROOT "${CMAKE_SOURCE_DIR}/recomp-ui" CACHE PATH
        "Root directory of the recomp-ui launcher repo" FORCE)
    if(NOT EXISTS "${RECOMP_UI_ROOT}/recomp_ui.cmake")
        message(FATAL_ERROR "recomp-ui missing at ${RECOMP_UI_ROOT}.")
    endif()
    include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
    recomp_target_launcher_ui(IllusionOfGaiaUsaSNESRecomp CONSOLE snes
        BOXART "${CMAKE_SOURCE_DIR}/launcher_assets/img/boxart.tga")
endif()

# Generate & rebuild from inside the launcher (desktop only; consoles and
# Android cannot run a compiler). recomp-ui/src is on the include path because
# the host speaks RecompLauncherCGameInfo.
if(NINTENDO_SWITCH OR ANDROID OR NINTENDO_3DS OR VITA)
    target_sources(IllusionOfGaiaUsaSNESRecomp PRIVATE
        ${SNESRECOMP_ROOT}/host/snesrecomp_codegen_host_none.c)
else()
    target_sources(IllusionOfGaiaUsaSNESRecomp PRIVATE
        ${SNESRECOMP_ROOT}/host/snesrecomp_codegen_host.c)
endif()
target_include_directories(IllusionOfGaiaUsaSNESRecomp PRIVATE
    ${SNESRECOMP_ROOT}/host
    ${CMAKE_SOURCE_DIR}/recomp-ui/src)

# Netplay is not built into this target.
# No multitap: two seats, one controller per port.

# Windows: author and version in the .exe's properties.
if(WIN32)
    target_sources(IllusionOfGaiaUsaSNESRecomp PRIVATE src/babel-recomp.rc)
endif()

# Nintendo Switch homebrew: package the ELF as an .nro (devkitPro's helpers).
# On the console the game reads its ROM, saves and mods from the directory the
# .nro is launched from (sdmc:/switch/babel-recomp/).
if(NINTENDO_SWITCH)
    file(READ "${CMAKE_SOURCE_DIR}/VERSION" _iog_version)
    string(STRIP "${_iog_version}" _iog_version)
    nx_generate_nacp(babel-recomp.nacp
        NAME "Babel Recomp"
        AUTHOR "mr0ta"
        VERSION "${_iog_version}")
    set(SNESRECOMP_SWITCH_ICON "${CMAKE_SOURCE_DIR}/res/icon_256.jpg" CACHE FILEPATH
        "256x256 JPEG icon for babel-recomp.nro")
    nx_create_nro(IllusionOfGaiaUsaSNESRecomp
        NACP babel-recomp.nacp
        ICON "${SNESRECOMP_SWITCH_ICON}"
        OUTPUT babel-recomp.nro)
endif()

# The handhelds' toolchains default Release to -O2; the emulation core is
# measurably faster at -O3 there (3DS: -3% per frame).
if(NINTENDO_3DS OR VITA)
    target_compile_options(IllusionOfGaiaUsaSNESRecomp PRIVATE $<$<CONFIG:Release>:-O3>)
endif()

# Nintendo 3DS homebrew (.3dsx for the Homebrew Launcher). The game reads its
# ROM, saves and mods from sdmc:/3ds/babel-recomp/.
if(NINTENDO_3DS)
    ctr_generate_smdh(babel-recomp.smdh
        NAME "Babel Recomp"
        DESCRIPTION "Illusion of Gaia (USA) v${GAME_RELEASE_VERSION}"
        AUTHOR "mr0ta"
        ICON "${CMAKE_SOURCE_DIR}/res/icon_48.png")
    ctr_create_3dsx(IllusionOfGaiaUsaSNESRecomp
        SMDH babel-recomp.smdh
        OUTPUT babel-recomp.3dsx)
    # The installable .cia (HOME Menu), for every model: makerom (Project_CTR)
    # and bannertool are not part of devkitPro; see scripts/toolchains.
    find_program(CTR_MAKEROM makerom HINTS $ENV{HOME}/toolchains/ctr-tools/bin $ENV{DEVKITPRO}/tools/bin)
    find_program(CTR_BANNERTOOL bannertool HINTS $ENV{HOME}/toolchains/ctr-tools/bin $ENV{DEVKITPRO}/tools/bin)
    if(CTR_MAKEROM AND CTR_BANNERTOOL)
        set(_r ${CMAKE_SOURCE_DIR}/res)
        add_custom_command(OUTPUT babel-recomp.cia
            COMMAND ${CTR_BANNERTOOL} makebanner -i ${_r}/3ds/banner.png -a ${_r}/3ds/banner.wav -o banner.bnr
            COMMAND ${CTR_BANNERTOOL} makesmdh -s "Babel Recomp" -l "Illusion of Gaia (USA) v${GAME_RELEASE_VERSION}"
                    -p "mr0ta" -i ${_r}/icon_48.png -o icon.icn
            COMMAND ${CTR_MAKEROM} -f cia -o babel-recomp.cia -target t -elf $<TARGET_FILE:IllusionOfGaiaUsaSNESRecomp>
                    -rsf ${_r}/3ds/app.rsf -icon icon.icn -banner banner.bnr
            DEPENDS IllusionOfGaiaUsaSNESRecomp ${_r}/3ds/app.rsf ${_r}/3ds/banner.png ${_r}/3ds/banner.wav ${_r}/icon_48.png
            VERBATIM)
        add_custom_target(babel-recomp-cia ALL DEPENDS babel-recomp.cia)
    else()
        message(STATUS "3DS: makerom/bannertool not found -- building the .3dsx only")
    endif()
endif()

# PlayStation Vita (.vpk for VitaShell). The game reads its ROM, saves and mods
# from ux0:/data/babel-recomp/; the data bundled in the .vpk is copied there on
# the first launch of each version.
if(VITA)
    include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
    set(_v ${CMAKE_SOURCE_DIR}/res/vita)
    target_link_options(IllusionOfGaiaUsaSNESRecomp PRIVATE -Wl,-q)
    target_link_libraries(IllusionOfGaiaUsaSNESRecomp PRIVATE ScePower_stub)
    vita_create_self(eboot.bin IllusionOfGaiaUsaSNESRecomp UNSAFE)
    string(REGEX REPLACE "^([0-9]+)\\.([0-9]+).*" "\\1" _vmaj "${GAME_RELEASE_VERSION}")
    string(REGEX REPLACE "^([0-9]+)\\.([0-9]+).*" "\\2" _vmin "${GAME_RELEASE_VERSION}")
    if(_vmaj LESS 10)
        set(_vmaj "0${_vmaj}")
    endif()
    if(_vmin LESS 10)
        set(_vmin "0${_vmin}")
    endif()
    file(GLOB_RECURSE _i18n RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/i18n/*)
    file(GLOB_RECURSE _mods RELATIVE ${CMAKE_SOURCE_DIR} CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/mods/preloaded/packages/*)
    set(_bundle)
    foreach(f IN LISTS _i18n _mods)
        list(APPEND _bundle FILE ${CMAKE_SOURCE_DIR}/${f} bundle/${f})
    endforeach()
    vita_create_vpk(babel-recomp.vpk BABL00001 eboot.bin
        VERSION ${_vmaj}.${_vmin}
        NAME "Babel Recomp"
        FILE ${_v}/sce_sys/icon0.png sce_sys/icon0.png
             ${_v}/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
             ${_v}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
             ${_v}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
             ${_bundle})
endif()
