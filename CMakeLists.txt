cmake_minimum_required(VERSION 3.31)
project(spritesheetc)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-Ofast -DNDEBUG -flto -funroll-loops -fprefetch-loop-arrays -frename-registers")

add_executable(spritesheetc src/spritesheetc.cpp)

set(EXAMPLES FALSE)
set(STATIC TRUE)
add_subdirectory(vendored/basis_universal)
target_include_directories(spritesheetc PRIVATE "${CMAKE_SOURCE_DIR}/vendored/basis_universal/encoder")

add_subdirectory(vendored/concurrentqueue)

add_subdirectory(vendored/glaze)

set(WEBP_BUILD_ANIM_UTILS OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_CWEBP      OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_DWEBP      OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_GIF2WEBP   OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_IMG2WEBP   OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_VWEBP      OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_WEBPINFO   OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_LIBWEBPMUX OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_WEBPMUX    OFF CACHE INTERNAL "" FORCE)
set(WEBP_BUILD_EXTRAS     OFF CACHE INTERNAL "" FORCE)
add_subdirectory(vendored/libwebp)

add_subdirectory(vendored/rectpack2D)

add_library(resvg STATIC IMPORTED)
set(RESVG_DIR "${CMAKE_SOURCE_DIR}/vendored/resvg")
set_target_properties(resvg PROPERTIES
    IMPORTED_LOCATION "${RESVG_DIR}/target/release/libresvg.a"
    INTERFACE_INCLUDE_DIRECTORIES "${RESVG_DIR}/crates/c-api"
    LINKER_LANGUAGE CXX
)

target_link_libraries(spritesheetc PRIVATE
    basisu_encoder
    concurrentqueue
    glaze::glaze
    webp
    rectpack2D::rectpack2D
    resvg
)
