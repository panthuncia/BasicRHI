// Negative case: a combined image sampler cannot be lowered.
[[vk::combinedImageSampler]] Texture2D<float4> tex : register(t0);
[[vk::combinedImageSampler]] SamplerState samp : register(s0);

float4 main(float2 uv : TEXCOORD0) : SV_Target {
	return tex.Sample(samp, uv);
}
