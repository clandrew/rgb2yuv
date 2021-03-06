// Rgb2Yuv.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include "pch.h"
#include "ShaderStructures.h"
#include "RgbToYuvCS.hlsl.h"

using namespace DirectX;

#define NAME_D3D12_OBJECT(x) DX::SetName(x.Get(), L#x)

namespace DX
{
	inline void ThrowIfFailed(HRESULT hr)
	{
		if (FAILED(hr))
		{
			// Set a breakpoint on this line to catch Win32 API errors.
			_com_issue_error(hr);
		}
	}

#if defined(_DEBUG)
	inline void SetName(ID3D12Object* pObject, LPCWSTR name)
	{
		pObject->SetName(name);
	}
#else
	inline void SetName(ID3D12Object*, LPCWSTR)
	{
	}
#endif
}

interface DECLSPEC_UUID("9f251514-9d4d-4902-9d60-18988ab7d4b5") DECLSPEC_NOVTABLE IDXGraphicsAnalysis : public IUnknown
{
	STDMETHOD_(void, BeginCapture)() PURE;
	STDMETHOD_(void, EndCapture)() PURE;
};


struct QueueWrapper
{
	ComPtr<ID3D12Fence> m_fence;
	UINT m_fenceValue;
	HANDLE m_fenceEvent;

	ComPtr<ID3D12CommandAllocator> m_commandAllocator;
	ComPtr<ID3D12CommandQueue> m_graphicsCommandQueue;

public:
	ComPtr<ID3D12GraphicsCommandList> graphicsCommandList;

	void Initialize(ID3D12Device* device)
	{
		DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));

		{
			D3D12_COMMAND_QUEUE_DESC queueDesc = {};
			queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			DX::ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_graphicsCommandQueue)));
			NAME_D3D12_OBJECT(m_graphicsCommandQueue);
		}

		DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&graphicsCommandList)));
		NAME_D3D12_OBJECT(graphicsCommandList);

		DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
		m_fenceValue = 0;
		m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_fenceEvent == nullptr)
		{
			DX::ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}
	}

	void CloseCommandListExecuteAndWaitUntilDone()
	{
		DX::ThrowIfFailed(graphicsCommandList->Close());

		ID3D12CommandList* commandLists[] = { graphicsCommandList.Get() };
		m_graphicsCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

		++m_fenceValue;

		DX::ThrowIfFailed(m_graphicsCommandQueue->Signal(m_fence.Get(), m_fenceValue));

		if (m_fence->GetCompletedValue() < m_fenceValue)
		{
			DX::ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent));
			WaitForSingleObjectEx(m_fenceEvent, INFINITE, FALSE);
		}

		DX::ThrowIfFailed(m_commandAllocator->Reset());
		DX::ThrowIfFailed(graphicsCommandList->Reset(m_commandAllocator.Get(), nullptr));
	}
};

class RgbTextureLoader
{
	ComPtr<IWICImagingFactory> m_wicImagingFactory;
	std::vector<ComPtr<ID3D12Resource>> m_uploads;

public:
	ComPtr<ID3D12Resource> LoadTextureFromPngFile(
		std::wstring const& fileName, 
		D3D12_RESOURCE_STATES state, 
		ID3D12Device* device, 
		ID3D12GraphicsCommandList* graphicsCommandList)
	{
		if (!m_wicImagingFactory)
		{
			DX::ThrowIfFailed(CoInitialize(nullptr));

			DX::ThrowIfFailed(CoCreateInstance(
				CLSID_WICImagingFactory,
				NULL,
				CLSCTX_INPROC_SERVER,
				IID_IWICImagingFactory,
				(LPVOID*)&m_wicImagingFactory));
		}

		struct LoadedImageData
		{
			UINT ImageWidth;
			UINT ImageHeight;
			std::vector<UINT> Buffer;
		};
		LoadedImageData imageData{};

		ComPtr<IWICBitmapDecoder> decoder;
		DX::ThrowIfFailed(m_wicImagingFactory->CreateDecoderFromFilename(
			fileName.c_str(),
			NULL,
			GENERIC_READ,
			WICDecodeMetadataCacheOnLoad, &decoder));

		ComPtr<IWICBitmapFrameDecode> spSource;
		DX::ThrowIfFailed(decoder->GetFrame(0, &spSource));

		ComPtr<IWICFormatConverter> spConverter;
		DX::ThrowIfFailed(m_wicImagingFactory->CreateFormatConverter(&spConverter));

		DX::ThrowIfFailed(spConverter->Initialize(
			spSource.Get(),
			GUID_WICPixelFormat32bppPBGRA,
			WICBitmapDitherTypeNone,
			NULL,
			0.f,
			WICBitmapPaletteTypeMedianCut));

		DX::ThrowIfFailed(spConverter->GetSize(&imageData.ImageWidth, &imageData.ImageHeight));

		imageData.Buffer.resize(imageData.ImageWidth * imageData.ImageHeight);
		DX::ThrowIfFailed(spConverter->CopyPixels(
			NULL,
			imageData.ImageWidth * sizeof(UINT),
			static_cast<UINT>(imageData.Buffer.size()) * sizeof(UINT),
			reinterpret_cast<BYTE*>(imageData.Buffer.data())));

		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		resourceDesc.Width = imageData.ImageWidth;
		resourceDesc.Height = imageData.ImageHeight;
		resourceDesc.MipLevels = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.SampleDesc.Quality = 0;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		ComPtr<ID3D12Resource> result;

		auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		DX::ThrowIfFailed(device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&result)));

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(result.Get(), 0, 1);

		auto uploadHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
		auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
		ComPtr<ID3D12Resource> upload;
		DX::ThrowIfFailed(device->CreateCommittedResource(
			&uploadHeap,
			D3D12_HEAP_FLAG_NONE,
			&bufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&upload)));

		D3D12_SUBRESOURCE_DATA initialData{};
		initialData.pData = imageData.Buffer.data();
		initialData.RowPitch = imageData.ImageWidth * 4;
		initialData.SlicePitch = imageData.ImageWidth * imageData.ImageHeight * 4;
		UpdateSubresources(graphicsCommandList, result.Get(), upload.Get(), 0, 0, 1, &initialData);

		m_uploads.push_back(upload);

		CD3DX12_RESOURCE_BARRIER resourceBarrier =
			CD3DX12_RESOURCE_BARRIER::Transition(result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
		graphicsCommandList->ResourceBarrier(1, &resourceBarrier);

		return result;
	}
		
	void SaveTextureToPngFile(
		ID3D12Resource* texture,
		D3D12_RESOURCE_STATES preState,
		D3D12_RESOURCE_STATES postState,
		std::wstring const& fileName,
		ID3D12Device* device,
		QueueWrapper* directQueueWrapper)
	{
		assert(false); // TODO: fix implementation

		ComPtr<ID3D12Resource> download;

		UINT64 footprintRowPitch = 0;
		{
			// Texture needs to be in copy source
			if (preState != D3D12_RESOURCE_STATE_COPY_SOURCE)
			{
				CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(texture, preState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				directQueueWrapper->graphicsCommandList->ResourceBarrier(1, &b);
			}

			D3D12_RESOURCE_DESC upscaledRgbDesc = texture->GetDesc();

			UINT64 totalResourceSize = 0;
			UINT fpRowCount = 0;

			device->GetCopyableFootprints(
				&upscaledRgbDesc,
				0,
				1,
				0,
				nullptr,
				&fpRowCount,
				&footprintRowPitch,
				&totalResourceSize);
			UINT alignedFootprintRowPitch = AlignTo256(footprintRowPitch);

			auto readbackHeapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
			int bufferRowPitch = AlignTo256(static_cast<UINT>(upscaledRgbDesc.Width) * 2); // Required for NV12
			int bufferSize = AlignTo256(bufferRowPitch * upscaledRgbDesc.Height * 2);
			auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);
			DX::ThrowIfFailed(device->CreateCommittedResource(
				&readbackHeapType,
				D3D12_HEAP_FLAG_NONE,
				&bufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&download)));

			D3D12_PLACED_SUBRESOURCE_FOOTPRINT downloadFootprint = {};
			downloadFootprint.Footprint.Width = static_cast<UINT>(upscaledRgbDesc.Width);
			downloadFootprint.Footprint.Height = upscaledRgbDesc.Height;
			downloadFootprint.Footprint.Depth = 1;
			downloadFootprint.Footprint.RowPitch = AlignTo256(static_cast<UINT>(upscaledRgbDesc.Width) * 2);

			downloadFootprint.Footprint.Format = DXGI_FORMAT_R8_TYPELESS;

			CD3DX12_TEXTURE_COPY_LOCATION copyDest(download.Get(), downloadFootprint);
			CD3DX12_TEXTURE_COPY_LOCATION copySrc(texture, 0);

			directQueueWrapper->graphicsCommandList->CopyTextureRegion(&copyDest, 0, 0, 0, &copySrc, nullptr);

			// Texture needs to be in copy source
			if (postState != D3D12_RESOURCE_STATE_COPY_SOURCE)
			{
				CD3DX12_RESOURCE_BARRIER b = CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, postState);
				directQueueWrapper->graphicsCommandList->ResourceBarrier(1, &b);
			}
		}
		directQueueWrapper->CloseCommandListExecuteAndWaitUntilDone();

		D3D12_RESOURCE_DESC desc = texture->GetDesc();

		ComPtr<IWICBitmapEncoder> encoder;
		if (FAILED(m_wicImagingFactory->CreateEncoder(GUID_ContainerFormatPng, NULL, &encoder)))
		{
			return;
		}

		ComPtr<IWICStream> stream;
		if (FAILED(m_wicImagingFactory->CreateStream(&stream)))
		{
			return;
		}

		if (FAILED(stream->InitializeFromFilename(fileName.c_str(), GENERIC_WRITE)))
		{
			return;
		}

		if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
		{
			return;
		}

		// Put all the pixels in bitmap

		ComPtr<IWICBitmapFrameEncode> frameEncode;
		if (FAILED(encoder->CreateNewFrame(&frameEncode, nullptr)))
		{
			return;
		}

		if (FAILED(frameEncode->Initialize(nullptr)))
		{
			return;
		}
		if (FAILED(frameEncode->SetSize(desc.Width, desc.Height)))
		{
			return;
		}

		if (FAILED(frameEncode->SetResolution(96, 96)))
		{
			return;
		}

		WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
		if (FAILED(frameEncode->SetPixelFormat(&pixelFormat)))
		{
			return;
		}

		ComPtr<IWICBitmap> wicBitmap;
		if (FAILED(m_wicImagingFactory->CreateBitmap(
			desc.Width,
			desc.Height,
			GUID_WICPixelFormat32bppPBGRA,
			WICBitmapCacheOnDemand,
			&wicBitmap)))
		{
			return;
		}
		
		// Here: Lock resource and put it in the bitmap
		{
			WICRect lockRect{};
			lockRect.Width = desc.Width;
			lockRect.Height = desc.Height;

			ComPtr<IWICBitmapLock> wicBitmapLock;
			if (FAILED(wicBitmap->Lock(&lockRect, WICBitmapLockWrite, &wicBitmapLock)))
			{
				return;
			}

			UINT wicBitmapStride{};
			if (FAILED(wicBitmapLock->GetStride(&wicBitmapStride)))
			{
				return;
			}

			UINT bufferSize;
			byte* wicBitmapData;
			if (FAILED(wicBitmapLock->GetDataPointer(&bufferSize, &wicBitmapData)))
			{
				return;
			}

			size_t imageSizeInBytes = footprintRowPitch * UINT64(desc.Height);
			void* mappedMemory = nullptr;
			D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(imageSizeInBytes) };
			if (FAILED(download->Map(0, &readRange, &mappedMemory)))
			{
				return;
			}

			byte* src = static_cast<byte*>(mappedMemory);
			byte* dst = wicBitmapData;

			for (int y = 0; y < desc.Height; ++y)
			{
				memcpy(dst, src, desc.Width * 4);
				src += footprintRowPitch;
				dst += wicBitmapStride;
			}

			download->Unmap(0, nullptr);
		}


		if (FAILED(frameEncode->WriteSource(
			wicBitmap.Get(),
			NULL)))
		{
			return;
		}

		if (FAILED(frameEncode->Commit()))
		{
			return;
		}
		if (FAILED(encoder->Commit()))
		{
			return;
		}

		if (FAILED(stream->Commit(STGC_DEFAULT)))
		{
			return;
		}

	}

	private:

		int AlignTo256(int i)
		{
			if ((i % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT) != 0)
			{
				i += D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - (i % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
			}
			else
			{
				return i;
			}
		}
};

ComPtr<IDXGIAdapter1> GetHardwareAdapter(IDXGIFactory4* dxgiFactory)
{
	ComPtr<IDXGIAdapter1> adapter;

	for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != dxgiFactory->EnumAdapters1(adapterIndex, &adapter); adapterIndex++)
	{
		DXGI_ADAPTER_DESC1 desc;
		adapter->GetDesc1(&desc);

		if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
		{
			// Don't select the Basic Render Driver adapter.
			continue;
		}

		// Check to see if the adapter supports Direct3D 12, but don't create the
		// actual device yet.
		if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
		{
			break;
		}
	}

	return adapter;
}

ComPtr<ID3D12Device> CreateDevice()
{
#if defined(_DEBUG)
	ComPtr<ID3D12Debug1> debugController;
	if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
	{
		debugController->EnableDebugLayer();
		debugController->SetEnableGPUBasedValidation(TRUE);
	}
#endif

	ComPtr<IDXGIFactory4> dxgiFactory;
	DX::ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));
	ComPtr<IDXGIAdapter1> adapter = GetHardwareAdapter(dxgiFactory.Get());

	ComPtr<ID3D12Device> device;
	HRESULT hr = D3D12CreateDevice(
		adapter.Get(),					// The hardware adapter.
		D3D_FEATURE_LEVEL_11_0,			// Minimum feature level this app can support.
		IID_PPV_ARGS(&device)		// Returns the Direct3D device created.
	);

	if (FAILED(hr)) // Fall back to WARP
	{
		ComPtr<IDXGIAdapter> warpAdapter;
		DX::ThrowIfFailed(dxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&warpAdapter)));

		hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
	}
	DX::ThrowIfFailed(hr);

	return device;
}

struct DescriptorHeapWrapper
{
	ComPtr<ID3D12DescriptorHeap> m_cbvSrvUavHeap;

	CD3DX12_GPU_DESCRIPTOR_HANDLE m_cbvSrvUavGpu;

	// Heap contents, in order
	static const int m_descriptorCount = 3;
	CD3DX12_CPU_DESCRIPTOR_HANDLE m_rgbUavCpu;
	CD3DX12_CPU_DESCRIPTOR_HANDLE m_yuvLuminanceUavCpu;
	CD3DX12_CPU_DESCRIPTOR_HANDLE m_yuvChrominanceUavCpu;

	void Initialize(ID3D12Resource* rgb, ID3D12Resource* yuv, ID3D12Device* device)
	{
		UINT uavHandleSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
		uavHeapDesc.NumDescriptors = m_descriptorCount;
		uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		DX::ThrowIfFailed(device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&m_cbvSrvUavHeap)));
		NAME_D3D12_OBJECT(m_cbvSrvUavHeap);

		m_rgbUavCpu.InitOffsetted(m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 0, uavHandleSize);
		m_yuvLuminanceUavCpu.InitOffsetted(m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 1, uavHandleSize);
		m_yuvChrominanceUavCpu.InitOffsetted(m_cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart(), 2, uavHandleSize);

		m_cbvSrvUavGpu = m_cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();

		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Rgb source
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(rgb, nullptr, &uavDesc, m_rgbUavCpu);
		}
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R8_UNORM; // Selects the luminance plane
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(yuv, nullptr, &uavDesc, m_yuvLuminanceUavCpu);
		}
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R8G8_UNORM; // Selects the chrominance plane
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.PlaneSlice = 1;
			device->CreateUnorderedAccessView(yuv, nullptr, &uavDesc, m_yuvChrominanceUavCpu);
		}
	}
};

struct RootSignatureWrapper
{
	ComPtr<ID3D12RootSignature> m_computeRootSignature;

	void Initialize(DescriptorHeapWrapper* cbvSrvUav, ID3D12Device* device)
	{
		CD3DX12_DESCRIPTOR_RANGE ranges[1];
		CD3DX12_ROOT_PARAMETER parameters[2];

		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, cbvSrvUav->m_descriptorCount, 0);

		parameters[0].InitAsDescriptorTable(_countof(ranges), ranges);
		parameters[1].InitAsConstants(2, 0);

		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

		CD3DX12_ROOT_SIGNATURE_DESC descRootSignature;
		descRootSignature.Init(_countof(parameters), parameters, 0, nullptr, rootSignatureFlags);

		ComPtr<ID3DBlob> pSignature;
		ComPtr<ID3DBlob> pError;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&descRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, pSignature.GetAddressOf(), pError.GetAddressOf()));
		DX::ThrowIfFailed(device->CreateRootSignature(0, pSignature->GetBufferPointer(), pSignature->GetBufferSize(), IID_PPV_ARGS(&m_computeRootSignature)));

	}
};

struct PipelineStateWrapper
{
	ComPtr<ID3D12PipelineState> m_computePipelineState;

	void Initialize(ID3D12RootSignature* rootSignature, ID3D12Device* device)
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{};
		pipelineStateDesc.pRootSignature = rootSignature;
		pipelineStateDesc.CS = CD3DX12_SHADER_BYTECODE((void*)(g_rgbToYuvCS), _countof(g_rgbToYuvCS));
		DX::ThrowIfFailed(device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_computePipelineState)));
	}
};

ComPtr<ID3D12Resource> CreateCompatibleYuvResource(ID3D12Resource* rgb, ID3D12Device* device)
{
	D3D12_RESOURCE_DESC loadedImageResourceDesc = rgb->GetDesc();

	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

	// NV12 needs to have multiple-of-two size.
	resourceDesc.Width = loadedImageResourceDesc.Width;
	if (resourceDesc.Width % 2 != 0)
		++resourceDesc.Width;

	resourceDesc.Height = loadedImageResourceDesc.Height;
	if (resourceDesc.Height % 2 != 0)
		++resourceDesc.Height;

	resourceDesc.MipLevels = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.Format = DXGI_FORMAT_NV12;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	auto defaultHeap = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
	ComPtr<ID3D12Resource> result;
	DX::ThrowIfFailed(device->CreateCommittedResource(
		&defaultHeap,
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&result)));
	DX::SetName(result.Get(), L"compatibleYUV");

	return result;
}

void ConvertRgbToYuv(
	ID3D12Resource* yuv,
	ID3D12GraphicsCommandList* cl,
	DescriptorHeapWrapper* cbvSrvUav,
	RootSignatureWrapper* rootSig,
	PipelineStateWrapper* pipelineState)
{
	D3D12_RESOURCE_DESC targetResourceDesc = yuv->GetDesc();

	// Run compute for format conversion
	cl->SetComputeRootSignature(rootSig->m_computeRootSignature.Get());
	cl->SetPipelineState(pipelineState->m_computePipelineState.Get());
	ID3D12DescriptorHeap* heaps[] = { cbvSrvUav->m_cbvSrvUavHeap.Get() };
	cl->SetDescriptorHeaps(_countof(heaps), heaps);

	cl->SetComputeRootDescriptorTable(0, cbvSrvUav->m_cbvSrvUavGpu);
	UINT rootConstants[2] = { static_cast<UINT>(targetResourceDesc.Width), targetResourceDesc.Height };
	cl->SetComputeRoot32BitConstants(1, 2, rootConstants, 0);

	UINT dispatchX = static_cast<UINT>(targetResourceDesc.Width) / 64 + 1;
	UINT dispatchY = targetResourceDesc.Height;
	cl->Dispatch(dispatchX, dispatchY, 1);
}

void PrintUsage()
{
	std::cout << "Usage: Rgb2Yuv.exe [sourceImageFile]\n";
}

int main(int argc, void** argv)
{
	if (argc != 2)
	{
		PrintUsage();
		return -1;
	}

	void* arg1 = argv[1];
	std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> textConverter;
	std::wstring imageFileName = textConverter.from_bytes((char*)arg1);

	ComPtr<IDXGraphicsAnalysis> graphicsAnalysis;
	DXGIGetDebugInterface1(0, IID_PPV_ARGS(&graphicsAnalysis));

	ComPtr<ID3D12Device> device = CreateDevice();

	QueueWrapper queueWrapper;
	queueWrapper.Initialize(device.Get());

	// Load source image
	RgbTextureLoader textureLoader;
	ComPtr<ID3D12Resource> rgb = textureLoader.LoadTextureFromPngFile(imageFileName.c_str(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, device.Get(), queueWrapper.graphicsCommandList.Get());
	queueWrapper.CloseCommandListExecuteAndWaitUntilDone();

	// Do format conversion
	if (graphicsAnalysis)
		graphicsAnalysis->BeginCapture();

	ComPtr<ID3D12Resource> yuv = CreateCompatibleYuvResource(rgb.Get(), device.Get());

	DescriptorHeapWrapper cbvSrvUav;
	cbvSrvUav.Initialize(rgb.Get(), yuv.Get(), device.Get());

	RootSignatureWrapper rootSignature;
	rootSignature.Initialize(&cbvSrvUav, device.Get());

	PipelineStateWrapper pipelineState;
	pipelineState.Initialize(rootSignature.m_computeRootSignature.Get(), device.Get());

	ConvertRgbToYuv(yuv.Get(), queueWrapper.graphicsCommandList.Get(), &cbvSrvUav, &rootSignature, &pipelineState);
	queueWrapper.CloseCommandListExecuteAndWaitUntilDone();

	/*textureLoader.SaveTextureToPngFile(
		yuv.Get(),
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		L"out.png",
		device.Get(),
		&queueWrapper);*/

	if (graphicsAnalysis)
		graphicsAnalysis->EndCapture();

	return 0;
}