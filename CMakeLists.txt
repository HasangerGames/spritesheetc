cmake_minimum_required(VERSION 3.31)
project(spritesheetc)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(SPRITESHEETC_STATIC "Build as static library" ON)

add_subdirectory(spritesheetc)

if(CMAKE_JS_VERSION OR NODE_ROOT_DIR)
    add_subdirectory(spritesheetc-node)
endif()
