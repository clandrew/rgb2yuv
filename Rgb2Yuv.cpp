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

class TextureLoader
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

		DX::ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&result)));

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(result.Get(), 0, 1);

		ComPtr<ID3D12Resource> upload;
		DX::ThrowIfFailed(device->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
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

struct DeviceResources
{
	ComPtr<ID3D12Device> device;
	ComPtr<ID3D12CommandAllocator> commandAllocator;
	ComPtr<ID3D12GraphicsCommandList> graphicsCommandList;
	ComPtr<ID3D12CommandQueue> graphicsCommandQueue;

	ComPtr<ID3D12RootSignature> computeRootSignature;
	ComPtr<ID3D12PipelineState> computePipelineState;

	ComPtr<ID3D12Fence> fence;
	UINT fenceValue;
	HANDLE fenceEvent;

	ComPtr<ID3D12Resource> yuvResource;
	ComPtr<ID3D12DescriptorHeap> uavHeap;

	void Initialize()
	{
		device = CreateDevice();

		DX::ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));

		{
			D3D12_COMMAND_QUEUE_DESC queueDesc = {};
			queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			DX::ThrowIfFailed(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&graphicsCommandQueue)));
			NAME_D3D12_OBJECT(graphicsCommandQueue);
		}

		DX::ThrowIfFailed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), nullptr, IID_PPV_ARGS(&graphicsCommandList)));
		NAME_D3D12_OBJECT(graphicsCommandList);

		DX::ThrowIfFailed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
		fenceValue = 0;
		fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (fenceEvent == nullptr)
		{
			DX::ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
		}
	}

	void CloseCommandListExecuteAndWaitUntilDone()
	{
		DX::ThrowIfFailed(graphicsCommandList->Close());

		ID3D12CommandList* commandLists[] = { graphicsCommandList.Get() };
		graphicsCommandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);

		++fenceValue;

		DX::ThrowIfFailed(graphicsCommandQueue->Signal(fence.Get(), fenceValue));

		if (fence->GetCompletedValue() < fenceValue)
		{
			DX::ThrowIfFailed(fence->SetEventOnCompletion(fenceValue, fenceEvent));
			WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
		}

		DX::ThrowIfFailed(commandAllocator->Reset());
		DX::ThrowIfFailed(graphicsCommandList->Reset(commandAllocator.Get(), nullptr));
	}
};

void DoConversion(ID3D12Resource* rgb, DeviceResources* deviceResources)
{
	D3D12_RESOURCE_DESC loadedImageResourceDesc = rgb->GetDesc();

	D3D12_DESCRIPTOR_HEAP_DESC uavHeapDesc = {};
	uavHeapDesc.NumDescriptors = 3;
	uavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	uavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	DX::ThrowIfFailed(deviceResources->device->CreateDescriptorHeap(&uavHeapDesc, IID_PPV_ARGS(&deviceResources->uavHeap)));
	NAME_D3D12_OBJECT(deviceResources->uavHeap);

	// Create render target resource
	D3D12_RESOURCE_DESC resourceDesc{};
	resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	resourceDesc.Width = loadedImageResourceDesc.Width;
	resourceDesc.Height = loadedImageResourceDesc.Height;
	resourceDesc.MipLevels = 1;
	resourceDesc.DepthOrArraySize = 1;
	resourceDesc.Format = DXGI_FORMAT_NV12;
	resourceDesc.SampleDesc.Count = 1;
	resourceDesc.SampleDesc.Quality = 0;
	resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

	DX::ThrowIfFailed(deviceResources->device->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&resourceDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		nullptr,
		IID_PPV_ARGS(&deviceResources->yuvResource)));

	CD3DX12_GPU_DESCRIPTOR_HANDLE uavHeapGpu(deviceResources->uavHeap->GetGPUDescriptorHandleForHeapStart());
	UINT uavHandleSize = deviceResources->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	CD3DX12_CPU_DESCRIPTOR_HANDLE rgbUavCpu(deviceResources->uavHeap->GetCPUDescriptorHandleForHeapStart(),				0 * uavHandleSize);
	CD3DX12_CPU_DESCRIPTOR_HANDLE yuvLuminanceUavCpu(deviceResources->uavHeap->GetCPUDescriptorHandleForHeapStart(),	1 * uavHandleSize);
	CD3DX12_CPU_DESCRIPTOR_HANDLE yuvChrominanceUavCpu(deviceResources->uavHeap->GetCPUDescriptorHandleForHeapStart(),	2 * uavHandleSize);

	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // Selects the luminance plane
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		deviceResources->device->CreateUnorderedAccessView(rgb, nullptr, &uavDesc, rgbUavCpu);
	}
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R8_UNORM; // Selects the luminance plane
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		deviceResources->device->CreateUnorderedAccessView(deviceResources->yuvResource.Get(), nullptr, &uavDesc, yuvLuminanceUavCpu);
	}
	{
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R8G8_UNORM; // Selects the luminance plane
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.PlaneSlice = 1;
		deviceResources->device->CreateUnorderedAccessView(deviceResources->yuvResource.Get(), nullptr, &uavDesc, yuvChrominanceUavCpu);
	}

	CD3DX12_DESCRIPTOR_RANGE ranges[1];
	CD3DX12_ROOT_PARAMETER parameter;

	ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);

	parameter.InitAsDescriptorTable(_countof(ranges), ranges, D3D12_SHADER_VISIBILITY_ALL);

	D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

	CD3DX12_ROOT_SIGNATURE_DESC descRootSignature;
	descRootSignature.Init(1, &parameter, 0, nullptr, rootSignatureFlags);

	ComPtr<ID3DBlob> pSignature;
	ComPtr<ID3DBlob> pError;
	DX::ThrowIfFailed(D3D12SerializeRootSignature(&descRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, pSignature.GetAddressOf(), pError.GetAddressOf()));
	DX::ThrowIfFailed(deviceResources->device->CreateRootSignature(0, pSignature->GetBufferPointer(), pSignature->GetBufferSize(), IID_PPV_ARGS(&deviceResources->computeRootSignature)));

	D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{};
	pipelineStateDesc.pRootSignature = deviceResources->computeRootSignature.Get();
	pipelineStateDesc.CS = CD3DX12_SHADER_BYTECODE((void*)(g_rgbToYuvCS), _countof(g_rgbToYuvCS));
	DX::ThrowIfFailed(deviceResources->device->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&deviceResources->computePipelineState)));

	// Do render for format conversion
	deviceResources->graphicsCommandList->SetComputeRootSignature(deviceResources->computeRootSignature.Get());
	deviceResources->graphicsCommandList->SetPipelineState(deviceResources->computePipelineState.Get());
	ID3D12DescriptorHeap* heaps[] = { deviceResources->uavHeap.Get() };
	deviceResources->graphicsCommandList->SetDescriptorHeaps(_countof(heaps), heaps);
	CD3DX12_GPU_DESCRIPTOR_HANDLE unorderedAccessViewGpu(deviceResources->uavHeap->GetGPUDescriptorHandleForHeapStart());
	deviceResources->graphicsCommandList->SetComputeRootDescriptorTable(0, unorderedAccessViewGpu);
	deviceResources->graphicsCommandList->Dispatch(loadedImageResourceDesc.Width, loadedImageResourceDesc.Height, 1);
}

int main()
{
	ComPtr<IDXGraphicsAnalysis> graphicsAnalysis;
	DXGIGetDebugInterface1(0, IID_PPV_ARGS(&graphicsAnalysis));

	DeviceResources deviceResources;
	deviceResources.Initialize();

	// Load source image
	TextureLoader textureLoader;
	ComPtr<ID3D12Resource> resource = textureLoader.LoadTextureFromPngFile(L"lcm.png", D3D12_RESOURCE_STATE_UNORDERED_ACCESS, deviceResources.device.Get(), deviceResources.graphicsCommandList.Get());
	deviceResources.CloseCommandListExecuteAndWaitUntilDone();

	// Do format conversion

	if (graphicsAnalysis)
		graphicsAnalysis->BeginCapture();

	DoConversion(resource.Get(), &deviceResources);
	deviceResources.CloseCommandListExecuteAndWaitUntilDone();

	if (graphicsAnalysis)
		graphicsAnalysis->EndCapture();

    // Verify w PIX
}