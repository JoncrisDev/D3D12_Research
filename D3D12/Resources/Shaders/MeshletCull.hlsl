#include "Common.hlsli"
#include "HZB.hlsli"
#include "D3D12.hlsli"
#include "VisibilityBuffer.hlsli"
#include "WaveOps.hlsli"
#include "ShaderDebugRender.hlsli"

/*
	-- 2 Phase Occlusion Culling --

	Works under the assumption that it's likely that objects visible in the previous frame, will be visible this frame.

	In Phase 1, we render all objects that were visible last frame by testing against the previous HZB.
	Occluded objects are stored in a list, to be processed later.
	The HZB is constructed from the current result.
	Phase 2 tests all previously occluded objects against the new HZB and renders unoccluded.
	The HZB is constructed again from this result to be used in the next frame.

	Cull both on a per-instance level as on a per-meshlet level.
	Leverage Mesh/Amplification shaders to drive per-meshlet culling.

	https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf
*/

#ifndef OCCLUSION_FIRST_PASS
#define OCCLUSION_FIRST_PASS 1
#endif

#ifndef ALPHA_MASK
#define ALPHA_MASK 0
#endif

#define NUM_AS_THREADS 32
#define NUM_MESHLET_THREADS 32
#define NUM_CULL_INSTANCES_THREADS 64

struct MeshletCandidate
{
    uint InstanceID;
    uint MeshletIndex;
};

RWStructuredBuffer<MeshletCandidate> uMeshletCandidates : 			register(u0);
RWBuffer<uint> uCounter_MeshletCandidates : 						register(u1);
RWStructuredBuffer<uint> uOccludedInstances : 						register(u2);
RWBuffer<uint> uCounter_OccludedInstances : 						register(u3);
RWStructuredBuffer<MeshletCandidate> uOccludedMeshlets : 			register(u4);
RWBuffer<uint> uCounter_OccludedMeshlets : 							register(u5);

RWStructuredBuffer<D3D12_DISPATCH_ARGUMENTS> uDispatchArguments : 	register(u0);

StructuredBuffer<MeshletCandidate> tMeshletsToProcess : 			register(t0);
Buffer<uint> tCounter_MeshletsToProcess : 							register(t1);
StructuredBuffer<uint> tInstancesToProcess : 						register(t0);
Buffer<uint> tCounter_OccludedInstances : 							register(t1);

Texture2D<float> tHZB : 											register(t2);

uint GetNumInstances()
{
#if OCCLUSION_FIRST_PASS
    return cView.NumInstances;
#else
    return tCounter_OccludedInstances[0];
#endif
}

InstanceData GetInstanceForThread(uint threadID)
{
#if OCCLUSION_FIRST_PASS
    return GetInstance(threadID);
#else
	return GetInstance(tInstancesToProcess[threadID]);
#endif
}

[numthreads(NUM_CULL_INSTANCES_THREADS, 1, 1)]
void CullInstancesCS(uint threadID : SV_DispatchThreadID)
{
	uint numInstances = GetNumInstances();
    if(threadID >= numInstances)
        return;

    InstanceData instance = GetInstanceForThread(threadID);
    MeshData mesh = GetMesh(instance.MeshIndex);

	FrustumCullData cullData = FrustumCull(instance.BoundsOrigin, instance.BoundsExtents, cView.ViewProjection);
	bool isVisible = cullData.IsVisible;
	bool wasOccluded = false;

	if(isVisible)
	{
#if OCCLUSION_FIRST_PASS
		FrustumCullData prevCullData = FrustumCull(instance.BoundsOrigin, instance.BoundsExtents, cView.ViewProjectionPrev);
		wasOccluded = !HZBCull(prevCullData, tHZB);

		// If the instance was occluded the previous frame, we can't be sure it's still occluded this frame.
		// Add it to the list to re-test in the second phase.
		if(wasOccluded)
		{
			uint elementOffset = 0;
			InterlockedAdd_WaveOps(uCounter_OccludedInstances, 0, elementOffset);
			uOccludedInstances[elementOffset] = instance.ID;
		}
#else
		isVisible = HZBCull(cullData, tHZB);
#endif
	}

    if(isVisible && !wasOccluded)
    {
        uint elementOffset;
        InterlockedAdd_Varying_WaveOps(uCounter_MeshletCandidates, 0, mesh.MeshletCount, elementOffset);
        for(uint i = 0; i < mesh.MeshletCount; ++i)
        {
            MeshletCandidate meshlet;
            meshlet.InstanceID = instance.ID;
            meshlet.MeshletIndex = i;
            uMeshletCandidates[elementOffset + i] = meshlet;
        }
    }
}

[numthreads(1, 1, 1)]
void BuildMeshShaderIndirectArgs(uint threadID : SV_DispatchThreadID)
{
    uint numMeshlets = tCounter_MeshletsToProcess[0];
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCount = uint3(DivideAndRoundUp(numMeshlets, NUM_AS_THREADS), 1, 1);
    uDispatchArguments[0] = args;
}

[numthreads(1, 1, 1)]
void BuildInstanceCullIndirectArgs(uint threadID : SV_DispatchThreadID)
{
    uint numInstances = tCounter_OccludedInstances[0];
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCount = uint3(DivideAndRoundUp(numInstances, NUM_CULL_INSTANCES_THREADS), 1, 1);
    uDispatchArguments[0] = args;
}

struct PayloadData
{
	uint InstanceIDs[NUM_AS_THREADS];
	uint MeshletIndices[NUM_AS_THREADS];
};

groupshared PayloadData gsPayload;

#if __SHADER_TARGET_STAGE == __SHADER_STAGE_AMPLIFICATION
[numthreads(NUM_AS_THREADS, 1, 1)]
void CullAndDrawMeshletsAS(uint threadID : SV_DispatchThreadID)
{
	bool shouldSubmit = false;
	if(threadID < tCounter_MeshletsToProcess[0])
	{
		MeshletCandidate meshlet = tMeshletsToProcess[threadID];
		InstanceData instance = GetInstance(meshlet.InstanceID);
		MeshData mesh = GetMesh(instance.MeshIndex);
		MeshletBounds bounds = BufferLoad<MeshletBounds>(mesh.BufferIndex, meshlet.MeshletIndex, mesh.MeshletBoundsOffset);
		float3 boundsOrigin = mul(float4(bounds.Center, 1), instance.LocalToWorld).xyz;
		float3 boundsExtents = abs(mul(bounds.Extents, (float3x3)instance.LocalToWorld));

		FrustumCullData cullData = FrustumCull(boundsOrigin, boundsExtents, cView.ViewProjection);
		bool isVisible = cullData.IsVisible;
		bool wasOccluded = false;

		if(isVisible)
		{
#if OCCLUSION_FIRST_PASS
			FrustumCullData prevCullData = FrustumCull(boundsOrigin, boundsExtents, cView.ViewProjectionPrev);
			if(prevCullData.IsVisible)
			{
				wasOccluded = !HZBCull(prevCullData, tHZB);
			}

			// If the meshlet was occluded the previous frame, we can't be sure it's still occluded this frame.
			// Add it to the list to re-test in the second phase.
			if(wasOccluded)
			{
				uint elementOffset;
				InterlockedAdd_WaveOps(uCounter_OccludedMeshlets, 0, elementOffset);
				uOccludedMeshlets[elementOffset] = meshlet;
			}
#else
			isVisible = HZBCull(cullData, tHZB);
#endif
		}

		shouldSubmit = isVisible && !wasOccluded;
		if(shouldSubmit)
		{
			uint index = WavePrefixCountBits(shouldSubmit);
			gsPayload.InstanceIDs[index] = meshlet.InstanceID;
			gsPayload.MeshletIndices[index] = meshlet.MeshletIndex;
		}
	}

	uint visibleCount = WaveActiveCountBits(shouldSubmit);
	DispatchMesh(visibleCount, 1, 1, gsPayload);
}
#endif

struct PrimitiveAttribute
{
	uint PrimitiveID : SV_PrimitiveID;
	uint MeshletID : MeshletID;
	uint InstanceID : InstanceID;
};

struct VertexAttribute
{
	float4 Position : SV_Position;
#if ALPHA_MASK
	float2 UV : TEXCOORD;
#endif
};

VertexAttribute FetchVertexAttributes(MeshData mesh, float4x4 world, uint vertexId)
{
	VertexAttribute result = (VertexAttribute)0;
	float3 Position = BufferLoad<float3>(mesh.BufferIndex, vertexId, mesh.PositionsOffset);
	float3 positionWS = mul(float4(Position, 1.0f), world).xyz;
	result.Position = mul(float4(positionWS, 1.0f), cView.ViewProjection);
#if ALPHA_MASK
	if(mesh.UVsOffset != 0xFFFFFFFF)
		result.UV = UnpackHalf2(BufferLoad<uint>(mesh.BufferIndex, vertexId, mesh.UVsOffset));
#endif
	return result;
}

[outputtopology("triangle")]
[numthreads(NUM_MESHLET_THREADS, 1, 1)]
void MSMain(
	in uint groupThreadID : SV_GroupIndex,
	in uint groupID : SV_GroupID,
	in payload PayloadData payload,
	out vertices VertexAttribute verts[MESHLET_MAX_VERTICES],
	out indices uint3 triangles[MESHLET_MAX_TRIANGLES],
	out primitives PrimitiveAttribute primitives[MESHLET_MAX_TRIANGLES])
{
	uint meshletIndex = payload.MeshletIndices[groupID];
	uint instanceID = payload.InstanceIDs[groupID];

	InstanceData instance = GetInstance(instanceID);
	MeshData mesh = GetMesh(instance.MeshIndex);
	Meshlet meshlet = BufferLoad<Meshlet>(mesh.BufferIndex, meshletIndex, mesh.MeshletOffset);

	SetMeshOutputCounts(meshlet.VertexCount, meshlet.TriangleCount);

	for(uint i = groupThreadID; i < meshlet.VertexCount; i += NUM_MESHLET_THREADS)
	{
		uint vertexId = BufferLoad<uint>(mesh.BufferIndex, i + meshlet.VertexOffset, mesh.MeshletVertexOffset);
		VertexAttribute result = FetchVertexAttributes(mesh, instance.LocalToWorld, vertexId);
		verts[i] = result;
	}

	for(uint i = groupThreadID; i < meshlet.TriangleCount; i += NUM_MESHLET_THREADS)
	{
		MeshletTriangle tri = BufferLoad<MeshletTriangle>(mesh.BufferIndex, i + meshlet.TriangleOffset, mesh.MeshletTriangleOffset);
		triangles[i] = uint3(tri.V0, tri.V1, tri.V2);

		PrimitiveAttribute pri;
		pri.PrimitiveID = i;
		pri.MeshletID = meshletIndex;
		pri.InstanceID = instanceID;
		primitives[i] = pri;
	}
}

VisBufferData PSMain(
    VertexAttribute vertexData,
    PrimitiveAttribute primitiveData) : SV_TARGET0
{
#if ALPHA_MASK
	InstanceData instance = GetInstance(primitiveData.InstanceID);
	MaterialData material = GetMaterial(instance.MaterialIndex);
	float opacity = Sample2D(material.Diffuse, sMaterialSampler, vertexData.UV).w;
	if(opacity < material.AlphaCutoff)
		discard;
#endif

	VisBufferData Data;
	Data.ObjectID = primitiveData.InstanceID;
	Data.PrimitiveID = primitiveData.PrimitiveID;
	Data.MeshletID = primitiveData.MeshletID;
	return Data;
}

[numthreads(1, 1, 1)]
void PrintStatsCS(uint threadId : SV_DispatchThreadID)
{
	uint numInstances = cView.NumInstances;
	uint occludedInstances = uCounter_OccludedInstances[0];
	uint visibleInstances = numInstances - occludedInstances;
	uint phase1Meshlets = uCounter_MeshletCandidates[0];
	uint phase2Meshlets = uCounter_OccludedMeshlets[0];

	TextWriter writer = CreateTextWriter(float2(20, 20));

	writer = writer + 'T' + 'o' + 't' + 'a'  + 'l'  + ' ';
	writer = writer + 'i' + 'n' + 's' + 't'  + 'a'  + 'n'  + 'c'  + 'e'  + 's' + ' ';
	writer.Int(numInstances);
	writer.NewLine();

	writer = writer + '-' + '-' + '-' + 'P' + 'h' + 'a' + 's' + 'e' + ' ' + '1' + '-' + '-' + '-';
	writer.NewLine();

	writer = writer + 'O' + 'c' + 'c' + 'l'  + 'u'  + 'd'  + 'e'  + 'd' + ' ';
	writer = writer + 'i' + 'n' + 's' + 't'  + 'a'  + 'n'  + 'c'  + 'e'  + 's' + ' ';
	writer.Int(occludedInstances);
	writer.NewLine();

	writer = writer + 'P' + 'r' + 'o' + 'c'  + 'e'  + 's'  + 's'  + 'e' + 'd' + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's' + ' ';
	writer.Int(phase1Meshlets);
	writer.NewLine();

	writer = writer + '-' + '-' + '-' + 'P' + 'h' + 'a' + 's' + 'e' + ' ' + '2' + '-' + '-' + '-';
	writer.NewLine();

	writer = writer + 'P' + 'r' + 'o' + 'c'  + 'e'  + 's'  + 's'  + 'e' + 'd' + ' ';
	writer = writer + 'm' + 'e' + 's' + 'h'  + 'l'  + 'e'  + 't'  + 's' + ' ';
	writer.Int(phase2Meshlets);
	writer.NewLine();
}