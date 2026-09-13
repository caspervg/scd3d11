#include <d3d11.h>
#include <wrl/client.h>

#include <iostream>
#include <vector>

using Microsoft::WRL::ComPtr;

int main() {
	ComPtr<ID3D11Device> device;
	ComPtr<ID3D11DeviceContext> context;
	D3D_FEATURE_LEVEL const levels[]{ D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
	UINT flags = D3D11_CREATE_DEVICE_DEBUG;
	HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3,
		D3D11_SDK_VERSION, &device, nullptr, &context);
	if (result == DXGI_ERROR_SDK_COMPONENT_MISSING) {
		flags = 0;
		result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, 3,
			D3D11_SDK_VERSION, &device, nullptr, &context);
	}
	if (FAILED(result)) return 1;

	D3D11_TEXTURE2D_DESC description{};
	description.Width = 8;
	description.Height = 8;
	description.MipLevels = 1;
	description.ArraySize = 1;
	// Same layout as the driver's depth buffer: typeless, bound for depth and sampled for ReShade.
	description.Format = DXGI_FORMAT_R24G8_TYPELESS;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
	ComPtr<ID3D11Texture2D> depth;
	if (FAILED(device->CreateTexture2D(&description, nullptr, &depth))) return 2;

	D3D11_DEPTH_STENCIL_VIEW_DESC depthViewDescription{};
	depthViewDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	depthViewDescription.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
	ComPtr<ID3D11DepthStencilView> depthView;
	if (FAILED(device->CreateDepthStencilView(depth.Get(), &depthViewDescription, &depthView))) return 3;
	D3D11_SHADER_RESOURCE_VIEW_DESC shaderViewDescription{};
	shaderViewDescription.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	shaderViewDescription.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	shaderViewDescription.Texture2D.MipLevels = 1;
	ComPtr<ID3D11ShaderResourceView> shaderView;
	if (FAILED(device->CreateShaderResourceView(depth.Get(), &shaderViewDescription, &shaderView))) return 3;
	description.Format = DXGI_FORMAT_R24G8_TYPELESS;
	description.BindFlags = 0;
	ComPtr<ID3D11Texture2D> scratch;
	ComPtr<ID3D11Texture2D> region;
	if (FAILED(device->CreateTexture2D(&description, nullptr, &scratch)) ||
		FAILED(device->CreateTexture2D(&description, nullptr, &region))) return 4;

	ComPtr<ID3D11InfoQueue> infoQueue;
	device.As(&infoQueue);
	if (infoQueue) infoQueue->ClearStoredMessages();
	context->OMSetRenderTargets(0, nullptr, depthView.Get());
	context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 7);
	D3D11_BOX const box{ 0, 0, 0, 4, 4, 1 };
	context->CopyResource(scratch.Get(), depth.Get());
	context->CopySubresourceRegion(region.Get(), 0, 0, 0, 0, scratch.Get(), 0, &box);
	context->CopySubresourceRegion(scratch.Get(), 0, 2, 2, 0, region.Get(), 0, &box);
	context->CopyResource(depth.Get(), scratch.Get());
	context->Flush();

	if (!infoQueue) return 0;
	bool clean = true;
	for (UINT64 index = 0; index < infoQueue->GetNumStoredMessages(); ++index) {
		SIZE_T size = 0;
		infoQueue->GetMessage(index, nullptr, &size);
		std::vector<unsigned char> storage(size);
		auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
		if (SUCCEEDED(infoQueue->GetMessage(index, message, &size)) && message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
			std::cerr << message->pDescription << '\n';
			clean = false;
		}
	}
	return clean ? 0 : 5;
}
