#include "StartupOverlay.h"
#include "StartupProgress.h"

#include <d3dcompiler.h>
#include <cstdio>
#include <vector>

namespace nSCD3D11 {
	namespace {
		char const kShaderSource[] = R"(
struct VertexInput { float2 position : POSITION; float2 texCoord : TEXCOORD0; };
struct VertexOutput { float4 position : SV_POSITION; float2 texCoord : TEXCOORD0; };
VertexOutput VSMain(VertexInput input) {
	VertexOutput output;
	output.position = float4(input.position, 0.0f, 1.0f);
	output.texCoord = input.texCoord;
	return output;
}
Texture2D notice : register(t0);
SamplerState noticeSampler : register(s0);
float4 PSMain(VertexOutput input) : SV_TARGET { return notice.Sample(noticeSampler, input.texCoord); }
)";

		struct OverlayVertex {
			float position[2];
			float texCoord[2];
		};

		void DrawNoticeText(HDC dc, RECT const &client, StartupProgress::Snapshot const &progress) {
			HBRUSH const background = CreateSolidBrush(RGB(14, 16, 20));
			if (background != nullptr) {
				FillRect(dc, &client, background);
				DeleteObject(background);
			}

			LONG const height = client.bottom - client.top;
			LONG const titleHeight = (height / 24 < 15) ? 15 : height / 24;
			LONG const detailHeight = (titleHeight * 5 / 8 < 12) ? 12 : titleHeight * 5 / 8;
			auto drawLine = [&](char const *text, LONG fontHeight, COLORREF color, LONG offset) {
				HFONT const font = CreateFontA(
					-fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
					OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
				HGDIOBJ const previousFont = font == nullptr ? nullptr : SelectObject(dc, font);
				RECT line = client;
				line.top = (client.top + client.bottom) / 2 + offset;
				line.bottom = line.top + fontHeight * 2;
				SetBkMode(dc, TRANSPARENT);
				SetTextColor(dc, color);
				DrawTextA(dc, text, -1, &line, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
				if (font != nullptr) {
					SelectObject(dc, previousFont);
					DeleteObject(font);
				}
			};
			char countText[96]{};
			if (progress.loadedPackageCount == 0) {
				std::snprintf(countText, sizeof(countText), "Preparing game data...");
			}
			else {
				std::snprintf(countText, sizeof(countText), "%u game data packages loaded",
				              static_cast<unsigned int>(progress.loadedPackageCount));
			}
			char const *phaseText = progress.resourceLoadingComplete
				? "Game data loaded. Initializing the game..."
				: "Loading plugins and game data. This can take a while.";
			drawLine("Starting SimCity 4", titleHeight, RGB(236, 239, 244), -titleHeight * 2);
			drawLine(countText, detailHeight, RGB(190, 198, 210), -titleHeight / 3);
			drawLine(phaseText, detailHeight, RGB(150, 158, 170), titleHeight * 3 / 4);
			drawLine("SCD3D11 active", detailHeight, RGB(104, 112, 126), titleHeight * 9 / 4);
		}

		HRESULT RenderNoticeBitmap(uint32_t width, uint32_t height, StartupProgress::Snapshot const &progress,
		                           HBITMAP &bitmap, void *&pixels) {
			BITMAPINFO bitmapInfo{};
			bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
			bitmapInfo.bmiHeader.biWidth = static_cast<LONG>(width);
			bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(height);
			bitmapInfo.bmiHeader.biPlanes = 1;
			bitmapInfo.bmiHeader.biBitCount = 32;
			bitmapInfo.bmiHeader.biCompression = BI_RGB;
			HDC screen = GetDC(nullptr);
			bitmap = screen == nullptr ? nullptr :
			         CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
			if (screen != nullptr) ReleaseDC(nullptr, screen);
			if (bitmap == nullptr || pixels == nullptr) return E_OUTOFMEMORY;
			HDC dc = CreateCompatibleDC(nullptr);
			if (dc == nullptr) {
				DeleteObject(bitmap);
				bitmap = nullptr;
				pixels = nullptr;
				return E_OUTOFMEMORY;
			}
			HGDIOBJ previous = SelectObject(dc, bitmap);
			RECT client{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
			DrawNoticeText(dc, client, progress);
			SelectObject(dc, previous);
			DeleteDC(dc);
			return S_OK;
		}
	}

	HRESULT StartupOverlay::Initialize(ID3D11Device *device, uint32_t width, uint32_t height) {
		if (device == nullptr || width == 0 || height == 0) return E_INVALIDARG;

		// This runs on every back buffer resize, but only the notice texture depends on the size,
		// so keep the compiled pipeline instead of paying two D3DCompile calls each time. A device
		// loss clears it through Shutdown, which is what makes the null check the right test.
		if (!vertexShader) {
			HRESULT const result = CreatePipeline(device);
			if (FAILED(result)) {
				// Never leave a half-built pipeline behind for the next attempt to skip over.
				Shutdown();
				return result;
			}
		}

		noticeView.Reset();
		noticeTexture.Reset();
		HRESULT const result = CreateNoticeTexture(device, width, height);
		if (FAILED(result)) {
			Shutdown();
			return result;
		}
		this->width = width;
		this->height = height;
		return S_OK;
	}

	void StartupOverlay::Shutdown(void) {
		active = false;
		width = height = 0;
		rasterizerState.Reset();
		blendState.Reset();
		depthStencilState.Reset();
		noticeTexture.Reset();
		noticeView.Reset();
		sampler.Reset();
		vertexBuffer.Reset();
		inputLayout.Reset();
		pixelShader.Reset();
		vertexShader.Reset();
		renderedRevision = 0;
	}

	void StartupOverlay::SetActive(bool value) { active = value; }

	bool StartupOverlay::IsActive(void) const { return active && noticeView && vertexBuffer; }

	HRESULT StartupOverlay::CreatePipeline(ID3D11Device *device) {
		Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
		Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
		Microsoft::WRL::ComPtr<ID3DBlob> errors;
		UINT const flags = D3DCOMPILE_ENABLE_STRICTNESS;
		HRESULT result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "StartupOverlay", nullptr, nullptr,
		                           "VSMain", "vs_4_0", flags, 0, &vertexBytecode, &errors);
		if (FAILED(result)) return result;
		result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "StartupOverlay", nullptr, nullptr,
		                    "PSMain", "ps_4_0", flags, 0, &pixelBytecode, &errors);
		if (FAILED(result)) return result;
		result = device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
		                                    nullptr, &vertexShader);
		if (FAILED(result)) return result;
		result = device->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
		                                   nullptr, &pixelShader);
		if (FAILED(result)) return result;
		D3D11_INPUT_ELEMENT_DESC const elements[] = {
			{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(OverlayVertex, position),
			 D3D11_INPUT_PER_VERTEX_DATA, 0},
			{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(OverlayVertex, texCoord),
			 D3D11_INPUT_PER_VERTEX_DATA, 0},
		};
		result = device->CreateInputLayout(elements, 2, vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
		                                   &inputLayout);
		if (FAILED(result)) return result;
		OverlayVertex const vertices[] = {
			{{-1.0f, 1.0f}, {0.0f, 0.0f}}, {{1.0f, 1.0f}, {1.0f, 0.0f}},
			{{-1.0f, -1.0f}, {0.0f, 1.0f}}, {{1.0f, -1.0f}, {1.0f, 1.0f}},
		};
		D3D11_BUFFER_DESC bufferDescription{sizeof(vertices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
		D3D11_SUBRESOURCE_DATA initialData{vertices, 0, 0};
		result = device->CreateBuffer(&bufferDescription, &initialData, &vertexBuffer);
		if (FAILED(result)) return result;
		D3D11_SAMPLER_DESC samplerDescription{};
		samplerDescription.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDescription.AddressU = samplerDescription.AddressV = samplerDescription.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDescription.MaxLOD = D3D11_FLOAT32_MAX;
		result = device->CreateSamplerState(&samplerDescription, &sampler);
		if (FAILED(result)) return result;

		D3D11_DEPTH_STENCIL_DESC depthDescription{};
		depthDescription.DepthEnable = FALSE;
		depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
		result = device->CreateDepthStencilState(&depthDescription, &depthStencilState);
		if (FAILED(result)) return result;

		D3D11_BLEND_DESC blendDescription{};
		blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
		result = device->CreateBlendState(&blendDescription, &blendState);
		if (FAILED(result)) return result;

		D3D11_RASTERIZER_DESC rasterizerDescription{};
		rasterizerDescription.FillMode = D3D11_FILL_SOLID;
		rasterizerDescription.CullMode = D3D11_CULL_NONE;
		rasterizerDescription.DepthClipEnable = TRUE;
		return device->CreateRasterizerState(&rasterizerDescription, &rasterizerState);
	}

	HRESULT StartupOverlay::CreateNoticeTexture(ID3D11Device *device, uint32_t width, uint32_t height) {
		StartupProgress::Snapshot const progress = StartupProgress::GetSnapshot();
		HBITMAP bitmap = nullptr;
		void *pixels = nullptr;
		HRESULT result = RenderNoticeBitmap(width, height, progress, bitmap, pixels);
		if (FAILED(result)) return result;

		D3D11_TEXTURE2D_DESC textureDescription{};
		textureDescription.Width = width;
		textureDescription.Height = height;
		textureDescription.MipLevels = 1;
		textureDescription.ArraySize = 1;
		textureDescription.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		textureDescription.SampleDesc.Count = 1;
		textureDescription.Usage = D3D11_USAGE_DEFAULT;
		textureDescription.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA data{pixels, width * 4, 0};
		result = device->CreateTexture2D(&textureDescription, &data, &noticeTexture);
		DeleteObject(bitmap);
		if (FAILED(result)) return result;
		result = device->CreateShaderResourceView(noticeTexture.Get(), nullptr, &noticeView);
		if (SUCCEEDED(result)) renderedRevision = progress.revision;
		return result;
	}

	void StartupOverlay::RefreshNoticeTexture(ID3D11DeviceContext *context) {
		StartupProgress::Snapshot const progress = StartupProgress::GetSnapshot();
		if (context == nullptr || noticeTexture == nullptr || progress.revision == renderedRevision) return;
		HBITMAP bitmap = nullptr;
		void *pixels = nullptr;
		if (SUCCEEDED(RenderNoticeBitmap(width, height, progress, bitmap, pixels))) {
			context->UpdateSubresource(noticeTexture.Get(), 0, nullptr, pixels, width * 4, 0);
			renderedRevision = progress.revision;
			DeleteObject(bitmap);
		}
	}

	void StartupOverlay::Draw(ID3D11DeviceContext *context) {
		if (!IsActive() || context == nullptr || width == 0 || height == 0) return;
		RefreshNoticeTexture(context);
		UINT const stride = sizeof(OverlayVertex);
		UINT const offset = 0;
		ID3D11Buffer *buffer = vertexBuffer.Get();
		ID3D11ShaderResourceView *view = noticeView.Get();
		ID3D11SamplerState *activeSampler = sampler.Get();
		D3D11_VIEWPORT const viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
		context->RSSetViewports(1, &viewport);
		context->OMSetBlendState(blendState.Get(), nullptr, 0xffffffff);
		context->OMSetDepthStencilState(depthStencilState.Get(), 0);
		context->RSSetState(rasterizerState.Get());
		context->IASetInputLayout(inputLayout.Get());
		context->IASetVertexBuffers(0, 1, &buffer, &stride, &offset);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->VSSetShader(vertexShader.Get(), nullptr, 0);
		context->PSSetShader(pixelShader.Get(), nullptr, 0);
		context->PSSetShaderResources(0, 1, &view);
		context->PSSetSamplers(0, 1, &activeSampler);
		context->Draw(4, 0);
	}
}
