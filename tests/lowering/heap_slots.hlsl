// HeapSlot ranges (binding b reads heap slot b, plus the array index) and static samplers.
Texture2D<float4> slots[4] : register(t10, space1);
RWTexture2D<float4> output : register(u0, space3);
RWByteAddressBuffer counters : register(u5, space3);
SamplerState linearSampler : register(s0, space2);
SamplerState pointSamplers[2] : register(s1, space2);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	const float2 uv = float2(id.xy) / 64.0;
	float4 c = slots[id.z & 3].SampleLevel(linearSampler, uv, 0);
	c += slots[1].SampleLevel(pointSamplers[id.x & 1], uv, 0);
	c += slots[2].SampleLevel(pointSamplers[1], uv, 0);
	output[id.xy] = c;
	counters.InterlockedAdd(0, 1);
}
