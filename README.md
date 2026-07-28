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

## Notes

`BasicRHI` exports package config files under:

- `<prefix>/lib/cmake/BasicRHI`

When installing for downstream projects, pass the prefix through `CMAKE_PREFIX_PATH`.
