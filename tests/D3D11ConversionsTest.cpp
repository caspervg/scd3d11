#include "D3D11Conversions.h"
#include "VertexFormatUtils.h"

#include <cassert>
#include <cstring>
#include <vector>
#include <wrl/client.h>

int main() {
    assert(!nSCD3D11::ClearsColor(0));
    assert(nSCD3D11::ClearsColor(0x4000));
    assert(nSCD3D11::D3D11DepthStencilClearFlags(0x1000) == D3D11_CLEAR_DEPTH);
    assert(nSCD3D11::D3D11DepthStencilClearFlags(0x2000) == D3D11_CLEAR_STENCIL);
    assert(nSCD3D11::D3D11DepthStencilClearFlags(0x7000) == (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL));
    assert(nSCD3D11::D3D11TopLeftY(1080, 0, 100) == 980);
    assert(nSCD3D11::D3D11TopLeftY(1080, 980, 100) == 0);
    assert(nSCD3D11::D3D11TopLeftY(1080, 0, 1080) == 0);
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
    assert(nSCD3D11::D3D11StencilOperation(2) == D3D11_STENCIL_OP_INCR_SAT);
    return 0;
}
