cmake_minimum_required(VERSION 3.16)
project(BasicRHI VERSION 1.0.0 LANGUAGES CXX)

# Require C++17
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)

option(BASICRHI_ENABLE_STREAMLINE "Enable Streamline integration in BasicRHI" ON)
option(BASICRHI_ENABLE_PIX "Enable PIX integration in BasicRHI" ON)
option(BASICRHI_ENABLE_RESHAPE "Enable GPU-Reshape debug instrumentation scaffolding in BasicRHI" OFF)
set(BASICRHI_STREAMLINE_HEADERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/Streamline" CACHE PATH "Directory containing Streamline headers (e.g. sl.h)")
set(BASICRHI_PIX_HEADERS_DIR "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/pix" CACHE PATH "Directory containing PIX headers (e.g. pix3.h)")

find_package(DirectX-Headers CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(DirectX12-Agility CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

set(_basicrhi_streamline_available OFF)
set(_basicrhi_pix_available OFF)
set(_basicrhi_reshape_available OFF)
set(_basicrhi_streamline_manual_include "")
set(_basicrhi_pix_manual_include "")

if(BASICRHI_ENABLE_RESHAPE)
    if(TARGET GRS.Backends.DX12.Layer)
        set(_basicrhi_reshape_available ON)
        message(STATUS "BasicRHI: GPU-Reshape runtime resolved from in-tree GPU-Reshape targets")
    else()
        message(WARNING "BasicRHI: BASICRHI_ENABLE_RESHAPE was requested, but GPU-Reshape targets are unavailable. ReShape support will be disabled.")
    endif()
endif()

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
    rhi_imgui_widgets.cpp
    ImGuiColorTextEdit/TextEditor.cpp
    "rhi_interop.h" 
    "rhi_reshape_shared.h"
    "rhi_dx12.h"  
    "rhi_interop_dx12.h" 
    "resource_states.h" 
    "rhi_helpers.h" 
    "rhi_interop_dx12.cpp" 
    "rhi_conversions_dx12.h" 
    "rhi_debug.h"
    "rhi_imgui_widgets.h"
    "ImGuiColorTextEdit/TextEditor.h"
    "rhi_colors.h"
    "rhi_allocator.h" 
    "rhi_allocator.cpp" "rhi_feature_info.h" "rhi_dx12_casting.h")

target_compile_options(BasicRHI PRIVATE /WX)
target_compile_options(BasicRHI PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
# Clang warns about things MSVC silently accepts; demote to warnings (keep /WX for everything else)
target_compile_options(BasicRHI PRIVATE $<$<CXX_COMPILER_ID:Clang>:
    -Wno-error=switch
    -Wno-error=reorder-ctor
    -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable
    -Wno-error=unused-function
    -Wno-error=misleading-indentation
    -Wno-error=nonportable-include-path
>)
add_library(BasicRHI::BasicRHI ALIAS BasicRHI)

target_link_libraries(BasicRHI PRIVATE imgui::imgui)

if(_basicrhi_streamline_available)
    if(TARGET Streamline::headers)
        target_link_libraries(BasicRHI PRIVATE Streamline::headers)
    else()
        target_include_directories(BasicRHI PRIVATE "${_basicrhi_streamline_manual_include}")
    endif()
endif()

if(_basicrhi_pix_available)
    if(TARGET Pix::headers)
        target_link_libraries(BasicRHI PUBLIC Pix::headers)
    else()
        target_include_directories(BasicRHI PUBLIC "${_basicrhi_pix_manual_include}")
    endif()
endif()

if(_basicrhi_reshape_available)
    set(_basicrhi_reshape_source_root "${CMAKE_CURRENT_LIST_DIR}/../GPU-Reshape/Source")
    set(_basicrhi_reshape_binary_root "${CMAKE_BINARY_DIR}/GPU-Reshape/Source")
    set(_basicrhi_reshape_common_include_dirs
        "${_basicrhi_reshape_source_root}/Features/Descriptor/Backend/Include"
        "${_basicrhi_reshape_binary_root}/Features/Descriptor/Backend/Include"
        "${_basicrhi_reshape_source_root}/Features/ResourceBounds/Backend/Include"
        "${_basicrhi_reshape_binary_root}/Features/ResourceBounds/Backend/Include"
        "${_basicrhi_reshape_source_root}/Libraries/Backend/Include"
        "${_basicrhi_reshape_binary_root}/Libraries/Backend/Include"
        "${_basicrhi_reshape_source_root}/Libraries/Message/Include"
        "${_basicrhi_reshape_binary_root}/Libraries/Message/Include"
        "${_basicrhi_reshape_source_root}/Libraries/Common/Include"
        "${_basicrhi_reshape_source_root}/Libraries/Bridge/Include"
        "${_basicrhi_reshape_binary_root}/Libraries/Bridge/Include"
        "${_basicrhi_reshape_source_root}/Services/HostResolver/Include"
        "${CMAKE_BINARY_DIR}/External/include"
        "${CMAKE_CURRENT_LIST_DIR}/../GPU-Reshape/ThirdParty/MD5/Include"
    )
    set(_basicrhi_reshape_dx12_include_dirs
        "${_basicrhi_reshape_source_root}/Backends/DX12/Layer/Include"
        "${_basicrhi_reshape_binary_root}/Backends/DX12/Layer/Include"
    )
    get_target_property(_basicrhi_fmt_include_dirs fmt::fmt INTERFACE_INCLUDE_DIRECTORIES)

    if(_basicrhi_fmt_include_dirs)
        target_include_directories(BasicRHI BEFORE PRIVATE ${_basicrhi_fmt_include_dirs})
    endif()

    target_include_directories(BasicRHI PRIVATE
        ${_basicrhi_reshape_common_include_dirs}
        ${_basicrhi_reshape_dx12_include_dirs}
    )

    target_link_libraries(BasicRHI PUBLIC
        $<BUILD_INTERFACE:GRS.Libraries.Backend>
    )
endif()

# Public header(s)
set_target_properties(BasicRHI PROPERTIES
    PUBLIC_HEADER "rhi.h;rhi_debug.h;rhi_imgui_widgets.h"
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
    fmt::fmt
    spdlog::spdlog_header_only
)

target_compile_definitions(BasicRHI PUBLIC
    BASICRHI_ENABLE_STREAMLINE=$<IF:$<BOOL:${_basicrhi_streamline_available}>,1,0>
    BASICRHI_ENABLE_PIX=$<IF:$<BOOL:${_basicrhi_pix_available}>,1,0>
    BASICRHI_ENABLE_RESHAPE=$<IF:$<BOOL:${_basicrhi_reshape_available}>,1,0>
)