// The on-disk pipeline cache, on the backend named by the first argument (vulkan or d3d12).
// A first device compiles a set of pipelines into an empty cache and saves it. A second device loads the file and must
// serve the same pipelines from it, also from several threads at once. Then a file for another device and a truncated
// file must both be rejected cleanly.
#include "rhi.h"
#include "rhi_pipeline_cache.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if BASICRHI_ENABLE_D3D12
#include <d3dcompiler.h>
#include <wrl.h>
#endif

namespace {
	constexpr uint32_t kComputeVariants = 4; // THREADS = 1, 2, 4, 8
	constexpr uint32_t kPipelineCount = kComputeVariants + 1; // and one graphics pipeline

	bool Check(rhi::Result result, const char* operation) {
		if (rhi::IsOk(result)) {
			return true;
		}
		std::fprintf(stderr, "%s failed: %s\n", operation, rhi::ResultName(result));
		return false;
	}

	std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
		std::ifstream file(path, std::ios::binary | std::ios::ate);
		if (!file) {
			std::fprintf(stderr, "cannot open %s\n", path.string().c_str());
			return {};
		}
		std::vector<uint8_t> bytes(static_cast<size_t>(file.tellg()));
		file.seekg(0);
		file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		return bytes;
	}

	struct Shaders {
		std::vector<uint8_t> compute[kComputeVariants];
		std::vector<uint8_t> vertex;
		std::vector<uint8_t> pixel;
		const char* computeEntry = "CSMain";
		const char* vertexEntry = "VSMain";
		const char* pixelEntry = "PSMain";
	};

#if BASICRHI_ENABLE_VULKAN
	// SPIR-V the build compiles from tests/pipeline_cache/PipelineCacheSmoke.hlsl.
	bool LoadVulkanShaders(Shaders& shaders) {
		const std::filesystem::path dir = BASICRHI_PIPELINE_CACHE_SPIRV_DIR;
		for (uint32_t variant = 0; variant < kComputeVariants; ++variant) {
			shaders.compute[variant] = ReadBytes(dir / ("CSMain" + std::to_string(1u << variant) + ".spv"));
		}
		shaders.vertex = ReadBytes(dir / "VSMain.spv");
		shaders.pixel = ReadBytes(dir / "PSMain.spv");
		for (const auto& compute : shaders.compute) {
			if (compute.empty()) return false;
		}
		return !shaders.vertex.empty() && !shaders.pixel.empty();
	}
#endif

#if BASICRHI_ENABLE_D3D12
	std::vector<uint8_t> CompileDxbc(const char* entry, const char* profile, const char* threads) {
		const D3D_SHADER_MACRO defines[] = { { "THREADS", threads }, { nullptr, nullptr } };
		Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
		const HRESULT hr = D3DCompileFromFile(std::filesystem::path(BASICRHI_PIPELINE_CACHE_HLSL).wstring().c_str(), defines, nullptr,
			entry, profile, 0, 0, &code, &errors);
		if (FAILED(hr)) {
			std::fprintf(stderr, "D3DCompileFromFile %s failed: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
			return {};
		}
		const auto* begin = static_cast<const uint8_t*>(code->GetBufferPointer());
		return { begin, begin + code->GetBufferSize() };
	}

	bool LoadD3D12Shaders(Shaders& shaders) {
		for (uint32_t variant = 0; variant < kComputeVariants; ++variant) {
			const std::string threads = std::to_string(1u << variant);
			shaders.compute[variant] = CompileDxbc("CSMain", "cs_5_1", threads.c_str());
			if (shaders.compute[variant].empty()) return false;
		}
		shaders.vertex = CompileDxbc("VSMain", "vs_5_1", "1");
		shaders.pixel = CompileDxbc("PSMain", "ps_5_1", "1");
		return !shaders.vertex.empty() && !shaders.pixel.empty();
	}
#endif

	rhi::Result CreateDevice(rhi::Backend backend, const char* cachePath, rhi::DevicePtr& device) {
		rhi::DeviceCreateInfo createInfo{};
		createInfo.backend = backend;
		createInfo.enableDebug = true;
		createInfo.pipelineCachePath = cachePath;
		return backend == rhi::Backend::Vulkan ? rhi::CreateVulkanDevice(createInfo, device) : rhi::CreateD3D12Device(createInfo, device);
	}

	rhi::ShaderBinary Binary(const std::vector<uint8_t>& bytes) {
		return { bytes.data(), static_cast<uint32_t>(bytes.size()) };
	}

	// Pipeline `index` of the set: the compute variants, then the graphics pipeline.
	bool CreateSetPipeline(rhi::Device& device, rhi::PipelineLayoutHandle layout, const Shaders& shaders, uint32_t index, rhi::PipelinePtr& out) {
		rhi::SubobjLayout subobjLayout{ layout };
		if (index < kComputeVariants) {
			rhi::SubobjShader compute{ rhi::ShaderStage::Compute, Binary(shaders.compute[index]), shaders.computeEntry };
			const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(compute) };
			return Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), out), "CreatePipeline compute");
		}
		rhi::SubobjShader vertex{ rhi::ShaderStage::Vertex, Binary(shaders.vertex), shaders.vertexEntry };
		rhi::SubobjShader pixel{ rhi::ShaderStage::Pixel, Binary(shaders.pixel), shaders.pixelEntry };
		rhi::SubobjRaster raster{};
		raster.rs.cull = rhi::CullMode::None;
		rhi::SubobjBlend blend{};
		rhi::SubobjDepth depth{};
		depth.ds.depthEnable = true;
		depth.ds.depthWrite = true;
		rhi::SubobjRTVs targets{};
		targets.rt.count = 1;
		targets.rt.formats[0] = rhi::Format::R8G8B8A8_UNorm;
		rhi::SubobjDSV dsv{ rhi::Format::D32_Float };
		rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
		const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(vertex), rhi::Make(pixel), rhi::Make(raster), rhi::Make(blend),
			rhi::Make(depth), rhi::Make(targets), rhi::Make(dsv), rhi::Make(topology) };
		return Check(device.CreatePipeline(items, static_cast<uint32_t>(std::size(items)), out), "CreatePipeline graphics");
	}

	// Creates the whole set, `threads` pipelines at a time from as many threads.
	bool CreateSet(rhi::Device& device, const Shaders& shaders, uint32_t threads) {
		rhi::PipelineLayoutPtr layout;
		if (!Check(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = {}, .staticSamplers = {}, .flags = rhi::PF_None }, layout),
				"CreatePipelineLayout")) {
			return false;
		}
		std::vector<rhi::PipelinePtr> pipelines(kPipelineCount);
		std::vector<char> ok(kPipelineCount, 0);
		std::vector<std::thread> workers;
		for (uint32_t worker = 0; worker < threads; ++worker) {
			workers.emplace_back([&, worker] {
				for (uint32_t index = worker; index < kPipelineCount; index += threads) {
					ok[index] = CreateSetPipeline(device, layout->GetHandle(), shaders, index, pipelines[index]) ? 1 : 0;
				}
			});
		}
		for (std::thread& worker : workers) worker.join();
		for (char created : ok) {
			if (!created) return false;
		}
		return true;
	}

	bool ExpectStats(const rhi::Device& device, const char* stage, rhi::PipelineCacheLoadResult load, uint64_t hits, uint64_t misses) {
		rhi::PipelineCacheStats stats{};
		if (!Check(device.GetPipelineCacheStats(stats), "GetPipelineCacheStats")) return false;
		std::printf("  %s: load=%u hits=%llu misses=%llu loaded=%llu saved=%llu saves=%u\n", stage, static_cast<unsigned>(stats.load),
			static_cast<unsigned long long>(stats.hits), static_cast<unsigned long long>(stats.misses),
			static_cast<unsigned long long>(stats.loadedBytes), static_cast<unsigned long long>(stats.savedBytes), stats.saves);
		if (stats.load != load || stats.hits != hits || stats.misses != misses) {
			std::fprintf(stderr, "%s: expected load=%u hits=%llu misses=%llu\n", stage, static_cast<unsigned>(load),
				static_cast<unsigned long long>(hits), static_cast<unsigned long long>(misses));
			return false;
		}
		return true;
	}
}

int main(int argc, char** argv) {
	const std::string backendName = argc > 1 ? argv[1] : "vulkan";
	const rhi::Backend backend = backendName == "d3d12" ? rhi::Backend::D3D12 : rhi::Backend::Vulkan;
	Shaders shaders;
	bool loaded = false;
#if BASICRHI_ENABLE_VULKAN
	if (backend == rhi::Backend::Vulkan) loaded = LoadVulkanShaders(shaders);
#endif
#if BASICRHI_ENABLE_D3D12
	if (backend == rhi::Backend::D3D12) loaded = LoadD3D12Shaders(shaders);
#endif
	if (!loaded) {
		std::fprintf(stderr, "BasicRHIPipelineCacheSmoke: no shaders for %s\n", backendName.c_str());
		return 1;
	}

	const std::filesystem::path cacheFile = std::filesystem::temp_directory_path() / ("BasicRHIPipelineCacheSmoke_" + backendName + ".bin");
	const std::string cachePath = cacheFile.string();
	std::error_code error;
	std::filesystem::remove(cacheFile, error);

	// Cold: an empty cache, every pipeline compiled, then saved.
	{
		rhi::DevicePtr device;
		if (!Check(CreateDevice(backend, cachePath.c_str(), device), "create device (cold)")) return 1;
		if (!CreateSet(device.Get(), shaders, 1)) return 1;
		if (!ExpectStats(device.Get(), "cold", rhi::PipelineCacheLoadResult::Empty, 0, kPipelineCount)) return 1;
		if (!Check(device->SavePipelineCache(), "SavePipelineCache")) return 1;
		rhi::PipelineCacheStats stats{};
		device->GetPipelineCacheStats(stats);
		if (stats.saves != 1 || stats.savedBytes == 0 || !std::filesystem::exists(cacheFile)) {
			std::fprintf(stderr, "cold: the save wrote nothing\n");
			return 1;
		}
	}

	// Warm: a new device loads the file and serves the same pipelines from it, from several threads.
	{
		rhi::DevicePtr device;
		if (!Check(CreateDevice(backend, cachePath.c_str(), device), "create device (warm)")) return 1;
		if (!CreateSet(device.Get(), shaders, 1)) return 1;
		if (!ExpectStats(device.Get(), "warm", rhi::PipelineCacheLoadResult::Loaded, kPipelineCount, 0)) return 1;
		if (!CreateSet(device.Get(), shaders, kPipelineCount)) return 1;
		if (!ExpectStats(device.Get(), "warm, parallel", rhi::PipelineCacheLoadResult::Loaded, 2 * kPipelineCount, 0)) return 1;

		// Nothing new: the save skips the write.
		if (!Check(device->SavePipelineCache(), "SavePipelineCache (nothing new)")) return 1;
		rhi::PipelineCacheStats stats{};
		device->GetPipelineCacheStats(stats);
		if (stats.saves != 0) {
			std::fprintf(stderr, "warm: a save with no new pipeline wrote the file\n");
			return 1;
		}

		// A file for another device: rejected, and the cache starts empty.
		const std::vector<uint8_t> good = ReadBytes(cacheFile);
		if (good.size() <= sizeof(rhi::detail::PipelineCacheFileHeader)) return 1;
		std::vector<uint8_t> stale = good;
		auto* header = reinterpret_cast<rhi::detail::PipelineCacheFileHeader*>(stale.data());
		header->identity.deviceId ^= 0xFFFFu;
		const std::filesystem::path staleFile = cacheFile.string() + ".stale";
		std::ofstream(staleFile, std::ios::binary).write(reinterpret_cast<const char*>(stale.data()), static_cast<std::streamsize>(stale.size()));
		if (!Check(device->OpenPipelineCache(staleFile.string().c_str()), "OpenPipelineCache (stale)")) return 1;
		if (!ExpectStats(device.Get(), "stale file", rhi::PipelineCacheLoadResult::Stale, 0, 0)) return 1;
		if (!CreateSet(device.Get(), shaders, 1)) return 1;

		// A truncated file: rejected too.
		const std::filesystem::path truncatedFile = cacheFile.string() + ".truncated";
		std::ofstream(truncatedFile, std::ios::binary).write(reinterpret_cast<const char*>(good.data()), static_cast<std::streamsize>(good.size() / 2));
		if (!Check(device->OpenPipelineCache(truncatedFile.string().c_str()), "OpenPipelineCache (truncated)")) return 1;
		if (!ExpectStats(device.Get(), "truncated file", rhi::PipelineCacheLoadResult::Corrupt, 0, 0)) return 1;

		// Closing the cache turns it off.
		if (!Check(device->OpenPipelineCache(nullptr), "OpenPipelineCache (off)")) return 1;
		if (!ExpectStats(device.Get(), "off", rhi::PipelineCacheLoadResult::Off, 0, 0)) return 1;
		if (!CreateSet(device.Get(), shaders, 1)) return 1;
		if (!ExpectStats(device.Get(), "off, after pipelines", rhi::PipelineCacheLoadResult::Off, 0, 0)) return 1;
		std::filesystem::remove(staleFile, error);
		std::filesystem::remove(truncatedFile, error);
	}

	// No path: the cache is off, as before this feature.
	{
		rhi::DevicePtr device;
		if (!Check(CreateDevice(backend, nullptr, device), "create device (no cache)")) return 1;
		if (!CreateSet(device.Get(), shaders, 1)) return 1;
		if (!ExpectStats(device.Get(), "no cache", rhi::PipelineCacheLoadResult::Off, 0, 0)) return 1;
	}
	std::filesystem::remove(cacheFile, error);

	std::printf("BasicRHIPipelineCacheSmoke (%s): success\n", backendName.c_str());
	return 0;
}
