#include "stdafx.h"
#include "Profiler.h"
#include "RHI/Graphics.h"
#include "RHI/CommandContext.h"
#include "RHI/CommandQueue.h"
#include "RHI/Buffer.h"
#include "pix3.h"

void CpuTimer::Begin()
{
	LARGE_INTEGER start;
	QueryPerformanceCounter(&start);
	m_StartTime = start.QuadPart;
}

void CpuTimer::End()
{
	LARGE_INTEGER end;
	QueryPerformanceCounter(&end);
	m_TotalTime = (float)(end.QuadPart - m_StartTime)* Profiler::Get()->GetSecondsPerCpuTick() * 1000.0f;
}

float CpuTimer::GetTime() const
{
	return m_TotalTime;
}

GpuTimer::GpuTimer() 
{
}

void GpuTimer::Begin(CommandContext* pContext)
{
	if (m_TimerIndex == -1)
	{
		m_TimerIndex = Profiler::Get()->GetNextTimerIndex();
	}
	Profiler::Get()->StartGpuTimer(pContext, m_TimerIndex);
}

void GpuTimer::End(CommandContext* pContext)
{
	Profiler::Get()->StopGpuTimer(pContext, m_TimerIndex);
}

float GpuTimer::GetTime(const uint64* pReadbackData) const
{
	if (m_TimerIndex >= 0)
	{
		return Profiler::Get()->GetGpuTime(pReadbackData, m_TimerIndex);
	}
	return 0.0f;
}

void ProfileNode::StartTimer(CommandContext* pContext)
{
	m_CpuTimer.Begin();

	if (pContext)
	{
		m_GpuTimer.Begin(pContext);

#ifdef USE_PIX
		wchar_t name[256];
		size_t written = 0;
		mbstowcs_s(&written, name, m_Name, 256);
		::PIXBeginEvent(pContext->GetCommandList(), 0, name);
#endif
	}
}

void ProfileNode::EndTimer(CommandContext* pContext)
{
	m_CpuTimer.End();
	m_Processed = false;

	if (pContext)
	{
		m_GpuTimer.End(pContext);

#ifdef USE_PIX
		::PIXEndEvent(pContext->GetCommandList());
#endif
	}
}

void ProfileNode::PopulateTimes(const uint64* pReadbackData, int frameIndex)
{
	if (m_Processed == false)
	{
		m_Processed = true;
		m_LastProcessedFrame = frameIndex;
		float cpuTime = m_CpuTimer.GetTime();
		m_CpuTimeHistory.AddTime(cpuTime);
		float gpuTime = m_GpuTimer.GetTime(pReadbackData);
		m_GpuTimeHistory.AddTime(gpuTime);

		for (auto& child : m_Children)
		{
			child->PopulateTimes(pReadbackData, frameIndex);
		}
	}
}

ProfileNode* ProfileNode::GetChild(const char* pName, int i)
{
	StringHash hash(pName);
	auto it = m_Map.find(hash);
	if (it != m_Map.end())
	{
		return it->second;
	}
	std::unique_ptr<ProfileNode> pNewNode = std::make_unique<ProfileNode>(pName, hash, this);
	ProfileNode* pNode = m_Children.insert(m_Children.begin() + i, std::move(pNewNode))._Ptr->get();
	m_Map[hash] = pNode;
	return pNode;
}

bool ProfileNode::HasChild(const char* pName)
{
	StringHash hash(pName);
	return m_Map.find(hash) != m_Map.end();
}

Profiler* Profiler::Get()
{
	static Profiler profiler;
	return &profiler;
}

void Profiler::Initialize(GraphicsDevice* pParent, uint32 numBackbuffers)
{
	D3D12_QUERY_HEAP_DESC desc{};
	desc.Count = HEAP_SIZE;
	desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	desc.NodeMask = 0;
	VERIFY_HR_EX(pParent->GetDevice()->CreateQueryHeap(&desc, IID_PPV_ARGS(m_pQueryHeap.GetAddressOf())), pParent->GetDevice());
	D3D::SetObjectName(m_pQueryHeap.Get(), "Profiler Timestamp Query Heap");

	m_FenceValues.resize(numBackbuffers);
	m_pReadBackBuffer = pParent->CreateBuffer(BufferDesc::CreateReadback(sizeof(uint64) * numBackbuffers * HEAP_SIZE), "Profiling Readback Buffer");

	{
		//GPU Frequency
		uint64 timeStampFrequency;
		pParent->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT)->GetCommandQueue()->GetTimestampFrequency(&timeStampFrequency);
		m_SecondsPerGpuTick = 1.0f / timeStampFrequency;
	}

	{
		//CPU Frequency
		LARGE_INTEGER cpuFrequency;
		QueryPerformanceFrequency(&cpuFrequency);
		m_SecondsPerCpuTick = 1.0f / cpuFrequency.QuadPart;
	}

	m_pRootBlock = std::make_unique<ProfileNode>("", StringHash(""), nullptr);
	m_pCurrentBlock = m_pRootBlock.get();

	ID3D12CommandQueue* pQueue = pParent->GetCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT)->GetCommandQueue();
	OPTICK_GPU_INIT_D3D12(pParent->GetDevice(), &pQueue, 1);
}

void Profiler::Shutdown()
{
	m_pReadBackBuffer.Reset();
	m_pQueryHeap.Reset();
}

void Profiler::Begin(const char* pName, CommandContext* pContext)
{
	if (m_pCurrentBlock->HasChild(pName))
	{
		m_pCurrentBlock = m_pCurrentBlock->GetChild(pName);
		m_pCurrentBlock->StartTimer(pContext);
	}
	else
	{
		uint32 i = 0;
		if (m_pPreviousBlock)
		{
			for (; i < m_pCurrentBlock->GetChildCount(); ++i)
			{
				if (m_pCurrentBlock->GetChild(i) == m_pPreviousBlock)
				{
					++i;
					break;
				}
			}
		}
		m_pCurrentBlock = m_pCurrentBlock->GetChild(pName, i);
		m_pCurrentBlock->StartTimer(pContext);
	}
}

void Profiler::End(CommandContext* pContext)
{
	m_pCurrentBlock->EndTimer(pContext);
	m_pPreviousBlock = m_pCurrentBlock;
	m_pCurrentBlock = m_pCurrentBlock->GetParent();
}

void Profiler::Resolve(SwapChain* pSwapchain, GraphicsDevice* pParent, int frameIndex)
{
	checkf(m_pCurrentBlock == m_pRootBlock.get(), "The current block isn't the root block then something must've gone wrong!");

	//Start the resolve on the current frame
	int offset = MAX_GPU_TIME_QUERIES * QUERY_PAIR_NUM * m_CurrentReadbackFrame;
	CommandContext* pContext = pParent->AllocateCommandContext(D3D12_COMMAND_LIST_TYPE_DIRECT);
	pContext->GetCommandList()->ResolveQueryData(m_pQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, m_CurrentTimer * QUERY_PAIR_NUM, m_pReadBackBuffer->GetResource(), offset * sizeof(uint64));
	m_FenceValues[m_CurrentReadbackFrame] = pContext->Execute(false);

	int numFrames = (int)m_FenceValues.size();
	if (frameIndex >= numFrames)
	{
		//Make sure the resolve from 2 frames ago is finished before we read.
		uint32 readFromIndex = (m_CurrentReadbackFrame + numFrames - 1) % numFrames;
		pParent->WaitForFence(m_FenceValues[readFromIndex]);
		m_pCurrentBlock->PopulateTimes((const uint64*)m_pReadBackBuffer->GetMappedData(), frameIndex - 2);
	}
	m_CurrentReadbackFrame = (m_CurrentReadbackFrame + 1) % numFrames;

	m_pPreviousBlock = nullptr;
	m_pCurrentBlock->StartTimer(nullptr);
	m_pCurrentBlock->EndTimer(nullptr);

	OPTICK_GPU_FLIP(pSwapchain->GetSwapChain());
	OPTICK_CATEGORY("Present", Optick::Category::Wait);
}

float Profiler::GetGpuTime(const uint64* pReadbackData, int timerIndex) const
{
	check(timerIndex >= 0);
	check(pReadbackData);
	uint64 start = pReadbackData[timerIndex * QUERY_PAIR_NUM];
	uint64 end = pReadbackData[timerIndex * QUERY_PAIR_NUM + 1];
	float time = (float)((end - start) * m_SecondsPerGpuTick * 1000.0);
	return time;
}

void Profiler::StartGpuTimer(CommandContext* pContext, int timerIndex)
{
	pContext->GetCommandList()->EndQuery(Profiler::Get()->GetQueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, timerIndex * QUERY_PAIR_NUM);
}

void Profiler::StopGpuTimer(CommandContext* pContext, int timerIndex)
{
	pContext->GetCommandList()->EndQuery(Profiler::Get()->GetQueryHeap(), D3D12_QUERY_TYPE_TIMESTAMP, timerIndex * QUERY_PAIR_NUM + 1);
}

int32 Profiler::GetNextTimerIndex()
{
	check(m_CurrentTimer < MAX_GPU_TIME_QUERIES);
	return m_CurrentTimer++;
}

/// IMGUI

void ProfileNode::RenderImGui(int frameIndex)
{
	ImGui::Spacing();
	if (ImGui::BeginTable("Profiling", 5, ImGuiTableFlags_SizingStretchProp))
	{
		ImGui::TableSetupColumn("Event", ImGuiTableColumnFlags_None, 3);
		ImGui::TableSetupColumn("CPU", ImGuiTableColumnFlags_None, 6);
		ImGui::TableSetupColumn("CPU (ms)", ImGuiTableColumnFlags_None, 1);
		ImGui::TableSetupColumn("GPU", ImGuiTableColumnFlags_None, 6);
		ImGui::TableSetupColumn("GPU (ms)", ImGuiTableColumnFlags_None, 1);
		ImGui::TableHeadersRow();

		for (auto& pChild : m_Children)
		{
			pChild->RenderNodeImgui(frameIndex);
		}
		ImGui::EndTable();
	}

	ImGui::Separator();
}

void ProfileNode::RenderNodeImgui(int frameIndex)
{
	if (frameIndex - m_LastProcessedFrame < 60)
	{
		static const ImColor CpuColor = ImColor(0, 125, 200);
		static const ImColor GpuColor = ImColor(120, 183, 0);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::PushID(m_Hash);

		bool expand = false;
		if (m_Children.size() > 0)
		{
			expand = ImGui::TreeNodeEx(m_Name, m_Children.size() > 2 ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None);
		}
		else
		{
			ImGui::Bullet();
			ImGui::Selectable(m_Name);
		}

		ImGui::PushStyleColor(ImGuiCol_PlotLines, ImU32(CpuColor));
		ImGui::PushStyleColor(ImGuiCol_Text, ImU32(CpuColor));
		float cpuTime = m_CpuTimeHistory.GetAverage();

		// 0
		{
			ImGui::TableNextColumn();

			if (cpuTime > 0)
			{
				const float* pData;
				uint32 offset, count;
				m_CpuTimeHistory.GetHistory(&pData, &count, &offset);

				ImGui::PlotLines("", pData, count, offset, 0, 0.0f, 0.03f, ImVec2(ImGui::GetColumnWidth(), 0));
			}
		}
		// 1
		{

			ImGui::TableNextColumn();

			ImGui::Text("%4.2f ms", cpuTime);
		}


		ImGui::PopStyleColor(2);

		ImGui::PushStyleColor(ImGuiCol_PlotLines, ImU32(GpuColor));
		ImGui::PushStyleColor(ImGuiCol_Text, ImU32(GpuColor));

		float gpuTime = m_GpuTimeHistory.GetAverage();
		// 2
		{
			ImGui::TableNextColumn();

			if (gpuTime > 0)
			{
				const float* pData;
				uint32 offset, count;
				m_GpuTimeHistory.GetHistory(&pData, &count, &offset);

				ImGui::PlotLines("", pData, count, offset, 0, 0.0f, 0.03f, ImVec2(ImGui::GetColumnWidth(), 0));
			}
		}

		// 3
		{
			ImGui::TableNextColumn();

			if (gpuTime > 0)
			{
				ImGui::Text("%4.2f ms", gpuTime);
			}
			else
			{
				ImGui::Text("N/A");
			}
		}

		ImGui::PopStyleColor(2);

		if (expand)
		{
			for (auto& childNode : m_Children)
			{
				childNode->RenderNodeImgui(frameIndex);
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}
