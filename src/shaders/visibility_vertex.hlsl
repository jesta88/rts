cbuffer SceneMatrices : register(b0)
{
	float4x4 u_viewProj;
};

struct VertexInput
{
	float3 position   : POSITION0;
	float4x4 model    : INSTANCE_MATRIX; // Assumes instance matrix is bound across 4 slots
	uint objectID     : INSTANCE_ID;     // Per-instance ID
};

struct VertexOutput
{
	float4 position     : SV_Position;
	uint objectID       : TEXCOORD0;       // 'flat' interpolation is implicit for integers
	uint primitiveID    : SV_PrimitiveID;  // System-generated primitive ID
};

VertexOutput main(VertexInput input)
{
	VertexOutput output;
	output.position = mul(u_viewProj, mul(input.model, float4(input.position, 1.0)));
	output.objectID = input.objectID;
	output.primitiveID = input.primitiveID; // Pass through the system-generated ID
	return output;
}

