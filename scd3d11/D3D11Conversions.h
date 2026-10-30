/*
 *  SCD3D11 - a free graphics driver for SimCity 4's SimGL interface
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

#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace nSCD3D11 {
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

    // Whether [offset, offset + size) lies within [0, limit), without overflowing. Negative signed
    // arguments convert to huge values and fail.
    constexpr bool RangeFits(uint64_t offset, uint64_t size, uint64_t limit) {
        return size <= limit && offset <= limit - size;
    }

    // Whether source rows starting at base stay inside the address space: every row but the last
    // advances rowPitch bytes, and lastRowBytes are read from the last one.
    bool SourceSpanFits(void const *base, uint64_t rows, uint64_t rowPitch, uint64_t lastRowBytes);

    struct D3D11Vertex {
        float position[3];
        float normal[3];
        uint8_t color[4];
        float texCoord[2][2];
    };

    // Identifies uploaded geometry. The digest alone never decides a match: count and format say how
    // the bytes are interpreted, and generation keys (a reservation generation, not a content hash)
    // live in a separate key space from content keys. Content digests are XXH3-128. A match is still
    // only overwhelmingly likely, not proof, that the contents are equal.
    struct GeometryCacheKey {
        XXH128_hash_t digest{};
        // Never 0 for a real key, and below 2^31 because every upload's byte size fits 32 bits. The flag
        // shares its word, which keeps a key at 24 bytes.
        uint32_t count : 31 = 0;
        uint32_t generation : 1 = 0;
        uint32_t format = 0; // SimGL vertex format or DXGI index format

        bool operator==(GeometryCacheKey const &other) const {
            // Not XXH128_isEqual, which is an out-of-line call on every cache probe.
            return digest.low64 == other.digest.low64 && digest.high64 == other.digest.high64 &&
                   count == other.count && format == other.format && generation == other.generation;
        }
    };

    // The only way index keys are built: format, count and the exact bytes uploaded.
    GeometryCacheKey IndexCacheKey(DXGI_FORMAT format, void const *indices, uint32_t count);

    // Content key of interleaved vertices. Only the bytes the format consumes are hashed, so padding
    // between strided vertices never affects it and packed and padded copies of a mesh share a key.
    GeometryCacheKey VertexCacheKey(uint32_t format, uint32_t stride, void const *vertices, uint32_t count);

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
