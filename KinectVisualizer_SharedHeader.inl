#define THREAD_PER_GROUP 32

// Do not modify below this line
#if __cplusplus
#define CBUFFER_ALIGN __declspec(align(16))
#else
#define CBUFFER_ALIGN
#endif

#if __hlsl
#define REGISTER(x) :register(x)
#define STRUCT(x) x
#else 
typedef DirectX::XMFLOAT2 float2;
#define REGISTER(x)
#define STRUCT(x) struct
#endif

// Calibration data from "Mon May  9 00:06:42 2016"
#define KINECT2_DEPTH_K float4(8.1123228034613387e-02, -2.5401647716173742e-01, 8.5823592746789967e-02, 0)
#define KINECT2_DEPTH_P float2(-7.1939346256350565e-04, -3.9918163217997978e-04)
#define KINECT2_DEPTH_F float2(3.6154786682149080e+02, 3.6089181073659796e+02)
#define KINECT2_DEPTH_C float2(2.5835355135786824e+02, 2.0575487215776474e+02)

#define KINECT2_COLOR_K float4(2.9149065825195806e-02, -2.9654300241936486e-02, -5.7307859602833576e-03, 0)
#define KINECT2_COLOR_P float2(-8.0282793629378685e-04, -1.0993038057764051e-03)
#define KINECT2_COLOR_F float2(1.0500229488335692e+03,1.0497918256490759e+03)
#define KINECT2_COLOR_C float2(9.7136112531037020e+02, 5.4918051628416595e+02)

#if __cplusplus || ( __hlsl )
CBUFFER_ALIGN STRUCT( cbuffer ) RenderCB REGISTER( b0 )
{
	float2      ColorReso;
	float2      DepthInfraredReso;
#if __cplusplus
	void * operator new(size_t i)
	{
		return _aligned_malloc( i, 16 );
	};
	void operator delete(void* p)
	{
		_aligned_free( p );
	};
#endif // __cplusplus
};
#endif // __cplusplus || (__hlsl && Pixel_Shader)
#undef CBUFFER_ALIGN