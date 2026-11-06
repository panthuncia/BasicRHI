// End-to-end check of the D3D12 clustered raytracing path against hand-computed hits.
// Exit codes: 0 pass, 1 fail, 77 skip (no driver support).
#include "rhi.h"
#include "rhi_helpers.h"
#include "rhi_interop_dx12.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "D3D12RaytracingSmoke_cs.h"
#include "D3D12RaytracingSmoke_omm_cs.h"
#include "D3D12RaytracingSmoke_lib.h"

extern "C" {
	__declspec(dllexport) extern const UINT D3D12SDKVersion = 722;
	__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D\\";
}

namespace {
	constexpr int kSkip = 77;
	constexpr uint32_t kRayCount = 6;

	struct RayResult {
		uint32_t hit, instanceId, clusterId, primitiveIndex;
		float baryU, baryV, t;
		uint32_t reserved;
	};
	static_assert(sizeof(RayResult) == 32);

	struct Vec3 { float x, y, z; };

	// Cluster A: unit quad at the origin. Cluster B: one triangle at x = 2.
	constexpr uint32_t kClusterAId = 1001, kClusterBId = 1002;
	const std::vector<Vec3> kPositions = { {0,0,0}, {1,0,0}, {1,1,0}, {0,1,0}, {2,0,0}, {3,0,0}, {3,1,0} };
	const std::vector<uint8_t> kIndices = { 0,1,2, 0,2,3, 0,1,2 };
	constexpr uint32_t kClusterBFirstVertex = 4, kClusterBFirstIndex = 6;

	bool Check(rhi::Result result, const char* what) {
		if (rhi::IsOk(result)) return true;
		std::fprintf(stderr, "%s failed: %s\n", what, rhi::ResultName(result));
		return false;
	}

	struct Context {
		rhi::DevicePtr device;
		rhi::Queue queue;
		rhi::CommandAllocatorPtr allocator;
		rhi::CommandListPtr commandList;
		rhi::DescriptorHeapPtr heap;
		rhi::DescriptorHeapPtr samplerHeap;
		rhi::PipelineLayoutPtr layout;
		rhi::PipelinePtr queryPipeline;
		rhi::PipelinePtr tracePipeline;
		rhi::PipelinePtr ommQueryPipeline;
		rhi::PipelinePtr ommTracePipeline;
		std::vector<rhi::ResourcePtr> keepAlive;
		std::vector<rhi::AccelerationStructurePtr> keepAliveAs;
		uint32_t nextDescriptor = 0;
		RayTracingFeatureInfo caps{};
	};

	rhi::Resource MakeBuffer(Context& c, uint64_t bytes, rhi::HeapType heap, rhi::ResourceFlags flags, const char* name) {
		const auto desc = rhi::helpers::ResourceDesc::Buffer(bytes, heap, flags, name);
		rhi::ResourcePtr buffer;
		if (!Check(c.device->CreateCommittedResource(desc, buffer), name)) return {};
		c.keepAlive.push_back(std::move(buffer));
		return c.keepAlive.back().Get();
	}

	rhi::Resource MakeUav(Context& c, uint64_t bytes, const char* name) { return MakeBuffer(c, bytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess, name); }
	rhi::Resource MakeAsStorage(Context& c, uint64_t bytes, const char* name) {
		return MakeBuffer(c, bytes, rhi::HeapType::DeviceLocal, static_cast<rhi::ResourceFlags>(rhi::RF_AllowUnorderedAccess | rhi::RF_RaytracingAccelerationStructure), name);
	}

	rhi::Resource MakeUpload(Context& c, const void* data, uint64_t bytes, const char* name) {
		rhi::Resource buffer = MakeBuffer(c, bytes, rhi::HeapType::Upload, {}, name);
		if (!buffer) return {};
		void* mapped = nullptr;
		buffer.Map(&mapped, 0, bytes);
		std::memcpy(mapped, data, bytes);
		buffer.Unmap(0, bytes);
		return buffer;
	}

	template <class T>
	rhi::Resource MakeUpload(Context& c, const std::vector<T>& data, const char* name) { return MakeUpload(c, data.data(), data.size() * sizeof(T), name); }

	uint64_t Va(Context& c, rhi::Resource buffer, uint64_t offset = 0) { return c.device->GetBufferDeviceAddress({ buffer.GetHandle(), offset }); }

	bool Submit(Context& c, const std::function<void(rhi::CommandList&)>& record) {
		rhi::CommandList list = c.commandList.Get();
		list.SetDescriptorHeaps(c.heap->GetHandle(), c.samplerHeap->GetHandle());
		list.BindLayout(c.layout->GetHandle());
		record(list);
		list.End();
		const rhi::CommandList lists[] = { list };
		const bool ok = Check(c.queue.Submit(lists, {}), "Queue::Submit") && Check(c.device->WaitIdle(), "WaitIdle");
		list.Recycle(c.allocator.Get());
		return ok;
	}

	template <class T>
	std::vector<T> Readback(Context& c, rhi::Resource source, uint32_t count) {
		const uint64_t bytes = count * sizeof(T);
		rhi::Resource readback = MakeBuffer(c, bytes, rhi::HeapType::Readback, {}, "readback");
		std::vector<T> out(count);
		if (!readback || !Submit(c, [&](rhi::CommandList& list) { list.CopyBufferRegion(readback.GetHandle(), 0, source.GetHandle(), 0, bytes); })) return {};
		void* mapped = nullptr;
		readback.Map(&mapped, 0, bytes);
		std::memcpy(out.data(), mapped, bytes);
		readback.Unmap(0, 0);
		return out;
	}

	bool Near(float a, float b) { return std::fabs(a - b) < 1e-3f; }

	bool Expect(const char* scene, uint32_t ray, const RayResult& got, uint32_t hit, uint32_t instanceId, uint32_t clusterId, uint32_t primitive, float u, float v) {
		bool ok = got.hit == hit;
		if (hit) ok = ok && got.instanceId == instanceId && got.clusterId == clusterId && got.primitiveIndex == primitive && Near(got.baryU, u) && Near(got.baryV, v) && Near(got.t, 1.0f);
		if (!ok) {
			std::fprintf(stderr, "%s ray %u: got hit=%u instance=%u cluster=%u prim=%u bary=(%f,%f) t=%f; wanted hit=%u instance=%u cluster=%u prim=%u bary=(%f,%f) t=1\n",
				scene, ray, got.hit, got.instanceId, got.clusterId, got.primitiveIndex, got.baryU, got.baryV, got.t, hit, instanceId, clusterId, primitive, u, v);
		}
		return ok;
	}

	// The scene every path must reproduce: instance 7 at the origin, instance 8 translated by +10 in x.
	bool ExpectStandardScene(const char* scene, const std::vector<RayResult>& r) {
		if (r.size() != kRayCount) return false;
		bool ok = true;
		ok &= Expect(scene, 0, r[0], 1, 7, kClusterAId, 0, 0.50f, 0.25f);
		ok &= Expect(scene, 1, r[1], 1, 7, kClusterAId, 1, 0.25f, 0.50f);
		ok &= Expect(scene, 2, r[2], 1, 7, kClusterBId, 0, 0.25f, 0.25f);
		ok &= Expect(scene, 3, r[3], 1, 8, kClusterAId, 0, 0.50f, 0.25f);
		ok &= Expect(scene, 4, r[4], 0, 0, 0, 0, 0, 0);
		ok &= Expect(scene, 5, r[5], 0, 0, 0, 0, 0, 0);
		return ok;
	}

	rhi::PackedDxr2ClusterBuildTriangleInfo ClusterArgs(Context& c, rhi::Resource vertices, rhi::Resource indices, uint32_t id, uint16_t triangles, uint16_t uniqueVertices, uint32_t firstVertex, uint32_t firstIndex) {
		rhi::PackedDxr2ClusterBuildTriangleInfo args{};
		args.clusterID = id;
		args.triangleCount = triangles;
		args.uniqueVertexIndexCount = uniqueVertices;
		args.baseGeometryIndexAndFlags = rhi::PackDxr2ClusterGeometryIndexAndFlags(0, rhi::RTClusterGeometry_Opaque);
		args.vertexBufferStride = sizeof(Vec3);
		args.indexBufferStride = 1;
		args.vertexBuffer = Va(c, vertices, firstVertex * sizeof(Vec3));
		args.indexBuffer = Va(c, indices, firstIndex);
		return args;
	}

	rhi::RayTracingClusterTriangleInputsDesc ClusterInputs() {
		rhi::RayTracingClusterTriangleInputsDesc inputs{};
		inputs.maxClusterTriangleCount = 2;
		inputs.maxClusterVertexCount = 4;
		inputs.maxTotalTriangleCount = 3;
		inputs.maxTotalVertexCount = 7;
		return inputs;
	}

	// Builds a BLAS over the CLAS addresses in an array, implicit destination. Returns its address.
	uint64_t BuildBlas(Context& c, uint64_t addressArrayVa, uint32_t clusterCount, bool allowOpacityMicromaps = false) {
		rhi::RayTracingClusterBottomLevelInputsDesc blasInputs{ clusterCount, clusterCount };
		rhi::RayTracingRtasOperationInputs inputs{};
		inputs.operationType = rhi::RayTracingRtasOperationType::BuildClustersBottomLevel;
		inputs.maxAccelerationStructureCount = 1;
		inputs.flags = rhi::RTASBuild_PreferFastTrace;
		inputs.allowOpacityMicromaps = allowOpacityMicromaps;
		inputs.clustersBottomLevel = &blasInputs;
		rhi::RayTracingRtasPrebuildInfo prebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(inputs, prebuild), "BLAS prebuild")) return 0;

		rhi::Resource result = MakeAsStorage(c, prebuild.resultDataMaxSizeInBytes, "BLAS data");
		rhi::Resource scratch = MakeUav(c, prebuild.scratchDataSizeInBytes, "BLAS scratch");
		rhi::Resource addresses = MakeUav(c, 8, "BLAS address");
		rhi::Resource sizes = MakeUav(c, 8, "BLAS size");
		const std::vector<rhi::PackedRayTracingClusterBuildBottomLevelInfo> args = { { clusterCount, sizeof(uint64_t), addressArrayVa } };
		rhi::Resource argsBuffer = MakeUpload(c, args, "BLAS args");

		rhi::RayTracingRtasOperationDesc desc{};
		desc.inputs = inputs;
		desc.batched.dstImplicitData = { result.GetHandle(), 0 };
		desc.batched.scratchData = { scratch.GetHandle(), 0 };
		desc.batched.dstAddressesArray = { addresses.GetHandle(), 0, 8, 8 };
		desc.batched.dstSizesArray = { sizes.GetHandle(), 0, 8, 8 };
		desc.batched.srcInfosArray = { argsBuffer.GetHandle(), 0, sizeof(args[0]), sizeof(args[0]) };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&desc, 1); })) return 0;
		const auto out = Readback<uint64_t>(c, addresses, 1);
		return out.empty() ? 0 : out[0];
	}

	void Translate(float transform[3][4], float x) {
		std::memset(transform, 0, sizeof(float) * 12);
		transform[0][0] = transform[1][1] = transform[2][2] = 1.0f;
		transform[0][3] = x;
	}

	// Classic TLAS over cluster BLASes: cluster BLASes must work anywhere a BLAS does.
	rhi::AccelerationStructureHandle BuildClassicTlas(Context& c, const std::vector<std::pair<uint32_t, float>>& instances, uint64_t blas) {
		std::vector<rhi::PackedRayTracingInstanceDesc> descs(instances.size());
		for (size_t i = 0; i < instances.size(); ++i) {
			Translate(descs[i].transform, instances[i].second);
			descs[i].instanceIDAndMask = (instances[i].first & 0x00FFFFFFu) | (0xFFu << 24);
			descs[i].contributionAndFlags = 0;
			descs[i].accelerationStructureDeviceAddress = blas;
		}
		rhi::Resource instanceBuffer = MakeUpload(c, descs, "TLAS instances");
		rhi::RayTracingGeometryDesc geometry{};
		geometry.type = rhi::RayTracingGeometryType::Instances;
		geometry.instances.instanceBuffer = { instanceBuffer.GetHandle(), 0, descs.size() * sizeof(descs[0]) };
		geometry.instances.stride = sizeof(descs[0]);
		geometry.instances.count = static_cast<uint32_t>(descs.size());
		rhi::AccelerationStructureBuildInputs inputs{};
		inputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
		inputs.flags = rhi::RTASBuild_PreferFastTrace;
		inputs.geometries = { &geometry, 1 };
		rhi::AccelerationStructurePrebuildInfo prebuild{};
		if (!Check(c.device->GetAccelerationStructurePrebuildInfo(inputs, prebuild), "TLAS prebuild")) return {};
		rhi::Resource storage = MakeAsStorage(c, prebuild.resultDataMaxSizeInBytes, "TLAS data");
		rhi::Resource scratch = MakeUav(c, prebuild.scratchDataSizeInBytes, "TLAS scratch");
		rhi::AccelerationStructureDesc tlasDesc{};
		tlasDesc.type = rhi::RayTracingAccelerationStructureType::TopLevel;
		tlasDesc.storage = storage.GetHandle();
		tlasDesc.sizeBytes = prebuild.resultDataMaxSizeInBytes;
		tlasDesc.flags = rhi::RTASBuild_PreferFastTrace;
		rhi::AccelerationStructurePtr tlas;
		if (!Check(c.device->CreateAccelerationStructure(tlasDesc, tlas), "CreateAccelerationStructure")) return {};
		rhi::AccelerationStructureBuildDesc build{};
		build.mode = rhi::RayTracingAccelerationStructureBuildMode::Build;
		build.destination = tlas->GetHandle();
		build.inputs = inputs;
		build.scratch = { scratch.GetHandle(), 0, prebuild.scratchDataSizeInBytes };
		if (!Submit(c, [&](rhi::CommandList& list) { list.BuildAccelerationStructures(&build, 1); })) return {};
		const rhi::AccelerationStructureHandle handle = tlas->GetHandle();
		c.keepAliveAs.push_back(std::move(tlas));
		return handle;
	}

	std::vector<RayResult> TraceWith(Context& c, const rhi::SrvDesc& sceneDesc, bool rayQuery) {
		const uint32_t sceneIndex = c.nextDescriptor++;
		const uint32_t resultsIndex = c.nextDescriptor++;
		if (!Check(c.device->CreateShaderResourceView({ c.heap->GetHandle(), sceneIndex }, {}, sceneDesc), "scene SRV")) return {};
		rhi::Resource results = MakeUav(c, kRayCount * sizeof(RayResult), "ray results");
		rhi::UavDesc uav{};
		uav.dimension = rhi::UavDim::Buffer;
		uav.buffer.kind = rhi::BufferViewKind::Structured;
		uav.buffer.numElements = kRayCount;
		uav.buffer.structureByteStride = sizeof(RayResult);
		if (!Check(c.device->CreateUnorderedAccessView({ c.heap->GetHandle(), resultsIndex }, results.GetHandle(), uav), "results UAV")) return {};
		const uint32_t constants[2] = { sceneIndex, resultsIndex };

		if (rayQuery) {
			if (!Submit(c, [&](rhi::CommandList& list) {
				list.BindPipeline(c.queryPipeline->GetHandle());
				list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 2, constants);
				list.Dispatch(1, 1, 1);
			})) return {};
		}
		else {
			const uint64_t handleSize = c.caps.shaderGroupHandleSize;
			const uint64_t stride = rhi::AlignRayTracingShaderRecordSize(handleSize, c.caps.shaderTableStrideAlignment);
			const uint64_t base = c.caps.shaderGroupBaseAlignment;
			std::vector<uint8_t> handles(handleSize * 3);
			if (!Check(c.device->GetRayTracingShaderGroupHandles(c.tracePipeline->GetHandle(), 0, 3, handles.data(), static_cast<uint32_t>(handles.size())), "shader group handles")) return {};
			std::vector<uint8_t> table(base * 3);
			for (uint64_t group = 0; group < 3; ++group) std::memcpy(table.data() + base * group, handles.data() + handleSize * group, handleSize);
			rhi::Resource tableBuffer = MakeUpload(c, table, "shader table");
			rhi::RayTracingDispatchDesc dispatch{};
			dispatch.rayGenerationShaderTable = { tableBuffer.GetHandle(), 0, stride, stride };
			dispatch.missShaderTable = { tableBuffer.GetHandle(), base, stride, stride };
			dispatch.hitGroupTable = { tableBuffer.GetHandle(), base * 2, stride, stride };
			dispatch.width = kRayCount;
			if (!Submit(c, [&](rhi::CommandList& list) {
				list.BindPipeline(c.tracePipeline->GetHandle());
				list.PushConstants(rhi::ShaderStage::AllRayTracing, 0, 0, 0, 2, constants);
				list.TraceRays(dispatch);
			})) return {};
		}
		return Readback<RayResult>(c, results, kRayCount);
	}

	// HitObject trace, reorder and invoke, launched from arguments in a buffer as the path tracer's dispatch is.
	std::vector<RayResult> TraceReorderedIndirect(Context& c, rhi::AccelerationStructureHandle tlas) {
		const uint32_t sceneIndex = c.nextDescriptor++;
		const uint32_t resultsIndex = c.nextDescriptor++;
		// SrvDesc is a union: {} only initializes its first member, so the view's fields must be set explicitly.
		rhi::SrvDesc scene{};
		scene.dimension = rhi::SrvDim::AccelerationStruct;
		scene.accel.deviceAddress = 0;
		scene.accel.address = {};
		scene.accel.partitioned = false;
		scene.accel.accelerationStructure = tlas;
		if (!Check(c.device->CreateShaderResourceView({ c.heap->GetHandle(), sceneIndex }, {}, scene), "scene SRV")) return {};
		rhi::Resource results = MakeUav(c, kRayCount * sizeof(RayResult), "reordered ray results");
		rhi::UavDesc uav{};
		uav.dimension = rhi::UavDim::Buffer;
		uav.buffer.kind = rhi::BufferViewKind::Structured;
		uav.buffer.numElements = kRayCount;
		uav.buffer.structureByteStride = sizeof(RayResult);
		if (!Check(c.device->CreateUnorderedAccessView({ c.heap->GetHandle(), resultsIndex }, results.GetHandle(), uav), "results UAV")) return {};
		const uint32_t constants[2] = { sceneIndex, resultsIndex };

		const uint64_t handleSize = c.caps.shaderGroupHandleSize;
		const uint64_t stride = rhi::AlignRayTracingShaderRecordSize(handleSize, c.caps.shaderTableStrideAlignment);
		const uint64_t base = c.caps.shaderGroupBaseAlignment;
		std::vector<uint8_t> handles(handleSize * 5);
		if (!Check(c.device->GetRayTracingShaderGroupHandles(c.tracePipeline->GetHandle(), 0, 5, handles.data(), static_cast<uint32_t>(handles.size())), "shader group handles")) return {};
		// Ray generation, miss, then two hit records: the ray's contribution of 1 must select the any-hit group.
		std::vector<uint8_t> table(base * 3 + stride * 2);
		std::memcpy(table.data(), handles.data() + handleSize * 3, handleSize);
		std::memcpy(table.data() + base, handles.data() + handleSize * 1, handleSize);
		std::memcpy(table.data() + base * 2, handles.data() + handleSize * 2, handleSize);
		std::memcpy(table.data() + base * 2 + stride, handles.data() + handleSize * 4, handleSize);
		rhi::Resource tableBuffer = MakeUpload(c, table, "reordered shader table");

		D3D12_DISPATCH_RAYS_DESC arguments{};
		arguments.RayGenerationShaderRecord = { Va(c, tableBuffer), stride };
		arguments.MissShaderTable = { Va(c, tableBuffer, base), stride, stride };
		arguments.HitGroupTable = { Va(c, tableBuffer, base * 2), stride * 2, stride };
		arguments.Width = kRayCount;
		arguments.Height = 1;
		arguments.Depth = 1;
		rhi::Resource argumentBuffer = MakeUpload(c, &arguments, sizeof(arguments), "dispatch arguments");

		rhi::RayTracingDispatchIndirectDesc dispatch{};
		dispatch.dispatchRaysDescBuffer = { argumentBuffer.GetHandle(), 0 };
		if (!Submit(c, [&](rhi::CommandList& list) {
			list.BindPipeline(c.tracePipeline->GetHandle());
			list.PushConstants(rhi::ShaderStage::AllRayTracing, 0, 0, 0, 2, constants);
			list.TraceRaysIndirect(dispatch);
		})) return {};
		return Readback<RayResult>(c, results, kRayCount);
	}

	// The extras only a hit shader reports: 1 triangle fetched, 2 heap reached, 4 any-hit ran first.
	bool ExpectHitShaderFeatures(const char* scene, const std::vector<RayResult>& r, uint32_t wanted) {
		if (r.size() != kRayCount) return false;
		bool ok = true;
		for (uint32_t i = 0; i < kRayCount; ++i) {
			if (r[i].hit != 0 && r[i].reserved != wanted) {
				std::fprintf(stderr, "%s ray %u: hit shader features %u, wanted %u\n", scene, i, r[i].reserved, wanted);
				ok = false;
			}
		}
		return ok;
	}

	std::vector<RayResult> TraceHandle(Context& c, rhi::AccelerationStructureHandle tlas, bool rayQuery) {
		// SrvDesc is a union: {} only initializes its first member, so the view's fields must be set explicitly.
		rhi::SrvDesc scene{};
		scene.dimension = rhi::SrvDim::AccelerationStruct;
		scene.accel.deviceAddress = 0;
		scene.accel.address = {};
		scene.accel.partitioned = false;
		scene.accel.accelerationStructure = tlas;
		return TraceWith(c, scene, rayQuery);
	}

	bool CreatePipelines(Context& c) {
		rhi::PushConstantRangeDesc push{};
		push.visibility = rhi::ShaderStage::All;
		push.num32BitValues = 4; // sceneIndex, resultsIndex; the OMM shaders add raysIndex, rayFlags
		push.set = 0;
		push.binding = 0;
		if (!Check(c.device->CreatePipelineLayout(rhi::PipelineLayoutDesc{ .ranges = {}, .pushConstants = { &push, 1 }, .staticSamplers = {}, .flags = rhi::PF_None }, c.layout), "CreatePipelineLayout")) return false;

		rhi::SubobjLayout layout{ c.layout->GetHandle() };
		rhi::SubobjShader cs{ rhi::ShaderStage::Compute, rhi::ShaderBinary{ g_rayQueryCS, static_cast<uint32_t>(sizeof(g_rayQueryCS)) }, "CSMain" };
		rhi::PipelineStreamItem computeItems[] = { rhi::Make(layout), rhi::Make(cs) };
		if (!Check(c.device->CreatePipeline(computeItems, 2, c.queryPipeline), "ray query pipeline")) return false;

		const rhi::ShaderBinary library{ g_traceLibrary, static_cast<uint32_t>(sizeof(g_traceLibrary)) };
		rhi::SubobjShader shaders[5] = { { rhi::ShaderStage::RayGen, library, "RayGen" }, { rhi::ShaderStage::Miss, library, "Miss" }, { rhi::ShaderStage::ClosestHit, library, "ClosestHit" },
			{ rhi::ShaderStage::RayGen, library, "RayGenReordered" }, { rhi::ShaderStage::AnyHit, library, "AnyHit" } };
		// Order matters: TraceReorderedIndirect picks records by these indices.
		rhi::RayTracingShaderGroupDesc groups[5] = {
			{ .type = rhi::RayTracingShaderGroupType::General, .name = "RayGen", .generalShader = "RayGen" },
			{ .type = rhi::RayTracingShaderGroupType::General, .name = "Miss", .generalShader = "Miss" },
			{ .type = rhi::RayTracingShaderGroupType::TrianglesHitGroup, .name = "HitGroup", .closestHitShader = "ClosestHit" },
			{ .type = rhi::RayTracingShaderGroupType::General, .name = "RayGenReordered", .generalShader = "RayGenReordered" },
			{ .type = rhi::RayTracingShaderGroupType::TrianglesHitGroup, .name = "AnyHitGroup", .closestHitShader = "ClosestHit", .anyHitShader = "AnyHit" },
		};
		rhi::SubobjRayTracingPipeline rt{};
		rt.globalLayout = c.layout->GetHandle();
		rt.shaders = { shaders, 5 };
		rt.shaderGroups = { groups, 5 };
		rt.shaderConfig.maxPayloadSizeInBytes = sizeof(RayResult) + sizeof(uint32_t);
		rt.shaderConfig.maxAttributeSizeInBytes = 8;
		rt.pipelineConfig.maxTraceRecursionDepth = 1;
		rt.flags = rhi::RTPipeline_AllowClusteredGeometry;
		rhi::PipelineStreamItem rtItems[] = { rhi::Make(rt) };
		return Check(c.device->CreatePipeline(rtItems, 1, c.tracePipeline), "raytracing pipeline");
	}

	bool HasDebugLayerErrors(ID3D12Device* device, UINT64 first) {
		ID3D12InfoQueue* queue = nullptr;
		if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
			std::fputs("D3D12 InfoQueue unavailable\n", stderr);
			return true;
		}
		bool errors = false;
		uint32_t warnings = 0;
		const UINT64 count = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
		for (UINT64 i = first; i < count; ++i) {
			SIZE_T bytes = 0;
			if (FAILED(queue->GetMessage(i, nullptr, &bytes)) || bytes == 0) continue;
			std::vector<std::byte> storage(bytes);
			auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
			if (FAILED(queue->GetMessage(i, message, &bytes))) continue;
			if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION) {
				errors = true;
				std::fprintf(stderr, "D3D12 validation error %u: %s\n", message->ID, message->pDescription);
			}
			else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING) {
				++warnings;
				std::fprintf(stderr, "D3D12 validation warning %u: %s\n", message->ID, message->pDescription);
			}
		}
		std::printf("D3D12 debug layer: %llu messages during the test, %u warnings, %s\n", count - first, warnings, errors ? "errors" : "no errors");
		queue->Release();
		return errors;
	}

	// ---------------------------------------------------------------------------------------------------------------
	// Opacity micromaps on clusters.
	//
	// Nine unit right triangles in z = 0, triangle t at x = 2t with vertices (0,0), (1,0), (0,1): a hit at barycentrics
	// (b1, b2) = (weight of vertex 1, weight of vertex 2) lands at (2t + b1, b2). One ray per micro-triangle centroid of a
	// level-3 grid (64 per triangle), for four instances 10 apart in y with different OMM-related instance flags.
	//
	// Cluster A (base location 0) OMM indices { 0, 1, FULLY_TRANSPARENT, FULLY_OPAQUE }
	// Cluster B (base location 1) OMM indices { 1, 2, FULLY_UNKNOWN_OPAQUE, FULLY_UNKNOWN_TRANSPARENT, CLUSTER_SKIP_OMM }
	//   so with the base location: OMM 2, OMM 3, then the special ones (which the base location must not offset).
	// OMMs 0-2 are OC1_4State, OMM 3 is OC1_2State, all level 3.
	constexpr uint32_t kOmmLevel = 3;
	constexpr uint32_t kOmmMicroCount = 1u << (2 * kOmmLevel);
	constexpr uint32_t kOmmClusterAId = 2001, kOmmClusterBId = 2002;
	constexpr uint32_t kOmmTrianglesA = 4, kOmmTrianglesB = 5, kOmmTriangles = kOmmTrianglesA + kOmmTrianglesB;
	constexpr uint32_t kOmmCount = 4;
	constexpr uint32_t kRayFlagForceOpaque = 0x1, kRayFlagForceOmm2State = 0x400;

	enum OmmState : uint32_t { Transparent = 0, Opaque = 1, UnknownTransparent = 2, UnknownOpaque = 3, NoOmm = 4 };
	enum Outcome : uint32_t { Miss = 0, OpaqueHit = 1, AnyHitHit = 2, AnyHitMiss = 3 };
	const char* OutcomeName(uint32_t o) { return o == Miss ? "miss" : o == OpaqueHit ? "hit, no any-hit" : o == AnyHitHit ? "hit after any-hit" : "any-hit then miss"; }

	// Vulkan reference code (VK_KHR_opacity_micromap, "Reference Code").
	uint32_t BarycentricsToSpaceFillingCurveIndex(float u, float v, uint32_t level) {
		u = std::clamp(u, 0.0f, 1.0f);
		v = std::clamp(v, 0.0f, 1.0f);
		uint32_t iu, iv, iw;
		float fu = u * (1u << level);
		float fv = v * (1u << level);
		iu = (uint32_t)fu;
		iv = (uint32_t)fv;
		float uf = fu - float(iu);
		float vf = fv - float(iv);
		if (iu >= (1u << level)) iu = (1u << level) - 1u;
		if (iv >= (1u << level)) iv = (1u << level) - 1u;
		uint32_t iuv = iu + iv;
		if (iuv >= (1u << level)) iu -= iuv - (1u << level) + 1u;
		iw = ~(iu + iv);
		if (uf + vf >= 1.0f && iuv < (1u << level) - 1u) --iw;
		uint32_t b0 = ~(iu ^ iw);
		b0 &= ((1u << level) - 1u);
		uint32_t t = (iu ^ iv) & b0;
		uint32_t f = t;
		f ^= f >> 1u;
		f ^= f >> 2u;
		f ^= f >> 4u;
		f ^= f >> 8u;
		uint32_t b1 = ((f ^ iu) & ~b0) | t;
		b0 = (b0 | (b0 << 8u)) & 0x00ff00ffu;
		b0 = (b0 | (b0 << 4u)) & 0x0f0f0f0fu;
		b0 = (b0 | (b0 << 2u)) & 0x33333333u;
		b0 = (b0 | (b0 << 1u)) & 0x55555555u;
		b1 = (b1 | (b1 << 8u)) & 0x00ff00ffu;
		b1 = (b1 | (b1 << 4u)) & 0x0f0f0f0fu;
		b1 = (b1 | (b1 << 2u)) & 0x33333333u;
		b1 = (b1 | (b1 << 1u)) & 0x55555555u;
		return b0 | (b1 << 1u);
	}

	uint32_t Hash(uint32_t x) {
		x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
		return x;
	}

	// The state written for micro-triangle i of each OMM (OMM 3 is 2-state, so 0/1 only).
	uint32_t OmmPattern(uint32_t omm, uint32_t i) {
		switch (omm) {
		case 0: return i % 4u;
		case 1: return Hash(i + 17u) & 3u;
		case 2: return (3u * i + i / 5u) % 4u;
		default: return (Hash(i * 3u + 101u) >> 5) & 1u;
		}
	}
	bool OmmIsTwoState(uint32_t omm) { return omm == 3; }

	struct OmmTriangleSource { int32_t omm; uint32_t uniformState; };
	// What each global triangle resolves to, once the base location is applied.
	const OmmTriangleSource kOmmTriangleSources[kOmmTriangles] = {
		{ 0, 0 }, { 1, 0 }, { -1, Transparent }, { -1, Opaque },
		{ 2, 0 }, { 3, 0 }, { -1, UnknownOpaque }, { -1, UnknownTransparent }, { -1, NoOmm },
	};

	struct OmmInstance { uint32_t id; float y; uint32_t flags; const char* name; };
	const OmmInstance kOmmInstances[] = {
		{ 0, 0.0f, rhi::RTInstance_None, "plain" },
		{ 1, 10.0f, rhi::RTInstance_DisableOpacityMicromaps, "DISABLE_OMMS" },
		{ 2, 20.0f, rhi::RTInstance_ForceOpacityMicromap2State, "FORCE_OMM_2_STATE" },
		{ 3, 30.0f, rhi::RTInstance_ForceOpaque, "FORCE_OPAQUE" },
	};
	constexpr uint32_t kOmmInstanceCount = 4;
	constexpr uint32_t kOmmRayCount = kOmmInstanceCount * kOmmTriangles * kOmmMicroCount;

	struct Bary { float b1, b2; };
	// Centroids of the uniform level-L grid: upright micro-triangles (i,j),(i+1,j),(i,j+1), inverted (i+1,j),(i,j+1),(i+1,j+1).
	std::vector<Bary> MicroCentroids(uint32_t level) {
		const uint32_t n = 1u << level;
		const float s = 1.0f / float(n);
		std::vector<Bary> out;
		for (uint32_t i = 0; i < n; ++i) {
			for (uint32_t j = 0; i + j < n; ++j) {
				out.push_back({ (float(i) + 1.0f / 3.0f) * s, (float(j) + 1.0f / 3.0f) * s });
				if (i + j + 1 < n) out.push_back({ (float(i) + 2.0f / 3.0f) * s, (float(j) + 2.0f / 3.0f) * s });
			}
		}
		return out;
	}

	// Packs one OMM's states: little-endian, micro-triangle i in bits [i*b, i*b + b).
	void PackOmm(std::vector<uint8_t>& data, uint32_t byteOffset, uint32_t omm) {
		const uint32_t bits = OmmIsTwoState(omm) ? 1u : 2u;
		for (uint32_t i = 0; i < kOmmMicroCount; ++i) {
			const uint32_t bit = i * bits;
			data[byteOffset + bit / 8] |= static_cast<uint8_t>(OmmPattern(omm, i) << (bit % 8));
		}
	}

	// Reads a state back under a bit-order hypothesis: LSB-first (the expectation) or MSB-first within each byte.
	uint32_t DecodeOmm(const std::vector<uint8_t>& data, uint32_t byteOffset, bool twoState, uint32_t index, bool msbFirst) {
		const uint32_t bits = twoState ? 1u : 2u;
		const uint32_t bit = index * bits;
		const uint32_t shift = msbFirst ? (8u - bits - bit % 8u) : bit % 8u;
		return (data[byteOffset + bit / 8] >> shift) & ((1u << bits) - 1u);
	}

	// Barycentric hypotheses: which pair of (w0, w1, w2) the reference code's (u, v) means.
	struct BaryMapping { const char* name; int u, v; };
	const BaryMapping kBaryMappings[] = { { "(u,v)=(b1,b2)", 1, 2 }, { "(u,v)=(b2,b1)", 2, 1 }, { "(u,v)=(b0,b1)", 0, 1 }, { "(u,v)=(b1,b0)", 1, 0 }, { "(u,v)=(b0,b2)", 0, 2 }, { "(u,v)=(b2,b0)", 2, 0 } };
	float BaryWeight(const Bary& b, int which) { return which == 0 ? 1.0f - b.b1 - b.b2 : which == 1 ? b.b1 : b.b2; }

	uint32_t ExpectedOutcome(uint32_t state, uint32_t instanceFlags, uint32_t rayFlags) {
		const bool disable = (instanceFlags & rhi::RTInstance_DisableOpacityMicromaps) != 0;
		const bool twoState = !disable && ((instanceFlags & rhi::RTInstance_ForceOpacityMicromap2State) != 0 || (rayFlags & kRayFlagForceOmm2State) != 0);
		const bool forceOpaque = (instanceFlags & rhi::RTInstance_ForceOpaque) != 0 || (rayFlags & kRayFlagForceOpaque) != 0;
		uint32_t s = disable ? NoOmm : state;
		if (twoState && s == UnknownTransparent) s = Transparent;
		if (twoState && s == UnknownOpaque) s = Opaque;
		if (s == Transparent) return Miss; // OMM classification comes first: nothing turns a transparent micro-triangle into a hit.
		if (s == Opaque) return OpaqueHit;
		return forceOpaque ? OpaqueHit : AnyHitHit; // Unknown, or no OMM on non-opaque geometry.
	}

	uint32_t Observed(const RayResult& r) { return r.hit ? (r.reserved ? AnyHitHit : OpaqueHit) : (r.reserved ? AnyHitMiss : Miss); }

	struct OmmScene {
		std::vector<uint8_t> data;
		uint32_t byteOffsets[kOmmCount]{};
		std::vector<Bary> centroids;
	};

	// The state a micro-triangle holds under the expected conventions.
	uint32_t ExpectedState(const OmmScene& s, uint32_t triangle, const Bary& b) {
		const OmmTriangleSource& src = kOmmTriangleSources[triangle];
		if (src.omm < 0) return src.uniformState;
		const uint32_t index = BarycentricsToSpaceFillingCurveIndex(b.b1, b.b2, kOmmLevel);
		return DecodeOmm(s.data, s.byteOffsets[src.omm], OmmIsTwoState(src.omm), index, false);
	}

	rhi::AccelerationStructureHandle BuildOmmTlas(Context& c, uint64_t blas) {
		std::vector<rhi::PackedRayTracingInstanceDesc> descs(kOmmInstanceCount);
		for (uint32_t i = 0; i < kOmmInstanceCount; ++i) {
			rhi::RayTracingInstanceDesc desc{};
			Translate(desc.transform, 0.0f);
			desc.transform[1][3] = kOmmInstances[i].y;
			desc.instanceID = kOmmInstances[i].id;
			desc.flags = static_cast<rhi::RayTracingInstanceFlags>(kOmmInstances[i].flags);
			descs[i] = rhi::PackRayTracingInstanceDesc(desc, blas);
		}
		rhi::Resource instanceBuffer = MakeUpload(c, descs, "OMM TLAS instances");
		rhi::RayTracingGeometryDesc geometry{};
		geometry.type = rhi::RayTracingGeometryType::Instances;
		geometry.instances.instanceBuffer = { instanceBuffer.GetHandle(), 0, descs.size() * sizeof(descs[0]) };
		geometry.instances.stride = sizeof(descs[0]);
		geometry.instances.count = static_cast<uint32_t>(descs.size());
		rhi::AccelerationStructureBuildInputs inputs{};
		inputs.type = rhi::RayTracingAccelerationStructureType::TopLevel;
		inputs.flags = rhi::RTASBuild_PreferFastTrace;
		inputs.geometries = { &geometry, 1 };
		rhi::AccelerationStructurePrebuildInfo prebuild{};
		if (!Check(c.device->GetAccelerationStructurePrebuildInfo(inputs, prebuild), "OMM TLAS prebuild")) return {};
		rhi::Resource storage = MakeAsStorage(c, prebuild.resultDataMaxSizeInBytes, "OMM TLAS data");
		rhi::Resource scratch = MakeUav(c, prebuild.scratchDataSizeInBytes, "OMM TLAS scratch");
		rhi::AccelerationStructureDesc tlasDesc{};
		tlasDesc.type = rhi::RayTracingAccelerationStructureType::TopLevel;
		tlasDesc.storage = storage.GetHandle();
		tlasDesc.sizeBytes = prebuild.resultDataMaxSizeInBytes;
		tlasDesc.flags = rhi::RTASBuild_PreferFastTrace;
		rhi::AccelerationStructurePtr tlas;
		if (!Check(c.device->CreateAccelerationStructure(tlasDesc, tlas), "OMM CreateAccelerationStructure")) return {};
		rhi::AccelerationStructureBuildDesc build{};
		build.destination = tlas->GetHandle();
		build.inputs = inputs;
		build.scratch = { scratch.GetHandle(), 0, prebuild.scratchDataSizeInBytes };
		if (!Submit(c, [&](rhi::CommandList& list) { list.BuildAccelerationStructures(&build, 1); })) return {};
		const rhi::AccelerationStructureHandle handle = tlas->GetHandle();
		c.keepAliveAs.push_back(std::move(tlas));
		return handle;
	}

	std::vector<RayResult> TraceOmm(Context& c, rhi::AccelerationStructureHandle tlas, rhi::Resource origins, uint32_t rayFlags, bool rayQuery) {
		const uint32_t sceneIndex = c.nextDescriptor++;
		const uint32_t resultsIndex = c.nextDescriptor++;
		const uint32_t raysIndex = c.nextDescriptor++;
		rhi::SrvDesc scene{};
		scene.dimension = rhi::SrvDim::AccelerationStruct;
		scene.accel.deviceAddress = 0;
		scene.accel.address = {};
		scene.accel.partitioned = false;
		scene.accel.accelerationStructure = tlas;
		if (!Check(c.device->CreateShaderResourceView({ c.heap->GetHandle(), sceneIndex }, {}, scene), "OMM scene SRV")) return {};
		rhi::SrvDesc rays{};
		rays.dimension = rhi::SrvDim::Buffer;
		rays.buffer.kind = rhi::BufferViewKind::Structured;
		rays.buffer.firstElement = 0;
		rays.buffer.numElements = kOmmRayCount;
		rays.buffer.structureByteStride = sizeof(float) * 4;
		if (!Check(c.device->CreateShaderResourceView({ c.heap->GetHandle(), raysIndex }, origins.GetHandle(), rays), "OMM rays SRV")) return {};
		rhi::Resource results = MakeUav(c, kOmmRayCount * sizeof(RayResult), "OMM ray results");
		rhi::UavDesc uav{};
		uav.dimension = rhi::UavDim::Buffer;
		uav.buffer.kind = rhi::BufferViewKind::Structured;
		uav.buffer.numElements = kOmmRayCount;
		uav.buffer.structureByteStride = sizeof(RayResult);
		if (!Check(c.device->CreateUnorderedAccessView({ c.heap->GetHandle(), resultsIndex }, results.GetHandle(), uav), "OMM results UAV")) return {};
		const uint32_t constants[4] = { sceneIndex, resultsIndex, raysIndex, rayFlags };

		if (rayQuery) {
			if (!Submit(c, [&](rhi::CommandList& list) {
				list.BindPipeline(c.ommQueryPipeline->GetHandle());
				list.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, 4, constants);
				list.Dispatch((kOmmRayCount + 63) / 64, 1, 1);
			})) return {};
		}
		else {
			const uint64_t handleSize = c.caps.shaderGroupHandleSize;
			const uint64_t stride = rhi::AlignRayTracingShaderRecordSize(handleSize, c.caps.shaderTableStrideAlignment);
			const uint64_t base = c.caps.shaderGroupBaseAlignment;
			std::vector<uint8_t> handles(handleSize * 3);
			if (!Check(c.device->GetRayTracingShaderGroupHandles(c.ommTracePipeline->GetHandle(), 0, 3, handles.data(), static_cast<uint32_t>(handles.size())), "OMM shader group handles")) return {};
			std::vector<uint8_t> table(base * 3);
			for (uint64_t group = 0; group < 3; ++group) std::memcpy(table.data() + base * group, handles.data() + handleSize * group, handleSize);
			rhi::Resource tableBuffer = MakeUpload(c, table, "OMM shader table");
			rhi::RayTracingDispatchDesc dispatch{};
			dispatch.rayGenerationShaderTable = { tableBuffer.GetHandle(), 0, stride, stride };
			dispatch.missShaderTable = { tableBuffer.GetHandle(), base, stride, stride };
			dispatch.hitGroupTable = { tableBuffer.GetHandle(), base * 2, stride, stride };
			dispatch.width = kOmmRayCount;
			if (!Submit(c, [&](rhi::CommandList& list) {
				list.BindPipeline(c.ommTracePipeline->GetHandle());
				list.PushConstants(rhi::ShaderStage::AllRayTracing, 0, 0, 0, 4, constants);
				list.TraceRays(dispatch);
			})) return {};
		}
		return Readback<RayResult>(c, results, kOmmRayCount);
	}

	bool CreateOmmPipelines(Context& c) {
		rhi::SubobjLayout layout{ c.layout->GetHandle() };
		rhi::SubobjShader cs{ rhi::ShaderStage::Compute, rhi::ShaderBinary{ g_ommQueryCS, static_cast<uint32_t>(sizeof(g_ommQueryCS)) }, "CSOmm" };
		rhi::PipelineStreamItem computeItems[] = { rhi::Make(layout), rhi::Make(cs) };
		if (!Check(c.device->CreatePipeline(computeItems, 2, c.ommQueryPipeline), "OMM ray query pipeline")) return false;

		const rhi::ShaderBinary library{ g_traceLibrary, static_cast<uint32_t>(sizeof(g_traceLibrary)) };
		rhi::SubobjShader shaders[4] = { { rhi::ShaderStage::RayGen, library, "RayGenOmm" }, { rhi::ShaderStage::Miss, library, "MissOmm" },
			{ rhi::ShaderStage::ClosestHit, library, "ClosestHitOmm" }, { rhi::ShaderStage::AnyHit, library, "AnyHitOmm" } };
		rhi::RayTracingShaderGroupDesc groups[3] = {
			{ .type = rhi::RayTracingShaderGroupType::General, .name = "RayGenOmm", .generalShader = "RayGenOmm" },
			{ .type = rhi::RayTracingShaderGroupType::General, .name = "MissOmm", .generalShader = "MissOmm" },
			{ .type = rhi::RayTracingShaderGroupType::TrianglesHitGroup, .name = "OmmHitGroup", .closestHitShader = "ClosestHitOmm", .anyHitShader = "AnyHitOmm" },
		};
		rhi::SubobjRayTracingPipeline rt{};
		rt.globalLayout = c.layout->GetHandle();
		rt.shaders = { shaders, 4 };
		rt.shaderGroups = { groups, 3 };
		rt.shaderConfig.maxPayloadSizeInBytes = sizeof(RayResult) + sizeof(uint32_t);
		rt.shaderConfig.maxAttributeSizeInBytes = 8;
		rt.pipelineConfig.maxTraceRecursionDepth = 1;
		rt.flags = static_cast<rhi::RayTracingPipelineFlags>(rhi::RTPipeline_AllowClusteredGeometry | rhi::RTPipeline_AllowOpacityMicromaps);
		rhi::PipelineStreamItem rtItems[] = { rhi::Make(rt) };
		return Check(c.device->CreatePipeline(rtItems, 1, c.ommTracePipeline), "OMM raytracing pipeline");
	}

	// Compares one pass against the model; prints the first few mismatches.
	bool ExpectOmmPass(const char* pass, const OmmScene& s, const std::vector<RayResult>& r, uint32_t rayFlags, uint32_t instanceMask = 0xFu) {
		if (r.size() != kOmmRayCount) {
			std::fprintf(stderr, "%s: no results\n", pass);
			return false;
		}
		uint32_t mismatches = 0, checked = 0;
		uint32_t outcomeCounts[4]{};
		for (uint32_t inst = 0; inst < kOmmInstanceCount; ++inst) {
			if ((instanceMask & (1u << inst)) == 0) continue;
			for (uint32_t tri = 0; tri < kOmmTriangles; ++tri) {
				for (uint32_t m = 0; m < kOmmMicroCount; ++m) {
					const RayResult& got = r[(inst * kOmmTriangles + tri) * kOmmMicroCount + m];
					const Bary& b = s.centroids[m];
					const uint32_t state = ExpectedState(s, tri, b);
					const uint32_t wanted = ExpectedOutcome(state, kOmmInstances[inst].flags, rayFlags);
					const uint32_t observed = Observed(got);
					++checked;
					++outcomeCounts[observed];
					const bool clusterB = tri >= kOmmTrianglesA;
					bool ok = observed == wanted;
					if (ok && got.hit) {
						ok = got.instanceId == kOmmInstances[inst].id && got.clusterId == (clusterB ? kOmmClusterBId : kOmmClusterAId) && got.primitiveIndex == (clusterB ? tri - kOmmTrianglesA : tri)
							&& Near(got.baryU, b.b1) && Near(got.baryV, b.b2) && Near(got.t, 1.0f);
					}
					if (!ok && mismatches++ < 8) {
						std::fprintf(stderr, "%s: instance %s triangle %u micro (%.4f,%.4f) state %u: got %s (hit=%u anyhits=%u instance=%u cluster=%u prim=%u bary=(%.4f,%.4f) t=%.3f), wanted %s\n",
							pass, kOmmInstances[inst].name, tri, b.b1, b.b2, state, OutcomeName(observed), got.hit, got.reserved, got.instanceId, got.clusterId, got.primitiveIndex, got.baryU, got.baryV, got.t, OutcomeName(wanted));
					}
				}
			}
		}
		std::printf("  %-44s %4u rays: miss=%u opaque-hit=%u any-hit=%u any-hit-miss=%u -> %s\n", pass, checked, outcomeCounts[0], outcomeCounts[1], outcomeCounts[2], outcomeCounts[3], mismatches ? "MISMATCH" : "ok");
		if (mismatches) std::fprintf(stderr, "%s: %u of %u rays mismatched\n", pass, mismatches, checked);
		return mismatches == 0;
	}

	// Recovers each micro-triangle's state on the plain instance from a 4-state and a forced 2-state pass, then scores the
	// barycentric-mapping and bit-order hypotheses against it.
	bool DetermineOmmConventions(const OmmScene& s, const std::vector<RayResult>& fourState, const std::vector<RayResult>& twoState) {
		if (fourState.size() != kOmmRayCount || twoState.size() != kOmmRayCount) return false;
		const uint32_t ommTriangles[] = { 0, 1, 4, 5 };
		bool expectedWins = false;
		std::puts("  convention scores (micro-triangles matched, of 256 over OMMs 0-3 on the plain instance):");
		for (const BaryMapping& mapping : kBaryMappings) {
			for (int msb = 0; msb < 2; ++msb) {
				uint32_t matched = 0;
				for (uint32_t tri : ommTriangles) {
					const OmmTriangleSource& src = kOmmTriangleSources[tri];
					for (uint32_t m = 0; m < kOmmMicroCount; ++m) {
						const uint32_t ray = tri * kOmmMicroCount + m; // instance 0
						uint32_t observed = NoOmm;
						switch (Observed(fourState[ray])) {
						case Miss: observed = Transparent; break;
						case OpaqueHit: observed = Opaque; break;
						case AnyHitHit: observed = Observed(twoState[ray]) == OpaqueHit ? UnknownOpaque : (Observed(twoState[ray]) == Miss ? UnknownTransparent : NoOmm); break;
						default: break;
						}
						const Bary& b = s.centroids[m];
						const uint32_t index = BarycentricsToSpaceFillingCurveIndex(BaryWeight(b, mapping.u), BaryWeight(b, mapping.v), kOmmLevel);
						if (DecodeOmm(s.data, s.byteOffsets[src.omm], OmmIsTwoState(src.omm), index, msb != 0) == observed) ++matched;
					}
				}
				std::printf("    %s, %s: %u\n", mapping.name, msb ? "MSB-first" : "LSB-first", matched);
				if (mapping.u == 1 && mapping.v == 2 && msb == 0 && matched == 4 * kOmmMicroCount) expectedWins = true;
			}
		}
		if (!expectedWins) std::fputs("OMM conventions: (u,v)=(b1,b2) with LSB-first states did not match every micro-triangle\n", stderr);
		return expectedWins;
	}

	bool RunOpacityMicromapTests(Context& c) {
		std::puts("opacity micromaps:");
		if (!CreateOmmPipelines(c)) return false;

		// Prebuild scaling.
		std::puts("  OMM array prebuild (OC1_4State): count, level -> result bytes, scratch bytes");
		for (uint32_t level : { 0u, 3u, 6u }) {
			for (uint32_t count : { 1u, 2u, 4u, 16u, 64u, 1024u, 16384u }) {
				const rhi::RayTracingOpacityMicromapHistogramEntry entry{ count, level, rhi::RayTracingOpacityMicromapFormat::OC1_4State };
				rhi::RayTracingOpacityMicromapArrayInputs inputs{};
				inputs.flags = rhi::RTASBuild_PreferFastTrace;
				inputs.histogram = { &entry, 1 };
				rhi::AccelerationStructurePrebuildInfo info{};
				if (!Check(c.device->GetOpacityMicromapArrayPrebuildInfo(inputs, info), "OMM prebuild")) return false;
				std::printf("    %6u x L%u -> %10llu %10llu\n", count, level, info.resultDataMaxSizeInBytes, info.scratchDataSizeInBytes);
			}
		}

		OmmScene s;
		s.centroids = MicroCentroids(kOmmLevel);
		if (s.centroids.size() != kOmmMicroCount) return false;
		{
			// Every micro-triangle index must be reached exactly once by the centroids under the reference ordering.
			std::vector<uint32_t> seen(kOmmMicroCount, 0);
			for (const Bary& b : s.centroids) ++seen[std::min(BarycentricsToSpaceFillingCurveIndex(b.b1, b.b2, kOmmLevel), kOmmMicroCount - 1)];
			for (uint32_t i = 0; i < kOmmMicroCount; ++i) if (seen[i] != 1) { std::fprintf(stderr, "centroid grid does not cover micro-triangle %u once\n", i); return false; }
		}
		s.data.assign(128, 0);
		const uint32_t offsets[kOmmCount] = { 0, 16, 32, 48 };
		std::vector<rhi::PackedRayTracingOpacityMicromapDesc> descs(kOmmCount);
		for (uint32_t omm = 0; omm < kOmmCount; ++omm) {
			s.byteOffsets[omm] = offsets[omm];
			PackOmm(s.data, offsets[omm], omm);
			descs[omm].byteOffset = offsets[omm];
			descs[omm].subdivisionLevel = kOmmLevel;
			descs[omm].format = static_cast<uint16_t>(OmmIsTwoState(omm) ? rhi::RayTracingOpacityMicromapFormat::OC1_2State : rhi::RayTracingOpacityMicromapFormat::OC1_4State);
		}
		const rhi::RayTracingOpacityMicromapHistogramEntry histogram[2] = {
			{ 3, kOmmLevel, rhi::RayTracingOpacityMicromapFormat::OC1_4State },
			{ 1, kOmmLevel, rhi::RayTracingOpacityMicromapFormat::OC1_2State },
		};
		rhi::RayTracingOpacityMicromapArrayBuildDesc ommBuild{};
		ommBuild.inputs.flags = rhi::RTASBuild_PreferFastTrace;
		ommBuild.inputs.histogram = { histogram, 2 };
		rhi::AccelerationStructurePrebuildInfo ommPrebuild{};
		if (!Check(c.device->GetOpacityMicromapArrayPrebuildInfo(ommBuild.inputs, ommPrebuild), "OMM array prebuild")) return false;
		std::printf("  OMM array (3 x OC1_4 L3 + 1 x OC1_2 L3): result %llu bytes, scratch %llu bytes\n", ommPrebuild.resultDataMaxSizeInBytes, ommPrebuild.scratchDataSizeInBytes);
		rhi::Resource ommInput = MakeUpload(c, s.data, "OMM input");
		rhi::Resource ommDescs = MakeUpload(c, descs, "OMM descs");
		rhi::Resource ommArray = MakeAsStorage(c, ommPrebuild.resultDataMaxSizeInBytes, "OMM array");
		rhi::Resource ommScratch = MakeUav(c, std::max<uint64_t>(ommPrebuild.scratchDataSizeInBytes, 256), "OMM scratch");
		ommBuild.inputBuffer = { ommInput.GetHandle(), 0 };
		ommBuild.perOmmDescs = { ommDescs.GetHandle(), 0, descs.size() * sizeof(descs[0]), sizeof(descs[0]) };
		ommBuild.destination = { ommArray.GetHandle(), 0 };
		ommBuild.scratch = { ommScratch.GetHandle(), 0 };

		// Geometry: three unshared vertices per triangle.
		std::vector<Vec3> positions;
		for (uint32_t tri = 0; tri < kOmmTriangles; ++tri) {
			const float x = 2.0f * float(tri);
			positions.push_back({ x, 0, 0 });
			positions.push_back({ x + 1, 0, 0 });
			positions.push_back({ x, 1, 0 });
		}
		std::vector<uint8_t> indices;
		for (uint32_t i = 0; i < kOmmTrianglesA * 3; ++i) indices.push_back(static_cast<uint8_t>(i));
		for (uint32_t i = 0; i < kOmmTrianglesB * 3; ++i) indices.push_back(static_cast<uint8_t>(i));
		// 32-bit OMM indices, cluster A's at 0 and cluster B's at 16 bytes.
		const std::vector<int32_t> ommIndices = { 0, 1, rhi::RTOmmSpecialIndex_FullyTransparent, rhi::RTOmmSpecialIndex_FullyOpaque,
			1, 2, rhi::RTOmmSpecialIndex_FullyUnknownOpaque, rhi::RTOmmSpecialIndex_FullyUnknownTransparent, rhi::RTOmmSpecialIndex_ClusterSkipOpacityMicromap };
		rhi::Resource vertexBuffer = MakeUpload(c, positions, "OMM vertices");
		rhi::Resource indexBuffer = MakeUpload(c, indices, "OMM indices");
		rhi::Resource ommIndexBuffer = MakeUpload(c, ommIndices, "OMM index buffer");

		std::vector<rhi::PackedDxr2ClusterBuildTriangleInfo> args(2);
		for (uint32_t cluster = 0; cluster < 2; ++cluster) {
			rhi::PackedDxr2ClusterBuildTriangleInfo& a = args[cluster];
			const uint32_t triangles = cluster == 0 ? kOmmTrianglesA : kOmmTrianglesB;
			const uint32_t firstTriangle = cluster == 0 ? 0 : kOmmTrianglesA;
			a.clusterID = cluster == 0 ? kOmmClusterAId : kOmmClusterBId;
			a.clusterFlags = rhi::RTCluster_AllowDisableOpacityMicromaps;
			a.triangleCount = static_cast<uint16_t>(triangles);
			a.uniqueVertexIndexCount = static_cast<uint16_t>(triangles * 3);
			a.baseGeometryIndexAndFlags = rhi::PackDxr2ClusterGeometryIndexAndFlags(0, rhi::RTClusterGeometry_None); // non-opaque
			a.opacityMicromapBaseLocation = cluster == 0 ? 0 : 1;
			a.vertexBufferStride = sizeof(Vec3);
			a.indexBufferStride = 1;
			a.opacityMicromapIndexBufferStride = sizeof(int32_t);
			a.vertexBuffer = Va(c, vertexBuffer, firstTriangle * 3 * sizeof(Vec3));
			a.indexBuffer = Va(c, indexBuffer, firstTriangle * 3);
			a.opacityMicromapArray = Va(c, ommArray);
			a.opacityMicromapIndexBuffer = Va(c, ommIndexBuffer, firstTriangle * sizeof(int32_t));
		}
		rhi::Resource argsBuffer = MakeUpload(c, args, "OMM cluster args");
		const uint64_t argsBytes = args.size() * sizeof(args[0]);

		rhi::RayTracingClusterTriangleInputsDesc triangleInputs{};
		triangleInputs.maxClusterTriangleCount = kOmmTrianglesB;
		triangleInputs.maxClusterVertexCount = kOmmTrianglesB * 3;
		triangleInputs.maxTotalTriangleCount = kOmmTriangles;
		triangleInputs.maxTotalVertexCount = kOmmTriangles * 3;
		triangleInputs.opacityMicromapIndexFormat = rhi::RayTracingClusterIndexFormat::Uint32;
		triangleInputs.maxOpacityMicromapIndicesPerCluster = kOmmTrianglesB;
		rhi::RayTracingRtasOperationInputs clasInputs{};
		clasInputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
		clasInputs.flags = rhi::RTASBuild_PreferFastTrace;
		clasInputs.maxAccelerationStructureCount = 2;
		clasInputs.triangleClusters = &triangleInputs;
		clasInputs.allowOpacityMicromaps = true;
		rhi::RayTracingRtasPrebuildInfo clasPrebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(clasInputs, clasPrebuild), "OMM CLAS prebuild")) return false;
		{
			rhi::RayTracingClusterTriangleInputsDesc plainTriangles = triangleInputs;
			plainTriangles.opacityMicromapIndexFormat = rhi::RayTracingClusterIndexFormat::None;
			rhi::RayTracingRtasOperationInputs plain = clasInputs;
			plain.triangleClusters = &plainTriangles;
			plain.allowOpacityMicromaps = false;
			rhi::RayTracingRtasPrebuildInfo plainPrebuild{};
			if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(plain, plainPrebuild), "plain CLAS prebuild")) return false;
			std::printf("  CLAS batch prebuild: with OMMs result=%llu scratch=%llu, without result=%llu scratch=%llu\n",
				clasPrebuild.resultDataMaxSizeInBytes, clasPrebuild.scratchDataSizeInBytes, plainPrebuild.resultDataMaxSizeInBytes, plainPrebuild.scratchDataSizeInBytes);
		}
		rhi::Resource clasData = MakeAsStorage(c, clasPrebuild.resultDataMaxSizeInBytes, "OMM CLAS data");
		rhi::Resource clasScratch = MakeUav(c, std::max<uint64_t>(clasPrebuild.scratchDataSizeInBytes, 256), "OMM CLAS scratch");
		rhi::Resource clasAddresses = MakeUav(c, 16, "OMM CLAS addresses");
		rhi::Resource clasSizes = MakeUav(c, 16, "OMM CLAS sizes");
		rhi::RayTracingRtasOperationDesc clasBuild{};
		clasBuild.inputs = clasInputs;
		clasBuild.batched.dstImplicitData = { clasData.GetHandle(), 0 };
		clasBuild.batched.scratchData = { clasScratch.GetHandle(), 0 };
		clasBuild.batched.dstAddressesArray = { clasAddresses.GetHandle(), 0, 16, 8 };
		clasBuild.batched.dstSizesArray = { clasSizes.GetHandle(), 0, 16, 8 };
		clasBuild.batched.srcInfosArray = { argsBuffer.GetHandle(), 0, argsBytes, sizeof(args[0]) };

		// OMM array build, then the CLAS build that reads it, in one command list.
		rhi::BufferBarrier ommReady{};
		ommReady.buffer = ommArray.GetHandle();
		ommReady.beforeSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;
		ommReady.afterSync = rhi::ResourceSyncState::BuildRaytracingAccelerationStructure;
		ommReady.beforeAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureWrite;
		ommReady.afterAccess = rhi::ResourceAccessType::RaytracingAccelerationStructureRead;
		if (!Submit(c, [&](rhi::CommandList& list) {
			list.BuildOpacityMicromapArrays(&ommBuild, 1);
			rhi::BarrierBatch batch{};
			batch.buffers = { &ommReady, 1 };
			list.Barriers(batch);
			list.ExecuteIndirectRtasOperations(&clasBuild, 1);
		})) return false;
		const auto addresses = Readback<uint64_t>(c, clasAddresses, 2);
		const auto sizes = Readback<uint32_t>(c, clasSizes, 4);
		if (addresses.size() != 2 || addresses[0] == 0 || addresses[1] == 0 || sizes.size() != 4 || sizes[0] == 0 || sizes[2] == 0) {
			std::fputs("OMM CLAS build returned invalid addresses or sizes\n", stderr);
			return false;
		}
		std::printf("  OMM CLAS sizes: A=%u B=%u\n", sizes[0], sizes[2]);

		const uint64_t blas = BuildBlas(c, Va(c, clasAddresses), 2, true);
		if (blas == 0) return false;
		const rhi::AccelerationStructureHandle tlas = BuildOmmTlas(c, blas);
		if (!tlas.valid()) return false;

		std::vector<float> origins;
		origins.reserve(kOmmRayCount * 4);
		for (uint32_t inst = 0; inst < kOmmInstanceCount; ++inst) {
			for (uint32_t tri = 0; tri < kOmmTriangles; ++tri) {
				for (const Bary& b : s.centroids) {
					origins.insert(origins.end(), { 2.0f * float(tri) + b.b1, kOmmInstances[inst].y + b.b2, -1.0f, 0.0f });
				}
			}
		}
		rhi::Resource originBuffer = MakeUpload(c, origins, "OMM ray origins");

		bool ok = true;
		const auto traced = TraceOmm(c, tlas, originBuffer, 0, false);
		const auto traced2 = TraceOmm(c, tlas, originBuffer, kRayFlagForceOmm2State, false);
		ok &= DetermineOmmConventions(s, traced, traced2);
		ok &= ExpectOmmPass("TraceRay, RAY_FLAG_NONE", s, traced, 0);
		ok &= ExpectOmmPass("TraceRay, RAY_FLAG_FORCE_OMM_2_STATE", s, traced2, kRayFlagForceOmm2State);
		ok &= ExpectOmmPass("TraceRay, RAY_FLAG_FORCE_OPAQUE", s, TraceOmm(c, tlas, originBuffer, kRayFlagForceOpaque, false), kRayFlagForceOpaque);
		ok &= ExpectOmmPass("RayQuery, RAY_FLAG_NONE", s, TraceOmm(c, tlas, originBuffer, 0, true), 0);
		ok &= ExpectOmmPass("RayQuery, RAY_FLAG_FORCE_OMM_2_STATE", s, TraceOmm(c, tlas, originBuffer, kRayFlagForceOmm2State, true), kRayFlagForceOmm2State);
		ok &= ExpectOmmPass("RayQuery, RAY_FLAG_FORCE_OPAQUE", s, TraceOmm(c, tlas, originBuffer, kRayFlagForceOpaque, true), kRayFlagForceOpaque);

		// Move both CLAS (ALLOW_OMM on the move), rebuild the BLAS from the copies, and trace again.
		{
			const uint64_t second = (static_cast<uint64_t>(sizes[0]) + 127u) & ~127ull;
			const uint64_t poolBytes = second + ((static_cast<uint64_t>(sizes[2]) + 127u) & ~127ull);
			rhi::RayTracingClusterMovesDesc moves{};
			moves.type = rhi::RayTracingClusterAccelerationStructureType::TriangleCluster;
			moves.noMoveOverlap = true;
			moves.maxMovedBytes = poolBytes;
			rhi::RayTracingRtasOperationInputs moveInputs{};
			moveInputs.operationType = rhi::RayTracingRtasOperationType::MoveObjects;
			moveInputs.operationMode = rhi::RayTracingRtasOperationMode::ExplicitDestinations;
			moveInputs.maxAccelerationStructureCount = 2;
			moveInputs.moveObjects = &moves;
			moveInputs.allowOpacityMicromaps = true;
			rhi::RayTracingRtasPrebuildInfo movePrebuild{};
			if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(moveInputs, movePrebuild), "OMM move prebuild")) return false;
			rhi::Resource moved = MakeAsStorage(c, poolBytes, "moved OMM CLAS pool");
			const std::vector<uint64_t> movedDestinations = { Va(c, moved, 0), Va(c, moved, second) };
			rhi::Resource movedDestinationBuffer = MakeUpload(c, movedDestinations, "OMM move destinations");
			const std::vector<rhi::PackedRayTracingClusterMoveObjectsInfo> moveArgs = { { addresses[0] }, { addresses[1] } };
			rhi::Resource moveArgsBuffer = MakeUpload(c, moveArgs, "OMM move args");
			rhi::Resource moveScratch = MakeUav(c, std::max<uint64_t>(movePrebuild.scratchDataSizeInBytes, 256), "OMM move scratch");
			rhi::RayTracingRtasOperationDesc move{};
			move.inputs = moveInputs;
			move.batched.scratchData = { moveScratch.GetHandle(), 0 };
			move.batched.dstAddressesArray = { movedDestinationBuffer.GetHandle(), 0, 16, 8 };
			move.batched.srcInfosArray = { moveArgsBuffer.GetHandle(), 0, 16, 8 };
			if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&move, 1); })) return false;
			const uint64_t movedBlas = BuildBlas(c, Va(c, movedDestinationBuffer), 2, true);
			const rhi::AccelerationStructureHandle movedTlas = movedBlas ? BuildOmmTlas(c, movedBlas) : rhi::AccelerationStructureHandle{};
			if (!movedTlas.valid()) return false;
			ok &= ExpectOmmPass("moved CLAS, TraceRay, RAY_FLAG_NONE", s, TraceOmm(c, movedTlas, originBuffer, 0, false), 0);
		}
		return ok;
	}
}

int main() {
	Context c;
	rhi::DeviceCreateInfo createInfo{};
	createInfo.backend = rhi::Backend::D3D12;
	// BASICRENDERER_VULKAN_VALIDATION (the renderer's switch for both backends' validation) set to 0 runs without the debug layer.
	bool debugLayer = true;
	if (const char* validation = std::getenv("BASICRENDERER_VULKAN_VALIDATION"); validation && validation[0] != '\0') debugLayer = std::string(validation) != "0";
	createInfo.enableDebug = debugLayer;
	std::printf("BasicRHID3D12RaytracingSmoke: D3D12 debug layer %s\n", debugLayer ? "on" : "off");
	createInfo.enablePreviewRayTracing = true;
	if (!Check(rhi::CreateD3D12Device(createInfo, c.device), "CreateD3D12Device")) return 1;

	if (!rhi::IsOk(c.device->QueryFeatureInfo(&c.caps.header)) || !c.caps.clusterAccelerationStructure || !c.caps.partitionedAccelerationStructure) {
		std::puts("BasicRHID3D12RaytracingSmoke: skipped, clusters and partitioned TLAS unavailable (driver, Developer Mode or runtime)");
		return kSkip;
	}
	std::printf("BasicRHID3D12RaytracingSmoke: shaderExecutionReordering=%d shaderExecutionReorderingActuallyReorders=%d\n", c.caps.shaderExecutionReordering, c.caps.shaderExecutionReorderingActuallyReorders);
	if (c.caps.maxClusterTriangles < 256 || c.caps.maxClusterVertices < 256 || c.caps.clusterAlignment != 128) {
		std::fprintf(stderr, "unexpected cluster caps: tris=%u verts=%u align=%llu\n", c.caps.maxClusterTriangles, c.caps.maxClusterVertices, c.caps.clusterAlignment);
		return 1;
	}

	ID3D12Device* nativeDevice = rhi::dx12::get_device(c.device.Get());
	UINT64 firstMessage = 0;
	if (ID3D12InfoQueue* queue = nullptr; nativeDevice && SUCCEEDED(nativeDevice->QueryInterface(IID_PPV_ARGS(&queue)))) {
		firstMessage = queue->GetNumStoredMessagesAllowedByRetrievalFilter();
		queue->Release();
	}

	c.queue = c.device->GetQueue(rhi::QueueKind::Graphics);
	rhi::DescriptorHeapDesc heapDesc{};
	heapDesc.type = rhi::DescriptorHeapType::CbvSrvUav;
	heapDesc.capacity = 128;
	heapDesc.shaderVisible = true;
	heapDesc.debugName = "raytracing smoke heap";
	rhi::DescriptorHeapDesc samplerHeapDesc{};
	samplerHeapDesc.type = rhi::DescriptorHeapType::Sampler;
	samplerHeapDesc.capacity = 1;
	samplerHeapDesc.shaderVisible = true;
	samplerHeapDesc.debugName = "raytracing smoke samplers";
	if (!c.queue
		|| !Check(c.device->CreateCommandAllocator(rhi::QueueKind::Graphics, c.allocator), "CreateCommandAllocator")
		|| !Check(c.device->CreateCommandList(rhi::QueueKind::Graphics, c.allocator.Get(), c.commandList), "CreateCommandList")
		|| !Check(c.device->CreateDescriptorHeap(heapDesc, c.heap), "CreateDescriptorHeap")
		|| !Check(c.device->CreateDescriptorHeap(samplerHeapDesc, c.samplerHeap), "CreateDescriptorHeap samplers")
		|| !CreatePipelines(c)) {
		return 1;
	}

	rhi::Resource vertices = MakeUpload(c, kPositions, "vertices");
	rhi::Resource indices = MakeUpload(c, kIndices, "indices");
	const std::vector<rhi::PackedDxr2ClusterBuildTriangleInfo> clusterArgs = {
		ClusterArgs(c, vertices, indices, kClusterAId, 2, 4, 0, 0),
		ClusterArgs(c, vertices, indices, kClusterBId, 1, 3, kClusterBFirstVertex, kClusterBFirstIndex),
	};
	rhi::Resource clusterArgsBuffer = MakeUpload(c, clusterArgs, "cluster args");
	const uint64_t clusterArgsBytes = clusterArgs.size() * sizeof(clusterArgs[0]);
	const auto triangleInputs = ClusterInputs();

	bool ok = true;

	// --- Implicit destinations: the path the renderer's streaming uses first.
	rhi::Resource clasAddresses = MakeUav(c, 16, "CLAS addresses");
	{
		rhi::RayTracingRtasOperationInputs inputs{};
		inputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
		inputs.flags = rhi::RTASBuild_PreferFastTrace;
		inputs.maxAccelerationStructureCount = 2;
		inputs.triangleClusters = &triangleInputs;
		rhi::RayTracingRtasPrebuildInfo prebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(inputs, prebuild), "CLAS prebuild")) return 1;
		rhi::Resource result = MakeAsStorage(c, prebuild.resultDataMaxSizeInBytes, "CLAS data");
		rhi::Resource scratch = MakeUav(c, prebuild.scratchDataSizeInBytes, "CLAS scratch");
		rhi::Resource sizes = MakeUav(c, 16, "CLAS sizes");
		rhi::RayTracingRtasOperationDesc desc{};
		desc.inputs = inputs;
		desc.batched.dstImplicitData = { result.GetHandle(), 0 };
		desc.batched.scratchData = { scratch.GetHandle(), 0 };
		desc.batched.dstAddressesArray = { clasAddresses.GetHandle(), 0, 16, 8 };
		desc.batched.dstSizesArray = { sizes.GetHandle(), 0, 16, 8 };
		desc.batched.srcInfosArray = { clusterArgsBuffer.GetHandle(), 0, clusterArgsBytes, sizeof(clusterArgs[0]) };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&desc, 1); })) return 1;
		const auto addresses = Readback<uint64_t>(c, clasAddresses, 2);
		const auto builtSizes = Readback<uint32_t>(c, sizes, 4);
		if (addresses.size() != 2 || addresses[0] == 0 || addresses[1] == 0 || (addresses[0] % 128) != 0 || (addresses[1] % 128) != 0 || builtSizes.empty() || builtSizes[0] == 0) {
			std::fputs("implicit CLAS build returned invalid addresses or sizes\n", stderr);
			return 1;
		}
		std::printf("CLAS sizes: quad=%u triangle=%u, batch prebuild=%llu scratch=%llu\n", builtSizes[0], builtSizes[2], prebuild.resultDataMaxSizeInBytes, prebuild.scratchDataSizeInBytes);
	}
	const uint64_t blas = BuildBlas(c, Va(c, clasAddresses), 2);
	if (blas == 0 || (blas % 256) != 0) {
		std::fputs("BLAS build returned an invalid address\n", stderr);
		return 1;
	}

	const rhi::AccelerationStructureHandle tlas = BuildClassicTlas(c, { { 7u, 0.0f }, { 8u, 10.0f } }, blas);
	if (!tlas.valid()) return 1;
	ok &= ExpectStandardScene("classic TLAS, ray query", TraceHandle(c, tlas, true));
	const auto traced = TraceHandle(c, tlas, false);
	ok &= ExpectStandardScene("classic TLAS, TraceRay", traced);
	ok &= ExpectHitShaderFeatures("classic TLAS, TraceRay", traced, 3u);
	const auto reordered = TraceReorderedIndirect(c, tlas);
	ok &= ExpectStandardScene("classic TLAS, reordered indirect", reordered);
	ok &= ExpectHitShaderFeatures("classic TLAS, reordered indirect", reordered, 7u);

	// --- Get-sizes, explicit destinations and a move: what the persistent CLAS cache does.
	{
		rhi::RayTracingRtasOperationInputs inputs{};
		inputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleCluster;
		inputs.operationMode = rhi::RayTracingRtasOperationMode::ComputeSizes;
		inputs.flags = rhi::RTASBuild_PreferFastTrace;
		inputs.maxAccelerationStructureCount = 2;
		inputs.triangleClusters = &triangleInputs;
		rhi::RayTracingRtasPrebuildInfo sizePrebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(inputs, sizePrebuild), "get-sizes prebuild")) return 1;
		rhi::Resource scratch = MakeUav(c, sizePrebuild.scratchDataSizeInBytes, "get-sizes scratch");
		rhi::Resource sizes = MakeUav(c, 16, "get-sizes output");
		rhi::RayTracingRtasOperationDesc desc{};
		desc.inputs = inputs;
		desc.batched.scratchData = { scratch.GetHandle(), 0 };
		desc.batched.dstSizesArray = { sizes.GetHandle(), 0, 16, 8 };
		desc.batched.srcInfosArray = { clusterArgsBuffer.GetHandle(), 0, clusterArgsBytes, sizeof(clusterArgs[0]) };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&desc, 1); })) return 1;
		const auto measured = Readback<uint32_t>(c, sizes, 4);
		if (measured.size() != 4 || measured[0] == 0 || measured[2] == 0) {
			std::fputs("get-sizes returned zero\n", stderr);
			return 1;
		}

		const uint64_t first = 0;
		const uint64_t second = (static_cast<uint64_t>(measured[0]) + 127u) & ~127ull;
		const uint64_t poolBytes = second + ((static_cast<uint64_t>(measured[2]) + 127u) & ~127ull);
		rhi::Resource pool = MakeAsStorage(c, poolBytes, "explicit CLAS pool");
		const std::vector<uint64_t> destinations = { Va(c, pool, first), Va(c, pool, second) };
		rhi::Resource destinationBuffer = MakeUpload(c, destinations, "explicit destinations");

		inputs.operationMode = rhi::RayTracingRtasOperationMode::ExplicitDestinations;
		rhi::RayTracingRtasPrebuildInfo explicitPrebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(inputs, explicitPrebuild), "explicit prebuild")) return 1;
		rhi::Resource explicitScratch = MakeUav(c, explicitPrebuild.scratchDataSizeInBytes, "explicit scratch");
		rhi::RayTracingRtasOperationDesc build{};
		build.inputs = inputs;
		build.batched.scratchData = { explicitScratch.GetHandle(), 0 };
		build.batched.dstAddressesArray = { destinationBuffer.GetHandle(), 0, 16, 8 };
		build.batched.srcInfosArray = { clusterArgsBuffer.GetHandle(), 0, clusterArgsBytes, sizeof(clusterArgs[0]) };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&build, 1); })) return 1;

		// Move both into a second allocation, then build from the moved copies.
		rhi::RayTracingClusterMovesDesc moves{};
		moves.type = rhi::RayTracingClusterAccelerationStructureType::TriangleCluster;
		moves.noMoveOverlap = true;
		moves.maxMovedBytes = poolBytes;
		rhi::RayTracingRtasOperationInputs moveInputs{};
		moveInputs.operationType = rhi::RayTracingRtasOperationType::MoveObjects;
		moveInputs.operationMode = rhi::RayTracingRtasOperationMode::ExplicitDestinations;
		moveInputs.maxAccelerationStructureCount = 2;
		moveInputs.moveObjects = &moves;
		rhi::RayTracingRtasPrebuildInfo movePrebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(moveInputs, movePrebuild), "move prebuild")) return 1;
		rhi::Resource moved = MakeAsStorage(c, poolBytes, "moved CLAS pool");
		const std::vector<uint64_t> movedDestinations = { Va(c, moved, first), Va(c, moved, second) };
		rhi::Resource movedDestinationBuffer = MakeUpload(c, movedDestinations, "move destinations");
		const std::vector<rhi::PackedRayTracingClusterMoveObjectsInfo> moveArgs = { { destinations[0] }, { destinations[1] } };
		rhi::Resource moveArgsBuffer = MakeUpload(c, moveArgs, "move args");
		rhi::Resource moveScratch = MakeUav(c, std::max<uint64_t>(movePrebuild.scratchDataSizeInBytes, 256), "move scratch");
		rhi::RayTracingRtasOperationDesc move{};
		move.inputs = moveInputs;
		move.batched.scratchData = { moveScratch.GetHandle(), 0 };
		move.batched.dstAddressesArray = { movedDestinationBuffer.GetHandle(), 0, 16, 8 };
		move.batched.srcInfosArray = { moveArgsBuffer.GetHandle(), 0, 16, 8 };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&move, 1); })) return 1;

		const uint64_t movedBlas = BuildBlas(c, Va(c, movedDestinationBuffer), 2);
		const rhi::AccelerationStructureHandle movedTlas = BuildClassicTlas(c, { { 7u, 0.0f }, { 8u, 10.0f } }, movedBlas);
		if (movedBlas == 0 || !movedTlas.valid()) return 1;
		ok &= ExpectStandardScene("explicit + moved CLAS", TraceHandle(c, movedTlas, true));
	}

	// --- Partitioned TLAS, then an update that re-points nothing but shifts the partition.
	{
		rhi::RayTracingPartitionedTlasInputsDesc partitionedInputs{};
		partitionedInputs.flags = rhi::RTASBuild_PreferFastTrace;
		partitionedInputs.instanceCount = 2;
		partitionedInputs.maxInstancePerPartitionCount = 2;
		partitionedInputs.partitionCount = 1;
		partitionedInputs.enablePartitionTranslation = true;
		rhi::RayTracingRtasOperationInputs inputs{};
		inputs.operationType = rhi::RayTracingRtasOperationType::BuildPartitionedTopLevel;
		inputs.partitionedTopLevel = &partitionedInputs;
		rhi::RayTracingRtasPrebuildInfo prebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(inputs, prebuild), "PTLAS prebuild")) return 1;
		rhi::Resource storage = MakeAsStorage(c, prebuild.resultDataMaxSizeInBytes, "PTLAS data");
		rhi::Resource scratch = MakeUav(c, prebuild.scratchDataSizeInBytes, "PTLAS scratch");

		std::vector<rhi::PackedDxr2PartitionedTlasWriteInstanceData> writes(2);
		for (uint32_t i = 0; i < 2; ++i) {
			Translate(writes[i].transform, i == 0 ? 0.0f : 10.0f);
			writes[i].instanceID = 7 + i;
			writes[i].instanceIndex = i;
			writes[i].partitionIndex = 0;
			writes[i].accelerationStructure = blas;
		}
		rhi::Resource writeBuffer = MakeUpload(c, writes, "PTLAS writes");
		const std::vector<rhi::PackedRayTracingPartitionedTlasIndirectCommand> commands = { { rhi::RayTracingPartitionedTlasOperationType::WriteInstance, 2, Va(c, writeBuffer), sizeof(writes[0]) } };
		rhi::Resource commandBuffer = MakeUpload(c, commands, "PTLAS commands");
		const std::vector<uint32_t> commandCount = { 1 };
		rhi::Resource countBuffer = MakeUpload(c, commandCount, "PTLAS command count");

		rhi::RayTracingRtasOperationDesc desc{};
		desc.inputs = inputs;
		desc.partitionedTopLevel.input = partitionedInputs;
		desc.partitionedTopLevel.dstAccelerationStructureData = { storage.GetHandle(), 0 };
		desc.partitionedTopLevel.scratchData = { scratch.GetHandle(), 0 };
		desc.partitionedTopLevel.srcInfos = { commandBuffer.GetHandle(), 0 };
		desc.partitionedTopLevel.srcInfosCount = { countBuffer.GetHandle(), 0 };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&desc, 1); })) return 1;

		rhi::SrvDesc scene{};
		scene.dimension = rhi::SrvDim::AccelerationStruct;
		scene.accel.deviceAddress = Va(c, storage);
		scene.accel.sizeBytes = prebuild.resultDataMaxSizeInBytes;
		scene.accel.partitioned = true;
		ok &= ExpectStandardScene("partitioned TLAS, ray query", TraceWith(c, scene, true));
		ok &= ExpectStandardScene("partitioned TLAS, TraceRay", TraceWith(c, scene, false));

		// Translate the partition by +20: the instance at the origin lands on the template-ray's x, in place.
		const std::vector<rhi::PackedRayTracingPartitionedTlasWritePartitionTranslationData> translations = { { 0u, { 20.0f, 0.0f, 0.0f } } };
		rhi::Resource translationBuffer = MakeUpload(c, translations, "PTLAS translation");
		const std::vector<rhi::PackedRayTracingPartitionedTlasIndirectCommand> updateCommands = { { rhi::RayTracingPartitionedTlasOperationType::WritePartitionTranslation, 1, Va(c, translationBuffer), sizeof(translations[0]) } };
		rhi::Resource updateBuffer = MakeUpload(c, updateCommands, "PTLAS update commands");
		desc.partitionedTopLevel.srcAccelerationStructureData = { storage.GetHandle(), 0 };
		desc.partitionedTopLevel.srcInfos = { updateBuffer.GetHandle(), 0 };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&desc, 1); })) return 1;
		const auto translated = TraceWith(c, scene, true);
		if (translated.size() != kRayCount) return 1;
		ok &= Expect("translated partition", 0, translated[0], 0, 0, 0, 0, 0, 0);
		ok &= Expect("translated partition", 5, translated[5], 1, 7, kClusterAId, 0, 0.50f, 0.25f);
	}

	// --- Templates: topology built once, positions supplied at instantiation. M8 animates with this.
	{
		const std::vector<rhi::PackedDxr2ClusterBuildTriangleTemplateInfo> templateArgs = { { clusterArgs[0], 0, 0, 0 } };
		rhi::Resource templateArgsBuffer = MakeUpload(c, templateArgs, "template args");
		rhi::RayTracingRtasOperationInputs inputs{};
		inputs.operationType = rhi::RayTracingRtasOperationType::BuildTriangleClusterTemplate;
		inputs.flags = rhi::RTASBuild_PreferFastTrace;
		inputs.maxAccelerationStructureCount = 1;
		inputs.triangleClusters = &triangleInputs;
		rhi::RayTracingRtasPrebuildInfo prebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(inputs, prebuild), "template prebuild")) return 1;
		rhi::Resource result = MakeAsStorage(c, prebuild.resultDataMaxSizeInBytes, "template data");
		rhi::Resource scratch = MakeUav(c, prebuild.scratchDataSizeInBytes, "template scratch");
		rhi::Resource templateAddress = MakeUav(c, 8, "template address");
		rhi::Resource templateSize = MakeUav(c, 8, "template size");
		rhi::RayTracingRtasOperationDesc desc{};
		desc.inputs = inputs;
		desc.batched.dstImplicitData = { result.GetHandle(), 0 };
		desc.batched.scratchData = { scratch.GetHandle(), 0 };
		desc.batched.dstAddressesArray = { templateAddress.GetHandle(), 0, 8, 8 };
		desc.batched.dstSizesArray = { templateSize.GetHandle(), 0, 8, 8 };
		desc.batched.srcInfosArray = { templateArgsBuffer.GetHandle(), 0, sizeof(templateArgs[0]), sizeof(templateArgs[0]) };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&desc, 1); })) return 1;
		const auto builtTemplate = Readback<uint64_t>(c, templateAddress, 1);
		if (builtTemplate.empty() || builtTemplate[0] == 0) {
			std::fputs("template build returned no address\n", stderr);
			return 1;
		}

		std::vector<Vec3> shifted(kPositions.begin(), kPositions.begin() + 4);
		for (Vec3& position : shifted) position.x += 20.0f;
		rhi::Resource shiftedBuffer = MakeUpload(c, shifted, "instantiated positions");
		rhi::PackedDxr2ClusterInstantiateTemplateInfo instantiateArgs{};
		instantiateArgs.clusterIdOffset = 5;
		instantiateArgs.clusterTemplateAddress = builtTemplate[0];
		instantiateArgs.vertexBufferStartAddress = Va(c, shiftedBuffer);
		instantiateArgs.vertexBufferStrideInBytes = sizeof(Vec3);
		const std::vector<rhi::PackedDxr2ClusterInstantiateTemplateInfo> instantiateList = { instantiateArgs };
		rhi::Resource instantiateBuffer = MakeUpload(c, instantiateList, "instantiate args");
		rhi::RayTracingRtasOperationInputs instantiateInputs = inputs;
		instantiateInputs.operationType = rhi::RayTracingRtasOperationType::InstantiateTriangleCluster;
		rhi::RayTracingRtasPrebuildInfo instantiatePrebuild{};
		if (!Check(c.device->GetRayTracingAccelerationStructureOperationPrebuildInfo(instantiateInputs, instantiatePrebuild), "instantiate prebuild")) return 1;
		rhi::Resource instantiated = MakeAsStorage(c, instantiatePrebuild.resultDataMaxSizeInBytes, "instantiated CLAS");
		rhi::Resource instantiateScratch = MakeUav(c, instantiatePrebuild.scratchDataSizeInBytes, "instantiate scratch");
		rhi::Resource instantiatedAddress = MakeUav(c, 8, "instantiated address");
		rhi::Resource instantiatedSize = MakeUav(c, 8, "instantiated size");
		rhi::RayTracingRtasOperationDesc instantiate{};
		instantiate.inputs = instantiateInputs;
		instantiate.batched.dstImplicitData = { instantiated.GetHandle(), 0 };
		instantiate.batched.scratchData = { instantiateScratch.GetHandle(), 0 };
		instantiate.batched.dstAddressesArray = { instantiatedAddress.GetHandle(), 0, 8, 8 };
		instantiate.batched.dstSizesArray = { instantiatedSize.GetHandle(), 0, 8, 8 };
		instantiate.batched.srcInfosArray = { instantiateBuffer.GetHandle(), 0, sizeof(instantiateArgs), sizeof(instantiateArgs) };
		if (!Submit(c, [&](rhi::CommandList& list) { list.ExecuteIndirectRtasOperations(&instantiate, 1); })) return 1;

		const uint64_t templateBlas = BuildBlas(c, Va(c, instantiatedAddress), 1);
		const rhi::AccelerationStructureHandle templateTlas = BuildClassicTlas(c, { { 9u, 0.0f } }, templateBlas);
		if (templateBlas == 0 || !templateTlas.valid()) return 1;
		const auto hits = TraceHandle(c, templateTlas, true);
		if (hits.size() != kRayCount) return 1;
		ok &= Expect("template", 5, hits[5], 1, 9, kClusterAId + 5, 0, 0.50f, 0.25f);
		ok &= Expect("template", 0, hits[0], 0, 0, 0, 0, 0, 0);
	}

	if (c.caps.opacityMicromap) {
		ok &= RunOpacityMicromapTests(c);
	}
	else {
		std::puts("opacity micromaps: skipped, RayTracingFeatureInfo::opacityMicromap is false (raytracing tier below 1.2)");
	}

	if (debugLayer && HasDebugLayerErrors(nativeDevice, firstMessage)) ok = false;
	if (!ok) {
		std::puts("BasicRHID3D12RaytracingSmoke: FAILED");
		return 1;
	}
	std::puts("BasicRHID3D12RaytracingSmoke: success");
	return 0;
}
