#include "Common.hlsli"
#include "Lighting.hlsli"

#define BLOCK_SIZE 16

#define RootSig "RootFlags(ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT), " \
				"CBV(b0, visibility=SHADER_VISIBILITY_VERTEX), " \
				"CBV(b1, visibility=SHADER_VISIBILITY_ALL), " \
				"CBV(b2, visibility=SHADER_VISIBILITY_PIXEL), " \
				"DescriptorTable(SRV(t0, numDescriptors = 3), visibility=SHADER_VISIBILITY_PIXEL), " \
				"DescriptorTable(SRV(t3, numDescriptors = 7), visibility=SHADER_VISIBILITY_PIXEL), " \
				"DescriptorTable(SRV(t10, numDescriptors = 32, space = 1), visibility=SHADER_VISIBILITY_PIXEL), " \
				"SRV(t500, visibility=SHADER_VISIBILITY_PIXEL), " \
				"StaticSampler(s0, filter=FILTER_ANISOTROPIC, maxAnisotropy = 4, visibility = SHADER_VISIBILITY_PIXEL), " \
				"StaticSampler(s1, filter=FILTER_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, addressV = TEXTURE_ADDRESS_CLAMP, visibility = SHADER_VISIBILITY_PIXEL), " \
				"StaticSampler(s2, filter=FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, visibility = SHADER_VISIBILITY_PIXEL, comparisonFunc=COMPARISON_GREATER), " \

struct PerObjectData
{
	float4x4 World;
	float4x4 WorldViewProj;
};

struct PerViewData
{
	float4x4 View;
	float4x4 ViewInverse;
	float4x4 Projection;
	float4x4 ProjectionInverse;
	float2 InvScreenDimensions;
	float NearZ;
	float FarZ;
	int FrameIndex;
	int3 SsrSamples;
	int test;
#if CLUSTERED_FORWARD
    int3 ClusterDimensions;
    int2 ClusterSize;
	float2 LightGridParams;
#endif
};

ConstantBuffer<PerObjectData> cObjectData : register(b0);
ConstantBuffer<PerViewData> cViewData : register(b1);

struct VSInput
{
	float3 position : POSITION;
	float2 texCoord : TEXCOORD;
	float3 normal : NORMAL;
	float3 tangent : TANGENT;
	float3 bitangent : TEXCOORD1;
};

struct PSInput
{
	float4 position : SV_POSITION;
	float3 positionWS : POSITION_WS;
	float3 positionVS : POSITION_VS;
	float2 texCoord : TEXCOORD;
	float3 normal : NORMAL;
	float3 tangent : TANGENT;
	float3 bitangent : TEXCOORD1;
};

#if TILED_FORWARD
Texture2D<uint2> tLightGrid : register(t3);
#elif CLUSTERED_FORWARD
StructuredBuffer<uint2> tLightGrid : register(t3);
#endif
StructuredBuffer<uint> tLightIndexList : register(t4);

#if CLUSTERED_FORWARD
uint GetSliceFromDepth(float depth)
{
    return floor(log(depth) * cViewData.LightGridParams.x - cViewData.LightGridParams.y);
}
#endif

LightResult DoLight(float4 pos, float3 worldPos, float3 vPos, float3 N, float3 V, float3 diffuseColor, float3 specularColor, float roughness)
{
#if TILED_FORWARD
	uint2 tileIndex = uint2(floor(pos.xy / BLOCK_SIZE));
	uint startOffset = tLightGrid[tileIndex].x;
	uint lightCount = tLightGrid[tileIndex].y;
#elif CLUSTERED_FORWARD
	uint3 clusterIndex3D = uint3(floor(pos.xy / cViewData.ClusterSize), GetSliceFromDepth(vPos.z));
    uint clusterIndex1D = clusterIndex3D.x + (cViewData.ClusterDimensions.x * (clusterIndex3D.y + cViewData.ClusterDimensions.y * clusterIndex3D.z));
	uint startOffset = tLightGrid[clusterIndex1D].x;
	uint lightCount = tLightGrid[clusterIndex1D].y;
#endif
	LightResult totalResult = (LightResult)0;

	for(uint i = 0; i < lightCount; ++i)
	{
		uint lightIndex = tLightIndexList[startOffset + i];
		Light light = tLights[lightIndex];
		LightResult result = DoLight(light, specularColor, diffuseColor, roughness, pos, worldPos, vPos, N, V);
		totalResult.Diffuse += result.Diffuse;
		totalResult.Specular += result.Specular;
	}
	return totalResult;
}

[RootSignature(RootSig)]
PSInput VSMain(VSInput input)
{
	PSInput result;
	result.position = mul(float4(input.position, 1.0f), cObjectData.WorldViewProj);
	result.positionWS = mul(float4(input.position, 1.0f), cObjectData.World).xyz;
	result.positionVS = mul(float4(result.positionWS, 1.0f), cViewData.View).xyz;
	result.texCoord = input.texCoord;
	result.normal = normalize(mul(input.normal, (float3x3)cObjectData.World));
	result.tangent = normalize(mul(input.tangent, (float3x3)cObjectData.World));
	result.bitangent = normalize(mul(input.bitangent, (float3x3)cObjectData.World));
	return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
	float4 baseColor = tDiffuseTexture.Sample(sDiffuseSampler, input.texCoord);
	float3 specular = 0.5f;
	float metalness = 0;
	float r = 0.5f;

	float3 diffuseColor = ComputeDiffuseColor(baseColor.rgb, metalness);
	float3 specularColor = ComputeF0(specular.r, baseColor.rgb, metalness);

	float3x3 TBN = float3x3(normalize(input.tangent), normalize(input.bitangent), normalize(input.normal));
	float3 N = TangentSpaceNormalMapping(tNormalTexture, sDiffuseSampler, TBN, input.texCoord, true);
	float3 V = normalize(cViewData.ViewInverse[3].xyz - input.positionWS);	

	float3 ssr = 0;
	float ssrMode = 1.0f;
	float roughnessThreshold = 0.1f;
	bool ssrEnabled = false;
	if (ssrMode > 0.0)
	{
		ssrEnabled = r > roughnessThreshold;
	}
	else
	{
		ssrEnabled = ssrMode > 0.0;
	}
	if(ssrEnabled)
	{
#if 0
		if(cViewData.SsrSamples.x > 64)
		{
			float3 reflectionWs = normalize(reflect(-V, N));
			RayDesc ray;
			ray.Origin = input.positionWS;
			ray.Direction = reflectionWs;
			ray.TMin = 0.001;
			ray.TMax = input.positionVS.z;

			RayQuery<RAY_FLAG_NONE> q;

			q.TraceRayInline(
				tAccelerationStructure,
				RAY_FLAG_NONE,
				~0,
				ray);
			q.Proceed();

			if(q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
			{
				float distance = q.CommittedRayT();
				float gradient = saturate((60 - distance) / 60);
				float3 worldHit = ray.Origin + ray.Direction * q.CommittedRayT();
				float4 proj = mul(float4(mul(float4(worldHit, 1), cViewData.View).xyz, 1), cViewData.Projection);
				proj.xyz /= proj.w;
				proj.x = (proj.x + 1) / 2.0f;
				proj.y = (1 - proj.y) / 2.0f;
				if(proj.z > 0 && proj.x > 0 && proj.x < 1 && proj.y > 0 && proj.y < 1)
				{
					ssr = gradient * saturate(0.5f * tPrevColor.SampleLevel(sClampSampler, proj.xy, 0).xyz);
				}
			}
		}

#endif
		if (cViewData.SsrSamples.x > 0 && cViewData.SsrSamples.x <= 64)
		{
			float reflectionThreshold = 0.0f;
			float3 reflectionWs = normalize(reflect(-V, N));
			if (dot(V, reflectionWs) <= reflectionThreshold)
			{
				uint frameIndex = cViewData.FrameIndex;
				float jitter = InterleavedGradientNoise(input.position.xy, frameIndex) - 1.0f;

				uint maxSteps = cViewData.SsrSamples.x;

				float3 rayStartVS = input.positionVS;
				float linearDepth = rayStartVS.z;
				float3 reflectionVs = mul(reflectionWs, (float3x3)cViewData.View);
				float3 rayEndVS = rayStartVS + (reflectionVs * linearDepth);

				float3 rayStart = 0;
				rayStart.x = (rayStartVS.x * cViewData.Projection[0][0] / rayStartVS.z + 1) / 2;
				rayStart.y = 1 - (rayStartVS.y * cViewData.Projection[1][1] / rayStartVS.z + 1) / 2;
				rayStart.z = (rayStartVS.z - cViewData.FarZ) / (cViewData.NearZ - cViewData.FarZ);

				float3 rayEnd = 0;
				rayEnd.x = (rayEndVS.x * cViewData.Projection[0][0] / rayEndVS.z + 1) / 2;
				rayEnd.y = 1 - (rayEndVS.y * cViewData.Projection[1][1] / rayEndVS.z + 1) / 2;
				rayEnd.z = (rayEndVS.z - cViewData.FarZ) / (cViewData.NearZ - cViewData.FarZ);

				float3 rayStep = ((rayEnd - rayStart) / float(maxSteps));
				rayStep = rayStep / length(rayEnd.xy - rayStart.xy);
				float3 rayPos = rayStart + (rayStep * jitter);
				float zThickness = abs(rayStep.z);

				uint hitIndex = 0;
				float3 bestHit = rayPos;
				float prevSceneZ = rayStart.z;
				for (uint currStep = 0; currStep < maxSteps; currStep += 4)
				{
					uint4 step = float4(1, 2, 3, 4) + currStep;
					float4 sceneZ = float4(
						LinearizeDepth(tDepth.SampleLevel(sClampSampler, rayPos.xy + rayStep.xy * step.x, 0).x, cViewData.NearZ, cViewData.FarZ),
						LinearizeDepth(tDepth.SampleLevel(sClampSampler, rayPos.xy + rayStep.xy * step.y, 0).x, cViewData.NearZ, cViewData.FarZ),
						LinearizeDepth(tDepth.SampleLevel(sClampSampler, rayPos.xy + rayStep.xy * step.z, 0).x, cViewData.NearZ, cViewData.FarZ),
						LinearizeDepth(tDepth.SampleLevel(sClampSampler, rayPos.xy + rayStep.xy * step.w, 0).x, cViewData.NearZ, cViewData.FarZ)
					);
					float4 currentPosition = rayPos.z + rayStep.z * step;
					uint4 zTest = abs(currentPosition - sceneZ - zThickness) < zThickness;
                    uint zMask = (((zTest.x << 0) | (zTest.y << 1)) | (zTest.z << 2)) | (zTest.w << 3);
					if(zMask > 0)
					{
						uint firstHit = firstbitlow(zMask);
						if(firstHit > 0)
						{
							prevSceneZ = sceneZ[firstHit - 1];
						}

						bestHit = rayPos + (rayStep * float(currStep + firstHit + 1));
						float zAfter = sceneZ[firstHit] - bestHit.z;
						float zBefore = (prevSceneZ - bestHit.z) + rayStep.z;
						float weight = saturate(zAfter / (zAfter - zBefore));
						float3 prevRayPos = bestHit - rayStep;
						bestHit = (prevRayPos * weight) + (bestHit * (1.0f - weight));
						hitIndex = currStep + firstHit;
						break;
					}
					prevSceneZ = sceneZ.w;
				}

				float4 hitColor = 0;
				if (hitIndex > 0)
				{
					float2 texCoord = bestHit.xy;
					float2 dist = (float2(texCoord.x, 1.0f - texCoord.y) * 2.0f) - float2(1.0f, 1.0f);
					float edgeAttenuation = (1.0 - (float(hitIndex - 1) / float(maxSteps))) * 4.0f;
					edgeAttenuation = saturate(edgeAttenuation);
					edgeAttenuation *= smoothstep(0.0f, 0.5f, saturate(1.0 - dot(dist, dist)));
					float3 reflectionResult = tPrevColor.SampleLevel(sClampSampler, texCoord.xy, 0).xyz;
					hitColor = float4(reflectionResult, edgeAttenuation);
				}
				float roughnessMask = 1.0f - (r / (1.0f - roughnessThreshold));
				roughnessMask = saturate(roughnessMask);
				float ssrWeight = (hitColor.w * roughnessMask);
				ssr = saturate(hitColor.xyz * ssrWeight);
			}
		}
	}


	LightResult lighting = DoLight(input.position, input.positionWS, input.positionVS, N, V, diffuseColor, specularColor, r);

	float ao = tAO.SampleLevel(sDiffuseSampler, (float2)input.position.xy * cViewData.InvScreenDimensions, 0).r;
	float3 color = lighting.Diffuse + lighting.Specular + ssr * ao; 
	color += ApplyAmbientLight(diffuseColor, ao, tLights[0].GetColor().rgb * 0.1f);
	color += ApplyVolumetricLighting(cViewData.ViewInverse[3].xyz, input.positionWS.xyz, input.position.xyz, cViewData.View, tLights[0], 10);
	
	return float4(color, baseColor.a);
}