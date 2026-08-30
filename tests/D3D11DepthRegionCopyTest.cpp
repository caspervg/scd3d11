#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
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
	description.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	description.SampleDesc.Count = 1;
	description.Usage = D3D11_USAGE_DEFAULT;
	description.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	ComPtr<ID3D11Texture2D> depth;
	if (FAILED(device->CreateTexture2D(&description, nullptr, &depth))) return 2;

	ComPtr<ID3D11DepthStencilView> depthView;
	if (FAILED(device->CreateDepthStencilView(depth.Get(), nullptr, &depthView))) return 3;

	D3D11_TEXTURE2D_DESC colorDescription{};
	colorDescription.Width = 8;
	colorDescription.Height = 8;
	colorDescription.MipLevels = 1;
	colorDescription.ArraySize = 1;
	colorDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	colorDescription.SampleDesc.Count = 1;
	colorDescription.Usage = D3D11_USAGE_DEFAULT;
	colorDescription.BindFlags = D3D11_BIND_RENDER_TARGET;
	ComPtr<ID3D11Texture2D> color;
	if (FAILED(device->CreateTexture2D(&colorDescription, nullptr, &color))) return 4;
	ComPtr<ID3D11RenderTargetView> colorView;
	if (FAILED(device->CreateRenderTargetView(color.Get(), nullptr, &colorView))) return 5;
	colorDescription.BindFlags = 0;
	ComPtr<ID3D11Texture2D> colorRegion;
	if (FAILED(device->CreateTexture2D(&colorDescription, nullptr, &colorRegion))) return 6;
	colorDescription.Usage = D3D11_USAGE_STAGING;
	colorDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ComPtr<ID3D11Texture2D> colorReadback;
	if (FAILED(device->CreateTexture2D(&colorDescription, nullptr, &colorReadback))) return 7;

	description.Format = DXGI_FORMAT_R24G8_TYPELESS;
	description.BindFlags = 0;
	ComPtr<ID3D11Texture2D> scratch;
	ComPtr<ID3D11Texture2D> region;
	if (FAILED(device->CreateTexture2D(&description, nullptr, &scratch)) ||
		FAILED(device->CreateTexture2D(&description, nullptr, &region))) return 8;

	ComPtr<ID3D11InfoQueue> infoQueue;
	device.As(&infoQueue);
	if (infoQueue) infoQueue->ClearStoredMessages();
	ID3D11RenderTargetView *boundColorView = colorView.Get();
	context->OMSetRenderTargets(1, &boundColorView, depthView.Get());
	float const savedColor[]{1.0f, 0.0f, 0.0f, 1.0f};
	float const overwrittenColor[]{0.0f, 0.0f, 1.0f, 1.0f};
	context->ClearRenderTargetView(colorView.Get(), savedColor);
	context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 7);
	D3D11_BOX const box{ 0, 0, 0, 4, 4, 1 };
	context->CopySubresourceRegion(colorRegion.Get(), 0, 0, 0, 0, color.Get(), 0, &box);
	context->ClearRenderTargetView(colorView.Get(), overwrittenColor);
	context->CopySubresourceRegion(color.Get(), 0, 0, 0, 0, colorRegion.Get(), 0, &box);
	context->CopyResource(colorReadback.Get(), color.Get());
	context->CopyResource(scratch.Get(), depth.Get());
	context->CopySubresourceRegion(region.Get(), 0, 0, 0, 0, scratch.Get(), 0, &box);
	context->CopySubresourceRegion(scratch.Get(), 0, 2, 2, 0, region.Get(), 0, &box);
	context->CopyResource(depth.Get(), scratch.Get());
	context->Flush();

	D3D11_MAPPED_SUBRESOURCE mapped{};
	if (FAILED(context->Map(colorReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return 9;
	uint8_t const *const pixel = static_cast<uint8_t const *>(mapped.pData);
	bool const colorRestored = pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
	context->Unmap(colorReadback.Get(), 0);
	if (!colorRestored) return 10;

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
	return clean ? 0 : 11;
}
