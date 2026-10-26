// Two compute entry points in one library module: the lowering keeps the one it is asked for.
RWStructuredBuffer<uint> first_output : register(u0);
RWStructuredBuffer<uint> second_output : register(u1);

[shader("compute")]
[numthreads(1, 1, 1)]
void first(uint3 id : SV_DispatchThreadID) {
	first_output[id.x] = 1;
}

[shader("compute")]
[numthreads(2, 1, 1)]
void second(uint3 id : SV_DispatchThreadID) {
	second_output[id.x] = 2;
}
