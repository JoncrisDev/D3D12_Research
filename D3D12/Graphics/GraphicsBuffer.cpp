#include "stdafx.h"
#include "GraphicsBuffer.h"
#include "CommandContext.h"
#include "Graphics.h"

void GraphicsBuffer::Create(Graphics* pGraphics, uint64 size, bool cpuVisible)
{
	CreateInternal(pGraphics->GetDevice(), 1, size, cpuVisible ? BufferUsage::Dynamic : BufferUsage::Default);
}

void GraphicsBuffer::SetData(CommandContext* pContext, void* pData, uint64 dataSize, uint32 offset)
{
	assert(dataSize + offset <= GetSize());
	pContext->InitializeBuffer(this, pData, dataSize);
}

void* GraphicsBuffer::Map(uint32 subResource /*= 0*/, uint64 readFrom /*= 0*/, uint64 readTo /*= 0*/)
{
	assert(m_pResource);
	assert((m_Usage & BufferUsage::Dynamic) == BufferUsage::Dynamic || (m_Usage & BufferUsage::ReadBack) == BufferUsage::ReadBack);
	CD3DX12_RANGE range(readFrom, readTo);
	m_pResource->Map(subResource, &range, &m_pMappedData);
	return m_pMappedData;
}

void GraphicsBuffer::Unmap(uint32 subResource /*= 0*/, uint64 writtenFrom /*= 0*/, uint64 writtenTo /*= 0*/)
{
	if (m_pMappedData)
	{
		assert(m_pResource);
		assert((m_Usage & BufferUsage::Dynamic) == BufferUsage::Dynamic || (m_Usage & BufferUsage::ReadBack) == BufferUsage::ReadBack);
		CD3DX12_RANGE range(writtenFrom, writtenFrom);
		m_pResource->Unmap(subResource, &range);
		m_pMappedData = nullptr;
	}
}

void GraphicsBuffer::CreateInternal(ID3D12Device* pDevice, uint32 elementStride, uint64 elementCount, BufferUsage usage)
{
	Release();

	m_Usage = usage;
	m_ElementCount = elementCount;
	m_ElementStride = elementStride;

	const int alignment = 16;
	int bufferSize = (GetSize() + (alignment - 1)) & ~(alignment - 1);

	D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
	if ((usage & BufferUsage::UnorderedAccess) == BufferUsage::UnorderedAccess)
	{
		flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
	if ((usage & BufferUsage::ShaderResource) != BufferUsage::ShaderResource)
	{
		flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
	}
	D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize, flags);

	bool cpuVisible = (usage & BufferUsage::Dynamic) == BufferUsage::Dynamic;
	bool readBack = (usage & BufferUsage::ReadBack) == BufferUsage::ReadBack;
	assert(!(cpuVisible && readBack));
	m_CurrentState = readBack ? D3D12_RESOURCE_STATE_COPY_DEST : (cpuVisible ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON);
	D3D12_HEAP_PROPERTIES properties = CD3DX12_HEAP_PROPERTIES(readBack ? D3D12_HEAP_TYPE_READBACK : (cpuVisible ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT));
	HR(pDevice->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, m_CurrentState, nullptr, IID_PPV_ARGS(&m_pResource)));

	CreateViews(pDevice);
}

StructuredBuffer::StructuredBuffer(Graphics* pGraphics)
{
	m_Uav = pGraphics->AllocateCpuDescriptors(1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_Srv = pGraphics->AllocateCpuDescriptors(1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void StructuredBuffer::Create(Graphics* pGraphics, uint32 elementStride, uint64 elementCount, bool cpuVisible /*= false*/)
{
	BufferUsage usage = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource;
	if (cpuVisible)
	{
		usage |= BufferUsage::Dynamic;
	}
	CreateInternal(pGraphics->GetDevice(), elementStride, elementCount, usage);
}

void StructuredBuffer::CreateViews(ID3D12Device* pDevice)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Buffer.CounterOffsetInBytes = 0;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
	uavDesc.Buffer.NumElements = (uint32)m_ElementCount;
	uavDesc.Buffer.StructureByteStride = m_ElementStride;
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

	ID3D12Resource* pCounterResource = nullptr;
	D3D12_RESOURCE_DESC counterDesc = CD3DX12_RESOURCE_DESC::Buffer(4, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
	D3D12_HEAP_PROPERTIES counterProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	HR(pDevice->CreateCommittedResource(&counterProperties, D3D12_HEAP_FLAG_NONE, &counterDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&pCounterResource)));
	m_pCounter = std::make_unique<GraphicsResource>(pCounterResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	pDevice->CreateUnorderedAccessView(m_pResource, pCounterResource, &uavDesc, m_Uav);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
	srvDesc.Buffer.NumElements = (uint32)m_ElementCount;
	srvDesc.Buffer.StructureByteStride = m_ElementStride;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;

	pDevice->CreateShaderResourceView(m_pResource, &srvDesc, m_Srv);
}

ByteAddressBuffer::ByteAddressBuffer(Graphics* pGraphics)
{
	m_Uav = pGraphics->AllocateCpuDescriptors(1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	m_Srv = pGraphics->AllocateCpuDescriptors(1, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

void ByteAddressBuffer::Create(Graphics* pGraphics, uint32 elementStride, uint64 elementCount, bool cpuVisible /*= false*/)
{
	assert(elementStride == 1);
	BufferUsage usage = BufferUsage::UnorderedAccess | BufferUsage::ShaderResource;
	if (cpuVisible)
	{
		usage |= BufferUsage::Dynamic;
	}
	CreateInternal(pGraphics->GetDevice(), elementStride, elementCount, usage);
}

void ByteAddressBuffer::CreateViews(ID3D12Device* pDevice)
{
	D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Buffer.CounterOffsetInBytes = 0;
	uavDesc.Buffer.FirstElement = 0;
	uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
	uavDesc.Buffer.NumElements = (uint32)m_ElementCount;
	uavDesc.Buffer.StructureByteStride = m_ElementStride;
	uavDesc.Format = DXGI_FORMAT_UNKNOWN;
	uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

	pDevice->CreateUnorderedAccessView(m_pResource, nullptr, &uavDesc, m_Uav);

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.Format = DXGI_FORMAT_UNKNOWN;
	srvDesc.Buffer.FirstElement = 0;
	srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
	srvDesc.Buffer.NumElements = (uint32)m_ElementCount;
	srvDesc.Buffer.StructureByteStride = m_ElementStride;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;

	pDevice->CreateShaderResourceView(m_pResource, &srvDesc, m_Srv);
}

void VertexBuffer::Create(Graphics* pGraphics, uint32 elementStride, uint64 elementCount, bool cpuVisible /*= false*/)
{
	BufferUsage usage = BufferUsage::Default;
	if (cpuVisible)
	{
		usage |= BufferUsage::Dynamic;
	}
	CreateInternal(pGraphics->GetDevice(), elementStride, elementCount, usage);
}

void VertexBuffer::CreateViews(ID3D12Device* pDevice)
{
	m_View.BufferLocation = GetGpuHandle();
	m_View.SizeInBytes = (uint32)GetSize();
	m_View.StrideInBytes = GetStride();
}

void IndexBuffer::Create(Graphics * pGraphics, bool smallIndices, uint64 elementCount, bool cpuVisible /*= false*/)
{
	m_SmallIndices = smallIndices;
	BufferUsage usage = BufferUsage::Default;
	if (cpuVisible)
	{
		usage |= BufferUsage::Dynamic;
	}
	CreateInternal(pGraphics->GetDevice(), smallIndices ? 2 : 4, elementCount, usage);
}

void IndexBuffer::CreateViews(ID3D12Device* pDevice)
{
	m_View.BufferLocation = GetGpuHandle();
	m_View.Format = m_SmallIndices ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
	m_View.SizeInBytes = (uint32)GetSize();
}

void ReadbackBuffer::Create(Graphics* pGraphics, uint64 size)
{
	CreateInternal(pGraphics->GetDevice(), 1, size, BufferUsage::ReadBack);
}
