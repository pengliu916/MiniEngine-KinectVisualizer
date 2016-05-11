#include "KinectVisualizer_SharedHeader.inl"
#include "Kinect2CalibData.inl"

static const float2 df = KINECT2_DEPTH_F;
static const float2 dc = KINECT2_DEPTH_C;
static const float4 dk = KINECT2_DEPTH_K;
static const float2 dp = KINECT2_DEPTH_P;
static const float2 cf = KINECT2_COLOR_F;
static const float2 cc = KINECT2_COLOR_C;
static const float4 ck = KINECT2_COLOR_K;
static const float2 cp = KINECT2_COLOR_P;

#if QuadVS
void vsmain( in uint VertID : SV_VertexID, out float4 Pos : SV_Position, out float2 Tex : TexCoord0 )
{
	// Texture coordinates range [0, 2], but only [0, 1] appears on screen.
	Tex = float2(uint2(VertID, VertID << 1) & 2);
	Pos = float4(lerp( float2(-1, 1), float2(1, -1), Tex ), 0, 1);
}
#endif

#if CopyPS
Buffer<uint> DepBuffer : register(t0);
Buffer<uint> InfBuffer : register(t1);
Buffer<uint4> ColBuffer : register(t2);

struct PSOutput
{
	float4 DepVisaulOut : SV_Target0;
	float4 InfraredOut : SV_Target1;
	uint DepthOut : SV_Target2;
};

PSOutput ps_copy_depth_infrared_main( float4 position : SV_Position, float2 Tex : TexCoord0 )
{
	PSOutput output;
	uint2 idx = (uint2)(Tex*DepthInfraredReso);
	uint id = idx.y*DepthInfraredReso.x + idx.x;

#if CorrectDistortion
	float2 xy = (idx - dc) / df; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + dk.x*r2 + dk.y*r4 + dk.z*r6) + 2.f*dp*xy.x*xy.y + dp.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * df + dc + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * DepthInfraredReso.x + (uint)new_xy.x;
#endif // CorrectDistortion

	output.DepVisaulOut = (DepBuffer[id] % 255) / 255.f;
	output.InfraredOut = pow((InfBuffer[id]) / 65535.f,0.32f);
	output.DepthOut = DepBuffer[id];
	return output;
}

float4 ps_copy_color_main( float4 position : SV_Position, float2 Tex : TexCoord0 ) : SV_Target0
{
	uint2 idx = (uint2)( Tex * ColorReso );
	uint id = idx.y * ColorReso.x + idx.x;

#if CorrectDistortion
	float2 xy = (idx - cc) / cf; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + ck.x*r2 + ck.y*r4 + ck.z*r6) + 2.f*cp*xy.x*xy.y + cp.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * cf + cc + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * ColorReso.x + (uint)new_xy.x;
#endif // CorrectDistortion

	return ColBuffer[id] / 255.f;
}
#endif

#if CopyCS
Buffer<uint> DepBuffer : register(t0);
Buffer<uint> InfBuffer : register(t1);
Buffer<uint4> ColBuffer : register(t2);
RWTexture2D<float4> DepthVisualTex : register(u0);
RWTexture2D<float4> InfraredTex : register(u1);
RWTexture2D<float4> ColorTex : register(u2);
RWTexture2D<uint> DepthTex : register(u3);

[numthreads( THREAD_PER_GROUP, 1, 1 )]
void cs_copy_depth_infrared_main( uint3 Gid : SV_GroupID, uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID, uint3 DTid : SV_DispatchThreadID )
{
	uint id = Gid.x * THREAD_PER_GROUP + GI;
	uint2 out_xy = uint2(id%DepthInfraredReso.x, id / DepthInfraredReso.x);

#if CorrectDistortion
	float2 xy = (out_xy - dc) / df; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + dk.x*r2 + dk.y*r4 + dk.z*r6) + 2.f*dp*xy.x*xy.y + dp.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * df + dc + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * DepthInfraredReso.x + (uint)new_xy.x;
#endif // CorrectDistortion

	DepthVisualTex[out_xy] = (DepBuffer[id].xxxx % 255) / 255.f;
	InfraredTex[out_xy] = pow(InfBuffer[id].xxxx / 65535.f,0.32f);
	DepthTex[out_xy] = DepBuffer[id];
}
[numthreads( THREAD_PER_GROUP, 1, 1 )]
void cs_copy_color_main( uint3 Gid : SV_GroupID, uint GI : SV_GroupIndex, uint3 GTid : SV_GroupThreadID, uint3 DTid : SV_DispatchThreadID )
{
	uint id = Gid.x * THREAD_PER_GROUP + GI;
	uint2 out_xy = uint2(id%ColorReso.x, id / ColorReso.x);

#if CorrectDistortion
	float2 xy = (out_xy - cc) / cf; // For opencv r is distance from 0,0 to X/Z,Y/Z 
	float r2 = dot( xy, xy );
	float r4 = r2 * r2;
	float r6 = r2 * r4;
	// For radial and tangential distortion correction
	float2 new_xy = xy*(1.f + ck.x*r2 + ck.y*r4 + ck.z*r6) + 2.f*cp*xy.x*xy.y + cp.yx*(r2 + 2.f*xy*xy);
	new_xy = new_xy * cf + cc + 0.5f; // Add 0.5f for correcting float to int truncation
	id = (uint)new_xy.y * ColorReso.x + (uint)new_xy.x;
#endif // CorrectDistortion

	ColorTex[out_xy] = ColBuffer[id] / 255.f;
}
#endif // CopyCS