// Shaders of BasicRHIVulkanFixedFunctionSmoke: a full-target triangle and a white pixel.
// VSFar puts the triangle behind the far plane (z = 1.5), so it only shows with depth clip off.

float4 FullTarget(uint id, float z) {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2, -2) + float2(-1, 1), z, 1);
}

float4 VSMain(uint id : SV_VertexID) : SV_Position { return FullTarget(id, 0.5); }
float4 VSFar(uint id : SV_VertexID) : SV_Position { return FullTarget(id, 1.5); }
float4 PSMain() : SV_Target { return float4(1, 1, 1, 1); }
