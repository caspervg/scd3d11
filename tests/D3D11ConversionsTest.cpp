#include "D3D11Conversions.h"
#include "VertexFormatUtils.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>
#include <vector>
#include <windows.h>
#include <wrl/client.h>

namespace {
    using Digest = std::pair<unsigned long long, unsigned long long>;

    Digest ToPair(XXH128_hash_t const &hash) {
        return {hash.low64, hash.high64};
    }

    // The AES content hash: distinct digests for every length and every single-bit change, no read past
    // the end of its input, and the switch to XXH3 above AES_CONTENT_HASH_MAX_BYTES.
    void CheckContentHash() {
        using nSCD3D11::AesContentHash;
        using nSCD3D11::ContentHash;
        std::printf("content hash uses AES-NI: %s\n", nSCD3D11::ContentHashUsesAes() ? "yes" : "no");
        std::vector<uint8_t> bytes(8192);
        uint32_t seed = 99;
        for (uint8_t &byte: bytes) {
            seed = seed * 1664525u + 1013904223u;
            byte = static_cast<uint8_t>(seed >> 24);
        }
        if (!nSCD3D11::ContentHashUsesAes()) {
            assert(XXH128_isEqual(ContentHash(bytes.data(), 100), XXH3_128bits(bytes.data(), 100)));
            return;
        }
        std::set<Digest> seen;
        size_t inputs = 0;
        for (size_t size = 0; size <= 600; ++size, ++inputs) seen.insert(ToPair(AesContentHash(bytes.data(), size)));
        for (size_t size: {1u, 2u, 3u, 4u, 5u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u, 47u, 48u, 64u, 200u, 4096u}) {
            std::vector<uint8_t> message(bytes.begin(), bytes.begin() + size);
            for (size_t bit = 0; bit < size * 8; ++bit, ++inputs) {
                message[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
                seen.insert(ToPair(AesContentHash(message.data(), size)));
                message[bit / 8] ^= static_cast<uint8_t>(1u << (bit % 8));
            }
        }
        // Index-like data: small, mostly repeated values that differ in a few places.
        std::vector<uint16_t> indices(48);
        for (uint32_t base = 0; base < 50000; ++base, ++inputs) {
            for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<uint16_t>(base / 7 + (i * base) % 5);
            indices[base % indices.size()] ^= static_cast<uint16_t>(base);
            seen.insert(ToPair(AesContentHash(indices.data(), indices.size() * sizeof(uint16_t))));
        }
        // Equal inputs collapse in the set, so count those apart.
        std::set<std::vector<uint8_t>> distinctInputs;
        for (uint32_t base = 0; base < 50000; ++base) {
            for (size_t i = 0; i < indices.size(); ++i) indices[i] = static_cast<uint16_t>(base / 7 + (i * base) % 5);
            indices[base % indices.size()] ^= static_cast<uint16_t>(base);
            uint8_t const *raw = reinterpret_cast<uint8_t const *>(indices.data());
            distinctInputs.emplace(raw, raw + indices.size() * sizeof(uint16_t));
        }
        size_t const repeatedInputs = 50000 - distinctInputs.size();
        if (seen.size() != inputs - repeatedInputs) {
            std::fprintf(stderr, "AES content hash: %zu digests for %zu distinct inputs\n", seen.size(),
                         inputs - repeatedInputs);
            assert(false);
        }

        // Every size up to two blocks, ending right before a page the process may not read.
        SYSTEM_INFO system{};
        GetSystemInfo(&system);
        auto *const pages = static_cast<uint8_t *>(
            VirtualAlloc(nullptr, system.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        assert(pages != nullptr);
        DWORD previous = 0;
        assert(VirtualProtect(pages + system.dwPageSize, system.dwPageSize, PAGE_NOACCESS, &previous));
        uint8_t *const end = pages + system.dwPageSize;
        for (size_t size = 0; size <= 64; ++size) {
            memcpy(end - size, bytes.data(), size);
            assert(XXH128_isEqual(AesContentHash(end - size, size), AesContentHash(bytes.data(), size)));
        }
        VirtualFree(pages, 0, MEM_RELEASE);

        size_t const limit = nSCD3D11::AES_CONTENT_HASH_MAX_BYTES;
        assert(XXH128_isEqual(ContentHash(bytes.data(), limit), AesContentHash(bytes.data(), limit)));
        assert(XXH128_isEqual(ContentHash(bytes.data(), limit + 1), XXH3_128bits(bytes.data(), limit + 1)));

        // Padded vertices share the packed key on both sides of the limit (gathered below it, streamed
        // through XXH3 above it).
        for (uint32_t count: {200u, 300u}) {
            uint32_t const packedStride = 16, paddedStride = 20;
            std::vector<uint8_t> packed(count * packedStride), padded(count * paddedStride, 0xcd);
            for (uint32_t vertex = 0; vertex < count; ++vertex) {
                memcpy(&packed[vertex * packedStride], &bytes[vertex * packedStride], packedStride);
                memcpy(&padded[vertex * paddedStride], &bytes[vertex * packedStride], packedStride);
            }
            nSCD3D11::GeometryCacheKey const packedKey =
                nSCD3D11::VertexCacheKey(kGDVertexFormat_V3F_C4UB, packedStride, packed.data(), count);
            assert(packedKey == nSCD3D11::VertexCacheKey(kGDVertexFormat_V3F_C4UB, paddedStride, padded.data(), count));
            padded[paddedStride * (count / 2) + 3] ^= 1;
            assert(!(packedKey == nSCD3D11::VertexCacheKey(kGDVertexFormat_V3F_C4UB, paddedStride, padded.data(), count)));
        }
    }
}

int main() {
    CheckContentHash();
    assert(!nSCD3D11::ClearsColor(0));
    assert(nSCD3D11::ClearsColor(0x4000));
    assert(nSCD3D11::D3D11DepthStencilClearFlags(0x1000) == D3D11_CLEAR_DEPTH);
    assert(nSCD3D11::D3D11DepthStencilClearFlags(0x2000) == D3D11_CLEAR_STENCIL);
    assert(nSCD3D11::D3D11DepthStencilClearFlags(0x7000) == (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL));
    using nSCD3D11::D3D11DepthBias;
    assert(D3D11DepthBias(0).units == 0 && D3D11DepthBias(0).slopeScale == 0.0f);
    assert(D3D11DepthBias(2).units == -2 && D3D11DepthBias(2).slopeScale == -2.0f);
    assert(D3D11DepthBias(16).units == -16 && D3D11DepthBias(40).units == -16);
    assert(D3D11DepthBias(-1).units == 0 && D3D11DepthBias(-1).slopeScale == 0.0f);
    assert(nSCD3D11::D3D11TopLeftY(1080, 0, 100) == 980);
    assert(nSCD3D11::D3D11TopLeftY(1080, 980, 100) == 0);
    assert(nSCD3D11::D3D11TopLeftY(1080, 0, 1080) == 0);
    using nSCD3D11::RangeFits;
    assert(RangeFits(INT32_MAX - 1, 1, INT32_MAX) && !RangeFits(INT32_MAX, 1, INT32_MAX));
    assert(RangeFits(0, UINT32_MAX, UINT32_MAX) && !RangeFits(1, UINT32_MAX, UINT32_MAX));
    assert(!RangeFits(UINT32_MAX, 1, UINT32_MAX) && !RangeFits(2, 1, 1));
    assert(!RangeFits(int32_t{-1}, 1, INT32_MAX));
    using nSCD3D11::SourceSpanFits;
    void const *const top = reinterpret_cast<void const *>(UINTPTR_MAX - 99);
    assert(SourceSpanFits(top, 1, 0, 100) && !SourceSpanFits(top, 1, 0, 101));
    assert(SourceSpanFits(top, 2, 50, 50) && !SourceSpanFits(top, 2, 50, 51));
    assert(!SourceSpanFits(top, 0x100000000ull, UINT32_MAX, 1));
    assert(!SourceSpanFits(reinterpret_cast<void const *>(uintptr_t{1}), 5, 0x40000000u, 16));
    assert(!SourceSpanFits(nullptr, 1, 0, 1));

    // Split input must go through one streaming state, not digests chained as seeds.
    uint8_t const hashInput[] = {1, 2, 3, 4};
    XXH3_state_t hashState;
    XXH3_128bits_reset(&hashState);
    XXH3_128bits_update(&hashState, hashInput, 2);
    XXH3_128bits_update(&hashState, hashInput + 2, 2);
    XXH128_hash_t const splitHash = XXH3_128bits_digest(&hashState);
    assert(XXH128_isEqual(splitHash, XXH3_128bits(hashInput, sizeof(hashInput))));
    assert(!XXH128_isEqual(splitHash, XXH3_128bits(hashInput, sizeof(hashInput) - 1)));

    uint32_t const wideIndices[] = {5, 0, 1};
    uint16_t const narrowIndices[] = {5, 0, 0, 0, 1, 0};
    nSCD3D11::GeometryCacheKey const wideKey = nSCD3D11::IndexCacheKey(DXGI_FORMAT_R32_UINT, wideIndices, 3);
    nSCD3D11::GeometryCacheKey const narrowKey = nSCD3D11::IndexCacheKey(DXGI_FORMAT_R16_UINT, narrowIndices, 6);
    // Identical bytes, different interpretation.
    assert(XXH128_isEqual(wideKey.digest, narrowKey.digest) && !(wideKey == narrowKey));
    assert(wideKey == nSCD3D11::IndexCacheKey(DXGI_FORMAT_R32_UINT, wideIndices, 3));
    assert(!(wideKey == nSCD3D11::IndexCacheKey(DXGI_FORMAT_R32_UINT, wideIndices, 2)));
    {
        // Two V3F_C4UB vertices, packed (16 bytes each) and with 4 bytes of padding per vertex.
        uint8_t packed[32]{};
        uint8_t padded[40]{};
        for (uint8_t i = 0; i < 32; ++i) packed[i] = static_cast<uint8_t>(i + 1);
        memcpy(padded, packed, 16);
        memcpy(padded + 20, packed + 16, 16);
        memset(padded + 16, 0xAA, 4);
        using nSCD3D11::VertexCacheKey;
        nSCD3D11::GeometryCacheKey const packedKey = VertexCacheKey(kGDVertexFormat_V3F_C4UB, 16, packed, 2);
        assert(packedKey == VertexCacheKey(kGDVertexFormat_V3F_C4UB, 20, padded, 2));
        memset(padded + 16, 0x55, 4);
        memset(padded + 36, 0x55, 4);
        assert(packedKey == VertexCacheKey(kGDVertexFormat_V3F_C4UB, 20, padded, 2));
        ++padded[20 + 12]; // second vertex, color
        assert(!(packedKey == VertexCacheKey(kGDVertexFormat_V3F_C4UB, 20, padded, 2)));
        assert(!(packedKey == VertexCacheKey(kGDVertexFormat_V3F_C4UB, 16, packed, 1)));
        ++packed[3]; // first vertex, position
        assert(!(packedKey == VertexCacheKey(kGDVertexFormat_V3F_C4UB, 16, packed, 2)));
    }
    nSCD3D11::GeometryCacheKey generationKey = wideKey;
    generationKey.generation = true;
    assert(!(generationKey == wideKey));

    // Sequential indices are keyed by primitive and vertex count alone, never like the indices' bytes,
    // and their count is known without building them.
    using nSCD3D11::SequentialIndexKey;
    std::vector<uint32_t> sequential;
    for (uint32_t primitive = 0; primitive < 9; ++primitive) {
        for (uint32_t count = 0; count < 13; ++count) {
            bool const built = nSCD3D11::BuildSequentialIndices(primitive, count, sequential);
            assert(nSCD3D11::SequentialIndexCount(primitive, count) == (built ? sequential.size() : 0u));
            if (built) {
                assert(!(SequentialIndexKey(primitive, count) ==
                         nSCD3D11::IndexCacheKey(DXGI_FORMAT_R32_UINT, sequential.data(),
                                                 static_cast<uint32_t>(sequential.size()))));
            }
        }
    }
    assert(SequentialIndexKey(2, 4) == SequentialIndexKey(2, 4));
    assert(!(SequentialIndexKey(2, 4) == SequentialIndexKey(6, 4)) && !(SequentialIndexKey(2, 4) == SequentialIndexKey(2, 5)));
    assert(nSCD3D11::SequentialIndexCount(2, UINT32_MAX) == (uint64_t{UINT32_MAX} - 2) * 3);

    struct SourceVertex {
        float position[3];
        float normal[3];
        uint8_t bgra[4];
        float texCoord[2][2];
    } source{{1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}, {10, 20, 30, 40}, {{0.25f, 0.5f}, {0.75f, 1.0f}}};
    std::vector<nSCD3D11::D3D11Vertex> vertices;
    assert(nSCD3D11::IsSupportedVertexFormat(kGDVertexFormat_V3F_N3F_C4UB_2T2F));
    assert(nSCD3D11::IsSupportedVertexFormat(RZMakeVertexFormat(kGDVertexFormat_V3F_N3F_C4UB_2T2F)));
    assert(!nSCD3D11::IsSupportedVertexFormat(39));
    assert(!nSCD3D11::IsSupportedVertexFormat(0x7fffffff));
    assert(nSCD3D11::ConvertVertices(kGDVertexFormat_V3F_N3F_C4UB_2T2F, sizeof(source), &source, 1, vertices));
    assert(vertices.size() == 1);
    assert(vertices[0].position[2] == 3.0f && vertices[0].normal[1] == 1.0f);
    assert(
        vertices[0].color[0] == 30 && vertices[0].color[1] == 20 && vertices[0].color[2] == 10 && vertices[0].color[3]
        == 40);
    assert(vertices[0].texCoord[1][0] == 0.75f && vertices[0].texCoord[1][1] == 1.0f);

    std::vector<uint32_t> indices;
    assert(nSCD3D11::BuildSequentialIndices(6, 4, indices));
    uint32_t const expectedQuad[] = {0, 1, 2, 0, 2, 3};
    assert(indices.size() == 6 && memcmp(indices.data(), expectedQuad, sizeof(expectedQuad)) == 0);
    assert(nSCD3D11::BuildSequentialIndices(7, 6, indices));
    uint32_t const expectedQuadStrip[] = {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5};
    assert(indices.size() == 12 && memcmp(indices.data(), expectedQuadStrip, sizeof(expectedQuadStrip)) == 0);
    assert(nSCD3D11::D3D11TextureFormat(7) == DXGI_FORMAT_BC3_UNORM);
    assert(nSCD3D11::D3D11TextureFormat(8) == DXGI_FORMAT_UNKNOWN);
    assert(nSCD3D11::D3D11TextureRowPitch(DXGI_FORMAT_BC1_UNORM, 5) == 16);
    assert(nSCD3D11::D3D11TextureRowPitch(DXGI_FORMAT_B8G8R8A8_UNORM, 5) == 20);
    assert(nSCD3D11::D3D11MipLevelCount(256, 128) == 9);
    assert(nSCD3D11::D3D11MipDimension(256, 8) == 1);
    assert(nSCD3D11::D3D11Comparison(3) == D3D11_COMPARISON_LESS_EQUAL);
    assert(nSCD3D11::D3D11Blend(5) == D3D11_BLEND_INV_SRC_ALPHA);
    assert(nSCD3D11::D3D11AlphaBlend(2) == D3D11_BLEND_SRC_ALPHA);
    assert(nSCD3D11::D3D11AlphaBlend(10) == D3D11_BLEND_ONE);
    assert(nSCD3D11::IsValidBlendFunction(10, 5));
    assert(!nSCD3D11::IsValidBlendFunction(1, 10));

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL const featureLevels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevels, 3,
        D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        return 1;
    for (uint32_t source = 0; source <= 10; ++source) {
        for (uint32_t destination = 0; destination <= 10; ++destination) {
            if (!nSCD3D11::IsValidBlendFunction(source, destination)) continue;
            D3D11_BLEND_DESC description{};
            D3D11_RENDER_TARGET_BLEND_DESC &target = description.RenderTarget[0];
            target.BlendEnable = TRUE;
            target.SrcBlend = nSCD3D11::D3D11Blend(source);
            target.DestBlend = nSCD3D11::D3D11Blend(destination);
            target.BlendOp = D3D11_BLEND_OP_ADD;
            target.SrcBlendAlpha = nSCD3D11::D3D11AlphaBlend(source);
            target.DestBlendAlpha = nSCD3D11::D3D11AlphaBlend(destination);
            target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            Microsoft::WRL::ComPtr<ID3D11BlendState> state;
            if (FAILED(device->CreateBlendState(&description, &state))) return 2;
        }
    }
    assert(nSCD3D11::D3D11StencilOperation(0) == D3D11_STENCIL_OP_ZERO);
    assert(nSCD3D11::D3D11StencilOperation(1) == D3D11_STENCIL_OP_KEEP);
    assert(nSCD3D11::D3D11StencilOperation(2) == D3D11_STENCIL_OP_REPLACE);
    assert(nSCD3D11::D3D11StencilOperation(5) == D3D11_STENCIL_OP_INVERT);
    assert(nSCD3D11::D3D11StencilOperation(6) == 0);

    // The SSE2 index range matches a plain scan for every tail length and unaligned start, including
    // indices with the top bit set, which the signed compares only handle through the bias.
    {
        uint32_t seed = 12345;
        auto const next = [&seed] {
            seed = seed * 1664525u + 1013904223u;
            return seed;
        };
        std::vector<uint16_t> shorts(80);
        std::vector<uint32_t> longs(80);
        for (int round = 0; round < 200; ++round) {
            uint32_t const mask = round % 4 == 0 ? 0xffffffffu : round % 4 == 1 ? 0x7fffu : round % 4 == 2 ? 0xffu : 0x80000003u;
            for (uint32_t i = 0; i < shorts.size(); ++i) {
                uint32_t const value = next() & mask;
                shorts[i] = static_cast<uint16_t>(value | (round % 4 == 3 ? 0x8000u : 0u));
                longs[i] = value;
            }
            for (uint32_t start = 0; start < 4; ++start) {
                for (uint32_t count = 1; start + count <= shorts.size(); ++count) {
                    uint32_t minimum = 1, maximum = 1;
                    nSCD3D11::IndexRange(shorts.data() + start, count, minimum, maximum);
                    uint32_t lo = UINT32_MAX, hi = 0;
                    for (uint32_t i = start; i < start + count; ++i) {
                        lo = (std::min)(lo, static_cast<uint32_t>(shorts[i]));
                        hi = (std::max)(hi, static_cast<uint32_t>(shorts[i]));
                    }
                    assert(minimum == lo && maximum == hi);
                    nSCD3D11::IndexRange(longs.data() + start, count, minimum, maximum);
                    lo = UINT32_MAX;
                    hi = 0;
                    for (uint32_t i = start; i < start + count; ++i) {
                        lo = (std::min)(lo, longs[i]);
                        hi = (std::max)(hi, longs[i]);
                    }
                    assert(minimum == lo && maximum == hi);
                }
            }
        }
        uint16_t const extremes16[]{0xffff, 0, 0x8000, 0x7fff, 1, 0xfffe, 0x8001, 0x7ffe, 0xffff};
        uint32_t minimum = 0, maximum = 0;
        nSCD3D11::IndexRange(extremes16, 9, minimum, maximum);
        assert(minimum == 0 && maximum == 0xffff);
        uint32_t const extremes32[]{0xffffffffu, 0x80000000u, 0x7fffffffu, 5, 0x80000001u};
        nSCD3D11::IndexRange(extremes32, 5, minimum, maximum);
        assert(minimum == 5 && maximum == 0xffffffffu);
        nSCD3D11::IndexRange(extremes32 + 1, 4, minimum, maximum);
        assert(minimum == 5 && maximum == 0x80000001u);
    }
    return 0;
}
