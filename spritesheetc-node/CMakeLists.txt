add_compile_definitions(NAPI_VERSION=8)

add_library(spritesheetc-node SHARED SpritesheetcAddon.cpp)

set_target_properties(spritesheetc-node PROPERTIES
    OUTPUT_NAME spritesheetc
    PREFIX ""
    SUFFIX ".node"
)

# Lets regular CMake see the headers, useful for debugging in an IDE
# Requires NODE_ROOT_DIR to be defined (-DNODE_ROOT_DIR=...)
if(NOT DEFINED CMAKE_JS_INC)
    set(CMAKE_JS_INC
        "../node_modules/node-addon-api"
        "${NODE_ROOT_DIR}/include/node"
    )
endif()
target_include_directories(spritesheetc-node PRIVATE ${CMAKE_JS_INC})

target_link_libraries(spritesheetc-node PUBLIC spritesheetc)

if(MSVC AND CMAKE_JS_NODELIB_DEF AND CMAKE_JS_NODELIB_TARGET)
    # Generate node.lib
    execute_process(COMMAND ${CMAKE_AR} /def:${CMAKE_JS_NODELIB_DEF} /out:${CMAKE_JS_NODELIB_TARGET} ${CMAKE_STATIC_LINKER_FLAGS})
endif()
