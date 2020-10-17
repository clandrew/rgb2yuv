// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

// add headers that you want to pre-compile here
#include <d3d12.h>
#include <DirectXMath.h>
#include "d3dx12.h"
#include <dxgi1_6.h>

#include <wincodec.h>

#include <wrl/client.h>
#include <comdef.h>

#include <string>
#include <vector>

using namespace Microsoft::WRL;

#endif //PCH_H
