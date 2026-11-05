/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// The FrameRate cheat's counter: frames per second in small white digits in
// the top right corner, drawn last before each Present. It needs no font: the
// pixel shader holds a 3x5 glyph for each digit.

#include "cGDriver.h"
#include "Diagnostics.h"

#include <cmath>
#include <d3dcompiler.h>
#include <string>

namespace nSCD3D11 {
	namespace {
		char const kFrameRateShader[] = R"(
cbuffer FrameRateConstants : register(b0) {
	float4 area; // left, top, pixels per glyph cell
	uint4 text;  // x: up to 8 digits, 4 bits each, first in the lowest bits; y: how many
};
// 3x5 digits, bit row * 3 + column, then blanks so that any 4-bit glyph number is in range.
static const uint kGlyphs[16] = {
	31599, 29850, 29671, 31207, 18925, 31183, 31695, 18727, 31727, 31215, 0, 0, 0, 0, 0, 0
};
float4 VSMain(uint id : SV_VertexID) : SV_POSITION {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
// Glyphs sit 4 cells apart, the fourth column being the gap.
bool Lit(int2 cell) {
	uint index = (uint)max(cell.x, 0) / 4u;
	uint column = (uint)max(cell.x, 0) % 4u;
	uint glyph = kGlyphs[(text.x >> (min(index, 7u) * 4u)) & 15u];
	bool inside = cell.x >= 0 && cell.y >= 0 && cell.y <= 4 && column != 3u && index < text.y;
	return inside && ((glyph >> ((uint)clamp(cell.y, 0, 4) * 3u + column)) & 1u) != 0u;
}
// White glyphs with a dark outline one cell wide, so they read on any background.
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET {
	int2 cell = int2(floor((position.xy - area.xy) / area.z)) - 1;
	bool lit = Lit(cell);
	bool edge = false;
	[unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) edge = edge || Lit(cell + int2(x, y));
	if (!edge) discard;
	return lit ? float4(1.0, 1.0, 1.0, 0.9) : float4(0.0, 0.0, 0.0, 0.6);
}
)";

		struct FrameRateConstants {
			float area[4];
			uint32_t text[4];
		};
		static_assert(sizeof(FrameRateConstants) % 16 == 0, "constant buffers are sized in 16-byte registers");

		// Frames are counted over this long before the number changes.
		constexpr double kWindowSeconds = 0.5;
	}

	std::atomic<bool> cGDriver::frameRateVisible{false};

	void cGDriver::ToggleFrameRate() {
		frameRateVisible = !frameRateVisible;
	}

	void cGDriver::DrawFrameRate() {
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		static LARGE_INTEGER const frequency = [] {
			LARGE_INTEGER value{};
			QueryPerformanceFrequency(&value);
			return value;
		}();
		if (frameRate.windowStart.QuadPart == 0) frameRate.windowStart = now;
		++frameRate.frames;
		double const seconds = static_cast<double>(now.QuadPart - frameRate.windowStart.QuadPart) /
		                       static_cast<double>(frequency.QuadPart);
		if (seconds >= kWindowSeconds) {
			frameRate.shown = static_cast<uint32_t>((std::min)(frameRate.frames / seconds + 0.5, 9999.0));
			frameRate.frames = 0;
			frameRate.windowStart = now;
		}

		if (!frameRate.pixelShader) {
			Microsoft::WRL::ComPtr<ID3DBlob> vertex, pixel;
			HRESULT result = D3DCompile(kFrameRateShader, sizeof(kFrameRateShader) - 1, "SCD3D11FrameRate", nullptr,
			                            nullptr, "VSMain", "vs_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex, nullptr);
			if (SUCCEEDED(result))
				result = D3DCompile(kFrameRateShader, sizeof(kFrameRateShader) - 1, "SCD3D11FrameRate", nullptr,
				                    nullptr, "PSMain", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, nullptr);
			if (SUCCEEDED(result))
				result = d3dDevice->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(), nullptr,
				                                       &frameRate.vertexShader);
			if (SUCCEEDED(result))
				result = d3dDevice->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(), nullptr,
				                                      &frameRate.pixelShader);
			D3D11_BUFFER_DESC buffer{sizeof(FrameRateConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
			if (SUCCEEDED(result)) result = d3dDevice->CreateBuffer(&buffer, nullptr, &frameRate.constants);
			D3D11_BLEND_DESC blend{};
			blend.RenderTarget[0].BlendEnable = TRUE;
			blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
			blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
			if (SUCCEEDED(result)) result = d3dDevice->CreateBlendState(&blend, &frameRate.blend);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "frame rate counter", result);
				frameRate = FrameRatePipeline{};
				frameRateVisible = false;
				return;
			}
		}

		std::string const label = std::to_string(frameRate.shown);
		FrameRateConstants constants{};
		uint32_t count = 0;
		auto add = [&](uint32_t glyph) { constants.text[0] |= glyph << (4 * count++); };
		for (char const digit : label) add(static_cast<uint32_t>(digit - '0'));
		constants.text[1] = count;
		// About 1/100 of the window's height, at least one pixel per cell.
		float const cell = (std::max)(1.0f, std::round(static_cast<float>(windowHeight) / 720.0f));
		float const width = static_cast<float>(4 * count + 1) * cell, height = 7.0f * cell;
		constants.area[0] = static_cast<float>(windowWidth) - width - 2.0f * cell;
		constants.area[1] = 2.0f * cell;
		constants.area[2] = cell;
		d3dContext->UpdateSubresource(frameRate.constants.Get(), 0, nullptr, &constants, 0, 0);

		ID3D11RenderTargetView* const target = renderTargetView.Get();
		d3dContext->OMSetRenderTargets(1, &target, nullptr);
		D3D11_VIEWPORT const viewport{constants.area[0], constants.area[1], width, height, 0.0f, 1.0f};
		d3dContext->RSSetViewports(1, &viewport);
		d3dContext->RSSetState(nullptr);
		d3dContext->OMSetBlendState(frameRate.blend.Get(), nullptr, 0xffffffff);
		d3dContext->OMSetDepthStencilState(nullptr, 0);
		d3dContext->IASetInputLayout(nullptr);
		d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		d3dContext->VSSetShader(frameRate.vertexShader.Get(), nullptr, 0);
		d3dContext->PSSetShader(frameRate.pixelShader.Get(), nullptr, 0);
		ID3D11Buffer* const buffer = frameRate.constants.Get();
		d3dContext->PSSetConstantBuffers(0, 1, &buffer);
		d3dContext->Draw(3, 0);
		RestoreOwnedContextState();
	}
}
