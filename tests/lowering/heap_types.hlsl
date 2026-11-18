// Every resource type through ResourceDescriptorHeap / SamplerDescriptorHeap (the shader ABI's heap bindings).
struct Params {
	float4 color;
	uint idx[4];
};
struct Item {
	float4 v;
	uint w;
};

[numthreads(8, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	Texture2D<float4> tex = ResourceDescriptorHeap[id.x];
	RWTexture2D<float4> output = ResourceDescriptorHeap[id.x + 1];
	Buffer<float4> texels = ResourceDescriptorHeap[id.x + 2];
	RWBuffer<float4> rwTexels = ResourceDescriptorHeap[id.x + 3];
	ByteAddressBuffer raw = ResourceDescriptorHeap[id.x + 4];
	RWStructuredBuffer<Item> items = ResourceDescriptorHeap[id.x + 5];
	ConstantBuffer<Params> params = ResourceDescriptorHeap[id.x + 6];
	RWByteAddressBuffer result = ResourceDescriptorHeap[7];
	RaytracingAccelerationStructure scene = ResourceDescriptorHeap[id.x + 8];
	SamplerState samp = SamplerDescriptorHeap[id.y];

	const uint count = items.IncrementCounter();
	float4 r = tex.SampleLevel(samp, float2(0.5, 0.5), 0) + texels[id.x] + rwTexels[id.x] + params.color + items[id.x].v;
	r += float4(raw.Load(id.x * 4), count, params.idx[1], 0);
	output[id.xy] = r;

	RayQuery<RAY_FLAG_NONE> query;
	RayDesc ray;
	ray.Origin = 0;
	ray.Direction = float3(0, 0, 1);
	ray.TMin = 0;
	ray.TMax = 1;
	query.TraceRayInline(scene, 0, 0xFF, ray);
	query.Proceed();
	result.Store(0, asuint(r.x) + query.CommittedStatus());
}
