// IndirectIndex textures and samplers, IndirectAddress and PushAddress cbuffers (arrays, row_major matrices). The record and push
// addresses are members of the declared root-constant cbuffer.
cbuffer Root : register(b0) {
	uint2 record;         // the record address
	uint2 objectAddress;  // the PushAddress cbuffer's address
	uint flags;
};
cbuffer Camera : register(b1) {
	row_major float4x4 viewProjection;
	float4 frustum[6];
};
cbuffer Palette : register(b2) {
	float4 colors[16];
	column_major float2x3 skew;
};
cbuffer Object : register(b3) {
	row_major float4x4 world;
	uint objectFlags;
};

Texture2D<float4> albedo : register(t0, space1);
Texture2D<float4> normalMap : register(t1, space1);
Texture2D<float> mask : register(t2, space1);
SamplerState linearSampler : register(s0, space2);
SamplerState pointSampler : register(s1, space2);

struct PSIn {
	float4 position : SV_Position;
	float2 uv : TEXCOORD0;
	nointerpolation uint index : TEXCOORD1;
};

float4 main(PSIn i) : SV_Target {
	float4 c = albedo.Sample(linearSampler, i.uv) * colors[i.index & 15];
	c += normalMap.Sample(pointSampler, i.uv) * mask.Sample(pointSampler, i.uv);
	c = mul(c, viewProjection) + frustum[i.index % 6] + mul(world, c);
	c.xy += mul(skew, c.xyz);
	return (flags & objectFlags) != 0 ? c : -c;
}
