cmake_minimum_required(VERSION 3.22.1)
project(SpaceEngineLandscape LANGUAGES C CXX)

get_filename_component(SE_WORKSPACE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(SE_RECOMP_DIR "${SE_WORKSPACE_ROOT}/recomp")
set(SE_SHIMS_DIR "${SE_WORKSPACE_ROOT}/shims")

add_subdirectory("${SE_SHIMS_DIR}/third_party" third_party_build)

set_property(GLOBAL PROPERTY JOB_POOLS se_compile=3)
set(CMAKE_JOB_POOL_COMPILE se_compile)

file(GLOB SE_RECOMP_FUNC_SRCS CONFIGURE_DEPENDS "${SE_RECOMP_DIR}/recomp_funcs_*.c")
list(SORT SE_RECOMP_FUNC_SRCS)

add_library(SpaceEngine SHARED
    jni_bridge.c
    "${SE_RECOMP_DIR}/sha256.c"
    "${SE_RECOMP_DIR}/pe_loader.c"
    "${SE_RECOMP_DIR}/eh64.c"
    "${SE_RECOMP_DIR}/recomp_imports.c"
    "${SE_RECOMP_DIR}/recomp_dispatch.c"
    "${SE_RECOMP_DIR}/engine_host.c"
    "${SE_SHIMS_DIR}/hle_bridge.c"
    "${SE_SHIMS_DIR}/crt_intercepts.c"
    "${SE_SHIMS_DIR}/win32_kernel32.c"
    "${SE_SHIMS_DIR}/win32_misc.c"
    "${SE_SHIMS_DIR}/minizip_ord.c"
    "${SE_SHIMS_DIR}/win32_user32_gdi32.c"
    "${SE_SHIMS_DIR}/shader_es3.c"
    "${SE_SHIMS_DIR}/wgl_egl.c"
    "${SE_SHIMS_DIR}/png_tiff_hle.c"
    "${SE_SHIMS_DIR}/sdl2_mixer_hle.c"
    ${SE_RECOMP_FUNC_SRCS}
)

set_source_files_properties(
    ${SE_RECOMP_FUNC_SRCS}
    "${SE_RECOMP_DIR}/recomp_dispatch.c"
    PROPERTIES COMPILE_OPTIONS "-g0;-O1;-fno-strict-aliasing;-Wno-unused-variable;-Wno-unused-label;-Wno-parentheses-equality"
)

target_compile_definitions(SpaceEngine PRIVATE SE_RECOMP_FULL=1)

target_include_directories(SpaceEngine PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SE_RECOMP_DIR}"
    "${SE_SHIMS_DIR}"
)

find_library(log-lib log)
find_library(android-lib android)

target_link_libraries(SpaceEngine
    ${log-lib}
    ${android-lib}
    se_png
    se_tiff
    se_sdl2_mixer_ext
    SDL2
    z
    dl
    m
)
