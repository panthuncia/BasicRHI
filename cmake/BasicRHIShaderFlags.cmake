# Build-time counterpart of rhi::AppendVulkanDxcSpirvArguments (rhi.h).
#
# Sets BASICRHI_VULKAN_DXC_FLAGS to the DXC arguments every SPIR-V shader consumed by
# BasicRHI's Vulkan backend must be compiled with. The descriptor-heap bindings are read
# from rhi.h so the header stays the single source of truth.
#
# Also provides:
#   basicrhi_compile_spirv(OUTPUT <file.spv> SOURCE <file.hlsl> ENTRY <name> PROFILE <cs_6_6>
#                          [DXC <path>] [DEFINES A=1 B ...] [INCLUDE_DIRS ...] [DEPENDS ...])
# which adds a custom command producing <file.spv>.

include_guard(GLOBAL)

set(_basicrhi_rhi_header "${CMAKE_CURRENT_LIST_DIR}/../rhi.h")
file(STRINGS "${_basicrhi_rhi_header}" _basicrhi_heap_lines REGEX "VULKAN_[A-Z_]*(HEAP_SET|HEAP_BINDING) = [0-9]+;")
foreach(_line IN LISTS _basicrhi_heap_lines)
    if(_line MATCHES "(VULKAN_[A-Z_]+) = ([0-9]+);")
        set(_basicrhi_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
    endif()
endforeach()
foreach(_name VULKAN_DESCRIPTOR_HEAP_SET VULKAN_RESOURCE_DESCRIPTOR_HEAP_BINDING
        VULKAN_SAMPLER_DESCRIPTOR_HEAP_BINDING VULKAN_COUNTER_DESCRIPTOR_HEAP_BINDING)
    if(NOT DEFINED _basicrhi_${_name})
        message(FATAL_ERROR "BasicRHIShaderFlags: could not read ${_name} from ${_basicrhi_rhi_header}")
    endif()
endforeach()

set(BASICRHI_VULKAN_DXC_FLAGS
    -spirv
    -fvk-use-dx-layout
    -fspv-target-env=vulkan1.3
    -fvk-bind-resource-heap ${_basicrhi_VULKAN_RESOURCE_DESCRIPTOR_HEAP_BINDING} ${_basicrhi_VULKAN_DESCRIPTOR_HEAP_SET}
    -fvk-bind-sampler-heap ${_basicrhi_VULKAN_SAMPLER_DESCRIPTOR_HEAP_BINDING} ${_basicrhi_VULKAN_DESCRIPTOR_HEAP_SET}
    -fvk-bind-counter-heap ${_basicrhi_VULKAN_COUNTER_DESCRIPTOR_HEAP_BINDING} ${_basicrhi_VULKAN_DESCRIPTOR_HEAP_SET})

# -fvk-bind-*-heap needs a recent DXC; the Vulkan SDK's copy is the default.
find_program(BASICRHI_DXC_EXECUTABLE dxc
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
    DOC "DXC used for build-time SPIR-V compilation")

function(basicrhi_compile_spirv)
    cmake_parse_arguments(ARG "" "OUTPUT;SOURCE;ENTRY;PROFILE;DXC" "DEFINES;INCLUDE_DIRS;DEPENDS" ${ARGN})
    foreach(_required OUTPUT SOURCE ENTRY PROFILE)
        if(NOT ARG_${_required})
            message(FATAL_ERROR "basicrhi_compile_spirv: ${_required} is required")
        endif()
    endforeach()
    set(_dxc "${ARG_DXC}")
    if(NOT _dxc)
        set(_dxc "${BASICRHI_DXC_EXECUTABLE}")
    endif()
    if(NOT _dxc)
        message(FATAL_ERROR "basicrhi_compile_spirv: DXC not found; install the Vulkan SDK or set BASICRHI_DXC_EXECUTABLE")
    endif()
    set(_args -nologo -HV 2021 -E ${ARG_ENTRY} -T ${ARG_PROFILE} ${BASICRHI_VULKAN_DXC_FLAGS} -D BASICRHI_SHADER_API_VULKAN=1)
    foreach(_define IN LISTS ARG_DEFINES)
        list(APPEND _args -D ${_define})
    endforeach()
    foreach(_dir IN LISTS ARG_INCLUDE_DIRS)
        list(APPEND _args -I "${_dir}")
    endforeach()
    get_filename_component(_outDir "${ARG_OUTPUT}" DIRECTORY)
    add_custom_command(
        OUTPUT "${ARG_OUTPUT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_outDir}"
        COMMAND "${_dxc}" ${_args} -Fo "${ARG_OUTPUT}" "${ARG_SOURCE}"
        DEPENDS "${ARG_SOURCE}" ${ARG_DEPENDS}
        COMMENT "SPIR-V ${ARG_ENTRY} <- ${ARG_SOURCE}"
        VERBATIM)
endfunction()
