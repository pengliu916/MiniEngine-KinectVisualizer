#pragma once
#include "TSDFVolume.inl"

class ManagedBuf
{
public:
    enum Type {
        kTypedBuffer = 0,
        k3DTexBuffer,
        kNumType
    };

    enum Bit {
        k8Bit = 0,
        k16Bit,
        k32Bit,
        kNumBitType
    };

    struct BufInterface {
        Type type;
        GpuResource* resource[2];
        D3D12_CPU_DESCRIPTOR_HANDLE SRV[2];
        D3D12_CPU_DESCRIPTOR_HANDLE UAV[2];
    };

    ManagedBuf(DirectX::XMUINT3 reso,
        Type defaultType = k3DTexBuffer, Bit defualtBit = k16Bit);
    ~ManagedBuf();
    inline const Type GetType() const { return _currentType; };
    inline const Bit GetBit() const { return _currentBit; };
    inline const DirectX::XMUINT3 GetReso() const { return _reso; };
    void CreateResource();
    void Destroy();
    bool ChangeResource(const DirectX::XMUINT3& reso, const Type bufType,
        const Bit bufBit);
    BufInterface GetResource();

private:
    void _CreateVolume(const DirectX::XMUINT3 reso,
        const Type bufType, const Bit bufBit, uint targetIdx);
    void _CookBuffer(const DirectX::XMUINT3 reso, const Type bufType,
        const Bit bufBit);

    // first index is ping-pong index second: 0 TSDF value, 1 weight
    VolumeTexture _volumeBuffer[2][2];
    TypedBuffer _typedBuffer[2][2];

    Type _currentType;
    Type _newType;
    Type _deprecatedType;

    Bit _currentBit;
    Bit _newBit;

    DirectX::XMUINT3 _reso;
    DirectX::XMUINT3 _newReso;
};