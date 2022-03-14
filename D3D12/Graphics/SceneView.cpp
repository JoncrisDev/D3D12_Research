#include "stdafx.h"
#include "SceneView.h"
#include "Core/CommandContext.h"
#include "Core/Buffer.h"
#include "Mesh.h"
#include "Core/PipelineState.h"
#include "Core/Texture.h"
#include "Core/ConsoleVariables.h"

namespace Tweakables
{
	extern ConsoleVariable<int> g_SsrSamples;
}

void DrawScene(CommandContext& context, const SceneView& scene, Batch::Blending blendModes)
{
	DrawScene(context, scene, scene.VisibilityMask, blendModes);
}

ShaderInterop::ViewUniforms GetViewUniforms(const SceneView& sceneView, Texture* pTarget)
{
	ShaderInterop::ViewUniforms parameters;
	const ViewTransform& view = sceneView.View;

	parameters.View = view.View;
	parameters.ViewInverse = view.ViewInverse;
	parameters.Projection = view.Projection;
	parameters.ProjectionInverse = view.ProjectionInverse;
	parameters.ViewProjection = view.ViewProjection;
	parameters.ViewProjectionInverse = view.ProjectionInverse * view.ViewInverse;

	Matrix reprojectionMatrix = view.ViewProjection.Invert() * view.PreviousViewProjection;
	// Transform from uv to clip space: texcoord * 2 - 1
	Matrix premult = {
		2.0f, 0, 0, 0,
		0, -2.0f, 0, 0,
		0, 0, 1, 0,
		-1, 1, 0, 1
	};
	// Transform from clip to uv space: texcoord * 0.5 + 0.5
	Matrix postmult = {
		0.5f, 0, 0, 0,
		0, -0.5f, 0, 0,
		0, 0, 1, 0,
		0.5f, 0.5f, 0, 1
	};

	parameters.PreviousViewProjection = view.PreviousViewProjection;
	parameters.ReprojectionMatrix = premult * reprojectionMatrix * postmult;
	parameters.ViewPosition = Vector4(view.Position);

	DirectX::XMVECTOR nearPlane, farPlane, left, right, top, bottom;
	view.Frustum.GetPlanes(&nearPlane, &farPlane, &right, &left, &top, &bottom);
	parameters.FrustumPlanes[0] = Vector4(nearPlane);
	parameters.FrustumPlanes[1] = Vector4(farPlane);
	parameters.FrustumPlanes[2] = Vector4(left);
	parameters.FrustumPlanes[3] = Vector4(right);
	parameters.FrustumPlanes[4] = Vector4(top);
	parameters.FrustumPlanes[5] = Vector4(bottom);

	if (pTarget)
	{
		parameters.ScreenDimensions = Vector2((float)pTarget->GetWidth(), (float)pTarget->GetHeight());
		parameters.ScreenDimensionsInv = Vector2(1.0f / pTarget->GetWidth(), 1.0f / pTarget->GetHeight());
	}
	parameters.ViewportDimensions = Vector2(view.Viewport.GetWidth(), view.Viewport.GetHeight());
	parameters.ViewportDimensionsInv = Vector2(1.0f / view.Viewport.GetWidth(), 1.0f / view.Viewport.GetHeight());

	parameters.ViewJitter.x = view.PreviousJitter.x - view.Jitter.x;
	parameters.ViewJitter.y = -(view.PreviousJitter.y - view.Jitter.y);
	parameters.NearZ = view.NearPlane;
	parameters.FarZ = view.FarPlane;
	parameters.FoV = view.FoV;

	parameters.SceneBoundsMin = Vector3(sceneView.SceneAABB.Center) - sceneView.SceneAABB.Extents;

	parameters.FrameIndex = sceneView.FrameIndex;
	parameters.SsrSamples = Tweakables::g_SsrSamples.Get();
	parameters.LightCount = sceneView.pLightBuffer->GetNumElements();

	parameters.DDGIProbeSize = 2 * Vector3(sceneView.SceneAABB.Extents) / (Vector3((float)sceneView.DDGIProbeVolumeDimensions.x, (float)sceneView.DDGIProbeVolumeDimensions.y, (float)sceneView.DDGIProbeVolumeDimensions.z) - Vector3::One);
	parameters.DDGIProbeVolumeDimensions = TIntVector3<uint32>(sceneView.DDGIProbeVolumeDimensions.x, sceneView.DDGIProbeVolumeDimensions.y, sceneView.DDGIProbeVolumeDimensions.z);
	parameters.DDGIIrradianceIndex = sceneView.pDDGIIrradiance ? sceneView.pDDGIIrradiance->GetSRVIndex() : DescriptorHandle::InvalidHeapIndex;
	parameters.DDGIDepthIndex = sceneView.pDDGIDepth ? sceneView.pDDGIDepth->GetSRVIndex() : DescriptorHandle::InvalidHeapIndex;
	parameters.DDGIProbeOffsetIndex = sceneView.pDDGIProbeOffset ? sceneView.pDDGIProbeOffset->GetSRVIndex() : DescriptorHandle::InvalidHeapIndex;

	memcpy(&parameters.LightViewProjections, &sceneView.ShadowData.LightViewProjections, ARRAYSIZE(parameters.LightViewProjections) * MAX_SHADOW_CASTERS);
	parameters.CascadeDepths = sceneView.ShadowData.CascadeDepths;
	parameters.NumCascades = sceneView.ShadowData.NumCascades;
	parameters.ShadowMapOffset = sceneView.ShadowData.ShadowMapOffset;

	parameters.TLASIndex = sceneView.pSceneTLAS ? sceneView.pSceneTLAS->GetSRVIndex() : DescriptorHandle::InvalidHeapIndex;
	parameters.MeshesIndex = sceneView.pMeshBuffer->GetSRVIndex();
	parameters.MaterialsIndex = sceneView.pMaterialBuffer->GetSRVIndex();
	parameters.MeshInstancesIndex = sceneView.pMeshInstanceBuffer->GetSRVIndex();
	parameters.TransformsIndex = sceneView.pTransformsBuffer->GetSRVIndex();
	parameters.LightsIndex = sceneView.pLightBuffer->GetSRVIndex();
	parameters.SkyIndex = sceneView.pSky->GetSRVIndex();
	return parameters;
}

void DrawScene(CommandContext& context, const SceneView& scene, const VisibilityMask& visibility, Batch::Blending blendModes)
{
	std::vector<const Batch*> meshes;
	for (const Batch& b : scene.Batches)
	{
		if (EnumHasAnyFlags(b.BlendMode, blendModes) && visibility.GetBit(b.InstanceData.World))
		{
			meshes.push_back(&b);
		}
	}

	auto CompareSort = [&scene, blendModes](const Batch* a, const Batch* b)
	{
		float aDist = Vector3::DistanceSquared(a->Bounds.Center, scene.View.Position);
		float bDist = Vector3::DistanceSquared(b->Bounds.Center, scene.View.Position);
		return EnumHasAnyFlags(blendModes, Batch::Blending::AlphaBlend) ? bDist < aDist : aDist < bDist;
	};
	std::sort(meshes.begin(), meshes.end(), CompareSort);

	for (const Batch* b : meshes)
	{
		context.SetRootConstants(0, b->InstanceData);
		if(context.GetCurrentPSO()->GetType() == PipelineStateType::Mesh)
		{
			context.DispatchMesh(ComputeUtils::GetNumThreadGroups(b->pMesh->NumMeshlets, 32));
		}
		else
		{
			context.SetIndexBuffer(b->pMesh->IndicesLocation);
			context.DrawIndexed(b->pMesh->IndicesLocation.Elements, 0, 0);
		}
	}
}
