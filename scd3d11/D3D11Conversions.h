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

    // PolygonOffset is taken as D3D7's ZBIAS, as SimGLDX7 would pass it on: 0 to 16, higher drawing nearer the
    // viewer. D3D11's DepthBias runs the other way in single depth units, so passing the offset straight
    // through pushed geometry a hair behind the ground it lies on, and it z-fought the ground in stripes.
    // Wine runs D3D7 ZBIAS as glPolygonOffset(-zbias, -zbias): a pull toward the viewer that also scales with
    // the depth slope, which the tilted camera gives every flat surface. This does the same.
    struct DepthBias {
        INT units;
        FLOAT slopeScale;
    };

    constexpr DepthBias D3D11DepthBias(int32_t polygonOffset) {
        int32_t const zbias = polygonOffset < 0 ? 0 : (polygonOffset > 16 ? 16 : polygonOffset);
        return {-zbias, -static_cast<FLOAT>(zbias)};
    }

    // The typeless format of a depth texture that takes a view of this depth-stencil format, so region copies
    // and the scratch copy always match the depth buffer.
    constexpr DXGI_FORMAT D3D11DepthTypelessFormat(DXGI_FORMAT depthStencilFormat) {
        return depthStencilFormat == DXGI_FORMAT_D24_UNORM_S8_UINT ? DXGI_FORMAT_R24G8_TYPELESS
             : depthStencilFormat == DXGI_FORMAT_D16_UNORM         ? DXGI_FORMAT_R16_TYPELESS
                                                                    : DXGI_FORMAT_R32_TYPELESS;
    }

    // The format a shader reads that depth texture through.
    constexpr DXGI_FORMAT D3D11DepthShaderFormat(DXGI_FORMAT depthStencilFormat) {
        return depthStencilFormat == DXGI_FORMAT_D24_UNORM_S8_UINT ? DXGI_FORMAT_R24_UNORM_X8_TYPELESS
             : depthStencilFormat == DXGI_FORMAT_D16_UNORM         ? DXGI_FORMAT_R16_UNORM
                                                                    : DXGI_FORMAT_R32_FLOAT;
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

    // Index keys are built from format, count and the exact bytes uploaded, except SequentialIndexKey.
    GeometryCacheKey IndexCacheKey(DXGI_FORMAT format, void const *indices, uint32_t count);

    // The 32-bit indices BuildSequentialIndices makes depend only on the primitive and the vertex count,
    // so their key is those two, as a generation key that no content key can equal.
    GeometryCacheKey SequentialIndexKey(uint32_t primitive, uint32_t vertexCount);

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

    // How many indices BuildSequentialIndices makes, 0 for a primitive or count it rejects.
    uint64_t SequentialIndexCount(uint32_t primitive, uint32_t vertexCount);

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
