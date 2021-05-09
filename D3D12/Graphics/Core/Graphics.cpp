#include "stdafx.h"
#include "Graphics.h"
#include "CommandQueue.h"
#include "CommandContext.h"
#include "OfflineDescriptorAllocator.h"
#include "GraphicsResource.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "Shader.h"
#include "DynamicResourceAllocator.h"
#include "Graphics/Mesh.h"
#include "Core/Input.h"
#include "Texture.h"
#include "Scene/Camera.h"
#include "ResourceViews.h"
#include "GraphicsBuffer.h"
#include "Graphics/Profiler.h"
#include "StateObject.h"
#include "Core/CommandLine.h"

const DXGI_FORMAT GraphicsDevice::DEPTH_STENCIL_FORMAT = DXGI_FORMAT_D32_FLOAT;
const DXGI_FORMAT GraphicsDevice::DEPTH_STENCIL_SHADOW_FORMAT = DXGI_FORMAT_D16_UNORM;
const DXGI_FORMAT GraphicsDevice::RENDER_TARGET_FORMAT = DXGI_FORMAT_R11G11B10_FLOAT;
const DXGI_FORMAT GraphicsDevice::SWAPCHAIN_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;

GraphicsDevice::GraphicsDevice(IDXGIAdapter4* pAdapter)
{
	VERIFY_HR(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(m_pDevice.ReleaseAndGetAddressOf())));
	m_pDevice.As(&m_pRaytracingDevice);
	D3D::SetObjectName(m_pDevice.Get(), "Main Device");

#if !PLATFORM_UWP
	auto OnDeviceRemovedCallback = [](void* pContext, BOOLEAN) {
		GraphicsDevice* pDevice = (GraphicsDevice*)pContext;
		std::string error = D3D::GetErrorString(DXGI_ERROR_DEVICE_REMOVED, pDevice->m_pDevice.Get());
		E_LOG(Error, "%s", error.c_str());
	};

	HANDLE deviceRemovedEvent = CreateEventA(nullptr, false, false, nullptr);
	VERIFY_HR(m_pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_pDeviceRemovalFence.GetAddressOf())));
	m_pDeviceRemovalFence->SetEventOnCompletion(UINT64_MAX, deviceRemovedEvent);
	RegisterWaitForSingleObject(&m_DeviceRemovedEvent, deviceRemovedEvent, OnDeviceRemovedCallback, this, INFINITE, 0);
#endif

	ID3D12InfoQueue* pInfoQueue = nullptr;
	if (SUCCEEDED(m_pDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue))))
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

		if (CommandLine::GetBool("d3dbreakvalidation"))
		{
			VERIFY_HR_EX(pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true), GetDevice());
			E_LOG(Warning, "D3D Validation Break on Severity Enabled");
		}
		pInfoQueue->PushStorageFilter(&NewFilter);
		pInfoQueue->Release();
	}

	bool setStablePowerState = CommandLine::GetBool("stablepowerstate");
	if (setStablePowerState)
	{
		D3D12EnableExperimentalFeatures(0, nullptr, nullptr, nullptr);
		m_pDevice->SetStablePowerState(TRUE);
	}

	//Feature checks
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS caps0{};
		if (SUCCEEDED(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &caps0, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS))))
		{
			checkf(caps0.ResourceHeapTier >= D3D12_RESOURCE_HEAP_TIER_2, "Device does not support Resource Heap Tier 2 or higher. Tier 1 is not supported");
			checkf(caps0.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3, "Device does not support Resource Binding Tier 3 or higher. Tier 2 and under is not supported.");
		}
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 caps5{};
		if (SUCCEEDED(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &caps5, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5))))
		{
			m_RenderPassTier = caps5.RenderPassesTier;
			m_RayTracingTier = caps5.RaytracingTier;
		}
		D3D12_FEATURE_DATA_D3D12_OPTIONS6 caps6{};
		if (SUCCEEDED(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &caps6, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS6))))
		{
			m_VRSTier = caps6.VariableShadingRateTier;
			m_VRSTileSize = caps6.ShadingRateImageTileSize;
		}
		D3D12_FEATURE_DATA_D3D12_OPTIONS7 caps7{};
		if (SUCCEEDED(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &caps7, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS7))))
		{
			m_MeshShaderSupport = caps7.MeshShaderTier;
			m_SamplerFeedbackSupport = caps7.SamplerFeedbackTier;
		}
		D3D12_FEATURE_DATA_SHADER_MODEL shaderModelSupport{};
		shaderModelSupport.HighestShaderModel = D3D_SHADER_MODEL_6_7;
		if (SUCCEEDED(m_pDevice->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModelSupport, sizeof(D3D12_FEATURE_DATA_SHADER_MODEL))))
		{
			m_ShaderModelMajor = (uint8)(shaderModelSupport.HighestShaderModel >> 0x4);
			m_ShaderModelMinor = (uint8)(shaderModelSupport.HighestShaderModel & 0xF);
		}
	}

	//Create all the required command queues
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_DIRECT] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_DIRECT);
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_COMPUTE] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COMPUTE);
	m_CommandQueues[D3D12_COMMAND_LIST_TYPE_COPY] = std::make_unique<CommandQueue>(this, D3D12_COMMAND_LIST_TYPE_COPY);

	// Allocators
	m_pDynamicAllocationManager = std::make_unique<DynamicAllocationManager>(this, BufferFlag::Upload);
	m_pGlobalViewHeap = std::make_unique<GlobalOnlineDescriptorHeap>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2000, 1000000);
	m_pPersistentDescriptorHeap = std::make_unique<OnlineDescriptorAllocator>(m_pGlobalViewHeap.get());

	check(m_DescriptorHeaps.size() == D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 256);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 128);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_RTV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 128);
	m_DescriptorHeaps[D3D12_DESCRIPTOR_HEAP_TYPE_DSV] = std::make_unique<OfflineDescriptorAllocator>(this, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 64);

	// Shaders
	m_pShaderManager = std::make_unique<ShaderManager>("Resources/Shaders/", m_ShaderModelMajor, m_ShaderModelMinor);
}

GraphicsDevice::~GraphicsDevice()
{

}

void GraphicsDevice::Destroy()
{
	IdleGPU();
#if !PLATFORM_UWP
	check(UnregisterWait(m_DeviceRemovedEvent) != 0);
#endif
}

void GraphicsDevice::GarbageCollect()
{
	m_pDynamicAllocationManager->CollectGarbage();
}

int GraphicsDevice::RegisterBindlessResource(ResourceView* pResourceView, ResourceView* pFallback)
{
	auto it = m_ViewToDescriptorIndex.find(pResourceView);
	if (it != m_ViewToDescriptorIndex.end())
	{
		return it->second;
	}
	if (pResourceView)
	{
		DescriptorHandle handle = m_pPersistentDescriptorHeap->Allocate(1);
		GetDevice()->CopyDescriptorsSimple(1, handle.CpuHandle, pResourceView->GetDescriptor(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		m_ViewToDescriptorIndex[pResourceView] = handle.HeapIndex;
		return handle.HeapIndex;
	}
	return pFallback ? RegisterBindlessResource(pFallback) : 0;
}

int GraphicsDevice::RegisterBindlessResource(Texture* pTexture, Texture* pFallback /*= nullptr*/)
{
	return RegisterBindlessResource(pTexture ? pTexture->GetSRV() : nullptr, pFallback ? pFallback->GetSRV() : nullptr);
}

CommandQueue* GraphicsDevice::GetCommandQueue(D3D12_COMMAND_LIST_TYPE type) const
{
	return m_CommandQueues.at(type).get();
}

CommandContext* GraphicsDevice::AllocateCommandContext(D3D12_COMMAND_LIST_TYPE type)
{
	int typeIndex = (int)type;
	CommandContext* pContext = nullptr;

	{
		std::scoped_lock<std::mutex> lock(m_ContextAllocationMutex);
		if (m_FreeCommandLists[typeIndex].size() > 0)
		{
			pContext = m_FreeCommandLists[typeIndex].front();
			m_FreeCommandLists[typeIndex].pop();
			pContext->Reset();
		}
		else
		{
			ComPtr<ID3D12CommandList> pCommandList;
			ID3D12CommandAllocator* pAllocator = m_CommandQueues[type]->RequestAllocator();
			VERIFY_HR(GetDevice()->CreateCommandList(0, type, pAllocator, nullptr, IID_PPV_ARGS(pCommandList.GetAddressOf())));
			D3D::SetObjectName(pCommandList.Get(), Sprintf("Pooled Commandlist - %d", m_CommandLists.size()).c_str());
			m_CommandLists.push_back(std::move(pCommandList));
			m_CommandListPool[typeIndex].emplace_back(std::make_unique<CommandContext>(this, static_cast<ID3D12GraphicsCommandList*>(m_CommandLists.back().Get()), type, pAllocator));
			pContext = m_CommandListPool[typeIndex].back().get();
		}
	}
	return pContext;
}

bool GraphicsDevice::IsFenceComplete(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	return pQueue->IsFenceComplete(fenceValue);
}

void GraphicsDevice::WaitForFence(uint64 fenceValue)
{
	D3D12_COMMAND_LIST_TYPE type = (D3D12_COMMAND_LIST_TYPE)(fenceValue >> 56);
	CommandQueue* pQueue = GetCommandQueue(type);
	pQueue->WaitForFence(fenceValue);
}

void GraphicsDevice::FreeCommandList(CommandContext* pCommandList)
{
	std::lock_guard<std::mutex> lockGuard(m_ContextAllocationMutex);
	m_FreeCommandLists[(int)pCommandList->GetType()].push(pCommandList);
}

bool GraphicsDevice::CheckTypedUAVSupport(DXGI_FORMAT format) const
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS featureData{};
	VERIFY_HR_EX(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &featureData, sizeof(featureData)), GetDevice());

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
			VERIFY_HR_EX(GetDevice()->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport, sizeof(formatSupport)), GetDevice());
			const DWORD mask = D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE;
			return ((formatSupport.Support2 & mask) == mask);
		}
		return false;

	default:
		return false;
	}
}

bool GraphicsDevice::UseRenderPasses() const
{
	return m_RenderPassTier >= D3D12_RENDER_PASS_TIER_0;
}

void GraphicsDevice::IdleGPU()
{
	for (auto& pCommandQueue : m_CommandQueues)
	{
		if (pCommandQueue)
		{
			pCommandQueue->WaitForIdle();
		}
	}
}

ID3D12Resource* GraphicsDevice::CreateResource(const D3D12_RESOURCE_DESC& desc, D3D12_RESOURCE_STATES initialState, D3D12_HEAP_TYPE heapType, D3D12_CLEAR_VALUE* pClearValue /*= nullptr*/)
{
	ID3D12Resource* pResource;
	D3D12_HEAP_PROPERTIES properties = CD3DX12_HEAP_PROPERTIES(heapType);
	VERIFY_HR_EX(GetDevice()->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, initialState, pClearValue, IID_PPV_ARGS(&pResource)), GetDevice());
	return pResource;
}

PipelineState* GraphicsDevice::CreatePipeline(const PipelineStateInitializer& psoDesc)
{
	std::unique_ptr<PipelineState> pPipeline = std::make_unique<PipelineState>(this);
	pPipeline->Create(psoDesc);
	m_Pipelines.push_back(std::move(pPipeline));
	return m_Pipelines.back().get();
}

StateObject* GraphicsDevice::CreateStateObject(const StateObjectInitializer& stateDesc)
{
	std::unique_ptr<StateObject> pStateObject = std::make_unique<StateObject>(this);
	pStateObject->Create(stateDesc);
	m_StateObjects.push_back(std::move(pStateObject));
	return m_StateObjects.back().get();
}

std::unique_ptr<GraphicsInstance> GraphicsInstance::CreateInstance(GraphicsInstanceFlags createFlags /*= GraphicsFlags::None*/)
{
	return std::make_unique<GraphicsInstance>(createFlags);
}

GraphicsInstance::GraphicsInstance(GraphicsInstanceFlags createFlags)
{
	UINT flags = 0;
	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::DebugDevice))
	{
		flags |= DXGI_CREATE_FACTORY_DEBUG;
	}
	VERIFY_HR(CreateDXGIFactory2(flags, IID_PPV_ARGS(m_pFactory.GetAddressOf())));
	BOOL allowTearing;
	if (SUCCEEDED(m_pFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(BOOL))))
	{
		m_AllowTearing = allowTearing;
	}

	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::DebugDevice))
	{
		ComPtr<ID3D12Debug> pDebugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
		{
			pDebugController->EnableDebugLayer();
		}
	}

	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::DRED))
	{
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> pDredSettings;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDredSettings))))
		{
			// Turn on auto-breadcrumbs and page fault reporting.
			pDredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			pDredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			pDredSettings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			E_LOG(Warning, "DRED Enabled");
		}
	}

	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::GpuValidation))
	{
		ComPtr<ID3D12Debug1> pDebugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebugController))))
		{
			pDebugController->SetEnableGPUBasedValidation(true);
		}
	}

#if PLATFORM_WINDOWS
	if (EnumHasAnyFlags(createFlags, GraphicsInstanceFlags::Pix))
	{
		if (GetModuleHandleA("WinPixGpuCapturer.dll") == 0)
		{
			std::string pixPath;
			if (D3D::GetLatestWinPixGpuCapturerPath(pixPath))
			{
				if (LoadLibraryA(pixPath.c_str()))
				{
					E_LOG(Warning, "Dynamically loaded PIX ('%s')", pixPath.c_str());
				}
			}
		}
	}
#endif
}

std::unique_ptr<SwapChain> GraphicsInstance::CreateSwapchain(GraphicsDevice* pDevice, WindowHandle pNativeWindow, DXGI_FORMAT format, uint32 width, uint32 height, uint32 numFrames, bool vsync)
{
	return std::make_unique<SwapChain>(pDevice, m_pFactory.Get(), pNativeWindow, format, width, height, numFrames, vsync);
}

ComPtr<IDXGIAdapter4> GraphicsInstance::EnumerateAdapter(bool useWarp)
{
	ComPtr<IDXGIAdapter4> pAdapter;
	ComPtr<ID3D12Device> pDevice;
	if (!useWarp)
	{
		uint32 adapterIndex = 0;
		E_LOG(Info, "Adapters:");
		DXGI_GPU_PREFERENCE gpuPreference = DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
		while (m_pFactory->EnumAdapterByGpuPreference(adapterIndex++, gpuPreference, IID_PPV_ARGS(pAdapter.ReleaseAndGetAddressOf())) == S_OK)
		{
			DXGI_ADAPTER_DESC3 desc;
			pAdapter->GetDesc3(&desc);
			E_LOG(Info, "\t%s - %f GB", UNICODE_TO_MULTIBYTE(desc.Description), (float)desc.DedicatedVideoMemory * Math::BytesToGigaBytes);

			uint32 outputIndex = 0;
			ComPtr<IDXGIOutput> pOutput;
			while (pAdapter->EnumOutputs(outputIndex++, pOutput.ReleaseAndGetAddressOf()) == S_OK)
			{
				ComPtr<IDXGIOutput6> pOutput1;
				pOutput.As(&pOutput1);
				DXGI_OUTPUT_DESC1 outputDesc;
				pOutput1->GetDesc1(&outputDesc);

				E_LOG(Info, "\t\tMonitor %d - %dx%d - HDR: %s - %d BPP",
					outputIndex,
					outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left,
					outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top,
					outputDesc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ? "Yes" : "No",
					outputDesc.BitsPerColor);
			}
		}
		m_pFactory->EnumAdapterByGpuPreference(0, gpuPreference, IID_PPV_ARGS(pAdapter.GetAddressOf()));
		DXGI_ADAPTER_DESC3 desc;
		pAdapter->GetDesc3(&desc);
		E_LOG(Info, "Using %s", UNICODE_TO_MULTIBYTE(desc.Description));

		constexpr D3D_FEATURE_LEVEL featureLevels[] =
		{
			D3D_FEATURE_LEVEL_12_2,
			D3D_FEATURE_LEVEL_12_1,
			D3D_FEATURE_LEVEL_12_0,
			D3D_FEATURE_LEVEL_11_1,
			D3D_FEATURE_LEVEL_11_0
		};
		auto GetFeatureLevelName = [](D3D_FEATURE_LEVEL featureLevel) {
			switch (featureLevel)
			{
			case D3D_FEATURE_LEVEL_12_2: return "D3D_FEATURE_LEVEL_12_2";
			case D3D_FEATURE_LEVEL_12_1: return "D3D_FEATURE_LEVEL_12_1";
			case D3D_FEATURE_LEVEL_12_0: return "D3D_FEATURE_LEVEL_12_0";
			case D3D_FEATURE_LEVEL_11_1: return "D3D_FEATURE_LEVEL_11_1";
			case D3D_FEATURE_LEVEL_11_0: return "D3D_FEATURE_LEVEL_11_0";
			default: noEntry(); return "";
			}
		};

		VERIFY_HR(D3D12CreateDevice(pAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice)));
		D3D12_FEATURE_DATA_FEATURE_LEVELS caps{};
		caps.pFeatureLevelsRequested = featureLevels;
		caps.NumFeatureLevels = ARRAYSIZE(featureLevels);
		VERIFY_HR(pDevice->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &caps, sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)));
		VERIFY_HR(D3D12CreateDevice(pAdapter.Get(), caps.MaxSupportedFeatureLevel, IID_PPV_ARGS(pDevice.ReleaseAndGetAddressOf())));
	}

	if (!pDevice)
	{
		E_LOG(Warning, "No D3D12 Adapter selected. Falling back to WARP");
		m_pFactory->EnumWarpAdapter(IID_PPV_ARGS(pAdapter.GetAddressOf()));
	}
	return pAdapter;
}

std::unique_ptr<GraphicsDevice> GraphicsInstance::CreateDevice(ComPtr<IDXGIAdapter4> pAdapter)
{
	return std::make_unique<GraphicsDevice>(pAdapter.Get());
}

SwapChain::SwapChain(GraphicsDevice* pDevice, IDXGIFactory6* pFactory, WindowHandle pNativeWindow, DXGI_FORMAT format, uint32 width, uint32 height, uint32 numFrames, bool vsync) : m_Format(format), m_CurrentImage(0), m_Vsync(vsync)
{
	DXGI_SWAP_CHAIN_DESC1 desc{};
	desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	desc.BufferCount = numFrames;
	desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	desc.Format = format;
	desc.Width = width;
	desc.Height = height;
	desc.Scaling = DXGI_SCALING_NONE;
	desc.Stereo = FALSE;
	desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	desc.SampleDesc.Count = 1;
	desc.SampleDesc.Quality = 0;

	DXGI_SWAP_CHAIN_FULLSCREEN_DESC fsDesc{};
	fsDesc.RefreshRate.Denominator = 60;
	fsDesc.RefreshRate.Numerator = 1;
	fsDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	fsDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	fsDesc.Windowed = true;

	CommandQueue* pPresentQueue = pDevice->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);
	ComPtr<IDXGISwapChain1> swapChain;
	
#if PLATFORM_UWP
	pFactory->CreateSwapChainForCoreWindow(
		pPresentQueue->GetCommandQueue(),
		reinterpret_cast<IUnknown*>(winrt::get_abi(winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread())),
		&desc,
		nullptr,
		swapChain.GetAddressOf());
#elif PLATFORM_WINDOWS
	VERIFY_HR(pFactory->CreateSwapChainForHwnd(
		pPresentQueue->GetCommandQueue(),
		(HWND)pNativeWindow,
		&desc,
		&fsDesc,
		nullptr,
		swapChain.GetAddressOf()));
#endif
	m_pSwapchain.Reset();
	swapChain.As(&m_pSwapchain);

	m_Backbuffers.resize(numFrames);
	for (uint32 i = 0; i < numFrames; ++i)
	{
		m_Backbuffers[i] = std::make_unique<Texture>(pDevice, "Render Target");
	}
}

void SwapChain::Destroy()
{
	m_pSwapchain->SetFullscreenState(false, nullptr);
}

void SwapChain::OnResize(uint32 width, uint32 height)
{
	for (size_t i = 0; i < m_Backbuffers.size(); ++i)
	{
		m_Backbuffers[i]->Release();
	}

	//Resize the buffers
	DXGI_SWAP_CHAIN_DESC1 desc{};
	m_pSwapchain->GetDesc1(&desc);
	VERIFY_HR(m_pSwapchain->ResizeBuffers(
		(uint32)m_Backbuffers.size(),
		width,
		height,
		desc.Format,
		desc.Flags
	));

	m_CurrentImage = 0;

	//Recreate the render target views
	for (uint32 i = 0; i < (uint32)m_Backbuffers.size(); ++i)
	{
		ID3D12Resource* pResource = nullptr;
		VERIFY_HR(m_pSwapchain->GetBuffer(i, IID_PPV_ARGS(&pResource)));
		m_Backbuffers[i]->CreateForSwapchain(pResource);
	}
}

void SwapChain::Present()
{
	m_pSwapchain->Present(m_Vsync, m_Vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
	m_CurrentImage = m_pSwapchain->GetCurrentBackBufferIndex();
}
