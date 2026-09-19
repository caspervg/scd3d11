/*
 * Live indexed shadows for True3D props and prebuilt network pieces. The
 * native paths either see only positions/UVs or require a pre-baked mask; this
 * pass runs where the index buffer and alpha texture still exist.
 */

#include "cGDriver.h"
#include "Diagnostics.h"
#include "NativeShadowMasks.h"
#include "VertexFormatUtils.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <limits>

namespace nSCD3D11 {
    namespace {
        constexpr UINT kShadowMapSize = 2048;
        constexpr float kShadowDepthBiasWorld = 0.05f;

        bool LiveShadowDiagnosticsEnabled() {
            static bool const enabled = std::strstr(GetCommandLineA(), "-LiveShadowDiag") != nullptr;
            return enabled;
        }

        char const kLiveShadowShader[] = R"(
cbuffer ShadowConstants : register(b0) {
	column_major float4x4 lightMatrix;
	column_major float4x4 textureMatrix;
	float4 material;   // alpha function, reference, alpha-test enabled, textured
	float4 projection0; // p[0], p[5], p[10], p[12]
	float4 projection1; // p[13], p[14], map texel, opacity
};
Texture2D sourceTexture : register(t0);
Texture2D<float> sceneDepth : register(t0);
Texture2D<float> shadowMap : register(t1);
SamplerState sourceSampler : register(s0);
SamplerState shadowSampler : register(s1);

struct VSInput {
	float3 position : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float2 uv : TEXCOORD0;
	float2 uv1 : TEXCOORD1;
};
struct CasterOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };

CasterOutput CasterVS(VSInput input) {
	CasterOutput output;
	output.position = mul(lightMatrix, float4(input.position, 1.0));
	output.uv = mul(textureMatrix, float4(input.uv, 0.0, 1.0)).xy;
	return output;
}
void CasterPS(CasterOutput input) {
	float alpha = material.w != 0.0 ? sourceTexture.Sample(sourceSampler, input.uv).a : 1.0;
	uint function = (uint)material.x;
	float reference = material.y;
	bool alphaPass = material.z == 0.0 ? alpha > (1.0 / 255.0) :
		(function == 0 ? false : function == 1 ? alpha < reference :
		 function == 2 ? abs(alpha - reference) <= (1.0 / 255.0) :
		 function == 3 ? alpha <= reference : function == 4 ? alpha > reference :
		 function == 5 ? abs(alpha - reference) > (1.0 / 255.0) :
		 function == 6 ? alpha >= reference : true);
	clip(alphaPass ? 1.0 : -1.0);
}

float4 FullscreenVS(uint id : SV_VertexID) : SV_POSITION {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float4 CompositePS(float4 position : SV_POSITION) : SV_TARGET {
	uint width, height;
	sceneDepth.GetDimensions(width, height);
	int2 pixel = int2(position.xy);
	float depth = sceneDepth.Load(int3(pixel, 0));
	if (depth >= 0.999999) discard;
	float2 uv = (float2(pixel) + 0.5) / float2(width, height);
	float ndcX = uv.x * 2.0 - 1.0;
	float ndcY = 1.0 - uv.y * 2.0;
	float3 viewPosition;
	viewPosition.x = (ndcX - projection0.w) / projection0.x;
	viewPosition.y = (ndcY - projection1.x) / projection0.y;
	viewPosition.z = (depth * 2.0 - 1.0 - projection1.y) / projection0.z;
	float4 light = mul(lightMatrix, float4(viewPosition, 1.0));
	float2 shadowUV = light.xy * float2(0.5, -0.5) + 0.5;
	if (any(shadowUV < 0.0) || any(shadowUV > 1.0) || light.z < 0.0 || light.z > 1.0) discard;
	float visibility = 0.0;
	[unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
		float caster = shadowMap.SampleLevel(shadowSampler, shadowUV + float2(x, y) * projection1.z, 0);
		visibility += light.z > caster + material.x ? 1.0 : 0.0;
	}
	visibility /= 9.0;
	clip(visibility - 0.001);
	return float4(0.0, 0.0, 0.0, visibility * projection1.w);
}
)";

        struct ShadowConstants {
            float lightMatrix[16];
            float textureMatrix[16];
            float material[4];
            float projection0[4];
            float projection1[4];
        };

        void TransformPoint(float const* matrix, float const point[3], float out[3]) {
            out[0] = matrix[0] * point[0] + matrix[4] * point[1] + matrix[8] * point[2] + matrix[12];
            out[1] = matrix[1] * point[0] + matrix[5] * point[1] + matrix[9] * point[2] + matrix[13];
            out[2] = matrix[2] * point[0] + matrix[6] * point[1] + matrix[10] * point[2] + matrix[14];
        }

        float Dot(float const a[3], float const b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

        void Normalize(float vector[3]) {
            float const length = std::sqrt(Dot(vector, vector));
            if (length > 1.0e-6f) for (float& value : *reinterpret_cast<float (*)[3]>(vector)) value /= length;
        }

        void Cross(float const a[3], float const b[3], float out[3]) {
            out[0] = a[1] * b[2] - a[2] * b[1];
            out[1] = a[2] * b[0] - a[0] * b[2];
            out[2] = a[0] * b[1] - a[1] * b[0];
        }

        float BuildLightMatrix(
            float const minimum[3], float const maximum[3], float const shadowDirection[3], float matrix[16]) {
            float forward[3]{shadowDirection[0], shadowDirection[1], shadowDirection[2]};
            Normalize(forward);
            float helper[3]{
                std::fabs(forward[1]) < 0.95f ? 0.0f : 1.0f,
                std::fabs(forward[1]) < 0.95f ? 1.0f : 0.0f, 0.0f
            };
            float right[3]{};
            Cross(helper, forward, right);
            Normalize(right);
            float up[3]{};
            Cross(forward, right, up);
            Normalize(up);

            float low[3]{FLT_MAX, FLT_MAX, FLT_MAX};
            float high[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (unsigned mask = 0; mask < 8; ++mask) {
                float point[3]{
                    (mask & 1) ? maximum[0] : minimum[0],
                    (mask & 2) ? maximum[1] : minimum[1],
                    (mask & 4) ? maximum[2] : minimum[2]
                };
                float const values[3]{Dot(right, point), Dot(up, point), Dot(forward, point)};
                for (unsigned axis = 0; axis < 3; ++axis) {
                    low[axis] = (std::min)(low[axis], values[axis]);
                    high[axis] = (std::max)(high[axis], values[axis]);
                }
            }
            // XY only needs the caster footprint. Depth includes ample receiver
            // space because the scene depth buffer decides where shadows land.
            low[0] -= 1.0f;
            high[0] += 1.0f;
            low[1] -= 1.0f;
            high[1] += 1.0f;
            low[2] -= 4096.0f;
            high[2] += 4096.0f;
            std::memset(matrix, 0, sizeof(float) * 16);
            float const scales[3]{2.0f / (high[0] - low[0]), 2.0f / (high[1] - low[1]), 1.0f / (high[2] - low[2])};
            for (unsigned column = 0; column < 3; ++column) {
                matrix[column * 4 + 0] = right[column] * scales[0];
                matrix[column * 4 + 1] = up[column] * scales[1];
                matrix[column * 4 + 2] = forward[column] * scales[2];
            }
            matrix[12] = -(high[0] + low[0]) / (high[0] - low[0]);
            matrix[13] = -(high[1] + low[1]) / (high[1] - low[1]);
            matrix[14] = -low[2] / (high[2] - low[2]);
            matrix[15] = 1.0f;
            return high[2] - low[2];
        }

        void Multiply(float const* left, float const* right, float* out) {
            for (unsigned column = 0; column < 4; ++column)
                for (unsigned row = 0; row < 4; ++row) {
                    out[column * 4 + row] = 0.0f;
                    for (unsigned k = 0; k < 4; ++k) out[column * 4 + row] += left[k * 4 + row] * right[column * 4 + k];
                }
        }

    }

    bool cGDriver::MatchesLiveShadowMesh(uint32_t firstVertex, uint32_t vertexCount) {
        // The game-side prebuilt-network Draw hook brackets only the relevant
        // occupant renderer, so it is both cheaper and more precise than
        // hashing those meshes (or heuristically accepting all scene draws).
        ++liveShadowMatchCalls;
        if (NativeShadowMasks::LiveNetworkDrawActive()) {
            ++liveShadowMatchHits;
            return true;
        }
        if (!NativeShadowMasks::HasLivePropMeshes() || interleavedPointer == nullptr) return false;
        uint8_t const* const source = interleavedPointer + static_cast<size_t>(firstVertex) * interleavedStride;
        uint32_t const uvCount = RZVertexFormatNumElements(interleavedFormat, kGDElementType_TexCoord);
        if (vertexCount == 0 || interleavedStride < sizeof(float) * 3 || uvCount == 0) return false;
        uint32_t const uvOffset = RZVertexFormatElementOffset(interleavedFormat, kGDElementType_TexCoord, 0);
        uint64_t signature = 0xCBF29CE484222325ull;
        auto hashWord = [&](uint32_t word) { signature = (signature ^ word) * 0x100000001B3ull; };
        hashWord(vertexCount);
        for (uint32_t index = 0; index < vertexCount; ++index) {
            uint8_t const* const vertex = source + static_cast<size_t>(index) * interleavedStride;
            uint32_t words[5]{};
            std::memcpy(words, vertex, sizeof(float) * 3);
            std::memcpy(words + 3, vertex + uvOffset, sizeof(float) * 2);
            for (uint32_t word : words) hashWord(word);
        }
        bool const matched = NativeShadowMasks::MatchLivePropSignature(signature);
        if (matched) ++liveShadowMatchHits;
        return matched;
    }

    void cGDriver::CaptureLiveShadowDraw(
        uint32_t firstVertex, uint32_t vertexCount, std::vector<uint32_t> const& indices,
        D3D11_PRIMITIVE_TOPOLOGY topology) {
        if (indices.size() < 3) return;
        if (interleavedPointer == nullptr) {
            static bool loggedNullSource = false;
            if (!loggedNullSource) {
                loggedNullSource = true;
                Log(LogCategory::Initialization,
                    "live shadows: matched draw has no interleaved source; capture skipped");
            }
            return;
        }
        uint8_t const* const source = interleavedPointer + static_cast<size_t>(firstVertex) * interleavedStride;
        std::vector<D3D11Vertex> vertices;
        if (!ConvertVertices(interleavedFormat, interleavedStride, source, vertexCount, vertices)) {
            static bool loggedConvert = false;
            if (!loggedConvert) {
                loggedConvert = true;
                Log(LogCategory::Initialization,
                    "live shadows: matched draw failed vertex conversion (format=0x%08X stride=%u count=%u)",
                    interleavedFormat, interleavedStride, vertexCount);
            }
            return;
        }
        AppendLiveShadowDraw(std::move(vertices), indices, topology, NativeShadowMasks::LiveNetworkDrawActive());
    }

    void cGDriver::AppendLiveShadowDraw(
        std::vector<D3D11Vertex> vertices, std::vector<uint32_t> indices,
        D3D11_PRIMITIVE_TOPOLOGY topology, bool networkCaster) {
        if (vertices.empty() || indices.size() < 3) return;
        // Untextured geometry still casts a solid silhouette; the caster shader
        // already handles a null view via material.w == 0. Only textured draws
        // need a live sampler.
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> texture;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler = defaultSampler;
        if (textureStageEnabled[0]) {
            auto const entry = textures.find(boundTextures[0]);
            if (entry != textures.end() && entry->second.view) {
                if (FAILED(EnsureSampler(textureStages[0]))) {
                    static bool loggedSampler = false;
                    if (!loggedSampler) {
                        loggedSampler = true;
                        Log(LogCategory::Initialization,
                            "live shadows: sampler creation failed; textured captures skipped");
                    }
                    return;
                }
                texture = entry->second.view;
                sampler = textureStages[0].sampler ? textureStages[0].sampler : defaultSampler;
            }
        }
        LiveShadowDraw draw;
        draw.vertices = std::move(vertices);
        draw.indices = std::move(indices);
        draw.texture = texture;
        draw.sampler = sampler;
        std::memcpy(draw.modelView, matrices[MODEL_VIEW], sizeof(draw.modelView));
        std::memcpy(draw.projection, matrices[PROJECTION], sizeof(draw.projection));
        std::memcpy(draw.textureMatrix, textureStages[0].matrix, sizeof(draw.textureMatrix));
        draw.alphaFunction = alphaFunction;
        draw.alphaReference = alphaReference;
        draw.alphaTest = enabledCapabilities[kGDCapability_AlphaTest];
        draw.network = networkCaster;
        draw.topology = topology;
        size_t const loggedVertices = draw.vertices.size();
        size_t const loggedIndices = draw.indices.size();
        liveShadowDraws.push_back(std::move(draw));
        static bool loggedPropCapture = false;
        static bool loggedNetworkCapture = false;
        bool& loggedCapture = networkCaster ? loggedNetworkCapture : loggedPropCapture;
        if (!loggedCapture) {
            loggedCapture = true;
            Log(LogCategory::Initialization,
                "native shadows: matched first %s caster (%u vertices, %u indices)",
                networkCaster ? "prebuilt network" : "prop", static_cast<unsigned>(loggedVertices),
                static_cast<unsigned>(loggedIndices));
        }
    }

    void cGDriver::RenderLivePropShadows() {
        static uint64_t diagnosticFrame = 0;
        bool const diagnosticLog = LiveShadowDiagnosticsEnabled() && diagnosticFrame++ % 120 == 0;
        if (diagnosticLog && liveShadowDraws.empty())
            Log(LogCategory::Initialization, "liveshadow frame: no matched caster draws");
        if (liveShadowDraws.empty()) return;
        if (!IsDeviceReady() || !depthShaderView) {
            // Captures are frame-local. Never render stale geometry from a frame
            // where the depth buffer was temporarily unavailable.
            liveShadowDraws.clear();
            return;
        }
        // Resource creation and drawing are kept in this one routine so device
        // loss simply drops the pipeline alongside the driver's other resources.
        LiveShadowPipeline& pipeline = liveShadows;
        HRESULT result = S_OK;
        if (!pipeline.casterVS) {
            Microsoft::WRL::ComPtr<ID3DBlob> casterVS, casterPS, compositeVS, compositePS, messages;
            auto compile = [&](char const* entry, char const* target, ID3DBlob** output) {
                return D3DCompile(kLiveShadowShader, sizeof(kLiveShadowShader) - 1, "SCD3D11LiveShadows", nullptr,
                                  nullptr,
                                  entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, output, &messages);
            };
            result = compile("CasterVS", "vs_4_0", &casterVS);
            if (SUCCEEDED(result)) result = compile("CasterPS", "ps_4_0", &casterPS);
            if (SUCCEEDED(result)) result = compile("FullscreenVS", "vs_4_0", &compositeVS);
            if (SUCCEEDED(result)) result = compile("CompositePS", "ps_4_0", &compositePS);
            if (messages) Log(LogCategory::Resource, "live shadow shader compiler: %s",
                              static_cast<char const*>(messages->GetBufferPointer()));
            if (SUCCEEDED(result)) result = d3dDevice->CreateVertexShader(
                casterVS->GetBufferPointer(), casterVS->GetBufferSize(), nullptr, &pipeline.casterVS);
            if (SUCCEEDED(result)) result = d3dDevice->CreatePixelShader(
                casterPS->GetBufferPointer(), casterPS->GetBufferSize(), nullptr, &pipeline.casterPS);
            if (SUCCEEDED(result)) result = d3dDevice->CreateVertexShader(
                compositeVS->GetBufferPointer(), compositeVS->GetBufferSize(), nullptr, &pipeline.compositeVS);
            if (SUCCEEDED(result)) result = d3dDevice->CreatePixelShader(
                compositePS->GetBufferPointer(), compositePS->GetBufferSize(), nullptr, &pipeline.compositePS);
            D3D11_INPUT_ELEMENT_DESC const elements[]{
                {
                    "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,offsetof(D3D11Vertex, position),
                    D3D11_INPUT_PER_VERTEX_DATA, 0
                },
                {
                    "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,offsetof(D3D11Vertex, normal),
                    D3D11_INPUT_PER_VERTEX_DATA, 0
                },
                {
                    "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0,offsetof(D3D11Vertex, color), D3D11_INPUT_PER_VERTEX_DATA,
                    0
                },
                {
                    "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0,offsetof(D3D11Vertex, texCoord[0]),
                    D3D11_INPUT_PER_VERTEX_DATA, 0
                },
                {
                    "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0,offsetof(D3D11Vertex, texCoord[1]),
                    D3D11_INPUT_PER_VERTEX_DATA, 0
                }
            };
            if (SUCCEEDED(result)) result = d3dDevice->CreateInputLayout(
                elements, 5, casterVS->GetBufferPointer(), casterVS->GetBufferSize(), &pipeline.inputLayout);
            D3D11_BUFFER_DESC buffer{sizeof(ShadowConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
            if (SUCCEEDED(result)) result = d3dDevice->CreateBuffer(&buffer, nullptr, &pipeline.constants);
            D3D11_TEXTURE2D_DESC texture{};
            texture.Width = kShadowMapSize;
            texture.Height = kShadowMapSize;
            texture.MipLevels = 1;
            texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_R32_TYPELESS;
            texture.SampleDesc.Count = 1;
            texture.Usage = D3D11_USAGE_DEFAULT;
            texture.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(result)) result = d3dDevice->CreateTexture2D(&texture, nullptr, &pipeline.map);
            D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
            dsv.Format = DXGI_FORMAT_D32_FLOAT;
            dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
            if (SUCCEEDED(result)) result = d3dDevice->CreateDepthStencilView(
                pipeline.map.Get(), &dsv, &pipeline.mapDepth);
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            if (SUCCEEDED(result)) result = d3dDevice->CreateShaderResourceView(
                pipeline.map.Get(), &srv, &pipeline.mapView);
            D3D11_SAMPLER_DESC sampler{};
            sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sampler.MaxLOD = FLT_MAX;
            if (SUCCEEDED(result)) result = d3dDevice->CreateSamplerState(&sampler, &pipeline.mapSampler);
            D3D11_DEPTH_STENCIL_DESC depth{};
            depth.DepthEnable = TRUE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
            depth.DepthFunc = D3D11_COMPARISON_LESS;
            if (SUCCEEDED(result)) result = d3dDevice->CreateDepthStencilState(&depth, &pipeline.casterDepth);
            depth.DepthEnable = FALSE;
            depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
            if (SUCCEEDED(result)) result = d3dDevice->CreateDepthStencilState(&depth, &pipeline.compositeDepth);
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].BlendEnable = TRUE;
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            if (SUCCEEDED(result)) result = d3dDevice->CreateBlendState(&blend, &pipeline.shadowBlend);
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode = D3D11_FILL_SOLID;
            raster.CullMode = D3D11_CULL_NONE;
            raster.DepthClipEnable = TRUE;
            if (SUCCEEDED(result)) result = d3dDevice->CreateRasterizerState(&raster, &pipeline.rasterizer);
            if (FAILED(result)) {
                LogHRESULT(LogCategory::Resource, "live shadow pipeline", result);
                pipeline = LiveShadowPipeline{};
                liveShadowDraws.clear();
                return;
            }
            Log(LogCategory::Initialization, "native shadows: live indexed prop pipeline created");
        }

        // SC4PIM-X reproduces the city camera as R_x(pitch) * R_y(view - 22.5)
        // (plus the OpenGL Z reflection) and the vanilla sun as a view-locked
        // 45-degree ray. In that basis a one-unit-high point casts one world unit
        // horizontally on the ground. Therefore, in view space:
        //
        //     caster-to-receiver ray = screen-right ground vector - world-up
        //
        // Every normal SC4 prop placement is a translation, uniform scale and
        // rotation around world Y, so column 1 of its model-view matrix is the
        // camera's transformed world-up vector. Reading it directly avoids the
        // incorrect cS3D transform reconstruction which collapsed the old ray to
        // camera-forward (0,0,-1), and it remains valid at every camera pitch.
        float viewUp[3]{};
        unsigned viewUpSamples = 0;
        for (LiveShadowDraw const& draw : liveShadowDraws) {
            float candidate[3]{draw.modelView[4], draw.modelView[5], draw.modelView[6]};
            float const length = std::sqrt(Dot(candidate, candidate));
            if (length <= 1.0e-6f) continue;
            for (float& value : candidate) value /= length;
            if (candidate[1] < 0.0f) for (float& value : candidate) value = -value;
            viewUp[1] += candidate[1];
            viewUp[2] += candidate[2];
            ++viewUpSamples;
        }
        if (viewUpSamples == 0) {
            liveShadowDraws.clear();
            return;
        }
        // SC4's camera has no roll. Ignore any X residue from a nonstandard
        // model-local transform and use the shared camera pitch from Y/Z.
        viewUp[0] = 0.0f;
        Normalize(viewUp);
        float shadowDirection[3]{1.0f, -viewUp[1], -viewUp[2]};
        Normalize(shadowDirection);
        static bool loggedDirection = false;
        if (!loggedDirection) {
            loggedDirection = true;
            Log(LogCategory::Initialization,
                "native shadows: SC4 camera basis from %u prop draws; up %.4f/%.4f/%.4f, ray %.4f/%.4f/%.4f",
                viewUpSamples, viewUp[0], viewUp[1], viewUp[2],
                shadowDirection[0], shadowDirection[1], shadowDirection[2]);
        }

        float boundsLow[3]{FLT_MAX,FLT_MAX,FLT_MAX}, boundsHigh[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        for (LiveShadowDraw const& draw : liveShadowDraws)
            for (D3D11Vertex const& vertex : draw.vertices) {
                float view[3]{};
                TransformPoint(draw.modelView, vertex.position, view);
                for (unsigned axis = 0; axis < 3; ++axis) {
                    boundsLow[axis] = (std::min)(boundsLow[axis], view[axis]);
                    boundsHigh[axis] = (std::max)(boundsHigh[axis], view[axis]);
                }
            }
        float lightView[16]{};
        float const lightDepthRange = BuildLightMatrix(boundsLow, boundsHigh, shadowDirection, lightView);
        if (diagnosticLog) {
            size_t vertexCount = 0, indexCount = 0, networkCount = 0;
            float lightLow[3]{FLT_MAX,FLT_MAX,FLT_MAX}, lightHigh[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
            for (LiveShadowDraw const& draw : liveShadowDraws) {
                vertexCount += draw.vertices.size();
                indexCount += draw.indices.size();
                if (draw.network) ++networkCount;
                for (D3D11Vertex const& vertex : draw.vertices) {
                    float view[3]{}, light[3]{};
                    TransformPoint(draw.modelView, vertex.position, view);
                    TransformPoint(lightView, view, light);
                    for (unsigned axis = 0; axis < 3; ++axis) {
                        lightLow[axis] = (std::min)(lightLow[axis], light[axis]);
                        lightHigh[axis] = (std::max)(lightHigh[axis], light[axis]);
                    }
                }
            }
            Log(LogCategory::Initialization,
                "liveshadow frame: draws=%u (network=%u prop=%u) match=%llu/%llu vertices=%u indices=%u view=[%.3g %.3g %.3g]-[%.3g %.3g %.3g]",
                static_cast<unsigned>(liveShadowDraws.size()), static_cast<unsigned>(networkCount),
                static_cast<unsigned>(liveShadowDraws.size() - networkCount),
                static_cast<unsigned long long>(liveShadowMatchCalls),
                static_cast<unsigned long long>(liveShadowMatchHits),
                static_cast<unsigned>(vertexCount),
                static_cast<unsigned>(indexCount), boundsLow[0], boundsLow[1], boundsLow[2],
                boundsHigh[0], boundsHigh[1], boundsHigh[2]);
            Log(LogCategory::Initialization,
                "liveshadow projection: up=%.4f/%.4f/%.4f ray=%.4f/%.4f/%.4f "
                "light=[%.3g %.3g %.3g]-[%.3g %.3g %.3g] depth=%.3g bias=%.7g",
                viewUp[0], viewUp[1], viewUp[2], shadowDirection[0], shadowDirection[1], shadowDirection[2],
                lightLow[0], lightLow[1], lightLow[2], lightHigh[0], lightHigh[1], lightHigh[2],
                lightDepthRange, kShadowDepthBiasWorld / lightDepthRange);
        }
        d3dContext->ClearDepthStencilView(pipeline.mapDepth.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        d3dContext->OMSetRenderTargets(0, nullptr, pipeline.mapDepth.Get());
        D3D11_VIEWPORT mapViewport{0, 0, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0, 1};
        d3dContext->RSSetViewports(1, &mapViewport);
        d3dContext->RSSetState(pipeline.rasterizer.Get());
        d3dContext->OMSetDepthStencilState(pipeline.casterDepth.Get(), 0);
        d3dContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        d3dContext->IASetInputLayout(pipeline.inputLayout.Get());
        d3dContext->VSSetShader(pipeline.casterVS.Get(), nullptr, 0);
        d3dContext->PSSetShader(pipeline.casterPS.Get(), nullptr, 0);
        for (LiveShadowDraw const& draw : liveShadowDraws) {
            UINT const vertexBytes = static_cast<UINT>(draw.vertices.size() * sizeof(D3D11Vertex)), indexBytes =
                           static_cast<UINT>(draw.indices.size() * sizeof(uint32_t));
            auto ensureBuffer = [&](Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, uint32_t& capacity, UINT bytes,
                                    UINT bind) {
                if (buffer && capacity >= bytes) return S_OK;
                buffer.Reset();
                capacity = (std::max)(bytes, 65536u);
                D3D11_BUFFER_DESC desc{capacity, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE, 0, 0};
                return d3dDevice->CreateBuffer(&desc, nullptr, &buffer);
            };
            if (FAILED(ensureBuffer(pipeline.vertices,pipeline.vertexCapacity,vertexBytes,D3D11_BIND_VERTEX_BUFFER)) ||
                FAILED(ensureBuffer(pipeline.indices,pipeline.indexCapacity,indexBytes,D3D11_BIND_INDEX_BUFFER)))
                continue;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(d3dContext->Map(pipeline.vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) continue;
            std::memcpy(mapped.pData, draw.vertices.data(), vertexBytes);
            d3dContext->Unmap(pipeline.vertices.Get(), 0);
            if (FAILED(d3dContext->Map(pipeline.indices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) continue;
            std::memcpy(mapped.pData, draw.indices.data(), indexBytes);
            d3dContext->Unmap(pipeline.indices.Get(), 0);
            ShadowConstants constants{};
            Multiply(lightView, draw.modelView, constants.lightMatrix);
            std::memcpy(constants.textureMatrix, draw.textureMatrix, sizeof(constants.textureMatrix));
            constants.material[0] = static_cast<float>(draw.alphaFunction);
            constants.material[1] = draw.alphaReference;
            constants.material[2] = draw.alphaTest ? 1.0f : 0.0f;
            constants.material[3] = draw.texture ? 1.0f : 0.0f;
            d3dContext->UpdateSubresource(pipeline.constants.Get(), 0, nullptr, &constants, 0, 0);
            ID3D11Buffer* cb = pipeline.constants.Get();
            d3dContext->VSSetConstantBuffers(0, 1, &cb);
            d3dContext->PSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView* texture = draw.texture.Get();
            d3dContext->PSSetShaderResources(0, 1, &texture);
            ID3D11SamplerState* sampler = draw.sampler.Get();
            d3dContext->PSSetSamplers(0, 1, &sampler);
            UINT stride = sizeof(D3D11Vertex), offset = 0;
            ID3D11Buffer* vb = pipeline.vertices.Get();
            d3dContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            d3dContext->IASetIndexBuffer(pipeline.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
            d3dContext->IASetPrimitiveTopology(draw.topology);
            d3dContext->DrawIndexed(static_cast<UINT>(draw.indices.size()), 0, 0);
        }
        ID3D11ShaderResourceView* nullView = nullptr;
        d3dContext->PSSetShaderResources(0, 1, &nullView);

        LiveShadowDraw const& reference = liveShadowDraws.front();
        ShadowConstants composite{};
        std::memcpy(composite.lightMatrix, lightView, sizeof(lightView));
        composite.material[0] = kShadowDepthBiasWorld / lightDepthRange;
        composite.projection0[0] = reference.projection[0];
        composite.projection0[1] = reference.projection[5];
        composite.projection0[2] = reference.projection[10];
        composite.projection0[3] = reference.projection[12];
        composite.projection1[0] = reference.projection[13];
        composite.projection1[1] = reference.projection[14];
        composite.projection1[2] = 1.0f / kShadowMapSize;
        composite.projection1[3] = 0.38f;
        d3dContext->UpdateSubresource(pipeline.constants.Get(), 0, nullptr, &composite, 0, 0);
        ID3D11RenderTargetView* target = renderTargetView.Get();
        d3dContext->OMSetRenderTargets(1, &target, nullptr);
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0, 1};
        d3dContext->RSSetViewports(1, &viewport);
        d3dContext->RSSetState(pipeline.rasterizer.Get());
        d3dContext->OMSetDepthStencilState(pipeline.compositeDepth.Get(), 0);
        d3dContext->OMSetBlendState(pipeline.shadowBlend.Get(), nullptr, 0xffffffff);
        d3dContext->IASetInputLayout(nullptr);
        d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dContext->VSSetShader(pipeline.compositeVS.Get(), nullptr, 0);
        d3dContext->PSSetShader(pipeline.compositePS.Get(), nullptr, 0);
        ID3D11Buffer* cb = pipeline.constants.Get();
        d3dContext->VSSetConstantBuffers(0, 1, &cb);
        d3dContext->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* views[2]{depthShaderView.Get(), pipeline.mapView.Get()};
        d3dContext->PSSetShaderResources(0, 2, views);
        ID3D11SamplerState* samplers[2]{pipeline.mapSampler.Get(), pipeline.mapSampler.Get()};
        d3dContext->PSSetSamplers(0, 2, samplers);
        d3dContext->Draw(3, 0);
        views[0] = views[1] = nullptr;
        d3dContext->PSSetShaderResources(0, 2, views);
        liveShadowDraws.clear();
        d3dContext->ClearState();
        InvalidateD3D11StateCache();
        ID3D11RenderTargetView* restoreTarget = renderTargetView.Get();
        d3dContext->OMSetRenderTargets(1, &restoreTarget, depthStencilView.Get());
        if (scissorEnabled) SetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
        else SetViewport();
    }
}
