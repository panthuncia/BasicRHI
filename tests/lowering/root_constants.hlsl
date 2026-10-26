// Several RootConstants32 ranges, all pushed: scalars, vectors, an array of structs, a row_major matrix.
struct Light {
	float3 direction;
	float intensity;
};

cbuffer DrawConstants : register(b0) {
	uint drawId;
	uint materialId;
	float2 uvScale;
};
cbuffer Lights : register(b1) {
	Light lights[2];
	float4 ambient;
};
cbuffer Transform : register(b2) {
	row_major float4x4 world;
};

struct VSOut {
	float4 position : SV_Position;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
};

VSOut main(float3 position : POSITION, float3 normal : NORMAL, float2 uv : TEXCOORD0) {
	VSOut o;
	o.position = mul(float4(position, 1), world);
	const Light light = lights[drawId & 1];
	o.color = ambient + light.intensity * saturate(dot(normal, -light.direction)) + float(materialId);
	o.uv = uv * uvScale;
	return o;
}
