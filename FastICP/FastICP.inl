#define THREAD_X 8
#define THREAD_Y 8
// Do not modify below this line
#define DATABUF_COUNT 7
#if __cplusplus
#define CBUFFER_ALIGN __declspec(align(16))
#else
#define CBUFFER_ALIGN
#endif

#if __hlsl
#define REGISTER(x) :register(x)
#define STRUCT(x) x
#else 
typedef DirectX::XMUINT2 uint2;
typedef DirectX::XMFLOAT2 float2;
typedef DirectX::XMMATRIX matrix;
#define REGISTER(x)
#define STRUCT(x) struct
#endif

#if __cplusplus || (__hlsl)
CBUFFER_ALIGN STRUCT(cbuffer) CBuffer REGISTER(b0)
{
    matrix mXform;
    float2 f2InvOrigReso;
    uint2 u2AlignedReso;
    float fNormalDiffThreshold;
#if __cplusplus
    void * operator new(size_t i) {
        return _aligned_malloc(i, 16);
    };
    void operator delete(void* p) {
        _aligned_free(p);
    };
#endif // __cplusplus
};
#endif // __cplusplus || (__hlsl && Pixel_Shader)

#if __hlsl
cbuffer forReduction : register(b1)
{
    uint uOffset;
}
#endif // __hlsl
#undef CBUFFER_ALIGN