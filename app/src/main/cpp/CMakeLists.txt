cmake_minimum_required(VERSION 3.22.1)
project("stonxmanager" VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(STONX_SOURCES
    stonx_jni.cpp
    stonx_core.cpp
    stonx_protocol.cpp
    crypto/sha256.cpp
    transfer_engine.cpp
    file_ops.cpp
    smart_sync.cpp
    pending_queue.cpp
)

add_library(stonxmanager SHARED ${STONX_SOURCES})

find_library(log-lib     log)
find_library(android-lib android)
find_library(media-lib   mediandk)
find_library(z-lib       z)

target_include_directories(stonxmanager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto)

target_link_libraries(stonxmanager
    ${log-lib} ${android-lib}
    ${media-lib} ${z-lib})

target_compile_options(stonxmanager PRIVATE
    -O2 -fvisibility=hidden -fstack-protector-strong
    -Wall -Wextra -Wno-unused-parameter)
