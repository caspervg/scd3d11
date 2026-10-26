/*
 * Live indexed shadows for True3D props and prebuilt network pieces
 * (-LiveShadows:on). SC4's own shadow paths only see positions and UVs; this
 * pass runs where the index buffer and alpha texture still exist.
 *
 * The same shadow map also takes every shadow record SC4's AddShadow made
 * (ShadowRegistry): buildings, flora, BAT props and poles cast through SC4's
 * own world-to-UV projector and prerendered texture, so their silhouettes match
 * the game's while landing on slopes and other buildings, and overlaps no
 * longer stack.
 *
 * The same composite also darkens whatever lies in the terrain's own shadow
 * (TerrainShadows), which SC4 never drew: hills shade the valleys and the
 * buildings behind them, with the same tone and without stacking.
 */

#include "cGDriver.h"
#include "D3D11Conversions.h"
#include "Diagnostics.h"
#include "LiveShadowHooks.h"
#include "ShadowRegistry.h"
#include "TerrainShadows.h"
#include "VertexFormatUtils.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <limits>
#include <new>

namespace nSCD3D11 {
    namespace {
        constexpr UINT kShadowMapSize = 2048;
        constexpr float kShadowDepthBiasWorld = 0.05f;
        // The black the casters composite with until SC4's own shadow
        // parameters have been read.
        constexpr float kFallbackShadowOpacity = 0.38f;
        // A registry caster's ground quad sits this far above the occupant's
        // placement height, so the terrain under it is behind it by more than
        // the composite bias along the 45-degree sun.
        constexpr float kGroundLiftWorld = 0.2f;
        // Used when SC4 reports no alpha-test scale: DrawShadows then blends the
        // texture alpha without a test, and the shadow map needs some cut. Also
        // the cut for True3D props cast through their own UVs.
        constexpr float kDefaultRegistryAlphaReference = 0.5f;
        constexpr UINT kRegistryVertexFloats = ShadowRegistry::kVertexFloats;
        // The per-draw caster arena starts at this many vertices and indices -
        // about 11 MB and 4 MB - and never grows past kArenaMaxBytes per
        // buffer, which also caps a pass's registry upload.
        constexpr uint32_t kArenaVertices = 1u << 18;
        constexpr uint32_t kArenaIndices = 1u << 20;
        constexpr uint64_t kArenaMaxBytes = 128ull << 20;
        // How far below a per-draw caster its shadow is assumed to fall when the
        // terrain altitude under it is unknown; only sizes redisplay regions.
        constexpr float kUnknownGroundDrop = 32.0f;
        // Two captures are the same caster when their shape matches and their
        // world boxes agree this closely; the view changes between passes, so
        // model-to-world is only reproduced up to rounding.
        constexpr float kSameCasterTolerance = 0.05f;
        // Network models used as textured road surfaces sit just above the
        // terrain. Their self-shadow only darkens the surface; an elevated deck
        // must still cast. Every vertex is checked against the terrain so slopes
        // do not make a ground-hugging piece look elevated.
        constexpr float kGroundNetworkCasterClearance = 0.5f;
        // A receiver starts to darken this far below the terrain's shadow
        // ceiling, and its penumbra - the gap down to the umbra - is never
        // narrower than this, so contact edges stay smooth.
        constexpr float kTerrainShadowBias = 0.5f;
        constexpr float kTerrainShadowSoftness = 3.0f;
        // A terrain draw's vertex lies this close to the height field when its
        // model space is world space.
        constexpr float kTerrainVertexTolerance = 0.05f;
        // A pixel this close to the drawn terrain surface is terrain.
        constexpr float kTerrainPixelTolerance = 1.0f;

        char const kLiveShadowShader[] = R"(
cbuffer ShadowConstants : register(b0) {
	column_major float4x4 lightMatrix;
	column_major float4x4 textureMatrix;
	float4 material;   // alpha function, reference, alpha-test enabled, textured
	float4 projection0; // p[0], p[5], p[10], p[12]
	float4 projection1; // p[13], p[14], map texel, opacity
	float4 uvBounds;    // registry casters: u min, v min, u max, v max
	float4 viewport;    // composite: target viewport x, y, width, height
	float4 tone;        // composite: shadow colour
	float4 sun;         // composite: direction light travels in view space, w = 1 / N.L of flat ground (0: all faces)
	column_major float4x4 eyeToWorld; // composite: view space to world, for the terrain map
	float4 terrainAxes; // composite: world xz to the map's along (xy) and across (zw) axes
	float4 terrainGrid; // composite: map uv = (along * x + y, across * z + w)
	float4 terrainShade; // composite: terrain map on, bias, minimum softness, caster map on
	float4 terrainCell;  // composite: cell width, how close to the terrain a terrain pixel lies, vertices x, z
};
Texture2D sourceTexture : register(t0);
Texture2D<float> sceneDepth : register(t0);
Texture2D<float> shadowMap : register(t1);
Texture2D<float2> terrainCeiling : register(t2); // ceiling, umbra
Texture2D<float4> terrainVertices : register(t3); // altitude, normal x, normal z, cell flipped
SamplerState sourceSampler : register(s0);
SamplerState shadowSampler : register(s1);
SamplerState terrainSampler : register(s2);

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

struct RegistryOutput {
	float4 position : SV_POSITION;
	float3 world : TEXCOORD0;
	float2 uv : TEXCOORD1;
};

RegistryOutput RegistryVS(float3 position : POSITION, float2 uv : TEXCOORD0) {
	RegistryOutput output;
	output.position = mul(lightMatrix, float4(position, 1.0));
	output.world = position;
	output.uv = uv;
	return output;
}
// A texel casts where SC4's own projected shadow would: the projector maps the
// point to the prerendered texture, the UV rectangle clips it as DrawShadows'
// second stage does, and the alpha test is DrawShadows' GEQUAL. A True3D prop
// (material.z) has real UVs instead and is alpha-tested through them.
void RegistryPS(RegistryOutput input) {
	if (material.z != 0.0) {
		clip(sourceTexture.Sample(sourceSampler, input.uv).a - material.y);
		return;
	}
	float2 uv = mul(textureMatrix, float4(input.world, 1.0)).xy;
	float2 inside = step(uvBounds.xy, uv) * step(uv, uvBounds.zw);
	clip(inside.x * inside.y - 0.5);
	clip(sourceTexture.Sample(sourceSampler, uv).a - material.y);
}

float4 FullscreenVS(uint id : SV_VertexID) : SV_POSITION {
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float3 ViewPosition(int2 pixel, float depth) {
	// A partial static pass renders its dirty rectangle through a projection
	// fitted to that rectangle, so NDC spans the viewport, not the window.
	float2 uv = (float2(pixel) + 0.5 - viewport.xy) / viewport.zw;
	float ndcX = uv.x * 2.0 - 1.0;
	float ndcY = 1.0 - uv.y * 2.0;
	float3 viewPosition;
	viewPosition.x = (ndcX - projection0.w) / projection0.x;
	viewPosition.y = (ndcY - projection1.x) / projection0.y;
	viewPosition.z = (depth * 2.0 - 1.0 - projection1.y) / projection0.z;
	return viewPosition;
}
// Of the two neighbours along one axis, the one on the same surface: the
// smaller depth step. Keeps silhouette edges from bending the normal.
float3 SurfaceStep(int2 pixel, float3 centre, int2 axis) {
	float3 forward = ViewPosition(pixel + axis, sceneDepth.Load(int3(pixel + axis, 0))) - centre;
	float3 backward = centre - ViewPosition(pixel - axis, sceneDepth.Load(int3(pixel - axis, 0)));
	return abs(forward.z) < abs(backward.z) ? forward : backward;
}
// The terrain under a world position as SC4 draws it: the cell's two
// triangles, split along the diagonal its flip flag picks, with the vertex
// normals its lighting uses interpolated across them. Returns the altitude.
float TerrainSurface(float2 xz, out float3 normal) {
	float2 grid = xz / terrainCell.x;
	int2 cell = clamp(int2(floor(grid)), int2(0, 0), int2(terrainCell.zw) - 2);
	float2 f = saturate(grid - float2(cell));
	float4 v00 = terrainVertices.Load(int3(cell, 0));
	float4 v10 = terrainVertices.Load(int3(cell + int2(1, 0), 0));
	float4 v01 = terrainVertices.Load(int3(cell + int2(0, 1), 0));
	float4 v11 = terrainVertices.Load(int3(cell + int2(1, 1), 0));
	float4 v;
	if (v00.w != 0.0) {
		v = f.x + f.y <= 1.0 ? v00 * (1.0 - f.x - f.y) + v10 * f.x + v01 * f.y
		                     : v11 * (f.x + f.y - 1.0) + v01 * (1.0 - f.x) + v10 * (1.0 - f.y);
	} else {
		v = f.x >= f.y ? v00 * (1.0 - f.x) + v10 * (f.x - f.y) + v11 * f.y
		               : v00 * (1.0 - f.y) + v01 * (f.y - f.x) + v11 * f.x;
	}
	normal = normalize(float3(v.y, sqrt(saturate(1.0 - v.y * v.y - v.z * v.z)), v.z));
	return v.x;
}

float4 CompositePS(float4 position : SV_POSITION) : SV_TARGET {
	int2 pixel = int2(position.xy);
	float depth = sceneDepth.Load(int3(pixel, 0));
	if (depth >= 0.999999) discard;
	float3 viewPosition = ViewPosition(pixel, depth);
	bool terrainMap = terrainShade.x != 0.0;
	float3 world = terrainMap ? mul(eyeToWorld, float4(viewPosition, 1.0)).xyz : float3(0.0, 0.0, 0.0);
	// A shadow takes away the sun's direct light, which SC4's prerendered art
	// and vertex lighting give a face in proportion to N.L. So it darkens fully
	// where N.L is that of flat ground - what SC4's shadow strength is set for -
	// less on a face turned partly away, and not at all on one turned fully
	// away, which is already unlit; shading that again only darkens it twice.
	float facing = 1.0;
	if (sun.w != 0.0) {
		float3 normal = normalize(cross(SurfaceStep(pixel, viewPosition, int2(0, 1)),
		                                SurfaceStep(pixel, viewPosition, int2(1, 0))));
		if (normal.z < 0.0) normal = -normal;
		float light = dot(normal, -sun.xyz);
		if (terrainMap) {
			// SC4 lights the terrain through vertex normals interpolated across
			// each triangle. The facets' own normals would switch the shadow on
			// and off along every crease, so terrain pixels use SC4's.
			float3 vertexNormal;
			float ground = TerrainSurface(world.xz, vertexNormal);
			float3 worldNormal = mul((float3x3)eyeToWorld, normal);
			if (abs(world.y - ground) < terrainCell.y && worldNormal.y > 0.3 * length(worldNormal))
				light = dot(vertexNormal, -normalize(mul((float3x3)eyeToWorld, sun.xyz)));
		}
		facing = saturate(light * sun.w);
		clip(facing - 0.001);
	}
	float shade = 0.0;
	if (terrainShade.w != 0.0) {
		float4 light = mul(lightMatrix, float4(viewPosition, 1.0));
		float2 shadowUV = light.xy * float2(0.5, -0.5) + 0.5;
		if (all(shadowUV >= 0.0) && all(shadowUV <= 1.0) && light.z >= 0.0 && light.z <= 1.0) {
			[unroll] for (int y = -1; y <= 1; ++y) [unroll] for (int x = -1; x <= 1; ++x) {
				float caster = shadowMap.SampleLevel(shadowSampler, shadowUV + float2(x, y) * projection1.z, 0);
				shade += light.z > caster + material.x ? 1.0 : 0.0;
			}
			shade /= 9.0;
		}
	}
	// The terrain up-sun of a point shadows it when the point lies below the
	// ceiling the map holds for its (x, z); walls and roofs read it the same way.
	// The larger of the two shadows wins, so they never stack.
	if (terrainMap) {
		float2 axes = float2(dot(world.xz, terrainAxes.xy), dot(world.xz, terrainAxes.zw));
		float2 ceiling = terrainCeiling.SampleLevel(terrainSampler, axes * terrainGrid.xz + terrainGrid.yw, 0);
		float penumbra = max(ceiling.x - ceiling.y, terrainShade.z);
		shade = max(shade, saturate((ceiling.x - world.y - terrainShade.y) / penumbra));
	}
	shade *= facing;
	clip(shade - 0.001);
	// Multiplied into the scene: a shadow takes the direct light away and
	// keeps the ground's own colour, tinted by SC4's shadow colour. Blending
	// towards that colour instead washes large terrain shadows out in blue.
	return float4(lerp(float3(1.0, 1.0, 1.0), tone.rgb, shade * projection1.w), 1.0);
}
)";

        struct ShadowConstants {
            float lightMatrix[16];
            float textureMatrix[16];
            float material[4];
            float projection0[4];
            float projection1[4];
            float uvBounds[4];
            float viewport[4];
            float tone[4];
            float sun[4];
            float eyeToWorld[16];
            float terrainAxes[4];
            float terrainGrid[4];
            float terrainShade[4];
            float terrainCell[4];
        };
        static_assert(sizeof(ShadowConstants) % 16 == 0, "constant buffers are sized in 16-byte registers");

        // A registry caster that survived culling, with where its geometry
        // landed in this pass's upload: its mesh, then its ground quad. The
        // quad always bounds the caster's reach; it is drawn only for
        // projector casters.
        struct RegistryDraw {
            ShadowRegistry::PassCaster const *caster = nullptr;
            ID3D11ShaderResourceView *texture = nullptr;
            float quad[4][3]{};
            bool hasQuad = false;
            bool drawQuad = false;
            UINT startIndex = 0;
            UINT indexCount = 0;
            INT baseVertex = 0;
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

        void TransformDirection(float const* matrix, float const direction[3], float out[3]) {
            for (unsigned row = 0; row < 3; ++row)
                out[row] = matrix[row] * direction[0] + matrix[4 + row] * direction[1] + matrix[8 + row] * direction[2];
        }

        // General 4x4 inverse by cofactors; the layout is irrelevant as long as
        // input and output share it.
        bool Invert(float const* m, float* out) {
            double inverse[16];
            inverse[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] +
                         m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
            inverse[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] -
                         m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
            inverse[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] +
                         m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
            inverse[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] -
                          m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
            inverse[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] -
                         m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
            inverse[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] +
                         m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
            inverse[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] -
                         m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
            inverse[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] +
                          m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
            inverse[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] +
                         m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
            inverse[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] -
                         m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
            inverse[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] +
                          m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
            inverse[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] -
                          m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
            inverse[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] -
                         m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
            inverse[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] +
                         m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
            inverse[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] -
                          m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
            inverse[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] +
                          m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
            double const determinant = m[0] * inverse[0] + m[1] * inverse[4] + m[2] * inverse[8] + m[3] * inverse[12];
            if (!std::isfinite(determinant) || std::fabs(determinant) < 1.0e-12) return false;
            for (unsigned index = 0; index < 16; ++index)
                out[index] = static_cast<float>(inverse[index] / determinant);
            return true;
        }

        enum class ViewportOverlap { Outside, Partial, Inside };

        // Where a world-space box lands in a pass's NDC square, which is the
        // pass's own viewport: full window or dirty rectangle alike.
        ViewportOverlap BoxInViewport(float const low[3], float const high[3], float const* worldToClip) {
            float ndcLow[2]{FLT_MAX, FLT_MAX}, ndcHigh[2]{-FLT_MAX, -FLT_MAX};
            for (unsigned mask = 0; mask < 8; ++mask) {
                float const corner[3]{
                    (mask & 1) ? high[0] : low[0], (mask & 2) ? high[1] : low[1], (mask & 4) ? high[2] : low[2]
                };
                for (unsigned axis = 0; axis < 2; ++axis) {
                    float const ndc = worldToClip[axis] * corner[0] + worldToClip[4 + axis] * corner[1] +
                                      worldToClip[8 + axis] * corner[2] + worldToClip[12 + axis];
                    ndcLow[axis] = (std::min)(ndcLow[axis], ndc);
                    ndcHigh[axis] = (std::max)(ndcHigh[axis], ndc);
                }
            }
            if (ndcHigh[0] < -1.0f || ndcLow[0] > 1.0f || ndcHigh[1] < -1.0f || ndcLow[1] > 1.0f)
                return ViewportOverlap::Outside;
            if (ndcLow[0] >= -1.0f && ndcHigh[0] <= 1.0f && ndcLow[1] >= -1.0f && ndcHigh[1] <= 1.0f)
                return ViewportOverlap::Inside;
            return ViewportOverlap::Partial;
        }

        // The ground quad a registry caster adds for silhouettes its mesh does
        // not cover from the sun (tree cards, thin LOD shells): the caster's box
        // pushed down the sun onto the plane at its placement height. Every
        // point of it shares its UV with the terrain point below along the sun,
        // so on flat ground it reproduces SC4's projected shadow exactly.
        bool GroundQuad(ShadowRegistry::Caster const& caster, float const sun[3], float quad[4][3]) {
            if (sun[1] > -1.0e-3f) return false;
            float const height = caster.baseHeight + kGroundLiftWorld;
            // The box's top and bottom corners land this far along the sun; the
            // footprint spans the box and both landings.
            float const alongLow = (height - caster.low[1]) / sun[1];
            float const alongHigh = (height - caster.high[1]) / sun[1];
            float low[2]{}, high[2]{};
            for (unsigned axis = 0; axis < 2; ++axis) {
                unsigned const world = axis * 2;
                float const a = sun[world] * alongLow, b = sun[world] * alongHigh;
                low[axis] = caster.low[world] + (std::min)({0.0f, a, b});
                high[axis] = caster.high[world] + (std::max)({0.0f, a, b});
            }
            float const corners[4][2]{{low[0], low[1]}, {high[0], low[1]}, {high[0], high[1]}, {low[0], high[1]}};
            for (unsigned index = 0; index < 4; ++index) {
                quad[index][0] = corners[index][0];
                quad[index][1] = height;
                quad[index][2] = corners[index][1];
            }
            return true;
        }

        double Milliseconds(LARGE_INTEGER const& start) {
            static LARGE_INTEGER const frequency = [] {
                LARGE_INTEGER value{};
                QueryPerformanceFrequency(&value);
                return value;
            }();
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return frequency.QuadPart != 0
                       ? static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 /
                         static_cast<double>(frequency.QuadPart)
                       : 0.0;
        }

        // Adds its scope's wall time to a -DrawStats total; costs a branch without -DrawStats.
        struct ScopeTimer {
            double* total;
            LARGE_INTEGER start{};

            explicit ScopeTimer(double& target) : total(DrawStatsEnabled() ? &target : nullptr) {
                if (total != nullptr) QueryPerformanceCounter(&start);
            }

            ~ScopeTimer() { if (total != nullptr) *total += Milliseconds(start); }
        };

        uint64_t gArenaGenerations = 0;

        // Appends one mesh to the arena; false when it does not fit.
        bool ArenaAppend(ID3D11DeviceContext* context, ShadowArena& arena, UINT stride, void const* vertices,
                         uint32_t vertexCount, uint32_t const* indices, uint32_t indexCount, uint32_t& firstVertex,
                         uint32_t& firstIndex) {
            if (arena.generation == 0 ||
                static_cast<uint64_t>(arena.vertexCursor) + vertexCount > arena.vertexCapacity ||
                static_cast<uint64_t>(arena.indexCursor) + indexCount > arena.indexCapacity)
                return false;
            D3D11_BOX const vertexBox{arena.vertexCursor * stride, 0, 0, (arena.vertexCursor + vertexCount) * stride, 1, 1};
            context->UpdateSubresource(arena.vertices.Get(), 0, &vertexBox, vertices, 0, 0);
            UINT const indexStride = sizeof(uint32_t);
            D3D11_BOX const indexBox{
                arena.indexCursor * indexStride, 0, 0, (arena.indexCursor + indexCount) * indexStride, 1, 1
            };
            context->UpdateSubresource(arena.indices.Get(), 0, &indexBox, indices, 0, 0);
            firstVertex = arena.vertexCursor;
            firstIndex = arena.indexCursor;
            arena.vertexCursor += vertexCount;
            arena.indexCursor += indexCount;
            return true;
        }

        // Empties the arena under a new generation, growing it to twice what
        // the current pass needs so the casters that follow find room too.
        bool ArenaRebuild(ID3D11Device* device, ShadowArena& arena, UINT stride, uint64_t needVertices,
                          uint64_t needIndices) {
            arena.vertexCursor = arena.indexCursor = 0;
            arena.generation = ++gArenaGenerations;
            uint64_t const vertices = (std::max)({
                static_cast<uint64_t>(arena.vertexCapacity), needVertices * 2, static_cast<uint64_t>(kArenaVertices)
            });
            uint64_t const indices = (std::max)({
                static_cast<uint64_t>(arena.indexCapacity), needIndices * 2, static_cast<uint64_t>(kArenaIndices)
            });
            if (vertices * stride > kArenaMaxBytes || indices * sizeof(uint32_t) > kArenaMaxBytes) {
                arena.generation = 0;
                return false;
            }
            if (arena.vertices && arena.indices && vertices == arena.vertexCapacity && indices == arena.indexCapacity)
                return true;
            arena.vertices.Reset();
            arena.indices.Reset();
            arena.vertexCapacity = arena.indexCapacity = 0;
            D3D11_BUFFER_DESC desc{
                static_cast<UINT>(vertices * stride), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0
            };
            HRESULT created = device->CreateBuffer(&desc, nullptr, &arena.vertices);
            desc.ByteWidth = static_cast<UINT>(indices * sizeof(uint32_t));
            desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
            if (SUCCEEDED(created)) created = device->CreateBuffer(&desc, nullptr, &arena.indices);
            if (FAILED(created)) {
                LogHRESULT(LogCategory::Resource, "shadow caster arena", created);
                arena.vertices.Reset();
                arena.indices.Reset();
                arena.generation = 0;
                return false;
            }
            arena.vertexCapacity = static_cast<uint32_t>(vertices);
            arena.indexCapacity = static_cast<uint32_t>(indices);
            return true;
        }

    }

    bool cGDriver::MatchesLiveShadowMesh(uint32_t firstVertex, uint32_t vertexCount) {
        // The game-side prebuilt-network Draw hook brackets only the relevant
        // occupant renderer, so it is both cheaper and more precise than
        // hashing those meshes (or heuristically accepting all scene draws).
        if (LiveShadowHooks::NetworkDrawActive()) return true;
        if (!LiveShadowHooks::HasPropMeshes() || interleavedPointer == nullptr) return false;
        uint8_t const* const source = interleavedPointer + static_cast<size_t>(firstVertex) * interleavedStride;
        uint32_t const uvCount = RZVertexFormatNumElements(interleavedFormat, kGDElementType_TexCoord);
        if (vertexCount == 0 || interleavedStride < sizeof(float) * 3 || uvCount == 0) return false;
        ScopeTimer const timer(frameStats.shadowHashMs);
        ++frameStats.shadowHashedDraws;
        frameStats.shadowHashedVertices += vertexCount;
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
        return LiveShadowHooks::MatchPropSignature(signature);
    }

    void cGDriver::CaptureLiveShadowDraw(
        uint32_t firstVertex, uint32_t vertexCount, std::vector<uint32_t> const& indices,
        D3D11_PRIMITIVE_TOPOLOGY topology) {
        if (indices.size() < 3) return;
        ScopeTimer const timer(frameStats.shadowCaptureMs);
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
        AppendLiveShadowDraw(std::move(vertices), indices, topology, LiveShadowHooks::NetworkDrawActive());
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
        ++frameStats.shadowCaptures;
        static bool loggedPropCapture = false;
        static bool loggedNetworkCapture = false;
        bool& loggedCapture = networkCaster ? loggedNetworkCapture : loggedPropCapture;
        if (!loggedCapture) {
            loggedCapture = true;
            Log(LogCategory::Initialization,
                "live shadows: matched first %s caster (%u vertices, %u indices)",
                networkCaster ? "prebuilt network" : "prop", static_cast<unsigned>(loggedVertices),
                static_cast<unsigned>(loggedIndices));
        }
    }

    void cGDriver::NoteLiveShadowTerrainView() {
        if (liveShadowTerrainViewValid || !LiveShadowHooks::Enabled()) return;
        std::memcpy(liveShadowTerrainView, matrices[MODEL_VIEW], sizeof(liveShadowTerrainView));
        // Called right after the reservation was uploaded, which holds at least
        // one vertex; its format leads with the position.
        std::memcpy(liveShadowTerrainVertex,
                    extensionVertexData.data() + static_cast<size_t>(extensionVertexStart) *
                                                 RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F),
                    sizeof(liveShadowTerrainVertex));
        liveShadowTerrainViewValid = true;
    }

    uint32_t cGDriver::ConsumeLiveShadowCleanRedraw() {
        uint32_t const pending = liveShadowCleanRedrawPending;
        liveShadowCleanRedrawPending = 0;
        return pending;
    }

    void cGDriver::CountForcedStaticRedraw(uint32_t causes) {
        ++frameStats.shadowForcedRedraws;
        for (uint32_t cause = 0; cause < kLiveShadowRedrawCauses; ++cause)
            if ((causes & (1u << cause)) != 0) ++frameStats.shadowForcedBy[cause];
    }

    // With the registry, live shadows keep the per-draw casters of every static pass
    // in world space. A full pass replaces the set. A partial pass updates it: a
    // caster drawn again replaces its entry, one SC4 should have redrawn (its box
    // meets the repainted rectangle) but did not is gone, and one not seen before
    // is new. The shadow of a caster that went, or of a new one reaching past the
    // rectangle, lands on pixels SC4 did not repaint, so that region is
    // redisplayed the way RedisplayStaticOverlay does for a shadow record: a small
    // partial pass on a later frame, which then finds the set already current.
    bool cGDriver::TrackLiveShadowWorldCasters(
        float const* eyeToWorld, float const* worldToView, float const* projection, float const sun[3],
        bool sunKnown, bool partialPass) {
        uint64_t const serial = ++liveShadowPassSerial;
        auto placeInWorld = [&](LiveShadowDraw& draw) {
            Multiply(eyeToWorld, draw.modelView, draw.modelToWorld);
            for (unsigned axis = 0; axis < 3; ++axis) {
                draw.worldLow[axis] = FLT_MAX;
                draw.worldHigh[axis] = -FLT_MAX;
            }
            for (D3D11Vertex const& vertex : draw.vertices) {
                float world[3]{};
                TransformPoint(draw.modelToWorld, vertex.position, world);
                for (unsigned axis = 0; axis < 3; ++axis) {
                    draw.worldLow[axis] = (std::min)(draw.worldLow[axis], world[axis]);
                    draw.worldHigh[axis] = (std::max)(draw.worldHigh[axis], world[axis]);
                }
            }
            uint64_t shape = 0xCBF29CE484222325ull;
            auto mix = [&](uint64_t value) { shape = (shape ^ value) * 0x100000001B3ull; };
            mix(reinterpret_cast<uintptr_t>(draw.texture.Get()));
            mix(draw.vertices.size());
            mix(draw.indices.size());
            mix(static_cast<uint64_t>(draw.topology));
            if (!draw.vertices.empty()) {
                uint32_t words[3]{};
                std::memcpy(words, draw.vertices.front().position, sizeof(words));
                for (uint32_t word : words) mix(word);
            }
            draw.shape = shape;
            draw.seenPass = serial;
        };
        // The box the caster's shadow can reach: its own box pushed down the sun
        // to the terrain under it.
        auto placeShadow = [&](LiveShadowDraw& draw) {
            float ground = draw.worldLow[1] - kUnknownGroundDrop;
            float altitude = 0.0f;
            if (TerrainShadows::Altitude((draw.worldLow[0] + draw.worldHigh[0]) * 0.5f,
                                         (draw.worldLow[2] + draw.worldHigh[2]) * 0.5f, altitude))
                ground = (std::min)(altitude, draw.worldLow[1]);
            std::memcpy(draw.shadowLow, draw.worldLow, sizeof(draw.shadowLow));
            std::memcpy(draw.shadowHigh, draw.worldHigh, sizeof(draw.shadowHigh));
            draw.shadowLow[1] = ground;
            if (!sunKnown || sun[1] > -1.0e-3f) {
                float const reach = draw.worldHigh[1] - ground;
                for (unsigned axis : {0u, 2u}) {
                    draw.shadowLow[axis] -= reach;
                    draw.shadowHigh[axis] += reach;
                }
                return;
            }
            for (unsigned mask = 0; mask < 8; ++mask) {
                float const corner[3]{
                    (mask & 1) ? draw.worldHigh[0] : draw.worldLow[0],
                    (mask & 2) ? draw.worldHigh[1] : draw.worldLow[1],
                    (mask & 4) ? draw.worldHigh[2] : draw.worldLow[2]
                };
                float const along = (ground - corner[1]) / sun[1];
                for (unsigned axis : {0u, 2u}) {
                    float const landed = corner[axis] + sun[axis] * along;
                    draw.shadowLow[axis] = (std::min)(draw.shadowLow[axis], landed);
                    draw.shadowHigh[axis] = (std::max)(draw.shadowHigh[axis], landed);
                }
            }
        };
        for (LiveShadowDraw& draw : liveShadowDraws) placeInWorld(draw);
        if (!partialPass) {
            for (LiveShadowDraw& draw : liveShadowDraws) placeShadow(draw);
            liveShadowWorldCasters = std::move(liveShadowDraws);
            liveShadowDraws.clear();
            liveShadowWorldCastersValid = true;
            return true;
        }
        if (!liveShadowWorldCastersValid) return false;

        float worldToClip[16]{};
        Multiply(projection, worldToView, worldToClip);
        std::vector<LiveShadowDraw const*> reached;
        std::vector<std::array<float, 4>> redisplay;
        auto queueRedisplay = [&](LiveShadowDraw const& draw) {
            redisplay.push_back({draw.shadowLow[0], draw.shadowLow[2], draw.shadowHigh[0], draw.shadowHigh[2]});
        };
        unsigned added = 0, removed = 0;
        size_t const known = liveShadowWorldCasters.size();
        for (LiveShadowDraw& draw : liveShadowDraws) {
            auto const same = [&](LiveShadowDraw const& cached) {
                if (cached.shape != draw.shape) return false;
                for (unsigned axis = 0; axis < 3; ++axis) {
                    if (std::fabs(cached.worldLow[axis] - draw.worldLow[axis]) > kSameCasterTolerance ||
                        std::fabs(cached.worldHigh[axis] - draw.worldHigh[axis]) > kSameCasterTolerance)
                        return false;
                }
                return true;
            };
            auto const cachedEnd = liveShadowWorldCasters.begin() + static_cast<ptrdiff_t>(known);
            auto const match = std::find_if(liveShadowWorldCasters.begin(), cachedEnd, same);
            if (match != cachedEnd) {
                std::memcpy(draw.shadowLow, match->shadowLow, sizeof(draw.shadowLow));
                std::memcpy(draw.shadowHigh, match->shadowHigh, sizeof(draw.shadowHigh));
                *match = std::move(draw);
                continue;
            }
            placeShadow(draw);
            if (BoxInViewport(draw.shadowLow, draw.shadowHigh, worldToClip) != ViewportOverlap::Inside) {
                queueRedisplay(draw);
            }
            liveShadowWorldCasters.push_back(std::move(draw));
            ++added;
        }
        liveShadowDraws.clear();
        auto const gone = [&](LiveShadowDraw const& cached) {
            if (cached.seenPass == serial ||
                BoxInViewport(cached.worldLow, cached.worldHigh, worldToClip) == ViewportOverlap::Outside)
                return false;
            queueRedisplay(cached);
            ++removed;
            return true;
        };
        liveShadowWorldCasters.erase(
            std::remove_if(liveShadowWorldCasters.begin(), liveShadowWorldCasters.end(), gone),
            liveShadowWorldCasters.end());
        for (auto const& box : redisplay)
            ShadowRegistry::RedisplayWorldRect(box[0], box[1], box[2], box[3]);
        frameStats.shadowRedisplays += static_cast<uint32_t>(redisplay.size());
        return true;
    }

    void cGDriver::RenderLivePropShadows(bool staticPass) {
        if (!LiveShadowHooks::Enabled()) return;
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
        DrawLivePropShadows(staticPass);
        double const milliseconds = Milliseconds(start);
        if (!staticPass) {
            frameStats.shadowDynamicMs += milliseconds;
            return;
        }
        frameStats.shadowStaticMs += milliseconds;
        frameStats.shadowStaticMaxMs = (std::max)(frameStats.shadowStaticMaxMs, milliseconds);
        frameStats.shadowWorldCasters = static_cast<uint32_t>(liveShadowWorldCasters.size());
    }

    void cGDriver::DrawLivePropShadows(bool staticPass) {
        // SC4 updates the static view in place for localized changes, handing the
        // driver the dirty rectangle as a scissored viewport. A shadow is not a
        // local effect: casters inside the rectangle throw onto receivers outside
        // it, and receivers inside it are darkened by casters that were never
        // redrawn. ShadowRegistry's casters arrive complete for the rectangle and
        // SC4 dirties a record's rectangle when it changes, and
        // TrackLiveShadowWorldCasters does the same for per-draw casters. A
        // partial pass that cannot be followed - or one whose terrain shadows
        // changed - composites what it honestly has, clipped to the rectangle,
        // and asks for a clean rebuild. A full-window pass is never treated as
        // partial, so this cannot drive a redraw loop.
        bool const partialPass =
            scissorEnabled && (viewportX > 0 || viewportY > 0 || viewportWidth < windowWidth ||
                               viewportHeight < windowHeight);
        if (staticPass) ++(partialPass ? frameStats.shadowPartialPasses : frameStats.shadowFullPasses);
        bool const registry = ShadowRegistry::Enabled();
        // SC4's own DrawShadows calls in this pass gathered the registry casters,
        // along with the view, sun and shadow parameters they were drawn with.
        // Without them the tone and sun are asked for directly, so a rotation
        // cannot leave a stale sun behind.
        ShadowRegistry::Pass pass;
        bool const havePass = ShadowRegistry::TakePass(pass);
        if (havePass) {
            std::memcpy(liveShadowColour, pass.colour, sizeof(liveShadowColour));
            liveShadowStrength = pass.strength;
            liveShadowToneValid = true;
        } else if (staticPass && LiveShadowHooks::ShadowParams(liveShadowColour, liveShadowStrength)) {
            liveShadowToneValid = true;
        }
        if (havePass && pass.sunValid) {
            std::memcpy(liveShadowSunWorld, pass.sunDirection, sizeof(liveShadowSunWorld));
            liveShadowSunWorldValid = true;
        } else if (staticPass && LiveShadowHooks::SunDirection(liveShadowSunWorld)) {
            liveShadowSunWorldValid = true;
        }
        // The terrain's shadow ceiling, rebuilt only when the height field or
        // the sun moved.
        TerrainShadowMap::Map const* terrainMap = nullptr;
        uint64_t terrainGeneration = 0;
        if (staticPass && liveShadowSunWorldValid && TerrainShadows::ShadowsRendered()) {
            ScopeTimer const timer(frameStats.shadowTerrainMs);
            try {
                terrainMap = TerrainShadows::Update(liveShadowSunWorld, terrainGeneration);
            } catch (std::bad_alloc const&) {
                terrainMap = nullptr;
            }
        }
        bool const terrainChanged = terrainMap != nullptr && terrainGeneration != liveShadowTerrainGeneration;
        if (terrainMap != nullptr) liveShadowTerrainGeneration = terrainGeneration;
        // DrawShadows' first argument maps eye space to world for SC4's texgen,
        // so its inverse is the view transform of every draw in this pass. A pass
        // without shadow records takes the model-view of its first terrain draw
        // instead: terrain vertices are world coordinates, which that draw's
        // first vertex lying on the city's height field confirms.
        float worldToView[16]{};
        float eyeToWorld[16]{};
        bool haveView = havePass && Invert(pass.eyeToWorld, worldToView);
        if (haveView) {
            std::memcpy(eyeToWorld, pass.eyeToWorld, sizeof(eyeToWorld));
        } else if (staticPass && liveShadowTerrainViewValid) {
            float altitude = 0.0f;
            bool const onTerrain =
                TerrainShadows::Altitude(liveShadowTerrainVertex[0], liveShadowTerrainVertex[2], altitude) &&
                std::fabs(altitude - liveShadowTerrainVertex[1]) <= kTerrainVertexTolerance;
            static bool loggedVertexCheck = false;
            if (!loggedVertexCheck) {
                loggedVertexCheck = true;
                Log(LogCategory::Initialization,
                    "live shadows: terrain vertex %.2f/%.2f/%.2f %s the height field (%.3f there)",
                    liveShadowTerrainVertex[0], liveShadowTerrainVertex[1], liveShadowTerrainVertex[2],
                    onTerrain ? "lies on" : "is off", altitude);
            }
            if (onTerrain && Invert(liveShadowTerrainView, eyeToWorld)) {
                std::memcpy(worldToView, liveShadowTerrainView, sizeof(worldToView));
                haveView = true;
            }
        }
        // The next pass - or the next frame after a scroll - has its own view.
        liveShadowTerrainViewValid = false;
        if (haveView) {
            liveShadowDraws.erase(
                std::remove_if(liveShadowDraws.begin(), liveShadowDraws.end(), [&](LiveShadowDraw const& draw) {
                    if (!draw.network) return false;
                    float modelToWorld[16]{};
                    Multiply(eyeToWorld, draw.modelView, modelToWorld);
                    for (D3D11Vertex const& vertex : draw.vertices) {
                        float world[3]{};
                        TransformPoint(modelToWorld, vertex.position, world);
                        float terrain = 0.0f;
                        if (!TerrainShadows::Altitude(world[0], world[2], terrain) ||
                            world[1] - terrain > kGroundNetworkCasterClearance)
                            return false;
                    }
                    return true;
                }),
                liveShadowDraws.end());
        }
        // The composite reconstructs view positions through the pass's own
        // projection, which a partial pass fits to its dirty rectangle.
        float projection[16]{};
        bool const haveProjection = !liveShadowDraws.empty() || liveShadowSceneProjectionValid;
        std::memcpy(projection, !liveShadowDraws.empty() ? liveShadowDraws.front().projection : liveShadowSceneProjection,
                    sizeof(projection));

        bool tracked = false;
        if (registry && staticPass) {
            if (haveView && haveProjection) {
                float sun[3]{liveShadowSunWorld[0], liveShadowSunWorld[1], liveShadowSunWorld[2]};
                Normalize(sun);
                ScopeTimer const timer(frameStats.shadowTrackMs);
                tracked = TrackLiveShadowWorldCasters(eyeToWorld, worldToView, projection, sun,
                                                      liveShadowSunWorldValid, partialPass);
            }
            if (!tracked) liveShadowWorldCastersValid = false;
        }
        // Without the registry a partial pass only ever sees the casters drawn
        // inside its own rectangle, so once one has cast it cannot be trusted.
        // With it, only a static pass the world-space set could not follow is.
        bool const castersStale =
            registry ? staticPass && !tracked && (!liveShadowDraws.empty() || !liveShadowWorldCasters.empty())
                     : liveShadowEverCaptured;
        if (staticPass && partialPass && castersStale) ++frameStats.shadowUntrackedPasses;
        if (partialPass && (castersStale || terrainChanged)) {
            liveShadowCleanRedrawPending |= (castersStale ? 1u << kRedrawUntrackedPartial : 0u) |
                                            (terrainChanged ? 1u << kRedrawTerrainChanged : 0u);
            static bool loggedPartial = false;
            if (!loggedPartial) {
                loggedPartial = true;
                Log(LogCategory::Initialization,
                    "live shadows: partial static pass (%dx%d at %d,%d) forces a clean redraw",
                    viewportWidth, viewportHeight, viewportX, viewportY);
            }
        }

        std::vector<LiveShadowDraw*> casters;
        if (tracked) {
            float worldToClip[16]{};
            Multiply(projection, worldToView, worldToClip);
            for (LiveShadowDraw& draw : liveShadowWorldCasters) {
                if (BoxInViewport(draw.shadowLow, draw.shadowHigh, worldToClip) == ViewportOverlap::Outside) continue;
                Multiply(worldToView, draw.modelToWorld, draw.modelView);
                casters.push_back(&draw);
            }
        } else {
            for (LiveShadowDraw& draw : liveShadowDraws) casters.push_back(&draw);
        }

        // Terrain shadows are composited in world space, so they need the view.
        bool const terrainShadows = terrainMap != nullptr && haveView;
        if (casters.empty() && pass.casters.empty() && !terrainShadows) {
            liveShadowDraws.clear();
            return;
        }
        if (!registry && !liveShadowDraws.empty()) liveShadowEverCaptured = true;
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
            Microsoft::WRL::ComPtr<ID3DBlob> casterVS, casterPS, compositeVS, compositePS, registryVS, registryPS,
                    messages;
            auto compile = [&](char const* entry, char const* target, ID3DBlob** output) {
                return D3DCompile(kLiveShadowShader, sizeof(kLiveShadowShader) - 1, "SCD3D11LiveShadows", nullptr,
                                  nullptr,
                                  entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0, output, &messages);
            };
            result = compile("CasterVS", "vs_4_0", &casterVS);
            if (SUCCEEDED(result)) result = compile("CasterPS", "ps_4_0", &casterPS);
            if (SUCCEEDED(result)) result = compile("FullscreenVS", "vs_4_0", &compositeVS);
            if (SUCCEEDED(result)) result = compile("CompositePS", "ps_4_0", &compositePS);
            if (SUCCEEDED(result)) result = compile("RegistryVS", "vs_4_0", &registryVS);
            if (SUCCEEDED(result)) result = compile("RegistryPS", "ps_4_0", &registryPS);
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
            if (SUCCEEDED(result)) result = d3dDevice->CreateVertexShader(
                registryVS->GetBufferPointer(), registryVS->GetBufferSize(), nullptr, &pipeline.registryVS);
            if (SUCCEEDED(result)) result = d3dDevice->CreatePixelShader(
                registryPS->GetBufferPointer(), registryPS->GetBufferSize(), nullptr, &pipeline.registryPS);
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
            D3D11_INPUT_ELEMENT_DESC const registryElements[]{
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
                {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, sizeof(float) * 3, D3D11_INPUT_PER_VERTEX_DATA, 0}
            };
            if (SUCCEEDED(result)) result = d3dDevice->CreateInputLayout(
                registryElements, 2, registryVS->GetBufferPointer(), registryVS->GetBufferSize(),
                &pipeline.registryLayout);
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
            // DrawShadows samples the prerendered texture bilinearly
            // (SetTexFiltering 1,1), clamped unless the record's wrap flag is set.
            // The terrain ceiling is filtered the same way.
            sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
            if (SUCCEEDED(result)) result = d3dDevice->CreateSamplerState(&sampler, &pipeline.clampSampler);
            sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            if (SUCCEEDED(result)) result = d3dDevice->CreateSamplerState(&sampler, &pipeline.wrapSampler);
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
            blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
            blend.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
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
            Log(LogCategory::Initialization, "live shadows: live indexed prop pipeline created");
        }

        if (!haveProjection) {
            liveShadowDraws.clear();
            return;
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
        // It is only the fallback: a pass with a view carries GetShadowDirection.
        float viewUp[3]{};
        unsigned viewUpSamples = 0;
        for (LiveShadowDraw const* caster : casters) {
            LiveShadowDraw const& draw = *caster;
            float candidate[3]{draw.modelView[4], draw.modelView[5], draw.modelView[6]};
            float const length = std::sqrt(Dot(candidate, candidate));
            if (length <= 1.0e-6f) continue;
            for (float& value : candidate) value /= length;
            if (candidate[1] < 0.0f) for (float& value : candidate) value = -value;
            viewUp[1] += candidate[1];
            viewUp[2] += candidate[2];
            ++viewUpSamples;
        }
        // SC4's camera has no roll. Ignore any X residue from a nonstandard
        // model-local transform and use the shared camera pitch from Y/Z.
        viewUp[0] = 0.0f;
        Normalize(viewUp);
        float estimatedDirection[3]{1.0f, -viewUp[1], -viewUp[2]};
        Normalize(estimatedDirection);
        float shadowDirection[3]{};
        bool gameDirection = false;
        if (haveView && liveShadowSunWorldValid) {
            TransformDirection(worldToView, liveShadowSunWorld, shadowDirection);
            gameDirection = Dot(shadowDirection, shadowDirection) > 1.0e-12f;
            Normalize(shadowDirection);
            // The sun only moves with a rotation, which redraws the static view,
            // so dynamic passes reuse it and agree with the static shadows.
            if (gameDirection) std::memcpy(liveShadowSunView, shadowDirection, sizeof(liveShadowSunView));
            liveShadowSunViewValid = gameDirection;
        } else if (liveShadowSunViewValid) {
            std::memcpy(shadowDirection, liveShadowSunView, sizeof(shadowDirection));
            gameDirection = true;
        }
        if (!gameDirection) {
            if (viewUpSamples == 0) {
                liveShadowDraws.clear();
                return;
            }
            std::memcpy(shadowDirection, estimatedDirection, sizeof(shadowDirection));
        }
        static bool loggedDirection = false;
        if (!loggedDirection) {
            loggedDirection = true;
            Log(LogCategory::Initialization,
                "live shadows: sun ray %.4f/%.4f/%.4f from %s; %u draws estimate %.4f/%.4f/%.4f",
                shadowDirection[0], shadowDirection[1], shadowDirection[2],
                gameDirection ? "GetShadowDirection" : "the camera estimate", viewUpSamples,
                estimatedDirection[0], estimatedDirection[1], estimatedDirection[2]);
        }
        float boundsLow[3]{FLT_MAX,FLT_MAX,FLT_MAX}, boundsHigh[3]{-FLT_MAX, -FLT_MAX, -FLT_MAX};
        auto includeBounds = [&](float const view[3]) {
            for (unsigned axis = 0; axis < 3; ++axis) {
                boundsLow[axis] = (std::min)(boundsLow[axis], view[axis]);
                boundsHigh[axis] = (std::max)(boundsHigh[axis], view[axis]);
            }
        };
        for (LiveShadowDraw const* caster : casters)
            for (D3D11Vertex const& vertex : caster->vertices) {
                float view[3]{};
                TransformPoint(caster->modelView, vertex.position, view);
                includeBounds(view);
            }

        // Registry casters that can shade this pass's viewport. The projection
        // is the pass's own, so its NDC square is exactly the rectangle being
        // drawn, full window or dirty rectangle alike.
        // Kept across passes: a pass can reach thousands of casters, and a fresh
        // vector each time is a large allocation per pass.
        static std::vector<RegistryDraw> registryDraws;
        registryDraws.clear();
        UINT registryVertexCount = 0, registryIndexCount = 0;
        unsigned registryCulled = 0, registryMissingTexture = 0, registryQuadOnly = 0, registryMeshCasters = 0;
        if (haveView) {
            ScopeTimer const timer(frameStats.shadowCullMs);
            float sunWorld[3]{liveShadowSunWorld[0], liveShadowSunWorld[1], liveShadowSunWorld[2]};
            if (!liveShadowSunWorldValid) TransformDirection(eyeToWorld, shadowDirection, sunWorld);
            Normalize(sunWorld);
            float worldToClip[16]{};
            Multiply(projection, worldToView, worldToClip);
            for (ShadowRegistry::PassCaster const& passCaster : pass.casters) {
                ShadowRegistry::Caster const& caster = *passCaster.caster;
                float quad[4][3];
                bool const hasQuad = GroundQuad(caster, sunWorld, quad);
                bool const drawQuad = hasQuad && !caster.meshUVs;
                if (caster.indices.empty() && !drawQuad) continue;
                // SC4 hands a strip pass thousands of casters, nearly all far
                // from it. Reject those first through the world box holding the
                // caster and its quad: the projection is affine, so the box's
                // NDC extent is its centre and half-size mapped through it.
                float low[3]{caster.low[0], caster.low[1], caster.low[2]};
                float high[3]{caster.high[0], caster.high[1], caster.high[2]};
                if (hasQuad) {
                    low[0] = (std::min)(low[0], quad[0][0]);
                    low[1] = (std::min)(low[1], quad[0][1]);
                    low[2] = (std::min)(low[2], quad[0][2]);
                    high[0] = (std::max)(high[0], quad[2][0]);
                    high[1] = (std::max)(high[1], quad[2][1]);
                    high[2] = (std::max)(high[2], quad[2][2]);
                }
                bool outside = false;
                for (unsigned axis = 0; axis < 2 && !outside; ++axis) {
                    float centre = worldToClip[12 + axis], extent = 0.0f;
                    for (unsigned k = 0; k < 3; ++k) {
                        centre += worldToClip[k * 4 + axis] * (low[k] + high[k]) * 0.5f;
                        extent += std::fabs(worldToClip[k * 4 + axis]) * (high[k] - low[k]) * 0.5f;
                    }
                    outside = centre + extent < -1.0f || centre - extent > 1.0f;
                }
                if (outside) {
                    ++registryCulled;
                    continue;
                }
                RegistryDraw draw;
                draw.caster = &passCaster;
                std::memcpy(draw.quad, quad, sizeof(quad));
                draw.hasQuad = hasQuad;
                draw.drawQuad = drawQuad;
                // The screen footprint of the caster together with its ground
                // shadow bounds everything it can shade, walls included: a
                // shadow on a wall lies between the caster and the ground.
                float points[12][3]{};
                unsigned pointCount = 0;
                for (unsigned mask = 0; mask < 8; ++mask) {
                    float const corner[3]{
                        (mask & 1) ? caster.high[0] : caster.low[0],
                        (mask & 2) ? caster.high[1] : caster.low[1],
                        (mask & 4) ? caster.high[2] : caster.low[2]
                    };
                    TransformPoint(worldToView, corner, points[pointCount++]);
                }
                if (draw.hasQuad)
                    for (auto const& corner : draw.quad) TransformPoint(worldToView, corner, points[pointCount++]);
                float ndcLow[2]{FLT_MAX, FLT_MAX}, ndcHigh[2]{-FLT_MAX, -FLT_MAX};
                for (unsigned index = 0; index < pointCount; ++index) {
                    float const* const view = points[index];
                    float const ndc[2]{
                        projection[0] * view[0] + projection[4] * view[1] + projection[8] * view[2] + projection[12],
                        projection[1] * view[0] + projection[5] * view[1] + projection[9] * view[2] + projection[13]
                    };
                    for (unsigned axis = 0; axis < 2; ++axis) {
                        ndcLow[axis] = (std::min)(ndcLow[axis], ndc[axis]);
                        ndcHigh[axis] = (std::max)(ndcHigh[axis], ndc[axis]);
                    }
                }
                if (ndcHigh[0] < -1.0f || ndcLow[0] > 1.0f || ndcHigh[1] < -1.0f || ndcLow[1] > 1.0f) {
                    ++registryCulled;
                    continue;
                }
                auto const texture = textures.find(passCaster.texture);
                if (texture == textures.end() || !texture->second.view) {
                    ++registryMissingTexture;
                    static bool loggedMissing = false;
                    if (!loggedMissing) {
                        loggedMissing = true;
                        Log(LogCategory::Initialization,
                            "shadow registry: caster %u names texture %u, which the driver does not have",
                            caster.id, passCaster.texture);
                    }
                    continue;
                }
                draw.texture = texture->second.view.Get();
                for (unsigned index = 0; index < pointCount; ++index) includeBounds(points[index]);
                draw.baseVertex = static_cast<INT>(registryVertexCount);
                draw.startIndex = registryIndexCount;
                draw.indexCount = static_cast<UINT>(caster.indices.size()) + (draw.drawQuad ? 6 : 0);
                registryVertexCount +=
                    (caster.indices.empty() ? 0 : static_cast<UINT>(caster.vertices.size() / kRegistryVertexFloats)) +
                    (draw.drawQuad ? 4 : 0);
                registryIndexCount += draw.indexCount;
                if (caster.indices.empty()) ++registryQuadOnly;
                if (caster.meshUVs) ++registryMeshCasters;
                registryDraws.push_back(draw);
            }
        }
        if (!registryDraws.empty() || registryCulled != 0 || registryMissingTexture != 0) {
            static bool loggedFirstDraw = false;
            if (!loggedFirstDraw) {
                loggedFirstDraw = true;
                Log(LogCategory::Initialization,
                    "shadow registry: first pass drew %u of %u casters (culled=%u notex=%u, view %s)",
                    static_cast<unsigned>(registryDraws.size()), static_cast<unsigned>(pass.casters.size()),
                    registryCulled, registryMissingTexture, haveView ? "known" : "unknown");
            }
        }
        // A pass with no caster in reach can still lie in the terrain's shadow;
        // it skips the shadow map and composites the terrain alone.
        bool const haveCasters = !casters.empty() || !registryDraws.empty();
        if (!haveCasters && !terrainShadows) {
            liveShadowDraws.clear();
            return;
        }
        // -DrawStats GPU timing of this call: shadow map, then composite.
        LiveShadowPipeline::GpuTimer* gpuTimer = nullptr;
        unsigned gpuStamps = 0;
        if (DrawStatsEnabled()) {
            for (LiveShadowPipeline::GpuTimer& timer : pipeline.gpuTimers) {
                if (!timer.pending) continue;
                D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
                UINT64 stamps[3]{};
                bool ready = d3dContext->GetData(timer.disjoint.Get(), &disjoint, sizeof(disjoint),
                                                 D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                for (unsigned index = 0; ready && index < 3; ++index)
                    ready = d3dContext->GetData(timer.stamps[index].Get(), &stamps[index], sizeof(UINT64),
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
                if (!ready) continue;
                timer.pending = false;
                if (disjoint.Disjoint || disjoint.Frequency == 0) continue;
                double const scale = 1000.0 / static_cast<double>(disjoint.Frequency);
                frameStats.shadowGpuMapMs += static_cast<double>(stamps[1] - stamps[0]) * scale;
                frameStats.shadowGpuCompositeMs += static_cast<double>(stamps[2] - stamps[1]) * scale;
                ++frameStats.shadowGpuSamples;
            }
            LiveShadowPipeline::GpuTimer& timer = pipeline.gpuTimers[pipeline.nextGpuTimer];
            if (!timer.pending) {
                D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
                bool created = timer.disjoint || SUCCEEDED(d3dDevice->CreateQuery(&query, &timer.disjoint));
                query.Query = D3D11_QUERY_TIMESTAMP;
                for (auto& stamp : timer.stamps)
                    created = created && (stamp || SUCCEEDED(d3dDevice->CreateQuery(&query, &stamp)));
                if (created) {
                    gpuTimer = &timer;
                    pipeline.nextGpuTimer = (pipeline.nextGpuTimer + 1) % std::size(pipeline.gpuTimers);
                    d3dContext->Begin(gpuTimer->disjoint.Get());
                    d3dContext->End(gpuTimer->stamps[gpuStamps++].Get());
                }
            }
        }
        // Ends the stamps up to `upTo`; the last one also closes the disjoint query.
        auto gpuStamp = [&](unsigned upTo) {
            if (gpuTimer == nullptr) return;
            while (gpuStamps <= upTo) d3dContext->End(gpuTimer->stamps[gpuStamps++].Get());
            if (gpuStamps == 3) {
                d3dContext->End(gpuTimer->disjoint.Get());
                gpuTimer->pending = true;
                gpuTimer = nullptr;
            }
        };
        float lightView[16]{};
        float lightDepthRange = 1.0f;
        if (haveCasters) lightDepthRange = BuildLightMatrix(boundsLow, boundsHigh, shadowDirection, lightView);
        float lightWorld[16]{};
        if (haveCasters && haveView) Multiply(lightView, worldToView, lightWorld);
        frameStats.shadowPassCasters += static_cast<uint32_t>(pass.casters.size());
        if (haveCasters) {
            ++frameStats.shadowMapRenders;
            frameStats.shadowRegistryCasters += static_cast<uint32_t>(registryDraws.size());
            d3dContext->ClearDepthStencilView(pipeline.mapDepth.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            d3dContext->OMSetRenderTargets(0, nullptr, pipeline.mapDepth.Get());
            D3D11_VIEWPORT mapViewport{
                0, 0, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize), 0, 1
            };
            d3dContext->RSSetViewports(1, &mapViewport);
            d3dContext->RSSetState(pipeline.rasterizer.Get());
            d3dContext->OMSetDepthStencilState(pipeline.casterDepth.Get(), 0);
            d3dContext->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            d3dContext->IASetInputLayout(pipeline.inputLayout.Get());
            d3dContext->VSSetShader(pipeline.casterVS.Get(), nullptr, 0);
            d3dContext->PSSetShader(pipeline.casterPS.Get(), nullptr, 0);
        }
        auto ensureBuffer = [&](Microsoft::WRL::ComPtr<ID3D11Buffer>& buffer, uint32_t& capacity, UINT bytes,
                                UINT bind) {
            if (buffer && capacity >= bytes) return S_OK;
            buffer.Reset();
            // Half again as much, so a growing city does not reallocate every pass.
            capacity = (std::max)(bytes + bytes / 2, 65536u);
            D3D11_BUFFER_DESC desc{capacity, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE, 0, 0};
            return d3dDevice->CreateBuffer(&desc, nullptr, &buffer);
        };
        ID3D11Buffer* cb = pipeline.constants.Get();
        // Captures already in the arena are drawn from where they were put;
        // only new ones are uploaded.
        UINT const casterStride = sizeof(D3D11Vertex);
        auto placeCasters = [&]() {
            for (LiveShadowDraw* draw : casters) {
                if (draw->arenaGeneration != 0 && draw->arenaGeneration == pipeline.casterArena.generation) continue;
                if (!ArenaAppend(d3dContext.Get(), pipeline.casterArena, casterStride, draw->vertices.data(),
                                 static_cast<uint32_t>(draw->vertices.size()), draw->indices.data(),
                                 static_cast<uint32_t>(draw->indices.size()), draw->arenaFirstVertex,
                                 draw->arenaFirstIndex))
                    return false;
                draw->arenaGeneration = pipeline.casterArena.generation;
            }
            return true;
        };
        bool castersReady = casters.empty() || placeCasters();
        if (!castersReady) {
            uint64_t needVertices = 0, needIndices = 0;
            for (LiveShadowDraw const* draw : casters) {
                needVertices += draw->vertices.size();
                needIndices += draw->indices.size();
            }
            ++frameStats.shadowArenaRebuilds;
            castersReady = ArenaRebuild(d3dDevice.Get(), pipeline.casterArena, casterStride, needVertices,
                                        needIndices) && placeCasters();
            if (!castersReady) {
                static bool loggedArena = false;
                if (!loggedArena) {
                    loggedArena = true;
                    Log(LogCategory::Resource, "live shadows: per-draw casters do not fit the caster arena");
                }
            }
        }
        if (castersReady) frameStats.shadowCasters += static_cast<uint32_t>(casters.size());
        if (castersReady && !casters.empty()) {
            UINT offset = 0;
            ID3D11Buffer* vb = pipeline.casterArena.vertices.Get();
            d3dContext->IASetVertexBuffers(0, 1, &vb, &casterStride, &offset);
            d3dContext->IASetIndexBuffer(pipeline.casterArena.indices.Get(), DXGI_FORMAT_R32_UINT, 0);
        }
        for (LiveShadowDraw const* caster : casters) {
            if (!castersReady) break;
            LiveShadowDraw const& draw = *caster;
            ShadowConstants constants{};
            Multiply(lightView, draw.modelView, constants.lightMatrix);
            std::memcpy(constants.textureMatrix, draw.textureMatrix, sizeof(constants.textureMatrix));
            constants.material[0] = static_cast<float>(draw.alphaFunction);
            constants.material[1] = draw.alphaReference;
            constants.material[2] = draw.alphaTest ? 1.0f : 0.0f;
            constants.material[3] = draw.texture ? 1.0f : 0.0f;
            d3dContext->UpdateSubresource(pipeline.constants.Get(), 0, nullptr, &constants, 0, 0);
            d3dContext->VSSetConstantBuffers(0, 1, &cb);
            d3dContext->PSSetConstantBuffers(0, 1, &cb);
            ID3D11ShaderResourceView* texture = draw.texture.Get();
            d3dContext->PSSetShaderResources(0, 1, &texture);
            ID3D11SamplerState* sampler = draw.sampler.Get();
            d3dContext->PSSetSamplers(0, 1, &sampler);
            d3dContext->IASetPrimitiveTopology(draw.topology);
            d3dContext->DrawIndexed(static_cast<UINT>(draw.indices.size()), draw.arenaFirstIndex,
                                    static_cast<INT>(draw.arenaFirstVertex));
        }

        // Registry casters: every caster's mesh and ground quad go into one
        // upload for the pass - SC4 creates and drops shadow records far too
        // often for a persistent copy to pay - then one draw each, since each
        // record carries its own projector and texture.
        if (!registryDraws.empty()) {
            UINT const registryStride = sizeof(float) * kRegistryVertexFloats;
            uint64_t const vertexBytes = static_cast<uint64_t>(registryVertexCount) * registryStride;
            uint64_t const indexBytes = static_cast<uint64_t>(registryIndexCount) * sizeof(uint32_t);
            D3D11_MAPPED_SUBRESOURCE vertexMap{}, indexMap{};
            bool uploaded = false;
            {
                ScopeTimer const timer(frameStats.shadowUploadMs);
                uploaded = vertexBytes <= kArenaMaxBytes && indexBytes <= kArenaMaxBytes &&
                           SUCCEEDED(ensureBuffer(pipeline.registryVertices, pipeline.registryVertexCapacity,
                                                  static_cast<UINT>(vertexBytes), D3D11_BIND_VERTEX_BUFFER)) &&
                           SUCCEEDED(ensureBuffer(pipeline.registryIndices, pipeline.registryIndexCapacity,
                                                  static_cast<UINT>(indexBytes), D3D11_BIND_INDEX_BUFFER)) &&
                           SUCCEEDED(d3dContext->Map(pipeline.registryVertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                                     &vertexMap));
                if (uploaded) {
                    uploaded = SUCCEEDED(d3dContext->Map(pipeline.registryIndices.Get(), 0, D3D11_MAP_WRITE_DISCARD,
                                                         0, &indexMap));
                    if (uploaded) {
                        auto* vertexOut = static_cast<float*>(vertexMap.pData);
                        auto* indexOut = static_cast<uint32_t*>(indexMap.pData);
                        for (RegistryDraw const& draw : registryDraws) {
                            ShadowRegistry::Caster const& caster = *draw.caster->caster;
                            // A caster without triangles casts from its quad alone.
                            size_t const meshFloats = caster.indices.empty() ? 0 : caster.vertices.size();
                            std::memcpy(vertexOut, caster.vertices.data(), meshFloats * sizeof(float));
                            vertexOut += meshFloats;
                            std::memcpy(indexOut, caster.indices.data(), caster.indices.size() * sizeof(uint32_t));
                            indexOut += caster.indices.size();
                            if (!draw.drawQuad) continue;
                            for (auto const& corner : draw.quad) {
                                std::memcpy(vertexOut, corner, sizeof(corner));
                                vertexOut[3] = vertexOut[4] = 0.0f;
                                vertexOut += kRegistryVertexFloats;
                            }
                            uint32_t const first = static_cast<uint32_t>(meshFloats / kRegistryVertexFloats);
                            uint32_t const quad[6]{first, first + 1, first + 2, first, first + 2, first + 3};
                            std::memcpy(indexOut, quad, sizeof(quad));
                            indexOut += 6;
                        }
                        d3dContext->Unmap(pipeline.registryIndices.Get(), 0);
                    }
                    d3dContext->Unmap(pipeline.registryVertices.Get(), 0);
                }
            }
            frameStats.shadowMeshUploads += static_cast<uint32_t>(registryDraws.size());
            frameStats.shadowUploadBytes += vertexBytes + indexBytes;
            ScopeTimer const submitTimer(frameStats.shadowSubmitMs);
            if (uploaded) {
                d3dContext->IASetInputLayout(pipeline.registryLayout.Get());
                d3dContext->VSSetShader(pipeline.registryVS.Get(), nullptr, 0);
                d3dContext->PSSetShader(pipeline.registryPS.Get(), nullptr, 0);
                d3dContext->VSSetConstantBuffers(0, 1, &cb);
                d3dContext->PSSetConstantBuffers(0, 1, &cb);
                d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11Buffer* const vertices = pipeline.registryVertices.Get();
                UINT const offset = 0;
                d3dContext->IASetVertexBuffers(0, 1, &vertices, &registryStride, &offset);
                d3dContext->IASetIndexBuffer(pipeline.registryIndices.Get(), DXGI_FORMAT_R32_UINT, 0);
                // DrawShadows tests strength * alpha against strength * alphaScale,
                // i.e. the texture's alpha against alphaScale.
                float const alphaReference =
                    pass.alphaScale > 0.0f ? pass.alphaScale : kDefaultRegistryAlphaReference;
                ShadowConstants constants{};
                std::memcpy(constants.lightMatrix, lightWorld, sizeof(lightWorld));
                for (RegistryDraw const& draw : registryDraws) {
                    if (draw.indexCount == 0) continue;
                    ShadowRegistry::Caster const& caster = *draw.caster->caster;
                    std::memcpy(constants.textureMatrix, caster.projector, sizeof(constants.textureMatrix));
                    std::memcpy(constants.uvBounds, caster.uvBounds, sizeof(constants.uvBounds));
                    constants.material[1] = caster.meshUVs ? kDefaultRegistryAlphaReference : alphaReference;
                    constants.material[2] = caster.meshUVs ? 1.0f : 0.0f;
                    d3dContext->UpdateSubresource(pipeline.constants.Get(), 0, nullptr, &constants, 0, 0);
                    ID3D11ShaderResourceView* texture = draw.texture;
                    d3dContext->PSSetShaderResources(0, 1, &texture);
                    // True3D UVs routinely run past [0, 1]: those textures repeat.
                    ID3D11SamplerState* sampler = draw.caster->wrap || caster.meshUVs
                                                      ? pipeline.wrapSampler.Get()
                                                      : pipeline.clampSampler.Get();
                    d3dContext->PSSetSamplers(0, 1, &sampler);
                    d3dContext->DrawIndexed(draw.indexCount, draw.startIndex, draw.baseVertex);
                }
            } else {
                static bool loggedUpload = false;
                if (!loggedUpload) {
                    loggedUpload = true;
                    Log(LogCategory::Resource, "shadow registry: caster upload failed (%llu vertex bytes, %llu index bytes)",
                        static_cast<unsigned long long>(vertexBytes), static_cast<unsigned long long>(indexBytes));
                }
            }
        }
        ID3D11ShaderResourceView* nullView = nullptr;
        d3dContext->PSSetShaderResources(0, 1, &nullView);

        // The terrain's shadow ceiling only changes with the height field or the
        // sun, so it is uploaded once per change rather than per pass.
        bool terrainBound = false;
        if (terrainShadows) {
            TerrainShadowMap::Map const& map = *terrainMap;
            if (pipeline.terrainMapGeneration != terrainGeneration) {
                if (!pipeline.terrainMap || pipeline.terrainMapWidth != map.width ||
                    pipeline.terrainMapHeight != map.height) {
                    pipeline.terrainMap.Reset();
                    pipeline.terrainMapView.Reset();
                    D3D11_TEXTURE2D_DESC texture{};
                    texture.Width = map.width;
                    texture.Height = map.height;
                    texture.MipLevels = 1;
                    texture.ArraySize = 1;
                    texture.Format = DXGI_FORMAT_R32G32_FLOAT;
                    texture.SampleDesc.Count = 1;
                    texture.Usage = D3D11_USAGE_DEFAULT;
                    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    HRESULT created = d3dDevice->CreateTexture2D(&texture, nullptr, &pipeline.terrainMap);
                    if (SUCCEEDED(created))
                        created = d3dDevice->CreateShaderResourceView(pipeline.terrainMap.Get(), nullptr,
                                                                      &pipeline.terrainMapView);
                    if (FAILED(created)) {
                        LogHRESULT(LogCategory::Resource, "terrain shadow map", created);
                        pipeline.terrainMap.Reset();
                        pipeline.terrainMapView.Reset();
                    }
                    pipeline.terrainMapWidth = pipeline.terrainMap ? map.width : 0;
                    pipeline.terrainMapHeight = pipeline.terrainMap ? map.height : 0;
                }
                TerrainShadowMap::HeightField const& field = TerrainShadows::Terrain();
                // Altitude, normal x and z (its y is up and recovered), and the
                // flip of the cell this vertex is the lowest corner of.
                // Runs inside a game hook: an exception must not unwind into SC4.
                std::vector<float> vertices;
                try {
                    vertices.resize(static_cast<size_t>(field.verticesX) * field.verticesZ * 4);
                } catch (std::bad_alloc const&) {
                    vertices.clear();
                }
                for (uint32_t z = 0; !vertices.empty() && z < field.verticesZ; ++z) {
                    for (uint32_t x = 0; x < field.verticesX; ++x) {
                        size_t const index = static_cast<size_t>(z) * field.verticesX + x;
                        float normal[3]{};
                        TerrainShadowMap::VertexNormal(field, x, z, normal);
                        vertices[index * 4 + 0] = field.altitudes[index];
                        vertices[index * 4 + 1] = normal[0];
                        vertices[index * 4 + 2] = normal[2];
                        vertices[index * 4 + 3] = !field.flipped.empty() && field.flipped[index] != 0 ? 1.0f : 0.0f;
                    }
                }
                if (!pipeline.terrainAltitude || pipeline.terrainAltitudeWidth != field.verticesX ||
                    pipeline.terrainAltitudeHeight != field.verticesZ) {
                    pipeline.terrainAltitude.Reset();
                    pipeline.terrainAltitudeView.Reset();
                    D3D11_TEXTURE2D_DESC texture{};
                    texture.Width = field.verticesX;
                    texture.Height = field.verticesZ;
                    texture.MipLevels = 1;
                    texture.ArraySize = 1;
                    texture.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                    texture.SampleDesc.Count = 1;
                    texture.Usage = D3D11_USAGE_DEFAULT;
                    texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    HRESULT created = d3dDevice->CreateTexture2D(&texture, nullptr, &pipeline.terrainAltitude);
                    if (SUCCEEDED(created))
                        created = d3dDevice->CreateShaderResourceView(pipeline.terrainAltitude.Get(), nullptr,
                                                                      &pipeline.terrainAltitudeView);
                    if (FAILED(created)) {
                        LogHRESULT(LogCategory::Resource, "terrain altitude map", created);
                        pipeline.terrainAltitude.Reset();
                        pipeline.terrainAltitudeView.Reset();
                    }
                    pipeline.terrainAltitudeWidth = pipeline.terrainAltitude ? field.verticesX : 0;
                    pipeline.terrainAltitudeHeight = pipeline.terrainAltitude ? field.verticesZ : 0;
                }
                std::vector<float> ceilings;
                try {
                    ceilings.resize(map.ceiling.size() * 2);
                } catch (std::bad_alloc const&) {
                    ceilings.clear();
                }
                for (size_t index = 0; !ceilings.empty() && index < map.ceiling.size(); ++index) {
                    ceilings[index * 2] = map.ceiling[index];
                    ceilings[index * 2 + 1] = map.umbra[index];
                }
                if (pipeline.terrainMap && pipeline.terrainAltitude && !vertices.empty() && !ceilings.empty()) {
                    d3dContext->UpdateSubresource(pipeline.terrainMap.Get(), 0, nullptr, ceilings.data(),
                                                  map.width * sizeof(float) * 2, 0);
                    d3dContext->UpdateSubresource(pipeline.terrainAltitude.Get(), 0, nullptr, vertices.data(),
                                                  field.verticesX * sizeof(float) * 4, 0);
                    pipeline.terrainMapGeneration = terrainGeneration;
                }
            }
            terrainBound = pipeline.terrainMapView != nullptr && pipeline.terrainAltitudeView != nullptr &&
                           pipeline.terrainMapGeneration == terrainGeneration;
        }
        gpuStamp(1);
        if (!haveCasters && !terrainBound) {
            gpuStamp(2);
            liveShadowDraws.clear();
            return;
        }

        ShadowConstants composite{};
        std::memcpy(composite.lightMatrix, lightView, sizeof(lightView));
        composite.material[0] = kShadowDepthBiasWorld / lightDepthRange;
        composite.terrainShade[3] = haveCasters ? 1.0f : 0.0f;
        if (terrainBound) {
            TerrainShadowMap::Map const& map = *terrainMap;
            std::memcpy(composite.eyeToWorld, eyeToWorld, sizeof(eyeToWorld));
            composite.terrainAxes[0] = map.along[0];
            composite.terrainAxes[1] = map.along[1];
            composite.terrainAxes[2] = map.across[0];
            composite.terrainAxes[3] = map.across[1];
            // Texel i's centre, at originAlong + i * spacing, samples at (i + 0.5) / width.
            composite.terrainGrid[0] = 1.0f / (map.spacing * static_cast<float>(map.width));
            composite.terrainGrid[1] = (0.5f - map.originAlong / map.spacing) / static_cast<float>(map.width);
            composite.terrainGrid[2] = 1.0f / (map.spacing * static_cast<float>(map.height));
            composite.terrainGrid[3] = (0.5f - map.originAcross / map.spacing) / static_cast<float>(map.height);
            composite.terrainShade[0] = 1.0f;
            composite.terrainShade[1] = kTerrainShadowBias;
            composite.terrainShade[2] = kTerrainShadowSoftness;
            TerrainShadowMap::HeightField const& field = TerrainShadows::Terrain();
            composite.terrainCell[0] = field.cellWidth;
            composite.terrainCell[1] = kTerrainPixelTolerance;
            composite.terrainCell[2] = static_cast<float>(field.verticesX);
            composite.terrainCell[3] = static_cast<float>(field.verticesZ);
        }
        composite.projection0[0] = projection[0];
        composite.projection0[1] = projection[5];
        composite.projection0[2] = projection[10];
        composite.projection0[3] = projection[12];
        composite.projection1[0] = projection[13];
        composite.projection1[1] = projection[14];
        composite.projection1[2] = 1.0f / kShadowMapSize;
        // SC4's shadow colour and strength (GetShadowParams), as DrawShadows
        // blends them, once a pass has reported them. Its fixed blue colour is
        // a daylight shadow tint: fade the custom composite with the lighting
        // manager's own day/night weight, even when no native overlay was drawn.
        float daylight = 1.0f;
        LiveShadowHooks::ShadowDaylight(daylight);
        composite.projection1[3] =
            (liveShadowToneValid ? liveShadowStrength : kFallbackShadowOpacity) * daylight;
        if (liveShadowToneValid) std::memcpy(composite.tone, liveShadowColour, sizeof(liveShadowColour));
        std::memcpy(composite.sun, shadowDirection, sizeof(shadowDirection));
        // N.L of flat ground: the sun's elevation. The camera estimate assumes
        // SC4's 45 degrees.
        float flatLight = 0.70710678f;
        if (gameDirection) {
            float sunWorld[3]{liveShadowSunWorld[0], liveShadowSunWorld[1], liveShadowSunWorld[2]};
            Normalize(sunWorld);
            flatLight = (std::max)(-sunWorld[1], 0.05f);
        }
        composite.sun[3] = 1.0f / flatLight;
        ID3D11RenderTargetView* target = renderTargetView.Get();
        d3dContext->OMSetRenderTargets(1, &target, nullptr);
        // The composite is a fullscreen triangle, so the viewport is what bounds
        // it. Outside a partial pass's dirty rectangle SC4 keeps the pixels it
        // already has, and darkening them again would re-blend the same shadow
        // over itself on every update. The pixel shader reads SV_Position and
        // maps it through this same rectangle, which is what the pass's
        // projection covers.
        D3D11_VIEWPORT viewport{0, 0, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0, 1};
        if (partialPass) {
            viewport.TopLeftX = static_cast<float>(viewportX);
            viewport.TopLeftY = static_cast<float>(D3D11TopLeftY(windowHeight, viewportY, viewportHeight));
            viewport.Width = static_cast<float>(viewportWidth);
            viewport.Height = static_cast<float>(viewportHeight);
        }
        composite.viewport[0] = viewport.TopLeftX;
        composite.viewport[1] = viewport.TopLeftY;
        composite.viewport[2] = (std::max)(viewport.Width, 1.0f);
        composite.viewport[3] = (std::max)(viewport.Height, 1.0f);
        d3dContext->UpdateSubresource(pipeline.constants.Get(), 0, nullptr, &composite, 0, 0);
        d3dContext->RSSetViewports(1, &viewport);
        d3dContext->RSSetState(pipeline.rasterizer.Get());
        d3dContext->OMSetDepthStencilState(pipeline.compositeDepth.Get(), 0);
        d3dContext->OMSetBlendState(pipeline.shadowBlend.Get(), nullptr, 0xffffffff);
        d3dContext->IASetInputLayout(nullptr);
        d3dContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dContext->VSSetShader(pipeline.compositeVS.Get(), nullptr, 0);
        d3dContext->PSSetShader(pipeline.compositePS.Get(), nullptr, 0);
        d3dContext->VSSetConstantBuffers(0, 1, &cb);
        d3dContext->PSSetConstantBuffers(0, 1, &cb);
        ID3D11ShaderResourceView* views[4]{
            depthShaderView.Get(), pipeline.mapView.Get(), terrainBound ? pipeline.terrainMapView.Get() : nullptr,
            terrainBound ? pipeline.terrainAltitudeView.Get() : nullptr
        };
        d3dContext->PSSetShaderResources(0, 4, views);
        ID3D11SamplerState* samplers[3]{
            pipeline.mapSampler.Get(), pipeline.mapSampler.Get(), pipeline.clampSampler.Get()
        };
        d3dContext->PSSetSamplers(0, 3, samplers);
        d3dContext->Draw(3, 0);
        gpuStamp(2);
        ++frameStats.shadowComposites;
        frameStats.shadowCompositePixels += static_cast<uint64_t>(viewport.Width * viewport.Height);
        views[0] = views[1] = views[2] = views[3] = nullptr;
        d3dContext->PSSetShaderResources(0, 4, views);
        liveShadowDraws.clear();
        d3dContext->ClearState();
        InvalidateD3D11StateCache();
        ID3D11RenderTargetView* restoreTarget = renderTargetView.Get();
        d3dContext->OMSetRenderTargets(1, &restoreTarget, depthStencilView.Get());
        if (scissorEnabled) SetViewport(viewportX, viewportY, viewportWidth, viewportHeight);
        else SetViewport();
    }
}
