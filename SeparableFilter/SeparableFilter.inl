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
typedef DirectX::XMUINT2 uint2;
typedef uint32_t uint;
#define REGISTER(x)
#define STRUCT(x) struct
#endif

#if __cplusplus || (__hlsl)
CBUFFER_ALIGN STRUCT(cbuffer) CBuffer REGISTER(b0)
{
    uint2 u2Reso;
    float fRangeVar; // Let edge distance threshold as 2*deviation
    float fEdgeThreshold; // Trigger edge removal
    int iKernelRadius;
    int iUIN;
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
#undef CBUFFER_ALIGN