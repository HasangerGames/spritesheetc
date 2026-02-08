set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (SPRITESHEETC_STATIC)
    add_library(spritesheetc STATIC spritesheetc.cpp)
else()
    add_library(spritesheetc SHARED spritesheetc.cpp)
endif()

target_include_directories(spritesheetc INTERFACE ${CMAKE_CURRENT_LIST_DIR})

include(FetchContent)

# basis_universal
FetchContent_Declare(
    basis_universal
    GIT_REPOSITORY https://github.com/BinomialLLC/basis_universal.git
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
set(BASISU_EXAMPLES FALSE CACHE BOOL "" FORCE)
set(BASISU_STATIC   TRUE  CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(basis_universal)
target_include_directories(spritesheetc SYSTEM PRIVATE "${basis_universal_SOURCE_DIR}/encoder")

# BS_thread_pool
FetchContent_Declare(
    bshoshany_thread_pool
    GIT_REPOSITORY https://github.com/bshoshany/thread-pool.git
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
)
FetchContent_MakeAvailable(bshoshany_thread_pool)
add_library(BS_thread_pool INTERFACE)
target_include_directories(BS_thread_pool INTERFACE ${bshoshany_thread_pool_SOURCE_DIR}/include)

# glaze
FetchContent_Declare(
    glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glaze)

# zlib-ng
set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    zlib-ng
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
    GIT_TAG develop
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(zlib-ng)
set(ZLIB_FOUND TRUE)
if (WIN32)
    set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/${CMAKE_BUILD_TYPE}/zlibstatic.lib")
else()
    set(ZLIB_LIBRARY "${zlib_BINARY_DIR}/libz.a")
endif()
set(ZLIB_INCLUDE_DIR "${zlib_BINARY_DIR}")

# libspng
set(BUILD_EXAMPLES OFF CACHE INTERNAL "" FORCE)
set(SPNG_SHARED    OFF CACHE INTERNAL "" FORCE)
FetchContent_Declare(
    libspng
    GIT_REPOSITORY https://github.com/randy408/libspng.git
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libspng)

# libwebp
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
FetchContent_Declare(
    libwebp
    GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebp
    GIT_TAG main
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(libwebp)

# rectpack2D
FetchContent_Declare(
    rectpack2D
    GIT_REPOSITORY https://github.com/TeamHypersomnia/rectpack2D.git
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(rectpack2D)

# resvg
FetchContent_Declare(
    resvg
    GIT_REPOSITORY https://github.com/linebender/resvg.git
    GIT_TAG main
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(resvg)
set(ENV{CARGO_PROFILE_RELEASE_LTO} "true")
set(ENV{CARGO_PROFILE_RELEASE_STRIP} "true")
execute_process(
    COMMAND cargo build --release
    WORKING_DIRECTORY "${resvg_SOURCE_DIR}/crates/c-api"
)
add_library(resvg STATIC IMPORTED)
if (WIN32)
    set_target_properties(resvg PROPERTIES IMPORTED_LOCATION "${resvg_SOURCE_DIR}/target/release/resvg.lib")
else()
    set_target_properties(resvg PROPERTIES IMPORTED_LOCATION "${resvg_SOURCE_DIR}/target/release/libresvg.a")
endif()
set_target_properties(resvg PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${resvg_SOURCE_DIR}/crates/c-api"
    LINKER_LANGUAGE CXX
)

# xxHash
FetchContent_Declare(
    xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG dev
    GIT_SHALLOW TRUE
)
set(BUILD_SHARED_LIBS OFF CACHE INTERNAL "" FORCE)
set(XXHASH_BUILD_XXHSUM OFF CACHE INTERNAL "" FORCE)
FetchContent_MakeAvailable(xxhash)
# for some reason the CI throws a fit if we don't force CMake to load the project
add_subdirectory(
    "${xxhash_SOURCE_DIR}/build/cmake"
    "${xxhash_BINARY_DIR}"
)

target_link_libraries(spritesheetc PRIVATE
    basisu_encoder
    BS_thread_pool
    glaze::glaze
    zlib-ng-static
    spng_static
    webp
    rectpack2D::rectpack2D
    resvg
    xxhash
)
