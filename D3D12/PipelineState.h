#pragma once

class Shader;

enum class BlendMode
{
	REPLACE = 0,
	ADD,
	MULTIPLY,
	ALPHA,
	ADDALPHA,
	PREMULALPHA,
	INVDESTALPHA,
	SUBTRACT,
	SUBTRACTALPHA,
	UNDEFINED,
};

class PipelineState
{
public:
	PipelineState();

	//BlendState
	void SetBlendMode(const BlendMode& blendMode, bool alphaToCoverage);

	//DepthStencilState
	void SetDepthEnabled(bool enabled);
	void SetDepthWrite(bool enabled);
	void SetDepthTest(const D3D12_COMPARISON_FUNC func);
	void SetStencilTest(bool stencilEnabled, D3D12_COMPARISON_FUNC mode, D3D12_STENCIL_OP pass, D3D12_STENCIL_OP fail, D3D12_STENCIL_OP zFail, unsigned int stencilRef, unsigned char compareMask, unsigned char writeMask);

	//RasterizerState
	void SetScissorEnabled(bool enabled);
	void SetMultisampleEnabled(bool enabled);
	void SetFillMode(D3D12_FILL_MODE fillMode);
	void SetCullMode(D3D12_CULL_MODE cullMode);
	void SetLineAntialias(bool lineAntiAlias);

	void Finalize(ID3D12Device* pDevice);
	ID3D12PipelineState* GetPipelineState() const { return m_pPipelineState.Get(); }

	void SetInputLayout(D3D12_INPUT_ELEMENT_DESC* pElements, uint32 count);
	void SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY_TYPE topology);

	void SetRootSignature(ID3D12RootSignature* pRootSignature);

	void SetVertexShader(const void* byteCode, uint32 byteCodeLength);
	void SetPixelShader(const void* byteCode, uint32 byteCodeLength);

private:
	D3D12_GRAPHICS_PIPELINE_STATE_DESC m_Desc = {};
	ComPtr<ID3D12PipelineState> m_pPipelineState;
};