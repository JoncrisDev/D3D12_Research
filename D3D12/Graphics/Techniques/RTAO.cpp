#include "stdafx.h"
#include "RTAO.h"
#include "Graphics/Core/Shader.h"
#include "Graphics/Core/PipelineState.h"
#include "Graphics/Core/RootSignature.h"
#include "Graphics/Core/GraphicsBuffer.h"
#include "Graphics/Core/Graphics.h"
#include "Graphics/Core/CommandContext.h"
#include "Graphics/Core/Texture.h"
#include "Graphics/Core/ShaderBindingTable.h"
#include "Graphics/Core/StateObject.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/Mesh.h"
#include "Scene/Camera.h"
#include "DemoApp.h"

RTAO::RTAO(GraphicsDevice* pDevice)
{
	if (pDevice->SupportsRayTracing())
	{
		SetupResources(pDevice);
		SetupPipelines(pDevice);
	}
}

void RTAO::Execute(RGGraph& graph, Texture* pColor, Texture* pDepth, const SceneData& sceneData, Camera& camera)
{
	static float g_AoPower = 3;
	static float g_AoRadius = 0.5f;
	static int32 g_AoSamples = 1;

	ImGui::Begin("Parameters");
	ImGui::Text("Ambient Occlusion");
	ImGui::SliderFloat("Power", &g_AoPower, 0, 10);
	ImGui::SliderFloat("Radius", &g_AoRadius, 0.1f, 5.0f);
	ImGui::SliderInt("Samples", &g_AoSamples, 1, 64);
	ImGui::End();

	RGPassBuilder rt = graph.AddPass("RTAO");
	rt.Bind([=](CommandContext& context, const RGPassResources& /*passResources*/)
		{
			context.InsertResourceBarrier(pDepth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			context.InsertResourceBarrier(pColor, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

			context.SetComputeRootSignature(m_pGlobalRS.get());
			context.SetPipelineState(m_pRtSO);

			struct Parameters
			{
				Matrix ViewInverse;
				Matrix ProjectionInverse;
				Matrix ViewProjectionInverse;
				float Power;
				float Radius;
				uint32 Samples;
				uint32 TLASIndex;
				uint32 FrameIndex;
			} parameters{};


			parameters.ViewInverse = camera.GetViewInverse();
			parameters.ProjectionInverse = camera.GetProjectionInverse();
			parameters.ViewProjectionInverse = camera.GetViewProjectionInverse();
			parameters.Power = g_AoPower;
			parameters.Radius = g_AoRadius;
			parameters.Samples = g_AoSamples;
			parameters.TLASIndex = sceneData.SceneTLAS;
			parameters.FrameIndex = sceneData.FrameIndex;

			ShaderBindingTable bindingTable(m_pRtSO);
			bindingTable.BindRayGenShader("RayGen");
			bindingTable.BindMissShader("Miss", {});

			context.SetComputeDynamicConstantBufferView(0, parameters);
			context.BindResource(1, 0, pColor->GetUAV());
			context.BindResource(2, 0, pDepth->GetSRV());
			context.BindResourceTable(3, sceneData.GlobalSRVHeapHandle.GpuHandle, CommandListContext::Compute);

			context.DispatchRays(bindingTable, pColor->GetWidth(), pColor->GetHeight());
		});
}

void RTAO::SetupResources(GraphicsDevice* pDevice)
{
}

void RTAO::SetupPipelines(GraphicsDevice* pDevice)
{
	ShaderLibrary* pShaderLibrary = pDevice->GetLibrary("RTAO.hlsl");
	m_pGlobalRS = std::make_unique<RootSignature>(pDevice);
	m_pGlobalRS->FinalizeFromShader("Global", pShaderLibrary);

	StateObjectInitializer stateDesc;
	stateDesc.AddLibrary(pShaderLibrary, { "RayGen", "Miss" });
	stateDesc.Name = "RT AO";
	stateDesc.MaxPayloadSize = sizeof(float);
	stateDesc.MaxAttributeSize = 2 * sizeof(float);
	stateDesc.pGlobalRootSignature = m_pGlobalRS.get();
	stateDesc.RayGenShader = "RayGen";
	stateDesc.AddMissShader("Miss");
	m_pRtSO = pDevice->CreateStateObject(stateDesc);
}
