#if TEX3D_UAV
#define BUFFER_INDEX(idx) idx
#define BufIdx uint3
#else
#define BUFFER_INDEX(idx) FlatIdx(idx, vParam.u3VoxelReso)
#define BufIdx uint
#endif

uint FlatIdx(uint3 idx, uint3 reso)
{
    return idx.x + idx.y * reso.x + idx.z * reso.x * reso.y;
}

uint3 MakeU3Idx(uint idx, uint3 res)
{
    uint stripCount = res.x * res.y;
    uint stripRemainder = idx % stripCount;
    uint z = idx / stripCount;
    uint y = stripRemainder / res.x;
    uint x = stripRemainder % res.x;
    return uint3(x, y, z);
}

uint PackedToUint(uint3 xyz)
{
    return(xyz.z | ((xyz.y) << 8) | ((xyz.x) << 16));
    //return(xyz.z | ((xyz.y) << 10) | ((xyz.x) << 20));
}

uint3 UnpackedToUint3(uint x)
{
    uint3 xyz;
    //xyz.z = x & 0x000003ff;
    //xyz.y = (x >> 10) & 0x000003ff;
    //xyz.x = (x >> 20) & 0x000003ff;

    xyz.z = x & 0x000000ff;
    xyz.y = (x >> 8) & 0x000000ff;
    xyz.x = (x >> 16) & 0x000000ff;
    return xyz;
}