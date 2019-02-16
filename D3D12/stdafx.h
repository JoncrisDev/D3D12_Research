// stdafx.h : include file for standard system include files,
// or project specific include files that are used frequently, but
// are changed infrequently
//
#pragma once

#if defined(_MSC_FULL_VER)
using int8 = __int8;
using int16 = __int16;
using int32 = __int32;
using int64 = __int64;

using uint8 = unsigned __int8;
using uint16 = unsigned __int16;
using uint32 = unsigned __int32;
using uint64 = unsigned __int64;
#else
using int8 = int8_t;
using int16 = int16_t;
using int32 = int32_t;
using int64 = int64_t;

using uint8 = uint8_t;
using uint16 = uint16_t;
using uint32 = uint32_t;
using uint64 = uint64_t;
#endif

#define NOMINMAX
#include <Windows.h>
#include <wrl/client.h>

#include <d3d12.h>
#include "d3dx12.h"
#include <string>

#include <dxgi1_4.h>

#include <DirectXMath.h>
#include <DirectXColors.h>
#include <DirectXPackedVector.h>
#include <DirectXCollision.h>
#include "External/SimpleMath/SimpleMath.h"

#include <D3Dcompiler.h>

#include <queue>
#include <vector>
#include <iostream>
#include <memory>
#include <array>
#include <queue>
#include <mutex>
#include <map>
#include "D3DUtils.h"

using namespace DirectX;
using namespace SimpleMath;
using namespace Microsoft::WRL;

using Rect = SimpleMath::Rectangle;