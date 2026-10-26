# BasicRHI

`BasicRHI` is the low-level rendering hardware abstraction used by `OpenRenderGraph` and `BasicRenderer`.

## Tracy GPU profiling

`BASICRHI_ENABLE_TRACY_GPU_PROFILING` is enabled by default. It adds D3D12 and
Vulkan GPU timestamp contexts without exposing native API objects:

- `Queue::InitializeTracyGpuContext()` creates one named Tracy context for that
  queue.
- `Queue::TracyGpuFrameBegin()` advances timestamp collection once per frame.
- `CommandList::BeginTracyGpuZone()` / `EndTracyGpuZone()` record a transient
  GPU range against a specific queue context.

OpenRenderGraph uses these APIs automatically, including for additional queue
instances of the same queue kind. GPU zones are attributed to their queue
context rather than to the CPU worker that recorded the command list, producing
one GPU timeline per API queue even during parallel command-list recording.
Timestamp commands are recorded with the pass, while Tracy zone metadata is
deferred until submission and emitted in command-list order. This preserves
Tracy's zone nesting without serializing parallel command-list recording.
Disable the CMake option to build BasicRHI without the Tracy dependency; the
profiling entry points then return `rhi::Result::Unsupported`.

## CMake target

- Exported target: `BasicRHI::BasicRHI`

## Configuration options

- `BASICRHI_ENABLE_STREAMLINE` (default `ON`)
- `BASICRHI_ENABLE_PIX` (default `ON`)
- `BASICRHI_STREAMLINE_HEADERS_DIR` (default `../ThirdParty/Streamline`)
- `BASICRHI_PIX_HEADERS_DIR` (default `../ThirdParty/pix`)

Core dependencies are expected via package manager (for example vcpkg):

- `directx-headers`
- `directx12-agility`
- `spdlog`

PIX and Streamline are resolved in this order:

1. Existing CMake target (`Pix::headers` / `Streamline::headers`)
2. `find_package(Pix CONFIG)` / `find_package(Streamline CONFIG)`
3. Manual header folder path (`BASICRHI_PIX_HEADERS_DIR`, `BASICRHI_STREAMLINE_HEADERS_DIR`)

If headers are not found, support is disabled automatically with a warning.

For explicit disable:

```powershell
-DBASICRHI_ENABLE_STREAMLINE=OFF -DBASICRHI_ENABLE_PIX=OFF
```

## Standalone build and install

`BasicRHI` now uses CMake presets for standalone builds.

Prerequisites:

- CMake 3.23+
- Ninja
- `VCPKG_ROOT` set to your vcpkg checkout path

Configure once:

```powershell
cmake --preset ninja-x64-vcpkg
```

Build:

```powershell
cmake --build --preset debug
cmake --build --preset release
```

Install (example prefix):

```powershell
cmake --install out/build/ninja-x64-vcpkg --prefix out/install/rhi
```

If `VCPKG_ROOT` is not set in your shell:

```powershell
$env:VCPKG_ROOT = "C:/src/vcpkg"
cmake --preset ninja-x64-vcpkg
```

Example with manual Streamline/PIX header paths:

```powershell
cmake --preset ninja-x64-vcpkg -DBASICRHI_STREAMLINE_HEADERS_DIR="C:/path/to/Streamline" -DBASICRHI_PIX_HEADERS_DIR="C:/path/to/pix"
```

## Consume from another CMake project

```cmake
find_package(BasicRHI CONFIG REQUIRED)
target_link_libraries(MyTarget PRIVATE BasicRHI::BasicRHI)
```

## Backend-independent conventions

A pipeline or pass description means the same thing on every backend; callers never branch on the API.
Where the underlying APIs disagree, the backend reconciles it.

- **Clip space is y-up**, as it is in D3D. The Vulkan backend begins each pass with a y-flipped viewport
  (a negative `VkViewport::height`) rather than asking shaders to invert `SV_Position.y`.
- **`RasterState::frontCCW` has D3D's meaning.** It is the winding of a triangle as it lands on the render
  target, with clip space y-up: `false`, the default, makes the clockwise face the front one (D3D's
  default), `true` the counter-clockwise one (`FrontCounterClockwise`). The Vulkan backend's y-flipped
  viewport puts the image the same way up as D3D's, and Vulkan derives the facing from that image, so it
  maps the flag to the `VkFrontFace` of the same name - which is also DXVK's mapping under the same flip.
  Setting the same `RasterState` therefore culls the same faces on D3D12 and Vulkan.
- **An indirect `Constant` argument may be any size.** On Vulkan it becomes push data tokens of a generated
  commands layout (`VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_DATA_EXT`), and the backend emits one token per 16 bytes
  of it (`kMaxPushDataTokenBytes` in `rhi_vulkan.cpp`), each reading its bytes from the argument's own place in
  the stream. The signature's stream layout and the push data written are what the caller described. The limit
  is empirical: on NVIDIA (GA102, driver 616.56), a push data token of more than 16 bytes loses the device (DMA
  page faults with no fault address and usually no active shader, rising with the sequences executed), although
  neither the extension nor the device's limits state a bound. 12- and 16-byte tokens were healthy, and the same
  20 bytes as a 16-byte and a 4-byte token were healthy.
- **Two Vulkan binding models, one API.** Descriptor heaps, layouts and shaders mean the same on both:
  - *Descriptor heap* (`VK_EXT_descriptor_heap`): the driver maps the shader ABI's set/bindings
    (`rhi_shader_abi.h`) onto the heaps, push data and records (`VkAppendDescriptorHeapMappings`).
  - *Descriptor buffer* (`VK_EXT_descriptor_buffer` + `VK_EXT_mutable_descriptor_type`), for drivers without
    heaps or hosts that use descriptor buffers: one universal pipeline layout (set 0 the resource heap as a mutable
    array, set 1 the sampler heap, 128 bytes of push constants), and each stage's SPIR-V lowered to it by a
    SPIRV-Tools pass (`rhi_vulkan_spirv_lowering.h`) that does what the heap mappings do: heap indices into the
    arrays, indices and uniform block addresses read from records and push data through buffer device addresses.
  Both start from the same binding plan (`rhi_vulkan_binding_plan.h`). A device BasicRHI creates prefers heaps;
  `BASICRHI_VK_BINDING=heap|buffer` forces one. An adopted device runs on the model whose features the host enabled.
  `EvaluateBindingModels` (`rhi_interop_vulkan.h`) answers which models a physical device supports before a host
  creates its device, and is logged as a table of required against device values. Descriptor-buffer limits apply
  without update-after-bind, so the check is mandatory. `BASICRHI_VK_LIMITS=base` clamps every limit to the Vulkan
  base-spec minimum to exercise the fallbacks.
- **Shader-visible heap capacities follow the device.**
  - **Configuration:** `DeviceCreateInfo::descriptorHeapCapacities` (an adopting host passes them in its `BindingRequirements`) gives a preferred and a minimum capacity. The defaults are 1,000,000 / 65,536 resources and 2048 / 256 samplers.
  - **Selection:** each model is set up with min(preferred, the device's limit). A model is unavailable only when that falls below the minimum (no usable bindless).
  - **Reporting:** the result is in `ShaderFeatureInfo::maxShaderVisible{Resource,Sampler}Descriptors`, and creating a larger shader-visible heap fails. ORG sizes its heaps from these values.
  - **Testing:** `BASICRHI_VK_MAX_DESCRIPTORS=<n>` clamps the device's descriptor limits to simulate a smaller GPU.
- **Push data is portable at 128 bytes.** In the descriptor-buffer model a layout's root constants are packed in
  declaration order. If they total more than 128 bytes, the prefix that fits in 120 is pushed, the rest goes to a
  per-command-list spill image in memory, and its address is pushed in the last 8 bytes. An indirect `Constant`
  argument must target the pushed prefix (`IndirectCommandsFeatureInfo::constantArgumentBytes`, and each layout's
  own prefix): declare the per-draw range first.
- **Descriptor-buffer diagnostics.** `BASICRHI_VK_VALIDATE_LOWERING=1` validates every lowered module with
  SPIRV-Tools' validator. `BASICRHI_VK_DUMP_LOWERED=<dir>` writes each stage's input and lowered SPIR-V. The lowering
  strips NonSemantic debug info, so Nsight source-level shader debugging needs the heap model.

## Notes

`BasicRHI` exports package config files under:

- `<prefix>/lib/cmake/BasicRHI`

When installing for downstream projects, pass the prefix through `CMAKE_PREFIX_PATH`.
