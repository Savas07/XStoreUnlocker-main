cmake_minimum_required(VERSION 3.15)
project(XStoreUnlocker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(XGameRuntime SHARED
    dllmain.cpp
    proxy.cpp
    store_hooks.cpp
    config.cpp
    logger.cpp
    exports.def
)

target_compile_definitions(XGameRuntime PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
