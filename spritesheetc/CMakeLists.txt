add_library(spritesheetc SHARED spritesheetc.cpp)

target_include_directories(spritesheetc INTERFACE ${CMAKE_CURRENT_LIST_DIR})

set(EXAMPLES FALSE)
set(STATIC TRUE)
add_subdirectory(vendored/basis_universal EXCLUDE_FROM_ALL)
set(BASISU_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/vendored/basis_universal/encoder")
target_include_directories(spritesheetc SYSTEM PRIVATE ${BASISU_INCLUDE_DIR})

add_subdirectory(vendored/concurrentqueue EXCLUDE_FROM_ALL)

add_subdirectory(vendored/glaze EXCLUDE_FROM_ALL)

set(BUILD_EXAMPLES OFF CACHE INTERNAL "" FORCE)
set(SPNG_SHARED    OFF CACHE INTERNAL "" FORCE)
add_subdirectory(vendored/libspng EXCLUDE_FROM_ALL)

#add_library(libvips STATIC IMPORTED)
#set(LIBVIPS_DIR "${CMAKE_CURRENT_LIST_DIR}/vendored/libvips")
#set_target_properties(libvips PROPERTIES
#    IMPORTED_LOCATION "${LIBVIPS_DIR}/build/libvips/libvips.a"
#    INTERFACE_INCLUDE_DIRECTORIES "${LIBVIPS_DIR}/libvips/include"
#    LINKER_LANGUAGE CXX
#)

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
add_subdirectory(vendored/libwebp EXCLUDE_FROM_ALL)

add_subdirectory(vendored/rectpack2D EXCLUDE_FROM_ALL)

add_library(resvg STATIC IMPORTED)
set(RESVG_DIR "${CMAKE_CURRENT_LIST_DIR}/vendored/resvg")
set_target_properties(resvg PROPERTIES
        IMPORTED_LOCATION "${RESVG_DIR}/target/release/libresvg.a"
        INTERFACE_INCLUDE_DIRECTORIES "${RESVG_DIR}/crates/c-api"
        LINKER_LANGUAGE CXX
)

add_subdirectory(vendored/xxHash/build/cmake EXCLUDE_FROM_ALL)

set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POSITION_INDEPENDENT_CODE ON) # keeps zlib-ng from blowing up when compiling the node addon
add_subdirectory(vendored/zlib-ng EXCLUDE_FROM_ALL)

target_link_libraries(spritesheetc PUBLIC
    basisu_encoder
    concurrentqueue
    glaze::glaze
    zlib-ng-static
    spng_static
    webp
    rectpack2D::rectpack2D
    resvg
    xxhash
)
