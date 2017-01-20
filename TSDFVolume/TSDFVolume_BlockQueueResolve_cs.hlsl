#include "TSDFVolume.inl"

ByteAddressBuffer buf_srvUploadBlocksBufCtr : register(t0);
RWByteAddressBuffer buf_uavIndirectParams : register(u0);

//------------------------------------------------------------------------------
// Compute Shader
//------------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void main(uint uGIdx : SV_GroupIndex)
{
    if (uGIdx == 0) {
        uint uNumThreadGroupX = buf_srvUploadBlocksBufCtr.Load(0);
        buf_uavIndirectParams.Store(12, uNumThreadGroupX);
    }
}