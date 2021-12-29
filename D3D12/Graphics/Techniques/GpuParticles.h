#pragma once

class GraphicsDevice;
class Buffer;
class PipelineState;
class RootSignature;
class CommandContext;
class Texture;
class Camera;
class RGGraph;
struct SceneView;

class GpuParticles
{
public:
	GpuParticles(GraphicsDevice* pDevice);
	~GpuParticles() = default;

	void Simulate(RGGraph& graph, const SceneView& resources, Texture* pSourceDepth);
	void Render(RGGraph& graph, const SceneView& resources, Texture* pTarget, Texture* pDepth);
private:

	GraphicsDevice* m_pDevice;

	std::unique_ptr<Buffer> m_pAliveList1;
	std::unique_ptr<Buffer> m_pAliveList2;
	std::unique_ptr<Buffer> m_pDeadList;
	std::unique_ptr<Buffer> m_pParticleBuffer;
	std::unique_ptr<Buffer> m_pCountersBuffer;

	PipelineState* m_pPrepareArgumentsPS = nullptr;

	PipelineState* m_pEmitPS = nullptr;
	std::unique_ptr<Buffer> m_pEmitArguments;

	std::unique_ptr<RootSignature> m_pSimulateRS;
	PipelineState* m_pSimulatePS = nullptr;
	std::unique_ptr<Buffer> m_pSimulateArguments;

	PipelineState* m_pSimulateEndPS = nullptr;
	std::unique_ptr<Buffer> m_pDrawArguments;

	std::unique_ptr<RootSignature> m_pRenderParticlesRS;
	PipelineState* m_pRenderParticlesPS = nullptr;

	float m_ParticlesToSpawn = 0;
};

