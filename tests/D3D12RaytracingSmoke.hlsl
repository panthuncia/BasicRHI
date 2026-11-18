// Traces a fixed set of rays and writes what each one hit, so the test can compare against hand-computed answers.
// Built three times by CMake: as cs_6_10 for the ray query paths (CSMain, CSOmm) and as lib_6_10 for the pipeline path.

struct Constants {
	uint sceneIndex;
	uint resultsIndex;
	// Opacity micromap rays only.
	uint raysIndex;
	uint rayFlags;
};
ConstantBuffer<Constants> constants : register(b0);

struct RayResult {
	uint hit;
	uint instanceId;
	uint clusterId;
	uint primitiveIndex;
	float baryU;
	float baryV;
	float t;
	uint reserved;
};

static const uint kRayCount = 6;
// All rays start at z = -1 and travel along +z, so a hit on the z = 0 geometry has t = 1.
static const float2 kRayOrigins[kRayCount] = {
	float2(0.75f, 0.25f),  // quad, lower triangle
	float2(0.25f, 0.75f),  // quad, upper triangle
	float2(2.50f, 0.25f),  // second cluster
	float2(10.75f, 0.25f), // quad in the translated instance
	float2(5.00f, 5.00f),  // nothing
	float2(20.75f, 0.25f), // quad at the positions supplied to template instantiation
};

RayDesc MakeRay(uint index) {
	RayDesc ray;
	ray.Origin = float3(kRayOrigins[index], -1.0f);
	ray.Direction = float3(0.0f, 0.0f, 1.0f);
	ray.TMin = 0.0f;
	ray.TMax = 100.0f;
	return ray;
}

// Opacity micromap rays: one per origin in a buffer, along +z from z = -1. reserved counts any-hit invocations
// (or non-opaque candidates, for ray query), which the any-hit accepts.
RayDesc MakeOmmRay(float3 origin) {
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = float3(0.0f, 0.0f, 1.0f);
	ray.TMin = 0.0f;
	ray.TMax = 100.0f;
	return ray;
}

#if defined(RAY_QUERY_PATH)

[numthreads(64, 1, 1)]
void CSOmm(uint3 thread : SV_DispatchThreadID) {
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[constants.sceneIndex];
	RWStructuredBuffer<RayResult> results = ResourceDescriptorHeap[constants.resultsIndex];
	StructuredBuffer<float4> origins = ResourceDescriptorHeap[constants.raysIndex];
	uint count = 0u;
	uint stride = 0u;
	origins.GetDimensions(count, stride);
	if (thread.x >= count) return;
	// RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS | RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY (2: no name in the preview DXC).
	RayQuery<RAY_FLAG_NONE, RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS | 2> query;
	query.TraceRayInline(scene, constants.rayFlags, 0xFF, MakeOmmRay(origins[thread.x].xyz));
	uint candidates = 0u;
	while (query.Proceed()) {
		if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) {
			++candidates;
			query.CommitNonOpaqueTriangleHit();
		}
	}
	RayResult result = (RayResult)0;
	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		result.hit = 1;
		result.instanceId = query.CommittedInstanceID();
		result.clusterId = query.CommittedClusterID();
		result.primitiveIndex = query.CommittedPrimitiveIndex();
		result.baryU = query.CommittedTriangleBarycentrics().x;
		result.baryV = query.CommittedTriangleBarycentrics().y;
		result.t = query.CommittedRayT();
	}
	result.reserved = candidates;
	results[thread.x] = result;
}

[numthreads(1, 1, 1)]
void CSMain() {
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[constants.sceneIndex];
	RWStructuredBuffer<RayResult> results = ResourceDescriptorHeap[constants.resultsIndex];
	for (uint i = 0; i < kRayCount; ++i) {
		// RAYQUERY_FLAG_ALLOW_CLUSTERED_GEOMETRY: the current preview DXC has the behaviour but not the name.
		RayQuery<RAY_FLAG_NONE, 2> query;
		query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, MakeRay(i));
		query.Proceed();
		RayResult result = (RayResult)0;
		if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
			result.hit = 1;
			result.instanceId = query.CommittedInstanceID();
			result.clusterId = query.CommittedClusterID();
			result.primitiveIndex = query.CommittedPrimitiveIndex();
			result.baryU = query.CommittedTriangleBarycentrics().x;
			result.baryV = query.CommittedTriangleBarycentrics().y;
			result.t = query.CommittedRayT();
		}
		results[i] = result;
	}
}

#else

struct [raypayload] Payload {
	RayResult result : read(caller) : write(caller, miss, closesthit);
	uint anyHitRan : read(closesthit) : write(caller, anyhit);
};

[shader("raygeneration")]
void RayGen() {
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[constants.sceneIndex];
	RWStructuredBuffer<RayResult> results = ResourceDescriptorHeap[constants.resultsIndex];
	const uint index = DispatchRaysIndex().x;
	Payload payload = (Payload)0;
	TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, MakeRay(index), payload);
	results[index] = payload.result;
}

[shader("miss")]
void Miss(inout Payload payload) {
	payload.result = (RayResult)0;
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes) {
	payload.result.hit = 1;
	payload.result.instanceId = InstanceID();
	payload.result.clusterId = ClusterID();
	payload.result.primitiveIndex = PrimitiveIndex();
	payload.result.baryU = attributes.barycentrics.x;
	payload.result.baryV = attributes.barycentrics.y;
	payload.result.t = RayTCurrent();

	// What the path tracer's hit shaders rely on: fetching the triangle from the BVH, and reaching the descriptor heap.
	const BuiltInTrianglePositions positions = TriangleObjectPositions();
	RWStructuredBuffer<RayResult> results = ResourceDescriptorHeap[constants.resultsIndex];
	uint elements = 0u;
	uint stride = 0u;
	results.GetDimensions(elements, stride);
	const bool fetched = positions.p0.z == 0.0f && positions.p1.z == 0.0f && positions.p2.z == 0.0f && any(positions.p0 != positions.p1);
	payload.result.reserved = (fetched ? 1u : 0u) | (elements == kRayCount ? 2u : 0u) | (payload.anyHitRan != 0u ? 4u : 0u);
}

[shader("anyhit")]
void AnyHit(inout Payload payload, BuiltInTriangleIntersectionAttributes attributes) {
	RWStructuredBuffer<RayResult> results = ResourceDescriptorHeap[constants.resultsIndex];
	uint elements = 0u;
	uint stride = 0u;
	results.GetDimensions(elements, stride);
	// A cluster ID the scene doesn't use, so the test sees this read ClusterID without rejecting anything.
	if (ClusterID() == 0xFFFFFFFFu || elements != kRayCount) {
		IgnoreHit();
	}
	payload.anyHitRan = 1u;
}

// Shader execution reordering over clusters. The rays are forced non-opaque so the any-hit above runs during traversal.
[shader("raygeneration")]
void RayGenReordered() {
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[constants.sceneIndex];
	RWStructuredBuffer<RayResult> results = ResourceDescriptorHeap[constants.resultsIndex];
	const uint index = DispatchRaysIndex().x;
	Payload payload = (Payload)0;
	dx::HitObject hit = dx::HitObject::TraceRay(scene, RAY_FLAG_FORCE_NON_OPAQUE, 0xFF, 1, 1, 0, MakeRay(index), payload);
	dx::MaybeReorderThread(hit, index & 3u, 2u);
	dx::HitObject::Invoke(hit, payload);
	results[index] = payload.result;
}

struct [raypayload] OmmPayload {
	RayResult result : read(caller) : write(caller, miss, closesthit);
	uint anyHits : read(caller, anyhit) : write(caller, anyhit);
};

[shader("raygeneration")]
void RayGenOmm() {
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[constants.sceneIndex];
	RWStructuredBuffer<RayResult> results = ResourceDescriptorHeap[constants.resultsIndex];
	StructuredBuffer<float4> origins = ResourceDescriptorHeap[constants.raysIndex];
	const uint index = DispatchRaysIndex().x;
	OmmPayload payload = (OmmPayload)0;
	TraceRay(scene, constants.rayFlags, 0xFF, 0, 1, 0, MakeOmmRay(origins[index].xyz), payload);
	RayResult result = payload.result;
	result.reserved = payload.anyHits;
	results[index] = result;
}

[shader("miss")]
void MissOmm(inout OmmPayload payload) {
	payload.result = (RayResult)0;
}

[shader("closesthit")]
void ClosestHitOmm(inout OmmPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
	payload.result.hit = 1;
	payload.result.instanceId = InstanceID();
	payload.result.clusterId = ClusterID();
	payload.result.primitiveIndex = PrimitiveIndex();
	payload.result.baryU = attributes.barycentrics.x;
	payload.result.baryV = attributes.barycentrics.y;
	payload.result.t = RayTCurrent();
}

// Records the invocation and accepts the hit, so an unknown micro-triangle reads as a hit with anyHits > 0.
[shader("anyhit")]
void AnyHitOmm(inout OmmPayload payload, BuiltInTriangleIntersectionAttributes attributes) {
	payload.anyHits += 1u;
}

#endif
