#include "pch.h"
#include "SensorTexGen.h"

using namespace DirectX;
using namespace Microsoft::WRL;

SensorTexGen::SensorTexGen( bool EnableColor, bool EnableDepth, bool EnableInfrared )
{
	m_pKinect2 = StreamFactory::createFromKinect2( EnableColor, EnableDepth, EnableInfrared);
	m_pKinect2->StartStream();
}

SensorTexGen::~SensorTexGen()
{
}

void SensorTexGen::OnCreateResource()
{
	HRESULT hr = S_OK;
	uint16_t DepWidth, DepHeight;
	do
	{
		m_pKinect2->GetDepthReso( DepWidth, DepHeight );
	} while (DepWidth == 0 || DepHeight == 0);

	m_KinectCB.DepthInfraredReso = XMFLOAT2( DepWidth, DepHeight );

	m_pFrameAlloc[IRGBDStreamer::kDepth] = new LinearFrameAllocator( DepWidth * DepHeight, sizeof( uint16_t ), DXGI_FORMAT_R16_UINT );
	m_pKinectBuffer[IRGBDStreamer::kDepth] = m_pFrameAlloc[IRGBDStreamer::kDepth]->RequestFrameBuffer();
	m_OutTexture[kDepthVisualTex].Create( L"Depth Visual Tex", DepWidth, DepHeight, 1, DXGI_FORMAT_R11G11B10_FLOAT );
	m_OutTexture[kDepthTex].Create( L"Depth Raw Tex", DepWidth, DepHeight, 1, DXGI_FORMAT_R16_UINT );

	m_pFrameAlloc[IRGBDStreamer::kInfrared] = new LinearFrameAllocator( DepWidth * DepHeight, sizeof( uint16_t ), DXGI_FORMAT_R16_UINT );
	m_pKinectBuffer[IRGBDStreamer::kInfrared] = m_pFrameAlloc[IRGBDStreamer::kInfrared]->RequestFrameBuffer();
	m_OutTexture[kInfraredTex].Create( L"Infrared Tex", DepWidth, DepHeight, 1, DXGI_FORMAT_R11G11B10_FLOAT );

	m_DepthInfraredViewport = {};
	m_DepthInfraredViewport.Width = DepWidth;
	m_DepthInfraredViewport.Height = DepHeight;
	m_DepthInfraredViewport.MaxDepth = 1.0f;
	m_DepthInfraredScissorRect = {};
	m_DepthInfraredScissorRect.right = DepWidth;
	m_DepthInfraredScissorRect.bottom = DepHeight;

	uint16_t ColWidth, ColHeight;
	do
	{
		m_pKinect2->GetColorReso( ColWidth, ColHeight );
	} while (ColWidth == 0 || ColHeight == 0);

	m_KinectCB.ColorReso = XMFLOAT2( ColWidth, ColHeight );

	m_pFrameAlloc[IRGBDStreamer::kColor] = new LinearFrameAllocator( ColWidth * ColHeight, sizeof( uint32_t ), DXGI_FORMAT_R8G8B8A8_UINT );
	m_pKinectBuffer[IRGBDStreamer::kColor] = m_pFrameAlloc[IRGBDStreamer::kColor]->RequestFrameBuffer();
	m_OutTexture[kColorTex].Create( L"Color Tex", ColWidth, ColHeight, 1, DXGI_FORMAT_R11G11B10_FLOAT );

	m_ColorViewport = {};
	m_ColorViewport.Width = ColWidth;
	m_ColorViewport.Height = ColHeight;
	m_ColorViewport.MaxDepth = 1.0f;
	m_ColorScissorRect = {};
	m_ColorScissorRect.right = ColWidth;
	m_ColorScissorRect.bottom = ColHeight;

	// Create rootsignature
	m_RootSignature.Reset( 3 );
	m_RootSignature[0].InitAsConstantBuffer( 0 );
	m_RootSignature[1].InitAsDescriptorRange( D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, IRGBDStreamer::kNumBufferTypes );
	m_RootSignature[2].InitAsDescriptorRange( D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, IRGBDStreamer::kNumBufferTypes + 1 );
	m_RootSignature.Finalize( D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
		D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS );

	// Create PSO
	ComPtr<ID3DBlob> QuadVS;
	ComPtr<ID3DBlob> CopyDepthPS[kNumDepthMode][kNumDataMode];
	ComPtr<ID3DBlob> CopyColorPS[kNumColorMode][kNumDataMode];
	ComPtr<ID3DBlob> CopyDepthCS[kNumDepthMode][kNumDataMode];
	ComPtr<ID3DBlob> CopyColorCS[kNumColorMode][kNumDataMode];
	uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
	D3D_SHADER_MACRO macro[] =
	{
		{"__hlsl"			,	"1"}, // 0
		{"QuadVS"			,	"1"}, // 1
		{"CopyPS"			,	"0"}, // 2
		{"CopyCS"			,	"0"}, // 3
		{"CorrectDistortion",	"0"}, // 4	
		{"VisualizedDepth"	,	"0"}, // 5	
		{"Infrared"			,	"0"}, // 6	
		{nullptr			,	nullptr}
	};
	V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SensorTexGen.hlsl" ) ).c_str(),
		macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "vsmain", "vs_5_1", compileFlags, 0, &QuadVS ) );
	macro[1].Definition = "0";

	char tempChar[8];
	for (int i = 0; i < kNumDataMode; ++i)
	{
		sprintf( tempChar, "%d", i );
		macro[4].Definition = tempChar; // CorrectDistortion
		macro[2].Definition = "1"; // CopyPS
		for (int j = 0; j < kNumDepthMode; ++j)
		{
			switch (j)
			{
			case kDepthWithVisualWithInfrared:
				macro[6].Definition = "1"; // Infrared
			case kDepthVisualTex:
				macro[5].Definition = "1"; // VisualizedDepth
			}
			V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SensorTexGen.hlsl" ) ).c_str(), macro,
				D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_copy_depth_infrared_main", "ps_5_1", compileFlags, 0, &CopyDepthPS[j][i] ) );
			macro[2].Definition = "0"; // CopyPS
			macro[3].Definition = "1"; // CopyCS
			V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SensorTexGen.hlsl" ) ).c_str(), macro,
				D3D_COMPILE_STANDARD_FILE_INCLUDE, "cs_copy_depth_infrared_main", "cs_5_1", compileFlags, 0, &CopyDepthCS[j][i] ) );

			macro[2].Definition = "1"; // CopyPS
			macro[3].Definition = "0"; // CopyCS
			macro[5].Definition = "0"; // VisualizedDepth
			macro[6].Definition = "0"; // Infrared
		}
		V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SensorTexGen.hlsl" ) ).c_str(), macro,
			D3D_COMPILE_STANDARD_FILE_INCLUDE, "ps_copy_color_main", "ps_5_1", compileFlags, 0, &CopyColorPS[0][i] ) );
		macro[2].Definition = "0"; // CopyPS
		macro[3].Definition = "1"; // CopyCS
		V( Graphics::CompileShaderFromFile( Core::GetAssetFullPath( _T( "SensorTexGen.hlsl" ) ).c_str(), macro,
			D3D_COMPILE_STANDARD_FILE_INCLUDE, "cs_copy_color_main", "cs_5_1", compileFlags, 0, &CopyColorCS[0][i] ) );
		macro[3].Definition = "0"; // CopyCS
	}

	DXGI_FORMAT RTformat[] =
	{
		m_OutTexture[kDepthTex].GetFormat(),
		m_OutTexture[kDepthVisualTex].GetFormat(),
		m_OutTexture[kInfraredTex].GetFormat(),
		m_OutTexture[kColorTex].GetFormat()
	};

	for (int i = 0; i < kNumDataMode; ++i)
	{
		m_GfxColorPSO[0][i].SetRootSignature( m_RootSignature );
		m_GfxColorPSO[0][i].SetRasterizerState( Graphics::g_RasterizerTwoSided );
		m_GfxColorPSO[0][i].SetBlendState( Graphics::g_BlendDisable );
		m_GfxColorPSO[0][i].SetDepthStencilState( Graphics::g_DepthStateDisabled );
		m_GfxColorPSO[0][i].SetSampleMask( 0xFFFFFFFF );
		m_GfxColorPSO[0][i].SetInputLayout( 0, nullptr );
		m_GfxColorPSO[0][i].SetPrimitiveTopologyType( D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE );
		m_GfxColorPSO[0][i].SetVertexShader( QuadVS->GetBufferPointer(), QuadVS->GetBufferSize() );
		m_GfxColorPSO[0][i].SetRenderTargetFormats( 1, &m_OutTexture[kColorTex].GetFormat(), DXGI_FORMAT_UNKNOWN );
		m_GfxColorPSO[0][i].SetPixelShader( CopyColorPS[0][i]->GetBufferPointer(), CopyColorPS[0][i]->GetBufferSize() );
		m_CptColorPSO[0][i].SetRootSignature( m_RootSignature );
		m_CptColorPSO[0][i].SetComputeShader( CopyColorCS[0][i]->GetBufferPointer(), CopyColorCS[0][i]->GetBufferSize() );
		m_CptColorPSO[0][i].Finalize();

		for (int j = 0; j < kNumDepthMode; ++j)
		{
			m_GfxDepthPSO[j][i] = m_GfxColorPSO[0][i];
			m_GfxDepthPSO[j][i].SetPixelShader( CopyDepthPS[j][i]->GetBufferPointer(), CopyDepthPS[j][i]->GetBufferSize() );
			m_GfxDepthPSO[j][i].SetRenderTargetFormats( j + 1, RTformat, DXGI_FORMAT_UNKNOWN );
			m_GfxDepthPSO[j][i].Finalize();

			m_CptDepthPSO[j][i].SetRootSignature( m_RootSignature );
			m_CptDepthPSO[j][i].SetComputeShader( CopyDepthCS[j][i]->GetBufferPointer(), CopyDepthCS[j][i]->GetBufferSize() );
			m_CptDepthPSO[j][i].Finalize();
		}
		m_GfxColorPSO[0][i].Finalize();
	}
}

void SensorTexGen::OnDestory()
{
	for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
	{
		m_pFrameAlloc[i]->Destory();
		delete m_pFrameAlloc[i];
	}
}

void SensorTexGen::OnRender( CommandContext& EngineContext )
{
	if (m_Streaming)
	{
		// Discard previous Kinect buffers, and request new buffers
		uint8_t* ptrs[IRGBDStreamer::kNumBufferTypes];
		for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
		{
			if (i == IRGBDStreamer::kColor && m_ColorMode == kNoColor) continue;
			if (i == IRGBDStreamer::kDepth && m_DepthMode == kNoDepth) continue;
			if (i == IRGBDStreamer::kInfrared && m_DepthMode != kDepthWithVisualWithInfrared) continue;
			m_pFrameAlloc[i]->DiscardBuffer( Graphics::g_stats.lastFrameEndFence, m_pKinectBuffer[i] );
			m_pKinectBuffer[i] = m_pFrameAlloc[i]->RequestFrameBuffer();
			ptrs[i] = reinterpret_cast<uint8_t*>(m_pKinectBuffer[i]->GetMappedPtr());
		}

		// Acquire frame data from Kinect
		FrameData Frames[IRGBDStreamer::kNumBufferTypes];
		m_pKinect2->GetFrames( Frames[IRGBDStreamer::kColor], Frames[IRGBDStreamer::kDepth], Frames[IRGBDStreamer::kInfrared] );

		// Copy Kinect frames into requested buffers
		for (int i = 0; i < IRGBDStreamer::kNumBufferTypes; ++i)
		{
			if (i == IRGBDStreamer::kColor && m_ColorMode == kNoColor) continue;
			if (i == IRGBDStreamer::kDepth && m_DepthMode == kNoDepth) continue;
			if (i == IRGBDStreamer::kInfrared && m_DepthMode != kDepthWithVisualWithInfrared) continue;
			if (Frames[i].pData)
				std::memcpy( ptrs[i], Frames[i].pData, Frames[i].Size );
		}
		// Render to screen
		if (m_UseCS)
		{
			ComputeContext& Context = EngineContext.GetComputeContext();
			{
				GPU_PROFILE( Context, L"Read&Present RGBD" );

				Context.SetRootSignature( m_RootSignature );
				Context.SetDynamicConstantBufferView( 0, sizeof( RenderCB ), &m_KinectCB );

				Context.SetDynamicDescriptors( 1, 0, 1, &m_pKinectBuffer[IRGBDStreamer::kDepth]->GetSRV() );
				Context.SetDynamicDescriptors( 1, 1, 1, &m_pKinectBuffer[IRGBDStreamer::kInfrared]->GetSRV() );
				Context.SetDynamicDescriptors( 1, 2, 1, &m_pKinectBuffer[IRGBDStreamer::kColor]->GetSRV() );

				Context.SetDynamicDescriptors( 2, 0, 1, &m_OutTexture[kDepthTex].GetUAV() );
				Context.SetDynamicDescriptors( 2, 1, 1, &m_OutTexture[kDepthVisualTex].GetUAV() );
				Context.SetDynamicDescriptors( 2, 2, 1, &m_OutTexture[kInfraredTex].GetUAV() );
				Context.SetDynamicDescriptors( 2, 3, 1, &m_OutTexture[kColorTex].GetUAV() );
				Context.SetPipelineState( m_CptDepthPSO[m_DepthMode][m_ProcessMode] );
				Context.Dispatch1D( m_OutTexture[kDepthTex].GetWidth()*m_OutTexture[kDepthTex].GetHeight(), THREAD_PER_GROUP );

				if (m_ColorMode != kNoColor)
				{
					Context.SetPipelineState( m_CptColorPSO[m_ColorMode][m_ProcessMode] );
					Context.Dispatch1D( m_OutTexture[kColorTex].GetWidth()*m_OutTexture[kColorTex].GetHeight(), THREAD_PER_GROUP );
				}
				Context.TransitionResource( m_OutTexture[kDepthVisualTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				Context.TransitionResource( m_OutTexture[kDepthTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				Context.TransitionResource( m_OutTexture[kInfraredTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				Context.TransitionResource( m_OutTexture[kColorTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}
		}
		else
		{
			GraphicsContext& Context = EngineContext.GetGraphicsContext();
			{
				GPU_PROFILE( Context, L"Read&Present RGBD" );

				Context.SetRootSignature( m_RootSignature );
				Context.SetPrimitiveTopology( D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
				Context.SetDynamicConstantBufferView( 0, sizeof( RenderCB ), &m_KinectCB );

				Context.SetDynamicDescriptors( 1, 0, 1, &m_pKinectBuffer[IRGBDStreamer::kDepth]->GetSRV() );
				Context.SetDynamicDescriptors( 1, 1, 1, &m_pKinectBuffer[IRGBDStreamer::kInfrared]->GetSRV() );
				Context.SetDynamicDescriptors( 1, 2, 1, &m_pKinectBuffer[IRGBDStreamer::kColor]->GetSRV() );

				Context.SetRenderTargets( m_DepthMode + 1, m_OutTexture );
				Context.SetViewports( 1, &m_DepthInfraredViewport );
				Context.SetScisors( 1, &m_DepthInfraredScissorRect );
				Context.SetPipelineState( m_GfxDepthPSO[m_DepthMode][m_ProcessMode] );
				Context.Draw( 3 );

				Context.SetRenderTargets( 1, &m_OutTexture[kColorTex] );
				Context.SetViewports( 1, &m_ColorViewport );
				Context.SetScisors( 1, &m_ColorScissorRect );
				if (m_ColorMode != kNoColor)
				{
					Context.SetPipelineState( m_GfxColorPSO[0][m_ProcessMode] );
					Context.Draw( 3 );
				}
				Context.TransitionResource( m_OutTexture[kDepthTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				Context.TransitionResource( m_OutTexture[kDepthVisualTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				Context.TransitionResource( m_OutTexture[kInfraredTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
				Context.TransitionResource( m_OutTexture[kColorTex], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			}
		}
	}
}

void SensorTexGen::RenderGui()
{
	static bool showImage[kNumTargetTex] = {};
	if (ImGui::CollapsingHeader( "Sensor Tex Generator" ))
	{
		ImGui::Checkbox( "Streaming Data", &m_Streaming );
		ImGui::Checkbox( "Use Compute Shader", &m_UseCS );
		ImGui::Checkbox( "Use Undistortion", (bool*)&m_ProcessMode );
		ImGui::Separator();
		ImGui::RadioButton( "No Depth", (int*)&m_DepthMode, -1 );
		ImGui::RadioButton( "Depth", (int*)&m_DepthMode, 0 );
		ImGui::RadioButton( "Depth&Visual", (int*)&m_DepthMode, 1 );
		ImGui::RadioButton( "Depth&Visual&Infrared", (int*)&m_DepthMode, 2 );
		ImGui::Separator();
		ImGui::RadioButton( "No Color", (int*)&m_ColorMode, -1 );
		ImGui::RadioButton( "Color", (int*)&m_ColorMode, 0 );

		float width = ImGui::GetContentRegionAvail().x;
		for (int i = 1; i < kNumTargetTex; ++i)
			if (ImGui::CollapsingHeader( TargetTexName[i].c_str() ))
			{
				ImTextureID tex_id = (void*)&m_OutTexture[i].GetSRV();
				uint32_t OrigTexWidth = m_OutTexture[i].GetWidth();
				uint32_t OrigTexHeight = m_OutTexture[i].GetHeight();

				ImGui::AlignFirstTextHeightToWidgets();
				ImGui::Text( "Native Reso:%dx%d", OrigTexWidth, OrigTexHeight ); ImGui::SameLine();
				ImGui::Checkbox( ("Show " + TargetTexName[i]).c_str(), &showImage[i] );
				ImGui::Image( tex_id, ImVec2( width, width*OrigTexHeight / OrigTexWidth ) );
			}
	}
	for (int i = 1; i < kNumTargetTex; ++i)
		if (showImage[i]) m_OutTexture[i].GuiShow();
	if (m_DepthMode == kNoDepth && m_ColorMode == kNoColor) m_DepthMode = kDepth;
}

void SensorTexGen::GetColorReso( uint16_t& width, uint16_t& height )
{
	width = m_OutTexture[kColorTex].GetWidth();
	height = m_OutTexture[kColorTex].GetHeight();
}

void SensorTexGen::GetDepthInfrareReso( uint16_t& width, uint16_t& height )
{
	width = m_OutTexture[kDepthTex].GetWidth();
	height = m_OutTexture[kDepthTex].GetHeight();
}