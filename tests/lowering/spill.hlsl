// Push data over 128 B: ranges b0 (64 B, pushed, declared; holds the texture record address), b1 (48 B, pushed, not declared;
// holds the Object address) and b2 (64 B, spilled, declared; holds the Material record address).
cbuffer PassConstants : register(b0) {
	uint2 textureRecord;   // the IndirectIndex record address: push data offset 0
	uint2 padding;
	float4 params[3];
};
cbuffer FrameConstants : register(b2) {
	float4 frame[3];
	uint2 materialRecord;  // the IndirectAddress record address: push data offset 112 + 48 = 160, spilled
	uint2 padding2;
};
cbuffer Material : register(b3) {
	float4 baseColor;
	float4 tints[4];
};
cbuffer Object : register(b4) {
	row_major float3x4 objectToWorld;
	float objectScale;
};

Texture2D<float4> textures[2] : register(t0, space1);
SamplerState samplers[1] : register(s0, space2);
RWStructuredBuffer<float4> output : register(u0, space3);

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID) {
	const float2 uv = float2(id.xy) * params[0].xy;
	float4 c = textures[0].SampleLevel(samplers[0], uv, 0) + textures[1].SampleLevel(samplers[0], uv, 0);
	c *= baseColor * tints[id.x & 3] * frame[id.y % 3];
	c.xyz += mul(objectToWorld, float4(c.xyz, 1)) * objectScale;
	output[id.x] = c + float4(materialRecord, textureRecord);
}
