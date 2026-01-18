cmake_minimum_required(VERSION 3.31)
project(spritesheetc)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SPRITESHEETC_BUILD_CLI "Build spritesheetc command-line application" ON)

add_subdirectory(spritesheetc)

if(SPRITESHEETC_BUILD_CLI)
    add_subdirectory(spritesheetc-cli)
endif()

#if(CMAKE_JS_VERSION)
    add_subdirectory(spritesheetc-node)
#endif()
