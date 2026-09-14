/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SC4's 2D canvas (cGZDrawContextRenderToSimGL::Draw) pushes CPU-side pixel rectangles through the
// BitBlt/StretchBlt family instead of textures in some configurations - notably exclusive
// fullscreen, where the whole region view and UI arrive this way. Argument layout comes from
// that caller: destination rect, source size, SimGL format/type, pixels already offset to the
// source origin (row length set through PixelStore), an optional colour key, and for the Alpha
// variants a trailing constant alpha where 0xFFFFFFFF means "none".

#include "cGDriver.h"
#include "Diagnostics.h"
#include "TextureUploadUtils.h"

#include <cstddef>
#include <cstring>
#include <d3dcompiler.h>

namespace nSCD3D11 {
	namespace {
		constexpr uint32_t kPuntBlitSourceAlpha = 0x6C4236E7;

		char const kBlitShaderSource[] = R"(
struct VertexInput { float2 position : POSITION; float2 texCoord : TEXCOORD0; };
struct VertexOutput { float4 position : SV_POSITION; float2 texCoord : TEXCOORD0; };
VertexOutput VSMain(VertexInput input) {
	VertexOutput output;
	output.position = float4(input.position, 0.0f, 1.0f);
	output.texCoord = input.texCoord;
	return output;
}
Texture2D source : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer BlitConstants : register(b0) {
	float4 modulate;
	float4 colorKey;
	float sourceAlpha;
};
float4 PSMain(VertexOutput input) : SV_TARGET {
	float4 color = source.Sample(sourceSampler, input.texCoord);
	color.a = lerp(1.0f, color.a, sourceAlpha);
	if (colorKey.a > 0.5f && all(abs(color.rgb - colorKey.rgb) < 0.5f / 255.0f)) color.a = 0.0f;
	return color * modulate;
}
)";

		struct BlitVertex {
			float position[2];
			float texCoord[2];
		};

		struct BlitConstants {
			float modulate[4];
			float colorKey[4];
			float sourceAlpha;
			float padding[7];
		};

		void UnpackColor(uint32_t value, float rgba[4]) {
			// 0xAARRGGBB; bare byte values only carry alpha, so 0xFFFFFFFF and 0xFF are both "no change".
			if (value <= 0xff) {
				rgba[0] = rgba[1] = rgba[2] = 1.0f;
				rgba[3] = static_cast<float>(value) / 255.0f;
				return;
			}
			rgba[0] = static_cast<float>((value >> 16) & 0xff) / 255.0f;
			rgba[1] = static_cast<float>((value >> 8) & 0xff) / 255.0f;
			rgba[2] = static_cast<float>(value & 0xff) / 255.0f;
			rgba[3] = static_cast<float>(value >> 24) / 255.0f;
		}
	}

	HRESULT cGDriver::CreateBlitPipeline() {
		Microsoft::WRL::ComPtr<ID3DBlob> vertexBytecode;
		Microsoft::WRL::ComPtr<ID3DBlob> pixelBytecode;
		HRESULT result = D3DCompile(kBlitShaderSource, sizeof(kBlitShaderSource) - 1, "SCD3D11Blit", nullptr,
		                            nullptr, "VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertexBytecode,
		                            nullptr);
		if (SUCCEEDED(result)) {
			result = D3DCompile(kBlitShaderSource, sizeof(kBlitShaderSource) - 1, "SCD3D11Blit", nullptr, nullptr,
			                    "PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixelBytecode, nullptr);
		}
		if (SUCCEEDED(result)) {
			result = d3dDevice->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
			                                       nullptr, &blit.vertexShader);
		}
		if (SUCCEEDED(result)) {
			result = d3dDevice->CreatePixelShader(pixelBytecode->GetBufferPointer(), pixelBytecode->GetBufferSize(),
			                                      nullptr, &blit.pixelShader);
		}
		if (SUCCEEDED(result)) {
			D3D11_INPUT_ELEMENT_DESC const elements[] = {
				{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(BlitVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
				{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(BlitVertex, texCoord), D3D11_INPUT_PER_VERTEX_DATA, 0},
			};
			result = d3dDevice->CreateInputLayout(elements, 2, vertexBytecode->GetBufferPointer(),
			                                      vertexBytecode->GetBufferSize(), &blit.inputLayout);
		}
		if (SUCCEEDED(result)) {
			D3D11_BUFFER_DESC const description{
				sizeof(BlitVertex) * 4, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0
			};
			result = d3dDevice->CreateBuffer(&description, nullptr, &blit.vertexBuffer);
		}
		if (SUCCEEDED(result)) {
			D3D11_BUFFER_DESC const description{
				sizeof(BlitConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0
			};
			result = d3dDevice->CreateBuffer(&description, nullptr, &blit.constants);
		}
		if (SUCCEEDED(result)) {
			// Point sampling: 1:1 blits stay exact and scaled ones never pull in texels from beyond
			// the source rectangle inside the grow-only upload texture.
			D3D11_SAMPLER_DESC description{};
			description.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
			description.AddressU = description.AddressV = description.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			description.MaxLOD = D3D11_FLOAT32_MAX;
			result = d3dDevice->CreateSamplerState(&description, &blit.sampler);
		}
		if (SUCCEEDED(result)) {
			D3D11_DEPTH_STENCIL_DESC description{};
			description.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			result = d3dDevice->CreateDepthStencilState(&description, &blit.depthState);
		}
		if (SUCCEEDED(result)) {
			D3D11_BLEND_DESC description{};
			description.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			result = d3dDevice->CreateBlendState(&description, &blit.opaqueBlend);
		}
		if (SUCCEEDED(result)) {
			D3D11_BLEND_DESC description{};
			D3D11_RENDER_TARGET_BLEND_DESC &target = description.RenderTarget[0];
			target.BlendEnable = TRUE;
			target.SrcBlend = D3D11_BLEND_SRC_ALPHA;
			target.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			target.BlendOp = D3D11_BLEND_OP_ADD;
			target.SrcBlendAlpha = D3D11_BLEND_ONE;
			target.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			result = d3dDevice->CreateBlendState(&description, &blit.alphaBlend);
		}
		D3D11_RASTERIZER_DESC rasterizerDescription{};
		rasterizerDescription.FillMode = D3D11_FILL_SOLID;
		rasterizerDescription.CullMode = D3D11_CULL_NONE;
		rasterizerDescription.DepthClipEnable = TRUE;
		if (SUCCEEDED(result)) result = d3dDevice->CreateRasterizerState(&rasterizerDescription, &blit.rasterizer);
		rasterizerDescription.ScissorEnable = TRUE;
		if (SUCCEEDED(result)) {
			result = d3dDevice->CreateRasterizerState(&rasterizerDescription, &blit.scissorRasterizer);
		}
		if (FAILED(result)) {
			LogHRESULT(LogCategory::Resource, "CreateBlitPipeline", result);
			blit = BlitPipeline{};
		}
		return result;
	}

	void cGDriver::DrawBlit(
		int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight, int32_t sourceWidth, int32_t sourceHeight,
		uint32_t format, uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey, BlitAlpha alpha,
		uint32_t alphaValue) {
		if (destWidth <= 0 || destHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0 || pixels == nullptr) {
			SetLastError(DriverError::INVALID_VALUE);
			return;
		}
		if (!IsDeviceReady() || deviceLost) return;

		uint32_t const pixelBytes = TextureSourcePixelBytes(format, type);
		if (pixelBytes == 0 || pixelBytes > 4) {
			static bool logged = false;
			if (!logged) {
				Log(LogCategory::Unsupported, "blit source format %u type %u is not implemented", format, type);
				logged = true;
			}
			SetLastError(DriverError::NOT_SUPPORTED);
			return;
		}
		if (!blit.vertexShader && FAILED(CreateBlitPipeline())) {
			SetLastError(DriverError::CREATE_CONTEXT_FAIL);
			return;
		}

		uint32_t const width = static_cast<uint32_t>(sourceWidth);
		uint32_t const height = static_cast<uint32_t>(sourceHeight);
		if (width > blit.textureWidth || height > blit.textureHeight) {
			uint32_t const textureWidth = width > blit.textureWidth ? width : blit.textureWidth;
			uint32_t const textureHeight = height > blit.textureHeight ? height : blit.textureHeight;
			D3D11_TEXTURE2D_DESC description{};
			description.Width = textureWidth;
			description.Height = textureHeight;
			description.MipLevels = 1;
			description.ArraySize = 1;
			// BGRA matches SC4's 32-bit canvas byte order, so those pixels upload without conversion.
			description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			description.SampleDesc.Count = 1;
			description.Usage = D3D11_USAGE_DEFAULT;
			description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			blit.textureView.Reset();
			blit.texture.Reset();
			blit.textureWidth = blit.textureHeight = 0;
			HRESULT result = d3dDevice->CreateTexture2D(&description, nullptr, &blit.texture);
			if (SUCCEEDED(result)) result = d3dDevice->CreateShaderResourceView(blit.texture.Get(), nullptr, &blit.textureView);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateTexture2D(blit)", result);
				blit.texture.Reset();
				SetLastError(DriverError::CREATE_CONTEXT_FAIL);
				return;
			}
			blit.textureWidth = textureWidth;
			blit.textureHeight = textureHeight;
		}

		colorKeyed = colorKeyed && colorKey != nullptr;
		uint32_t key = 0;
		if (colorKeyed) std::memcpy(&key, colorKey, pixelBytes);
		bool const usesSourceAlpha = alpha == BlitAlpha::Source || alpha == BlitAlpha::SourceModulated;

		BlitConstants constants{};
		constants.modulate[0] = constants.modulate[1] = constants.modulate[2] = constants.modulate[3] = 1.0f;
		if (alpha == BlitAlpha::Constant) {
			float rgba[4];
			UnpackColor(alphaValue, rgba);
			constants.modulate[3] = rgba[3];
		} else if (alpha == BlitAlpha::SourceModulated) {
			UnpackColor(alphaValue, constants.modulate);
		}
		constants.sourceAlpha = usesSourceAlpha ? 1.0f : 0.0f;

		// Only the source rectangle is uploaded; the texture itself is grow-only.
		uint32_t const sourcePitch = (pixelStoreRowLength != 0 ? pixelStoreRowLength : width) * pixelBytes;
		D3D11_BOX const box{0, 0, 0, width, height, 1};
		if (type == 1 && format == 3) {
			d3dContext->UpdateSubresource(blit.texture.Get(), 0, &box, pixels, sourcePitch, 0);
		} else {
			blit.scratch.resize(static_cast<size_t>(width) * height * 4);
			bool const byteBgr = type == 1 && format == 2;
			for (uint32_t y = 0; y < height; ++y) {
				uint8_t const *source = static_cast<uint8_t const *>(pixels) + static_cast<size_t>(y) * sourcePitch;
				uint8_t *destination = blit.scratch.data() + static_cast<size_t>(y) * width * 4;
				for (uint32_t x = 0; x < width; ++x, source += pixelBytes, destination += 4) {
					if (byteBgr) {
						destination[0] = source[0];
						destination[1] = source[1];
						destination[2] = source[2];
						destination[3] = 0xff;
						continue;
					}
					uint8_t rgba[4];
					if (!ConvertTextureSourcePixel(format, type, source, rgba)) {
						SetLastError(DriverError::NOT_SUPPORTED);
						return;
					}
					destination[0] = rgba[2];
					destination[1] = rgba[1];
					destination[2] = rgba[0];
					destination[3] = usesSourceAlpha ? rgba[3] : 0xff;
					// Packed formats cannot be compared in the shader, so key them here.
					if (colorKeyed) {
						uint32_t raw = 0;
						std::memcpy(&raw, source, pixelBytes);
						if (raw == key) destination[3] = 0;
					}
				}
			}
			if (!byteBgr) {
				constants.sourceAlpha = 1.0f;
				colorKeyed = false;
			}
			d3dContext->UpdateSubresource(blit.texture.Get(), 0, &box, blit.scratch.data(), width * 4, 0);
		}
		if (colorKeyed) {
			// 24/32-bit keys are 0x00RRGGBB, matching the byte order of the BGR(A) pixels.
			constants.colorKey[0] = static_cast<float>((key >> 16) & 0xff) / 255.0f;
			constants.colorKey[1] = static_cast<float>((key >> 8) & 0xff) / 255.0f;
			constants.colorKey[2] = static_cast<float>(key & 0xff) / 255.0f;
			constants.colorKey[3] = 1.0f;
		}
		d3dContext->UpdateSubresource(blit.constants.Get(), 0, nullptr, &constants, 0, 0);

		D3D11_MAPPED_SUBRESOURCE mapped{};

		float const left = 2.0f * static_cast<float>(destX) / static_cast<float>(windowWidth) - 1.0f;
		float const right = 2.0f * static_cast<float>(destX + destWidth) / static_cast<float>(windowWidth) - 1.0f;
		float const top = 1.0f - 2.0f * static_cast<float>(destY) / static_cast<float>(windowHeight);
		float const bottom = 1.0f - 2.0f * static_cast<float>(destY + destHeight) / static_cast<float>(windowHeight);
		float const u = static_cast<float>(width) / static_cast<float>(blit.textureWidth);
		float const v = static_cast<float>(height) / static_cast<float>(blit.textureHeight);
		BlitVertex const vertices[4] = {
			{{left, top}, {0.0f, 0.0f}}, {{right, top}, {u, 0.0f}},
			{{left, bottom}, {0.0f, v}}, {{right, bottom}, {u, v}},
		};
		HRESULT const mapResult = d3dContext->Map(blit.vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
		if (FAILED(mapResult)) {
			NoteDeviceLoss(mapResult);
			return;
		}
		std::memcpy(mapped.pData, vertices, sizeof(vertices));
		d3dContext->Unmap(blit.vertexBuffer.Get(), 0);

		UINT viewportCount = 1;
		D3D11_VIEWPORT savedViewport{};
		d3dContext->RSGetViewports(&viewportCount, &savedViewport);
		D3D11_VIEWPORT const fullViewport{
			0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0.0f, 1.0f
		};

		UINT const stride = sizeof(BlitVertex);
		UINT const offset = 0;
		ID3D11Buffer *vertexBuffer = blit.vertexBuffer.Get();
		ID3D11ShaderResourceView *view = blit.textureView.Get();
		ID3D11SamplerState *sampler = blit.sampler.Get();
		ID3D11Buffer *constantBuffer = blit.constants.Get();
		bool const blends = alpha != BlitAlpha::Opaque || colorKeyed || constants.sourceAlpha != 0.0f;
		d3dContext->RSSetViewports(1, &fullViewport);
		d3dContext->RSSetState(scissorEnabled ? blit.scissorRasterizer.Get() : blit.rasterizer.Get());
		d3dContext->OMSetBlendState(blends ? blit.alphaBlend.Get() : blit.opaqueBlend.Get(), nullptr, 0xffffffff);
		d3dContext->OMSetDepthStencilState(blit.depthState.Get(), 0);
		d3dContext->IASetInputLayout(blit.inputLayout.Get());
		d3dContext->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
		d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		d3dContext->VSSetShader(blit.vertexShader.Get(), nullptr, 0);
		d3dContext->PSSetShader(blit.pixelShader.Get(), nullptr, 0);
		d3dContext->PSSetShaderResources(0, 1, &view);
		d3dContext->PSSetSamplers(0, 1, &sampler);
		d3dContext->PSSetConstantBuffers(0, 1, &constantBuffer);
		d3dContext->Draw(4, 0);

		if (viewportCount == 1) d3dContext->RSSetViewports(1, &savedViewport);
		// Every binding above belongs to the geometry pipeline's caches; force it to rebind.
		InvalidateD3D11StateCache();
	}

	void cGDriver::BitBlt(
		int32_t destX, int32_t destY, int32_t width, int32_t height, uint32_t format, uint32_t type,
		void const *pixels, bool colorKeyed, void const *colorKey) {
		DrawBlit(destX, destY, width, height, width, height, format, type, pixels, colorKeyed, colorKey,
		         blitUsesSourceAlpha ? BlitAlpha::Source : BlitAlpha::Opaque, 0xffffffff);
	}

	void cGDriver::StretchBlt(
		int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight, int32_t sourceWidth, int32_t sourceHeight,
		uint32_t format, uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey) {
		DrawBlit(destX, destY, destWidth, destHeight, sourceWidth, sourceHeight, format, type, pixels, colorKeyed,
		         colorKey, blitUsesSourceAlpha ? BlitAlpha::Source : BlitAlpha::Opaque, 0xffffffff);
	}

	void cGDriver::BitBltAlpha(
		int32_t destX, int32_t destY, int32_t width, int32_t height, uint32_t format, uint32_t type,
		void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) {
		DrawBlit(destX, destY, width, height, width, height, format, type, pixels, colorKeyed, colorKey,
		         BlitAlpha::Constant, alpha);
	}

	void cGDriver::StretchBltAlpha(
		int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight, int32_t sourceWidth, int32_t sourceHeight,
		uint32_t format, uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) {
		DrawBlit(destX, destY, destWidth, destHeight, sourceWidth, sourceHeight, format, type, pixels, colorKeyed,
		         colorKey, BlitAlpha::Constant, alpha);
	}

	void cGDriver::BitBltAlphaModulate(
		int32_t destX, int32_t destY, int32_t width, int32_t height, uint32_t format, uint32_t type,
		void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) {
		DrawBlit(destX, destY, width, height, width, height, format, type, pixels, colorKeyed, colorKey,
		         BlitAlpha::SourceModulated, alpha);
	}

	void cGDriver::StretchBltAlphaModulate(
		int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight, int32_t sourceWidth, int32_t sourceHeight,
		uint32_t format, uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) {
		DrawBlit(destX, destY, destWidth, destHeight, sourceWidth, sourceHeight, format, type, pixels, colorKeyed,
		         colorKey, BlitAlpha::SourceModulated, alpha);
	}

	bool cGDriver::Punt(uint32_t id, void *data) {
		if (id != kPuntBlitSourceAlpha || data == nullptr) return false;
		blitUsesSourceAlpha = *static_cast<bool *>(data);
		return true;
	}
}
