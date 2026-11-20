cmake_minimum_required(VERSION 3.18)
project(se_third_party C CXX)

set(SE_TP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(SE_TP_CFG  "${SE_TP_ROOT}/config")

# Copy pnglibconf.h from upstream prebuilt header at configure time if needed
if(NOT EXISTS "${SE_TP_CFG}/pnglibconf.h")
    configure_file(
        "${SE_TP_ROOT}/libpng/scripts/pnglibconf.h.prebuilt"
        "${SE_TP_CFG}/pnglibconf.h"
        COPYONLY
    )
endif()

# ---------------------------------------------------------------------------
# 1. zlib (host / non-Android builds that lack system libz)
# ---------------------------------------------------------------------------
if(NOT ANDROID)
    set(SE_ZLIB_SRCS
        "${SE_TP_ROOT}/zlib/adler32.c"
        "${SE_TP_ROOT}/zlib/compress.c"
        "${SE_TP_ROOT}/zlib/crc32.c"
        "${SE_TP_ROOT}/zlib/deflate.c"
        "${SE_TP_ROOT}/zlib/gzclose.c"
        "${SE_TP_ROOT}/zlib/gzlib.c"
        "${SE_TP_ROOT}/zlib/gzread.c"
        "${SE_TP_ROOT}/zlib/gzwrite.c"
        "${SE_TP_ROOT}/zlib/infback.c"
        "${SE_TP_ROOT}/zlib/inffast.c"
        "${SE_TP_ROOT}/zlib/inflate.c"
        "${SE_TP_ROOT}/zlib/inftrees.c"
        "${SE_TP_ROOT}/zlib/trees.c"
        "${SE_TP_ROOT}/zlib/uncompr.c"
        "${SE_TP_ROOT}/zlib/zutil.c"
    )
    add_library(se_zlib STATIC ${SE_ZLIB_SRCS})
    target_include_directories(se_zlib PUBLIC "${SE_TP_ROOT}/zlib")
    target_compile_options(se_zlib PRIVATE -O2 -w)
    set(SE_ZLIB_TARGET se_zlib)
else()
    set(SE_ZLIB_TARGET z)
endif()

# ---------------------------------------------------------------------------
# 2. libpng 1.6.44
# ---------------------------------------------------------------------------
set(SE_PNG_SRCS
    "${SE_TP_ROOT}/libpng/png.c"
    "${SE_TP_ROOT}/libpng/pngerror.c"
    "${SE_TP_ROOT}/libpng/pngget.c"
    "${SE_TP_ROOT}/libpng/pngmem.c"
    "${SE_TP_ROOT}/libpng/pngpread.c"
    "${SE_TP_ROOT}/libpng/pngread.c"
    "${SE_TP_ROOT}/libpng/pngrio.c"
    "${SE_TP_ROOT}/libpng/pngrtran.c"
    "${SE_TP_ROOT}/libpng/pngrutil.c"
    "${SE_TP_ROOT}/libpng/pngset.c"
    "${SE_TP_ROOT}/libpng/pngtrans.c"
    "${SE_TP_ROOT}/libpng/pngwio.c"
    "${SE_TP_ROOT}/libpng/pngwrite.c"
    "${SE_TP_ROOT}/libpng/pngwtran.c"
    "${SE_TP_ROOT}/libpng/pngwutil.c"
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)")
    list(APPEND SE_PNG_SRCS
        "${SE_TP_ROOT}/libpng/arm/arm_init.c"
        "${SE_TP_ROOT}/libpng/arm/filter_neon_intrinsics.c"
        "${SE_TP_ROOT}/libpng/arm/palette_neon_intrinsics.c"
    )
endif()

add_library(se_png STATIC ${SE_PNG_SRCS})
target_include_directories(se_png PUBLIC "${SE_TP_CFG}" "${SE_TP_ROOT}/libpng")
target_link_libraries(se_png PUBLIC ${SE_ZLIB_TARGET} m)
target_compile_options(se_png PRIVATE -O2 -w)

# ---------------------------------------------------------------------------
# 3. libtiff 4.7.0 (internal + ZIP/PixarLog codecs, C API)
# ---------------------------------------------------------------------------
set(SE_TIFF_SRCS
    "${SE_TP_ROOT}/libtiff/libtiff/tif_aux.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_close.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_codec.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_color.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_compress.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_dir.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_dirinfo.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_dirread.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_dirwrite.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_dumpmode.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_error.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_extension.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_fax3.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_fax3sm.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_flush.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_getimage.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_hash_set.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_luv.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_lzw.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_next.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_open.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_packbits.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_pixarlog.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_predict.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_print.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_read.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_strip.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_swab.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_thunder.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_tile.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_version.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_warning.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_write.c"
    "${SE_TP_ROOT}/libtiff/libtiff/tif_zip.c"
    "${SE_TP_ROOT}/libtiff/port/dummy.c"
)
if(WIN32)
    list(APPEND SE_TIFF_SRCS "${SE_TP_ROOT}/libtiff/libtiff/tif_win32.c")
else()
    list(APPEND SE_TIFF_SRCS "${SE_TP_ROOT}/libtiff/libtiff/tif_unix.c")
endif()

add_library(se_tiff STATIC ${SE_TIFF_SRCS})
target_include_directories(se_tiff PUBLIC
    "${SE_TP_CFG}"
    "${SE_TP_ROOT}/libtiff/libtiff"
    "${SE_TP_ROOT}/libtiff/port"
)
target_link_libraries(se_tiff PUBLIC ${SE_ZLIB_TARGET} m)
target_compile_options(se_tiff PRIVATE -O2 -w)

# ---------------------------------------------------------------------------
# 4. SDL2 2.30.12 + SDL-Mixer-X 2.5.0 (WAV + stb_vorbis)
# ---------------------------------------------------------------------------
if(ANDROID)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES ON CACHE BOOL "" FORCE)
    set(SE_SDL2_TARGET SDL2)
else()
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER_D3D OFF CACHE BOOL "" FORCE)
    set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
    set(SE_SDL2_TARGET SDL2-static)
endif()
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
set(SDL_X11 OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "" FORCE)

add_subdirectory("${SE_TP_ROOT}/SDL2" "${CMAKE_CURRENT_BINARY_DIR}/SDL2" EXCLUDE_FROM_ALL)

set(SE_MIXER_SRCS
    "${SE_TP_ROOT}/SDL-Mixer-X/src/effect_position.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/effect_stereoreverse.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/effects_internal.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/mixer.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/mixer_x_deprecated.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/music.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/utils.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/codecs/load_aiff.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/codecs/load_voc.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/codecs/mp3utils.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/codecs/music_ogg.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/codecs/music_ogg_stb.c"
    "${SE_TP_ROOT}/SDL-Mixer-X/src/codecs/music_wav.c"
)

add_library(se_sdl2_mixer_ext STATIC ${SE_MIXER_SRCS})
target_compile_definitions(se_sdl2_mixer_ext PRIVATE
    MUSIC_WAV
    MUSIC_OGG
    OGG_USE_STB
    HAVE_INTTYPES_H
    HAVE_SETENV
    HAVE_SINF
    _REENTRANT
    _USE_MATH_DEFINES
)
target_include_directories(se_sdl2_mixer_ext
    PUBLIC
        "${SE_TP_ROOT}/SDL-Mixer-X/include"
    PRIVATE
        "${SE_TP_ROOT}/SDL-Mixer-X/src"
        "${SE_TP_ROOT}/SDL-Mixer-X/src/codecs"
)
target_link_libraries(se_sdl2_mixer_ext PUBLIC ${SE_SDL2_TARGET} m)
target_compile_options(se_sdl2_mixer_ext PRIVATE -O2 -w)
