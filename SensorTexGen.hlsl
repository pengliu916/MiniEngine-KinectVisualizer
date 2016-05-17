#include "SensorTexGen_SharedHeader.inl"
#include "CalibData.inl"

#if QuadVS
void vsmain( in uint VertID : SV_VertexID, out float4 Pos : SV_Position, out float2 Tex : TexCoord0 )
{
	// Texture coordinates range [0, 2], but only [0, 1] appears on screen.
	Tex = float2(uint2(VertID, VertID << 1) & 2);
	Pos = float4(lerp( float2(-1, 1), float2(1, -1), Tex ), 0, 1);
}
#endif // QuadVS

#if CopyPS
Buffer<uint> DepBuffer : register(t0);
Buffer<uint> InfBuffer : register(t1);
Buffer<uint4> ColBuffer : register(t2);

struct PSOutput
{
	uint DepthOut : SV_Target0;
#if VisualizedDepth
	float4 DepVisaulOut : SV_Target1;
#endif // VisualizedDepth
#if Infrared
	float4 InfraredOut : SV_Target2;
#endif // Infrared
};

PSOutput ps_copy_depth_infrared_main( float4 position : SV_Position, float2 Tex : TexCoord0 )
{
	PSOutput output;
	uint2 idx = (uint2)(Tex*DepthInfraredReso);
	uint id = idx.y*DepthInfraredReso.x + idx.x;

#if CorrectDistortion
	float2 xy = (idx - DEPTH_C) / DEPTH_F; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + DEPTH_K.x*r2 + DEPTH_K.y*r4 + DEPTH_K.z*r6) + 2.f*DEPTH_P*xy.x*xy.y + DEPTH_P.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * DEPTH_F + DEPTH_C + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * DepthInfraredReso.x + (uint)new_xy.x;
#endif // CorrectDistortion
#if VisualizedDepth
	output.DepVisaulOut = (DepBuffer[id] % 255) / 255.f;
#endif // VisualizedDepth
#if Infrared
	output.InfraredOut = pow((InfBuffer[id]) / 65535.f,0.32f);
#endif // Infrared
	output.DepthOut = DepBuffer[id];
	return output;
}

float4 ps_copy_color_main( float4 position : SV_Position, float2 Tex : TexCoord0 ) : SV_Target0
{
	uint2 idx = (uint2)( Tex * ColorReso );
	uint id = idx.y * ColorReso.x + idx.x;

#if CorrectDistortion
	float2 xy = (idx - COLOR_C) / COLOR_F; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + COLOR_K.x*r2 + COLOR_K.y*r4 + COLOR_K.z*r6) + 2.f*COLOR_P*xy.x*xy.y + COLOR_P.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * COLOR_F + COLOR_C + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * ColorReso.x + (uint)new_xy.x;
#endif // CorrectDistortion

	return ColBuffer[id] / 255.f;
}
#endif // CopyPS

#if CopyCS
Buffer<uint> DepBuffer : register(t0);
Buffer<uint> InfBuffer : register(t1);
Buffer<uint4> ColBuffer : register(t2);
RWTexture2D<uint> DepthTex : register(u0);
RWTexture2D<float4> DepthVisualTex : register(u1);
RWTexture2D<float4> InfraredTex : register(u2);
RWTexture2D<float4> ColorTex : register(u3);

[numthreads( THREAD_PER_GROUP, 1, 1 )]
void cs_copy_depth_infrared_main( uint3 Gid : SV_GroupID, uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID, uint3 DTid : SV_DispatchThreadID )
{
	uint id = Gid.x * THREAD_PER_GROUP + GI;
	uint2 out_xy = uint2(id%DepthInfraredReso.x, id / DepthInfraredReso.x);

#if CorrectDistortion
	float2 xy = (out_xy - DEPTH_C) / DEPTH_F; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + DEPTH_K.x*r2 + DEPTH_K.y*r4 + DEPTH_K.z*r6) + 2.f*DEPTH_P*xy.x*xy.y + DEPTH_P.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * DEPTH_F + DEPTH_C + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * DepthInfraredReso.x + (uint)new_xy.x;
#endif // CorrectDistortion
#if VisualizedDepth
	DepthVisualTex[out_xy] = (DepBuffer[id].xxxx % 255) / 255.f;
#endif // VisualizedDepth
#if Infrared
	InfraredTex[out_xy] = pow(InfBuffer[id].xxxx / 65535.f,0.32f);
#endif // Infrared
	DepthTex[out_xy] = DepBuffer[id];
}

[numthreads( THREAD_PER_GROUP, 1, 1 )]
void cs_copy_color_main( uint3 Gid : SV_GroupID, uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID, uint3 DTid : SV_DispatchThreadID )
{
	uint id = Gid.x * THREAD_PER_GROUP + GI;
	uint2 out_xy = uint2(id%ColorReso.x, id / ColorReso.x);

#if CorrectDistortion
	float2 xy = (out_xy - COLOR_C) / COLOR_F; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + COLOR_K.x*r2 + COLOR_K.y*r4 + COLOR_K.z*r6) + 2.f*COLOR_P*xy.x*xy.y + COLOR_P.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * COLOR_F + COLOR_C + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * ColorReso.x + (uint)new_xy.x;
#endif // CorrectDistortion

	ColorTex[out_xy] = ColBuffer[id] / 255.f;
}
#endif // CopyCS