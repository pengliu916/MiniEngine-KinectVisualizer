#define CPU_THREAD_X 128
#define CPU_THREAD_Y 128
#define GPU_THREAD_X 16
#define GPU_THREAD_Y 64
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
#define REGISTER(x)
#define STRUCT(x) struct
#endif

#if __cplusplus || (__hlsl)
CBUFFER_ALIGN STRUCT(cbuffer) CBuffer REGISTER(b0)
{
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