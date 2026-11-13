#pragma once

// The on-disk pipeline cache's file format, shared by the backends. Internal to BasicRHI.
//
// A file is a header and the backend's payload: a VkPipelineCache's data on Vulkan, a serialized
// ID3D12PipelineLibrary on D3D12. The header names the device and driver the payload was made on, so a file
// from another GPU or driver is rejected before the driver sees it, and carries a hash of the payload, so a
// truncated or damaged file is rejected too.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include "rhi.h"

namespace rhi::detail {

	/** @brief 64-bit FNV-1a over bytes fed in pieces. Stable across runs and builds, so it can key on-disk data. */
	class Fnv1a64 {
	public:
		void Add(const void* data, size_t size) noexcept {
			const auto* bytes = static_cast<const uint8_t*>(data);
			for (size_t index = 0; index < size; ++index) {
				hash_ ^= bytes[index];
				hash_ *= 1099511628211ull;
			}
		}
		template<class T>
		void AddValue(const T& value) noexcept {
			static_assert(std::is_trivially_copyable_v<T>, "AddValue takes plain values; hash structs field by field");
			Add(&value, sizeof(T));
		}
		/** @brief A string and its length, so "ab"+"c" and "a"+"bc" differ. Null hashes as empty. */
		void AddString(const char* text) noexcept {
			const size_t length = text ? std::strlen(text) : 0;
			AddValue(static_cast<uint64_t>(length));
			Add(text, length);
		}
		uint64_t Value() const noexcept { return hash_; }

	private:
		uint64_t hash_ = 14695981039346656037ull;
	};

	/** @brief The device and driver a cache payload belongs to. */
	struct PipelineCacheIdentity {
		uint32_t backend = 0; // rhi::Backend
		uint32_t vendorId = 0;
		uint32_t deviceId = 0;
		uint32_t reserved = 0;
		uint64_t driverVersion = 0;
		uint8_t uuid[16]{}; // Vulkan: pipelineCacheUUID. D3D12: zero (the runtime checks the driver itself).
	};

	struct PipelineCacheFileHeader {
		static constexpr uint32_t kMagic = 0x43505242u; // "BRPC"
		static constexpr uint32_t kVersion = 1u;
		uint32_t magic = kMagic;
		uint32_t version = kVersion;
		PipelineCacheIdentity identity{};
		uint64_t payloadBytes = 0;
		uint64_t payloadHash = 0;
	};

	/** @brief A load result for logs. */
	inline const char* PipelineCacheLoadName(PipelineCacheLoadResult result) noexcept {
		switch (result) {
		case PipelineCacheLoadResult::Off: return "off";
		case PipelineCacheLoadResult::Empty: return "empty";
		case PipelineCacheLoadResult::Loaded: return "loaded";
		case PipelineCacheLoadResult::Stale: return "stale (another device or driver)";
		case PipelineCacheLoadResult::Corrupt: return "corrupt";
		case PipelineCacheLoadResult::Unsupported: return "unsupported";
		}
		return "unknown";
	}

	inline bool SamePipelineCacheIdentity(const PipelineCacheIdentity& a, const PipelineCacheIdentity& b) noexcept {
		return a.backend == b.backend && a.vendorId == b.vendorId && a.deviceId == b.deviceId &&
			a.driverVersion == b.driverVersion && std::memcmp(a.uuid, b.uuid, sizeof(a.uuid)) == 0;
	}

	inline std::filesystem::path PipelineCachePath(const char* utf8Path) {
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8Path)));
	}

	/**
	 * @brief Reads a cache file into `payload`. Returns Empty when there is no file, Stale when it is for another device
	 * or driver, Corrupt when it is damaged, and Loaded with the payload otherwise.
	 */
	inline PipelineCacheLoadResult ReadPipelineCacheFile(const char* utf8Path, const PipelineCacheIdentity& identity, std::vector<uint8_t>& payload) noexcept {
		payload.clear();
		try {
			const std::filesystem::path path = PipelineCachePath(utf8Path);
			std::error_code error;
			if (!std::filesystem::exists(path, error)) {
				return PipelineCacheLoadResult::Empty;
			}
			std::ifstream file(path, std::ios::binary);
			if (!file) {
				return PipelineCacheLoadResult::Corrupt;
			}
			PipelineCacheFileHeader header{};
			if (!file.read(reinterpret_cast<char*>(&header), sizeof(header)) ||
				header.magic != PipelineCacheFileHeader::kMagic || header.version != PipelineCacheFileHeader::kVersion) {
				return PipelineCacheLoadResult::Corrupt;
			}
			if (!SamePipelineCacheIdentity(header.identity, identity)) {
				return PipelineCacheLoadResult::Stale;
			}
			const uint64_t fileBytes = static_cast<uint64_t>(std::filesystem::file_size(path, error));
			if (error || fileBytes != sizeof(header) + header.payloadBytes) {
				return PipelineCacheLoadResult::Corrupt;
			}
			payload.resize(static_cast<size_t>(header.payloadBytes));
			if (!payload.empty() && !file.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(payload.size()))) {
				payload.clear();
				return PipelineCacheLoadResult::Corrupt;
			}
			Fnv1a64 hash;
			hash.Add(payload.data(), payload.size());
			if (hash.Value() != header.payloadHash) {
				payload.clear();
				return PipelineCacheLoadResult::Corrupt;
			}
			return PipelineCacheLoadResult::Loaded;
		}
		catch (...) {
			payload.clear();
			return PipelineCacheLoadResult::Corrupt;
		}
	}

	/** @brief Writes a cache file: a temporary file beside it, then a rename, so a crash never leaves half a file. */
	inline Result WritePipelineCacheFile(const char* utf8Path, const PipelineCacheIdentity& identity, const void* payload, size_t payloadBytes) noexcept {
		try {
			const std::filesystem::path path = PipelineCachePath(utf8Path);
			std::error_code error;
			if (path.has_parent_path()) {
				std::filesystem::create_directories(path.parent_path(), error);
			}
			std::filesystem::path temporary = path;
			temporary += ".tmp";
			{
				std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
				if (!file) {
					return Result::Failed;
				}
				PipelineCacheFileHeader header{};
				header.identity = identity;
				header.payloadBytes = payloadBytes;
				Fnv1a64 hash;
				hash.Add(payload, payloadBytes);
				header.payloadHash = hash.Value();
				file.write(reinterpret_cast<const char*>(&header), sizeof(header));
				file.write(static_cast<const char*>(payload), static_cast<std::streamsize>(payloadBytes));
				if (!file) {
					return Result::Failed;
				}
			}
			std::filesystem::rename(temporary, path, error);
			if (error) {
				std::filesystem::remove(temporary, error);
				return Result::Failed;
			}
			return Result::Ok;
		}
		catch (...) {
			return Result::Failed;
		}
	}

	/** @brief The counters behind Device::GetPipelineCacheStats. Pipeline creation updates them from any thread. */
	struct PipelineCacheCounters {
		std::atomic<uint64_t> hits{ 0 };
		std::atomic<uint64_t> misses{ 0 };
		std::atomic<uint64_t> loadedBytes{ 0 };
		std::atomic<uint64_t> savedBytes{ 0 };
		std::atomic<uint32_t> saves{ 0 };
		// Pipelines added since the last load or save; a save with none skips the write.
		std::atomic<uint64_t> added{ 0 };
		std::atomic<PipelineCacheLoadResult> load{ PipelineCacheLoadResult::Off };

		void Reset(PipelineCacheLoadResult result, uint64_t payloadBytes) noexcept {
			hits = 0;
			misses = 0;
			savedBytes = 0;
			saves = 0;
			added = 0;
			loadedBytes = payloadBytes;
			load = result;
		}
		void Fill(PipelineCacheStats& out) const noexcept {
			out.load = load.load();
			out.hits = hits.load();
			out.misses = misses.load();
			out.loadedBytes = loadedBytes.load();
			out.savedBytes = savedBytes.load();
			out.saves = saves.load();
		}
	};

} // namespace rhi::detail
