#pragma once

class SamplerDesc :public D3D12_SAMPLER_DESC
{
public:
    SamplerDesc();
    void SetTextureAddressMode(D3D12_TEXTURE_ADDRESS_MODE AddressMode);
    void SetBorderColor(DirectX::XMVECTOR BorderCol);

    // Allocate new descriptor as needed;
    // return handle to existing descriptor when possible
    D3D12_CPU_DESCRIPTOR_HANDLE CreateDescriptor(void);

    // Create descriptor in place (no deduplication)
    void CreateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& Handle);
};