# HLSL fixtures for BasicRHISpirvLoweringTests: compiled with the shader ABI's DXC flags (BasicRHIShaderFlags.cmake) into
# ${CMAKE_CURRENT_BINARY_DIR}/spirv, which the test reads (BASICRHI_LOWERING_FIXTURE_DIR).

set(_lowering_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/spirv")
set(_lowering_fixture_outputs)

# basicrhi_compile_spirv's command plus extra DXC arguments (the engine-style fixture's register shifts).
function(_basicrhi_lowering_fixture NAME PROFILE)
    cmake_parse_arguments(ARG "" "SOURCE" "EXTRA_ARGS" ${ARGN})
    set(_source "${CMAKE_CURRENT_SOURCE_DIR}/${NAME}.hlsl")
    if(ARG_SOURCE)
        set(_source "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}")
    endif()
    set(_output "${_lowering_fixture_dir}/${NAME}.spv")
    if(ARG_EXTRA_ARGS)
        if(NOT BASICRHI_DXC_EXECUTABLE)
            message(FATAL_ERROR "BasicRHISpirvLoweringTests: DXC not found; install the Vulkan SDK or set BASICRHI_DXC_EXECUTABLE")
        endif()
        add_custom_command(
            OUTPUT "${_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_lowering_fixture_dir}"
            COMMAND "${BASICRHI_DXC_EXECUTABLE}" -nologo -HV 2021 -E main -T ${PROFILE} ${BASICRHI_VULKAN_DXC_FLAGS}
                -D BASICRHI_SHADER_API_VULKAN=1 ${ARG_EXTRA_ARGS} -Fo "${_output}" "${_source}"
            DEPENDS "${_source}"
            COMMENT "SPIR-V main <- ${_source}"
            VERBATIM)
    else()
        basicrhi_compile_spirv(OUTPUT "${_output}" SOURCE "${_source}" ENTRY main PROFILE ${PROFILE})
    endif()
    set(_lowering_fixture_outputs ${_lowering_fixture_outputs} "${_output}" PARENT_SCOPE)
endfunction()

_basicrhi_lowering_fixture(heap_types cs_6_6)
_basicrhi_lowering_fixture(root_constants vs_6_6)
_basicrhi_lowering_fixture(spill cs_6_6)
_basicrhi_lowering_fixture(indirect ps_6_6)
_basicrhi_lowering_fixture(heap_slots cs_6_6)
_basicrhi_lowering_fixture(combined_sampler ps_6_6)
# A library with two entry points (-E is ignored for lib targets).
_basicrhi_lowering_fixture(two_entries lib_6_6)
# NonSemantic.Shader.DebugInfo.100 debug info referencing the resource variables.
_basicrhi_lowering_fixture(indirect_debug ps_6_6 SOURCE indirect.hlsl EXTRA_ARGS -fspv-debug=vulkan-with-source)
_basicrhi_lowering_fixture(dclf_engine ps_6_6
    EXTRA_ARGS -fvk-b-shift 0 0 -fvk-t-shift 200 0 -fvk-s-shift 400 0 -fvk-u-shift 500 0)

add_custom_target(BasicRHISpirvLoweringFixtures DEPENDS ${_lowering_fixture_outputs})
add_dependencies(BasicRHISpirvLoweringTests BasicRHISpirvLoweringFixtures)
target_compile_definitions(BasicRHISpirvLoweringTests PRIVATE BASICRHI_LOWERING_FIXTURE_DIR="${_lowering_fixture_dir}")
