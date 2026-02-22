# BasicRHI

`BasicRHI` is the low-level rendering hardware abstraction used by `OpenRenderGraph` and `BasicRenderer`.

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

## Build and install

```powershell
cmake -S BasicRHI -B out/build/rhi -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build out/build/rhi
cmake --install out/build/rhi --prefix out/install/rhi
```

Example with manual header paths:

```powershell
cmake -S BasicRHI -B out/build/rhi -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBASICRHI_STREAMLINE_HEADERS_DIR="C:/path/to/Streamline" -DBASICRHI_PIX_HEADERS_DIR="C:/path/to/pix"
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
