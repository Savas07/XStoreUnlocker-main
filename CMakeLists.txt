cmake_minimum_required(VERSION 3.20)
project(XStoreAPIUnlocker VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(XGameRuntime SHARED
    src/dllmain.cpp
    src/proxy.cpp
    src/proxy.h
    src/store_hooks.cpp
    src/store_hooks.h
    src/config.cpp
    src/config.h
    src/logger.h
    src/xstore_types.h
)

set_target_properties(XGameRuntime PROPERTIES
    OUTPUT_NAME "XGameRuntime"
    PREFIX ""
    SUFFIX ".dll"
)

target_sources(XGameRuntime PRIVATE src/exports.def)

target_link_libraries(XGameRuntime PRIVATE
    kernel32
    user32
    advapi32
    shlwapi
)

target_compile_definitions(XGameRuntime PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)
