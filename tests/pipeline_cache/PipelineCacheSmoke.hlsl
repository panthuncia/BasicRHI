// Shaders of BasicRHIPipelineCacheSmoke: compute permutations (THREADS) and a full-target triangle.

#ifndef THREADS
#define THREADS 1
#endif

[numthreads(THREADS, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {}

float4 VSMain(uint id : SV_VertexID) : SV_Position {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), 0.5, 1);
}

float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
