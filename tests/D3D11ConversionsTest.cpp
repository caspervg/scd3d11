#include "D3D11Conversions.h"
#include "VertexFormatUtils.h"

#include <cassert>
#include <cstring>
#include <vector>
#include <wrl/client.h>

int main() {
    assert(!nSCGL::ClearsColor(0));
    assert(nSCGL::ClearsColor(0x4000));
    assert(nSCGL::D3D11DepthStencilClearFlags(0x1000) == D3D11_CLEAR_DEPTH);
    assert(nSCGL::D3D11DepthStencilClearFlags(0x2000) == D3D11_CLEAR_STENCIL);
    assert(nSCGL::D3D11DepthStencilClearFlags(0x7000) == (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL));
    assert(nSCGL::D3D11TopLeftY(1080, 0, 100) == 980);
    assert(nSCGL::D3D11TopLeftY(1080, 980, 100) == 0);
    assert(nSCGL::D3D11TopLeftY(1080, 0, 1080) == 0);
    uint8_t const hashInput[] = {1, 2, 3, 4};
    uint64_t splitHash = nSCGL::HashBytes(hashInput, 2);
    splitHash = nSCGL::HashBytes(hashInput + 2, 2, splitHash);
    assert(splitHash == nSCGL::HashBytes(hashInput, sizeof(hashInput)));
    assert(splitHash != nSCGL::HashBytes(hashInput, sizeof(hashInput) - 1));

    struct SourceVertex {
        float position[3];
        float normal[3];
        uint8_t bgra[4];
        float texCoord[2][2];
    } source{{1.0f, 2.0f, 3.0f}, {0.0f, 1.0f, 0.0f}, {10, 20, 30, 40}, {{0.25f, 0.5f}, {0.75f, 1.0f}}};
    std::vector<nSCGL::D3D11Vertex> vertices;
    assert(nSCGL::IsSupportedVertexFormat(kGDVertexFormat_V3F_N3F_C4UB_2T2F));
    assert(nSCGL::IsSupportedVertexFormat(RZMakeVertexFormat(kGDVertexFormat_V3F_N3F_C4UB_2T2F)));
    assert(!nSCGL::IsSupportedVertexFormat(39));
    assert(!nSCGL::IsSupportedVertexFormat(0x7fffffff));
    assert(nSCGL::ConvertVertices(kGDVertexFormat_V3F_N3F_C4UB_2T2F, sizeof(source), &source, 1, vertices));
    assert(vertices.size() == 1);
    assert(vertices[0].position[2] == 3.0f && vertices[0].normal[1] == 1.0f);
    assert(
        vertices[0].color[0] == 30 && vertices[0].color[1] == 20 && vertices[0].color[2] == 10 && vertices[0].color[3]
        == 40);
    assert(vertices[0].texCoord[1][0] == 0.75f && vertices[0].texCoord[1][1] == 1.0f);

    std::vector<uint32_t> indices;
    assert(nSCGL::BuildSequentialIndices(6, 4, indices));
    uint32_t const expectedQuad[] = {0, 1, 2, 0, 2, 3};
    assert(indices.size() == 6 && memcmp(indices.data(), expectedQuad, sizeof(expectedQuad)) == 0);
    assert(nSCGL::BuildSequentialIndices(7, 6, indices));
    uint32_t const expectedQuadStrip[] = {0, 1, 2, 2, 1, 3, 2, 3, 4, 4, 3, 5};
    assert(indices.size() == 12 && memcmp(indices.data(), expectedQuadStrip, sizeof(expectedQuadStrip)) == 0);
    assert(nSCGL::D3D11TextureFormat(7) == DXGI_FORMAT_BC3_UNORM);
    assert(nSCGL::D3D11TextureFormat(8) == DXGI_FORMAT_UNKNOWN);
    assert(nSCGL::D3D11TextureRowPitch(DXGI_FORMAT_BC1_UNORM, 5) == 16);
    assert(nSCGL::D3D11TextureRowPitch(DXGI_FORMAT_B8G8R8A8_UNORM, 5) == 20);
    assert(nSCGL::D3D11MipLevelCount(256, 128) == 9);
    assert(nSCGL::D3D11MipDimension(256, 8) == 1);
    assert(nSCGL::D3D11Comparison(3) == D3D11_COMPARISON_LESS_EQUAL);
    assert(nSCGL::D3D11Blend(5) == D3D11_BLEND_INV_SRC_ALPHA);
    assert(nSCGL::D3D11AlphaBlend(2) == D3D11_BLEND_SRC_ALPHA);
    assert(nSCGL::D3D11AlphaBlend(10) == D3D11_BLEND_ONE);
    assert(nSCGL::IsValidBlendFunction(10, 5));
    assert(!nSCGL::IsValidBlendFunction(1, 10));

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    D3D_FEATURE_LEVEL const featureLevels[]{D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, featureLevels, 3,
        D3D11_SDK_VERSION, &device, nullptr, nullptr)))
        return 1;
    for (uint32_t source = 0; source <= 10; ++source) {
        for (uint32_t destination = 0; destination <= 10; ++destination) {
            if (!nSCGL::IsValidBlendFunction(source, destination)) continue;
            D3D11_BLEND_DESC description{};
            D3D11_RENDER_TARGET_BLEND_DESC &target = description.RenderTarget[0];
            target.BlendEnable = TRUE;
            target.SrcBlend = nSCGL::D3D11Blend(source);
            target.DestBlend = nSCGL::D3D11Blend(destination);
            target.BlendOp = D3D11_BLEND_OP_ADD;
            target.SrcBlendAlpha = nSCGL::D3D11AlphaBlend(source);
            target.DestBlendAlpha = nSCGL::D3D11AlphaBlend(destination);
            target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
            target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            Microsoft::WRL::ComPtr<ID3D11BlendState> state;
            if (FAILED(device->CreateBlendState(&description, &state))) return 2;
        }
    }
    assert(nSCGL::D3D11StencilOperation(2) == D3D11_STENCIL_OP_INCR_SAT);
    return 0;
}
