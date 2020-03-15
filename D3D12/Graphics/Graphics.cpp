#include "stdafx.h"
#include "Graphics.h"
#include "CommandAllocatorPool.h"
#include "CommandQueue.h"
#include "CommandContext.h"
#include "OfflineDescriptorAllocator.h"
#include "GraphicsResource.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Shader.h"
#include "Mesh.h"
#include "DynamicResourceAllocator.h"
#include "ImGuiRenderer.h"
#include "Core/Input.h"
#include "Texture.h"
#include "GraphicsBuffer.h"
#include "Profiler.h"
#include "ClusteredForward.h"
#include "Scene/Camera.h"
#include "RenderGraph/RenderGraph.h"
#include "RenderGraph/Blackboard.h"
#include "RenderGraph/ResourceAllocator.h"
#include "DebugRenderer.h"
#include "ResourceViews.h"
#include "TiledForward.h"

#ifdef _DEBUG
#define D3D_VALIDATION 1
#endif

#ifndef D3D_VALIDATION
#define D3D_VALIDATION 0
#endif

#ifndef GPU_VALIDATION
#define GPU_VALIDATION 0
#endif

const DXGI_FORMAT Graphics::DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D32_FLOAT;
const DXGI_FORMAT Graphics::DEPTH_STENCIL_SHADOW_FORMAT = DXGI_FORMAT_D16_UNORM;
const DXGI_FORMAT Graphics::RENDER_TARGET_FORMAT = DXGI_FORMAT_R11G11B10_FLOAT;
const DXGI_FORMAT Graphics::SWAPCHAIN_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

bool gDumpRenderGraph = false;

float g_WhitePoint = 4;
float g_MinLogLuminance = -10;
float g_MaxLogLuminance = 2;
float g_Tau = 10;

float g_AoPower = 3;
float g_AoThreshold = 0.0025f;
float g_AoRadius = 0.25f;
int g_AoSamples = 16;

Graphics::Graphics(uint32 width, uint32 height, int sampleCount /*= 1*/)
	: m_WindowWidth(width), m_WindowHeight(height), m_SampleCount(sampleCount)
{

}

Graphics::~Graphics()
{
}

void Graphics::Initialize(HWND window)
{
	m_pWindow = window;

	m_pCamera = std::make_unique<FreeCamera>(this);
	m_pCamera->SetPosition(Vector3(0, 100, -15));
	m_pCamera->SetRotation(Quaternion::CreateFromYawPitchRoll(Math::PIDIV4, Math::PIDIV4, 0));
	m_pCamera->SetNearPlane(500.0f);
	m_pCamera->SetFarPlane(10.0f);
	m_pCamera->SetViewport(0, 0, 1, 1);

	Shader::AddGlobalShaderDefine("BLOCK_SIZE", std::to_string(FORWARD_PLUS_BLOCK_SIZE));
	Shader::AddGlobalShaderDefine("SHADOWMAP_DX", std::to_string(1.0f / SHADOW_MAP_SIZE));
	Shader::AddGlobalShaderDefine("PCF_KERNEL_SIZE", std::to_string(5));
	Shader::AddGlobalShaderDefine("MAX_SHADOW_CASTERS", std::to_string(MAX_SHADOW_CASTERS));

	InitD3D();
	InitializeAssets();

	RandomizeLights(m_DesiredLightCount);
}

void Graphics::RandomizeLights(int count)
{
	m_Lights.resize(count);

	BoundingBox sceneBounds;
	sceneBounds.Center = Vector3(0, 70, 0);
	sceneBounds.Extents = Vector3(140, 70, 60);

	int lightIndex = 0;
	Vector3 Dir(-300, -300, -300);
	Dir.Normalize();
	m_Lights[lightIndex] = Light::Directional(Vector3(300, 300, 300), Dir, 0.1f);
	m_Lights[lightIndex].ShadowIndex = 0;
	
	int randomLightsStartIndex = lightIndex+1;

	for (int i = randomLightsStartIndex; i < m_Lights.size(); ++i)
	{
		Vector3 c = Vector3(Math::RandomRange(0.0f, 1.0f), Math::RandomRange(0.0f, 1.0f), Math::RandomRange(0.0f, 1.0f));
		Vector4 color(c.x, c.y, c.z, 1);

		Vector3 position;
		position.x = Math::RandomRange(-sceneBounds.Extents.x, sceneBounds.Extents.x) + sceneBounds.Center.x;
		position.y = Math::RandomRange(-sceneBounds.Extents.y, sceneBounds.Extents.y) + sceneBounds.Center.y;
		position.z = Math::RandomRange(-sceneBounds.Extents.z, sceneBounds.Extents.z) + sceneBounds.Center.z;

		const float range = Math::RandomRange(4.0f, 6.0f);
		const float angle = Math::RandomRange(40.0f, 80.0f);

		Light::Type type = rand() % 2 == 0 ? Light::Type::Point : Light::Type::Spot;
		switch (type)
		{
		case Light::Type::Point:
			m_Lights[i] = Light::Point(position, range, 4.0f, 0.5f, color);
			break;
		case Light::Type::Spot:
			m_Lights[i] = Light::Spot(position, range, Math::RandVector(), angle, 4.0f, 0.5f, color);
			break;
		case Light::Type::Directional:
		case Light::Type::MAX:
		default:
			assert(false);
			break;
		}
	}

	//It's a bit weird but I don't sort the lights that I manually created because I access them by their original index during the update function
	std::sort(m_Lights.begin() + randomLightsStartIndex, m_Lights.end(), [](const Light& a, const Light& b) { return (int)a.LightType < (int)b.LightType; });

	IdleGPU();
	if (m_pLightBuffer->GetDesc().ElementCount != count)
	{
		m_pLightBuffer->Create(BufferDesc::CreateStructured(count, sizeof(Light)));
	}
	CommandContext* pContext = AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_pLightBuffer->SetData(pContext, m_Lights.data(), sizeof(Light) * m_Lights.size());
	pContext->Execute(true);
}

void Graphics::Update()
{
	PROFILE_BEGIN("Update Game State");

	m_pCamera->Update();

	if (Input::Instance().IsKeyPressed('O'))
	{
		RandomizeLights(m_DesiredLightCount);
	}

	std::sort(m_TransparantBatches.begin(), m_TransparantBatches.end(), [this](const Batch& a, const Batch& b) {
		float aDist = Vector3::DistanceSquared(a.pMesh->GetBounds().Center, m_pCamera->GetPosition());
		float bDist = Vector3::DistanceSquared(b.pMesh->GetBounds().Center, m_pCamera->GetPosition());
		return aDist > bDist;
		});

	std::sort(m_OpaqueBatches.begin(), m_OpaqueBatches.end(), [this](const Batch& a, const Batch& b) {
		float aDist = Vector3::DistanceSquared(a.pMesh->GetBounds().Center, m_pCamera->GetPosition());
		float bDist = Vector3::DistanceSquared(b.pMesh->GetBounds().Center, m_pCamera->GetPosition());
		return aDist < bDist;
		});

	// SHADOW MAP PARTITIONING
	/////////////////////////////////////////

	struct LightData
	{
		Matrix LightViewProjections[MAX_SHADOW_CASTERS];
		Vector4 ShadowMapOffsets[MAX_SHADOW_CASTERS];
	} lightData;

	Matrix projection = Math::CreateOrthographicMatrix(512, 512, 10000, 0.1f);
	
	m_ShadowCasters = 0;
	lightData.LightViewProjections[m_ShadowCasters] = Matrix(XMMatrixLookAtLH(m_Lights[0].Position, Vector3(), Vector3(0.0f, 1.0f, 0.0f))) * projection;
	lightData.ShadowMapOffsets[m_ShadowCasters].x = 0.0f;
	lightData.ShadowMapOffsets[m_ShadowCasters].y = 0.0f;
	lightData.ShadowMapOffsets[m_ShadowCasters].z = 1.0f;
	++m_ShadowCasters;

	////////////////////////////////
	// LET THE RENDERING BEGIN!
	////////////////////////////////
	
	PROFILE_END();

	BeginFrame();
	m_pImGuiRenderer->Update();

	RGGraph graph(m_pGraphAllocator.get());
	struct MainData
	{
		RGResourceHandle DepthStencil;
		RGResourceHandle DepthStencilResolved;
	};
	MainData Data;
	Data.DepthStencil = graph.ImportTexture("Depth Stencil", GetDepthStencil());
	Data.DepthStencilResolved = graph.ImportTexture("Resolved Depth Stencil", GetResolvedDepthStencil());

	uint64 nextFenceValue = 0;
	uint64 lightCullingFence = 0;

	//1. DEPTH PREPASS
	// - Depth only pass that renders the entire scene
	// - Optimization that prevents wasteful lighting calculations during the base pass
	// - Required for light culling
	graph.AddPass("Depth Prepass", [&](RGPassBuilder& builder)
		{
			builder.NeverCull();
			Data.DepthStencil = builder.Write(Data.DepthStencil);

			return [=](CommandContext& renderContext, const RGPassResources& resources)
			{
				Texture* pDepthStencil = resources.GetTexture(Data.DepthStencil);
				const TextureDesc& desc = pDepthStencil->GetDesc();
				renderContext.InsertResourceBarrier(pDepthStencil, D3D12_RESOURCE_STATE_DEPTH_WRITE);
				renderContext.InsertResourceBarrier(m_pMSAANormals.get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

				RenderPassInfo info = RenderPassInfo(m_pMSAANormals.get(), RenderPassAccess::Clear_Resolve, pDepthStencil, RenderPassAccess::Clear_Store);
				info.RenderTargets[0].ResolveTarget = m_pNormals.get();

				renderContext.BeginRenderPass(info);
				renderContext.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
				renderContext.SetViewport(FloatRect(0, 0, (float)desc.Width, (float)desc.Height));

				renderContext.SetGraphicsPipelineState(m_pDepthPrepassPSO.get());
				renderContext.SetGraphicsRootSignature(m_pDepthPrepassRS.get());

				struct Parameters
				{
					Matrix World;
					Matrix WorldViewProj;
				} constBuffer;

				for (const Batch& b : m_OpaqueBatches)
				{
					constBuffer.World = b.WorldMatrix;
					constBuffer.WorldViewProj = constBuffer.World * m_pCamera->GetViewProjection();

					renderContext.SetDynamicConstantBufferView(0, &constBuffer, sizeof(Parameters));
					renderContext.SetDynamicDescriptor(1, 0, b.pMaterial->pNormalTexture->GetSRV());
					b.pMesh->Draw(&renderContext);
				}
				renderContext.EndRenderPass();
			};
		});

	//2. [OPTIONAL] DEPTH RESOLVE
	// - If MSAA is enabled, run a compute shader to resolve the depth buffer
	if (m_SampleCount > 1)
	{
		graph.AddPass("Depth Resolve", [&](RGPassBuilder& builder)
			{
				Data.DepthStencil = builder.Read(Data.DepthStencil);
				Data.DepthStencilResolved = builder.Write(Data.DepthStencilResolved);

				return [=](CommandContext& renderContext, const RGPassResources& resources)
				{
					renderContext.InsertResourceBarrier(resources.GetTexture(Data.DepthStencil), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					renderContext.InsertResourceBarrier(resources.GetTexture(Data.DepthStencilResolved), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

					renderContext.SetComputeRootSignature(m_pResolveDepthRS.get());
					renderContext.SetComputePipelineState(m_pResolveDepthPSO.get());

					renderContext.SetDynamicDescriptor(0, 0, resources.GetTexture(Data.DepthStencilResolved)->GetUAV());
					renderContext.SetDynamicDescriptor(1, 0, resources.GetTexture(Data.DepthStencil)->GetSRV());

					int dispatchGroupsX = Math::DivideAndRoundUp(m_WindowWidth, 16);
					int dispatchGroupsY = Math::DivideAndRoundUp(m_WindowHeight, 16);
					renderContext.Dispatch(dispatchGroupsX, dispatchGroupsY);

					renderContext.InsertResourceBarrier(resources.GetTexture(Data.DepthStencilResolved), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					renderContext.InsertResourceBarrier(resources.GetTexture(Data.DepthStencil), D3D12_RESOURCE_STATE_DEPTH_READ);
					renderContext.FlushResourceBarriers();
				};
			});
	}

	graph.AddPass("SSAO", [&](RGPassBuilder& builder)
		{
			builder.NeverCull();
			Data.DepthStencilResolved = builder.Read(Data.DepthStencilResolved);
			return [=](CommandContext& renderContext, const RGPassResources& resources)
			{
				renderContext.InsertResourceBarrier(resources.GetTexture(Data.DepthStencilResolved), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				renderContext.InsertResourceBarrier(m_pNormals.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				renderContext.InsertResourceBarrier(m_pSSAOTarget.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				renderContext.InsertResourceBarrier(m_pNoiseTexture.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

				renderContext.SetComputeRootSignature(m_pSSAORS.get());
				renderContext.SetComputePipelineState(m_pSSAOPSO.get());

				constexpr int ssaoRandomVectors = 64;
				struct ShaderParameters
				{
					Vector4 RandomVectors[ssaoRandomVectors];
					Matrix ProjectionInverse;
					Matrix Projection;
					Matrix View;
					uint32 Dimensions[2];
					float Near;
					float Far;
					float Power;
					float Radius;
					float Threshold;
					int Samples;
				} shaderParameters;

				//lovely hacky
				static bool written = false;
				static Vector4 randoms[ssaoRandomVectors];
				if (!written)
				{
					for (int i = 0; i < ssaoRandomVectors; ++i)
					{
						randoms[i] = Vector4(Math::RandVector());
						randoms[i].z = Math::Lerp(0.1f, 0.8f, (float)abs(randoms[i].z));
						randoms[i].Normalize();
						randoms[i] *= Math::Lerp(0.2f, 1.0f, (float)pow(Math::RandomRange(0, 1), 2));
					}
					written = true;
				}
				memcpy(shaderParameters.RandomVectors, randoms, sizeof(Vector4) * ssaoRandomVectors);

				shaderParameters.ProjectionInverse = m_pCamera->GetProjectionInverse();
				shaderParameters.Projection = m_pCamera->GetProjection();
				shaderParameters.View = m_pCamera->GetView();
				shaderParameters.Dimensions[0] = m_pSSAOTarget->GetWidth();
				shaderParameters.Dimensions[1] = m_pSSAOTarget->GetHeight();
				shaderParameters.Near = m_pCamera->GetNear();
				shaderParameters.Far = m_pCamera->GetFar();
				shaderParameters.Power = g_AoPower;
				shaderParameters.Radius = g_AoRadius;
				shaderParameters.Threshold = g_AoThreshold;
				shaderParameters.Samples = g_AoSamples;

				renderContext.SetComputeDynamicConstantBufferView(0, &shaderParameters, sizeof(ShaderParameters));
				renderContext.SetDynamicDescriptor(1, 0, m_pSSAOTarget->GetUAV());
				renderContext.SetDynamicDescriptor(2, 0, resources.GetTexture(Data.DepthStencilResolved)->GetSRV());
				renderContext.SetDynamicDescriptor(2, 1, m_pNormals.get()->GetSRV());
				renderContext.SetDynamicDescriptor(2, 2, m_pNoiseTexture.get()->GetSRV());

				int dispatchGroupsX = Math::DivideAndRoundUp(m_pSSAOTarget->GetWidth(), 16);
				int dispatchGroupsY = Math::DivideAndRoundUp(m_pSSAOTarget->GetHeight(), 16);
				renderContext.Dispatch(dispatchGroupsX, dispatchGroupsY);

				renderContext.InsertResourceBarrier(resources.GetTexture(Data.DepthStencilResolved), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			};
		});

	graph.AddPass("Blur SSAO", [&](RGPassBuilder& builder)
		{
			builder.NeverCull();
			return [=](CommandContext& renderContext, const RGPassResources& resources)
			{
				renderContext.InsertResourceBarrier(m_pSSAOBlurred.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				renderContext.InsertResourceBarrier(m_pSSAOTarget.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

				renderContext.SetComputeRootSignature(m_pSSAOBlurRS.get());
				renderContext.SetComputePipelineState(m_pSSAOBlurPSO.get());

				struct ShaderParameters
				{
					float Dimensions[2];
					uint32 Horizontal;
					float Far;
					float Near;
				} shaderParameters;

				shaderParameters.Horizontal = 1;
				shaderParameters.Dimensions[0] = 1.0f / m_pSSAOTarget->GetWidth();
				shaderParameters.Dimensions[1] = 1.0f / m_pSSAOTarget->GetHeight();
				shaderParameters.Far = m_pCamera->GetFar();
				shaderParameters.Near = m_pCamera->GetNear();

				Texture* pDepth = m_SampleCount == 1 ? m_pDepthStencil.get() : m_pResolvedDepthStencil.get();

				renderContext.SetComputeDynamicConstantBufferView(0, &shaderParameters, sizeof(ShaderParameters));
				renderContext.SetDynamicDescriptor(1, 0, m_pSSAOBlurred->GetUAV());
				renderContext.SetDynamicDescriptor(2, 0, pDepth->GetSRV());
				renderContext.SetDynamicDescriptor(2, 1, m_pSSAOTarget->GetSRV());

				renderContext.Dispatch(Math::DivideAndRoundUp(m_pSSAOBlurred->GetWidth(), 256), m_pSSAOBlurred->GetHeight());

				renderContext.InsertResourceBarrier(m_pSSAOBlurred.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				renderContext.InsertResourceBarrier(m_pSSAOTarget.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

				renderContext.SetDynamicDescriptor(1, 0, m_pSSAOTarget->GetUAV());
				renderContext.SetDynamicDescriptor(2, 0, pDepth->GetSRV());
				renderContext.SetDynamicDescriptor(2, 1, m_pSSAOBlurred->GetSRV());

				shaderParameters.Horizontal = 0;
				renderContext.SetComputeDynamicConstantBufferView(0, &shaderParameters, sizeof(ShaderParameters));
				renderContext.Dispatch(m_pSSAOBlurred->GetWidth(), Math::DivideAndRoundUp(m_pSSAOBlurred->GetHeight(), 256));
			};
		});

	//4. SHADOW MAPPING
	// - Renders the scene depth onto a separate depth buffer from the light's view
	if (m_ShadowCasters > 0)
	{
		graph.AddPass("Shadow Mapping", [&](RGPassBuilder& builder)
			{
				builder.NeverCull();
				return [=](CommandContext& context, const RGPassResources& resources)
				{
					context.InsertResourceBarrier(m_pShadowMap.get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

					context.BeginRenderPass(RenderPassInfo(m_pShadowMap.get(), RenderPassAccess::Clear_Store));
					context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

					for (int i = 0; i < m_ShadowCasters; ++i)
					{
						GPU_PROFILE_SCOPE("Light View", &context);
						const Vector4& shadowOffset = lightData.ShadowMapOffsets[i];
						FloatRect viewport;
						viewport.Left = shadowOffset.x * (float)m_pShadowMap->GetWidth();
						viewport.Top = shadowOffset.y * (float)m_pShadowMap->GetHeight();
						viewport.Right = viewport.Left + shadowOffset.z * (float)m_pShadowMap->GetWidth();
						viewport.Bottom = viewport.Top + shadowOffset.z * (float)m_pShadowMap->GetHeight();
						context.SetViewport(viewport);

						struct PerObjectData
						{
							Matrix WorldViewProjection;
						} ObjectData{};
						context.SetGraphicsRootSignature(m_pShadowsRS.get());

						//Opaque
						{
							GPU_PROFILE_SCOPE("Opaque", &context);
							context.SetGraphicsPipelineState(m_pShadowsOpaquePSO.get());

							for (const Batch& b : m_OpaqueBatches)
							{
								ObjectData.WorldViewProjection = b.WorldMatrix * lightData.LightViewProjections[i];
								context.SetDynamicConstantBufferView(0, &ObjectData, sizeof(PerObjectData));
								b.pMesh->Draw(&context);
							}
						}
						//Transparant
						{
							GPU_PROFILE_SCOPE("Transparant", &context);
							context.SetGraphicsPipelineState(m_pShadowsAlphaPSO.get());

							context.SetDynamicConstantBufferView(0, &ObjectData, sizeof(PerObjectData));
							for (const Batch& b : m_TransparantBatches)
							{
								ObjectData.WorldViewProjection = b.WorldMatrix * lightData.LightViewProjections[i];
								context.SetDynamicConstantBufferView(0, &ObjectData, sizeof(PerObjectData));
								context.SetDynamicDescriptor(1, 0, b.pMaterial->pDiffuseTexture->GetSRV());
								b.pMesh->Draw(&context);
							}
						}
					}
					context.EndRenderPass();
				};
			});
	}

	if (m_RenderPath == RenderPath::Tiled)
	{
		TiledForwardInputResources resources;
		resources.ResolvedDepthBuffer = Data.DepthStencilResolved;
		resources.DepthBuffer = Data.DepthStencil;
		resources.pOpaqueBatches = &m_OpaqueBatches;
		resources.pTransparantBatches = &m_TransparantBatches;
		resources.pRenderTarget = GetCurrentRenderTarget();
		resources.pLightBuffer = m_pLightBuffer.get();
		resources.pCamera = m_pCamera.get();
		resources.pShadowMap = m_pShadowMap.get();
		m_pTiledForward->Execute(graph, resources);
	}
	else if (m_RenderPath == RenderPath::Clustered)
	{
		ClusteredForwardInputResources resources;
		resources.DepthBuffer = Data.DepthStencil;
		resources.pOpaqueBatches = &m_OpaqueBatches;
		resources.pTransparantBatches = &m_TransparantBatches;
		resources.pRenderTarget = GetCurrentRenderTarget();
		resources.pLightBuffer = m_pLightBuffer.get();
		resources.pCamera = m_pCamera.get();
		resources.pAO = m_pSSAOTarget.get();
		m_pClusteredForward->Execute(graph, resources);
	}

	m_pDebugRenderer->Render(graph);

	//7. MSAA Render Target Resolve
	// - We have to resolve a MSAA render target ourselves. Unlike D3D11, this is not done automatically by the API.
	//	Luckily, there's a method that does it for us!
	if (m_SampleCount > 1)
	{
		graph.AddPass("Resolve", [&](RGPassBuilder& builder)
			{
				builder.NeverCull();
				return [=](CommandContext& context, const RGPassResources& resources)
				{
					context.InsertResourceBarrier(GetCurrentRenderTarget(), D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
					context.InsertResourceBarrier(m_pHDRRenderTarget.get(), D3D12_RESOURCE_STATE_RESOLVE_DEST);
					context.ResolveResource(GetCurrentRenderTarget(), 0, m_pHDRRenderTarget.get(), 0, RENDER_TARGET_FORMAT);
				};
			});
	}

	//8. Tonemapping
	{
		bool downscaleTonemapInput = true;
		Texture* pToneMapInput = downscaleTonemapInput ? m_pDownscaledColor.get() : m_pHDRRenderTarget.get();
		RGResourceHandle toneMappingInput = graph.ImportTexture("Tonemap Input", pToneMapInput);

		if (downscaleTonemapInput)
		{
			graph.AddPass("Downsample Color", [&](RGPassBuilder& builder)
				{
					builder.NeverCull();
					toneMappingInput = builder.Write(toneMappingInput);
					return [=](CommandContext& context, const RGPassResources& resources)
					{
						Texture* pToneMapInput = resources.GetTexture(toneMappingInput);
						context.InsertResourceBarrier(pToneMapInput, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
						context.InsertResourceBarrier(m_pHDRRenderTarget.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

						context.SetComputePipelineState(m_pGenerateMipsPSO.get());
						context.SetComputeRootSignature(m_pGenerateMipsRS.get());

						struct DownscaleParameters
						{
							uint32 TargetDimensions[2];
						} Parameters{};
						Parameters.TargetDimensions[0] = pToneMapInput->GetWidth();
						Parameters.TargetDimensions[1] = pToneMapInput->GetHeight();

						context.SetComputeDynamicConstantBufferView(0, &Parameters, sizeof(DownscaleParameters));
						context.SetDynamicDescriptor(1, 0, pToneMapInput->GetUAV());
						context.SetDynamicDescriptor(2, 0, m_pHDRRenderTarget->GetSRV());

						context.Dispatch(
							Math::DivideAndRoundUp(Parameters.TargetDimensions[0], 16), 
							Math::DivideAndRoundUp(Parameters.TargetDimensions[1], 16)
						);
					};
				});
		}

		graph.AddPass("Luminance Histogram", [&](RGPassBuilder& builder)
			{
				builder.NeverCull();
				toneMappingInput = builder.Read(toneMappingInput);
				return [=](CommandContext& context, const RGPassResources& resources)
				{
					Texture* pToneMapInput = resources.GetTexture(toneMappingInput);

					context.InsertResourceBarrier(m_pLuminanceHistogram.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
					context.InsertResourceBarrier(pToneMapInput, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					context.ClearUavUInt(m_pLuminanceHistogram.get(), m_pLuminanceHistogram->GetUAV());

					context.SetComputePipelineState(m_pLuminanceHistogramPSO.get());
					context.SetComputeRootSignature(m_pLuminanceHistogramRS.get());

					struct HistogramParameters
					{
						uint32 Width;
						uint32 Height;
						float MinLogLuminance;
						float OneOverLogLuminanceRange;
					} Parameters;
					Parameters.Width = pToneMapInput->GetWidth();
					Parameters.Height = pToneMapInput->GetHeight();
					Parameters.MinLogLuminance = g_MinLogLuminance;
					Parameters.OneOverLogLuminanceRange = 1.0f / (g_MaxLogLuminance - g_MinLogLuminance);

					context.SetComputeDynamicConstantBufferView(0, &Parameters, sizeof(HistogramParameters));
					context.SetDynamicDescriptor(1, 0, m_pLuminanceHistogram->GetUAV());
					context.SetDynamicDescriptor(2, 0, pToneMapInput->GetSRV());

					context.Dispatch(
						Math::DivideAndRoundUp(pToneMapInput->GetWidth(), 16),
						Math::DivideAndRoundUp(pToneMapInput->GetHeight(), 16)
					);
				};
			});

		graph.AddPass("Average Luminance", [&](RGPassBuilder& builder)
			{
				builder.NeverCull();
				return [=](CommandContext& context, const RGPassResources& resources)
				{
					context.InsertResourceBarrier(m_pLuminanceHistogram.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
					context.InsertResourceBarrier(m_pAverageLuminance.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

					context.SetComputePipelineState(m_pAverageLuminancePSO.get());
					context.SetComputeRootSignature(m_pAverageLuminanceRS.get());

					struct AverageParameters
					{
						int32 PixelCount;
						float MinLogLuminance;
						float LogLuminanceRange;
						float TimeDelta;
						float Tau;
					} Parameters;

					Parameters.PixelCount = pToneMapInput->GetWidth() * pToneMapInput->GetHeight();
					Parameters.MinLogLuminance = g_MinLogLuminance;
					Parameters.LogLuminanceRange = g_MaxLogLuminance - g_MinLogLuminance;
					Parameters.TimeDelta = GameTimer::DeltaTime();
					Parameters.Tau = g_Tau;

					context.SetComputeDynamicConstantBufferView(0, &Parameters, sizeof(AverageParameters));
					context.SetDynamicDescriptor(1, 0, m_pAverageLuminance->GetUAV());
					context.SetDynamicDescriptor(2, 0, m_pLuminanceHistogram->GetSRV());

					context.Dispatch(1);
				};
			});

		graph.AddPass("Tonemap", [&](RGPassBuilder& builder)
			{
				builder.NeverCull();
				return [=](CommandContext& context, const RGPassResources& resources)
				{
					context.InsertResourceBarrier(GetCurrentBackbuffer(), D3D12_RESOURCE_STATE_RENDER_TARGET);
					context.InsertResourceBarrier(m_pAverageLuminance.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
					context.InsertResourceBarrier(m_pHDRRenderTarget.get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

					context.SetGraphicsPipelineState(m_pToneMapPSO.get());
					context.SetGraphicsRootSignature(m_pToneMapRS.get());
					context.SetViewport(FloatRect(0, 0, (float)m_WindowWidth, (float)m_WindowHeight));
					context.BeginRenderPass(RenderPassInfo(GetCurrentBackbuffer(), RenderPassAccess::Clear_Store, nullptr, RenderPassAccess::NoAccess));

					context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
					context.SetDynamicConstantBufferView(0, &g_WhitePoint, sizeof(float));
					context.SetDynamicDescriptor(1, 0, m_pHDRRenderTarget->GetSRV());
					context.SetDynamicDescriptor(1, 1, m_pAverageLuminance->GetSRV());
					context.Draw(0, 3);
					context.EndRenderPass();
				};
			});
	}

	//9. UI
	// - ImGui render, pretty straight forward
	{
		m_pImGuiRenderer->Render(graph, GetCurrentBackbuffer());
	}

	graph.AddPass("Temp Barriers", [&](RGPassBuilder& builder)
		{
			builder.NeverCull();
			return [=](CommandContext& context, const RGPassResources& resources)
			{
				context.InsertResourceBarrier(GetCurrentRenderTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET);
				context.InsertResourceBarrier(GetCurrentBackbuffer(), D3D12_RESOURCE_STATE_PRESENT);
			};
		});

	graph.Compile();
	if (gDumpRenderGraph)
	{
		graph.DumpGraphMermaid("graph.html");
		gDumpRenderGraph = false;
	}
	nextFenceValue = graph.Execute(this);

	//10. PRESENT
	//	- Set fence for the currently queued frame
	//	- Present the frame buffer
	//	- Wait for the next frame to be finished to start queueing work for it
	EndFrame(nextFenceValue);
}

void Graphics::Shutdown()
{
	// Wait for the GPU to be done with all resources.
	IdleGPU();
	m_pSwapchain->SetFullscreenState(false, nullptr);
}

void Graphics::BeginFrame()
{
	m_pImGuiRenderer->NewFrame();
}

void Graphics::EndFrame(uint64 fenceValue)
{
	//This always gets me confused!
	//The 'm_CurrentBackBufferIndex' is the frame that just got queued so we set the fence value on that frame
	//We present and request the new backbuffer index and wait for that one to finish on the GPU before starting to queue work for that frame.

	++m_Frame;
	Profiler::Instance()->BeginReadback(m_Frame);
	m_FenceValues[m_CurrentBackBufferIndex] = fenceValue;
	m_pSwapchain->Present(1, 0);
	m_CurrentBackBufferIndex = m_pSwapchain->GetCurrentBackBufferIndex();
	WaitForFence(m_FenceValues[m_CurrentBackBufferIndex]);
	Profiler::Instance()->EndReadBack(m_Frame);
	m_pDebugRenderer->EndFrame();
}

void Graphics::InitD3D()
{
	E_LOG(Info, "Graphics::InitD3D()");
	UINT dxgiFactoryFlags = 0;

#if D3D_VALIDATION
	//Enable debug
	ComPtr<ID3D12Debug> pDebugController;
	HR(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController)));
	pDebugController->EnableDebugLayer();

#if GPU_VALIDATION
	ComPtr<ID3D12Debug1> pDebugController1;
	HR(pDebugController->QueryInterface(IID_PPV_ARGS(&pDebugController1)));
	pDebugController1->SetEnableGPUBasedValidation(true);
#endif

	// Enable additional debug layers.
	dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
#endif

	//Create the factory
	ComPtr<IDXGIFactory6> pFactory;
	HR(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&pFactory)));

	ComPtr<IDXGIAdapter4> pAdapter;
	uint32 adapterIndex = 0;
	E_LOG(Info, "Adapters:");
	DXGI_GPU_PREFERENCE gpuPreference = DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
	while (pFactory->EnumAdapterByGpuPreference(adapterIndex++, gpuPreference, IID_PPV_ARGS(pAdapter.ReleaseAndGetAddressOf())) == S_OK)
	{
		DXGI_ADAPTER_DESC3 desc;
		pAdapter->GetDesc3(&desc);
		char name[256];
		ToMultibyte(desc.Description, name, 256);
		E_LOG(Info, "\t%s", name);
	}
	pFactory->EnumAdapterByGpuPreference(0, gpuPreference, IID_PPV_ARGS(pAdapter.GetAddressOf()));
	DXGI_ADAPTER_DESC3 desc;
	pAdapter->GetDesc3(&desc);
	char name[256];
	ToMultibyte(desc.Description, name, 256);
	E_LOG(Info, "Using %s", name);

	//Create the device
	HR(D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_pDevice)));
	pAdapter.Reset();

#if D3D_VALIDATION
	ID3D12InfoQueue* pInfoQueue = nullptr;
	if (HR(m_pDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue))))
	{
		// Suppress whole categories of messages
		//D3D12_MESSAGE_CATEGORY Categories[] = {};

		// Suppress messages based on their severity level
		D3D12_MESSAGE_SEVERITY Severities[] =
		{
			D3D12_MESSAGE_SEVERITY_INFO
		};

		// Suppress individual messages by their ID
		D3D12_MESSAGE_ID DenyIds[] =
		{
			// This occurs when there are uninitialized descriptors in a descriptor table, even when a
			// shader does not access the missing descriptors.  I find this is common when switching
			// shader permutations and not wanting to change much code to reorder resources.
			D3D12_MESSAGE_ID_INVALID_DESCRIPTOR_HANDLE,
		};

		D3D12_INFO_QUEUE_FILTER NewFilter = {};
		//NewFilter.DenyList.NumCategories = _countof(Categories);
		//NewFilter.DenyList.pCategoryList = Categories;
		NewFilter.DenyList.NumSeverities = _countof(Severities);
		NewFilter.DenyList.pSeverityList = Severities;
		NewFilter.DenyList.NumIDs = _countof(DenyIds);
		NewFilter.DenyList.pIDList = DenyIds;

#if 1
		HR(pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true));
#endif
		pInfoQueue->PushStorageFilter(&NewFilter);
		pInfoQueue->Release();
	}
#endif

	D3D12_FEATURE_DATA_D3D12_OPTIONS5 featureSupport{};
	if (m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &featureSupport, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5)) == S_OK)
	{
		m_RenderPassTier = featureSupport.RenderPassesTier;
		m_RayTracingTier = featureSupport.RaytracingTier;
	}

	//Check MSAA support
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels;
	qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	qualityLevels.Format = RENDER_TARGET_FORMAT;
	qualityLevels.NumQualityLevels = 0;
	qualityLevels.SampleCount = m_SampleCount;
	HR(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels)));
	m_SampleQuality = qualityLevels.NumQualityLevels - 1;

	//Create all the required command queues
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_DIRECT] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_COMPUTE] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COMPUTE);
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_COPY] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COPY);
	//m_CommandQueues[D3D12_COMMAND_LIST_TYPE_BUNDLE] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_BUNDLE);

	assert(m_DescriptorHeaps.size() == D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 128);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 128);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_DSV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64);

	m_pDynamicAllocationManager = std::make_unique<DynamicAllocationManager>(this);
	Profiler::Instance()->Initialize(this);

	m_pSwapchain.Reset();

	DXGI_SWAP_CHAIN_DESC1 swapchainDesc = {};
	swapchainDesc.Width = m_WindowWidth;
	swapchainDesc.Height = m_WindowHeight;
	swapchainDesc.Format = SWAPCHAIN_FORMAT;
	swapchainDesc.SampleDesc.Count = 1;
	swapchainDesc.SampleDesc.Quality = 0;
	swapchainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapchainDesc.BufferCount = FRAME_COUNT;
	swapchainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
	swapchainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
	swapchainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
	swapchainDesc.Stereo = false;
	ComPtr<IDXGISwapChain1> swapChain;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc{};
	fsDesc.RefreshRate.Denominator = 60;
	fsDesc.RefreshRate.Numerator = 1;
	fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	fsDesc.Windowed = true;
	HR(pFactory->CreateSwapChainForHwnd(
		m_CommandQueues[D3D12_COMMAND_LIST_TYPE_DIRECT]->GetCommandQueue(), 
		m_pWindow, 
		&swapchainDesc, 
		&fsDesc, 
		nullptr, 
		swapChain.GetAddressOf()));

	swapChain.As(&m_pSwapchain);

	//Create the textures but don't create the resources themselves yet.
	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		m_Backbuffers[i] = std::make_unique<Texture>(this, "Render Target");
	}
	m_pDepthStencil = std::make_unique<Texture>(this, "Depth Stencil");

	if (m_SampleCount > 1)
	{
		m_pResolvedDepthStencil = std::make_unique<Texture>(this, "Resolved Depth Stencil");
		m_pMultiSampleRenderTarget = std::make_unique<Texture>(this, "MSAA Target");
	}
	m_pHDRRenderTarget = std::make_unique<Texture>(this, "HDR Target");
	m_pDownscaledColor = std::make_unique<Texture>(this, "Downscaled HDR Target");
	m_pMSAANormals = std::make_unique<Texture>(this, "MSAA Normals");
	m_pNormals = std::make_unique<Texture>(this, "Normals");
	m_pSSAOTarget = std::make_unique<Texture>(this, "SSAO");
	m_pSSAOBlurred = std::make_unique<Texture>(this, "SSAO Blurred");

	m_pClusteredForward = std::make_unique<ClusteredForward>(this);
	m_pTiledForward = std::make_unique<TiledForward>(this);
	m_pImGuiRenderer = std::make_unique<ImGuiRenderer>(this);
	m_pImGuiRenderer->AddUpdateCallback(ImGuiCallbackDelegate::CreateRaw(this, &Graphics::UpdateImGui));

	OnResize(m_WindowWidth, m_WindowHeight);

	m_pGraphAllocator = std::make_unique<RGResourceAllocator>(this);
	m_pDebugRenderer = std::make_unique<DebugRenderer>(this);
	m_pDebugRenderer->SetCamera(m_pCamera.get());
}

void Graphics::OnResize(int width, int height)
{
	E_LOG(Info, "Viewport resized: %dx%d", width, height);
	m_WindowWidth = width;
	m_WindowHeight = height;

	IdleGPU();

	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		m_Backbuffers[i]->Release();
	}
	m_pDepthStencil->Release();

	//Resize the buffers
	HR(m_pSwapchain->ResizeBuffers(
		FRAME_COUNT, 
		m_WindowWidth, 
		m_WindowHeight, 
		SWAPCHAIN_FORMAT,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

	m_CurrentBackBufferIndex = 0;

	//Recreate the render target views
	for (int i = 0; i < FRAME_COUNT; ++i)
	{
		ID3D12Resource* pResource = nullptr;
		HR(m_pSwapchain->GetBuffer(i, IID_PPV_ARGS(&pResource)));
		m_Backbuffers[i]->CreateForSwapchain(pResource);
	}
	if (m_SampleCount > 1)
	{
		m_pDepthStencil->Create(TextureDesc::CreateDepth(width, height, DEPTH_STENCIL_FORMAT, TextureFlag::DepthStencil | TextureFlag::ShaderResource, m_SampleCount, ClearBinding(0.0f, 0)));
		m_pResolvedDepthStencil->Create(TextureDesc::Create2D(width, height, DXGI_FORMAT_R32_FLOAT, TextureFlag::ShaderResource | TextureFlag::UnorderedAccess));
		m_pMultiSampleRenderTarget->Create(TextureDesc::CreateRenderTarget(width, height, RENDER_TARGET_FORMAT, TextureFlag::RenderTarget, m_SampleCount, ClearBinding(Color(0, 0, 0, 0))));
	}
	else
	{
		m_pDepthStencil->Create(TextureDesc::CreateDepth(width, height, DEPTH_STENCIL_FORMAT, TextureFlag::DepthStencil | TextureFlag::ShaderResource, m_SampleCount, ClearBinding(0.0f, 0)));
	}
	m_pHDRRenderTarget->Create(TextureDesc::CreateRenderTarget(width, height, RENDER_TARGET_FORMAT, TextureFlag::ShaderResource | TextureFlag::RenderTarget));
	m_pDownscaledColor->Create(TextureDesc::Create2D(Math::DivideAndRoundUp(width, 4), Math::DivideAndRoundUp(height, 4), RENDER_TARGET_FORMAT, TextureFlag::ShaderResource | TextureFlag::UnorderedAccess));

	m_pMSAANormals->Create(TextureDesc::CreateRenderTarget(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureFlag::RenderTarget, m_SampleCount));
	m_pNormals->Create(TextureDesc::Create2D(width, height, DXGI_FORMAT_R32G32B32A32_FLOAT, TextureFlag::ShaderResource));
	m_pSSAOTarget->Create(TextureDesc::Create2D(Math::DivideAndRoundUp(width, 2), Math::DivideAndRoundUp(height, 2), DXGI_FORMAT_R8_UNORM, TextureFlag::UnorderedAccess | TextureFlag::ShaderResource));
	m_pSSAOBlurred->Create(TextureDesc::Create2D(Math::DivideAndRoundUp(width, 2), Math::DivideAndRoundUp(height, 2), DXGI_FORMAT_R8_UNORM, TextureFlag::UnorderedAccess | TextureFlag::ShaderResource));

	m_pCamera->SetDirty();

	m_pClusteredForward->OnSwapchainCreated(width, height);
	m_pTiledForward->OnSwapchainCreated(width, height);
}

void Graphics::InitializeAssets()
{
	m_pLightBuffer = std::make_unique<Buffer>(this, "Lights");

	//Input layout
	//UNIVERSAL
	D3D12_INPUT_ELEMENT_DESC inputElements[] = {
		D3D12_INPUT_ELEMENT_DESC{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 44, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	D3D12_INPUT_ELEMENT_DESC depthOnlyInputElements[] = {
		D3D12_INPUT_ELEMENT_DESC{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		D3D12_INPUT_ELEMENT_DESC{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
	};

	//Shadow mapping
	//Vertex shader-only pass that writes to the depth buffer using the light matrix
	{
		//Opaque
		{
			Shader vertexShader("Resources/Shaders/DepthOnly.hlsl", Shader::Type::VertexShader, "VSMain");
			Shader alphaVertexShader("Resources/Shaders/DepthOnly.hlsl", Shader::Type::VertexShader, "VSMain", { "ALPHA_BLEND" });
			Shader alphaPixelShader("Resources/Shaders/DepthOnly.hlsl", Shader::Type::PixelShader, "PSMain", { "ALPHA_BLEND" });

			//Rootsignature
			m_pShadowsRS = std::make_unique<RootSignature>();
			m_pShadowsRS->FinalizeFromShader("Shadow Mapping (Opaque)", vertexShader, m_pDevice.Get());

			//Pipeline state
			m_pShadowsOpaquePSO = std::make_unique<GraphicsPipelineState>();
			m_pShadowsOpaquePSO->SetInputLayout(depthOnlyInputElements, sizeof(depthOnlyInputElements) / sizeof(depthOnlyInputElements[0]));
			m_pShadowsOpaquePSO->SetRootSignature(m_pShadowsRS->GetRootSignature());
			m_pShadowsOpaquePSO->SetVertexShader(vertexShader.GetByteCode(), vertexShader.GetByteCodeSize());
			m_pShadowsOpaquePSO->SetRenderTargetFormats(nullptr, 0, DEPTH_STENCIL_SHADOW_FORMAT, 1, 0);
			m_pShadowsOpaquePSO->SetCullMode(D3D12_CULL_MODE_NONE);
			m_pShadowsOpaquePSO->SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
			m_pShadowsOpaquePSO->SetDepthBias(-1, -5.0f, -4.0f);
			m_pShadowsOpaquePSO->Finalize("Shadow Mapping (Opaque) Pipeline", m_pDevice.Get());

			m_pShadowsAlphaPSO = std::make_unique<GraphicsPipelineState>(*m_pShadowsOpaquePSO);
			m_pShadowsAlphaPSO->SetVertexShader(alphaVertexShader.GetByteCode(), alphaVertexShader.GetByteCodeSize());
			m_pShadowsAlphaPSO->SetPixelShader(alphaPixelShader.GetByteCode(), alphaPixelShader.GetByteCodeSize());
			m_pShadowsAlphaPSO->Finalize("Shadow Mapping (Alpha) Pipeline", m_pDevice.Get());
		}

		m_pShadowMap = std::make_unique<Texture>(this, "Shadow Map");
		m_pShadowMap->Create(TextureDesc::CreateDepth(SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, DEPTH_STENCIL_SHADOW_FORMAT, TextureFlag::DepthStencil | TextureFlag::ShaderResource, 1, ClearBinding(0.0f, 0)));
	}

	//Depth prepass
	//Simple vertex shader to fill the depth buffer to optimize later passes
	{
		Shader vertexShader("Resources/Shaders/Prepass.hlsl", Shader::Type::VertexShader, "VSMain");
		Shader pixelShader("Resources/Shaders/Prepass.hlsl", Shader::Type::PixelShader, "PSMain");

		//Rootsignature
		m_pDepthPrepassRS = std::make_unique<RootSignature>();
		m_pDepthPrepassRS->FinalizeFromShader("Depth Prepass", vertexShader, m_pDevice.Get());

		//Pipeline state
		m_pDepthPrepassPSO = std::make_unique<GraphicsPipelineState>();
		m_pDepthPrepassPSO->SetInputLayout(inputElements, sizeof(inputElements) / sizeof(inputElements[0]));
		m_pDepthPrepassPSO->SetRootSignature(m_pDepthPrepassRS->GetRootSignature());
		m_pDepthPrepassPSO->SetVertexShader(vertexShader.GetByteCode(), vertexShader.GetByteCodeSize());
		m_pDepthPrepassPSO->SetPixelShader(pixelShader.GetByteCode(), pixelShader.GetByteCodeSize());
		m_pDepthPrepassPSO->SetRenderTargetFormat(DXGI_FORMAT_R32G32B32A32_FLOAT, DEPTH_STENCIL_FORMAT, m_SampleCount, m_SampleQuality);
		m_pDepthPrepassPSO->SetDepthTest(D3D12_COMPARISON_FUNC_GREATER);
		m_pDepthPrepassPSO->Finalize("Depth Prepass Pipeline", m_pDevice.Get());
	}

	//Luminance Historgram
	{
		Shader computeShader("Resources/Shaders/LuminanceHistogram.hlsl", Shader::Type::ComputeShader, "CSMain");

		//Rootsignature
		m_pLuminanceHistogramRS = std::make_unique<RootSignature>();
		m_pLuminanceHistogramRS->FinalizeFromShader("Luminance Historgram", computeShader, m_pDevice.Get());

		//Pipeline state
		m_pLuminanceHistogramPSO = std::make_unique<ComputePipelineState>();
		m_pLuminanceHistogramPSO->SetRootSignature(m_pLuminanceHistogramRS->GetRootSignature());
		m_pLuminanceHistogramPSO->SetComputeShader(computeShader.GetByteCode(), computeShader.GetByteCodeSize());
		m_pLuminanceHistogramPSO->Finalize("Luminance Historgram", m_pDevice.Get());

		m_pLuminanceHistogram = std::make_unique<Buffer>(this);
		m_pLuminanceHistogram->Create(BufferDesc::CreateByteAddress(sizeof(uint32) * 256));
		m_pAverageLuminance = std::make_unique<Texture>(this);
		m_pAverageLuminance->Create(TextureDesc::Create2D(1, 1, DXGI_FORMAT_R32_FLOAT, TextureFlag::UnorderedAccess | TextureFlag::ShaderResource));
	}

	//Average Luminance
	{
		Shader computeShader("Resources/Shaders/AverageLuminance.hlsl", Shader::Type::ComputeShader, "CSMain");

		//Rootsignature
		m_pAverageLuminanceRS = std::make_unique<RootSignature>();
		m_pAverageLuminanceRS->FinalizeFromShader("Average Luminance", computeShader, m_pDevice.Get());

		//Pipeline state
		m_pAverageLuminancePSO = std::make_unique<ComputePipelineState>();
		m_pAverageLuminancePSO->SetRootSignature(m_pAverageLuminanceRS->GetRootSignature());
		m_pAverageLuminancePSO->SetComputeShader(computeShader.GetByteCode(), computeShader.GetByteCodeSize());
		m_pAverageLuminancePSO->Finalize("Average Luminance", m_pDevice.Get());
	}

	//Tonemapping
	{
		Shader vertexShader("Resources/Shaders/Tonemapping.hlsl", Shader::Type::VertexShader, "VSMain");
		Shader pixelShader("Resources/Shaders/Tonemapping.hlsl", Shader::Type::PixelShader, "PSMain");

		//Rootsignature
		m_pToneMapRS = std::make_unique<RootSignature>();
		m_pToneMapRS->FinalizeFromShader("Tonemapping", vertexShader, m_pDevice.Get());

		//Pipeline state
		m_pToneMapPSO = std::make_unique<GraphicsPipelineState>();
		m_pToneMapPSO->SetDepthEnabled(false);
		m_pToneMapPSO->SetDepthWrite(false);
		m_pToneMapPSO->SetRootSignature(m_pToneMapRS->GetRootSignature());
		m_pToneMapPSO->SetVertexShader(vertexShader.GetByteCode(), vertexShader.GetByteCodeSize());
		m_pToneMapPSO->SetPixelShader(pixelShader.GetByteCode(), pixelShader.GetByteCodeSize());
		m_pToneMapPSO->SetRenderTargetFormat(SWAPCHAIN_FORMAT, DEPTH_STENCIL_FORMAT, 1, 0);
		m_pToneMapPSO->Finalize("Tone mapping Pipeline", m_pDevice.Get());
	}

	//Depth resolve
	//Resolves a multisampled depth buffer to a normal depth buffer
	//Only required when the sample count > 1
	if(m_SampleCount > 1)
	{
		Shader computeShader("Resources/Shaders/ResolveDepth.hlsl", Shader::Type::ComputeShader, "CSMain", { "DEPTH_RESOLVE_MIN" });

		m_pResolveDepthRS = std::make_unique<RootSignature>();
		m_pResolveDepthRS->FinalizeFromShader("Depth Resolve", computeShader, m_pDevice.Get());

		m_pResolveDepthPSO = std::make_unique<ComputePipelineState>();
		m_pResolveDepthPSO->SetComputeShader(computeShader.GetByteCode(), computeShader.GetByteCodeSize());
		m_pResolveDepthPSO->SetRootSignature(m_pResolveDepthRS->GetRootSignature());
		m_pResolveDepthPSO->Finalize("Resolve Depth Pipeline", m_pDevice.Get());
	}

	//Mip generation
	{
		Shader computeShader("Resources/Shaders/GenerateMips.hlsl", Shader::Type::ComputeShader, "CSMain");

		m_pGenerateMipsRS = std::make_unique<RootSignature>();
		m_pGenerateMipsRS->FinalizeFromShader("Generate Mips", computeShader, m_pDevice.Get());

		m_pGenerateMipsPSO = std::make_unique<ComputePipelineState>();
		m_pGenerateMipsPSO->SetComputeShader(computeShader.GetByteCode(), computeShader.GetByteCodeSize());
		m_pGenerateMipsPSO->SetRootSignature(m_pGenerateMipsRS->GetRootSignature());
		m_pGenerateMipsPSO->Finalize("Generate Mips PSO", m_pDevice.Get());
	}

	//SSAO
	{
		Shader computeShader("Resources/Shaders/SSAO.hlsl", Shader::Type::ComputeShader, "CSMain");

		m_pSSAORS = std::make_unique<RootSignature>();
		m_pSSAORS->FinalizeFromShader("SSAO", computeShader, m_pDevice.Get());

		m_pSSAOPSO = std::make_unique<ComputePipelineState>();
		m_pSSAOPSO->SetComputeShader(computeShader.GetByteCode(), computeShader.GetByteCodeSize());
		m_pSSAOPSO->SetRootSignature(m_pSSAORS->GetRootSignature());
		m_pSSAOPSO->Finalize("SSAO PSO", m_pDevice.Get());
	}

	//SSAO
	{
		Shader computeShader("Resources/Shaders/SSAOBlur.hlsl", Shader::Type::ComputeShader, "CSMain");

		m_pSSAOBlurRS = std::make_unique<RootSignature>();
		m_pSSAOBlurRS->FinalizeFromShader("SSAO Blur", computeShader, m_pDevice.Get());

		m_pSSAOBlurPSO = std::make_unique<ComputePipelineState>();
		m_pSSAOBlurPSO->SetComputeShader(computeShader.GetByteCode(), computeShader.GetByteCodeSize());
		m_pSSAOBlurPSO->SetRootSignature(m_pSSAOBlurRS->GetRootSignature());
		m_pSSAOBlurPSO->Finalize("SSAO Blur PSO", m_pDevice.Get());
	}


	CommandContext* pContext = AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_COPY);

	//Geometry
	{
		m_pMesh = std::make_unique<Mesh>();
		m_pMesh->Load("Resources/sponza/sponza.dae", this, pContext);

		for (int i = 0; i < m_pMesh->GetMeshCount(); ++i)
		{
			Batch b;
			b.Bounds = m_pMesh->GetMesh(i)->GetBounds();
			b.pMesh = m_pMesh->GetMesh(i);
			b.pMaterial = &m_pMesh->GetMaterial(b.pMesh->GetMaterialId());
			b.WorldMatrix = Matrix::Identity;
			if (b.pMaterial->IsTransparent)
			{
				m_TransparantBatches.push_back(b);
			}
			else
			{
				m_OpaqueBatches.push_back(b);
			}
		}
	}

	ComPtr<ID3D12Device5> pDevice;
	if (m_RayTracingTier != D3D12_RAYTRACING_TIER_NOT_SUPPORTED && m_pDevice.As(&pDevice) == S_OK)
	{
		PIX_CAPTURE_SCOPE();

		CommandContext* pContext = AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
		ID3D12GraphicsCommandList* pC = pContext->GetCommandList();
		ComPtr<ID3D12GraphicsCommandList4> pCmd;
		pC->QueryInterface(IID_PPV_ARGS(pCmd.GetAddressOf()));

		std::unique_ptr<Buffer> pBLAS, pTLAS;
		std::unique_ptr<Buffer> pBLASScratch;
		std::unique_ptr<Buffer> pTLASScratch, pDescriptorsBuffer;

		ComPtr<ID3D12StateObject> pPipeline;
		ComPtr<ID3D12StateObjectProperties> pPipelineProperties;

		std::unique_ptr<RootSignature> pRayGenSignature = std::make_unique<RootSignature>();
		std::unique_ptr<RootSignature> pHitSignature = std::make_unique<RootSignature>();
		std::unique_ptr<RootSignature> pMissSignature = std::make_unique<RootSignature>();
		std::unique_ptr<RootSignature> pDummySignature = std::make_unique<RootSignature>();

		std::unique_ptr<Texture> pOutputTexture = std::make_unique<Texture>(this, "Ray Tracing Output");
		UnorderedAccessView* pOutputRawUAV = nullptr;

		std::unique_ptr<Buffer> pShaderBindingTable = std::make_unique<Buffer>(this, "Shader Binding Table");

		std::unique_ptr<OnlineDescriptorAllocator> pDescriptorAllocator = std::make_unique<OnlineDescriptorAllocator>(this, pContext, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		DescriptorHandle uavHandle = pDescriptorAllocator->AllocateTransientDescriptor(2);
		DescriptorHandle srvHandle = uavHandle + pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		std::unique_ptr<Buffer> pVertexBuffer = std::make_unique<Buffer>(this, "Vertex Buffer");
		Vector3 positions[] = {
			{ 0.0f, 0.25f, 0.0f },
			{ 0.25f, -0.25f, 0.0f },
			{ -0.25f, -0.25f, 0.0f },
		};
		pVertexBuffer->Create(BufferDesc::CreateVertexBuffer(3, sizeof(Vector3)));
		pVertexBuffer->SetData(pContext, positions, ARRAYSIZE(positions));

		//Bottom Level Acceleration Structure
		{
			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			geometryDesc.Triangles.IndexBuffer = 0;
			geometryDesc.Triangles.IndexCount = 0;
			geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_UNKNOWN;
			geometryDesc.Triangles.Transform3x4 = 0;
			geometryDesc.Triangles.VertexBuffer.StartAddress = pVertexBuffer->GetGpuHandle();
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = pVertexBuffer->GetDesc().ElementSize;
			geometryDesc.Triangles.VertexCount = pVertexBuffer->GetDesc().ElementCount;
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildInfo{};
			prebuildInfo.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			prebuildInfo.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
			prebuildInfo.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			prebuildInfo.NumDescs = 1;
			prebuildInfo.pGeometryDescs = &geometryDesc;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			pDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfo, &info);

			pBLASScratch = std::make_unique<Buffer>(this, "BLAS Scratch Buffer");
			pBLASScratch->Create(BufferDesc::CreateByteAddress(Math::AlignUp<int>(info.ScratchDataSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), BufferFlag::UnorderedAccess));
			pBLAS = std::make_unique<Buffer>(this, "BLAS");
			pBLAS->Create(BufferDesc::CreateAccelerationStructure(Math::AlignUp<int>(info.ResultDataMaxSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), BufferFlag::UnorderedAccess));

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc{};
			asDesc.Inputs = prebuildInfo;
			asDesc.DestAccelerationStructureData = pBLAS->GetGpuHandle();
			asDesc.ScratchAccelerationStructureData = pBLASScratch->GetGpuHandle();
			asDesc.SourceAccelerationStructureData = 0;

			pCmd->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);
		}
		//Top Level Acceleration Structure
		{
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildInfo{};
			prebuildInfo.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			prebuildInfo.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
			prebuildInfo.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			prebuildInfo.NumDescs = 1;

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			pDevice->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfo, &info);

			pTLASScratch = std::make_unique<Buffer>(this, "TLAS Scratch");
			pTLASScratch->Create(BufferDesc::CreateByteAddress(Math::AlignUp<int>(info.ScratchDataSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), BufferFlag::None));
			pTLAS = std::make_unique<Buffer>(this, "TLAS");
			pTLAS->Create(BufferDesc::CreateAccelerationStructure(Math::AlignUp<int>(info.ResultDataMaxSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)));
			pDescriptorsBuffer = std::make_unique<Buffer>(this, "Descriptors Buffer");
			pDescriptorsBuffer->Create(BufferDesc::CreateVertexBuffer(1, sizeof(D3D12_RAYTRACING_INSTANCE_DESC), BufferFlag::Upload));

			D3D12_RAYTRACING_INSTANCE_DESC* pInstanceDesc = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(pDescriptorsBuffer->Map());
			pInstanceDesc->AccelerationStructure = pBLAS->GetGpuHandle();
			pInstanceDesc->Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
			pInstanceDesc->InstanceContributionToHitGroupIndex = 0;
			pInstanceDesc->InstanceID = 0;
			pInstanceDesc->InstanceMask = 0xFF;
			memcpy(pInstanceDesc->Transform, &Matrix::Identity, sizeof(Matrix));
			pDescriptorsBuffer->Unmap();

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc{};
			asDesc.DestAccelerationStructureData = pTLAS->GetGpuHandle();
			asDesc.ScratchAccelerationStructureData = pTLASScratch->GetGpuHandle();
			asDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			asDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
			asDesc.Inputs.InstanceDescs = pDescriptorsBuffer->GetGpuHandle();
			asDesc.Inputs.NumDescs = 1;
			asDesc.SourceAccelerationStructureData = 0;

			pCmd->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);
		}
		//Raytracing Pipeline
		{
			pRayGenSignature->SetDescriptorTableSimple(0, 0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, D3D12_SHADER_VISIBILITY_ALL);
			pRayGenSignature->SetDescriptorTableSimple(1, 0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, D3D12_SHADER_VISIBILITY_ALL);
			pRayGenSignature->Finalize("Ray Gen RS", pDevice.Get(), D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);
			pHitSignature->Finalize("Hit RS", pDevice.Get(), D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);
			pMissSignature->Finalize("Hit RS", pDevice.Get(), D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE);
			pDummySignature->Finalize("Dummy Global RS", pDevice.Get(), D3D12_ROOT_SIGNATURE_FLAG_NONE);

			ShaderLibrary rayGenShader("Resources/RayTracingShaders/RayGen.hlsl");
			ShaderLibrary hitShader("Resources/RayTracingShaders/Hit.hlsl");
			ShaderLibrary missShader("Resources/RayTracingShaders/Miss.hlsl");

			CD3DX12_STATE_OBJECT_DESC desc(D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE);
			{
				CD3DX12_DXIL_LIBRARY_SUBOBJECT* pRayGenDesc = desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
				pRayGenDesc->SetDXILLibrary(&CD3DX12_SHADER_BYTECODE(rayGenShader.GetByteCode(), rayGenShader.GetByteCodeSize()));
				pRayGenDesc->DefineExport(L"RayGen");
				CD3DX12_DXIL_LIBRARY_SUBOBJECT* pHitDesc = desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
				pHitDesc->SetDXILLibrary(&CD3DX12_SHADER_BYTECODE(hitShader.GetByteCode(), hitShader.GetByteCodeSize()));
				pHitDesc->DefineExport(L"ClosestHit");
				CD3DX12_DXIL_LIBRARY_SUBOBJECT* pMissDesc = desc.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
				pMissDesc->SetDXILLibrary(&CD3DX12_SHADER_BYTECODE(missShader.GetByteCode(), missShader.GetByteCodeSize()));
				pMissDesc->DefineExport(L"Miss");
			}
			{
				CD3DX12_HIT_GROUP_SUBOBJECT* pHitGroupDesc = desc.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
				pHitGroupDesc->SetHitGroupExport(L"HitGroup");
				pHitGroupDesc->SetClosestHitShaderImport(L"ClosestHit");
			}
			{
				CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT* pRayGenRs = desc.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
				pRayGenRs->SetRootSignature(pRayGenSignature->GetRootSignature());
				CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT* pRayGenAssociation = desc.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
				pRayGenAssociation->AddExport(L"RayGen");
				pRayGenAssociation->SetSubobjectToAssociate(*pRayGenRs);

				CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT* pMissRs = desc.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
				pMissRs->SetRootSignature(pMissSignature->GetRootSignature());
				CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT* pMissAssociation = desc.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
				pMissAssociation->AddExport(L"Miss");
				pMissAssociation->SetSubobjectToAssociate(*pMissRs);

				CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT* pHitRs = desc.CreateSubobject<CD3DX12_LOCAL_ROOT_SIGNATURE_SUBOBJECT>();
				pHitRs->SetRootSignature(pHitSignature->GetRootSignature());
				CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT* pHitAssociation = desc.CreateSubobject<CD3DX12_SUBOBJECT_TO_EXPORTS_ASSOCIATION_SUBOBJECT>();
				pHitAssociation->AddExport(L"HitGroup");
				pHitAssociation->SetSubobjectToAssociate(*pHitRs);
			}
			{
				CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT* pRtConfig = desc.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
				pRtConfig->Config(4 * sizeof(float), 2 * sizeof(float));

				CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT* pRtPipelineConfig = desc.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
				pRtPipelineConfig->Config(1);

				CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT* pGlobalRs = desc.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
				pGlobalRs->SetRootSignature(pDummySignature->GetRootSignature());
			}
			D3D12_STATE_OBJECT_DESC stateObject = *desc;
			HR(pDevice->CreateStateObject(&stateObject, IID_PPV_ARGS(pPipeline.GetAddressOf())));
			HR(pPipeline->QueryInterface(IID_PPV_ARGS(pPipelineProperties.GetAddressOf())));
		}
		//Output texture
		{
			pOutputTexture->Create(TextureDesc::Create2D(m_WindowWidth, m_WindowHeight, DXGI_FORMAT_R8G8B8A8_UNORM, TextureFlag::UnorderedAccess));
			pOutputTexture->CreateUAV(&pOutputRawUAV, TextureUAVDesc(0));
		}
		//Copy descriptors
		{
			pDevice->CopyDescriptorsSimple(1, uavHandle.GetCpuHandle(), pOutputTexture->GetUAV(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			pDevice->CopyDescriptorsSimple(1, srvHandle.GetCpuHandle(), pTLAS->GetSRV()->GetDescriptor(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		}

		int rayGenSize = 0;
		//Shader Bindings
		{
			int64 progIdSize = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
			int64 totalSize = 0;
		
			struct SBTEntry
			{
				SBTEntry(const std::wstring& entryPoint, const std::vector<void*>& inputData)
					: EntryPoint(entryPoint), InputData(inputData)
				{}
				std::wstring EntryPoint;
				std::vector<void*> InputData;
				int Size;
			};
			std::vector<SBTEntry> entries;
			entries.emplace_back(SBTEntry(L"RayGen", { reinterpret_cast<uint64*>(uavHandle.GetGpuHandle().ptr), reinterpret_cast<uint64*>(srvHandle.GetGpuHandle().ptr) }));
			entries.emplace_back(SBTEntry(L"Miss", { }));
			entries.emplace_back(SBTEntry(L"HitGroup", { }));

			for (SBTEntry& entry : entries)
			{
				entry.Size = Math::AlignUp<int64>(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT + 8 * entry.InputData.size(), D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
				totalSize += entry.Size;
			}
			rayGenSize = entries[0].Size;

			pShaderBindingTable->Create(BufferDesc::CreateVertexBuffer(Math::AlignUp<int64>(totalSize, 256), 1, BufferFlag::Upload));
			char* pData = (char*)pShaderBindingTable->Map();
			for (SBTEntry& entry : entries)
			{
				void* id = pPipelineProperties->GetShaderIdentifier(entry.EntryPoint.c_str());
				memcpy(pData, id, progIdSize);
				memcpy(pData + progIdSize, entry.InputData.data(), entry.InputData.size() * 8);
				pData += entry.Size;
			}
			pShaderBindingTable->Unmap();

		}
		//Dispatch Rays
		{
			D3D12_DISPATCH_RAYS_DESC rayDesc{};
			rayDesc.Width = pOutputTexture->GetWidth();
			rayDesc.Height = pOutputTexture->GetHeight();
			rayDesc.Depth = 1;
			rayDesc.RayGenerationShaderRecord.StartAddress = pShaderBindingTable->GetGpuHandle();
			rayDesc.RayGenerationShaderRecord.SizeInBytes = rayGenSize;

			pContext->InsertResourceBarrier(pOutputTexture.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			pContext->ClearUavUInt(pOutputTexture.get(), pOutputRawUAV);
			pContext->FlushResourceBarriers();
	
			pCmd->SetPipelineState1(pPipeline.Get());
			pCmd->DispatchRays(&rayDesc);
		}
		pContext->Execute(true);
	}

	m_pNoiseTexture = std::make_unique<Texture>(this, "Noise");
	m_pNoiseTexture->Create(pContext, "Resources/Textures/Noise.png", false);

	pContext->Execute(true);
}

void Graphics::UpdateImGui()
{
	m_FrameTimes[m_Frame % m_FrameTimes.size()] = GameTimer::DeltaTime();

	ImGui::Begin("SSAO");
	Vector2 image((float)m_pSSAOBlurred->GetWidth(), (float)m_pSSAOBlurred->GetHeight());
	Vector2 windowSize(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y);
	float width = windowSize.x;
	float height = windowSize.x * image.y / image.x;
	if (image.x / windowSize.x < image.y / windowSize.y)
	{
		width = image.x / image.y * windowSize.y;
		height = windowSize.y;
	}
	ImGui::Image(m_pSSAOTarget.get(), ImVec2(width, height));
	ImGui::End();

	ImGui::SetNextWindowPos(ImVec2(0, 0), 0, ImVec2(0, 0));
	ImGui::SetNextWindowSize(ImVec2(300, (float)m_WindowHeight));
	ImGui::Begin("GPU Stats", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	ImGui::Text("MS: %.4f", GameTimer::DeltaTime() * 1000.0f);
	ImGui::SameLine(100);
	ImGui::Text("FPS: %.1f", 1.0f / GameTimer::DeltaTime());
	ImGui::PlotLines("Frametime", m_FrameTimes.data(), (int)m_FrameTimes.size(), m_Frame % m_FrameTimes.size(), 0, 0.0f, 0.03f, ImVec2(200, 100));

	if (ImGui::TreeNodeEx("Lighting", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Combo("Render Path", (int*)& m_RenderPath, [](void* data, int index, const char** outText)
			{
				RenderPath p = (RenderPath)index;
				switch (p)
				{
				case RenderPath::Tiled:
					*outText = "Tiled";
					break;
				case RenderPath::Clustered:
					*outText = "Clustered";
					break;
				default:
					break;
				}
				return true;
			}, nullptr, 2);
		extern bool gVisualizeClusters;
		ImGui::Checkbox("Visualize Clusters", &gVisualizeClusters);

		ImGui::Separator();
		ImGui::SliderInt("Lights", &m_DesiredLightCount, 10, 16384*10);
		if (ImGui::Button("Generate Lights"))
		{
			RandomizeLights(m_DesiredLightCount);
		}

		ImGui::SliderFloat("Min Log Luminance", &g_MinLogLuminance, -100, 20);
		ImGui::SliderFloat("Max Log Luminance", &g_MaxLogLuminance, -50, 50);
		ImGui::SliderFloat("White Point", &g_WhitePoint, 0, 20);
		ImGui::SliderFloat("Tau", &g_Tau, 0, 100);

		ImGui::SliderFloat("AO Power", &g_AoPower, 1, 10);
		ImGui::SliderFloat("AO Threshold", &g_AoThreshold, 0, 0.025f);
		ImGui::SliderFloat("AO Radius", &g_AoRadius, 0.1f, 5.0f);
		ImGui::SliderInt("AO Samples", &g_AoSamples, 0, 64);

		if (ImGui::Button("Dump RenderGraph"))
		{
			gDumpRenderGraph = true;
		}

		ImGui::TreePop();
	}
	if (ImGui::TreeNodeEx("Descriptor Heaps", ImGuiTreeNodeFlags_DefaultOpen))
	{
		ImGui::Text("Used CPU Descriptor Heaps");
		for (const auto& pAllocator : m_DescriptorHeaps)
		{
			switch (pAllocator->GetType())
			{
			case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
				ImGui::TextWrapped("Constant/Shader/Unordered Access Views");
				break;
			case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
				ImGui::TextWrapped("Samplers");
				break;
			case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
				ImGui::TextWrapped("Render Target Views");
				break;
			case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
				ImGui::TextWrapped("Depth Stencil Views");
				break;
			default:
				break;
			}
			uint32 totalDescriptors = pAllocator->GetNumDescriptors();
			uint32 usedDescriptors = pAllocator->GetNumAllocatedDescriptors();
			std::stringstream str;
			str << usedDescriptors << "/" << totalDescriptors;
			ImGui::ProgressBar((float)usedDescriptors / totalDescriptors, ImVec2(-1, 0), str.str().c_str());
		}
		ImGui::TreePop();
	}
	ImGui::End();

	static bool showOutputLog = false;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::SetNextWindowPos(ImVec2(300, showOutputLog ? (float)m_WindowHeight - 250 : (float)m_WindowHeight - 20));
	ImGui::SetNextWindowSize(ImVec2(showOutputLog ? (float)(m_WindowWidth - 250) * 0.5f : m_WindowWidth - 250, 250));
	ImGui::SetNextWindowCollapsed(!showOutputLog);

	showOutputLog = ImGui::Begin("Output Log", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	if (showOutputLog)
	{
		ImGui::SetScrollHereY(1.0f);
		for (const Console::LogEntry& entry : Console::GetHistory())
		{
			switch (entry.Type)
			{
			case LogType::VeryVerbose:
			case LogType::Verbose:
			case LogType::Info:
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
				ImGui::TextWrapped("[Info] %s", entry.Message.c_str());
				break;
			case LogType::Warning:
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 0, 1));
				ImGui::TextWrapped("[Warning] %s", entry.Message.c_str());
				break;
			case LogType::Error:
			case LogType::FatalError:
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0, 0, 1));
				ImGui::TextWrapped("[Error] %s", entry.Message.c_str());
				break;
			}
			ImGui::PopStyleColor();
		}
	}
	ImGui::End();

	if (showOutputLog)
	{
		ImGui::SetNextWindowPos(ImVec2(250 + (m_WindowWidth - 250) / 2.0f, showOutputLog ? (float)m_WindowHeight - 250 : (float)m_WindowHeight - 20));
		ImGui::SetNextWindowSize(ImVec2((float)(m_WindowWidth - 250) * 0.5f, 250));
		ImGui::SetNextWindowCollapsed(!showOutputLog);
		ImGui::Begin("Profiler", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
		ProfileNode* pRootNode = Profiler::Instance()->GetRootNode();
		pRootNode->RenderImGui(m_Frame);
		ImGui::End();
	}
	ImGui::PopStyleVar();
}

CommandQueue* Graphics::GetCommandQueue(D3D12_COMMAND_LIST_TYPE type) const
{
	return m_CommandQueues.at(type).get();
}

CommandContext* Graphics::AllocateCommandContext(D3D12_COMMAND_LIST_TYPE type)
{
	int typeIndex = (int)type;

	std::lock_guard<std::mutex> lockGuard(m_ContextAllocationMutex);
	if (m_FreeCommandLists[typeIndex].size() > 0)
	{
		CommandContext* pCommandList = m_FreeCommandLists[typeIndex].front();
		m_FreeCommandLists[typeIndex].pop();
		pCommandList->Reset();
		return pCommandList;
	}
	else
	{
		ComPtr<ID3D12CommandList> pCommandList;
		ID3D12CommandAllocator* pAllocator = m_CommandQueues[type]->RequestAllocator();
		m_pDevice->CreateCommandList(0, type, pAllocator, nullptr, IID_PPV_ARGS(pCommandList.GetAddressOf()));
		m_CommandLists.push_back(std::move(pCommandList));
		m_CommandListPool[typeIndex].emplace_back(std::make_unique<CommandContext>(this, static_cast<ID3D12GraphicsCommandList*>(m_CommandLists.back().Get()), pAllocator, type));
		return m_CommandListPool[typeIndex].back().get();
	}
}

bool Graphics::IsFenceComplete(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	return pQueue->IsFenceComplete(fenceValue);
}

void Graphics::WaitForFence(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	pQueue->WaitForFence(fenceValue);
}

void Graphics::FreeCommandList(CommandContext* pCommandList)
{
	std::lock_guard<std::mutex> lockGuard(m_ContextAllocationMutex);
	m_FreeCommandLists[(int)pCommandList->GetType()].push(pCommandList);
}

bool Graphics::CheckTypedUAVSupport(DXGI_FORMAT format) const
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS featureData{};
	HR(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData)));

	switch (format)
	{
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
		// Unconditionally supported.
		return true;

	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SINT:
		// All these are supported if this optional feature is set.
		return featureData.TypedUAVLoadAdditionalFormats;

	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_A8_UNORM:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
		// Conditionally supported by specific pDevices.
		if (featureData.TypedUAVLoadAdditionalFormats)
		{
			D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = { format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
			HR(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)));
			const DWORD mask = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
			return ((formatSupport.Support2 & mask) == mask);
		}
		return false;

	default:
		return false;
	}
}

bool Graphics::UseRenderPasses() const
{
	return m_RenderPassTier > D3D12_RENDER_PASS_TIER::D3D12_RENDER_PASS_TIER_0;
}

void Graphics::IdleGPU()
{
	for (auto& pCommandQueue : m_CommandQueues)
	{
		if (pCommandQueue)
		{
			pCommandQueue->WaitForIdle();
		}
	}
}

uint32 Graphics::GetMultiSampleQualityLevel(uint32 msaa)
{
	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS qualityLevels;
	qualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	qualityLevels.Format = RENDER_TARGET_FORMAT;
	qualityLevels.NumQualityLevels = 0;
	qualityLevels.SampleCount = msaa;
	HR(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &qualityLevels, sizeof(qualityLevels)));
	return qualityLevels.NumQualityLevels - 1;
}

ID3D12Resource* Graphics::CreateResource(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState, D3D12_HEAP_TYPE heapType, D3D12_CLEAR_VALUE* pClearValue /*= nullptr*/)
{
	ID3D12Resource* pResource;
	D3D12_HEAP_PROPERTIES properties = CD3DX12_HEAP_PROPERTIES(heapType);
	HR(m_pDevice->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initialState, pClearValue, IID_PPV_ARGS(&pResource)));
	return pResource;
}