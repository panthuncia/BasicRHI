cmake_minimum_required(VERSION 3.16)
project(BasicRHI VERSION 1.0.0 LANGUAGES CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

option(BASICRHI_ENABLE_STREAMLINE "Enable Streamline integration in BasicRHI" ON)
option(BASICRHI_ENABLE_PIX "Enable PIX integration in BasicRHI" ON)
set(BASICRHI_STREAMLINE_HEADERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/Streamline" CACHE PATH "Directory containing Streamline headers (e.g. sl.h)")
set(BASICRHI_PIX_HEADERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/pix" CACHE PATH "Directory containing PIX headers (e.g. pix3.h)")

find_package(DirectX-Headers CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(DirectX12-Agility CONFIG REQUIRED)

set(_basicrhi_streamline_available OFF)
set(_basicrhi_pix_available OFF)
set(_basicrhi_streamline_manual_include "")
set(_basicrhi_pix_manual_include "")

if(BASICRHI_ENABLE_STREAMLINE)
    if(NOT TARGET Streamline::headers)
        find_package(Streamline CONFIG QUIET)
    endif()

    if(TARGET Streamline::headers)
        set(_basicrhi_streamline_available ON)
        message(STATUS "BasicRHI: Streamline headers resolved via target Streamline::headers")
    elseif(EXISTS "${BASICRHI_STREAMLINE_HEADERS_DIR}/sl.h")
        set(_basicrhi_streamline_available ON)
        set(_basicrhi_streamline_manual_include "${BASICRHI_STREAMLINE_HEADERS_DIR}")
        message(STATUS "BasicRHI: Streamline headers resolved from BASICRHI_STREAMLINE_HEADERS_DIR=${BASICRHI_STREAMLINE_HEADERS_DIR}")
    else()
        message(WARNING "BasicRHI: Streamline headers were not found. Disabling Streamline support. Set BASICRHI_STREAMLINE_HEADERS_DIR to override.")
    endif()
endif()

if(BASICRHI_ENABLE_PIX)
    if(NOT TARGET Pix::headers)
        find_package(Pix CONFIG QUIET)
    endif()

    if(TARGET Pix::headers)
        set(_basicrhi_pix_available ON)
        message(STATUS "BasicRHI: PIX headers resolved via target Pix::headers")
    elseif(EXISTS "${BASICRHI_PIX_HEADERS_DIR}/pix3.h")
        set(_basicrhi_pix_available ON)
        set(_basicrhi_pix_manual_include "${BASICRHI_PIX_HEADERS_DIR}")
        message(STATUS "BasicRHI: PIX headers resolved from BASICRHI_PIX_HEADERS_DIR=${BASICRHI_PIX_HEADERS_DIR}")
    else()
        message(WARNING "BasicRHI: PIX headers were not found. Disabling PIX support. Set BASICRHI_PIX_HEADERS_DIR to override.")
    endif()
endif()

# Build a static library for now.
add_library(BasicRHI STATIC 
    rhi_dx12.cpp
    "rhi_interop.h" 
    "rhi_dx12.h"  
    "rhi_interop_dx12.h" 
    "resource_states.h" 
    "rhi_helpers.h" 
    "rhi_interop_dx12.cpp" 
    "rhi_conversions_dx12.h" 
    "rhi_debug.h"
    "rhi_colors.h"
    "rhi_allocator.h" 
    "rhi_allocator.cpp" "rhi_feature_info.h" "rhi_dx12_casting.h")

target_compile_options(BasicRHI PRIVATE /WX)
target_compile_options(BasicRHI PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
add_library(BasicRHI::BasicRHI ALIAS BasicRHI)

if(_basicrhi_streamline_available)
    if(TARGET Streamline::headers)
        target_link_libraries(BasicRHI PRIVATE Streamline::headers)
    else()
        target_include_directories(BasicRHI PRIVATE "${_basicrhi_streamline_manual_include}")
    endif()
endif()

if(_basicrhi_pix_available)
    if(TARGET Pix::headers)
        target_link_libraries(BasicRHI PRIVATE Pix::headers)
    else()
        target_include_directories(BasicRHI PRIVATE "${_basicrhi_pix_manual_include}")
    endif()
endif()

# Public header(s)
set_target_properties(BasicRHI PROPERTIES
    PUBLIC_HEADER "rhi.h"
)

# Set sources
target_sources(BasicRHI PRIVATE
    rhi_dx12.cpp
    rhi_interop_dx12.cpp
)

# Public include paths:
# - During build (add_subdirectory consumers): this source dir
# - After install (find_package consumers): ${prefix}/include
target_include_directories(BasicRHI
    PUBLIC
        ${DIRECTX_HEADERS_INCLUDE_DIR}
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/BasicRHI>
)

# --- Installation ---

# Install the library and its public headers to standard dirs
install(TARGETS BasicRHI
    EXPORT BasicRHITargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}   # .dll on Windows
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}   # .so on Linux / .dylib on macOS
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}   # import lib / static lib if ever built as such
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/BasicRHI
)

# Export a targets file so dependents can do find_package(BasicRHI CONFIG)
install(EXPORT BasicRHITargets
    NAMESPACE BasicRHI::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BasicRHI
)

# Optionally generate a very simple package config (so find_package(BasicRHI CONFIG) works)
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/BasicRHIConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BasicRHIConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/BasicRHIConfig.cmake"
    PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/BasicRHI"
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/BasicRHIConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/BasicRHIConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/BasicRHI"
)

target_link_libraries(BasicRHI
PUBLIC
    Microsoft::DirectX-Headers
PRIVATE 
    Microsoft::DirectX12-Agility 
    spdlog::spdlog_header_only
)

target_compile_definitions(BasicRHI PUBLIC
    BASICRHI_ENABLE_STREAMLINE=$<IF:$<BOOL:${_basicrhi_streamline_available}>,1,0>
    BASICRHI_ENABLE_PIX=$<IF:$<BOOL:${_basicrhi_pix_available}>,1,0>
)