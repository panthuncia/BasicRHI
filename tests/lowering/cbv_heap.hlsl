// Heap-indexed constant buffers, for cbvAsStorageBuffer: a static ConstantBuffer read from ResourceDescriptorHeap, ORG's
// ConstantBuffer array at the resource heap's binding indexed by a push value, and register-bound cbuffers that a heap slot
// (element 4) and an indirect index map. Push data (Root) stays a push constant.
struct Frame {
	row_major float4x4 viewProjection;
	float4 tint;
	uint count;
};
struct Lens {
	float4 weights[8];
	column_major float3x4 skew;
	uint flags;
};
struct Indirect {
	float4 scale;
};

cbuffer Root : register(b0) {
	uint frameHandle;
	uint lensHandle;
	uint outputHandle;
	uint pick;
	uint2 record;  // the indirect index record's address
};
cbuffer Material : register(b1, space1) {
	float4 palette[4];
	row_major float2x4 rotation;
};
ConstantBuffer<Indirect> indirect : register(b0, space2);

[[vk::binding(1000000, 0)]] ConstantBuffer<Lens> OrgConstantBuffers_lens[];
static ConstantBuffer<Frame> frame = ResourceDescriptorHeap[frameHandle];

[numthreads(8, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	float4 v = mul(frame.viewProjection, frame.tint) * frame.count;
	v += OrgConstantBuffers_lens[lensHandle].weights[pick & 7] * OrgConstantBuffers_lens[lensHandle].flags;
	v.xyz += mul(OrgConstantBuffers_lens[lensHandle].skew, v);
	v += palette[id.x & 3] + float4(mul(rotation, v), 0, 0) + indirect.scale;
	RWByteAddressBuffer output = ResourceDescriptorHeap[outputHandle];
	output.Store4(id.x * 16, asuint(v));
}
