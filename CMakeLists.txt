cmake_minimum_required(VERSION 4.0)
project(lyren LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(LYREN_LTO "Optimize across translation units in optimized builds" ON)
option(LYREN_NATIVE "Tune optimized builds for this machine's CPU" OFF)
if(LYREN_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(LANGUAGES C CXX)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON)
endif()

include(CTest)
option(LYREN_TEST_GRAPHICS "Include tests requiring Wayland and Vulkan in CTest" OFF)

include(cmake/Deps.cmake)
include(cmake/WaylandProtocols.cmake)
include(cmake/Shaders.cmake)

# Icons: every SVG under assets/icons is embedded and looked up by file name.
file(GLOB ICON_FILES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/assets/icons/*.svg)
set(icon_arrays "")
set(icon_entries "")
foreach(f ${ICON_FILES})
    get_filename_component(name ${f} NAME_WE)
    string(MAKE_C_IDENTIFIER ${name} id)
    string(APPEND icon_arrays "static const unsigned char svg_${id}[] = {\n#embed \"icons/${name}.svg\" suffix(, 0)\n};\n")
    string(APPEND icon_entries "    {\"${name}\", reinterpret_cast<const char*>(svg_${id})},\n")
endforeach()
file(
    CONFIGURE
    OUTPUT
    ${CMAKE_BINARY_DIR}/generated/icon_data.cpp
    CONTENT
    "// Generated from assets/icons by CMakeLists.txt.
#include \"text/icons.hpp\"

namespace lyren::text {
${icon_arrays}
const IconSource icon_sources[] = {
${icon_entries}};
const std::size_t icon_source_count = std::size(icon_sources);
}
"
    @ONLY)

# Headless services, playback and persistence.
pkg_check_modules(
    MEDIA
    REQUIRED
    IMPORTED_TARGET
    libcurl
    libcrypto
    zlib
    libmpg123
    flac
    fdk-aac
    vorbisfile
    libpipewire-0.3)
add_library(
    lyren_base STATIC
    src/core/log.cpp
    src/core/i18n.cpp
    src/core/loop.cpp
    src/core/utf8.cpp
    src/core/encoding.cpp
    src/core/crypto.cpp
    src/sys/paths.cpp
    src/sys/config.cpp
    src/sys/creds.cpp
    src/net/http.cpp
    src/net/fetch.cpp
    src/audio/spool.cpp
    src/audio/decoder.cpp
    src/audio/dec_mp3.cpp
    src/audio/dec_flac.cpp
    src/audio/dec_vorbis.cpp
    src/audio/dec_mp4.cpp
    src/audio/output.cpp
    src/audio/player.cpp
    src/ncm/crypto.cpp
    src/ncm/xeapi.cpp
    src/ncm/client.cpp
    src/ncm/netease.cpp
    src/qq/crypto.cpp
    src/qq/auth.cpp
    src/qq/client.cpp
    src/qq/qq.cpp
    src/qishui/client.cpp
    src/qishui/qishui.cpp
    src/qishui/media.cpp
    src/kugou/client.cpp
    src/kugou/kugou.cpp
    src/kugou/lyrics.cpp
    src/bodian/client.cpp
    src/bodian/bodian.cpp
    src/app/state.cpp
    src/media/types.cpp
    src/media/provider.cpp
    src/media/lyrics.cpp)
target_include_directories(lyren_base PUBLIC src)
target_link_libraries(lyren_base PUBLIC PkgConfig::MEDIA PRIVATE glaze)
target_compile_options(lyren_base PUBLIC -Wall -Wextra -Wno-missing-field-initializers --embed-dir=${CMAKE_SOURCE_DIR}/assets)
if(LYREN_NATIVE)
    target_compile_options(lyren_base PUBLIC $<$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>:-march=native>)
endif()

# Reusable window, rendering and UI code; tests do not need the application shell.
pkg_check_modules(UILIBS REQUIRED IMPORTED_TARGET libturbojpeg libwebp libwebpdemux)
find_package(GIF REQUIRED)
pkg_check_modules(QRENCODE REQUIRED IMPORTED_TARGET libqrencode)
pkg_check_modules(SDBUS REQUIRED IMPORTED_TARGET libsystemd)
add_library(
    lyren_ui STATIC
    ${CMAKE_BINARY_DIR}/generated/icon_data.cpp
    src/wl/display.cpp
    src/wl/input.cpp
    src/wl/clipboard.cpp
    src/wl/window.cpp
    src/wl/layer.cpp
    src/gfx/context.cpp
    src/gfx/renderer.cpp
    src/gfx/image.cpp
    src/text/fonts.cpp
    src/text/shaper.cpp
    src/text/atlas.cpp
    src/text/icons.cpp
    src/ui/theme.cpp
    src/ui/painter.cpp
    src/ui/motion.cpp
    src/ui/text_edit.cpp
    src/ui/ui.cpp
    src/ui/widgets.cpp
    src/ui/text_box.cpp
    src/app/frames.cpp
    src/app/images.cpp)
target_link_libraries(lyren_ui PUBLIC lyren_base PkgConfig::SYSLIBS vulkan_headers PRIVATE PkgConfig::UILIBS GIF::GIF nanosvg nexus)

add_library(
    lyren_lib STATIC
    src/sys/bus.cpp
    src/sys/mpris.cpp
    src/app/app.cpp
    src/app/shell.cpp
    src/app/account.cpp
    src/app/library.cpp
    src/app/catalog.cpp
    src/app/playback.cpp
    src/app/playbar.cpp
    src/app/pages.cpp
    src/app/settings.cpp
    src/app/stage.cpp
    src/app/panel.cpp
    src/app/profile.cpp
    src/app/posts.cpp
    src/app/desktop.cpp)
target_link_libraries(lyren_lib PUBLIC lyren_ui PkgConfig::SDBUS glaze PRIVATE PkgConfig::QRENCODE)
add_wayland_protocols(
    lyren_ui
    ${WP_DIR}/stable/xdg-shell/xdg-shell.xml
    ${WP_DIR}/stable/viewporter/viewporter.xml
    ${WP_DIR}/stable/tablet/tablet-v2.xml
    ${WP_DIR}/staging/cursor-shape/cursor-shape-v1.xml
    ${WP_DIR}/staging/fractional-scale/fractional-scale-v1.xml
    ${WP_DIR}/staging/xdg-activation/xdg-activation-v1.xml
    ${WP_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
    ${WP_DIR}/unstable/text-input/text-input-unstable-v3.xml
    ${CMAKE_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml)
add_shaders(lyren_ui shaders/ui.vert shaders/ui.frag)

add_executable(lyren src/main.cpp)
target_link_libraries(lyren PRIVATE lyren_lib)

add_executable(lyren-probe tools/lyren-probe.cpp)
target_link_libraries(lyren-probe PRIVATE lyren_base PkgConfig::QRENCODE glaze)

if(BUILD_TESTING)
    add_executable(
        lyren_tests
        tests/main.cpp
        tests/test_loop.cpp
        tests/test_result.cpp
        tests/test_config.cpp
        tests/test_i18n.cpp
        tests/test_text.cpp
        tests/test_utf8.cpp
        tests/test_encoding.cpp
        tests/test_ui.cpp
        tests/test_audio.cpp
        tests/test_http.cpp
        tests/test_ncm.cpp
        tests/test_xeapi.cpp
        tests/test_qq.cpp
        tests/test_kugou.cpp
        tests/test_bodian.cpp
        tests/test_qishui.cpp
        tests/test_provider.cpp
        tests/test_timeline.cpp
        tests/test_lyrics.cpp
        tests/test_image.cpp)
    target_link_libraries(lyren_tests PRIVATE lyren_ui doctest glaze)
    target_compile_definitions(lyren_tests PRIVATE LYREN_TEST_DATA="${CMAKE_SOURCE_DIR}/tests/data")
    add_test(NAME unit COMMAND lyren_tests)
    set_tests_properties(unit PROPERTIES ENVIRONMENT "XDG_CACHE_HOME=${CMAKE_BINARY_DIR}/test-cache;NO_PROXY=127.0.0.1;no_proxy=127.0.0.1")
    add_test(NAME no_alsa COMMAND ${CMAKE_COMMAND} "-DEXECUTABLES=$<TARGET_FILE:lyren>;$<TARGET_FILE:lyren-probe>" -P ${CMAKE_SOURCE_DIR}/tests/no_alsa.cmake)

    if(LYREN_TEST_GRAPHICS)
        add_executable(lyren_gfx_tests tests/main.cpp tests/test_gfx.cpp)
        target_link_libraries(lyren_gfx_tests PRIVATE lyren_lib doctest)
        find_program(LYREN_KWIN_WAYLAND kwin_wayland REQUIRED)
        add_test(NAME graphics COMMAND sh ${CMAKE_SOURCE_DIR}/tests/test_gfx.sh $<TARGET_FILE:lyren_gfx_tests> ${LYREN_KWIN_WAYLAND})
        set_tests_properties(graphics PROPERTIES TIMEOUT 30)
    endif()
endif()
