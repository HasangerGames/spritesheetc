add_executable(spritesheetc-cli main.cpp)

set_target_properties(spritesheetc-cli PROPERTIES OUTPUT_NAME spritesheetc)

target_link_libraries(spritesheetc-cli PRIVATE spritesheetc)

set(CMAKE_CXX_FLAGS_RELEASE "-Ofast -DNDEBUG -flto=auto -funroll-loops -fprefetch-loop-arrays -frename-registers")
