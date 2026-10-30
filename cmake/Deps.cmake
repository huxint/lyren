# Build-time-only dependencies (headers, protocol XML) are fetched and pinned
# here. Runtime libraries come from the system (pkg-config).
include(FetchContent)

FetchContent_Declare(
  vulkan_headers
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
  URL_HASH
    SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e
  SOURCE_SUBDIR none)
FetchContent_Declare(
  wayland_protocols
  URL https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/1.49/downloads/wayland-protocols-1.49.tar.xz
  URL_HASH
    SHA256=ec4c8f74942d6dff7ace8b4ce4764f0ef9ff618a935d974ea77edee2ad240b14
  SOURCE_SUBDIR none)
FetchContent_Declare(
  glaze
  URL https://github.com/stephenberry/glaze/archive/refs/tags/v9.0.0.tar.gz
  URL_HASH
    SHA256=dd7b033c5bf6e4308615bb7834c541291d5b20f64121d9a68222a39414635517
  SOURCE_SUBDIR none)
FetchContent_Declare(
  nanosvg
  URL https://github.com/memononen/nanosvg/archive/239e102ec2c691f2902e20ace2ed36ee4a35cfe6.tar.gz
  URL_HASH
    SHA256=2bc68bdb518d7800252042e5cad50a0ab321596f0cbf49ef2a752926329063d2
  SOURCE_SUBDIR none)
FetchContent_Declare(
  doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
  URL_HASH
    SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
  SOURCE_SUBDIR none)
FetchContent_Declare(
  nexus
  URL https://github.com/huxint/nexus/archive/28946ed94718047baee966b34237a12efe961115.tar.gz
  URL_HASH
    SHA256=3e851022efb502b35b574675c011b58b8847870c6fbe95f8ebab1ef42ad0198f
  SOURCE_SUBDIR none)
FetchContent_MakeAvailable(vulkan_headers wayland_protocols glaze nanosvg
                           doctest nexus)

add_library(vulkan_headers INTERFACE)
target_include_directories(vulkan_headers SYSTEM
                           INTERFACE ${vulkan_headers_SOURCE_DIR}/include)
add_library(glaze INTERFACE)
target_include_directories(glaze SYSTEM INTERFACE ${glaze_SOURCE_DIR}/include)
add_library(nanosvg INTERFACE)
target_include_directories(nanosvg SYSTEM INTERFACE ${nanosvg_SOURCE_DIR}/src)
add_library(doctest INTERFACE)
target_include_directories(doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})

find_package(Threads REQUIRED)
add_library(nexus INTERFACE)
target_include_directories(nexus SYSTEM INTERFACE ${nexus_SOURCE_DIR}/include)
target_link_libraries(nexus INTERFACE Threads::Threads)
target_compile_options(nexus INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-fcontracts>
  $<$<COMPILE_LANGUAGE:CXX>:$<IF:$<CONFIG:Debug>,-fcontract-evaluation-semantic=enforce,-fcontract-evaluation-semantic=ignore>>)
target_link_options(nexus INTERFACE -fcontracts)

find_package(PkgConfig REQUIRED)
pkg_check_modules(
  SYSLIBS
  REQUIRED
  IMPORTED_TARGET
  vulkan
  wayland-client
  xkbcommon
  freetype2
  harfbuzz
  fontconfig
  libpng)
