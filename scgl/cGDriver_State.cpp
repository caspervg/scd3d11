/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "cGDriver.h"
#include "Diagnostics.h"

namespace nSCGL
{
	bool cGDriver::ApplyRenderStates() {
		uint64_t const depthKey =
			static_cast<uint64_t>(enabledCapabilities[kGDCapability_DepthTest]) |
			(static_cast<uint64_t>(depthWriteEnabled) << 1) |
			(static_cast<uint64_t>(depthFunction) << 2) |
			(static_cast<uint64_t>(enabledCapabilities[kGDCapability_StencilTest]) << 6) |
			(static_cast<uint64_t>(stencilReadMask) << 7) |
			(static_cast<uint64_t>(stencilWriteMask) << 15) |
			(static_cast<uint64_t>(stencilFunction) << 23) |
			(static_cast<uint64_t>(stencilFailOperation) << 27) |
			(static_cast<uint64_t>(stencilDepthFailOperation) << 30) |
			(static_cast<uint64_t>(stencilPassOperation) << 33);
		RecordEncountered(ObservedCategory::RenderState, depthKey);

		auto depthIterator = depthStencilStates.find(depthKey);
		if (depthIterator == depthStencilStates.end()) {
			D3D11_DEPTH_STENCIL_DESC description{};
			description.DepthEnable = enabledCapabilities[kGDCapability_DepthTest];
			description.DepthWriteMask = depthWriteEnabled ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
			description.DepthFunc = D3D11Comparison(depthFunction);
			description.StencilEnable = enabledCapabilities[kGDCapability_StencilTest];
			description.StencilReadMask = stencilReadMask;
			description.StencilWriteMask = stencilWriteMask;
			description.FrontFace.StencilFunc = D3D11Comparison(stencilFunction);
			description.FrontFace.StencilFailOp = D3D11StencilOperation(stencilFailOperation);
			description.FrontFace.StencilDepthFailOp = D3D11StencilOperation(stencilDepthFailOperation);
			description.FrontFace.StencilPassOp = D3D11StencilOperation(stencilPassOperation);
			description.BackFace = description.FrontFace;

			Microsoft::WRL::ComPtr<ID3D11DepthStencilState> state;
			HRESULT const result = d3dDevice->CreateDepthStencilState(&description, &state);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateDepthStencilState", result);
				return false;
			}
			depthIterator = depthStencilStates.emplace(depthKey, state).first;
		}
		d3dContext->OMSetDepthStencilState(depthIterator->second.Get(), static_cast<UINT>(stencilReference));

		uint64_t const blendKey =
			static_cast<uint64_t>(enabledCapabilities[kGDCapability_Blend]) |
			(static_cast<uint64_t>(sourceBlend) << 1) |
			(static_cast<uint64_t>(destinationBlend) << 5) |
			(static_cast<uint64_t>(colorWriteEnabled) << 9);
		RecordEncountered(ObservedCategory::RenderState, (1ull << 60) | blendKey);
		auto blendIterator = blendStates.find(blendKey);
		if (blendIterator == blendStates.end()) {
			D3D11_BLEND_DESC description{};
			D3D11_RENDER_TARGET_BLEND_DESC& target = description.RenderTarget[0];
			target.BlendEnable = enabledCapabilities[kGDCapability_Blend];
			target.SrcBlend = D3D11Blend(sourceBlend);
			target.DestBlend = D3D11Blend(destinationBlend);
			target.BlendOp = D3D11_BLEND_OP_ADD;
			target.SrcBlendAlpha = D3D11Blend(sourceBlend);
			target.DestBlendAlpha = D3D11Blend(destinationBlend);
			target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
			target.RenderTargetWriteMask = colorWriteEnabled ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;

			Microsoft::WRL::ComPtr<ID3D11BlendState> state;
			HRESULT const result = d3dDevice->CreateBlendState(&description, &state);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateBlendState", result);
				return false;
			}
			blendIterator = blendStates.emplace(blendKey, state).first;
		}
		d3dContext->OMSetBlendState(blendIterator->second.Get(), nullptr, 0xffffffff);

		uint64_t const rasterizerKey =
			static_cast<uint64_t>(enabledCapabilities[kGDCapability_CullFace]) |
			(static_cast<uint64_t>(scissorEnabled) << 1) |
			(static_cast<uint64_t>(static_cast<uint32_t>(polygonOffset)) << 2);
		RecordEncountered(ObservedCategory::RenderState, (2ull << 60) | rasterizerKey);
		auto rasterizerIterator = rasterizerStates.find(rasterizerKey);
		if (rasterizerIterator == rasterizerStates.end()) {
			D3D11_RASTERIZER_DESC description{};
			description.FillMode = D3D11_FILL_SOLID;
			description.CullMode = enabledCapabilities[kGDCapability_CullFace] ? D3D11_CULL_BACK : D3D11_CULL_NONE;
			description.FrontCounterClockwise = TRUE;
			description.DepthBias = polygonOffset;
			description.DepthClipEnable = TRUE;
			description.ScissorEnable = scissorEnabled;

			Microsoft::WRL::ComPtr<ID3D11RasterizerState> state;
			HRESULT const result = d3dDevice->CreateRasterizerState(&description, &state);
			if (FAILED(result)) {
				LogHRESULT(LogCategory::Resource, "ID3D11Device::CreateRasterizerState", result);
				return false;
			}
			rasterizerIterator = rasterizerStates.emplace(rasterizerKey, state).first;
		}
		d3dContext->RSSetState(rasterizerIterator->second.Get());

		if (shadeModel == 0) {
			Log(LogCategory::Unsupported, "flat shading requested; generic pipeline currently uses smooth interpolation");
		}
		return true;
	}
}
