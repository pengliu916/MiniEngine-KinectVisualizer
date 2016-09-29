Texture2D<float4> ColorMap : register(t1);

#if SHADED
float4 main(float2 Tex : TEXCOORD0, float3 Col : COLOR0) : SV_Target0
{
    return ColorMap[Tex] * float4(Col,1.f);
}
#else
float4 main(float2 Tex : TEXCOORD0) : SV_Target0
{
    return ColorMap[Tex];
}
#endif // SHADED