/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// cGDriver (scd3d11/) on Direct3D 12. Every SimGL call keeps the state cGDriver keeps and draws the way
// it draws. Only how the GPU is fed differs: one command list per frame, geometry, constants and texture
// data through per-frame upload memory, pipeline state objects built on first use and kept.

#include "cGDriver12.h"

#include "cGDCombiner.h"
#include "D3D11Conversions.h"
#include "TextureUploadUtils.h"
#include "VertexFormatUtils.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <utility>

using Microsoft::WRL::ComPtr;
using namespace nSCD3D11;

// The SDK declares these destructors pure. SCD3D11 defines them in cGDriver.cpp, the server here.
cIGZGBufferRegionExtension::~cIGZGBufferRegionExtension() {}
cIGZGDriverVertexBufferExtension::~cIGZGDriverVertexBufferExtension() {}

namespace nSCD3D12 {
	namespace {
		constexpr uint32_t kFrameCount = 3;
		constexpr uint64_t kUploadChunkBytes = 32ull << 20;
		// Shader-visible texture views. Slot 0 is the empty view for stages without a texture.
		constexpr uint32_t kViewSlots = 1u << 18;
		// With relighting, the lighting pass's views of the back buffer, light layer, depth buffer, sunlight and
		// terrain field.
		constexpr uint32_t kLightingViews = 1;
		constexpr uint32_t kLightingViewCount = 5;
		// Timestamps per frame in the lighting pass: its start, after the light hooks' own work, after the
		// sunlight trace, its end.
		constexpr uint32_t kLightingTimestamps = 4;
		constexpr uint32_t kRegionCount = 8;
		constexpr uint32_t kExtensionCapacity = 32768;
		// Sampler heap: 32 filter and wrap combinations, then the blit sampler.
		constexpr uint32_t kBlitSampler = 32;
		constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
		constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R24G8_TYPELESS;
		constexpr uint32_t kPuntBlitSourceAlpha = 0x6C4236E7;
		// The view object that draws SC4's own shadows, in the static and the dynamic pass. They point at
		// SC4's sun, so relighting leaves them out and casts its own.
		constexpr uint32_t kShadowViewObjectVtable = 0x00AB39D0;

		// cGDriver_Geometry.cpp's shader, unchanged.
		char const kShaderSource[] = R"(
#ifndef SCD3D11_INTERPOLATION
#define SCD3D11_INTERPOLATION
#endif
struct StageState
{
	uint4 modes;
	uint4 parameters0;
	uint4 parameters1;
};

cbuffer DriverConstants : register(b0)
{
	column_major float4x4 modelView;
	column_major float4x4 projection;
	column_major float4x4 normalMatrix;
	uint flags;
	uint alphaFunction;
	float alphaReference;
	uint enabledLights;
	float4 globalAmbient;
	float4 lightAmbient[8];
	float4 lightDiffuse[8];
	float4 lightSpecular[8];
	float4 lightPosition[8];
	float4 materialAmbient;
	float4 materialDiffuse;
	float4 materialSpecular;
	float4 materialEmission;
	float4 materialParameters;
	column_major float4x4 textureMatrix0;
	column_major float4x4 textureMatrix1;
	StageState stage0;
	StageState stage1;
	float4 fogColor;
	float4 fogParameters;
	float4 textureFactor;
};

Texture2D texture0 : register(t0);
Texture2D texture1 : register(t1);
SamplerState sampler0 : register(s0);
SamplerState sampler1 : register(s1);

struct VSInput
{
	float3 position : POSITION;
	float3 normal : NORMAL;
	float4 color : COLOR0;
	float2 texCoord0 : TEXCOORD0;
	float2 texCoord1 : TEXCOORD1;
};

struct PSInput
{
	float4 position : SV_POSITION;
	SCD3D11_INTERPOLATION float3 normal : NORMAL;
	SCD3D11_INTERPOLATION float4 color : COLOR0;
	SCD3D11_INTERPOLATION float3 viewPosition : TEXCOORD3;
	float2 texCoord0 : TEXCOORD0;
	float2 texCoord1 : TEXCOORD1;
	float fogDistance : TEXCOORD2;
};

PSInput VSMain(VSInput input)
{
	PSInput output;
	float4 viewPosition = mul(modelView, float4(input.position, 1.0f));
	output.position = mul(projection, viewPosition);
	output.position.z = (output.position.z + output.position.w) * 0.5f;
	output.normal = mul((float3x3)normalMatrix, input.normal);
	output.color = input.color;
	output.viewPosition = viewPosition.xyz;
	float4 source0 = (stage0.parameters1.w & 0xfffffff8) == 0x10
		? viewPosition
		: float4(stage0.parameters1.w == 1 ? input.texCoord1 : input.texCoord0, 0.0f, 1.0f);
	float4 source1 = (stage1.parameters1.w & 0xfffffff8) == 0x10
		? viewPosition
		: float4(stage1.parameters1.w == 1 ? input.texCoord1 : input.texCoord0, 0.0f, 1.0f);
	output.texCoord0 = mul(textureMatrix0, source0).xy;
	output.texCoord1 = mul(textureMatrix1, source1).xy;
	output.fogDistance = abs(viewPosition.z);
	return output;
}

#ifdef SCD3D12_INSTANCED
cbuffer RetainedFrame : register(b1)
{
	column_major float4x4 frameProjection;
};

// A kept draw (see cGDriver12::DrawRetained): each instance brings its modelview's first three rows. The frame's
// projection places it on screen and the draw's own projection gives its depth, which SC4 nudges per layer.
PSInput VSInstanced(VSInput input, float4 row0 : INSTANCE0, float4 row1 : INSTANCE1, float4 row2 : INSTANCE2)
{
	PSInput output;
	float4 position = float4(input.position, 1.0f);
	float4 viewPosition = float4(dot(row0, position), dot(row1, position), dot(row2, position), 1.0f);
	output.position = mul(frameProjection, viewPosition);
	output.position.z = dot(projection[2], viewPosition);
	output.position.z = (output.position.z + output.position.w) * 0.5f;
	output.normal = float3(dot(row0.xyz, input.normal), dot(row1.xyz, input.normal), dot(row2.xyz, input.normal));
	output.color = input.color;
	output.viewPosition = viewPosition.xyz;
	float4 source0 = (stage0.parameters1.w & 0xfffffff8) == 0x10
		? viewPosition
		: float4(stage0.parameters1.w == 1 ? input.texCoord1 : input.texCoord0, 0.0f, 1.0f);
	float4 source1 = (stage1.parameters1.w & 0xfffffff8) == 0x10
		? viewPosition
		: float4(stage1.parameters1.w == 1 ? input.texCoord1 : input.texCoord0, 0.0f, 1.0f);
	output.texCoord0 = mul(textureMatrix0, source0).xy;
	output.texCoord1 = mul(textureMatrix1, source1).xy;
	output.fogDistance = abs(viewPosition.z);
	return output;
}
#endif

float4 CombinerArgument(uint packed, float4 textureColor, float4 previous, float4 constantColor, float4 primary)
{
	uint source = packed & 15;
	uint operand = packed >> 4;
	float4 value = source == 0 ? textureColor : (source == 1 ? previous : (source == 2 ? constantColor : primary));
	return operand == 0 ? value : (operand == 1 ? 1.0f - value :
		(operand == 2 ? value.aaaa : 1.0f - value.aaaa));
}

float3 CombineRGB(StageState state, float4 textureColor, float4 previous, float4 primary)
{
	float4 a = CombinerArgument(state.parameters0.y, textureColor, previous, textureFactor, primary);
	float4 b = CombinerArgument(state.parameters0.z, textureColor, previous, textureFactor, primary);
	float4 c = CombinerArgument(state.parameters0.w, textureColor, previous, textureFactor, primary);
	uint mode = state.modes.y;
	float3 result = mode == 0 ? a.rgb :
		(mode == 1 ? a.rgb * b.rgb :
		(mode == 2 ? a.rgb + b.rgb :
		(mode == 3 ? a.rgb + b.rgb - 0.5f :
		(mode == 4 ? a.rgb * c.rgb + b.rgb * (1.0f - c.rgb) :
		4.0f * dot(a.rgb - 0.5f, b.rgb - 0.5f)))));
	return saturate(result * (1u << state.modes.w));
}

float CombineAlpha(StageState state, float4 textureColor, float4 previous, float4 primary)
{
	float a = CombinerArgument(state.parameters1.x, textureColor, previous, textureFactor, primary).a;
	float b = CombinerArgument(state.parameters1.y, textureColor, previous, textureFactor, primary).a;
	float c = CombinerArgument(state.parameters1.z, textureColor, previous, textureFactor, primary).a;
	uint mode = state.modes.z;
	float result = mode == 0 ? a : (mode == 1 ? a * b :
		(mode == 2 ? a + b : (mode == 3 ? a + b - 0.5f : a * c + b * (1.0f - c))));
	return saturate(result * (1u << state.parameters0.x));
}

float4 ApplyStage(StageState state, float4 textureColor, float4 previous, float4 primary)
{
	uint mode = state.modes.x;
	float4 result = textureColor;
	if (mode == 1) result = previous * textureColor;
	else if (mode == 2) result = float4(lerp(previous.rgb, textureColor.rgb, textureColor.a), previous.a);
	else if (mode == 3) result = float4(lerp(previous.rgb, textureFactor.rgb, textureColor.rgb), previous.a * textureColor.a);
	else if (mode != 0) result = float4(CombineRGB(state, textureColor, previous, primary),
		CombineAlpha(state, textureColor, previous, primary));
	return result;
}

float4 Shade(PSInput input)
{
	float4 primary = input.color;
	if ((flags & 4) != 0)
	{
		// Direct3D 7 fixed-function lighting as SimGLDX7 set it up: specular disabled, a vertex without
		// a normal gets no diffuse term, and the lit color clamps before texturing.
		float4 ambientMaterial = (flags & 32) != 0 ? input.color : materialAmbient;
		float4 diffuseMaterial = (flags & 64) != 0 ? input.color : materialDiffuse;
		float3 normal = (flags & 128) != 0 ? normalize(input.normal) : 0.0f;
		float3 lit = materialEmission.rgb + globalAmbient.rgb * ambientMaterial.rgb;
		[loop] for (uint light = 0; light < 8; ++light)
		{
			if ((enabledLights & (1u << light)) == 0) continue;
			float3 vectorToLight = lightPosition[light].w == 0.0f
				? normalize(lightPosition[light].xyz)
				: normalize(lightPosition[light].xyz / lightPosition[light].w - input.viewPosition);
			lit += lightAmbient[light].rgb * ambientMaterial.rgb +
				lightDiffuse[light].rgb * diffuseMaterial.rgb * saturate(dot(normal, vectorToLight));
		}
		primary = float4(saturate(lit), diffuseMaterial.a);
	}
	float4 color = primary;
	if ((flags & 1) != 0) color = ApplyStage(stage0, texture0.Sample(sampler0, input.texCoord0), color, primary);
	if ((flags & 2) != 0) color = ApplyStage(stage1, texture1.Sample(sampler1, input.texCoord1), color, primary);
	if ((flags & 8) != 0)
	{
		bool alphaPass = alphaFunction == 0 ? false :
			alphaFunction == 1 ? color.a < alphaReference :
			alphaFunction == 2 ? color.a == alphaReference :
			alphaFunction == 3 ? color.a <= alphaReference :
			alphaFunction == 4 ? color.a > alphaReference :
			alphaFunction == 5 ? color.a != alphaReference :
			alphaFunction == 6 ? color.a >= alphaReference : true;
		clip(alphaPass ? 1.0f : -1.0f);
	}
	if ((flags & 16) != 0)
	{
		float distance = input.fogDistance;
		float mode = fogParameters.w;
		float factor = mode < 0.5f ? exp(-fogParameters.x * distance) :
			(mode < 1.5f ? exp(-fogParameters.x * fogParameters.x * distance * distance) :
			saturate((fogParameters.z - distance) / max(fogParameters.z - fogParameters.y, 0.000001f)));
		color.rgb = lerp(fogColor.rgb, color.rgb, saturate(factor));
	}
	return color;
}

#ifndef SCD3D12_GBUFFER
float4 PSMain(PSInput input) : SV_TARGET
{
	return Shade(input);
}
#else
struct GBufferOutput
{
	float4 color : SV_TARGET0;
	float4 light : SV_TARGET1;
};

// What SC4's own lighting multiplied into the colour: the colour multiplier and the lights' ambient.
// SC4 draws without normals, so there is no diffuse term to take out.
float3 LightFactor()
{
	if ((flags & 4) == 0) return 1.0f;
	float3 factor = globalAmbient.rgb;
	[loop] for (uint light = 0; light < 8; ++light)
	{
		if ((enabledLights & (1u << light)) != 0) factor += lightAmbient[light].rgb;
	}
	return saturate(factor);
}

// The light layer holds the factor premultiplied by the relight weight. An opaque draw replaces it, a
// blended one covers it by its alpha.
GBufferOutput PSMain(PSInput input)
{
	GBufferOutput output;
	output.color = Shade(input);
#if SCD3D12_GBUFFER == 1
	output.light = float4(LightFactor(), 1.0f);
#else
	float coverage = saturate(output.color.a);
	output.light = float4(LightFactor() * coverage, coverage);
#endif
	return output;
}
#endif
)";

		// cGDriver_Blit.cpp's shader, unchanged.
		char const kBlitShaderSource[] = R"(
struct VertexInput { float2 position : POSITION; float2 texCoord : TEXCOORD0; };
struct VertexOutput { float4 position : SV_POSITION; float2 texCoord : TEXCOORD0; };
VertexOutput VSMain(VertexInput input) {
	VertexOutput output;
	output.position = float4(input.position, 0.0f, 1.0f);
	output.texCoord = input.texCoord;
	return output;
}
Texture2D source : register(t0);
SamplerState sourceSampler : register(s0);
cbuffer BlitConstants : register(b0) {
	float4 modulate;
	float4 colorKey;
	float sourceAlpha;
};
float4 Blit(VertexOutput input) {
	float4 color = source.Sample(sourceSampler, input.texCoord);
	color.a = lerp(1.0f, color.a, sourceAlpha);
	if (colorKey.a > 0.5f && all(abs(color.rgb - colorKey.rgb) < 0.5f / 255.0f)) color.a = 0.0f;
	return color * modulate;
}
#ifndef SCD3D12_GBUFFER
float4 PSMain(VertexOutput input) : SV_TARGET {
	return Blit(input);
}
#else
// Blits are UI: their alpha is how far they cover the city's relight weight.
struct GBufferOutput { float4 color : SV_TARGET0; float4 light : SV_TARGET1; };
GBufferOutput PSMain(VertexOutput input) {
	GBufferOutput output;
	output.color = Blit(input);
	output.light = float4(0.0f, 0.0f, 0.0f, saturate(output.color.a));
	return output;
}
#endif
)";

		// The lighting pass: SC4's frame relit with another sun, from the colour, the light layer and the
		// depth buffer. SC4's camera is orthographic, so a pixel's view position is its window position
		// scaled, and the walk towards the sun is a straight line on screen with a constant climb.
		char const kLightingShaderSource[] = R"(
#define SHADOW_STEPS 24
cbuffer LightingConstants : register(b0)
{
	float4 camera;    // view units per pixel across and down, view z per unit of depth, 1 once known
	float4 sun;       // towards the sun in view space
	float4 sunColor;
	float4 ambient;
	float4 sc4Sun;    // towards SC4's own sun in view space
	float4 sc4Light;  // the ambient and direct light SC4's art is lit with, the largest relight ratio
	float4 march;     // longest shadow in view units and in pixels, bias, softness
	float4 occluder;  // thickness: deeper steps belong to something else in front, blur depth tolerance
	// The world, from the light hooks (cGDriver12::WorldFrame), when world.x is 1.
	row_major float4x4 pixelToWorld;
	row_major float4x4 worldToView;
	float4 world;     // known, metres per pixel
	float4 terrain;   // terrain field vertices per row (0 without), vertices per metre, height tolerance
	float4 lights;    // night lights: count, tile size in pixels, tiles across, tiles
};
Texture2D<float4> scene : register(t0);
Texture2D<float4> lightLayer : register(t1);
Texture2D<float> sceneDepth : register(t2);
Texture2D<float> sunlight : register(t3);
StructuredBuffer<float4> terrainField : register(t4);
StructuredBuffer<float4> nightLights : register(t5);
StructuredBuffer<uint> lightTiles : register(t6);

float4 VSMain(uint id : SV_VertexID) : SV_POSITION
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float3 ViewPosition(int2 pixel)
{
	return float3(pixel.x * camera.x, -pixel.y * camera.y, sceneDepth.Load(int3(pixel, 0)) * camera.z);
}

// Of the two neighbours along one axis, the step to the one on the same surface: the smaller depth step.
float3 SurfaceStep(int2 pixel, float3 centre, int2 axis)
{
	float3 forward = ViewPosition(pixel + axis) - centre;
	float3 backward = centre - ViewPosition(pixel - axis);
	return abs(forward.z) < abs(backward.z) ? forward : backward;
}

float3 Normal(float3 across, float3 down)
{
	float3 normal = normalize(cross(across, down));
	return normal.z < 0.0f ? -normal : normal;
}

// Pass 1: how much of the sun each city pixel sees, from a walk towards it over the depth buffer. The
// steps grow with distance so contact shadows stay sharp, and each pixel of a 4x4 tile starts the walk
// at another offset, which the blur in pass 2 averages out.
float TracePS(float4 position : SV_POSITION) : SV_TARGET
{
	int2 pixel = int2(position.xy);
	if (lightLayer.Load(int3(pixel, 0)).a <= 0.0f || camera.w == 0.0f) return 1.0f;
	float3 centre = ViewPosition(pixel);
	float3 normal = Normal(SurfaceStep(pixel, centre, int2(1, 0)), SurfaceStep(pixel, centre, int2(0, 1)));
	if (dot(normal, sun.xyz) <= 0.0f) return 1.0f;
	float2 pixelsPerUnit = float2(sun.x / camera.x, -sun.y / camera.y);
	float rate = length(pixelsPerUnit);
	if (rate < 1.0e-4f) return 1.0f;
	float reach = min(march.x, march.y / rate);
	uint2 size;
	sceneDepth.GetDimensions(size.x, size.y);
	static const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
	float offset = (bayer[(pixel.y & 3) * 4 + (pixel.x & 3)] + 0.5f) / 16.0f;
	float shade = 0.0f;
	[loop] for (uint i = 0; i < SHADOW_STEPS; ++i)
	{
		float along = (i + offset) / SHADOW_STEPS;
		float distance = reach * along * along;
		float2 at = float2(pixel) + 0.5f + pixelsPerUnit * distance;
		if (any(at < 0.0f) || any(at >= float2(size))) break;
		float height = sceneDepth.Load(int3(int2(at), 0)) * camera.z - (centre.z + sun.z * distance);
		if (height < occluder.x) shade = max(shade, saturate((height - march.z) / march.w));
	}
	return 1.0f - shade;
}

// The terrain field between its vertices: world normal and height.
float4 TerrainAt(float2 xz)
{
	float2 grid = clamp(xz * terrain.y, 0.0f, terrain.x - 1.001f);
	uint2 cell = uint2(grid);
	float2 f = grid - float2(cell);
	uint row = uint(terrain.x);
	uint first = cell.y * row + cell.x;
	return lerp(lerp(terrainField[first], terrainField[first + 1], f.x),
	            lerp(terrainField[first + row], terrainField[first + row + 1], f.x), f.y);
}

float3 WorldPosition(float2 position, int2 pixel)
{
	float4 at = mul(pixelToWorld, float4(position, sceneDepth.Load(int3(pixel, 0)), 1.0f));
	return at.xyz / at.w;
}

// Light from the night lights in the pixel's tile, which fades to nothing at each light's radius.
float3 NightLight(int2 pixel, float3 at, float3 normal)
{
	uint tile = (uint(pixel.y) / uint(lights.y)) * uint(lights.z) + uint(pixel.x) / uint(lights.y);
	// Root views are not bounds checked: the window can be larger than the camera the tiles were made for.
	if (uint(pixel.x) / uint(lights.y) >= uint(lights.z) || tile >= uint(lights.w)) return 0.0f;
	uint first = lightTiles[tile * 2];
	uint count = min(lightTiles[tile * 2 + 1], 128u);
	float3 sum = 0.0f;
	[loop] for (uint i = 0; i < count; ++i)
	{
		uint index = min(lightTiles[first + i], uint(lights.x) - 1);
		float4 place = nightLights[index * 2];
		float3 toLight = place.xyz - at;
		float distance2 = dot(toLight, toLight);
		float fade = saturate(1.0f - distance2 / (place.w * place.w));
		// Facing the light, softened so a surface edge-on still catches some.
		float facing = saturate(dot(normal, toLight) * rsqrt(distance2 + 1.0e-4f) * 0.7f + 0.3f);
		sum += nightLights[index * 2 + 1].rgb * (fade * fade * facing);
	}
	return sum;
}

// Pass 2: the relit frame. The sunlight is averaged over the 4x4 tile around the pixel, from the
// neighbours that lie on the pixel's own surface.
float4 PSMain(float4 position : SV_POSITION) : SV_TARGET
{
	int2 pixel = int2(position.xy);
	float4 color = scene.Load(int3(pixel, 0));
	float4 layer = lightLayer.Load(int3(pixel, 0));
	if (layer.a <= 0.0f || camera.w == 0.0f) return color;
	float3 centre = ViewPosition(pixel);
	float3 across = SurfaceStep(pixel, centre, int2(1, 0));
	float3 down = SurfaceStep(pixel, centre, int2(0, 1));
	float3 normal = Normal(across, down);
	float3 at = 0.0f, worldNormal = 0.0f;
	if (world.x > 0.0f)
	{
		at = WorldPosition(position.xy, pixel);
		worldNormal = mul(normal, (float3x3)worldToView);
		// On the terrain (close to its height, and facing about the same way), the terrain's own smooth
		// normal stands in for the flat one from depth.
		if (terrain.x > 0.0f)
		{
			float4 field = TerrainAt(at.xz);
			float3 smooth = normalize(field.xyz + float3(0.0f, 1.0e-6f, 0.0f));
			if (abs(at.y - field.w) < terrain.z && dot(worldNormal, smooth) > 0.8f)
			{
				worldNormal = smooth;
				normal = mul((float3x3)worldToView, smooth);
			}
		}
	}
	float facing = saturate(dot(normal, sun.xyz));
	float visibility = 1.0f;
	if (facing > 0.0f)
	{
		float total = 0.0f, count = 0.0f;
		[unroll] for (int y = -2; y < 2; ++y)
		{
			[unroll] for (int x = -2; x < 2; ++x)
			{
				int2 at = pixel + int2(x, y);
				float expected = centre.z + across.z * x + down.z * y;
				float actual = sceneDepth.Load(int3(at, 0)) * camera.z;
				if (abs(actual - expected) < occluder.y)
				{
					total += sunlight.Load(int3(at, 0));
					count += 1.0f;
				}
			}
		}
		visibility = count > 0.0f ? total / count : sunlight.Load(int3(pixel, 0));
	}
	// SC4's art carries its own sun and its light factor. Both come out, the new light goes in.
	float baked = sc4Light.x + sc4Light.y * saturate(dot(normal, sc4Sun.xyz));
	float3 factor = max(layer.rgb / layer.a, 1.0f / 255.0f);
	float3 light = ambient.rgb + sunColor.rgb * facing * visibility;
	if (lights.x > 0.0f) light += NightLight(pixel, at, worldNormal);
	float3 ratio = min(light / (baked * factor), sc4Light.z);
	return float4(saturate(color.rgb * lerp(1.0f, ratio, saturate(layer.a))), color.a);
}
)";

		struct DriverConstants {
			float modelView[16];
			float projection[16];
			float normalMatrix[16];
			uint32_t flags;
			uint32_t alphaFunction;
			float alphaReference;
			uint32_t enabledLights;
			float globalAmbient[4];
			float lightAmbient[8][4];
			float lightDiffuse[8][4];
			float lightSpecular[8][4];
			float lightPosition[8][4];
			float materialAmbient[4];
			float materialDiffuse[4];
			float materialSpecular[4];
			float materialEmission[4];
			float materialParameters[4];
			float textureMatrices[2][16];

			struct StageConstants {
				uint32_t modes[4];
				uint32_t parameters0[4];
				uint32_t parameters1[4];
			} stages[2];

			float fogColor[4];
			float fogParameters[4];
			float textureFactor[4];
		};

		struct BlitVertex {
			float position[2];
			float texCoord[2];
		};

		struct BlitConstants {
			float modulate[4];
			float colorKey[4];
			float sourceAlpha;
			float padding[7];
		};

		struct LightingConstants {
			float camera[4];
			float sun[4];
			float sunColor[4];
			float ambient[4];
			float sc4Sun[4];
			float sc4Light[4];
			float march[4];
			float occluder[4];
			float pixelToWorld[16];
			float worldToView[16];
			float world[4];
			float terrain[4];
			float lights[4];
		};
		// Off the terrain by more than this (metres) a pixel keeps its normal from depth.
		constexpr float kTerrainTolerance = 2.0f;

		// The light SC4's art is drawn with: its sun stands at the screen's left, 45 degrees up, and turns
		// with the camera. The ambient and direct shares light flat ground fully.
		constexpr float kSC4Ambient = 0.55f;
		constexpr float kSC4Direct = 0.64f;
		// Relighting may brighten a pixel at most this much, so a face SC4 drew nearly black stays dark.
		constexpr float kLargestRelight = 4.0f;
		// Shadows reach at most this far, in world units (metres) and in pixels.
		constexpr float kShadowReach = 400.0f;
		constexpr float kShadowReachPixels = 400.0f;
		// A step this much below the surface ahead starts the shadow, which deepens over the softness.
		// Deeper than the thickness, the surface ahead belongs to something else in front.
		constexpr float kShadowBias = 0.3f;
		constexpr float kShadowSoftness = 2.0f;
		constexpr float kShadowThickness = 80.0f;
		// The sunlight blur takes a neighbour within this much of the pixel's own surface.
		constexpr float kShadowBlurTolerance = 1.0f;

		void MakeNormalMatrix(float const *m, float *out) {
			float const a00=m[0], a01=m[4], a02=m[8], a10=m[1], a11=m[5], a12=m[9];
			float const a20=m[2], a21=m[6], a22=m[10];
			float const c00=a11*a22-a12*a21, c01=a12*a20-a10*a22, c02=a10*a21-a11*a20;
			float const c10=a02*a21-a01*a22, c11=a00*a22-a02*a20, c12=a01*a20-a00*a21;
			float const c20=a01*a12-a02*a11, c21=a02*a10-a00*a12, c22=a00*a11-a01*a10;
			float const determinant = a00*c00 + a01*c01 + a02*c02;
			memset(out, 0, sizeof(float) * 16);
			if (std::fabs(determinant) < 1.0e-12f) {
				out[0]=out[5]=out[10]=out[15]=1.0f;
				return;
			}
			float const scale=1.0f/determinant;
			out[0]=c00*scale; out[4]=c01*scale; out[8]=c02*scale;
			out[1]=c10*scale; out[5]=c11*scale; out[9]=c12*scale;
			out[2]=c20*scale; out[6]=c21*scale; out[10]=c22*scale; out[15]=1.0f;
		}

		uint16_t Pack16BitPixel(DXGI_FORMAT format, uint8_t const rgba[4]) {
			switch (format) {
				case DXGI_FORMAT_B5G6R5_UNORM:
					return static_cast<uint16_t>(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) | (rgba[2] >> 3));
				case DXGI_FORMAT_B5G5R5A1_UNORM:
					return static_cast<uint16_t>(((rgba[3] >= 128) ? 0x8000 : 0) |
					                             ((rgba[0] >> 3) << 10) | ((rgba[1] >> 3) << 5) | (rgba[2] >> 3));
				case DXGI_FORMAT_B4G4R4A4_UNORM:
					return static_cast<uint16_t>(((rgba[3] >> 4) << 12) |
					                             ((rgba[0] >> 4) << 8) | ((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
				default:
					return 0;
			}
		}

		bool IsCompressed(DXGI_FORMAT format) {
			return format == DXGI_FORMAT_BC1_UNORM || format == DXGI_FORMAT_BC2_UNORM || format == DXGI_FORMAT_BC3_UNORM;
		}

		void UnpackColor(uint32_t value, float rgba[4]) {
			// 0xAARRGGBB. Bare byte values only carry alpha, so 0xFFFFFFFF and 0xFF are both "no change".
			if (value <= 0xff) {
				rgba[0] = rgba[1] = rgba[2] = 1.0f;
				rgba[3] = static_cast<float>(value) / 255.0f;
				return;
			}
			rgba[0] = static_cast<float>((value >> 16) & 0xff) / 255.0f;
			rgba[1] = static_cast<float>((value >> 8) & 0xff) / 255.0f;
			rgba[2] = static_cast<float>(value & 0xff) / 255.0f;
			rgba[3] = static_cast<float>(value >> 24) / 255.0f;
		}

		constexpr uint64_t AlignUp(uint64_t value, uint64_t alignment) {
			return (value + alignment - 1) & ~(alignment - 1);
		}

		D3D12_RESOURCE_DESC TextureDescription(DXGI_FORMAT format, uint32_t width, uint32_t height, uint32_t levels,
		                                       D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			description.Width = width;
			description.Height = height;
			description.DepthOrArraySize = 1;
			description.MipLevels = static_cast<UINT16>(levels);
			description.Format = format;
			description.SampleDesc.Count = 1;
			description.Flags = flags;
			return description;
		}

		// Everything that goes into a pipeline state object. Kinds 0 and 1 are the geometry pipeline with
		// smooth and flat interpolation, 2 and 3 the opaque and blending blit.
		struct PipelineKey {
			uint64_t bits = 0;
			int32_t depthBias = 0;
			bool operator==(PipelineKey const &other) const {
				return bits == other.bits && depthBias == other.depthBias;
			}
		};

		struct PipelineKeyHash {
			size_t operator()(PipelineKey const &key) const {
				return static_cast<size_t>(key.bits * 0x9E3779B97F4A7C15ull ^ static_cast<uint32_t>(key.depthBias));
			}
		};

		// Converted geometry kept across frames, in upload memory the GPU reads directly, like SCD3D11's
		// dynamic cache buffers. Filled in order, a segment at a time, the oldest segment reused first.
		constexpr uint32_t kCacheSegments = 8;
		constexpr uint64_t kCacheSegmentBytes = 32ull << 20;

		enum class CacheKind : uint32_t { Vertices, Indices, ConvertedIndices, SequentialIndices };

		// What a cached buffer was made from: a blob (or none, for sequential indices) and how it was read.
		struct CacheKey {
			uint64_t low = 0;
			uint64_t high = 0;
			uint32_t size = 0;
			CacheKind kind = CacheKind::Vertices;
			uint32_t format = 0; // vertex format, index type or primitive
			uint32_t stride = 0;
			uint32_t first = 0;
			uint32_t count = 0;

			bool operator==(CacheKey const &other) const {
				return low == other.low && high == other.high && size == other.size && kind == other.kind &&
				       format == other.format && stride == other.stride && first == other.first &&
				       count == other.count;
			}
		};

		struct CacheKeyHash {
			size_t operator()(CacheKey const &key) const {
				uint64_t const mixed = key.low ^ key.high * 0x9E3779B97F4A7C15ull ^
				                       (static_cast<uint64_t>(key.format) << 32 | key.count) * 0xC2B2AE3D27D4EB4Full ^
				                       (static_cast<uint64_t>(key.kind) << 32 | key.first);
				return static_cast<size_t>(mixed ^ mixed >> 29);
			}
		};

		struct CacheEntry {
			uint32_t segment = 0;
			uint32_t offset = 0;
			uint32_t bytes = 0;
			// Indices only: how many to draw, and the range of vertices they use.
			uint32_t indexCount = 0;
			uint32_t minimumIndex = 0;
			uint32_t maximumIndex = 0;
		};
	}

	struct cGDriver12::Impl {
		enum class BlitAlpha { Opaque, Source, Constant, SourceModulated };

		struct Texture {
			ComPtr<ID3D12Resource> resource;
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			uint32_t width = 0;
			uint32_t height = 0;
			uint32_t levels = 0;
			uint32_t view = 0;
		};

		struct TextureStage {
			uint8_t magFilter = 1;
			uint8_t minFilter = 6;
			uint8_t wrapU = 3;
			uint8_t wrapV = 3;
			uint8_t environmentMode = 1;
			uint8_t rgbMode = 1;
			uint8_t alphaMode = 1;
			uint8_t rgbScale = 0;
			uint8_t alphaScale = 0;
			uint8_t rgbParameters[3]{0x00, 0x01, 0x02};
			uint8_t alphaParameters[3]{0x00, 0x01, 0x02};
			uint32_t coordinateSource = 0;
			float matrix[16]{};
		};

		struct UploadChunk {
			ComPtr<ID3D12Resource> buffer;
			uint8_t *cpu = nullptr;
			D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
			uint64_t size = 0;
		};

		struct Allocation {
			uint8_t *cpu = nullptr;
			D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
			ID3D12Resource *resource = nullptr;
			uint64_t offset = 0;
		};

		// What one frame in flight holds on to until the GPU is past it.
		struct FrameSlot {
			ComPtr<ID3D12CommandAllocator> allocator;
			uint64_t fence = 0;
			std::vector<UploadChunk> chunks;
			std::vector<ComPtr<ID3D12Resource> > garbage;
			std::vector<uint32_t> freedViews;
			// The lighting pass ran in this frame and its timestamps are waiting to be read.
			bool timed = false;
			// The frame drew some of the city, so the lighting pass had city pixels to light.
			bool cityFrame = false;
		};

		struct Region {
			ComPtr<ID3D12Resource> texture;
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
			int32_t type = 0;
			// With relighting, a colour region also keeps the light layer, so the two move together.
			ComPtr<ID3D12Resource> light;
			D3D12_RESOURCE_STATES lightState = D3D12_RESOURCE_STATE_COPY_DEST;
		};

		// Where the camera is, as the lighting pass needs it. Taken from the city's own draws.
		struct Camera {
			bool known = false;
			float up[3]{};
			float pixel[2]{};
			float depth = 0.0f;
		};

		struct CacheSegment {
			ComPtr<ID3D12Resource> buffer;
			uint8_t *cpu = nullptr;
			D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
			uint64_t cursor = 0;
			// The fence value of the last frame that read from it.
			uint64_t lastUse = 0;
			std::vector<CacheKey> keys;
		};

		// The blobs the next draw reads, from DrawBlobs. Taken by the draw, so a draw without them
		// converts and uploads its geometry every time.
		nSCDWire::BlobKey drawVertices;
		nSCDWire::BlobKey drawIndices;
		CacheSegment cacheSegments[kCacheSegments];
		uint32_t activeCacheSegment = 0;
		std::unordered_map<CacheKey, CacheEntry, CacheKeyHash> cache;
		// Vertices are cached the second time they are drawn. Animated geometry (the loading screen, some
		// effects) is new every frame and would only churn the cache. A slot remembers a recent blob.
		std::vector<uint64_t> seenVertices = std::vector<uint64_t>(1u << 16);

		ComPtr<IDXGIFactory6> factory;
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		uint64_t fenceValue = 0;
		// GPU time of each frame: a timestamp as its command list opens and one as it closes, read back
		// once the frame slot comes round again.
		ComPtr<ID3D12QueryHeap> timestamps;
		ComPtr<ID3D12Resource> timestampReadback;
		uint64_t *timestampValues = nullptr;
		double timestampMilliseconds = 0.0;
		bool frameTimed[kFrameCount]{};
		double gpuMilliseconds = 0.0;
		uint32_t gpuFrames = 0;
		FrameSlot frames[kFrameCount];
		uint32_t frameIndex = 0;
		std::vector<UploadChunk> chunkPool;
		uint64_t uploadCursor = 0;

		ComPtr<ID3D12RootSignature> rootSignature;
		ComPtr<ID3DBlob> vertexShader, pixelShader, flatVertexShader, flatPixelShader;
		// The owned scene's: VSInstanced, smooth and flat.
		ComPtr<ID3DBlob> instancedVertexShaders[2];
		ComPtr<ID3DBlob> blitVertexShader, blitPixelShader;
		std::unordered_map<PipelineKey, ComPtr<ID3D12PipelineState>, PipelineKeyHash> pipelines;

		ComPtr<ID3D12DescriptorHeap> rtvHeap;
		ComPtr<ID3D12DescriptorHeap> dsvHeap;
		ComPtr<ID3D12DescriptorHeap> viewHeap;
		ComPtr<ID3D12DescriptorHeap> samplerHeap;
		uint32_t viewIncrement = 0;
		uint32_t rtvIncrement = 0;
		uint32_t samplerIncrement = 0;
		std::vector<uint32_t> freeViews;
		uint32_t nextView = 1;

		ComPtr<ID3D12Resource> backBuffer;
		ComPtr<ID3D12Resource> depthBuffer;
		// Plain copy of the depth buffer for buffer region copies, which may not touch part of a depth
		// buffer. Valid until a draw or clear writes depth or stencil.
		ComPtr<ID3D12Resource> depthScratch;
		D3D12_RESOURCE_STATES depthScratchState = D3D12_RESOURCE_STATE_COPY_DEST;
		bool depthScratchValid = false;
		int32_t windowWidth = 0;
		int32_t windowHeight = 0;
		Region regions[kRegionCount];
		uint8_t regionFlags = 0;
		// Set while Resize rebuilds the targets, so textures and buffer regions survive.
		bool resizing = false;

		// Relighting. The light layer is drawn alongside the back buffer: SC4's light factor in rgb and
		// the relight weight in alpha (1 for the city, 0 for UI), premultiplied by that weight. The
		// lighting pass writes the published frame into litBuffer, so the back buffer stays SC4's.
		bool relight = false;
		cGDriver12::Sun sun;
		Camera camera;
		// True once the stream has bracketed a draw with its owner. Streams without owners count every
		// depth-tested draw as the city.
		bool ownersSeen = false;
		uint32_t ownerDepth = 0;
		uint8_t ownerPass = 0;
		// SwapCamera: the projection it replaces, view x inverse(from), and the camera's projection.
		bool swapCamera = false;
		float swapFrom[16]{}, swapChange[16]{}, swapProjection[16]{};
		// The innermost owner is SC4's shadow view object.
		bool ownerDrawsShadows = false;
		ComPtr<ID3D12Resource> lightBuffer;
		ComPtr<ID3D12Resource> litBuffer;
		// How much sun each pixel sees, traced before the lighting pass blurs and uses it.
		ComPtr<ID3D12Resource> sunlightBuffer;
		ComPtr<ID3D12RootSignature> lightingRootSignature;
		ComPtr<ID3D12PipelineState> lightingPipeline, tracePipeline;
		ComPtr<ID3DBlob> gbufferPixelShaders[2][2], blitGBufferPixelShader;
		ComPtr<ID3D12QueryHeap> lightingTimestamps;
		ComPtr<ID3D12Resource> lightingTimestampReadback;
		uint64_t timestampFrequency = 0;
		std::vector<float> lightingMilliseconds;
		// The parts of it: the light hooks' work and the sunlight trace.
		std::vector<float> worldMilliseconds, traceMilliseconds;
		bool cityDrawn = false;
		cGDriver12::LightHooks *lightHooks = nullptr;
		bool lightHooked = false;
		ComPtr<ID3D12Resource> terrainField;

		// The command list's bindings, so unchanged state is not set again. Forgotten with each new list.
		ID3D12PipelineState *appliedPipeline = nullptr;
		D3D12_GPU_VIRTUAL_ADDRESS appliedConstants = 0;
		uint32_t appliedViews[2]{};
		uint32_t appliedSamplers[2]{};
		D3D_PRIMITIVE_TOPOLOGY appliedTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
		PipelineKey appliedPipelineKey{};

		// SimGL state, as cGDriver keeps it.
		uint32_t interleavedFormat = UINT_MAX;
		uint32_t interleavedStride = 0;
		uint8_t const *interleavedPointer = nullptr;
		uint8_t activeMatrixMode = 0;
		float matrices[2][16]{};
		float normalMatrix[16]{};
		bool normalMatrixDirty = true;
		bool constantsDirty = true;
		uint32_t constantsFlagInputs = 0;
		DriverConstants constantsCache{};
		D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = 0;
		std::vector<D3D11Vertex> vertexScratch;
		std::vector<uint32_t> sourceIndexScratch;
		std::vector<uint32_t> drawIndexScratch;
		std::vector<uint8_t> extensionVertices;
		bool extensionVerticesValid = false;
		std::unordered_map<uint32_t, Texture> textures;
		uint32_t nextTextureId = 1;
		uint32_t boundTextures[2]{};
		uint8_t activeTextureStage = 0;
		bool textureStageEnabled[2]{true, false};
		TextureStage textureStages[2];
		uint32_t pixelStoreRowLength = 0;
		Texture blitTexture;
		std::vector<uint8_t> blitScratch;
		bool blitUsesSourceAlpha = false;
		bool enabledCapabilities[kGDNumCapabilities]{};
		bool colorWriteEnabled = true;
		uint8_t depthFunction = 1;
		bool depthWriteEnabled = true;
		uint8_t stencilFunction = 7;
		int32_t stencilReference = 0;
		int32_t appliedStencilReference = INT32_MIN;
		uint8_t stencilReadMask = 0xff;
		uint8_t stencilWriteMask = 0xff;
		uint8_t stencilFailOperation = 1;
		uint8_t stencilDepthFailOperation = 1;
		uint8_t stencilPassOperation = 1;
		uint8_t sourceBlend = 1;
		uint8_t destinationBlend = 0;
		uint8_t alphaFunction = 7;
		float alphaReference = 0.0f;
		uint8_t shadeModel = 1;
		float textureFactor[4]{1.0f, 1.0f, 1.0f, 1.0f};
		uint8_t fogMode = 0;
		float fogColor[4]{};
		float fogDensity = 1.0f;
		float fogStart = 0.0f;
		float fogEnd = 1.0f;
		bool ambientVertexColors = true;
		bool diffuseVertexColors = true;
		bool diffuseFromVertex = true;
		int32_t polygonOffset = 0;
		bool scissorEnabled = false;
		int32_t viewportX = 0, viewportY = 0, viewportWidth = 0, viewportHeight = 0;
		D3D12_VIEWPORT viewport{};
		D3D12_RECT scissor{};
		bool lightingEnabled = false;
		bool lightsEnabled[8]{};
		float globalAmbient[4]{0.0f, 0.0f, 0.0f, 1.0f};
		float lightAmbient[8][4]{};
		float lightDiffuse[8][4]{};
		float lightSpecular[8][4]{};
		float lightPosition[8][4]{};
		float materialAmbient[4]{0.0f, 0.0f, 0.0f, 1.0f};
		float materialDiffuse[4]{1.0f, 1.0f, 1.0f, 1.0f};
		float materialSpecular[4]{0.0f, 0.0f, 0.0f, 1.0f};
		float materialEmission[4]{0.0f, 0.0f, 0.0f, 1.0f};
		float materialShininess = 0.0f;
		float clearColor[4]{};
		float clearDepth = 1.0f;
		uint8_t clearStencil = 0;
		DrawFilter *drawFilter = nullptr;

		// The owned scene's capture, and the triangle lists it is handed.
		DrawCapture *capture = nullptr;
		bool skipRegionCopies = false;
		std::vector<D3D11Vertex> captureVertices;
		std::vector<uint32_t> captureSource, captureIndices;

		// Textures the owned scene keeps: a reference to the image and a view of its own, so the image stays
		// usable after the game deletes the name or gives it a new image. Pin n is pins[n - 1].
		struct Pin {
			ComPtr<ID3D12Resource> resource;
			uint32_t view = 0;
			uint32_t references = 0;
			// The name that still holds the image, and its state is the texture's. Once the game lets go of it
			// the pin keeps it readable on its own.
			uint32_t name = 0;
			bool orphan = false;
		};
		std::vector<Pin> pins;
		std::vector<uint32_t> freePins;
		std::unordered_map<ID3D12Resource *, uint32_t> pinOf;

		Impl() {
			for (float *matrix: matrices) matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
			lightAmbient[0][3] = 1.0f;
			lightDiffuse[0][0] = lightDiffuse[0][1] = lightDiffuse[0][2] = lightDiffuse[0][3] = 1.0f;
			lightSpecular[0][0] = lightSpecular[0][1] = lightSpecular[0][2] = lightSpecular[0][3] = 1.0f;
			lightPosition[0][0] = lightPosition[0][1] = 1.0f;
			for (TextureStage &stage: textureStages) {
				stage.matrix[0] = stage.matrix[5] = stage.matrix[10] = stage.matrix[15] = 1.0f;
			}
		}

		~Impl() {
			if (queue && fence) {
				WaitIdle();
				for (uint32_t index = 0; index < kFrameCount; ++index) ReadTimestamps(index);
			}
			if (fenceEvent != nullptr) CloseHandle(fenceEvent);
			auto const report = [](char const *name, std::vector<float> sorted) {
				if (sorted.empty()) return;
				std::sort(sorted.begin(), sorted.end());
				double total = 0.0;
				for (float value: sorted) total += value;
				std::printf("%s, frames that drew the city: %zu frames, %.3f ms on average, median %.3f ms, "
				            "95th percentile %.3f ms, slowest %.3f ms\n",
				            name, sorted.size(), total / static_cast<double>(sorted.size()), sorted[sorted.size() / 2],
				            sorted[sorted.size() * 95 / 100], sorted.back());
			};
			report("lighting pass", lightingMilliseconds);
			if (lightHooked) {
				report("  of which the world (structures, lights)", worldMilliseconds);
				report("  of which the sunlight trace", traceMilliseconds);
			}
		}

		// The lighting pass's GPU time in frame slot index, once the GPU is past it.
		void ReadTimestamps(uint32_t index) {
			FrameSlot &slot = frames[index];
			if (!slot.timed) return;
			slot.timed = false;
			size_t const bytes = kLightingTimestamps * sizeof(uint64_t);
			D3D12_RANGE const range{index * bytes, index * bytes + bytes};
			uint64_t *ticks = nullptr;
			if (FAILED(lightingTimestampReadback->Map(0, &range, reinterpret_cast<void **>(&ticks)))) return;
			uint64_t times[kLightingTimestamps];
			memcpy(times, ticks + index * kLightingTimestamps, sizeof(times));
			D3D12_RANGE const none{0, 0};
			lightingTimestampReadback->Unmap(0, &none);
			if (times[3] <= times[0] || timestampFrequency == 0 || !slot.cityFrame) return;
			auto const milliseconds = [&](uint32_t from, uint32_t to) {
				return static_cast<float>(static_cast<double>(times[to] - times[from]) * 1000.0 /
				                          static_cast<double>(timestampFrequency));
			};
			lightingMilliseconds.push_back(milliseconds(0, 3));
			worldMilliseconds.push_back(milliseconds(0, 1));
			traceMilliseconds.push_back(milliseconds(1, 2));
		}

		// Frames and submission

		void WaitFor(uint64_t value) {
			if (fence->GetCompletedValue() >= value) return;
			fence->SetEventOnCompletion(value, fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}

		void WaitIdle() { WaitFor(fenceValue); }

		void ReportDebugMessages() {
#ifndef NDEBUG
			ComPtr<ID3D12InfoQueue> info;
			if (FAILED(device.As(&info))) return;
			for (UINT64 i = 0, count = info->GetNumStoredMessages(); i < count; ++i) {
				SIZE_T size = 0;
				info->GetMessage(i, nullptr, &size);
				std::vector<uint8_t> storage(size);
				auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
				if (SUCCEEDED(info->GetMessage(i, message, &size))) {
					std::fprintf(stderr, "D3D12: %.*s\n", static_cast<int>(message->DescriptionByteLength),
					             message->pDescription);
				}
			}
			info->ClearStoredMessages();
#endif
		}

		// Waits for the slot's last use, frees what it held and opens the command list on it.
		void BeginFrame(uint32_t index) {
			frameIndex = index;
			FrameSlot &slot = frames[index];
			WaitFor(slot.fence);
			ReadTimestamps(index);
			if (frameTimed[index] && timestampValues != nullptr) {
				gpuMilliseconds += static_cast<double>(timestampValues[index * 2 + 1] - timestampValues[index * 2]) *
				                   timestampMilliseconds;
				++gpuFrames;
				frameTimed[index] = false;
			}
			for (UploadChunk &chunk: slot.chunks) {
				if (chunk.size == kUploadChunkBytes) chunkPool.push_back(std::move(chunk));
			}
			slot.chunks.clear();
			slot.garbage.clear();
			freeViews.insert(freeViews.end(), slot.freedViews.begin(), slot.freedViews.end());
			slot.freedViews.clear();
			uploadCursor = 0;
			slot.allocator->Reset();
			list->Reset(slot.allocator.Get(), nullptr);
			if (timestamps) list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2);
			BindFrame();
			// Constants live in upload memory the next frames recycle.
			constantsAddress = 0;
		}

		// The heaps, root signature, targets, viewport and scissor every draw expects, with the record of what
		// is bound forgotten.
		void BindFrame() {
			ID3D12DescriptorHeap *const heaps[]{viewHeap.Get(), samplerHeap.Get()};
			list->SetDescriptorHeaps(2, heaps);
			list->SetGraphicsRootSignature(rootSignature.Get());
			appliedPipeline = nullptr;
			appliedConstants = 0;
			appliedViews[0] = appliedViews[1] = UINT32_MAX;
			appliedSamplers[0] = appliedSamplers[1] = UINT32_MAX;
			appliedTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
			appliedStencilReference = INT32_MIN;
			if (backBuffer) {
				BindTargets();
				list->RSSetViewports(1, &viewport);
				list->RSSetScissorRects(1, &scissor);
			}
		}

		// The back buffer, with the light layer next to it when relighting, and the depth buffer.
		void BindTargets() {
			D3D12_CPU_DESCRIPTOR_HANDLE const target = RenderTargetView();
			D3D12_CPU_DESCRIPTOR_HANDLE const depth = DepthStencilView();
			list->OMSetRenderTargets(relight ? 2 : 1, &target, TRUE, &depth);
		}

		// Hands the recorded commands to the queue and moves on to the next frame slot.
		bool Submit() {
			if (timestamps) {
				list->EndQuery(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2 + 1);
				list->ResolveQueryData(timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frameIndex * 2, 2,
				                       timestampReadback.Get(), frameIndex * 2 * sizeof(uint64_t));
				frameTimed[frameIndex] = true;
			}
			if (FAILED(list->Close())) {
				ReportDebugMessages();
				return false;
			}
			ID3D12CommandList *const lists[]{list.Get()};
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence.Get(), ++fenceValue);
			frames[frameIndex].fence = fenceValue;
			ReportDebugMessages();
			BeginFrame((frameIndex + 1) % kFrameCount);
			return true;
		}

		// Keeps a resource alive until the GPU has finished the frame being recorded.
		void Retire(ComPtr<ID3D12Resource> &resource) {
			if (resource) frames[frameIndex].garbage.push_back(std::move(resource));
			resource.Reset();
		}

		Allocation Allocate(uint64_t bytes, uint64_t alignment) {
			FrameSlot &slot = frames[frameIndex];
			if (!slot.chunks.empty()) {
				UploadChunk &chunk = slot.chunks.back();
				uint64_t const offset = AlignUp(uploadCursor, alignment);
				if (offset + bytes <= chunk.size) {
					uploadCursor = offset + bytes;
					return {chunk.cpu + offset, chunk.gpu + offset, chunk.buffer.Get(), offset};
				}
			}
			UploadChunk chunk;
			if (bytes <= kUploadChunkBytes && !chunkPool.empty()) {
				chunk = std::move(chunkPool.back());
				chunkPool.pop_back();
			} else {
				chunk.size = (std::max)(kUploadChunkBytes, AlignUp(bytes, 65536));
				D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_UPLOAD};
				D3D12_RESOURCE_DESC description{};
				description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				description.Width = chunk.size;
				description.Height = description.DepthOrArraySize = description.MipLevels = 1;
				description.SampleDesc.Count = 1;
				description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
				                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
				                                           IID_PPV_ARGS(&chunk.buffer)))) return {};
				D3D12_RANGE const none{0, 0};
				if (FAILED(chunk.buffer->Map(0, &none, reinterpret_cast<void **>(&chunk.cpu)))) return {};
				chunk.gpu = chunk.buffer->GetGPUVirtualAddress();
			}
			slot.chunks.push_back(std::move(chunk));
			UploadChunk &fresh = slot.chunks.back();
			uploadCursor = bytes;
			return {fresh.cpu, fresh.gpu, fresh.buffer.Get(), 0};
		}

		Allocation Upload(void const *data, uint64_t bytes, uint64_t alignment) {
			Allocation const allocation = Allocate(bytes, alignment);
			if (allocation.cpu != nullptr) std::memcpy(allocation.cpu, data, bytes);
			return allocation;
		}

		// Geometry cache

		// A cached buffer, or null. Its segment is then in use by the frame being recorded.
		CacheEntry const *FindCached(CacheKey const &key) {
			auto const found = cache.find(key);
			if (found == cache.end()) return nullptr;
			cacheSegments[found->second.segment].lastUse = fenceValue + 1;
			return &found->second;
		}

		// Room for a new cached buffer. False when it does not fit, or the next segment is still in use by
		// this frame, and then the caller uploads for this frame only.
		bool StoreCached(CacheKey const &key, uint64_t bytes, CacheEntry &entry, uint8_t *&cpu) {
			if (bytes == 0 || bytes > kCacheSegmentBytes) return false;
			CacheSegment *segment = &cacheSegments[activeCacheSegment];
			uint64_t offset = AlignUp(segment->cursor, 16);
			if (!segment->buffer || offset + bytes > kCacheSegmentBytes) {
				if (segment->buffer) activeCacheSegment = (activeCacheSegment + 1) % kCacheSegments;
				segment = &cacheSegments[activeCacheSegment];
				uint64_t const thisFrame = fenceValue + 1;
				if (segment->lastUse == thisFrame) return false;
				WaitFor(segment->lastUse);
				for (CacheKey const &old: segment->keys) cache.erase(old);
				segment->keys.clear();
				segment->cursor = offset = 0;
				if (!segment->buffer) {
					D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_UPLOAD};
					D3D12_RESOURCE_DESC description{};
					description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
					description.Width = kCacheSegmentBytes;
					description.Height = description.DepthOrArraySize = description.MipLevels = 1;
					description.SampleDesc.Count = 1;
					description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
					D3D12_RANGE const none{0, 0};
					if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
					                                           D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					                                           IID_PPV_ARGS(&segment->buffer))) ||
					    FAILED(segment->buffer->Map(0, &none, reinterpret_cast<void **>(&segment->cpu)))) {
						segment->buffer.Reset();
						return false;
					}
					segment->gpu = segment->buffer->GetGPUVirtualAddress();
				}
			}
			segment->cursor = offset + bytes;
			segment->lastUse = fenceValue + 1;
			segment->keys.push_back(key);
			entry.segment = activeCacheSegment;
			entry.offset = static_cast<uint32_t>(offset);
			entry.bytes = static_cast<uint32_t>(bytes);
			cache[key] = entry;
			cpu = segment->cpu + offset;
			return true;
		}

		D3D12_GPU_VIRTUAL_ADDRESS CachedAddress(CacheEntry const &entry) const {
			return cacheSegments[entry.segment].gpu + entry.offset;
		}

		void Transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES &state, D3D12_RESOURCE_STATES wanted) {
			if (state == wanted) return;
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = state;
			barrier.Transition.StateAfter = wanted;
			list->ResourceBarrier(1, &barrier);
			state = wanted;
		}

		// 0 the back buffer, 1 the light layer, 2 the relit frame, 3 the sunlight.
		D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetView(uint32_t index = 0) const {
			D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(index) * rtvIncrement;
			return handle;
		}
		D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const { return dsvHeap->GetCPUDescriptorHandleForHeapStart(); }

		// Texture views

		D3D12_CPU_DESCRIPTOR_HANDLE ViewCpu(uint32_t slot) const {
			D3D12_CPU_DESCRIPTOR_HANDLE handle = viewHeap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(slot) * viewIncrement;
			return handle;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE ViewGpu(uint32_t slot) const {
			D3D12_GPU_DESCRIPTOR_HANDLE handle = viewHeap->GetGPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<UINT64>(slot) * viewIncrement;
			return handle;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE SamplerGpu(uint32_t slot) const {
			D3D12_GPU_DESCRIPTOR_HANDLE handle = samplerHeap->GetGPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<UINT64>(slot) * samplerIncrement;
			return handle;
		}

		// A view slot is reused only once the frames that may still read it are done.
		uint32_t CreateView(ID3D12Resource *resource, DXGI_FORMAT format, uint32_t levels) {
			uint32_t slot = 0;
			if (!freeViews.empty()) {
				slot = freeViews.back();
				freeViews.pop_back();
			} else if (nextView < kViewSlots) {
				slot = nextView++;
			} else {
				return 0;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC description{};
			description.Format = format;
			description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			description.Texture2D.MipLevels = levels;
			device->CreateShaderResourceView(resource, &description, ViewCpu(slot));
			return slot;
		}

		void ReleaseTexture(Texture &texture) {
			if (texture.resource && !pinOf.empty()) {
				auto const pinned = pinOf.find(texture.resource.Get());
				if (pinned != pinOf.end() && !pins[pinned->second - 1].orphan) {
					// The pin keeps the image from here on, ready to be read by any shader (PinView).
					Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
					pins[pinned->second - 1].orphan = true;
				}
			}
			if (texture.view != 0) frames[frameIndex].freedViews.push_back(texture.view);
			texture.view = 0;
			Retire(texture.resource);
		}

		// cGDriver::CreateTextureResource
		bool CreateTextureResource(Texture &texture, uint32_t internalFormat, uint32_t width, uint32_t height,
		                           uint32_t levels) {
			DXGI_FORMAT const format = D3D11TextureFormat(internalFormat);
			if (!device || format == DXGI_FORMAT_UNKNOWN || width == 0 || height == 0 ||
			    width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
				return false;
			}
			if (levels == 0) levels = 1;
			if (levels > D3D11MipLevelCount(width, height)) return false;

			D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC const description = TextureDescription(format, width, height, levels);
			ComPtr<ID3D12Resource> resource;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                           IID_PPV_ARGS(&resource)))) {
				return false;
			}
			uint32_t const view = CreateView(resource.Get(), format, levels);
			if (view == 0) return false;
			ReleaseTexture(texture);
			texture.resource = std::move(resource);
			texture.view = view;
			texture.state = D3D12_RESOURCE_STATE_COPY_DEST;
			texture.format = format;
			texture.width = width;
			texture.height = height;
			texture.levels = levels;
			return true;
		}

		// Copies rows into a texture through upload memory. Width and height are in texels, already
		// whole blocks for compressed formats, rows counts block rows for those. fill writes one row and
		// returns false to abandon the upload.
		template<typename Fill>
		bool UploadTexture(Texture &texture, uint32_t subresource, uint32_t x, uint32_t y, uint32_t width,
		                   uint32_t height, uint32_t rows, uint32_t rowBytes, Fill fill) {
			uint64_t const pitch = AlignUp(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
			Allocation const allocation = Allocate(pitch * rows, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
			if (allocation.cpu == nullptr) return false;
			for (uint32_t row = 0; row < rows; ++row) {
				if (!fill(allocation.cpu + row * pitch, row)) return false;
			}
			Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_COPY_DEST);
			D3D12_TEXTURE_COPY_LOCATION destination{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
			destination.SubresourceIndex = subresource;
			D3D12_TEXTURE_COPY_LOCATION source{allocation.resource, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
			source.PlacedFootprint.Offset = allocation.offset;
			source.PlacedFootprint.Footprint = {texture.format, width, height, 1, static_cast<UINT>(pitch)};
			list->CopyTextureRegion(&destination, x, y, 0, &source, nullptr);
			return true;
		}

		// Pipelines

		static uint32_t SamplerIndex(TextureStage const &stage) {
			bool const minimumLinear = stage.minFilter == 1 || stage.minFilter == 5 || stage.minFilter == 7;
			bool const magnificationLinear = stage.magFilter == 1;
			bool const mipLinear = stage.minFilter == 6 || stage.minFilter == 7;
			return (minimumLinear ? 1u : 0u) | (magnificationLinear ? 2u : 0u) | (mipLinear ? 4u : 0u) |
			       (stage.wrapU == 2 ? 8u : 0u) | (stage.wrapV == 2 ? 16u : 0u);
		}

		ID3D12PipelineState *Pipeline(PipelineKey const &key) {
			auto const found = pipelines.find(key);
			if (found != pipelines.end()) return found->second.Get();

			uint64_t const bits = key.bits;
			uint32_t const kind = bits & 3;
			bool const blit = kind >= 2;
			D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
			description.pRootSignature = rootSignature.Get();
			// Bit 52: an owned scene draw, with instances.
			bool const instanced = !blit && ((bits >> 52) & 1);
			ID3DBlob *vs = blit ? blitVertexShader.Get() : (kind == 1 ? flatVertexShader.Get() : vertexShader.Get());
			if (instanced) vs = instancedVertexShaders[kind].Get();
			ID3DBlob *ps = blit ? blitPixelShader.Get() : (kind == 1 ? flatPixelShader.Get() : pixelShader.Get());
			if (relight) ps = blit ? blitGBufferPixelShader.Get() : gbufferPixelShaders[kind][(bits >> 6) & 1].Get();
			description.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
			description.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};

			D3D12_INPUT_ELEMENT_DESC const geometryElements[] = {
				{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(D3D11Vertex, position),
				 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				{"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(D3D11Vertex, normal),
				 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				{"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, offsetof(D3D11Vertex, color),
				 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(D3D11Vertex, texCoord[0]),
				 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				{"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(D3D11Vertex, texCoord[1]),
				 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				{"INSTANCE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
				{"INSTANCE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
				{"INSTANCE", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA, 1},
			};
			D3D12_INPUT_ELEMENT_DESC const blitElements[] = {
				{"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(BlitVertex, position),
				 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
				{"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(BlitVertex, texCoord),
				 D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
			};
			description.InputLayout = blit ? D3D12_INPUT_LAYOUT_DESC{blitElements, 2}
			                               : D3D12_INPUT_LAYOUT_DESC{geometryElements, instanced ? 8u : 5u};

			D3D12_RENDER_TARGET_BLEND_DESC &target = description.BlendState.RenderTarget[0];
			target.LogicOp = D3D12_LOGIC_OP_NOOP;
			target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			D3D12_DEPTH_STENCIL_DESC &depth = description.DepthStencilState;
			depth.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
			depth.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP,
			                   D3D12_COMPARISON_FUNC_ALWAYS};
			depth.BackFace = depth.FrontFace;
			D3D12_RASTERIZER_DESC &rasterizer = description.RasterizerState;
			rasterizer.FillMode = D3D12_FILL_MODE_SOLID;
			rasterizer.CullMode = D3D12_CULL_MODE_NONE;
			rasterizer.DepthClipEnable = TRUE;
			if (blit) {
				// cGDriver::CreateBlitPipeline: no depth or stencil, source-over blending for the alpha kind.
				target.SrcBlend = target.SrcBlendAlpha = D3D12_BLEND_ONE;
				target.DestBlend = target.DestBlendAlpha = D3D12_BLEND_ZERO;
				if (kind == 3) {
					target.BlendEnable = TRUE;
					target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
					target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
					target.SrcBlendAlpha = D3D12_BLEND_ONE;
					target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				}
				target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
				description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			} else {
				// cGDriver::ApplyRenderStates
				uint32_t const sourceBlendValue = (bits >> 7) & 15;
				uint32_t const destinationBlendValue = (bits >> 11) & 15;
				target.BlendEnable = (bits >> 6) & 1;
				target.SrcBlend = static_cast<D3D12_BLEND>(D3D11Blend(sourceBlendValue));
				target.DestBlend = static_cast<D3D12_BLEND>(D3D11Blend(destinationBlendValue));
				target.SrcBlendAlpha = static_cast<D3D12_BLEND>(D3D11AlphaBlend(sourceBlendValue));
				target.DestBlendAlpha = static_cast<D3D12_BLEND>(D3D11AlphaBlend(destinationBlendValue));
				target.RenderTargetWriteMask = ((bits >> 15) & 1) ? D3D12_COLOR_WRITE_ENABLE_ALL : 0;

				depth.DepthEnable = (bits >> 16) & 1;
				depth.DepthWriteMask = ((bits >> 17) & 1) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
				depth.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(D3D11Comparison((bits >> 18) & 7));
				depth.StencilEnable = (bits >> 21) & 1;
				depth.StencilReadMask = static_cast<UINT8>(bits >> 22);
				depth.StencilWriteMask = static_cast<UINT8>(bits >> 30);
				depth.FrontFace.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(D3D11Comparison((bits >> 38) & 7));
				depth.FrontFace.StencilFailOp = static_cast<D3D12_STENCIL_OP>(D3D11StencilOperation((bits >> 41) & 7));
				depth.FrontFace.StencilDepthFailOp =
					static_cast<D3D12_STENCIL_OP>(D3D11StencilOperation((bits >> 44) & 7));
				depth.FrontFace.StencilPassOp = static_cast<D3D12_STENCIL_OP>(D3D11StencilOperation((bits >> 47) & 7));
				depth.BackFace = depth.FrontFace;

				rasterizer.CullMode = ((bits >> 50) & 1) ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
				rasterizer.FrontCounterClockwise = TRUE;
				rasterizer.DepthBias = key.depthBias;
				description.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>((bits >> 2) & 3);
				uint32_t const cut = (bits >> 4) & 3;
				description.IBStripCutValue = cut == 1 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFF
				                              : cut == 2 ? D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF
				                              : D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
			}
			description.SampleMask = UINT_MAX;
			description.NumRenderTargets = 1;
			description.RTVFormats[0] = kColorFormat;
			if (relight) {
				// The light layer follows the colour's coverage. The city puts its factor and full weight
				// down, UI takes the weight away, and additive glows leave the surface beneath as it was.
				description.NumRenderTargets = 2;
				description.RTVFormats[1] = kColorFormat;
				description.BlendState.IndependentBlendEnable = TRUE;
				D3D12_RENDER_TARGET_BLEND_DESC &light = description.BlendState.RenderTarget[1];
				light.LogicOp = D3D12_LOGIC_OP_NOOP;
				light.BlendOp = light.BlendOpAlpha = D3D12_BLEND_OP_ADD;
				light.RenderTargetWriteMask = target.RenderTargetWriteMask;
				bool const city = (bits >> 51) & 1;
				if (target.BlendEnable && target.DestBlend == D3D12_BLEND_ONE) {
					light.RenderTargetWriteMask = 0;
				} else if (city) {
					light.BlendEnable = target.BlendEnable;
					light.SrcBlend = light.SrcBlendAlpha = D3D12_BLEND_ONE;
					light.DestBlend = light.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
				} else {
					light.BlendEnable = TRUE;
					light.SrcBlend = light.SrcBlendAlpha = D3D12_BLEND_ZERO;
					light.DestBlend = light.DestBlendAlpha =
						target.BlendEnable ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_ZERO;
				}
			}
			description.DSVFormat = kDepthFormat;
			description.SampleDesc.Count = 1;

			ComPtr<ID3D12PipelineState> pipeline;
			if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&pipeline)))) {
				std::fprintf(stderr, "cannot create a pipeline state (key %016llX %d)\n",
				             static_cast<unsigned long long>(bits), key.depthBias);
				ReportDebugMessages();
			}
			return pipelines.emplace(key, pipeline).first->second.Get();
		}

		bool SetPipeline(PipelineKey const &key) {
			if (appliedPipeline != nullptr && key == appliedPipelineKey) return true;
			ID3D12PipelineState *const pipeline = Pipeline(key);
			if (pipeline == nullptr) return false;
			list->SetPipelineState(pipeline);
			appliedPipeline = pipeline;
			appliedPipelineKey = key;
			return true;
		}

		void SetTopology(D3D_PRIMITIVE_TOPOLOGY topology) {
			if (topology == appliedTopology) return;
			list->IASetPrimitiveTopology(topology);
			appliedTopology = topology;
		}

		void SetConstants(D3D12_GPU_VIRTUAL_ADDRESS address) {
			if (address == appliedConstants) return;
			list->SetGraphicsRootConstantBufferView(0, address);
			appliedConstants = address;
		}

		void SetStage(uint32_t stage, uint32_t view, uint32_t sampler) {
			if (view != appliedViews[stage]) {
				list->SetGraphicsRootDescriptorTable(1 + stage, ViewGpu(view));
				appliedViews[stage] = view;
			}
			if (sampler != appliedSamplers[stage]) {
				list->SetGraphicsRootDescriptorTable(3 + stage, SamplerGpu(sampler));
				appliedSamplers[stage] = sampler;
			}
		}

		// The constants of a geometry draw from the SimGL state, as cGDriver::UpdateConstants makes them.
		void FillConstants(DriverConstants &constants, uint32_t flagInputs) {
			constants.flags = flagInputs;
			memcpy(constants.modelView, matrices[0], sizeof(constants.modelView));
			memcpy(constants.projection, matrices[1], sizeof(constants.projection));
			if (normalMatrixDirty) {
				MakeNormalMatrix(matrices[0], normalMatrix);
				normalMatrixDirty = false;
			}
			memcpy(constants.normalMatrix, normalMatrix, sizeof(constants.normalMatrix));
			if (swapCamera && ownerDepth > 0 && ownerPass >= 3 && matrices[1][0] == swapFrom[0] &&
			    matrices[1][5] == swapFrom[5] && matrices[1][12] == swapFrom[12] && matrices[1][13] == swapFrom[13]) {
				for (int c = 0; c < 4; ++c) {
					for (int row = 0; row < 4; ++row) {
						constants.modelView[c * 4 + row] = swapChange[row] * matrices[0][c * 4] + swapChange[4 + row] * matrices[0][c * 4 + 1] +
						                                   swapChange[8 + row] * matrices[0][c * 4 + 2] + swapChange[12 + row] * matrices[0][c * 4 + 3];
					}
					constants.projection[c * 4] = swapProjection[c * 4];
					constants.projection[c * 4 + 1] = swapProjection[c * 4 + 1];
				}
				MakeNormalMatrix(constants.modelView, constants.normalMatrix);
			}
			constants.alphaFunction = alphaFunction;
			constants.alphaReference = alphaReference;
			if (enabledCapabilities[kGDCapability_AlphaTest]) constants.flags |= 8;
			memcpy(constants.globalAmbient, globalAmbient, sizeof(constants.globalAmbient));
			memcpy(constants.lightAmbient, lightAmbient, sizeof(constants.lightAmbient));
			memcpy(constants.lightDiffuse, lightDiffuse, sizeof(constants.lightDiffuse));
			memcpy(constants.lightSpecular, lightSpecular, sizeof(constants.lightSpecular));
			memcpy(constants.lightPosition, lightPosition, sizeof(constants.lightPosition));
			memcpy(constants.materialAmbient, materialAmbient, sizeof(constants.materialAmbient));
			memcpy(constants.materialDiffuse, materialDiffuse, sizeof(constants.materialDiffuse));
			memcpy(constants.materialSpecular, materialSpecular, sizeof(constants.materialSpecular));
			memcpy(constants.materialEmission, materialEmission, sizeof(constants.materialEmission));
			constants.materialParameters[0] = materialShininess;
			for (uint32_t stageIndex = 0; stageIndex < 2; ++stageIndex) {
				TextureStage const &source = textureStages[stageIndex];
				DriverConstants::StageConstants &destination = constants.stages[stageIndex];
				memcpy(constants.textureMatrices[stageIndex], source.matrix, sizeof(source.matrix));
				destination.modes[0] = source.environmentMode;
				destination.modes[1] = source.rgbMode;
				destination.modes[2] = source.alphaMode;
				destination.modes[3] = source.rgbScale;
				destination.parameters0[0] = source.alphaScale;
				for (uint32_t parameter = 0; parameter < 3; ++parameter) {
					destination.parameters0[parameter + 1] = source.rgbParameters[parameter];
					destination.parameters1[parameter] = source.alphaParameters[parameter];
				}
				destination.parameters1[3] = source.coordinateSource;
			}
			if (flagInputs & 4) {
				if (ambientVertexColors) constants.flags |= 32;
				if (diffuseFromVertex) constants.flags |= 64;
				for (uint32_t light = 0; light < 8; ++light)
					if (lightsEnabled[light]) constants.enabledLights |= 1u << light;
			}
			if (enabledCapabilities[kGDCapability_Fog]) constants.flags |= 16;
			memcpy(constants.fogColor, fogColor, sizeof(constants.fogColor));
			constants.fogParameters[0] = fogDensity;
			constants.fogParameters[1] = fogStart;
			constants.fogParameters[2] = fogEnd;
			constants.fogParameters[3] = static_cast<float>(fogMode);
			memcpy(constants.textureFactor, textureFactor, sizeof(constants.textureFactor));
		}

		// The geometry pipeline for the SimGL state.
		PipelineKey GeometryKey(D3D12_PRIMITIVE_TOPOLOGY_TYPE type, uint64_t cut) const {
			bool const stencil = enabledCapabilities[kGDCapability_StencilTest];
			PipelineKey key;
			key.bits = static_cast<uint64_t>(shadeModel == 0 ? 1 : 0) |
			           (static_cast<uint64_t>(type) << 2) |
			           (cut << 4) |
			           (static_cast<uint64_t>(enabledCapabilities[kGDCapability_Blend]) << 6) |
			           (static_cast<uint64_t>(sourceBlend) << 7) |
			           (static_cast<uint64_t>(destinationBlend) << 11) |
			           (static_cast<uint64_t>(colorWriteEnabled) << 15) |
			           (static_cast<uint64_t>(enabledCapabilities[kGDCapability_DepthTest]) << 16) |
			           (static_cast<uint64_t>(depthWriteEnabled) << 17) |
			           (static_cast<uint64_t>(depthFunction) << 18) |
			           (static_cast<uint64_t>(stencil) << 21) |
			           (static_cast<uint64_t>(stencilReadMask) << 22) |
			           (static_cast<uint64_t>(stencilWriteMask) << 30) |
			           (static_cast<uint64_t>(stencilFunction) << 38) |
			           (static_cast<uint64_t>(stencilFailOperation) << 41) |
			           (static_cast<uint64_t>(stencilDepthFailOperation) << 44) |
			           (static_cast<uint64_t>(stencilPassOperation) << 47) |
			           (static_cast<uint64_t>(enabledCapabilities[kGDCapability_CullFace]) << 50) |
			           (static_cast<uint64_t>(relight && DrawsCity()) << 51);
			key.depthBias = polygonOffset;
			return key;
		}

		// cGDriver::BindGeometryPipeline. primitive is a SimGL primitive D3D draws natively, indexBits 16 or
		// 32 for indexed draws (strips then cut at the largest index, as in Direct3D 11) or 0.
		bool BindGeometry(uint32_t primitive, uint32_t indexBits) {
			D3D11_PRIMITIVE_TOPOLOGY const topology = D3D11Topology(primitive);
			if (!backBuffer || topology == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) return false;
			if (relight && ownerDrawsShadows && ownerDepth > 0) return false;
			if (relight) cityDrawn |= DrawsCity();

			uint32_t views[2]{};
			uint32_t samplers[2]{};
			uint32_t flagInputs = 0;
			for (uint32_t stage = 0; stage < 2; ++stage) {
				if (!textureStageEnabled[stage]) continue;
				auto const found = textures.find(boundTextures[stage]);
				if (found == textures.end() || !found->second.resource) continue;
				Texture &texture = found->second;
				Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
				views[stage] = texture.view;
				samplers[stage] = SamplerIndex(textureStages[stage]);
				flagInputs |= 1u << stage;
			}
			bool const lit = lightingEnabled;
			if (lit) flagInputs |= 4;
			if (lit && RZVertexFormatNumElements(interleavedFormat, kGDElementType_Normal) != 0) flagInputs |= 128;

			if (constantsDirty || flagInputs != constantsFlagInputs || constantsAddress == 0) {
				DriverConstants constants{};
				FillConstants(constants, flagInputs);

				if (constantsAddress == 0 || memcmp(&constantsCache, &constants, sizeof(constants)) != 0) {
					Allocation const allocation =
						Upload(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
					if (allocation.cpu == nullptr) return false;
					constantsAddress = allocation.gpu;
					constantsCache = constants;
				}
				constantsDirty = false;
				constantsFlagInputs = flagInputs;
				if (relight) TrackCamera();
			}

			bool const stencil = enabledCapabilities[kGDCapability_StencilTest];
			bool const strip = topology == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ||
			                   topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
			D3D12_PRIMITIVE_TOPOLOGY_TYPE const type =
				topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
				: (topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST || topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP)
				? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
				: D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			uint64_t const cut = strip && indexBits != 0 ? (indexBits == 16 ? 1 : 2) : 0;
			PipelineKey const key = GeometryKey(type, cut);
			if (!SetPipeline(key)) return false;
			if (stencilReference != appliedStencilReference) {
				list->OMSetStencilRef(static_cast<UINT>(stencilReference));
				appliedStencilReference = stencilReference;
			}
			SetConstants(constantsAddress);
			for (uint32_t stage = 0; stage < 2; ++stage) SetStage(stage, views[stage], samplers[stage]);
			SetTopology(static_cast<D3D_PRIMITIVE_TOPOLOGY>(topology));
			if ((enabledCapabilities[kGDCapability_DepthTest] && depthWriteEnabled) || stencil) {
				depthScratchValid = false;
			}
			return true;
		}

		// The owned scene's capture

		// What BindGeometry would bind for a triangle list of vertices in format, without touching the GPU.
		bool CaptureState(uint32_t format, CapturedState &state) {
			if (!backBuffer || (relight && ownerDrawsShadows && ownerDepth > 0)) return false;
			uint32_t flagInputs = 0;
			for (uint32_t stage = 0; stage < 2; ++stage) {
				state.textures[stage] = state.samplers[stage] = 0;
				if (!textureStageEnabled[stage]) continue;
				auto const found = textures.find(boundTextures[stage]);
				if (found == textures.end() || !found->second.resource) continue;
				state.textures[stage] = boundTextures[stage];
				state.samplers[stage] = SamplerIndex(textureStages[stage]);
				flagInputs |= 1u << stage;
			}
			if (lightingEnabled) flagInputs |= 4;
			if (lightingEnabled && RZVertexFormatNumElements(format, kGDElementType_Normal) != 0) flagInputs |= 128;
			DriverConstants constants{};
			FillConstants(constants, flagInputs);
			std::memcpy(state.constants, &constants, sizeof(constants));
			PipelineKey const key = GeometryKey(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, 0);
			state.pipeline = key.bits;
			state.depthBias = key.depthBias;
			state.stencilReference = stencilReference;
			return true;
		}

		// Hands a draw to the capture as a triangle list: vertices from base (count of them, or up to the largest
		// index), indices of indexType 3 (16-bit) or 5 (32-bit), or in order when indices is null. True when
		// the capture took it.
		bool CaptureDraw(uint8_t const *base, uint32_t format, uint32_t stride, uint32_t count, uint32_t primitive,
		                 void const *indices, uint32_t indexCount, uint32_t indexType) {
			if (base == nullptr || stride == 0) return false;
			uint32_t const sourceCount = indices != nullptr ? indexCount : count;
			std::vector<uint32_t> &source = captureSource;
			source.resize(sourceCount);
			// Strips are cut at the largest index, as Direct3D does.
			uint32_t const restart = primitive == 1 && indices != nullptr ? (indexType == 3 ? 0xFFFF : UINT32_MAX) : UINT32_MAX;
			if (indices == nullptr) {
				for (uint32_t i = 0; i < sourceCount; ++i) source[i] = i;
			} else if (indexType == 3) {
				auto const *from = static_cast<uint16_t const *>(indices);
				for (uint32_t i = 0; i < sourceCount; ++i) source[i] = from[i];
			} else if (indexType == 5) {
				std::memcpy(source.data(), indices, sourceCount * sizeof(uint32_t));
			} else {
				return false;
			}
			std::vector<uint32_t> &list = captureIndices;
			list.clear();
			if (primitive == 0) {
				list.assign(source.begin(), source.end() - sourceCount % 3);
			} else if (primitive == 1) {
				// Each triangle of a strip starts at its first vertex, so flat shading keeps its colour.
				uint32_t run = 0;
				for (uint32_t i = 0; i < sourceCount; ++i) {
					if (source[i] == restart) {
						run = 0;
						continue;
					}
					if (++run < 3) continue;
					uint32_t const a = source[i - 2], b = source[i - 1], c = source[i];
					if ((run & 1) != 0) list.insert(list.end(), {a, b, c});
					else list.insert(list.end(), {a, c, b});
				}
			} else if (primitive == 2 || primitive == 6 || primitive == 7) {
				if (!ConvertPrimitiveIndices(primitive, source, list)) return false;
			} else {
				return false;
			}
			if (list.empty()) return true;
			uint32_t low = UINT32_MAX, high = 0;
			for (uint32_t index: list) {
				low = (std::min)(low, index);
				high = (std::max)(high, index);
			}
			if (indices == nullptr && high >= count) return false;
			for (uint32_t &index: list) index -= low;
			if (!ConvertVertices(format, stride, base + static_cast<size_t>(low) * stride, high - low + 1, captureVertices)) {
				return false;
			}
			CapturedState state;
			if (!CaptureState(format, state)) return true;
			return capture->Capture(state, captureVertices.data(), high - low + 1, list.data(),
			                        static_cast<uint32_t>(list.size()), format);
		}

		// Whether the next draw is part of the city, which relighting may change.
		bool DrawsCity() const {
			return ownersSeen ? ownerDepth > 0 : enabledCapabilities[kGDCapability_DepthTest];
		}

		// Learns the camera from a static city draw. Models only turn about world up, so column 1 of their
		// model-view is world up in view space. A tilted one (an automaton on a slope) is skipped.
		void TrackCamera() {
			if (!DrawsCity() || (ownersSeen && ownerPass != 1) || !enabledCapabilities[kGDCapability_DepthTest]) return;
			LearnCamera(matrices[0], matrices[1], viewportWidth, viewportHeight);
		}

		void LearnCamera(float const *modelView, float const *projection, int32_t viewportWidth, int32_t viewportHeight) {
			float const length = std::sqrt(modelView[5] * modelView[5] + modelView[6] * modelView[6]);
			if (length < 1.0e-6f || std::fabs(modelView[4]) > 1.0e-3f * length || projection[0] == 0.0f ||
			    projection[5] == 0.0f || projection[10] == 0.0f || viewportWidth <= 0 || viewportHeight <= 0) {
				return;
			}
			camera.up[0] = 0.0f;
			camera.up[1] = modelView[5] / length;
			camera.up[2] = modelView[6] / length;
			// A partial redraw fits the projection to its viewport, so pixel sizes come from both.
			camera.pixel[0] = 2.0f / (static_cast<float>(viewportWidth) * projection[0]);
			camera.pixel[1] = 2.0f / (static_cast<float>(viewportHeight) * projection[5]);
			camera.depth = 2.0f / projection[10];
			camera.known = true;
		}

		// Converts vertices [first, first + count) of the InterleavedArrays source and binds them. With the
		// blob they came from, the converted vertices are kept and later draws of the blob reuse them.
		bool UploadVertices(uint8_t const *base, uint32_t format, uint32_t stride, uint32_t first, uint32_t count,
		                    nSCDWire::BlobKey const &blob) {
			if (base == nullptr || stride == 0 || count == 0) return false;
			uint64_t const byteSize = static_cast<uint64_t>(count) * sizeof(D3D11Vertex);
			if (byteSize > UINT32_MAX) return false;
			CacheKey const key{blob.low, blob.high, blob.size, CacheKind::Vertices, format, stride, first, count};
			D3D12_GPU_VIRTUAL_ADDRESS address = 0;
			if (blob.size != 0) {
				if (CacheEntry const *cached = FindCached(key)) address = CachedAddress(*cached);
			}
			if (address == 0) {
				uint8_t const *const source = base + static_cast<size_t>(first) * stride;
				if (!ConvertVertices(format, stride, source, count, vertexScratch)) return false;
				CacheEntry entry;
				uint8_t *cpu = nullptr;
				uint64_t &seen = seenVertices[(blob.low ^ blob.high) & (seenVertices.size() - 1)];
				bool const again = blob.size != 0 && seen == blob.low;
				seen = blob.low;
				if (again && StoreCached(key, byteSize, entry, cpu)) {
					std::memcpy(cpu, vertexScratch.data(), byteSize);
					address = CachedAddress(entry);
				} else {
					Allocation const allocation = Upload(vertexScratch.data(), byteSize, 16);
					if (allocation.cpu == nullptr) return false;
					address = allocation.gpu;
				}
			}
			D3D12_VERTEX_BUFFER_VIEW const view{address, static_cast<UINT>(byteSize), sizeof(D3D11Vertex)};
			list->IASetVertexBuffers(0, 1, &view);
			return true;
		}

		// Binds indices. A cached copy is kept when key names what they were made from.
		bool UploadIndices(void const *indices, uint32_t count, bool wide, CacheKey const *key = nullptr,
		                   uint32_t minimumIndex = 0, uint32_t maximumIndex = 0) {
			uint64_t const bytes = static_cast<uint64_t>(count) * (wide ? 4 : 2);
			if (count == 0 || bytes > UINT32_MAX) return false;
			D3D12_GPU_VIRTUAL_ADDRESS address = 0;
			CacheEntry entry{0, 0, 0, count, minimumIndex, maximumIndex};
			uint8_t *cpu = nullptr;
			if (key != nullptr && StoreCached(*key, bytes, entry, cpu)) {
				std::memcpy(cpu, indices, bytes);
				address = CachedAddress(entry);
			} else {
				Allocation const allocation = Upload(indices, bytes, 4);
				if (allocation.cpu == nullptr) return false;
				address = allocation.gpu;
			}
			BindIndices(address, count, wide);
			return true;
		}

		void BindIndices(D3D12_GPU_VIRTUAL_ADDRESS address, uint32_t count, bool wide) {
			D3D12_INDEX_BUFFER_VIEW const view{
				address, count * (wide ? 4u : 2u), wide ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT
			};
			list->IASetIndexBuffer(&view);
		}

		// The extension's vertices hold byteSize bytes, as cGDriver::ExtensionReservationHolds.
		bool ExtensionHolds(uint32_t byteSize) const {
			uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
			return extensionVerticesValid && byteSize != 0 && byteSize % stride == 0 &&
			       byteSize <= extensionVertices.size();
		}

		// Viewport

		void ApplyViewport() {
			if (!backBuffer) return;
			list->RSSetViewports(1, &viewport);
			list->RSSetScissorRects(1, &scissor);
		}

		void SetFullViewport() {
			scissorEnabled = false;
			viewportX = viewportY = 0;
			viewportWidth = windowWidth;
			viewportHeight = windowHeight;
			viewport = {0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0.0f, 1.0f};
			// Direct3D 12 always scissors. A disabled SimGL scissor is the whole target.
			scissor = {0, 0, windowWidth, windowHeight};
			ApplyViewport();
		}

		// Buffer regions and depth

		// Two planes, depth and stencil, both copied for a depth region.
		void CopyRegion(ID3D12Resource *destination, uint32_t x, uint32_t y, ID3D12Resource *source,
		                D3D12_BOX const &box, bool depth) {
			for (uint32_t plane = 0; plane < (depth ? 2u : 1u); ++plane) {
				D3D12_TEXTURE_COPY_LOCATION to{destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
				to.SubresourceIndex = plane;
				D3D12_TEXTURE_COPY_LOCATION from{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
				from.SubresourceIndex = plane;
				list->CopyTextureRegion(&to, x, y, 0, &from, &box);
			}
		}

		bool RefreshDepthScratch() {
			if (!depthScratch) {
				D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
				D3D12_RESOURCE_DESC const description = TextureDescription(kDepthTypeless, windowWidth, windowHeight, 1);
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
				                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				                                           IID_PPV_ARGS(&depthScratch)))) return false;
				depthScratchState = D3D12_RESOURCE_STATE_COPY_DEST;
				depthScratchValid = false;
			}
			if (depthScratchValid) return true;
			D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			Transition(depthBuffer.Get(), depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(depthScratch.Get(), depthScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyResource(depthScratch.Get(), depthBuffer.Get());
			Transition(depthBuffer.Get(), depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			depthScratchValid = true;
			return true;
		}

		bool IsRegion(uint32_t region) const {
			return region >= 1 && region <= kRegionCount && (regionFlags & (1u << (region - 1))) != 0 &&
			       regions[region - 1].texture != nullptr;
		}

		bool CreateRegion(uint32_t index, int32_t type) {
			if (!device || index >= kRegionCount || type < 0 || type > 1 || windowWidth <= 0 || windowHeight <= 0) {
				return false;
			}
			D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC const description =
				TextureDescription(type == 0 ? kColorFormat : kDepthTypeless, windowWidth, windowHeight, 1);
			ComPtr<ID3D12Resource> texture, light;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                           IID_PPV_ARGS(&texture)))) return false;
			if (relight && type == 0 &&
			    FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                           IID_PPV_ARGS(&light)))) return false;
			Retire(regions[index].texture);
			Retire(regions[index].light);
			regions[index].texture = std::move(texture);
			regions[index].state = D3D12_RESOURCE_STATE_COPY_DEST;
			regions[index].light = std::move(light);
			regions[index].lightState = D3D12_RESOURCE_STATE_COPY_DEST;
			regions[index].type = type;
			return true;
		}

		void RetireRegion(Region &region) {
			Retire(region.texture);
			Retire(region.light);
		}

		// Copies the light layer the way a colour region copy moves the back buffer. toRegion says which
		// way.
		void CopyLightRegion(Region &region, bool toRegion, uint32_t x, uint32_t y, D3D12_BOX const &box) {
			if (!region.light || !lightBuffer) return;
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			Transition(region.light.Get(), region.lightState,
			           toRegion ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(lightBuffer.Get(), state, toRegion ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST);
			if (toRegion) CopyRegion(region.light.Get(), x, y, lightBuffer.Get(), box, false);
			else CopyRegion(lightBuffer.Get(), x, y, region.light.Get(), box, false);
			Transition(lightBuffer.Get(), state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}

		bool RegionArgumentsFit(uint32_t region, int32_t a, int32_t b, int32_t width, int32_t height, int32_t c,
		                        int32_t d) const {
			return IsRegion(region) && width > 0 && height > 0 && a >= 0 && b >= 0 && c >= 0 && d >= 0 &&
			       RangeFits(a, width, windowWidth) && RangeFits(b, height, windowHeight) &&
			       RangeFits(c, width, windowWidth) && RangeFits(d, height, windowHeight);
		}

		// Blits

		// cGDriver::DrawBlit
		void DrawBlit(int32_t destX, int32_t destY, int32_t destWidth, int32_t destHeight, int32_t sourceWidth,
		              int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels, bool colorKeyed,
		              void const *colorKey, BlitAlpha alpha, uint32_t alphaValue) {
			if (destWidth <= 0 || destHeight <= 0 || sourceWidth <= 0 || sourceHeight <= 0 || pixels == nullptr) return;
			if (!backBuffer) return;
			uint32_t const pixelBytes = TextureSourcePixelBytes(format, type);
			if (pixelBytes == 0 || pixelBytes > 4) return;

			uint32_t const width = static_cast<uint32_t>(sourceWidth);
			uint32_t const height = static_cast<uint32_t>(sourceHeight);
			if (width > blitTexture.width || height > blitTexture.height) {
				uint32_t const textureWidth = (std::max)(width, blitTexture.width);
				uint32_t const textureHeight = (std::max)(height, blitTexture.height);
				ReleaseTexture(blitTexture);
				blitTexture.width = blitTexture.height = 0;
				D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
				// BGRA matches SC4's 32-bit canvas byte order, so those pixels upload without conversion.
				D3D12_RESOURCE_DESC const description =
					TextureDescription(DXGI_FORMAT_B8G8R8A8_UNORM, textureWidth, textureHeight, 1);
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
				                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				                                           IID_PPV_ARGS(&blitTexture.resource)))) return;
				blitTexture.view = CreateView(blitTexture.resource.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, 1);
				blitTexture.state = D3D12_RESOURCE_STATE_COPY_DEST;
				blitTexture.format = DXGI_FORMAT_B8G8R8A8_UNORM;
				blitTexture.width = textureWidth;
				blitTexture.height = textureHeight;
				blitTexture.levels = 1;
			}

			colorKeyed = colorKeyed && colorKey != nullptr;
			uint32_t key = 0;
			if (colorKeyed) std::memcpy(&key, colorKey, pixelBytes);
			// Some 32-bit UI icons come through the keyed opaque BitBlt with their transparency in alpha.
			bool const keyedSourceWithAlpha =
				colorKeyed && alpha == BlitAlpha::Opaque && TextureSourceComponents(format) == 4;
			bool const usesSourceAlpha =
				alpha == BlitAlpha::Source || alpha == BlitAlpha::SourceModulated || keyedSourceWithAlpha;

			BlitConstants constants{};
			constants.modulate[0] = constants.modulate[1] = constants.modulate[2] = constants.modulate[3] = 1.0f;
			if (alpha == BlitAlpha::Constant) {
				float rgba[4];
				UnpackColor(alphaValue, rgba);
				constants.modulate[3] = rgba[3];
			} else if (alpha == BlitAlpha::SourceModulated) {
				UnpackColor(alphaValue, constants.modulate);
			}
			constants.sourceAlpha = usesSourceAlpha ? 1.0f : 0.0f;

			uint32_t const sourcePitch = (pixelStoreRowLength != 0 ? pixelStoreRowLength : width) * pixelBytes;
			uint8_t const *const sourceRows = static_cast<uint8_t const *>(pixels);
			bool uploaded = false;
			if (type == 1 && format == 3) {
				uploaded = UploadTexture(blitTexture, 0, 0, 0, width, height, height, width * 4,
				                         [&](uint8_t *row, uint32_t y) {
					                         std::memcpy(row, sourceRows + static_cast<size_t>(y) * sourcePitch, width * 4);
					                         return true;
				                         });
			} else {
				bool const byteBgr = type == 1 && format == 2;
				uploaded = UploadTexture(blitTexture, 0, 0, 0, width, height, height, width * 4,
				                         [&](uint8_t *destination, uint32_t y) {
					                         uint8_t const *source = sourceRows + static_cast<size_t>(y) * sourcePitch;
					                         for (uint32_t x = 0; x < width; ++x, source += pixelBytes, destination += 4) {
						                         if (byteBgr) {
							                         destination[0] = source[0];
							                         destination[1] = source[1];
							                         destination[2] = source[2];
							                         destination[3] = 0xff;
							                         continue;
						                         }
						                         uint8_t rgba[4];
						                         if (!ConvertTextureSourcePixel(format, type, source, rgba)) return false;
						                         destination[0] = rgba[2];
						                         destination[1] = rgba[1];
						                         destination[2] = rgba[0];
						                         destination[3] = usesSourceAlpha ? rgba[3] : 0xff;
						                         // Packed formats cannot be compared in the shader, so key them here.
						                         if (colorKeyed) {
							                         uint32_t raw = 0;
							                         std::memcpy(&raw, source, pixelBytes);
							                         if (raw == key) destination[3] = 0;
						                         }
					                         }
					                         return true;
				                         });
				if (!byteBgr) {
					constants.sourceAlpha = 1.0f;
					colorKeyed = false;
				}
			}
			if (!uploaded) return;
			if (colorKeyed) {
				// 24/32-bit keys are 0x00RRGGBB, matching the byte order of the BGR(A) pixels.
				constants.colorKey[0] = static_cast<float>((key >> 16) & 0xff) / 255.0f;
				constants.colorKey[1] = static_cast<float>((key >> 8) & 0xff) / 255.0f;
				constants.colorKey[2] = static_cast<float>(key & 0xff) / 255.0f;
				constants.colorKey[3] = 1.0f;
			}

			float const left = 2.0f * static_cast<float>(destX) / static_cast<float>(windowWidth) - 1.0f;
			float const right = 2.0f * static_cast<float>(destX + destWidth) / static_cast<float>(windowWidth) - 1.0f;
			float const top = 1.0f - 2.0f * static_cast<float>(destY) / static_cast<float>(windowHeight);
			float const bottom = 1.0f - 2.0f * static_cast<float>(destY + destHeight) / static_cast<float>(windowHeight);
			float const u = static_cast<float>(width) / static_cast<float>(blitTexture.width);
			float const v = static_cast<float>(height) / static_cast<float>(blitTexture.height);
			BlitVertex const vertices[4] = {
				{{left, top}, {0.0f, 0.0f}}, {{right, top}, {u, 0.0f}},
				{{left, bottom}, {0.0f, v}}, {{right, bottom}, {u, v}},
			};
			Allocation const vertexAllocation = Upload(vertices, sizeof(vertices), 16);
			Allocation const constantAllocation =
				Upload(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
			if (vertexAllocation.cpu == nullptr || constantAllocation.cpu == nullptr) return;

			bool const blends = alpha != BlitAlpha::Opaque || colorKeyed || constants.sourceAlpha != 0.0f;
			PipelineKey pipeline;
			pipeline.bits = blends ? 3 : 2;
			if (!SetPipeline(pipeline)) return;
			Transition(blitTexture.resource.Get(), blitTexture.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			SetConstants(constantAllocation.gpu);
			SetStage(0, blitTexture.view, kBlitSampler);
			SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			D3D12_VERTEX_BUFFER_VIEW const view{vertexAllocation.gpu, sizeof(vertices), sizeof(BlitVertex)};
			list->IASetVertexBuffers(0, 1, &view);
			// The whole window as the viewport. The scissor stays, which is the whole window too while
			// SimGL's scissor is off.
			D3D12_VIEWPORT const full{
				0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0.0f, 1.0f
			};
			list->RSSetViewports(1, &full);
			list->DrawInstanced(4, 1, 0, 0);
			list->RSSetViewports(1, &viewport);
		}

		// Relighting

		// The sun's direction for azimuth and elevation in degrees, in view space. The azimuth turns from
		// the screen's left towards the far side of the scene, about world up.
		void SunDirection(float azimuth, float elevation, float out[3]) const {
			float const a = azimuth * 0.01745329f;
			float const e = elevation * 0.01745329f;
			float const *const up = camera.up;
			// Left on screen is level, since SC4's camera does not roll. Away is level and into the scene.
			float const left[3]{-1.0f, 0.0f, 0.0f};
			float const away[3]{0.0f, up[2], -up[1]};
			for (int i = 0; i < 3; ++i) {
				out[i] = std::cos(e) * (std::cos(a) * left[i] + std::sin(a) * away[i]) + std::sin(e) * up[i];
			}
		}

		// The same about world axes: the azimuth turns from world +x towards +z, about world up (+y).
		static void WorldSunDirection(float azimuth, float elevation, float out[3]) {
			float const a = azimuth * 0.01745329f;
			float const e = elevation * 0.01745329f;
			out[0] = std::cos(e) * std::cos(a);
			out[1] = std::sin(e);
			out[2] = std::cos(e) * std::sin(a);
		}

		// Draws the published frame: the back buffer relit from the light layer and the depth buffer.
		void RecordLighting() {
			ID3D12DescriptorHeap *const heaps[]{viewHeap.Get(), samplerHeap.Get()};
			list->SetDescriptorHeaps(2, heaps);
			uint32_t const query = frameIndex * kLightingTimestamps;
			list->EndQuery(lightingTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
			LightingConstants constants{};
			if (camera.known) {
				constants.camera[0] = camera.pixel[0];
				constants.camera[1] = camera.pixel[1];
				constants.camera[2] = camera.depth;
				constants.camera[3] = 1.0f;
			}
			SunDirection(sun.azimuth, sun.elevation, constants.sun);
			SunDirection(0.0f, 45.0f, constants.sc4Sun);
			cGDriver12::WorldFrame frame{};
			bool const world = lightHooks != nullptr && camera.known && lightHooks->BeginLighting(frame);
			if (world) {
				memcpy(constants.pixelToWorld, frame.pixelToWorld, sizeof(constants.pixelToWorld));
				memcpy(constants.worldToView, frame.worldToView, sizeof(constants.worldToView));
				constants.world[0] = 1.0f;
				constants.world[1] = frame.metresPerPixel;
				if (frame.terrainSize != 0 && terrainField) {
					constants.terrain[0] = static_cast<float>(frame.terrainSize);
					constants.terrain[1] = frame.terrainScale;
					constants.terrain[2] = kTerrainTolerance;
				}
				if (frame.lightCount != 0 && frame.tileSize != 0) {
					constants.lights[0] = static_cast<float>(frame.lightCount);
					constants.lights[1] = static_cast<float>(frame.tileSize);
					constants.lights[2] = static_cast<float>(frame.tilesAcross);
					constants.lights[3] = static_cast<float>(frame.tileCount);
				}
				if (sun.fixedInWorld) {
					float direction[3];
					WorldSunDirection(sun.azimuth, sun.elevation, direction);
					for (int row = 0; row < 3; ++row) {
						float const *const m = frame.worldToView + row * 4;
						constants.sun[row] = m[0] * direction[0] + m[1] * direction[1] + m[2] * direction[2];
					}
				}
			}
			memcpy(constants.sunColor, sun.color, sizeof(sun.color));
			memcpy(constants.ambient, sun.ambient, sizeof(sun.ambient));
			constants.sc4Light[0] = kSC4Ambient;
			constants.sc4Light[1] = kSC4Direct;
			constants.sc4Light[2] = kLargestRelight;
			constants.march[0] = kShadowReach;
			constants.march[1] = kShadowReachPixels;
			constants.march[2] = kShadowBias;
			constants.march[3] = kShadowSoftness;
			constants.occluder[0] = kShadowThickness;
			constants.occluder[1] = kShadowBlurTolerance;
			Allocation const allocation = Upload(&constants, sizeof(constants), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
			if (allocation.cpu == nullptr) return;
			list->EndQuery(lightingTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 1);
			D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_RENDER_TARGET;
			D3D12_RESOURCE_STATES lightState = D3D12_RESOURCE_STATE_RENDER_TARGET;
			D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
			Transition(backBuffer.Get(), colorState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			Transition(lightBuffer.Get(), lightState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			Transition(depthBuffer.Get(), depthState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			D3D12_VIEWPORT const full{0.0f, 0.0f, static_cast<float>(windowWidth), static_cast<float>(windowHeight), 0.0f, 1.0f};
			D3D12_RECT const all{0, 0, windowWidth, windowHeight};
			list->RSSetViewports(1, &full);
			list->RSSetScissorRects(1, &all);
			// Without night lights the light buffers are never read, but the root views still want an address.
			D3D12_GPU_VIRTUAL_ADDRESS const lights = world && frame.lightCount != 0 ? frame.lights : allocation.gpu;
			D3D12_GPU_VIRTUAL_ADDRESS const tiles = world && frame.lightCount != 0 ? frame.tiles : allocation.gpu;
			auto const bindLighting = [&] {
				list->SetGraphicsRootSignature(lightingRootSignature.Get());
				list->SetGraphicsRootConstantBufferView(0, allocation.gpu);
				list->SetGraphicsRootDescriptorTable(1, ViewGpu(kLightingViews));
				list->SetGraphicsRootShaderResourceView(2, lights);
				list->SetGraphicsRootShaderResourceView(3, tiles);
				list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			};
			bindLighting();
			D3D12_CPU_DESCRIPTOR_HANDLE const sunlight = RenderTargetView(3);
			list->OMSetRenderTargets(1, &sunlight, FALSE, nullptr);
			if (world && lightHooks->TraceSunlight(allocation.gpu, ViewGpu(kLightingViews))) {
				bindLighting();
			} else {
				list->SetPipelineState(tracePipeline.Get());
				list->DrawInstanced(3, 1, 0, 0);
			}
			list->EndQuery(lightingTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 2);
			D3D12_RESOURCE_STATES sunlightState = D3D12_RESOURCE_STATE_RENDER_TARGET;
			Transition(sunlightBuffer.Get(), sunlightState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			D3D12_CPU_DESCRIPTOR_HANDLE const output = RenderTargetView(2);
			list->OMSetRenderTargets(1, &output, FALSE, nullptr);
			list->SetPipelineState(lightingPipeline.Get());
			list->DrawInstanced(3, 1, 0, 0);
			Transition(sunlightBuffer.Get(), sunlightState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			Transition(backBuffer.Get(), colorState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			Transition(lightBuffer.Get(), lightState, D3D12_RESOURCE_STATE_RENDER_TARGET);
			Transition(depthBuffer.Get(), depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			list->EndQuery(lightingTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query + 3);
			list->ResolveQueryData(lightingTimestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, kLightingTimestamps,
			                       lightingTimestampReadback.Get(), query * sizeof(uint64_t));
			frames[frameIndex].timed = true;
			frames[frameIndex].cityFrame = cityDrawn;
			cityDrawn = false;
		}

		// The shaders that also write the light layer, and the lighting pass with its root signature,
		// pipeline and timestamps.
		template<typename Compile>
		bool CreateLighting(Compile const &compile, UINT compileFlags) {
			nextView = kLightingViews + kLightingViewCount;
			for (uint32_t kind = 0; kind < 2; ++kind) {
				for (uint32_t blended = 0; blended < 2; ++blended) {
					D3D_SHADER_MACRO const macros[] = {
						{"SCD3D12_GBUFFER", blended ? "2" : "1"},
						{kind == 1 ? "SCD3D11_INTERPOLATION" : nullptr, "nointerpolation"},
						{nullptr, nullptr}
					};
					if (!compile(kShaderSource, sizeof(kShaderSource) - 1, macros, "PSMain", "ps_5_0", compileFlags,
					             gbufferPixelShaders[kind][blended])) return false;
				}
			}
			D3D_SHADER_MACRO const blitMacros[] = {{"SCD3D12_GBUFFER", "1"}, {nullptr, nullptr}};
			ComPtr<ID3DBlob> lightingVertexShader, lightingPixelShader, tracePixelShader;
			if (!compile(kBlitShaderSource, sizeof(kBlitShaderSource) - 1, blitMacros, "PSMain", "ps_5_0",
			             D3DCOMPILE_ENABLE_STRICTNESS, blitGBufferPixelShader) ||
			    !compile(kLightingShaderSource, sizeof(kLightingShaderSource) - 1, nullptr, "VSMain", "vs_5_0",
			             D3DCOMPILE_ENABLE_STRICTNESS, lightingVertexShader) ||
			    !compile(kLightingShaderSource, sizeof(kLightingShaderSource) - 1, nullptr, "PSMain", "ps_5_0",
			             D3DCOMPILE_ENABLE_STRICTNESS, lightingPixelShader) ||
			    !compile(kLightingShaderSource, sizeof(kLightingShaderSource) - 1, nullptr, "TracePS", "ps_5_0",
			             D3DCOMPILE_ENABLE_STRICTNESS, tracePixelShader)) {
				return false;
			}

			// The constants, the colour, light layer, depth, sunlight and terrain field views, then the night lights
			// and their tiles.
			D3D12_DESCRIPTOR_RANGE const views{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kLightingViewCount, 0, 0, 0};
			D3D12_ROOT_PARAMETER parameters[4]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable = {1, &views};
			parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			for (uint32_t i = 0; i < 2; ++i) {
				parameters[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
				parameters[2 + i].Descriptor = {kLightingViewCount + i, 0};
				parameters[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			}
			D3D12_ROOT_SIGNATURE_DESC const signature{4, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
			ComPtr<ID3DBlob> serialized, errors;
			if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
			    FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
			                                       IID_PPV_ARGS(&lightingRootSignature)))) {
				return false;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
			description.pRootSignature = lightingRootSignature.Get();
			description.VS = {lightingVertexShader->GetBufferPointer(), lightingVertexShader->GetBufferSize()};
			description.PS = {lightingPixelShader->GetBufferPointer(), lightingPixelShader->GetBufferSize()};
			description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			description.SampleMask = UINT_MAX;
			description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			description.RasterizerState.DepthClipEnable = TRUE;
			description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			description.NumRenderTargets = 1;
			description.RTVFormats[0] = kColorFormat;
			description.SampleDesc.Count = 1;
			if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&lightingPipeline)))) return false;
			description.PS = {tracePixelShader->GetBufferPointer(), tracePixelShader->GetBufferSize()};
			description.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
			if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&tracePipeline)))) return false;

			D3D12_QUERY_HEAP_DESC const queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrameCount * kLightingTimestamps, 0};
			if (FAILED(device->CreateQueryHeap(&queries, IID_PPV_ARGS(&lightingTimestamps)))) return false;
			D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_READBACK};
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = kFrameCount * kLightingTimestamps * sizeof(uint64_t);
			buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
			                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                           IID_PPV_ARGS(&lightingTimestampReadback)))) return false;
			return SUCCEEDED(queue->GetTimestampFrequency(&timestampFrequency));
		}

		// New light layer and relit frame for a new back buffer, and the lighting pass's views of them.
		bool CreateLightingTargets(uint32_t width, uint32_t height) {
			D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC description =
				TextureDescription(kColorFormat, width, height, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
			D3D12_CLEAR_VALUE clear{kColorFormat, {0, 0, 0, 0}};
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
			                                           IID_PPV_ARGS(&lightBuffer))) ||
			    FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
			                                           IID_PPV_ARGS(&litBuffer)))) {
				return false;
			}
			device->CreateRenderTargetView(lightBuffer.Get(), nullptr, RenderTargetView(1));
			device->CreateRenderTargetView(litBuffer.Get(), nullptr, RenderTargetView(2));
			// A new layer starts as no city at all, so a frame drawn before the first clear passes through.
			list->ClearRenderTargetView(RenderTargetView(1), clear.Color, 0, nullptr);
			description.Format = clear.Format = DXGI_FORMAT_R8_UNORM;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
			                                           IID_PPV_ARGS(&sunlightBuffer)))) {
				return false;
			}
			device->CreateRenderTargetView(sunlightBuffer.Get(), nullptr, RenderTargetView(3));
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			view.Format = kColorFormat;
			device->CreateShaderResourceView(backBuffer.Get(), &view, ViewCpu(kLightingViews));
			device->CreateShaderResourceView(lightBuffer.Get(), &view, ViewCpu(kLightingViews + 1));
			view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			device->CreateShaderResourceView(depthBuffer.Get(), &view, ViewCpu(kLightingViews + 2));
			view.Format = DXGI_FORMAT_R8_UNORM;
			device->CreateShaderResourceView(sunlightBuffer.Get(), &view, ViewCpu(kLightingViews + 3));
			TerrainFieldView();
			return true;
		}

		// The lighting pass's view of the terrain field (float4 per vertex), or an empty one.
		void TerrainFieldView() {
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Buffer.StructureByteStride = 16;
			view.Buffer.NumElements = terrainField ? static_cast<UINT>(terrainField->GetDesc().Width / 16) : 1;
			device->CreateShaderResourceView(terrainField.Get(), &view, ViewCpu(kLightingViews + 4));
		}
	};

	cGDriver12::cGDriver12() : impl(std::make_unique<Impl>()) {}
	cGDriver12::~cGDriver12() = default;

	bool cGDriver12::Create(bool useWarp, LUID const *adapterLuid) {
		Impl &d = *impl;
#ifndef NDEBUG
		ComPtr<ID3D12Debug> debug;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif
		if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&d.factory)))) return false;
		ComPtr<IDXGIAdapter1> adapter;
		if (adapterLuid != nullptr) d.factory->EnumAdapterByLuid(*adapterLuid, IID_PPV_ARGS(&adapter));
		else if (useWarp) d.factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
		else d.factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
		if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d.device)))) return false;
#ifndef NDEBUG
		ComPtr<ID3D12InfoQueue> info;
		if (SUCCEEDED(d.device.As(&info))) {
			// The debug layer says so when a clear value differs from the resource's optimized one. Clears
			// follow the game's colour, so that is expected.
			D3D12_MESSAGE_ID hidden[] = {
				D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
				D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
			};
			D3D12_INFO_QUEUE_FILTER filter{};
			filter.DenyList.NumIDs = static_cast<UINT>(std::size(hidden));
			filter.DenyList.pIDList = hidden;
			info->AddStorageFilterEntries(&filter);
		}
#endif

		D3D12_COMMAND_QUEUE_DESC queue{};
		queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(d.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&d.queue)))) return false;
		for (Impl::FrameSlot &slot: d.frames) {
			if (FAILED(d.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
			                                            IID_PPV_ARGS(&slot.allocator)))) return false;
		}
		if (FAILED(d.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d.frames[0].allocator.Get(), nullptr,
		                                       IID_PPV_ARGS(&d.list)))) return false;
		d.list->Close();
		if (FAILED(d.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d.fence)))) return false;
		d.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (d.fenceEvent == nullptr) return false;

		D3D12_DESCRIPTOR_HEAP_DESC heap{};
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		heap.NumDescriptors = d.relight ? 4 : 1;
		if (FAILED(d.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&d.rtvHeap)))) return false;
		d.rtvIncrement = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		heap.NumDescriptors = 1;
		if (FAILED(d.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&d.dsvHeap)))) return false;
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heap.NumDescriptors = kViewSlots;
		heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(d.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&d.viewHeap)))) return false;
		heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		heap.NumDescriptors = kBlitSampler + 1;
		if (FAILED(d.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&d.samplerHeap)))) return false;
		d.viewIncrement = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		d.samplerIncrement = d.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

		// Slot 0: an empty view, bound to stages without a texture.
		D3D12_SHADER_RESOURCE_VIEW_DESC empty{};
		empty.Format = kColorFormat;
		empty.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		empty.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		empty.Texture2D.MipLevels = 1;
		d.device->CreateShaderResourceView(nullptr, &empty, d.ViewCpu(0));

		// Every sampler cGDriver::EnsureSampler can make, then the blit's.
		D3D12_CPU_DESCRIPTOR_HANDLE sampler = d.samplerHeap->GetCPUDescriptorHandleForHeapStart();
		for (uint32_t index = 0; index <= kBlitSampler; ++index, sampler.ptr += d.samplerIncrement) {
			D3D12_SAMPLER_DESC description{};
			if (index == kBlitSampler) {
				description.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
				description.AddressU = description.AddressV = description.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			} else {
				auto const type = [](bool linear) { return linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT; };
				description.Filter = D3D12_ENCODE_BASIC_FILTER(type(index & 1), type(index & 2), type(index & 4),
				                                               D3D12_FILTER_REDUCTION_TYPE_STANDARD);
				description.AddressU = (index & 8) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				description.AddressV = (index & 16) ? D3D12_TEXTURE_ADDRESS_MODE_CLAMP : D3D12_TEXTURE_ADDRESS_MODE_WRAP;
				description.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			}
			description.MaxAnisotropy = 1;
			description.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			description.MaxLOD = FLT_MAX;
			d.device->CreateSampler(&description, sampler);
		}

		// Root signature: the constants, then a view and a sampler table per texture stage.
		D3D12_DESCRIPTOR_RANGE const ranges[] = {
			{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
			{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, 0},
			{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0},
			{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 1, 0, 0},
		};
		D3D12_ROOT_PARAMETER parameters[6]{};
		parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		parameters[0].Descriptor = {0, 0};
		parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		for (uint32_t i = 0; i < 4; ++i) {
			parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1 + i].DescriptorTable = {1, &ranges[i]};
			parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}
		// The owned scene's frame projection, for VSInstanced.
		parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		parameters[5].Constants = {1, 0, 16};
		parameters[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		D3D12_ROOT_SIGNATURE_DESC const signature{
			6, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
		};
		ComPtr<ID3DBlob> serialized, errors;
		if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
		    FAILED(d.device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
		                                         IID_PPV_ARGS(&d.rootSignature)))) {
			return false;
		}

		// The same compiler flags as cGDriver::CreateGeometryPipeline.
		UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
		compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
		auto const compile = [&](char const *source, size_t size, D3D_SHADER_MACRO const *macros, char const *entry,
		                         char const *target, UINT flags, ComPtr<ID3DBlob> &bytecode) {
			ComPtr<ID3DBlob> messages;
			HRESULT const result = D3DCompile(source, size, "SCD3D12", macros, nullptr, entry, target, flags, 0,
			                                  &bytecode, &messages);
			if (FAILED(result) && messages) std::fprintf(stderr, "%s\n", static_cast<char const *>(messages->GetBufferPointer()));
			return SUCCEEDED(result);
		};
		D3D_SHADER_MACRO const flatMacros[] = {{"SCD3D11_INTERPOLATION", "nointerpolation"}, {nullptr, nullptr}};
		D3D_SHADER_MACRO const instancedMacros[2][3] = {
			{{"SCD3D12_INSTANCED", "1"}, {nullptr, nullptr}, {nullptr, nullptr}},
			{{"SCD3D12_INSTANCED", "1"}, {"SCD3D11_INTERPOLATION", "nointerpolation"}, {nullptr, nullptr}},
		};
		if (!compile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, "VSMain", "vs_5_0", compileFlags, d.vertexShader) ||
		    !compile(kShaderSource, sizeof(kShaderSource) - 1, nullptr, "PSMain", "ps_5_0", compileFlags, d.pixelShader) ||
		    !compile(kShaderSource, sizeof(kShaderSource) - 1, flatMacros, "PSMain", "ps_5_0", compileFlags,
		             d.flatPixelShader) ||
		    // Compiled with the same modes as its pixel shader, so their registers line up.
		    !compile(kShaderSource, sizeof(kShaderSource) - 1, flatMacros, "VSMain", "vs_5_0", compileFlags,
		             d.flatVertexShader) ||
		    !compile(kShaderSource, sizeof(kShaderSource) - 1, instancedMacros[0], "VSInstanced", "vs_5_0", compileFlags,
		             d.instancedVertexShaders[0]) ||
		    !compile(kShaderSource, sizeof(kShaderSource) - 1, instancedMacros[1], "VSInstanced", "vs_5_0", compileFlags,
		             d.instancedVertexShaders[1]) ||
		    !compile(kBlitShaderSource, sizeof(kBlitShaderSource) - 1, nullptr, "VSMain", "vs_5_0",
		             D3DCOMPILE_ENABLE_STRICTNESS, d.blitVertexShader) ||
		    !compile(kBlitShaderSource, sizeof(kBlitShaderSource) - 1, nullptr, "PSMain", "ps_5_0",
		             D3DCOMPILE_ENABLE_STRICTNESS, d.blitPixelShader)) {
			return false;
		}
		if (d.relight && !d.CreateLighting(compile, compileFlags)) return false;

		// Frame timing is optional: without it the server still draws.
		UINT64 frequency = 0;
		D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kFrameCount * 2, 0};
		D3D12_HEAP_PROPERTIES const readbackHeap{D3D12_HEAP_TYPE_READBACK};
		D3D12_RESOURCE_DESC readback{};
		readback.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		readback.Width = kFrameCount * 2 * sizeof(uint64_t);
		readback.Height = readback.DepthOrArraySize = readback.MipLevels = 1;
		readback.SampleDesc.Count = 1;
		readback.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		if (SUCCEEDED(d.queue->GetTimestampFrequency(&frequency)) && frequency != 0 &&
		    SUCCEEDED(d.device->CreateQueryHeap(&queries, IID_PPV_ARGS(&d.timestamps))) &&
		    SUCCEEDED(d.device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &readback,
		                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		                                                IID_PPV_ARGS(&d.timestampReadback))) &&
		    SUCCEEDED(d.timestampReadback->Map(0, nullptr, reinterpret_cast<void **>(&d.timestampValues)))) {
			d.timestampMilliseconds = 1000.0 / static_cast<double>(frequency);
		} else {
			d.timestamps.Reset();
		}

		d.BeginFrame(0);
		return true;
	}

	ID3D12Device *cGDriver12::Device() const { return impl->device.Get(); }

	void cGDriver12::TakeGpuTime(double &milliseconds, uint32_t &frames) {
		milliseconds = std::exchange(impl->gpuMilliseconds, 0.0);
		frames = std::exchange(impl->gpuFrames, 0);
	}
	ID3D12CommandQueue *cGDriver12::Queue() const { return impl->queue.Get(); }

	// cGDriver::SetVideoMode: new targets of the mode's size, and the textures and buffer regions of the
	// old device are gone.
	void cGDriver12::VideoMode(bool, uint32_t width, uint32_t height, bool) {
		Impl &d = *impl;
		if (!d.device || width == 0 || height == 0) return;
		// The lighting pass's views are rewritten below while earlier frames may still read them.
		if (d.relight) d.WaitIdle();
		d.Retire(d.backBuffer);
		d.Retire(d.lightBuffer);
		d.Retire(d.litBuffer);
		d.Retire(d.sunlightBuffer);
		d.Retire(d.depthBuffer);
		d.Retire(d.depthScratch);
		d.depthScratchValid = false;
		// A new video mode starts over, as SC4 expects. A resize keeps textures and region numbers.
		if (!d.resizing) {
			for (Impl::Region &region: d.regions) d.RetireRegion(region);
			d.regionFlags = 0;
			for (auto &entry: d.textures) d.ReleaseTexture(entry.second);
			d.textures.clear();
			d.boundTextures[0] = d.boundTextures[1] = 0;
			d.ReleaseTexture(d.blitTexture);
			d.blitTexture = {};
		}

		D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
		D3D12_RESOURCE_DESC description = TextureDescription(kColorFormat, width, height, 1,
		                                                     D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
		D3D12_CLEAR_VALUE const clear{kColorFormat, {0, 0, 0, 1}};
		if (FAILED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
		                                              D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
		                                              IID_PPV_ARGS(&d.backBuffer)))) return;
		d.device->CreateRenderTargetView(d.backBuffer.Get(), nullptr, d.RenderTargetView());
		description = TextureDescription(kDepthTypeless, width, height, 1, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		D3D12_CLEAR_VALUE depthClear{kDepthFormat};
		depthClear.DepthStencil = {1.0f, 0};
		if (FAILED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
		                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthClear,
		                                              IID_PPV_ARGS(&d.depthBuffer)))) {
			d.backBuffer.Reset();
			return;
		}
		D3D12_DEPTH_STENCIL_VIEW_DESC depthView{};
		depthView.Format = kDepthFormat;
		depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		d.device->CreateDepthStencilView(d.depthBuffer.Get(), &depthView, d.DepthStencilView());
		if (d.relight && !d.CreateLightingTargets(width, height)) {
			d.backBuffer.Reset();
			return;
		}
		d.windowWidth = static_cast<int32_t>(width);
		d.windowHeight = static_cast<int32_t>(height);
		if (d.resizing) {
			for (uint32_t index = 0; index < kRegionCount; ++index) {
				if ((d.regionFlags & (1u << index)) != 0) d.CreateRegion(index, d.regions[index].type);
			}
		}
		d.BindTargets();
		d.SetFullViewport();
	}

	void cGDriver12::Resize(uint32_t width, uint32_t height) {
		impl->resizing = true;
		VideoMode(false, width, height, false);
		impl->resizing = false;
	}

	// The vertices the next DrawPrims or DrawPrimsIndexed reads, as cGDriverReplayHooks::Vertices hands
	// them to cGDriver through GetVertices.
	void cGDriver12::Vertices(uint32_t name, void const *data, uint32_t bytes) {
		Impl &d = *impl;
		uint32_t const stride = RZVertexFormatStride(VertexBufferType(name));
		if (stride == 0 || bytes == 0 || bytes % stride != 0) return;
		d.extensionVerticesValid = bytes / stride <= kExtensionCapacity;
		if (!d.extensionVerticesValid) return;
		auto const *source = static_cast<uint8_t const *>(data);
		d.extensionVertices.assign(source, source + bytes);
	}

	bool cGDriver12::Frame() { return true; }

	void cGDriver12::OwnerBegin(nSCDWire::DrawOwner const &owner) {
		Impl &d = *impl;
		d.ownersSeen = true;
		++d.ownerDepth;
		d.ownerPass = owner.pass;
		d.ownerDrawsShadows = owner.vtable == kShadowViewObjectVtable;
	}
	void cGDriver12::OwnerEnd() {
		Impl &d = *impl;
		if (d.ownerDepth > 0) --d.ownerDepth;
	}
	void cGDriver12::SwapCamera(float const *fromView, float const *fromProjection, float const *view,
	                            float const *projection) {
		Impl &d = *impl;
		d.swapCamera = fromView != nullptr;
		if (!d.swapCamera) return;
		float const *const from = fromView;
		memcpy(d.swapFrom, fromProjection, sizeof(d.swapFrom));
		memcpy(d.swapProjection, projection, sizeof(d.swapProjection));
		// view x inverse(from), with from a rotation and a translation.
		float inverse[16]{};
		for (int row = 0; row < 3; ++row) {
			for (int c = 0; c < 3; ++c) inverse[c * 4 + row] = from[row * 4 + c];
			inverse[12 + row] = -(from[row * 4] * from[12] + from[row * 4 + 1] * from[13] + from[row * 4 + 2] * from[14]);
		}
		inverse[15] = 1.0f;
		for (int c = 0; c < 4; ++c) {
			for (int row = 0; row < 4; ++row) {
				d.swapChange[c * 4 + row] = view[row] * inverse[c * 4] + view[4 + row] * inverse[c * 4 + 1] +
				                            view[8 + row] * inverse[c * 4 + 2] + view[12 + row] * inverse[c * 4 + 3];
			}
		}
	}

	void cGDriver12::Relight(Sun const &sun) {
		impl->relight = true;
		impl->sun = sun;
	}

	ID3D12Resource *cGDriver12::Output() const {
		return impl->relight ? impl->litBuffer.Get() : impl->backBuffer.Get();
	}

	bool cGDriver12::ReadBack(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height, ID3D12Resource *texture) {
		Impl &d = *impl;
		ID3D12Resource *const output = texture != nullptr ? texture : Output();
		if (output == nullptr) return false;
		D3D12_RESOURCE_DESC const description = output->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
		UINT64 total = 0;
		d.device->GetCopyableFootprints(&description, 0, 1, 0, &footprint, nullptr, nullptr, &total);
		D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = total;
		buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> readback;
		if (FAILED(d.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
		                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
		                                              IID_PPV_ARGS(&readback)))) return false;
		D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
		d.Transition(output, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
		destination.PlacedFootprint = footprint;
		D3D12_TEXTURE_COPY_LOCATION source{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
		d.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
		d.Transition(output, state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		if (!d.Submit()) return false;
		d.WaitIdle();

		width = static_cast<uint32_t>(description.Width);
		height = description.Height;
		rgba.resize(static_cast<size_t>(width) * height * 4);
		uint8_t *mapped = nullptr;
		if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)))) return false;
		for (uint32_t y = 0; y < height; ++y) {
			std::memcpy(rgba.data() + static_cast<size_t>(y) * width * 4,
			            mapped + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch,
			            static_cast<size_t>(width) * 4);
		}
		readback->Unmap(0, nullptr);
		return true;
	}

	bool cGDriver12::QueryInterface(uint32_t riid, void **ppvObj) {
		if (ppvObj == nullptr) return false;
		switch (riid) {
			case GZIID_cIGZUnknown:
			case GZIID_cIGZGDriver: *ppvObj = static_cast<cIGZGDriver *>(this); return true;
			case GZIID_cIGZGBufferRegionExtension: *ppvObj = static_cast<cIGZGBufferRegionExtension *>(this); return true;
			case GZIID_cIGZGDriverLightingExtension: *ppvObj = static_cast<cIGZGDriverLightingExtension *>(this); return true;
			case GZIID_cIGZGDriverVertexBufferExtension:
				*ppvObj = static_cast<cIGZGDriverVertexBufferExtension *>(this);
				return true;
			case GZIID_cIGZGSnapshotExtension: *ppvObj = static_cast<cIGZGSnapshotExtension *>(this); return true;
			default: *ppvObj = nullptr; return false;
		}
	}
	uint32_t cGDriver12::AddRef(void) { return 1; }
	uint32_t cGDriver12::Release(void) { return 1; }

	// Drawing

	void cGDriver12::InterleavedArrays(uint32_t format, int32_t stride, void const *pointer) {
		Impl &d = *impl;
		uint32_t const packedStride = RZVertexFormatStride(format);
		if (!IsSupportedVertexFormat(format) || pointer == nullptr || stride < 0 ||
		    (stride != 0 && static_cast<uint32_t>(stride) < packedStride)) {
			return;
		}
		d.interleavedFormat = format;
		d.interleavedStride = stride == 0 ? packedStride : static_cast<uint32_t>(stride);
		d.interleavedPointer = static_cast<uint8_t const *>(pointer);
	}

	void cGDriver12::DrawBlobs(nSCDWire::BlobKey const &vertices, nSCDWire::BlobKey const &indices) {
		impl->drawVertices = vertices;
		impl->drawIndices = indices;
	}

	void cGDriver12::DrawArrays(uint32_t primitive, int32_t first, int32_t count) {
		Impl &d = *impl;
		nSCDWire::BlobKey const blob = std::exchange(d.drawVertices, {});
		d.drawIndices = {};
		if (first < 0 || count <= 0 || !d.backBuffer) return;
		if (d.drawFilter != nullptr && !d.drawFilter->AllowDraw(primitive, first, static_cast<uint32_t>(count), 0, nullptr)) {
			return;
		}
		if (d.capture != nullptr && d.interleavedPointer != nullptr &&
		    d.CaptureDraw(d.interleavedPointer + static_cast<size_t>(first) * d.interleavedStride, d.interleavedFormat,
		                  d.interleavedStride, static_cast<uint32_t>(count), primitive, nullptr, 0, 0)) {
			return;
		}
		if (!d.UploadVertices(d.interleavedPointer, d.interleavedFormat, d.interleavedStride,
		                      static_cast<uint32_t>(first), static_cast<uint32_t>(count), blob)) return;
		if (D3D11Topology(primitive) != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) {
			if (d.BindGeometry(primitive, 0)) d.list->DrawInstanced(static_cast<UINT>(count), 1, 0, 0);
			return;
		}
		// The same primitive and count always give the same indices.
		CacheKey const key{0, 0, 0, CacheKind::SequentialIndices, primitive, 0, 0,
		                         static_cast<uint32_t>(count)};
		if (CacheEntry const *cached = d.FindCached(key)) {
			d.BindIndices(d.CachedAddress(*cached), cached->indexCount, true);
			if (d.BindGeometry(0, 32)) d.list->DrawIndexedInstanced(cached->indexCount, 1, 0, 0, 0);
			return;
		}
		if (!BuildSequentialIndices(primitive, static_cast<uint32_t>(count), d.drawIndexScratch)) return;
		if (!d.UploadIndices(d.drawIndexScratch.data(), static_cast<uint32_t>(d.drawIndexScratch.size()), true,
		                     &key) ||
		    !d.BindGeometry(0, 32)) return;
		d.list->DrawIndexedInstanced(static_cast<UINT>(d.drawIndexScratch.size()), 1, 0, 0, 0);
	}

	void cGDriver12::DrawElements(uint32_t primitive, int32_t count, uint32_t type, void const *indices) {
		Impl &d = *impl;
		nSCDWire::BlobKey const vertexBlob = std::exchange(d.drawVertices, {});
		nSCDWire::BlobKey const indexBlob = std::exchange(d.drawIndices, {});
		if (count <= 0 || indices == nullptr || (type != 3 && type != 5) || !d.backBuffer) return;
		if (d.drawFilter != nullptr && !d.drawFilter->AllowDraw(primitive, 0, static_cast<uint32_t>(count), type, indices)) {
			return;
		}
		if (d.capture != nullptr && d.CaptureDraw(d.interleavedPointer, d.interleavedFormat, d.interleavedStride, 0,
		                                           primitive, indices, static_cast<uint32_t>(count), type)) {
			return;
		}
		bool const convertPrimitive = D3D11Topology(primitive) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		CacheKey const key{
			indexBlob.low, indexBlob.high, indexBlob.size,
			convertPrimitive ? CacheKind::ConvertedIndices : CacheKind::Indices,
			convertPrimitive ? primitive : type, 0, 0, static_cast<uint32_t>(count)
		};
		CacheEntry const *cached = indexBlob.size != 0 ? d.FindCached(key) : nullptr;
		uint32_t minimumIndex = UINT32_MAX;
		uint32_t maximumIndex = 0;
		if (cached != nullptr) {
			minimumIndex = cached->minimumIndex;
			maximumIndex = cached->maximumIndex;
		} else {
			auto const scan = [&](auto const *source) {
				for (int32_t i = 0; i < count; ++i) {
					minimumIndex = (std::min)(minimumIndex, static_cast<uint32_t>(source[i]));
					maximumIndex = (std::max)(maximumIndex, static_cast<uint32_t>(source[i]));
				}
				if (convertPrimitive) d.sourceIndexScratch.assign(source, source + count);
			};
			if (type == 3) scan(static_cast<uint16_t const *>(indices));
			else scan(static_cast<uint32_t const *>(indices));
			if (convertPrimitive && !ConvertPrimitiveIndices(primitive, d.sourceIndexScratch, d.drawIndexScratch)) {
				return;
			}
		}
		if (maximumIndex == UINT32_MAX || minimumIndex > INT32_MAX) return;
		if (!d.UploadVertices(d.interleavedPointer, d.interleavedFormat, d.interleavedStride, minimumIndex,
		                      maximumIndex - minimumIndex + 1, vertexBlob)) return;
		INT const baseVertex = -static_cast<INT>(minimumIndex);
		bool const wide = convertPrimitive || type == 5;
		uint32_t drawCount = 0;
		if (cached != nullptr) {
			// Still there: a segment this frame reads is never reused within the frame.
			drawCount = cached->indexCount;
			d.BindIndices(d.CachedAddress(*cached), drawCount, wide);
		} else {
			void const *source = convertPrimitive ? d.drawIndexScratch.data() : indices;
			drawCount = static_cast<uint32_t>(convertPrimitive ? d.drawIndexScratch.size() : count);
			if (!d.UploadIndices(source, drawCount, wide, indexBlob.size != 0 ? &key : nullptr, minimumIndex,
			                     maximumIndex)) return;
		}
		if (!d.BindGeometry(convertPrimitive ? 0 : primitive, wide ? 32 : 16)) return;
		d.list->DrawIndexedInstanced(drawCount, 1, 0, baseVertex, 0);
	}

	uint32_t cGDriver12::MakeVertexFormat(uint32_t, intptr_t) { return UINT_MAX; }
	uint32_t cGDriver12::MakeVertexFormat(uint32_t format) { return RZMakeVertexFormat(format); }
	uint32_t cGDriver12::VertexFormatStride(uint32_t format) { return RZVertexFormatStride(format); }
	uint32_t cGDriver12::VertexFormatElementOffset(uint32_t format, uint32_t element, uint32_t index) {
		return RZVertexFormatElementOffset(format, element, index);
	}
	uint32_t cGDriver12::VertexFormatNumElements(uint32_t format, uint32_t element) {
		return RZVertexFormatNumElements(format, element);
	}

	// Clears ignore the scissor, as in Direct3D 11.
	void cGDriver12::Clear(uint32_t mask) {
		Impl &d = *impl;
		if (!d.backBuffer) return;
		if (ClearsColor(mask)) {
			d.list->ClearRenderTargetView(d.RenderTargetView(), d.clearColor, 0, nullptr);
			float const noCity[4]{};
			if (d.relight) d.list->ClearRenderTargetView(d.RenderTargetView(1), noCity, 0, nullptr);
		}
		D3D12_CLEAR_FLAGS const flags = static_cast<D3D12_CLEAR_FLAGS>(
			((mask & 0x1000) != 0 ? D3D12_CLEAR_FLAG_DEPTH : 0) | ((mask & 0x2000) != 0 ? D3D12_CLEAR_FLAG_STENCIL : 0));
		if (flags != 0) {
			d.list->ClearDepthStencilView(d.DepthStencilView(), flags, d.clearDepth, d.clearStencil, 0, nullptr);
			d.depthScratchValid = false;
		}
	}
	void cGDriver12::ClearColor(float r, float g, float b, float a) {
		Impl &d = *impl;
		d.clearColor[0] = r;
		d.clearColor[1] = g;
		d.clearColor[2] = b;
		d.clearColor[3] = a;
	}
	void cGDriver12::ClearDepth(double depth) {
		impl->clearDepth = static_cast<float>(depth < 0.0 ? 0.0 : (depth > 1.0 ? 1.0 : depth));
	}
	void cGDriver12::ClearStencil(int32_t stencil) { impl->clearStencil = static_cast<uint8_t>(stencil); }

	// Render state

	void cGDriver12::ColorMask(bool enabled) { impl->colorWriteEnabled = enabled; }
	void cGDriver12::DepthFunc(uint32_t function) {
		if (D3D11Comparison(function) != 0) impl->depthFunction = static_cast<uint8_t>(function);
	}
	void cGDriver12::DepthMask(bool enabled) { impl->depthWriteEnabled = enabled; }
	void cGDriver12::StencilFunc(uint32_t function, int32_t reference, uint32_t mask) {
		Impl &d = *impl;
		if (D3D11Comparison(function) == 0) return;
		d.stencilFunction = static_cast<uint8_t>(function);
		d.stencilReference = reference;
		d.stencilReadMask = static_cast<uint8_t>(mask);
	}
	void cGDriver12::StencilMask(uint32_t mask) { impl->stencilWriteMask = static_cast<uint8_t>(mask); }
	void cGDriver12::StencilOp(uint32_t fail, uint32_t depthFail, uint32_t pass) {
		Impl &d = *impl;
		if (D3D11StencilOperation(fail) == 0 || D3D11StencilOperation(depthFail) == 0 ||
		    D3D11StencilOperation(pass) == 0) {
			return;
		}
		d.stencilFailOperation = static_cast<uint8_t>(fail);
		d.stencilDepthFailOperation = static_cast<uint8_t>(depthFail);
		d.stencilPassOperation = static_cast<uint8_t>(pass);
	}
	void cGDriver12::BlendFunc(uint32_t source, uint32_t destination) {
		Impl &d = *impl;
		if (!IsValidBlendFunction(source, destination)) return;
		d.sourceBlend = static_cast<uint8_t>(source);
		d.destinationBlend = static_cast<uint8_t>(destination);
	}
	void cGDriver12::AlphaFunc(uint32_t function, float reference) {
		Impl &d = *impl;
		if (D3D11Comparison(function) == 0) return;
		if (d.alphaFunction == function && memcmp(&d.alphaReference, &reference, sizeof(reference)) == 0) return;
		d.constantsDirty = true;
		d.alphaFunction = static_cast<uint8_t>(function);
		d.alphaReference = reference;
	}
	void cGDriver12::ShadeModel(uint32_t mode) {
		if (mode <= 1) impl->shadeModel = static_cast<uint8_t>(mode);
	}
	void cGDriver12::PolygonOffset(int32_t offset) { impl->polygonOffset = offset; }

	void cGDriver12::Fog(uint32_t parameter, uint32_t value) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (parameter == 0 && value <= 2) d.fogMode = static_cast<uint8_t>(value);
	}
	void cGDriver12::Fog(uint32_t parameter, float const *value) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (value == nullptr) return;
		switch (parameter) {
			case 1: memcpy(d.fogColor, value, sizeof(d.fogColor)); break;
			case 2: if (*value >= 0.0f) d.fogDensity = *value; break;
			case 3: d.fogStart = *value; break;
			case 4: d.fogEnd = *value; break;
			default: break;
		}
	}

	// SimGLDX7 turned the colour multiplier into D3D lighting with a white ambient material.
	void cGDriver12::ColorMultiplier(float r, float g, float b) {
		Impl &d = *impl;
		d.constantsDirty = true;
		d.lightingEnabled = true;
		d.materialAmbient[0] = d.materialAmbient[1] = d.materialAmbient[2] = 1.0f;
		LightModelAmbient(r, g, b, 1.0f);
	}
	void cGDriver12::AlphaMultiplier(float a) {
		Impl &d = *impl;
		d.constantsDirty = true;
		d.lightingEnabled = true;
		d.materialDiffuse[3] = a;
		if (a < 1.0f) d.diffuseFromVertex = false;
		else if (d.diffuseVertexColors) d.diffuseFromVertex = true;
	}
	void cGDriver12::EnableVertexColors(bool ambient, bool diffuse) {
		Impl &d = *impl;
		d.constantsDirty = true;
		d.ambientVertexColors = ambient;
		d.diffuseVertexColors = diffuse;
		d.diffuseFromVertex = diffuse;
	}

	void cGDriver12::MatrixMode(uint32_t mode) {
		if (mode < 2) impl->activeMatrixMode = static_cast<uint8_t>(mode);
	}
	void cGDriver12::LoadMatrix(float const *matrix) {
		Impl &d = *impl;
		float *const target = d.matrices[d.activeMatrixMode];
		if (matrix == nullptr || memcmp(target, matrix, sizeof(d.matrices[0])) == 0) return;
		memcpy(target, matrix, sizeof(d.matrices[0]));
		d.constantsDirty = true;
		if (d.activeMatrixMode == 0) d.normalMatrixDirty = true;
	}
	void cGDriver12::LoadIdentity(void) {
		float identity[16]{};
		identity[0] = identity[5] = identity[10] = identity[15] = 1.0f;
		LoadMatrix(identity);
	}

	void cGDriver12::Enable(uint32_t state) {
		Impl &d = *impl;
		if (state >= kGDNumCapabilities || state == kGDCapability_Unused0) return;
		bool &value = state == kGDCapability_Texture2D ? d.textureStageEnabled[d.activeTextureStage]
		                                               : d.enabledCapabilities[state];
		if (!value) d.constantsDirty = true;
		value = true;
	}
	void cGDriver12::Disable(uint32_t state) {
		Impl &d = *impl;
		if (state >= kGDNumCapabilities || state == kGDCapability_Unused0) return;
		bool &value = state == kGDCapability_Texture2D ? d.textureStageEnabled[d.activeTextureStage]
		                                               : d.enabledCapabilities[state];
		if (value) d.constantsDirty = true;
		value = false;
	}
	bool cGDriver12::IsEnabled(uint32_t state) {
		Impl &d = *impl;
		if (state >= kGDNumCapabilities || state == kGDCapability_Unused0) return false;
		return state == kGDCapability_Texture2D ? d.textureStageEnabled[d.activeTextureStage]
		                                        : d.enabledCapabilities[state];
	}
	void cGDriver12::GetBoolean(uint32_t parameter, bool *value) {
		if (parameter == 0 && value != nullptr) *value = impl->pixelStoreRowLength != 0;
	}
	void cGDriver12::GetInteger(uint32_t parameter, int32_t *value) {
		if (parameter == 0 && value != nullptr) *value = static_cast<int32_t>(impl->pixelStoreRowLength);
	}
	void cGDriver12::GetFloat(uint32_t parameter, float *value) {
		if (parameter == 0 && value != nullptr) *value = static_cast<float>(impl->pixelStoreRowLength);
	}
	// The game reads state back from the shim, which answers queries itself.
	uint32_t cGDriver12::GetError(void) { return 0; }

	// Textures

	void cGDriver12::GenTextures(int32_t count, uint32_t *names) {
		Impl &d = *impl;
		if (count < 0 || (count > 0 && names == nullptr)) return;
		for (int32_t i = 0; i < count; ++i) {
			while (d.nextTextureId == 0 || d.textures.find(d.nextTextureId) != d.textures.end()) ++d.nextTextureId;
			names[i] = d.nextTextureId;
			d.textures.emplace(d.nextTextureId++, Impl::Texture{});
		}
	}
	void cGDriver12::DeleteTextures(int32_t count, uint32_t const *names) {
		Impl &d = *impl;
		if (count < 0 || (count > 0 && names == nullptr)) return;
		for (int32_t i = 0; i < count; ++i) {
			for (uint32_t &bound: d.boundTextures) if (bound == names[i]) bound = 0;
			auto const found = d.textures.find(names[i]);
			if (found == d.textures.end()) continue;
			d.ReleaseTexture(found->second);
			d.textures.erase(found);
		}
	}
	bool cGDriver12::IsTexture(uint32_t name) {
		auto const found = impl->textures.find(name);
		return found != impl->textures.end() && found->second.resource;
	}
	void cGDriver12::PrioritizeTextures(int32_t, uint32_t const *, float const *) {}
	bool cGDriver12::AreTexturesResident(int32_t count, uint32_t const *names, bool *resident) {
		if (count < 0 || (count > 0 && (names == nullptr || resident == nullptr))) return false;
		bool all = true;
		for (int32_t i = 0; i < count; ++i) {
			resident[i] = IsTexture(names[i]);
			all &= resident[i];
		}
		return all;
	}

	void cGDriver12::BindTexture(uint32_t target, uint32_t texture) {
		Impl &d = *impl;
		if (target != 0 || (texture != 0 && d.textures.find(texture) == d.textures.end())) return;
		d.boundTextures[d.activeTextureStage] = texture;
	}

	void cGDriver12::TexImage2D(uint32_t target, int32_t level, int32_t internalFormat, int32_t width, int32_t height,
	                            int32_t border, uint32_t format, uint32_t type, void const *pixels) {
		Impl &d = *impl;
		uint32_t const name = d.boundTextures[d.activeTextureStage];
		auto const found = d.textures.find(name);
		if (target != 0 || level < 0 || border != 0 || width <= 0 || height <= 0 || found == d.textures.end()) return;
		Impl::Texture &texture = found->second;
		DXGI_FORMAT const requestedFormat = D3D11TextureFormat(static_cast<uint32_t>(internalFormat));
		if (level == 0) {
			if (!texture.resource || texture.width != static_cast<uint32_t>(width) ||
			    texture.height != static_cast<uint32_t>(height) || texture.format != requestedFormat) {
				if (!d.CreateTextureResource(texture, static_cast<uint32_t>(internalFormat), static_cast<uint32_t>(width),
				                             static_cast<uint32_t>(height), 1)) return;
			}
		} else {
			uint32_t const maximumLevels = texture.resource ? D3D11MipLevelCount(texture.width, texture.height) : 0;
			if (requestedFormat != texture.format || static_cast<uint32_t>(level) >= maximumLevels ||
			    static_cast<uint32_t>(width) != D3D11MipDimension(texture.width, static_cast<uint32_t>(level)) ||
			    static_cast<uint32_t>(height) != D3D11MipDimension(texture.height, static_cast<uint32_t>(level))) {
				return;
			}
			if (static_cast<uint32_t>(level) >= texture.levels) {
				// A level past the chain: a full chain, with the levels already there copied over.
				ComPtr<ID3D12Resource> const old = texture.resource;
				D3D12_RESOURCE_STATES oldState = texture.state;
				uint32_t const oldLevels = texture.levels;
				if (!d.CreateTextureResource(texture, static_cast<uint32_t>(internalFormat), texture.width,
				                             texture.height, maximumLevels)) return;
				d.Transition(old.Get(), oldState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				for (uint32_t oldLevel = 0; oldLevel < oldLevels; ++oldLevel) {
					D3D12_TEXTURE_COPY_LOCATION to{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
					to.SubresourceIndex = oldLevel;
					D3D12_TEXTURE_COPY_LOCATION from{old.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
					from.SubresourceIndex = oldLevel;
					d.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
				}
			}
		}
		if (pixels != nullptr) {
			LoadTextureLevel(name, level, 0, 0, width, height, format, type, d.pixelStoreRowLength, pixels);
		}
	}

	void cGDriver12::PixelStore(uint32_t parameter, int32_t value) {
		if (parameter == 0 && value >= 0) impl->pixelStoreRowLength = static_cast<uint32_t>(value);
	}

	void cGDriver12::TexEnv(uint32_t target, uint32_t parameter, int32_t value) {
		Impl &d = *impl;
		if (target != 0 || parameter != 0 || value < 0 || value > 5) return;
		if (d.textureStages[d.activeTextureStage].environmentMode == value) return;
		d.constantsDirty = true;
		d.textureStages[d.activeTextureStage].environmentMode = static_cast<uint8_t>(value);
	}
	void cGDriver12::TexEnv(uint32_t target, uint32_t parameter, float const *value) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (target != 0 || parameter != 1 || value == nullptr) return;
		// Shared by both stages, as D3DRS_TEXTUREFACTOR was.
		memcpy(d.textureFactor, value, sizeof(d.textureFactor));
	}
	void cGDriver12::TexParameter(uint32_t target, uint32_t parameter, int32_t value) {
		Impl &d = *impl;
		if (target != 0 || parameter >= 4 || value < 0 || value >= 8) return;
		Impl::TextureStage &stage = d.textureStages[d.activeTextureStage];
		uint8_t *const fields[4] = {&stage.magFilter, &stage.minFilter, &stage.wrapU, &stage.wrapV};
		*fields[parameter] = static_cast<uint8_t>(value);
	}

	void cGDriver12::TexStage(uint32_t stage) {
		if (stage < 2) impl->activeTextureStage = static_cast<uint8_t>(stage);
	}
	void cGDriver12::TexStageCoord(uint32_t source) {
		Impl &d = *impl;
		if (d.textureStages[d.activeTextureStage].coordinateSource == source) return;
		d.constantsDirty = true;
		d.textureStages[d.activeTextureStage].coordinateSource = source;
	}
	void cGDriver12::TexStageMatrix(float const *matrix, uint32_t rows, uint32_t columns, uint32_t flags) {
		Impl &d = *impl;
		float result[16]{};
		if (matrix == nullptr) {
			result[0] = result[5] = result[10] = result[15] = 1.0f;
		} else {
			memcpy(result, matrix, sizeof(result));
			if ((flags & 3) == 1 && rows == 4 && columns == 2) {
				result[2] = result[6] = result[14] = 0.0f;
				result[10] = result[15] = 1.0f;
				result[3] = result[7] = result[11] = 0.0f;
			}
		}
		float *const destination = d.textureStages[d.activeTextureStage].matrix;
		if (memcmp(destination, result, sizeof(result)) == 0) return;
		memcpy(destination, result, sizeof(result));
		d.constantsDirty = true;
	}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineScaleParamType parameter,
	                                 eGDTextureStageCombineScaleParam value) {
		Impl &d = *impl;
		d.constantsDirty = true;
		uint32_t const index = static_cast<uint32_t>(parameter);
		uint32_t const scale = static_cast<uint32_t>(value);
		if (index >= 2 || scale >= 3) return;
		if (index == 0) d.textureStages[d.activeTextureStage].rgbScale = static_cast<uint8_t>(scale);
		else d.textureStages[d.activeTextureStage].alphaScale = static_cast<uint8_t>(scale);
	}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineOperandType parameter, eGDBlend value) {
		Impl &d = *impl;
		d.constantsDirty = true;
		uint32_t const index = static_cast<uint32_t>(parameter);
		uint32_t const blend = static_cast<uint32_t>(value);
		if (index >= 8 || blend < 2 || blend > 5 || (index & 3) >= 3) return;
		Impl::TextureStage &stage = d.textureStages[d.activeTextureStage];
		uint8_t *const parameters = index < 4 ? stage.rgbParameters : stage.alphaParameters;
		parameters[index & 3] = static_cast<uint8_t>((parameters[index & 3] & 0x0f) | ((blend - 2) << 4));
	}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineSourceParamType parameter,
	                                 eGDTextureStageCombineSourceParam value) {
		Impl &d = *impl;
		d.constantsDirty = true;
		uint32_t const index = static_cast<uint32_t>(parameter);
		uint32_t const source = static_cast<uint32_t>(value);
		if (index >= 8 || source >= 4 || (index & 3) >= 3) return;
		Impl::TextureStage &stage = d.textureStages[d.activeTextureStage];
		uint8_t *const parameters = index < 4 ? stage.rgbParameters : stage.alphaParameters;
		parameters[index & 3] = static_cast<uint8_t>((parameters[index & 3] & 0xf0) | source);
	}
	void cGDriver12::TexStageCombine(eGDTextureStageCombineParamType parameter, eGDTextureStageCombineModeParam value) {
		Impl &d = *impl;
		d.constantsDirty = true;
		uint32_t const index = static_cast<uint32_t>(parameter);
		uint32_t const mode = static_cast<uint32_t>(value);
		if (index >= 2 || mode >= 6) return;
		if (index == 0) d.textureStages[d.activeTextureStage].rgbMode = static_cast<uint8_t>(mode);
		else d.textureStages[d.activeTextureStage].alphaMode = static_cast<uint8_t>(mode);
	}
	void cGDriver12::SetCombiner(cGDCombiner const &combiner, uint32_t stage) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (stage >= 2) return;
		Impl::TextureStage &destination = d.textureStages[stage];
		destination.environmentMode = 4;
		destination.rgbMode = combiner.RGBCombineMode;
		destination.alphaMode = combiner.AlphaCombineMode;
		destination.rgbScale = combiner.RGBScale;
		destination.alphaScale = combiner.AlphaScale;
		for (uint32_t index = 0; index < 3; ++index) {
			destination.rgbParameters[index] = static_cast<uint8_t>(
				(combiner.RGBParams[index].OperandType << 4) | combiner.RGBParams[index].SourceType);
			destination.alphaParameters[index] = static_cast<uint8_t>(
				(combiner.AlphaParams[index].OperandType << 4) | combiner.AlphaParams[index].SourceType);
		}
	}

	void cGDriver12::SetTexture(uint32_t texture, uint32_t stage) {
		Impl &d = *impl;
		if (stage >= 2 || (texture != 0 && d.textures.find(texture) == d.textures.end())) return;
		d.boundTextures[stage] = texture;
	}
	intptr_t cGDriver12::GetTexture(uint32_t stage) { return stage < 2 ? impl->boundTextures[stage] : 0; }

	intptr_t cGDriver12::CreateTexture(uint32_t internalFormat, uint32_t width, uint32_t height, uint32_t levels,
	                                   uint32_t) {
		Impl &d = *impl;
		uint32_t name = 0;
		GenTextures(1, &name);
		auto const found = d.textures.find(name);
		if (!d.CreateTextureResource(found->second, internalFormat, width, height, levels)) {
			d.textures.erase(found);
			return 0;
		}
		d.boundTextures[d.activeTextureStage] = name;
		return name;
	}

	// cGDriver::LoadTextureLevel
	void cGDriver12::LoadTextureLevel(uint32_t name, int32_t level, int32_t xOffset, int32_t yOffset, int32_t width,
	                                  int32_t height, uint32_t sourceFormat, uint32_t sourceType, uint32_t rowLength,
	                                  void const *pixels) {
		Impl &d = *impl;
		auto const found = d.textures.find(name);
		if (found == d.textures.end() || !found->second.resource || pixels == nullptr || level < 0 || width <= 0 ||
		    height <= 0 || xOffset < 0 || yOffset < 0 || static_cast<uint32_t>(level) >= found->second.levels) {
			return;
		}
		Impl::Texture &texture = found->second;
		uint32_t const mipWidth = D3D11MipDimension(texture.width, static_cast<uint32_t>(level));
		uint32_t const mipHeight = D3D11MipDimension(texture.height, static_cast<uint32_t>(level));
		if (!RangeFits(static_cast<uint32_t>(xOffset), static_cast<uint32_t>(width), mipWidth) ||
		    !RangeFits(static_cast<uint32_t>(yOffset), static_cast<uint32_t>(height), mipHeight)) {
			return;
		}
		uint8_t const *const sourceRows = static_cast<uint8_t const *>(pixels);
		uint32_t const x = static_cast<uint32_t>(xOffset);
		uint32_t const y = static_cast<uint32_t>(yOffset);

		if (IsCompressed(texture.format)) {
			DXGI_FORMAT expected = DXGI_FORMAT_UNKNOWN;
			if (sourceFormat == 7 || sourceFormat == 8) expected = DXGI_FORMAT_BC1_UNORM;
			if (sourceFormat == 9) expected = DXGI_FORMAT_BC2_UNORM;
			if (sourceFormat == 10) expected = DXGI_FORMAT_BC3_UNORM;
			uint32_t const sourceWidth = rowLength ? rowLength : static_cast<uint32_t>(width);
			if (sourceWidth < static_cast<uint32_t>(width) || expected != texture.format ||
			    !IsValidBlockCompressedUpdate(mipWidth, mipHeight, x, y, static_cast<uint32_t>(width),
			                                  static_cast<uint32_t>(height))) {
				return;
			}
			uint32_t const blockBytes = D3D11TextureRowPitch(texture.format, 1);
			uint64_t const sourcePitch = (static_cast<uint64_t>(sourceWidth) + 3) / 4 * blockBytes;
			uint32_t const rowBytes = D3D11TextureRowPitch(texture.format, static_cast<uint32_t>(width));
			// Whole blocks, also for the 2x2 and 1x1 tail levels.
			uint32_t const blockWidth = (static_cast<uint32_t>(width) + 3) & ~3u;
			uint32_t const blockHeight = (static_cast<uint32_t>(height) + 3) & ~3u;
			d.UploadTexture(texture, static_cast<uint32_t>(level), x, y, blockWidth, blockHeight, blockHeight / 4,
			                rowBytes, [&](uint8_t *row, uint32_t blockRow) {
				                std::memcpy(row, sourceRows + blockRow * sourcePitch, rowBytes);
				                return true;
			                });
			return;
		}

		uint32_t const sourcePixelBytes = TextureSourcePixelBytes(sourceFormat, sourceType);
		if (sourcePixelBytes == 0) return;
		uint32_t const sourceWidth = rowLength ? rowLength : static_cast<uint32_t>(width);
		if (sourceWidth < static_cast<uint32_t>(width)) return;
		uint64_t const sourcePitch = static_cast<uint64_t>(sourceWidth) * sourcePixelBytes;
		uint32_t const rowBytes = D3D11TextureRowPitch(texture.format, static_cast<uint32_t>(width));
		// Type 13 is GL_UNSIGNED_SHORT_4_4_4_4_REV: with format 3 (BGRA) exactly DXGI's B4G4R4A4 layout.
		bool const direct = (sourceFormat == 3 && sourceType == 1 && texture.format == DXGI_FORMAT_B8G8R8A8_UNORM) ||
		                    (sourceFormat == 3 && sourceType == 13 && texture.format == DXGI_FORMAT_B4G4R4A4_UNORM);
		d.UploadTexture(texture, static_cast<uint32_t>(level), x, y, static_cast<uint32_t>(width),
		                static_cast<uint32_t>(height), static_cast<uint32_t>(height), rowBytes,
		                [&](uint8_t *destination, uint32_t row) {
			                uint8_t const *source = sourceRows + row * sourcePitch;
			                if (direct) {
				                std::memcpy(destination, source, rowBytes);
				                return true;
			                }
			                for (int32_t column = 0; column < width; ++column, source += sourcePixelBytes) {
				                uint8_t rgba[4];
				                if (!ConvertTextureSourcePixel(sourceFormat, sourceType, source, rgba)) return false;
				                if (texture.format == DXGI_FORMAT_B8G8R8A8_UNORM) {
					                destination[column * 4 + 0] = rgba[2];
					                destination[column * 4 + 1] = rgba[1];
					                destination[column * 4 + 2] = rgba[0];
					                destination[column * 4 + 3] = rgba[3];
				                } else {
					                uint16_t const packed = Pack16BitPixel(texture.format, rgba);
					                std::memcpy(destination + column * 2, &packed, sizeof(packed));
				                }
			                }
			                return true;
		                });
	}

	// Video modes come from the VideoMode hook.
	uint32_t cGDriver12::CountVideoModes(void) const { return 0; }
	void cGDriver12::GetVideoModeInfo(uint32_t, sGDMode &) {}
	void cGDriver12::GetVideoModeInfo(sGDMode &) {}
	void cGDriver12::SetVideoMode(int32_t, void *, bool, bool) {}

	// Blits

	void cGDriver12::BitBlt(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
	                        void const *pixels, bool colorKeyed, void const *colorKey) {
		impl->DrawBlit(x, y, width, height, width, height, format, type, pixels, colorKeyed, colorKey,
		               impl->blitUsesSourceAlpha ? Impl::BlitAlpha::Source : Impl::BlitAlpha::Opaque, 0xffffffff);
	}
	void cGDriver12::StretchBlt(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
	                            int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels, bool colorKeyed,
	                            void const *colorKey) {
		impl->DrawBlit(x, y, width, height, sourceWidth, sourceHeight, format, type, pixels, colorKeyed, colorKey,
		               impl->blitUsesSourceAlpha ? Impl::BlitAlpha::Source : Impl::BlitAlpha::Opaque, 0xffffffff);
	}
	void cGDriver12::BitBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
	                             void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) {
		impl->DrawBlit(x, y, width, height, width, height, format, type, pixels, colorKeyed, colorKey,
		               Impl::BlitAlpha::Constant, alpha);
	}
	void cGDriver12::StretchBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
	                                 int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
	                                 bool colorKeyed, void const *colorKey, uint32_t alpha) {
		impl->DrawBlit(x, y, width, height, sourceWidth, sourceHeight, format, type, pixels, colorKeyed, colorKey,
		               Impl::BlitAlpha::Constant, alpha);
	}
	void cGDriver12::BitBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
	                                     uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey,
	                                     uint32_t alpha) {
		impl->DrawBlit(x, y, width, height, width, height, format, type, pixels, colorKeyed, colorKey,
		               Impl::BlitAlpha::SourceModulated, alpha);
	}
	void cGDriver12::StretchBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
	                                         int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
	                                         bool colorKeyed, void const *colorKey, uint32_t alpha) {
		impl->DrawBlit(x, y, width, height, sourceWidth, sourceHeight, format, type, pixels, colorKeyed, colorKey,
		               Impl::BlitAlpha::SourceModulated, alpha);
	}
	bool cGDriver12::Punt(uint32_t id, void *data) {
		if (id != kPuntBlitSourceAlpha || data == nullptr) return false;
		impl->blitUsesSourceAlpha = *static_cast<bool *>(data);
		return true;
	}

	// Viewport. SimGL counts y from the bottom, Direct3D from the top.

	void cGDriver12::SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) {
		Impl &d = *impl;
		if (x < 0 || y < 0 || width < 0 || height < 0 ||
		    !RangeFits(static_cast<uint32_t>(x), static_cast<uint32_t>(width), INT32_MAX) ||
		    !RangeFits(static_cast<uint32_t>(y), static_cast<uint32_t>(height), INT32_MAX)) {
			return;
		}
		d.scissorEnabled = true;
		d.viewportX = x;
		d.viewportY = y;
		d.viewportWidth = width;
		d.viewportHeight = height;
		int32_t const top = D3D11TopLeftY(d.windowHeight, y, height);
		d.viewport = {static_cast<float>(x), static_cast<float>(top), static_cast<float>(width),
		              static_cast<float>(height), 0.0f, 1.0f};
		d.scissor = {x, top, x + width, top + height};
		d.ApplyViewport();
	}
	void cGDriver12::SetViewport(void) { impl->SetFullViewport(); }
	void cGDriver12::GetViewport(int32_t dimensions[4]) {
		Impl &d = *impl;
		dimensions[0] = d.viewportX;
		dimensions[1] = d.viewportY;
		dimensions[2] = d.viewportX + d.viewportWidth;
		dimensions[3] = d.viewportY + d.viewportHeight;
	}

	char const *cGDriver12::GetDriverInfo(void) const { return "Maxis 3D GDriver\nDirect3D 12\n12.0\n"; }
	uint32_t cGDriver12::GetGZCLSID(void) const { return 0xBADB6906; }
	bool cGDriver12::Init(void) { return true; }
	bool cGDriver12::Shutdown(void) { return true; }
	bool cGDriver12::IsDeviceReady(void) { return impl->backBuffer != nullptr; }

	// The frame is complete: hand it to the GPU without waiting for it.
	void cGDriver12::Flush(void) {
		Impl &d = *impl;
		if (d.relight && d.backBuffer) d.RecordLighting();
		d.Submit();
	}

	// Vertex buffer extension. The replay fills the buffer through Vertices().

	char const *cGDriver12::GetVertexBufferName(uint32_t) { return nullptr; }
	uint32_t cGDriver12::VertexBufferType(uint32_t name) { return name == 0 ? kGDVertexFormat_V3F_C4UB_2T2F : UINT32_MAX; }
	uint32_t cGDriver12::MaxVertices(uint32_t name) { return name == 0 ? kExtensionCapacity : 0; }
	uint32_t cGDriver12::GetVertices(int32_t, uint32_t) { return 0; }
	uint32_t cGDriver12::ContinueVertices(uint32_t, uint32_t) { return 0; }
	void cGDriver12::ReleaseVertices(uint32_t) {}

	void cGDriver12::DrawPrims(uint32_t name, uint32_t primitive, void *, uint32_t byteSize) {
		Impl &d = *impl;
		nSCDWire::BlobKey const blob = std::exchange(d.drawVertices, {});
		d.drawIndices = {};
		uint32_t const format = kGDVertexFormat_V3F_C4UB_2T2F;
		uint32_t const stride = RZVertexFormatStride(format);
		if (name != 0 || !d.backBuffer || !d.ExtensionHolds(byteSize)) return;
		if (D3D11Topology(primitive) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) return;
		if (d.capture != nullptr &&
		    d.CaptureDraw(d.extensionVertices.data(), format, stride, byteSize / stride, primitive, nullptr, 0, 0)) {
			return;
		}
		if (!d.UploadVertices(d.extensionVertices.data(), format, stride, 0, byteSize / stride, blob)) return;
		uint32_t const previousFormat = d.interleavedFormat;
		d.interleavedFormat = format;
		if (d.BindGeometry(primitive, 0)) d.list->DrawInstanced(byteSize / stride, 1, 0, 0);
		d.interleavedFormat = previousFormat;
	}

	void cGDriver12::DrawPrimsIndexed(uint32_t name, uint32_t primitive, uint32_t count, uint16_t *indices) {
		Impl &d = *impl;
		nSCDWire::BlobKey const vertexBlob = std::exchange(d.drawVertices, {});
		nSCDWire::BlobKey const indexBlob = std::exchange(d.drawIndices, {});
		if (name != 0 || count == 0 || indices == nullptr || !d.backBuffer) return;
		CacheKey const key{indexBlob.low, indexBlob.high, indexBlob.size, CacheKind::Indices, 3, 0, 0, count};
		CacheEntry const *cached = indexBlob.size != 0 ? d.FindCached(key) : nullptr;
		uint32_t maximumIndex = 0;
		if (cached != nullptr) {
			maximumIndex = cached->maximumIndex;
		} else {
			for (uint32_t i = 0; i < count; ++i) maximumIndex = (std::max)(maximumIndex, uint32_t{indices[i]});
		}
		uint32_t const format = kGDVertexFormat_V3F_C4UB_2T2F;
		uint32_t const stride = RZVertexFormatStride(format);
		uint32_t const byteSize = (maximumIndex + 1) * stride;
		if (!d.ExtensionHolds(byteSize) || D3D11Topology(primitive) == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED) return;
		if (d.lightHooks != nullptr && primitive == 0) {
			d.lightHooks->IndexedVertices(d.extensionVertices.data(), byteSize, stride, indices, count);
		}
		if (d.capture != nullptr &&
		    d.CaptureDraw(d.extensionVertices.data(), format, stride, byteSize / stride, primitive, indices, count, 3)) {
			return;
		}
		if (!d.UploadVertices(d.extensionVertices.data(), format, stride, 0, byteSize / stride, vertexBlob)) return;
		if (cached != nullptr) d.BindIndices(d.CachedAddress(*cached), count, false);
		else if (!d.UploadIndices(indices, count, false, indexBlob.size != 0 ? &key : nullptr, 0, maximumIndex)) return;
		uint32_t const previousFormat = d.interleavedFormat;
		d.interleavedFormat = format;
		if (d.BindGeometry(primitive, 16)) d.list->DrawIndexedInstanced(count, 1, 0, 0, 0);
		d.interleavedFormat = previousFormat;
	}

	void cGDriver12::Reset(void) { impl->extensionVerticesValid = false; }

	// Buffer regions: SC4's backing store. Colour regions copy to and from the back buffer, depth regions
	// through a plain copy of the depth buffer, which partial copies may not touch.

	bool cGDriver12::BufferRegionEnabled(void) { return impl->device != nullptr; }

	uint32_t cGDriver12::NewBufferRegion(int32_t type) {
		Impl &d = *impl;
		for (uint32_t index = 0; index < kRegionCount; ++index) {
			if ((d.regionFlags & (1u << index)) != 0) continue;
			if (!d.CreateRegion(index, type)) return 0;
			d.regionFlags |= static_cast<uint8_t>(1u << index);
			return index + 1;
		}
		return 0;
	}

	bool cGDriver12::DeleteBufferRegion(int32_t region) {
		Impl &d = *impl;
		if (region < 1 || region > static_cast<int32_t>(kRegionCount)) return false;
		uint32_t const index = static_cast<uint32_t>(region - 1);
		d.RetireRegion(d.regions[index]);
		d.regionFlags &= static_cast<uint8_t>(~(1u << index));
		return true;
	}

	bool cGDriver12::ReadBufferRegion(uint32_t region, int32_t destinationX, int32_t destinationY, int32_t width,
	                                  int32_t height, int32_t sourceX, int32_t sourceY) {
		Impl &d = *impl;
		// The owned scene draws the whole static view itself and needs no backing store.
		if (d.skipRegionCopies) return true;
		if (!d.RegionArgumentsFit(region, destinationX, destinationY, width, height, sourceX, sourceY)) return false;
		Impl::Region &target = d.regions[region - 1];
		D3D12_BOX const box{
			static_cast<UINT>(sourceX), static_cast<UINT>(sourceY), 0,
			static_cast<UINT>(sourceX + width), static_cast<UINT>(sourceY + height), 1
		};
		d.Transition(target.texture.Get(), target.state, D3D12_RESOURCE_STATE_COPY_DEST);
		if (target.type == 0) {
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			d.Transition(d.backBuffer.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
			d.CopyRegion(target.texture.Get(), destinationX, destinationY, d.backBuffer.Get(), box, false);
			d.Transition(d.backBuffer.Get(), state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			d.CopyLightRegion(target, true, destinationX, destinationY, box);
		} else {
			if (!d.RefreshDepthScratch()) return false;
			d.Transition(d.depthScratch.Get(), d.depthScratchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
			d.CopyRegion(target.texture.Get(), destinationX, destinationY, d.depthScratch.Get(), box, true);
		}
		return true;
	}

	bool cGDriver12::DrawBufferRegion(uint32_t region, int32_t sourceX, int32_t sourceY, int32_t width, int32_t height,
	                                  int32_t destinationX, int32_t destinationY) {
		Impl &d = *impl;
		// The owned scene draws the whole static view itself and needs no backing store.
		if (d.skipRegionCopies) return true;
		if (!d.RegionArgumentsFit(region, destinationX, destinationY, width, height, sourceX, sourceY)) return false;
		Impl::Region &source = d.regions[region - 1];
		D3D12_BOX const box{
			static_cast<UINT>(sourceX), static_cast<UINT>(sourceY), 0,
			static_cast<UINT>(sourceX + width), static_cast<UINT>(sourceY + height), 1
		};
		d.Transition(source.texture.Get(), source.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
		if (source.type == 0) {
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_RENDER_TARGET;
			d.Transition(d.backBuffer.Get(), state, D3D12_RESOURCE_STATE_COPY_DEST);
			d.CopyRegion(d.backBuffer.Get(), destinationX, destinationY, source.texture.Get(), box, false);
			d.Transition(d.backBuffer.Get(), state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			d.CopyLightRegion(source, false, destinationX, destinationY, box);
			return true;
		}
		// Patch the plain copy, then replace the depth buffer with it.
		if (!d.RefreshDepthScratch()) return false;
		d.Transition(d.depthScratch.Get(), d.depthScratchState, D3D12_RESOURCE_STATE_COPY_DEST);
		d.CopyRegion(d.depthScratch.Get(), destinationX, destinationY, source.texture.Get(), box, true);
		d.Transition(d.depthScratch.Get(), d.depthScratchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		d.Transition(d.depthBuffer.Get(), depthState, D3D12_RESOURCE_STATE_COPY_DEST);
		d.list->CopyResource(d.depthBuffer.Get(), d.depthScratch.Get());
		d.Transition(d.depthBuffer.Get(), depthState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
		d.depthScratchValid = true;
		return true;
	}

	bool cGDriver12::IsBufferRegion(uint32_t region) { return impl->IsRegion(region); }
	bool cGDriver12::CanDoPartialRegionWrites(void) { return true; }
	bool cGDriver12::CanDoOffsetReads(void) { return true; }
	bool cGDriver12::FinalRelease(void) { return true; }
	bool cGDriver12::DeleteAllBufferRegions(void) {
		Impl &d = *impl;
		for (Impl::Region &region: d.regions) d.RetireRegion(region);
		d.regionFlags = 0;
		return true;
	}

	cIGZBuffer *cGDriver12::CopyColorBuffer(int32_t, int32_t, int32_t, int32_t, cIGZBuffer *buffer) { return buffer; }

	// Lighting extension

	void cGDriver12::EnableLighting(bool enabled) {
		impl->constantsDirty = true;
		impl->lightingEnabled = enabled;
	}
	void cGDriver12::EnableLight(uint32_t light, bool enabled) {
		impl->constantsDirty = true;
		if (light < 8) impl->lightsEnabled[light] = enabled;
	}
	// D3DRS_AMBIENT is a D3DCOLOR: clamped to [0, 1], alpha ignored.
	void cGDriver12::LightModelAmbient(float r, float g, float b, float) {
		Impl &d = *impl;
		d.constantsDirty = true;
		d.globalAmbient[0] = (std::clamp)(r, 0.0f, 1.0f);
		d.globalAmbient[1] = (std::clamp)(g, 0.0f, 1.0f);
		d.globalAmbient[2] = (std::clamp)(b, 0.0f, 1.0f);
	}
	void cGDriver12::LightColor(uint32_t light, uint32_t parameter, float const *color) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (light >= 8 || parameter > 2 || color == nullptr) return;
		float *const destination = parameter == 0 ? d.lightAmbient[light]
		                           : (parameter == 1 ? d.lightDiffuse[light] : d.lightSpecular[light]);
		memcpy(destination, color, sizeof(d.lightAmbient[light]));
	}
	void cGDriver12::LightColor(uint32_t light, float const *ambient, float const *diffuse, float const *specular) {
		impl->constantsDirty = true;
		if (ambient) LightColor(light, 0, ambient);
		if (diffuse) LightColor(light, 1, diffuse);
		if (specular) LightColor(light, 2, specular);
	}
	void cGDriver12::LightPosition(uint32_t light, float const *position) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (light < 8 && position != nullptr) memcpy(d.lightPosition[light], position, sizeof(d.lightPosition[light]));
	}
	void cGDriver12::LightDirection(uint32_t light, float const *direction) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (light >= 8 || direction == nullptr) return;
		memcpy(d.lightPosition[light], direction, sizeof(float) * 3);
		d.lightPosition[light][3] = 0.0f;
	}
	void cGDriver12::MaterialColor(uint32_t parameter, float const *color) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (parameter > 4 || color == nullptr) return;
		if (parameter == 4) {
			d.materialShininess = color[0];
			return;
		}
		float *const destination = parameter == 0 ? d.materialAmbient
		                           : (parameter == 1 ? d.materialDiffuse
		                              : (parameter == 2 ? d.materialSpecular : d.materialEmission));
		memcpy(destination, color, sizeof(d.materialAmbient));
	}
	void cGDriver12::MaterialColor(float const *ambient, float const *diffuse, float const *specular,
	                               float const *emission, float shininess) {
		Impl &d = *impl;
		d.constantsDirty = true;
		if (ambient) memcpy(d.materialAmbient, ambient, sizeof(d.materialAmbient));
		if (diffuse) memcpy(d.materialDiffuse, diffuse, sizeof(d.materialDiffuse));
		if (specular) memcpy(d.materialSpecular, specular, sizeof(d.materialSpecular));
		if (emission) memcpy(d.materialEmission, emission, sizeof(d.materialEmission));
		if (shininess >= 0.0f) d.materialShininess = shininess;
	}

	// The scene lane's hooks.

	void cGDriver12::SetDrawFilter(DrawFilter *filter) { impl->drawFilter = filter; }

	void cGDriver12::SaveDrawState(DrawState &state) const {
		Impl const &d = *impl;
		for (uint32_t i = 0; i < 2; ++i) {
			Impl::TextureStage const &from = d.textureStages[i];
			DrawState::Stage &to = state.stages[i];
			to.enabled = d.textureStageEnabled[i];
			to.texture = d.boundTextures[i];
			to.magFilter = from.magFilter;
			to.minFilter = from.minFilter;
			to.wrapU = from.wrapU;
			to.wrapV = from.wrapV;
			to.mode = from.environmentMode;
			to.coordinates = from.coordinateSource;
			memcpy(to.matrix, from.matrix, sizeof(to.matrix));
		}
		state.activeStage = d.activeTextureStage;
		state.alphaTest = d.enabledCapabilities[kGDCapability_AlphaTest];
		state.depthTest = d.enabledCapabilities[kGDCapability_DepthTest];
		state.cullFace = d.enabledCapabilities[kGDCapability_CullFace];
		state.blend = d.enabledCapabilities[kGDCapability_Blend];
		state.colorWrite = d.colorWriteEnabled;
		state.depthWrite = d.depthWriteEnabled;
		state.depthFunction = d.depthFunction;
		state.sourceBlend = d.sourceBlend;
		state.destinationBlend = d.destinationBlend;
		state.alphaFunction = d.alphaFunction;
		state.alphaReference = d.alphaReference;
		state.lighting = d.lightingEnabled;
		state.diffuseFromVertex = d.diffuseFromVertex;
		memcpy(state.globalAmbient, d.globalAmbient, sizeof(state.globalAmbient));
		memcpy(state.materialAmbient, d.materialAmbient, sizeof(state.materialAmbient));
		memcpy(state.materialDiffuse, d.materialDiffuse, sizeof(state.materialDiffuse));
		state.vertexFormat = d.interleavedFormat;
		state.vertexStride = d.interleavedStride;
		state.vertexPointer = d.interleavedPointer;
	}

	void cGDriver12::RestoreDrawState(DrawState const &state) {
		Impl &d = *impl;
		// The constants the last draw built stay valid when nothing they are made from changes back.
		bool constantsChange = d.alphaFunction != state.alphaFunction ||
		                       memcmp(&d.alphaReference, &state.alphaReference, sizeof(float)) != 0 ||
		                       d.enabledCapabilities[kGDCapability_AlphaTest] != state.alphaTest ||
		                       d.lightingEnabled != state.lighting || d.diffuseFromVertex != state.diffuseFromVertex ||
		                       memcmp(d.globalAmbient, state.globalAmbient, sizeof(d.globalAmbient)) != 0 ||
		                       memcmp(d.materialAmbient, state.materialAmbient, sizeof(d.materialAmbient)) != 0 ||
		                       memcmp(d.materialDiffuse, state.materialDiffuse, sizeof(d.materialDiffuse)) != 0;
		for (uint32_t i = 0; i < 2; ++i) {
			Impl::TextureStage const &stage = d.textureStages[i];
			constantsChange = constantsChange || stage.environmentMode != state.stages[i].mode ||
			                  stage.coordinateSource != state.stages[i].coordinates ||
			                  memcmp(stage.matrix, state.stages[i].matrix, sizeof(stage.matrix)) != 0;
		}
		for (uint32_t i = 0; i < 2; ++i) {
			DrawState::Stage const &from = state.stages[i];
			Impl::TextureStage &to = d.textureStages[i];
			d.textureStageEnabled[i] = from.enabled;
			d.boundTextures[i] = from.texture;
			to.magFilter = from.magFilter;
			to.minFilter = from.minFilter;
			to.wrapU = from.wrapU;
			to.wrapV = from.wrapV;
			to.environmentMode = from.mode;
			to.coordinateSource = from.coordinates;
			memcpy(to.matrix, from.matrix, sizeof(to.matrix));
		}
		d.activeTextureStage = state.activeStage;
		d.enabledCapabilities[kGDCapability_AlphaTest] = state.alphaTest;
		d.enabledCapabilities[kGDCapability_DepthTest] = state.depthTest;
		d.enabledCapabilities[kGDCapability_CullFace] = state.cullFace;
		d.enabledCapabilities[kGDCapability_Blend] = state.blend;
		d.colorWriteEnabled = state.colorWrite;
		d.depthWriteEnabled = state.depthWrite;
		d.depthFunction = state.depthFunction;
		d.sourceBlend = state.sourceBlend;
		d.destinationBlend = state.destinationBlend;
		d.alphaFunction = state.alphaFunction;
		d.alphaReference = state.alphaReference;
		d.lightingEnabled = state.lighting;
		d.diffuseFromVertex = state.diffuseFromVertex;
		memcpy(d.globalAmbient, state.globalAmbient, sizeof(d.globalAmbient));
		memcpy(d.materialAmbient, state.materialAmbient, sizeof(d.materialAmbient));
		memcpy(d.materialDiffuse, state.materialDiffuse, sizeof(d.materialDiffuse));
		d.interleavedFormat = state.vertexFormat;
		d.interleavedStride = state.vertexStride;
		d.interleavedPointer = state.vertexPointer;
		if (constantsChange) d.constantsDirty = true;
	}

	ID3D12GraphicsCommandList *cGDriver12::CommandList() const { return impl->list.Get(); }

	bool cGDriver12::Upload(void const *data, uint64_t bytes, ID3D12Resource *&resource, uint64_t &offset,
	                        uint64_t alignment) {
		Impl::Allocation const allocation = impl->Upload(data, bytes, alignment);
		resource = allocation.resource;
		offset = allocation.offset;
		return allocation.cpu != nullptr;
	}

	void cGDriver12::DrawTriangles(D3D12_VERTEX_BUFFER_VIEW const &vertices, D3D12_INDEX_BUFFER_VIEW const &indices,
	                               uint32_t count, uint32_t format) {
		Impl &d = *impl;
		if (count == 0 || !d.backBuffer) return;
		d.interleavedFormat = format;
		if (!d.BindGeometry(0, 16)) return;
		d.list->IASetVertexBuffers(0, 1, &vertices);
		d.list->IASetIndexBuffer(&indices);
		d.list->DrawIndexedInstanced(count, 1, 0, 0, 0);
	}

	// The retained scene's hooks.

	void cGDriver12::SaveTransform(Transform &transform) const {
		Impl const &d = *impl;
		memcpy(transform.modelView, d.matrices[0], sizeof(transform.modelView));
		memcpy(transform.projection, d.matrices[1], sizeof(transform.projection));
		transform.mode = d.activeMatrixMode;
	}

	void cGDriver12::RestoreTransform(Transform const &transform) {
		Impl &d = *impl;
		d.activeMatrixMode = 0;
		LoadMatrix(transform.modelView);
		d.activeMatrixMode = 1;
		LoadMatrix(transform.projection);
		d.activeMatrixMode = transform.mode;
	}

	int32_t cGDriver12::GetPolygonOffset() const { return impl->polygonOffset; }

	// The light hooks'.

	void cGDriver12::SetLightHooks(LightHooks *hooks) {
		impl->lightHooks = hooks;
		impl->lightHooked = impl->lightHooked || hooks != nullptr;
	}
	cGDriver12::Sun const &cGDriver12::RelightSun() const { return impl->sun; }
	D3D12_GPU_DESCRIPTOR_HANDLE cGDriver12::ViewTable() const { return impl->ViewGpu(0); }

	uint32_t cGDriver12::TextureView(uint32_t name) {
		Impl &d = *impl;
		auto const found = d.textures.find(name);
		if (found == d.textures.end() || !found->second.resource) return 0;
		auto &texture = found->second;
		d.Transition(texture.resource.Get(), texture.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
		return texture.view;
	}

	uint32_t cGDriver12::CreateBufferView(ID3D12Resource *buffer) {
		Impl &d = *impl;
		if (buffer == nullptr || d.nextView >= kViewSlots) return 0;
		uint32_t const slot = d.nextView++;
		D3D12_SHADER_RESOURCE_VIEW_DESC description{};
		description.Format = DXGI_FORMAT_R32_TYPELESS;
		description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		description.Buffer.NumElements = static_cast<UINT>(buffer->GetDesc().Width / 4);
		description.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
		d.device->CreateShaderResourceView(buffer, &description, d.ViewCpu(slot));
		return slot;
	}

	void cGDriver12::SetTerrainField(ID3D12Resource *buffer) {
		Impl &d = *impl;
		d.terrainField = buffer;
		// Frames in flight may still read the old view.
		if (d.relight && d.backBuffer) {
			d.WaitIdle();
			d.TerrainFieldView();
		}
	}
	ID3D12Resource *cGDriver12::DepthBuffer() const { return impl->depthBuffer.Get(); }
	ID3D12Resource *cGDriver12::BackBuffer() const { return impl->backBuffer.Get(); }
	D3D12_CPU_DESCRIPTOR_HANDLE cGDriver12::DepthView() const { return impl->DepthStencilView(); }
	void cGDriver12::Rebind() { impl->BindFrame(); }

	// The owned scene's hooks.

	static_assert(sizeof(DriverConstants) == cGDriver12::kConstantBytes);
	static_assert(offsetof(DriverConstants, modelView) == cGDriver12::kModelViewOffset);
	static_assert(offsetof(DriverConstants, projection) == cGDriver12::kProjectionOffset);
	static_assert(offsetof(DriverConstants, textureMatrices) == cGDriver12::kTextureMatrixOffset);
	static_assert(offsetof(DriverConstants, stages) == cGDriver12::kStageOffset);
	static_assert(sizeof(DriverConstants::StageConstants) == cGDriver12::kStageBytes);
	static_assert(offsetof(DriverConstants::StageConstants, parameters1) + 12 == cGDriver12::kCoordinateSourceOffset);

	void cGDriver12::SetCapture(DrawCapture *capture, bool ownsStaticView) {
		impl->capture = capture;
		impl->skipRegionCopies = capture != nullptr && ownsStaticView;
	}

	void cGDriver12::ClearTargets() {
		Impl &d = *impl;
		if (!d.backBuffer) return;
		float const black[4]{0.0f, 0.0f, 0.0f, 1.0f}, noCity[4]{};
		d.list->ClearRenderTargetView(d.RenderTargetView(), black, 0, nullptr);
		if (d.relight) d.list->ClearRenderTargetView(d.RenderTargetView(1), noCity, 0, nullptr);
		d.list->ClearDepthStencilView(d.DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
		                              nullptr);
		d.depthScratchValid = false;
	}

	bool cGDriver12::CaptureState(uint32_t format, CapturedState &state) { return impl->CaptureState(format, state); }

	uint32_t cGDriver12::PinTexture(uint32_t name) {
		Impl &d = *impl;
		auto const found = d.textures.find(name);
		if (found == d.textures.end() || !found->second.resource) return 0;
		Impl::Texture const &texture = found->second;
		auto const pinned = d.pinOf.find(texture.resource.Get());
		if (pinned != d.pinOf.end()) {
			++d.pins[pinned->second - 1].references;
			return pinned->second;
		}
		uint32_t const view = d.CreateView(texture.resource.Get(), texture.format, texture.levels);
		if (view == 0) return 0;
		uint32_t pin;
		if (!d.freePins.empty()) {
			pin = d.freePins.back();
			d.freePins.pop_back();
		} else {
			d.pins.emplace_back();
			pin = static_cast<uint32_t>(d.pins.size());
		}
		Impl::Pin &entry = d.pins[pin - 1];
		entry.resource = texture.resource;
		entry.view = view;
		entry.references = 1;
		entry.name = name;
		entry.orphan = false;
		d.pinOf.emplace(texture.resource.Get(), pin);
		return pin;
	}

	uint32_t cGDriver12::PinView(uint32_t pin) {
		Impl &d = *impl;
		if (pin == 0 || pin > d.pins.size() || !d.pins[pin - 1].resource) return 0;
		Impl::Pin const &entry = d.pins[pin - 1];
		if (!entry.orphan) {
			auto const found = d.textures.find(entry.name);
			if (found != d.textures.end() && found->second.resource == entry.resource) {
				d.Transition(entry.resource.Get(), found->second.state, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
			}
		}
		return entry.view;
	}

	void cGDriver12::UnpinTexture(uint32_t pin) {
		Impl &d = *impl;
		if (pin == 0 || pin > d.pins.size()) return;
		Impl::Pin &entry = d.pins[pin - 1];
		if (entry.references == 0 || --entry.references != 0) return;
		d.pinOf.erase(entry.resource.Get());
		d.frames[d.frameIndex].freedViews.push_back(entry.view);
		d.Retire(entry.resource);
		entry = {};
		d.freePins.push_back(pin);
	}

	void cGDriver12::BeginRetained(float const *projection) {
		Impl &d = *impl;
		d.list->SetGraphicsRoot32BitConstants(5, 16, projection, 0);
		D3D12_VIEWPORT const full{0.0f, 0.0f, static_cast<float>(d.windowWidth), static_cast<float>(d.windowHeight), 0.0f, 1.0f};
		D3D12_RECT const all{0, 0, d.windowWidth, d.windowHeight};
		d.list->RSSetViewports(1, &full);
		d.list->RSSetScissorRects(1, &all);
	}

	void cGDriver12::DrawRetained(RetainedDraw const &draw) {
		Impl &d = *impl;
		if (draw.indexCount == 0) return;
		uint32_t views[2]{};
		for (uint32_t stage = 0; stage < 2; ++stage) {
			uint32_t const pin = draw.pins[stage];
			if (pin == 0 || pin > d.pins.size()) continue;
			Impl::Pin &entry = d.pins[pin - 1];
			views[stage] = entry.view;
			if (entry.orphan) continue;
			// Still the game's texture: its state is tracked there.
			auto const found = d.textures.find(entry.name);
			if (found != d.textures.end() && found->second.resource == entry.resource) {
				d.Transition(entry.resource.Get(), found->second.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			}
		}
		PipelineKey key;
		key.bits = draw.pipeline | 1ull << 52;
		key.depthBias = draw.depthBias;
		if (!d.SetPipeline(key)) return;
		if (draw.stencilReference != d.appliedStencilReference) {
			d.list->OMSetStencilRef(static_cast<UINT>(draw.stencilReference));
			d.appliedStencilReference = draw.stencilReference;
		}
		d.SetConstants(draw.constants);
		for (uint32_t stage = 0; stage < 2; ++stage) d.SetStage(stage, views[stage], draw.samplers[stage]);
		d.SetTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		D3D12_VERTEX_BUFFER_VIEW const buffers[2]{draw.vertices, draw.instances};
		d.list->IASetVertexBuffers(0, 2, buffers);
		d.list->IASetIndexBuffer(&draw.indices);
		d.list->DrawIndexedInstanced(draw.indexCount, draw.instances.SizeInBytes / draw.instances.StrideInBytes,
		                             draw.startIndex, 0, 0);
		d.depthScratchValid = false;
		if (d.relight) d.cityDrawn = true;
	}

	void cGDriver12::EndRetained() { impl->ApplyViewport(); }

	void cGDriver12::RetainedCamera(float const *view, float const *projection) {
		Impl &d = *impl;
		if (d.relight) d.LearnCamera(view, projection, d.windowWidth, d.windowHeight);
	}
}
