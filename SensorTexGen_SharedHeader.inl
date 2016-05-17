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
typedef DirectX::XMFLOAT4 float4;
typedef DirectX::XMMATRIX matrix;
#define REGISTER(x)
#define STRUCT(x) struct
#endif

#include "CalibData.inl"

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