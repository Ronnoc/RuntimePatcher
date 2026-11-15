cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" FEFW_VERSION LIMIT_COUNT 1)
project(fefw_class_foundation VERSION ${FEFW_VERSION} LANGUAGES C CXX)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(fefw_class_foundation STATIC
    src/game_table.cpp
    src/msbt_patcher.cpp
    src/bntx_patcher.cpp
    src/vfs_overlay.cpp
)

set_target_properties(fefw_class_foundation PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(fefw_class_foundation PUBLIC FEFW_VERSION="${FEFW_VERSION}")
target_include_directories(fefw_class_foundation PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# A parent project may supply its own zstd target (FEFW_ZSTD_TARGET); otherwise a pinned zstd is fetched.
set(FEFW_ZSTD_TARGET "" CACHE STRING "Existing zstd library target to link instead of fetching zstd")
if(FEFW_ZSTD_TARGET)
    target_link_libraries(fefw_class_foundation PUBLIC ${FEFW_ZSTD_TARGET})
else()
    include(FetchContent)
    if(DEFINED ENV{FEFW_DEPS_DIR})
        set(_fefw_deps_default "$ENV{FEFW_DEPS_DIR}")
    elseif(WIN32 AND DEFINED ENV{LOCALAPPDATA})
        set(_fefw_deps_default "$ENV{LOCALAPPDATA}/fefw/deps")
    elseif(APPLE AND DEFINED ENV{HOME})
        set(_fefw_deps_default "$ENV{HOME}/Library/Caches/fefw/deps")
    elseif(DEFINED ENV{XDG_CACHE_HOME})
        set(_fefw_deps_default "$ENV{XDG_CACHE_HOME}/fefw/deps")
    elseif(DEFINED ENV{HOME})
        set(_fefw_deps_default "$ENV{HOME}/.cache/fefw/deps")
    else()
        set(_fefw_deps_default "${CMAKE_BINARY_DIR}/_downloads")
    endif()
    # Same location as tools/build_emulator_mod.py, so both builds share one download.
    set(FEFW_DEPS_DIR "${_fefw_deps_default}" CACHE PATH "Where pinned third-party sources are cached")
    FetchContent_Declare(zstd
        URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
        DOWNLOAD_DIR "${FEFW_DEPS_DIR}"
        URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
        SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(zstd)
    file(GLOB FEFW_ZSTD_SOURCES "${zstd_SOURCE_DIR}/lib/common/*.c" "${zstd_SOURCE_DIR}/lib/compress/*.c" "${zstd_SOURCE_DIR}/lib/decompress/*.c")
    add_library(fefw_zstd STATIC ${FEFW_ZSTD_SOURCES})
    target_include_directories(fefw_zstd SYSTEM PUBLIC "${zstd_SOURCE_DIR}/lib")
    target_compile_definitions(fefw_zstd PRIVATE ZSTD_DISABLE_ASM)
    set_target_properties(fefw_zstd PROPERTIES C_STANDARD 11)
    target_link_libraries(fefw_class_foundation PUBLIC fefw_zstd)
endif()

option(FEFW_BUILD_TOOLS "Build fefw_table_dump" ON)
if(FEFW_BUILD_TOOLS)
    add_executable(fefw_table_dump tools/table_dump.cpp)
    target_link_libraries(fefw_table_dump PRIVATE fefw_class_foundation)
endif()

# Code-mod DLL for PC recompilation runtimes (mods/<name>/exefs/*.dll, see dll/nxmod_abi.h)
option(FEFW_BUILD_NXMOD_DLL "Build fefw_runtime_patcher (code-mod DLL)" ON)
if(FEFW_BUILD_NXMOD_DLL)
    add_library(fefw_runtime_patcher SHARED dll/nxmod_runtime_patcher.cpp)
    target_link_libraries(fefw_runtime_patcher PRIVATE fefw_class_foundation)
    set_target_properties(fefw_runtime_patcher PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden)
endif()

option(FEFW_RUNTIME_BUILD_TESTS "Build the runtime unit tests" ${PROJECT_IS_TOP_LEVEL})
if(FEFW_RUNTIME_BUILD_TESTS)
    enable_testing()
    add_executable(fefw_unit_tests tests/unit_tests.cpp)
    target_link_libraries(fefw_unit_tests PRIVATE fefw_class_foundation)
    add_test(NAME fefw_unit_tests COMMAND fefw_unit_tests)
endif()
