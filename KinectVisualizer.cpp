#include "pch.h"
#include "KinectVisualizer.h"
#include <ppl.h>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace
{
	bool _UseCSForCopy = false;
	bool _CorrectDistortion = false;
}

KinectVisualizer::KinectVisualizer( uint32_t width, uint32_t height, std::wstring name )
{
	m_width = width;
	m_height = height;
	m_pKinect2 = StreamFactory::createFromKinect2( true, true, true );
	m_pKinect2->StartStream();
}


KinectVisualizer::~KinectVisualizer()
{
}

void KinectVisualizer::OnConfiguration()
{
	Core::g_config.FXAA = false;
	Core::g_config.swapChainDesc.Width = m_width;
	Core::g_config.swapChainDesc.Height = m_height;
	Core::g_config.swapChainDesc.BufferCount = 5;
}

HRESULT KinectVisualizer::OnCreateResource()
{
	HRESULT hr;

	uint16_t Width, Height;
	do
	{
		m_pKinect2->GetDepthReso( Width, Height );
	} while (Width == 0 || Height == 0);

	m_KinectCB.DepthInfraredReso = XMFLOAT2( Width, Height );

	m_pFrameAlloc[IRGBDStreamer::kDepth] = new LinearFrameAllocator( Width * Height, sizeof( uint16_t ), DXGI_FORMAT_R16_UINT );
	m_pKinectBuffer[IRGBDStreamer::kDepth] = m_pFrameAlloc[IRGBDStreamer::kDepth]->RequestFrameBuffer();
	m_Texture[IRGBDStreamer::kDepth].Create( L"Depth Buffer", Width, Height, 1, DXGI_FORMAT_R11G11B10_FLOAT );

	m_pFrameAlloc[IRGBDStreamer::kInfrared] = new LinearFrameAllocator( Width * Height, sizeof( uint16_t ), DXGI_FORMAT_R16_UINT );
	m_pKinectBuffer[IRGBDStreamer::kInfrared] = m_pFrameAlloc[IRGBDStreamer::kInfrared]->RequestFrameBuffer();
	m_Texture[IRGBDStreamer::kInfrared].Create( L"Infrared Buffer", Width, Height, 1, DXGI_FORMAT_R11G11B10_FLOAT );

	m_DepthInfraredViewport = {};
	m_DepthInfraredViewport.Width = Width;
	m_DepthInfraredViewport.Height = Height;
	m_DepthInfraredViewport.MaxDepth = 1.0f;
	m_DepthInfraredScissorRect = {};
	m_DepthInfraredScissorRect.right = Width;
	m_DepthInfraredScissorRect.bottom = Height;

	do
	{
		m_pKinect2->GetColorReso( Width, Height );
	} while (Width == 0 || Height == 0);

	m_KinectCB.ColorReso = XMFLOAT2( Width, Height );

	m_pFrameAlloc[IRGBDStreamer::kColor] = new LinearFrameAllocator( Width * Height, sizeof( uint32_t ), DXGI_FORMAT_R8G8B8A8_UINT );
	m_pKinectBuffer[IRGBDStreamer::kColor] = m_pFrameAlloc[IRGBDStreamer::kColor]->RequestFrameBuffer();
	m_Texture[IRGBDStreamer::kColor].Create( L"Color Buffer", Width, Height, 1, DXGI_FORMAT_R11G11B10_FLOAT );

	m_ColorViewport = {};
	m_ColorViewport.Width = Width;
	m_ColorViewport.Height = Height;
	m_ColorViewport.MaxDepth = 1.0f;
	m_ColorScissorRect = {};
	m_ColorScissorRect.right = Width;
	m_ColorScissorRect.bottom = Height;

	// Create rootsignature
	m_RootSignature.Reset( 3 );
	m_RootSignature[0].InitAsConstantBuffer( 0 );
	m_RootSignature[1].InitAsDescriptorRange( D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, IRGBDStreamer::kNumBufferTypes );
	m_RootSignature[2].InitAsDescriptorRange( D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, IRGBDStreamer::kNumBufferTypes );
	m_RootSignature.Finalize( D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS );

	// Create PSO
	ComPtr<ID3DBlob> QuadVS;
	ComPtr<ID3DBlob> CopyDepthInfraredPS;
	ComPtr<ID3DBlob> CopyDepthInfraredUndistortedPS;
	ComPtr<ID3DBlob> CopyColorPS;
	ComPtr<ID3DBlob> CopyColorUndistortedPS;
	ComPtr<ID3DBlob> CopyDepthInfraredCS;
	ComPtr<ID3DBlob> CopyDepthInfraredUndistortedCS;
	ComPtr<ID3DBlob> CopyColorCS;
	ComPtr<ID3DBlob> CopyColorUndistortedCS;
	uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	D3D_SHADER_MACRO macro[] =
	{
		{"__hlsl"			,	"1"}, // 0
		{"QuadVS"			,	"1"}, // 1
		{"CopyPS"			,	"0"}, // 2
		{"CopyCS"			,	"0"}, // 3
		{"CorrectDistortion",	"0"}, // 4	
		{nullptr			,	nullptr}
	};
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "vsmain", "vs_5_1", compileFlags, 0, &QuadVS ) );
	macro[1].Definition = "0";
	macro[2].Definition = "1";
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_copy_depth_infrared_main", "ps_5_1", compileFlags, 0, &CopyDepthInfraredPS ) );
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_copy_color_main", "ps_5_1", compileFlags, 0, &CopyColorPS ) );
	macro[2].Definition = "0";
	macro[3].Definition = "1";
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "cs_copy_depth_infrared_main", "cs_5_1", compileFlags, 0, &CopyDepthInfraredCS ) );
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "cs_copy_color_main", "cs_5_1", compileFlags, 0, &CopyColorCS ) );
	macro[3].Definition = "0";
	macro[4].Definition = "1";
	macro[2].Definition = "1";
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_copy_depth_infrared_main", "ps_5_1", compileFlags, 0, &CopyDepthInfraredUndistortedPS) );
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_copy_color_main", "ps_5_1", compileFlags, 0, &CopyColorUndistortedPS ) );
	macro[2].Definition = "0";
	macro[3].Definition = "1";
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "cs_copy_depth_infrared_main", "cs_5_1", compileFlags, 0, &CopyDepthInfraredUndistortedCS ) );
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "KinectVisualizer.hlsl" ) ).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "cs_copy_color_main", "cs_5_1", compileFlags, 0, &CopyColorUndistortedCS ) );

	m_DepthInfraredCSPSO.SetRootSignature( m_RootSignature );
	m_DepthInfraredCSPSO.SetComputeShader( CopyDepthInfraredCS->GetBufferPointer(), CopyDepthInfraredCS->GetBufferSize() );
	m_DepthInfraredCSPSO.Finalize();

	m_DepthInfraredUndistortedCSPSO.SetRootSignature( m_RootSignature );
	m_DepthInfraredUndistortedCSPSO.SetComputeShader( CopyDepthInfraredUndistortedCS->GetBufferPointer(), CopyDepthInfraredUndistortedCS->GetBufferSize() );
	m_DepthInfraredUndistortedCSPSO.Finalize();

	m_ColorCSPSO.SetRootSignature( m_RootSignature );
	m_ColorCSPSO.SetComputeShader( CopyColorCS->GetBufferPointer(), CopyColorCS->GetBufferSize() );
	m_ColorCSPSO.Finalize();

	m_ColorUndistortedCSPSO.SetRootSignature( m_RootSignature );
	m_ColorUndistortedCSPSO.SetComputeShader( CopyColorUndistortedCS->GetBufferPointer(), CopyColorUndistortedCS->GetBufferSize() );
	m_ColorUndistortedCSPSO.Finalize();

	m_DepthInfraredPSO.SetRootSignature( m_RootSignature );
	m_DepthInfraredPSO.SetRasterizerState( Graphics::g_RasterizerTwoSided );
	m_DepthInfraredPSO.SetBlendState( Graphics::g_BlendDisable );
	m_DepthInfraredPSO.SetDepthStencilState( Graphics::g_DepthStateDisabled );
	m_DepthInfraredPSO.SetSampleMask( 0xFFFFFFFF );
	m_DepthInfraredPSO.SetInputLayout( 0, nullptr );
	m_DepthInfraredPSO.SetPrimitiveTopologyType( D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE );
	m_DepthInfraredPSO.SetVertexShader( QuadVS->GetBufferPointer(), QuadVS->GetBufferSize() );

	m_ColorPSO = m_DepthInfraredPSO;
	m_ColorPSO.SetRenderTargetFormats( 1, &m_Texture[IRGBDStreamer::kColor].GetFormat(), DXGI_FORMAT_UNKNOWN );
	m_ColorUndistortedPSO = m_ColorPSO;
	m_ColorUndistortedPSO.SetPixelShader( CopyColorUndistortedPS->GetBufferPointer(), CopyColorUndistortedPS->GetBufferSize() );
	m_ColorUndistortedPSO.Finalize();
	m_ColorPSO.SetPixelShader( CopyColorPS->GetBufferPointer(), CopyColorPS->GetBufferSize() );
	m_ColorPSO.Finalize();

	DXGI_FORMAT RTformat[] =
	{
		m_Texture[IRGBDStreamer::kDepth].GetFormat(),
		m_Texture[IRGBDStreamer::kInfrared].GetFormat()
	};
	m_DepthInfraredPSO.SetRenderTargetFormats( 2, RTformat, DXGI_FORMAT_UNKNOWN );
	m_DepthInfraredUndistortedPSO = m_DepthInfraredPSO;
	m_DepthInfraredUndistortedPSO.SetPixelShader( CopyDepthInfraredUndistortedPS->GetBufferPointer(), CopyDepthInfraredUndistortedPS->GetBufferSize() );
	m_DepthInfraredUndistortedPSO.Finalize();
	m_DepthInfraredPSO.SetPixelShader( CopyDepthInfraredPS->GetBufferPointer(), CopyDepthInfraredPS->GetBufferSize() );
	m_DepthInfraredPSO.Finalize();
	return S_OK;
}

HRESULT KinectVisualizer::OnSizeChanged()
{
	uint32_t width = Core::g_config.swapChainDesc.Width;
	uint32_t height = Core::g_config.swapChainDesc.Height;

	float fAspectRatio = width / (FLOAT)height;
	m_Camera.Projection( XM_PIDIV2 / 2, fAspectRatio );
	return S_OK;
}

void KinectVisualizer::OnUpdate()
{
	m_Camera.ProcessInertia();

	static bool showPanel = true;
	static bool showImage[IRGBDStreamer::kNumBufferTypes] = {};
	if (ImGui::Begin( "KinectVisualizer", &showPanel ))
	{
		ImGui::Checkbox( "Use Compute Shader", &_UseCSForCopy );
		ImGui::Checkbox( "Use Undistortion", &_CorrectDistortion );
		float width = ImGui::GetContentRegionAvail().x;
		std::string names[] = {"Color Image","Depth Image","Infrared Image"};
		for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
			if (ImGui::CollapsingHeader( names[i].c_str() ))
			{
				ImTextureID tex_id = (void*)&m_Texture[i].GetSRV();
				uint32_t OrigTexWidth = m_Texture[i].GetWidth();
				uint32_t OrigTexHeight = m_Texture[i].GetHeight();

				ImGui::AlignFirstTextHeightToWidgets();
				ImGui::Text( "Native Reso:%dx%d", OrigTexWidth, OrigTexHeight ); ImGui::SameLine();
				ImGui::Checkbox( ("Show " + names[i]).c_str(), &showImage[i] );
				ImGui::Image( tex_id, ImVec2( width, width*OrigTexHeight / OrigTexWidth ) );
			}
	}
	ImGui::End();

	for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
		if (showImage[i]) m_Texture[i].GuiShow();
}

void KinectVisualizer::OnRender( CommandContext & EngineContext )
{
	// Discard previous Kinect buffers, and request new buffers
	uint8_t* ptrs[IRGBDStreamer::kNumBufferTypes];
	for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
	{
		m_pFrameAlloc[i]->DiscardBuffer( Graphics::g_stats.lastFrameEndFence, m_pKinectBuffer[i] );
		m_pKinectBuffer[i] = m_pFrameAlloc[i]->RequestFrameBuffer();
		ptrs[i] = reinterpret_cast<uint8_t*>(m_pKinectBuffer[i]->GetMappedPtr());
	}

	// Acquire frame data from Kinect
	FrameData Frames[IRGBDStreamer::kNumBufferTypes];
	m_pKinect2->GetFrames( Frames[IRGBDStreamer::kColor], Frames[IRGBDStreamer::kDepth], Frames[IRGBDStreamer::kInfrared] );

	// Copy Kinect frames into requested buffers
	for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
		if (Frames[i].pData)
			std::memcpy( ptrs[i], Frames[i].pData, Frames[i].Size );

	// Render to screen
	if (_UseCSForCopy)
	{
		ComputeContext& Context = EngineContext.GetComputeContext();
		{
			GPU_PROFILE( Context, L"Read RGBD" );

			Context.SetRootSignature( m_RootSignature );
			Context.SetPipelineState( _CorrectDistortion ? m_DepthInfraredUndistortedCSPSO : m_DepthInfraredCSPSO );
			Context.SetDynamicConstantBufferView( 0, sizeof( RenderCB ), &m_KinectCB );

			Context.SetDynamicDescriptors( 1, 0, 1, &m_pKinectBuffer[IRGBDStreamer::kDepth]->GetSRV() );
			Context.SetDynamicDescriptors( 1, 1, 1, &m_pKinectBuffer[IRGBDStreamer::kInfrared]->GetSRV() );
			Context.SetDynamicDescriptors( 1, 2, 1, &m_pKinectBuffer[IRGBDStreamer::kColor]->GetSRV() );

			Context.SetDynamicDescriptors( 2, 0, 1, &m_Texture[IRGBDStreamer::kDepth].GetUAV() );
			Context.SetDynamicDescriptors( 2, 1, 1, &m_Texture[IRGBDStreamer::kInfrared].GetUAV() );
			Context.SetDynamicDescriptors( 2, 2, 1, &m_Texture[IRGBDStreamer::kColor].GetUAV() );

			Context.Dispatch1D( m_Texture[IRGBDStreamer::kDepth].GetWidth()*m_Texture[IRGBDStreamer::kDepth].GetHeight(), THREAD_PER_GROUP );

			Context.SetPipelineState( _CorrectDistortion ? m_ColorUndistortedCSPSO : m_ColorCSPSO );
			Context.Dispatch1D( m_Texture[IRGBDStreamer::kColor].GetWidth()*m_Texture[IRGBDStreamer::kColor].GetHeight(), THREAD_PER_GROUP );

			Context.TransitionResource( m_Texture[IRGBDStreamer::kColor], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			Context.TransitionResource( m_Texture[IRGBDStreamer::kDepth], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			Context.TransitionResource( m_Texture[IRGBDStreamer::kInfrared], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		}
	}
	else
	{
		GraphicsContext& Context = EngineContext.GetGraphicsContext();
		{
			GPU_PROFILE( Context, L"Read RGBD" );

			Context.SetRootSignature( m_RootSignature );
			Context.SetPipelineState( _CorrectDistortion ? m_DepthInfraredUndistortedPSO : m_DepthInfraredPSO );
			Context.SetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
			Context.SetDynamicConstantBufferView( 0, sizeof( RenderCB ), &m_KinectCB );

			Context.SetDynamicDescriptors( 1, 0, 1, &m_pKinectBuffer[IRGBDStreamer::kDepth]->GetSRV() );
			Context.SetDynamicDescriptors( 1, 1, 1, &m_pKinectBuffer[IRGBDStreamer::kInfrared]->GetSRV() );
			Context.SetDynamicDescriptors( 1, 2, 1, &m_pKinectBuffer[IRGBDStreamer::kColor]->GetSRV() );

			Context.SetRenderTargets( 2, &m_Texture[IRGBDStreamer::kDepth] );
			Context.SetViewports( 1, &m_DepthInfraredViewport );
			Context.SetScisors( 1, &m_DepthInfraredScissorRect );
			Context.Draw( 3 );

			Context.SetPipelineState( _CorrectDistortion ? m_ColorUndistortedPSO : m_ColorPSO );
			Context.SetRenderTargets( 1, &m_Texture[IRGBDStreamer::kColor] );
			Context.SetViewports( 1, &m_ColorViewport );
			Context.SetScisors( 1, &m_ColorScissorRect );
			Context.Draw( 3 );

			Context.TransitionResource( m_Texture[IRGBDStreamer::kColor], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			Context.TransitionResource( m_Texture[IRGBDStreamer::kDepth], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			Context.TransitionResource( m_Texture[IRGBDStreamer::kInfrared], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
		}
	}
}

void KinectVisualizer::OnDestroy()
{
	for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
	{
		m_pFrameAlloc[i]->Destory();
		delete m_pFrameAlloc[i];
	}
}

bool KinectVisualizer::OnEvent( MSG * msg )
{
	return false;
}
