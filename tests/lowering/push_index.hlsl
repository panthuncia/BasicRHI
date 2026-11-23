// PushIndex constant buffers (LayoutRangeSource::PushIndex): the space 9 blocks are heap slots (frameSlot + slot offset + i), where
// frameSlot is a member of the root-constant cbuffer (or a spilled push data dword the shader does not declare).
cbuffer Root : register(b0) {
	uint2 record;
	uint frameSlot;
	uint flags;
};
cbuffer FrameA : register(b0, space9) {
	float4 tint[4];
};
cbuffer FrameB : register(b1, space9) {
	row_major float4x4 viewProjection;
};
cbuffer FrameC : register(b2, space9) {
	float4 fog;
	uint mode;
};

struct PSIn {
	float4 position : SV_Position;
	nointerpolation uint index : TEXCOORD0;
};

float4 main(PSIn i) : SV_Target {
	float4 c = mul(tint[i.index & 3], viewProjection);
	if ((mode & i.index) != 0) {
		c += fog;
	}
	return (flags & 1) != 0 ? c : -c;
}
