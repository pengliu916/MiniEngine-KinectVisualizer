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
#define REGISTER(x)
#define STRUCT(x) struct
#endif

#if __cplusplus || (__hlsl)
CBUFFER_ALIGN STRUCT(cbuffer) CBuffer REGISTER(b1)
{
    float fAngleThreshold; // neighbor beyond that threshold is invalid
    float fDistThreshold; // neighbor beyond that threshold is invalid
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