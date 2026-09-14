/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SimCity 4's city view as SCD3D11's ReShade integration hands it over, shared by SimCity4.fx and
// SimCity4Sunlight.fx:
//  - The effects run before the UI, on a city view SC4 has redrawn completely (backing store restore or scroll
//    shift, then the whole dynamic view), so no pixel is processed twice.
//  - The camera is orthographic and SCD3D11 hands over its projection and the unencoded depth buffer: depth is
//    exact meters, every pixel covers the same ground and shares one view vector. Scrolling moves the view by
//    whole pixels, so single-frame effects need no history to stay stable.

#pragma once

#include "ReShade.fxh"

// Set by SCD3D11 while the city view renders, zero otherwise: P[0][0], P[1][1], P[2][2], P[3][2] of the
// orthographic camera. No initializer, so ReShade's performance mode keeps it a runtime value.
uniform float4 SC4Ortho < source = "scd3d11_ortho"; >;

// Bound by SCD3D11: the city's depth buffer as SC4 wrote it, without the encoding the DEPTH semantic carries.
texture SC4DepthTex : SC4_DEPTH;

sampler SC4Depth { Texture = SC4DepthTex; MagFilter = POINT; MinFilter = POINT; MipFilter = POINT; };
sampler SC4Color { Texture = ReShade::BackBufferTex; SRGBTexture = true; };

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);
static const float kPi = 3.14159265;
static const float kBayer[16] = { 0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0 };

bool HasCamera()
{
	return SC4Ortho.x != 0.0 && SC4Ortho.z != 0.0;
}

// D3D depth in [0, 1], at the depth buffer's full precision and independent of any RESHADE_DEPTH_* definition.
float RawDepth(float2 uv)
{
	return tex2Dlod(SC4Depth, float4(uv, 0.0, 0.0)).x;
}

bool IsSky(float rawDepth)
{
	return rawDepth > 0.999999; // nothing drawn, past the edge of the city
}

// Meters along the view direction; orthographic, so nothing else depends on the pixel.
float ViewDistance(float rawDepth)
{
	return (rawDepth * 2.0 - 1.0 - SC4Ortho.w) / abs(SC4Ortho.z);
}

float MetersPerPixel()
{
	return 2.0 / (abs(SC4Ortho.x) * BUFFER_WIDTH);
}

float2 ScreenMeters()
{
	return 2.0 / abs(SC4Ortho.xy);
}

// Camera space in meters: x right, y down like texcoords, z away from the camera.
float3 CameraPosition(float2 uv)
{
	return float3(uv * ScreenMeters(), ViewDistance(RawDepth(uv)));
}

// A direction in the GL view space SCD3D11's directions come in (x right, y up, z towards the viewer).
float3 EyeToCamera(float3 direction)
{
	return float3(direction.x * sign(SC4Ortho.x), -direction.y * sign(SC4Ortho.y), direction.z * sign(SC4Ortho.z));
}

float2 TexelCenter(float2 uv)
{
	return (min(floor(saturate(uv) * BUFFER_SCREEN_SIZE), BUFFER_SCREEN_SIZE - 1.0) + 0.5) * BUFFER_PIXEL_SIZE;
}

// Faces the camera (negative z). Differentiates towards whichever neighbour lies on the same surface.
float3 CameraNormal(float2 uv, float3 center)
{
	float3 left = CameraPosition(uv - float2(BUFFER_RCP_WIDTH, 0.0));
	float3 right = CameraPosition(uv + float2(BUFFER_RCP_WIDTH, 0.0));
	float3 up = CameraPosition(uv - float2(0.0, BUFFER_RCP_HEIGHT));
	float3 down = CameraPosition(uv + float2(0.0, BUFFER_RCP_HEIGHT));
	float3 dx = abs(right.z - center.z) < abs(center.z - left.z) ? right - center : center - left;
	float3 dy = abs(down.z - center.z) < abs(center.z - up.z) ? down - center : center - up;
	return normalize(cross(dy, dx));
}

// A texel of a buffer `scale` times smaller than the screen stands for the full resolution texel at its top-left.
float2 CoarseTexelUV(int2 texel, int scale)
{
	return (float2(texel * scale) + 0.5) * BUFFER_PIXEL_SIZE;
}

float CoarseViewDistance(int2 texel, int scale)
{
	return ViewDistance(RawDepth(CoarseTexelUV(texel, scale)));
}

float Bayer(int2 pixel)
{
	return (kBayer[(pixel.y & 3) * 4 + (pixel.x & 3)] + 0.5) / 16.0;
}

float3 LinearToSRGB(float3 color)
{
	return lerp(12.92 * color, 1.055 * pow(color, 1.0 / 2.4) - 0.055, step(0.0031308, color));
}

// 4x4 depth-aware average of a coarse buffer. A 4x4 window holds each entry of the 4x4 jitter pattern exactly
// once, so this removes the pattern completely.
float Denoise(sampler source, int2 texel, int scale, int2 size)
{
	float center = CoarseViewDistance(texel, scale);
	float tolerance = MetersPerPixel() * 4.0 * scale;
	float sum = 0.0;
	float weight = 0.0;
	for (int y = -2; y < 2; y++)
	for (int x = -2; x < 2; x++)
	{
		int2 tap = clamp(texel + int2(x, y), int2(0, 0), size - 1);
		float w = saturate(1.0 - abs(CoarseViewDistance(tap, scale) - center) / tolerance) + 0.001;
		sum += tex2Dfetch(source, tap).r * w;
		weight += w;
	}
	return sum / weight;
}

// Bilinear upsample of a coarse buffer that leaves out texels on other surfaces.
float Upsample(sampler source, float2 pixelCenter, float viewDistance, int scale, int2 size)
{
	float2 position = (pixelCenter - 0.5) / scale;
	int2 base = int2(floor(position));
	float2 f = position - float2(base);
	float tolerance = MetersPerPixel() * 2.0 * scale;
	float sum = 0.0;
	float weight = 0.0;
	for (int y = 0; y < 2; y++)
	for (int x = 0; x < 2; x++)
	{
		int2 tap = clamp(base + int2(x, y), int2(0, 0), size - 1);
		float bilinear = (x == 0 ? 1.0 - f.x : f.x) * (y == 0 ? 1.0 - f.y : f.y);
		float w = bilinear * (saturate(1.0 - abs(CoarseViewDistance(tap, scale) - viewDistance) / tolerance) + 0.001);
		sum += tex2Dfetch(source, tap).r * w;
		weight += w;
	}
	return sum / max(weight, 0.0001);
}

// Scene brightness from SC4's own image, as log2 luminance for a mipmapped buffer to average.
float LogLuminance(float2 uv)
{
	return log2(dot(tex2D(SC4Color, uv).rgb, kLuma) + 0.0001);
}

float Daylight(float averageLuminance)
{
	return smoothstep(0.04, 0.10, averageLuminance);
}
