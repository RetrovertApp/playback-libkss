# libkss Playback Plugin
#
# This plugin plays MSX music formats using the libkss library by digital-sound-antiques.
# Supported formats: KSS, MGS, BGM, OPX, MPK, MBM (MSX/MSX2 sound chips)
# Sound chips emulated: AY-3-8910 (PSG), SN76489, YM2413 (OPLL), Y8950 (MSX-AUDIO), SCC
#
# libkss uses git submodules for chip emulators and drivers. Since GitHub tarballs
# do not include submodule contents, we download each dependency separately.

cmake_minimum_required(VERSION 3.16)
project(playback_libkss C)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PlaybackPlugin)

# Download libkss v1.2.1 (main repo)
download_library(
    NAME libkss
    URL "https://github.com/digital-sound-antiques/libkss/archive/refs/tags/1.2.1.tar.gz"
    FILENAME "libkss-1.2.1.tar.gz"
    HASH "303a0948350a137630a69658d658e9c11b42832588768b9f60c19b8586348c76"
    EXTRACT_DIR "libkss-1.2.1"
    PATCHES patches/kssplay-channel-mask-type.patch
)

# Download submodules (chip emulators + KSS drivers)
# These are pinned to the exact commits referenced by libkss 1.2.1.
download_library(
    NAME libkss_emu2149
    URL "https://github.com/digital-sound-antiques/emu2149/archive/02fc5f0b411c35e3d69cc7f161595b56b4fda4f2.tar.gz"
    FILENAME "libkss-emu2149.tar.gz"
    HASH "0f12ca5b5d426e42c0eb430486cf4dfa880c144563a518dfd086ef215507da32"
    EXTRACT_DIR "emu2149-02fc5f0b411c35e3d69cc7f161595b56b4fda4f2"
)

download_library(
    NAME libkss_emu2212
    URL "https://github.com/digital-sound-antiques/emu2212/archive/ce7ade109942d9ba1b8067a27af47ddf37795fd7.tar.gz"
    FILENAME "libkss-emu2212.tar.gz"
    HASH "92691556be0cc644fee93277077b344fc6b88c3e31abd9de80e894620738cbe8"
    EXTRACT_DIR "emu2212-ce7ade109942d9ba1b8067a27af47ddf37795fd7"
)

download_library(
    NAME libkss_emu2413
    URL "https://github.com/digital-sound-antiques/emu2413/archive/a2dfc20ff507e4fd075cd325620bcea655e2c1f7.tar.gz"
    FILENAME "libkss-emu2413.tar.gz"
    HASH "bd816079cfbf13c56a260c095d6b8e260c2f877e5b13f50d4e570b96e8b92830"
    EXTRACT_DIR "emu2413-a2dfc20ff507e4fd075cd325620bcea655e2c1f7"
)

download_library(
    NAME libkss_emu76489
    URL "https://github.com/digital-sound-antiques/emu76489/archive/2da50f887bf998e796c700e1a03f76c62a3809ff.tar.gz"
    FILENAME "libkss-emu76489.tar.gz"
    HASH "cddddbed8afdbe3fc21c5d2917286066c02d737bc4dc69a431acd7bc5a124fc5"
    EXTRACT_DIR "emu76489-2da50f887bf998e796c700e1a03f76c62a3809ff"
)

download_library(
    NAME libkss_emu8950
    URL "https://github.com/digital-sound-antiques/emu8950/archive/bb4c784a7804353124cfba95b825bff1de1f8088.tar.gz"
    FILENAME "libkss-emu8950.tar.gz"
    HASH "ce7eef913de6d4c95ddee3585ffe16e56add59ac3831fdd997a7d6dbf4a9a17b"
    EXTRACT_DIR "emu8950-bb4c784a7804353124cfba95b825bff1de1f8088"
)

download_library(
    NAME libkss_kmz80
    URL "https://github.com/digital-sound-antiques/kmz80/archive/daf94dee27c7471d1e3415d44a30f850d32c529d.tar.gz"
    FILENAME "libkss-kmz80.tar.gz"
    HASH "3695aa0e02235a1d018147c218821b0df43258ea5b4aaf730c51761d82853638"
    EXTRACT_DIR "kmz80-daf94dee27c7471d1e3415d44a30f850d32c529d"
)

download_library(
    NAME libkss_drivers
    URL "https://github.com/digital-sound-antiques/kss-drivers/archive/e8032da38ea8439caca699954fce82b5050bf86f.tar.gz"
    FILENAME "libkss-drivers.tar.gz"
    HASH "533cc994c0b4b85bcecedd98351a981204779833d89d49577e619d3d5685d47e"
    EXTRACT_DIR "kss-drivers-e8032da38ea8439caca699954fce82b5050bf86f"
)

# Populate the modules/ directories so that libkss source includes resolve correctly.
# libkss expects: modules/emu2149/emu2149.h, modules/drivers/mbr143.h, etc.
set(_modules_dir "${libkss_SOURCE_DIR}/modules")
set(_submodules
    emu2149  "${libkss_emu2149_SOURCE_DIR}"
    emu2212  "${libkss_emu2212_SOURCE_DIR}"
    emu2413  "${libkss_emu2413_SOURCE_DIR}"
    emu76489 "${libkss_emu76489_SOURCE_DIR}"
    emu8950  "${libkss_emu8950_SOURCE_DIR}"
    kmz80    "${libkss_kmz80_SOURCE_DIR}"
    drivers  "${libkss_drivers_SOURCE_DIR}"
)

list(LENGTH _submodules _len)
math(EXPR _last "${_len} - 1")
foreach(_i RANGE 0 ${_last} 2)
    math(EXPR _j "${_i} + 1")
    list(GET _submodules ${_i} _name)
    list(GET _submodules ${_j} _src)
    set(_dest "${_modules_dir}/${_name}")
    # Use a sentinel file to avoid redundant copies
    if(NOT EXISTS "${_dest}/.populated")
        message(STATUS "[libkss] Populating modules/${_name}")
        file(COPY "${_src}/" DESTINATION "${_dest}")
        file(WRITE "${_dest}/.populated" "1\n")
    endif()
endforeach()

# Chip emulator sources
file(GLOB EMU2149_SRC ${_modules_dir}/emu2149/*.c)
file(GLOB EMU2212_SRC ${_modules_dir}/emu2212/*.c)
file(GLOB EMU2413_SRC ${_modules_dir}/emu2413/*.c)
file(GLOB EMU8950_SRC ${_modules_dir}/emu8950/*.c)
file(GLOB EMU76489_SRC ${_modules_dir}/emu76489/*.c)
file(GLOB KMZ80_SRC ${_modules_dir}/kmz80/*.c)

# libkss core sources
file(GLOB LIBKSS_SRC ${libkss_SOURCE_DIR}/src/*.c)
file(GLOB LIBKSS_KSS_SRC ${libkss_SOURCE_DIR}/src/kss/*.c)
file(GLOB LIBKSS_VM_SRC ${libkss_SOURCE_DIR}/src/vm/*.c)
file(GLOB LIBKSS_FILTER_SRC ${libkss_SOURCE_DIR}/src/filters/*.c)
file(GLOB LIBKSS_RCONV_SRC ${libkss_SOURCE_DIR}/src/rconv/*.c)

add_library(libkss_static STATIC
    ${EMU2149_SRC}
    ${EMU2212_SRC}
    ${EMU2413_SRC}
    ${EMU8950_SRC}
    ${EMU76489_SRC}
    ${KMZ80_SRC}
    ${LIBKSS_SRC}
    ${LIBKSS_KSS_SRC}
    ${LIBKSS_VM_SRC}
    ${LIBKSS_FILTER_SRC}
    ${LIBKSS_RCONV_SRC}
)

target_include_directories(libkss_static PUBLIC
    ${libkss_SOURCE_DIR}/src
    ${libkss_SOURCE_DIR}/src/kss
    ${_modules_dir}
)

suppress_external_warnings(libkss_static)
set_target_properties(libkss_static PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
)

# Plugin shared library
add_library(libkss_playback SHARED
    libkss_plugin.c
)

target_include_directories(libkss_playback PRIVATE
    ${libkss_SOURCE_DIR}/src
    ${libkss_SOURCE_DIR}/src/kss
)

target_link_libraries(libkss_playback PRIVATE libkss_static)

if(UNIX)
    target_link_libraries(libkss_playback PRIVATE m)
endif()

target_compile_options(libkss_playback PRIVATE
    -Wno-unused-parameter
)

rv_set_plugin_properties(libkss_playback)
