// A DCLF-style engine pixel shader. D3D11 registers, compiled with -fvk-b-shift 0 0 -fvk-t-shift 200 0 -fvk-s-shift 400 0
// -fvk-u-shift 500 0. The per-draw (b190, 96 B, pushed) and per-pass (b191, 128 B, spilled) push cbuffers; constant buffers by
// IndirectAddress (from the draw's pipeline row, and from the frame record whose address is a per-pass word: spilled) and by
// PushAddress; textures and samplers by IndirectIndex from the material row.
cbuffer PerDraw : register(b190) {
	uint4 drawWords[6];
};
cbuffer PerPass : register(b191) {
	float4 passWords[8];
};
cbuffer TechniqueConstants : register(b0) {
	row_major float4x4 World;
	float4 Tint;
};
cbuffer Geometry : register(b2) {
	float4 Arr[8];
	float Scale;
};
cbuffer Lighting : register(b4) {
	float4 LightColor;
	float3 LightDirection;
};

Texture2D<float4> Tex0 : register(t0);
Texture2D<float4> Tex3 : register(t3);
SamplerState Samp0 : register(s0);
SamplerState Samp3 : register(s3);

struct PSIn {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	nointerpolation uint index : TEXCOORD1;
};

float4 main(PSIn i) : SV_Target {
	float4 c = Tex0.Sample(Samp0, i.uv) * Tint + Tex3.Sample(Samp3, i.uv) * Arr[i.index & 7] * Scale;
	c = mul(World, c) + passWords[i.index & 7] + float4(drawWords[5]);
	return c * LightColor + float4(LightDirection, 0);
}
