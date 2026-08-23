/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include <cstdint>
#include <vector>
#include <d3d11.h>

namespace nSCGL {
    constexpr bool ClearsColor(uint32_t mask) {
        return (mask & 0x4000) != 0;
    }

    constexpr UINT D3D11DepthStencilClearFlags(uint32_t mask) {
        return ((mask & 0x1000) != 0 ? D3D11_CLEAR_DEPTH : 0) |
               ((mask & 0x2000) != 0 ? D3D11_CLEAR_STENCIL : 0);
    }

    constexpr int32_t D3D11TopLeftY(int32_t targetHeight, int32_t y, int32_t height) {
        return targetHeight - y - height;
    }

    struct D3D11Vertex {
        float position[3];
        float normal[3];
        uint8_t color[4];
        float texCoord[2][2];
    };

    uint64_t HashBytes(void const *data, size_t size, uint64_t hash = 14695981039346656037ull);

    bool IsSupportedVertexFormat(uint32_t format);

    bool ConvertVertices(
        uint32_t format,
        uint32_t stride,
        void const *source,
        uint32_t count,
        std::vector<D3D11Vertex> &destination);

    bool BuildSequentialIndices(uint32_t primitive, uint32_t vertexCount, std::vector<uint32_t> &indices);

    bool ConvertPrimitiveIndices(
        uint32_t primitive,
        std::vector<uint32_t> const &source,
        std::vector<uint32_t> &destination);

    D3D11_PRIMITIVE_TOPOLOGY D3D11Topology(uint32_t primitive);

    DXGI_FORMAT D3D11TextureFormat(uint32_t internalFormat);

    uint32_t D3D11TextureRowPitch(DXGI_FORMAT format, uint32_t width);

    uint32_t D3D11MipLevelCount(uint32_t width, uint32_t height);

    uint32_t D3D11MipDimension(uint32_t dimension, uint32_t level);

    D3D11_COMPARISON_FUNC D3D11Comparison(uint32_t function);

    D3D11_BLEND D3D11Blend(uint32_t blend);

    D3D11_BLEND D3D11AlphaBlend(uint32_t blend);

    bool IsValidBlendFunction(uint32_t source, uint32_t destination);

    D3D11_STENCIL_OP D3D11StencilOperation(uint32_t operation);
}
