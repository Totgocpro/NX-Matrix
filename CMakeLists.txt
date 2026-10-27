cmake_minimum_required(VERSION 3.16)

project(NX-Matrix VERSION 0.1.0 LANGUAGES C CXX)

set(NXMATRIX_VERSION "${PROJECT_VERSION}" CACHE STRING "Version shown in the UI and written to the NACP")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Switch-only homebrew: cross-compile with devkitPro's toolchain file:
#   cmake -B build-switch -DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Switch.cmake
if(NOT CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    message(FATAL_ERROR "NX-Matrix is a Nintendo Switch homebrew: pass -DCMAKE_TOOLCHAIN_FILE=$ENV{DEVKITPRO}/cmake/Switch.cmake")
endif()
if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO env var must be set (devkitPro + devkitA64 required)")
endif()

include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
# Dependencies still declare compatibility with very old CMake versions.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

## Plutonium
# Plutonium has no CMakeLists (pure devkitPro Makefile project), so we fetch
# its sources, build libpu.a with its own Makefile and import the archive.
set(NXMATRIX_PLUTONIUM_TAG "1.0.0" CACHE STRING "Plutonium git tag fetched via FetchContent")
set(NXMATRIX_PLUTONIUM_DIR "" CACHE PATH "Local Plutonium checkout (skips FetchContent when set)")
if(NXMATRIX_PLUTONIUM_DIR)
    set(PLUTONIUM_SOURCE_DIR "${NXMATRIX_PLUTONIUM_DIR}")
else()
    FetchContent_Declare(plutonium
        GIT_REPOSITORY https://github.com/XorTroll/Plutonium.git
        GIT_TAG ${NXMATRIX_PLUTONIUM_TAG}
    )
    FetchContent_GetProperties(plutonium)
    if(NOT plutonium_POPULATED)
        FetchContent_Populate(plutonium)
    endif()
    set(PLUTONIUM_SOURCE_DIR "${plutonium_SOURCE_DIR}")
endif()
message(STATUS "Using Plutonium from: ${PLUTONIUM_SOURCE_DIR}")
set(PLUTONIUM_LIB "${PLUTONIUM_SOURCE_DIR}/Plutonium/lib/libpu.a")
# ALL: the lib must build as part of the default build (dependencies alone do
# not pull a plain custom target into the build).
add_custom_target(plutonium_build ALL
    COMMAND ${CMAKE_MAKE_PROGRAM} -C "${PLUTONIUM_SOURCE_DIR}"
    BYPRODUCTS "${PLUTONIUM_LIB}"
    COMMENT "Building Plutonium (libpu.a) with devkitPro"
    VERBATIM
)
add_library(plutonium STATIC IMPORTED GLOBAL)
set_target_properties(plutonium PROPERTIES
    IMPORTED_LOCATION "${PLUTONIUM_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${PLUTONIUM_SOURCE_DIR}/Plutonium/include"
)
add_dependencies(plutonium plutonium_build)

## nlohmann/json
set(NXMATRIX_JSON_TAG "v3.11.3" CACHE STRING "nlohmann/json git tag fetched via FetchContent")
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG ${NXMATRIX_JSON_TAG}
)
FetchContent_MakeAvailable(nlohmann_json)

## libolm
# Built as a static library from its own CMake project (tests disabled).
set(NXMATRIX_OLM_TAG "3.2.16" CACHE STRING "libolm git tag fetched via FetchContent")
set(OLM_TESTS OFF CACHE INTERNAL "")
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(libolm
    GIT_REPOSITORY https://gitlab.matrix.org/matrix-org/olm.git
    GIT_TAG ${NXMATRIX_OLM_TAG}
)
FetchContent_MakeAvailable(libolm)
set(BUILD_SHARED_LIBS ON)

## libcurl + zlib
# The Switch.cmake toolchain points pkg-config at aarch64-none-elf-pkg-config.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SWITCH_CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SWITCH_ZLIB REQUIRED IMPORTED_TARGET zlib)

## CA bundle
# Horizon has no system CA store, so the Mozilla bundle ships in the romfs and
# is handed to curl at startup ("romfs:/cacert.pem").
set(NXMATRIX_ROMFS_DIR "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${NXMATRIX_ROMFS_DIR}")

## Header icons
# Assets/*.svg rasterized once to white 96x96 PNGs (rsvg-convert); the app
# tints them at runtime (SDL_SetTextureColorMod) and reads romfs:/*.png.
foreach(NXMATRIX_ICON settings plus search)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Assets/${NXMATRIX_ICON}.png"
                   "${NXMATRIX_ROMFS_DIR}/${NXMATRIX_ICON}.png" COPYONLY)
endforeach()
set(NXMATRIX_CACERT "${NXMATRIX_ROMFS_DIR}/cacert.pem")
if(NOT EXISTS "${NXMATRIX_CACERT}")
    message(STATUS "Downloading CA bundle from https://curl.se/ca/cacert.pem")
    file(DOWNLOAD "https://curl.se/ca/cacert.pem" "${NXMATRIX_CACERT}" STATUS CACERT_STATUS)
    list(GET CACERT_STATUS 0 CACERT_CODE)
    # file(DOWNLOAD) reports success on HTTP error pages too: check the payload.
    file(READ "${NXMATRIX_CACERT}" CACERT_HEAD LIMIT 4096)
    if(NOT CACERT_CODE EQUAL 0 OR NOT CACERT_HEAD MATCHES "BEGIN CERTIFICATE")
        file(REMOVE "${NXMATRIX_CACERT}")
        message(FATAL_ERROR "Could not download the CA bundle; place cacert.pem at ${NXMATRIX_CACERT} manually")
    endif()
endif()

## Color emoji font (Noto Color Emoji, CBDT bitmap font)
# The Switch shared fonts have no emoji glyphs; romfs:/NotoColorEmoji.ttf is
# rasterized through FreeType (src/Ui/Elements/RichText.cpp). nx_create_nro
# packs the whole romfs dir, so downloading it here is all that is needed.
set(NXMATRIX_EMOJI_FONT "${NXMATRIX_ROMFS_DIR}/NotoColorEmoji.ttf")
if(NOT EXISTS "${NXMATRIX_EMOJI_FONT}")
    set(NXMATRIX_EMOJI_URLS
        "https://github.com/googlefonts/noto-emoji/raw/main/2D/fonts/NotoColorEmoji.ttf"
        "https://raw.githubusercontent.com/googlefonts/noto-emoji/main/2D/fonts/NotoColorEmoji.ttf")
    set(NXMATRIX_EMOJI_OK OFF)
    foreach(EMOJI_URL IN LISTS NXMATRIX_EMOJI_URLS)
        message(STATUS "Downloading Noto Color Emoji from ${EMOJI_URL}")
        file(DOWNLOAD "${EMOJI_URL}" "${NXMATRIX_EMOJI_FONT}" STATUS EMOJI_STATUS)
        list(GET EMOJI_STATUS 0 EMOJI_CODE)
        # file(DOWNLOAD) can "succeed" on an HTTP error page, so validate the
        # payload: the font is >100 KB of binary, error pages are not.
        if(EMOJI_CODE EQUAL 0)
            file(SIZE "${NXMATRIX_EMOJI_FONT}" EMOJI_SIZE)
            file(READ "${NXMATRIX_EMOJI_FONT}" EMOJI_HEAD LIMIT 64)
            if(EMOJI_SIZE GREATER 100000 AND NOT EMOJI_HEAD MATCHES "<(!DOCTYPE|html)")
                set(NXMATRIX_EMOJI_OK ON)
                break()
            endif()
        endif()
        file(REMOVE "${NXMATRIX_EMOJI_FONT}")
    endforeach()
    if(NOT NXMATRIX_EMOJI_OK)
        message(FATAL_ERROR "Could not download the emoji font; place NotoColorEmoji.ttf at ${NXMATRIX_EMOJI_FONT} manually")
    endif()
endif()

# All C++ sources live under src/, all headers under include/.
# GlobRecurse is required by the project layout spec.
file(GLOB_RECURSE NXMATRIX_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c"
)

add_executable(nx-matrix ${NXMATRIX_SOURCES})
target_include_directories(nx-matrix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(nx-matrix PRIVATE NXMATRIX_VERSION="${NXMATRIX_VERSION}")
# -fno-rtti is mandatory: libpu.a is built with -fno-rtti, so the typeinfo of
# Plutonium base classes does not exist; with RTTI our derived classes would
# reference it and fail at link. Exceptions stay enabled (nlohmann throws).
# No typeid/dynamic_cast/throw is used in our own code.
target_compile_options(nx-matrix PRIVATE -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -ffunction-sections -fno-rtti -O2)

# devkitPro portlibs (SDL2, freetype, mbedtls, ...) + libnx.
target_link_directories(nx-matrix PRIVATE
    "$ENV{DEVKITPRO}/portlibs/switch/lib"
    "$ENV{DEVKITPRO}/libnx/lib"
)
target_include_directories(nx-matrix PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")
# FreeType headers live in a dedicated subdir (pkg-config freetype2 exposes
# it); RichText.cpp includes ft2build.h from there.
target_include_directories(nx-matrix PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include/freetype2")
# Same link set as NX-Discord (proven against switch-curl/SDL2/Plutonium).
# NOTE: no explicit -specs=switch.specs here: the Switch.cmake toolchain
# already injects it (passing it twice makes ld fail on switch.ld).
target_link_libraries(nx-matrix PRIVATE
    plutonium
    olm
    SDL2_mixer opusfile opus modplug mpg123 vorbisidec ogg
    SDL2_ttf SDL2_gfx SDL2_image SDL2
    EGL GLESv2 glapi drm_nouveau webp png jpeg
    # png16 after freetype: CBDT glyph decoding (FT_LOAD_COLOR) pulls FreeType's
    # PNG shim, which needs libpng symbols further down the link line.
    freetype harfbuzz bz2 png16
    PkgConfig::SWITCH_ZLIB PkgConfig::SWITCH_CURL
    nlohmann_json::nlohmann_json
    mbedtls mbedx509 mbedcrypto
    nx m
)

## Homebrew packaging (.nro for hbmenu + .nacp metadata)
nx_generate_nacp(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/nx-matrix.nacp"
    NAME "NX-Matrix" AUTHOR "Tototcs" VERSION "${NXMATRIX_VERSION}")
# hbmenu decodes the JPEG in the NRO asset section; libnx's own default icon is
# 256x256, so Assets/icon256.jpg (downscaled Assets/icon.png) matches it.
nx_create_nro(nx-matrix
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/nx-matrix.nro"
    ICON "${CMAKE_CURRENT_SOURCE_DIR}/Assets/icon256.jpg"
    NACP "${CMAKE_CURRENT_BINARY_DIR}/nx-matrix.nacp"
    ROMFS "${NXMATRIX_ROMFS_DIR}")