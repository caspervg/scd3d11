/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Ambience for SimCity 4's city view: ambient occlusion, haze, night light glow, sharpening and color, plus a
// tilt-shift technique that turns the city into a scale model.
//  - Buildings are pre-rendered with their lighting baked in, on coarse LOD meshes. The ambient occlusion adds
//    only the large-scale occlusion SC4 lacks: contact shading where buildings meet the ground, dark streets
//    between towers. Its radius is in meters, so it keeps its look across zoom levels.
//  - At night the lit windows are the only light sources, so the glow follows the city's average brightness.
//
// Order in ReShade: SMAA (optional), SimCity 4 Sunlight (optional), SimCity 4, SimCity 4 Miniature.

#include "SimCity4.fxh"

#ifndef SC4_AO_SLICES
	#define SC4_AO_SLICES 3 // Directions per pixel; the 4x4 denoise turns 3 into 48.
#endif
#ifndef SC4_AO_STEPS
	#define SC4_AO_STEPS 8 // Depth samples per direction, on each side.
#endif
#ifndef SC4_DOF_TAPS
	#define SC4_DOF_TAPS 48
#endif

uniform float AoStrength <
	ui_category = "Ambient occlusion"; ui_label = "Strength";
	ui_type = "slider"; ui_min = 0.0; ui_max = 3.0; ui_step = 0.05;
> = 1.5;
uniform float AoRadius <
	ui_category = "Ambient occlusion"; ui_label = "Radius (m)";
	ui_tooltip = "How far away geometry still occludes. A zone tile is 16 m wide.";
	ui_type = "slider"; ui_min = 2.0; ui_max = 64.0; ui_step = 1.0;
> = 16.0;

uniform float HazeDensity <
	ui_category = "Atmosphere"; ui_label = "Haze per km";
	ui_tooltip = "Aerial perspective; mostly visible when zoomed far out.";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.1;
uniform float3 HazeColor <
	ui_category = "Atmosphere"; ui_label = "Haze color";
	ui_type = "color";
> = float3(0.60, 0.70, 0.85);

uniform float GlowNight <
	ui_category = "Lights"; ui_label = "Night glow";
	ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.05;
> = 0.8;
uniform float GlowDay <
	ui_category = "Lights"; ui_label = "Day glow";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.05;
uniform float GlowThreshold <
	ui_category = "Lights"; ui_label = "Glow threshold";
	ui_tooltip = "Brightness above which lights glow at night. Daylight never glows below 0.85.";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.35;

uniform float Sharpening <
	ui_category = "Image"; ui_label = "Sharpening";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.4;
uniform float Exposure <
	ui_category = "Image"; ui_label = "Exposure";
	ui_type = "slider"; ui_min = -2.0; ui_max = 2.0; ui_step = 0.05;
> = 0.0;
uniform float Contrast <
	ui_category = "Image"; ui_label = "Contrast";
	ui_type = "slider"; ui_min = 0.5; ui_max = 1.5; ui_step = 0.01;
> = 1.05;
uniform float Saturation <
	ui_category = "Image"; ui_label = "Saturation";
	ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
> = 1.0;
uniform float Vibrance <
	ui_category = "Image"; ui_label = "Vibrance";
	ui_tooltip = "Saturates dull colors more than vivid ones.";
	ui_type = "slider"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
> = 0.15;
uniform float Temperature <
	ui_category = "Image"; ui_label = "Temperature";
	ui_tooltip = "Negative is cooler, positive warmer.";
	ui_type = "slider"; ui_min = -1.0; ui_max = 1.0; ui_step = 0.01;
> = 0.0;
uniform float Vignette <
	ui_category = "Image"; ui_label = "Vignette";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.25;

uniform int FocusMode <
	ui_category = "Miniature"; ui_label = "Focus on";
	ui_type = "combo"; ui_items = "Mouse cursor\0Screen center\0";
> = 0;
uniform float FocusBand <
	ui_category = "Miniature"; ui_label = "Sharp band";
	ui_tooltip = "Depth kept sharp around the focus, as a share of the screen height.";
	ui_type = "slider"; ui_min = 0.0; ui_max = 0.5; ui_step = 0.01;
> = 0.04;
uniform float MiniatureBlur <
	ui_category = "Miniature"; ui_label = "Blur";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.6;
uniform bool ShowFocus <
	ui_category = "Miniature"; ui_label = "Show sharp band";
> = false;

uniform int DebugView <
	ui_category = "Debug"; ui_label = "Show";
	ui_tooltip = "Magenta means SCD3D11 did not provide the city camera: update it, or this is not the city view.";
	ui_type = "combo"; ui_items = "Image\0Ambient occlusion\0Normals\0Glow\0";
> = 0;

uniform float2 MousePoint < source = "mousepoint"; >;
uniform float Timer < source = "timer"; >;

texture SC4AOTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = R16F; };
texture SC4AOBlurTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = R16F; };
texture SC4SceneTex { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RGBA16F; };
texture SC4LumTex { Width = 256; Height = 256; Format = R16F; MipLevels = 9; };
// Log average luminance, focus distance, haze reference distance, timer of the last update.
texture SC4StateTex { Format = RGBA32F; };
texture SC4StatePrevTex { Format = RGBA32F; };
texture SC4Glow1Tex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };
texture SC4Glow2Tex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = RGBA16F; };
texture SC4Glow3Tex { Width = BUFFER_WIDTH / 8; Height = BUFFER_HEIGHT / 8; Format = RGBA16F; };
texture SC4Glow4Tex { Width = BUFFER_WIDTH / 16; Height = BUFFER_HEIGHT / 16; Format = RGBA16F; };
texture SC4Glow5Tex { Width = BUFFER_WIDTH / 32; Height = BUFFER_HEIGHT / 32; Format = RGBA16F; };
texture SC4Glow6Tex { Width = BUFFER_WIDTH / 64; Height = BUFFER_HEIGHT / 64; Format = RGBA16F; };
texture SC4BlurTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = RGBA16F; };

sampler SC4AO { Texture = SC4AOTex; };
sampler SC4AOBlur { Texture = SC4AOBlurTex; };
sampler SC4Scene { Texture = SC4SceneTex; };
sampler SC4Lum { Texture = SC4LumTex; };
sampler SC4State { Texture = SC4StateTex; };
sampler SC4StatePrev { Texture = SC4StatePrevTex; };
sampler SC4Glow1 { Texture = SC4Glow1Tex; };
sampler SC4Glow2 { Texture = SC4Glow2Tex; };
sampler SC4Glow3 { Texture = SC4Glow3Tex; };
sampler SC4Glow4 { Texture = SC4Glow4Tex; };
sampler SC4Glow5 { Texture = SC4Glow5Tex; };
sampler SC4Glow6 { Texture = SC4Glow6Tex; };
sampler SC4Blur { Texture = SC4BlurTex; };

static const int2 kHalfSize = int2(BUFFER_WIDTH / 2, BUFFER_HEIGHT / 2);

float4 ReadState()
{
	return tex2Dfetch(SC4State, int2(0, 0));
}

// ---- State ----------------------------------------------------------------------------------------------

float GroundDistance(float2 uv, float fallback)
{
	float raw = RawDepth(uv);
	return IsSky(raw) ? fallback : ViewDistance(raw);
}

float4 PS_State(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float4 previous = tex2Dfetch(SC4StatePrev, int2(0, 0));
	float elapsed = Timer - previous.a;
	// By elapsed time: SC4 redraws the city view at no fixed rate. A second technique in the same frame sees
	// no elapsed time and leaves the state alone.
	float blend = previous.a <= 0.0 || elapsed < 0.0 ? 1.0 : 1.0 - exp(-elapsed / 250.0);

	float4 target = float4(tex2Dlod(SC4Lum, float4(0.5, 0.5, 0.0, 8.0)).r, previous.g, previous.b, 0.0);
	float snapReference = 0.0;
	if (HasCamera())
	{
		float2 focusUV = FocusMode == 0 ? MousePoint * BUFFER_PIXEL_SIZE : float2(0.5, 0.5);
		target.g = GroundDistance(saturate(focusUV), previous.g);
		// The farthest of a few samples along the bottom edge: the nearest ground, rooftops left out.
		float reference = max(max(GroundDistance(float2(0.1, 0.98), -1e9), GroundDistance(float2(0.5, 0.98), -1e9)),
			GroundDistance(float2(0.9, 0.98), -1e9));
		target.b = reference < -1e8 ? previous.b : reference;
		// Zooming or rotating shifts every distance at once; easing the haze through that would flash.
		snapReference = abs(target.b - previous.b) > 0.25 * ScreenMeters().y ? 1.0 : 0.0;
	}

	float4 state = lerp(previous, target, blend);
	state.b = lerp(state.b, target.b, snapReference);
	state.a = Timer;
	return state;
}

float4 PS_StateCopy(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	return ReadState();
}

// ---- Ambient occlusion ----------------------------------------------------------------------------------

// Cosine-weighted visibility of the arc from the view vector to horizon angle h, around normal angle n (GTAO).
float IntegrateArc(float h, float n)
{
	return 0.25 * (-cos(2.0 * h - n) + cos(n) + 2.0 * h * sin(n));
}

float PS_AO(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	if (!HasCamera() || AoStrength <= 0.0) return 1.0;

	int2 pixel = int2(position.xy);
	float2 uv = CoarseTexelUV(pixel, 2);
	float raw = RawDepth(uv);
	if (IsSky(raw)) return 1.0;

	float3 center = float3(uv * ScreenMeters(), ViewDistance(raw));
	float3 normal = CameraNormal(uv, center);
	float radiusPixels = min(AoRadius / MetersPerPixel(), BUFFER_HEIGHT * 0.1);
	if (radiusPixels < 2.0) return 1.0;
	float falloff = AoRadius * 0.6; // occluders fade out over the outer part of the radius

	float2 jitter = float2(Bayer(pixel), Bayer(int2(pixel.y + 1, pixel.x + 2)));
	float visibility = 0.0;
	for (int slice = 0; slice < SC4_AO_SLICES; slice++)
	{
		// Orthographic: screen directions are camera directions and the view vector is (0, 0, -1) everywhere,
		// so every slice is an exact plane through the pixel.
		float angle = (slice + jitter.x) * kPi / SC4_AO_SLICES;
		float2 direction = float2(cos(angle), sin(angle));
		float3 axis = float3(-direction.y, direction.x, 0.0);
		float3 projected = normal - axis * dot(normal, axis);
		float n = atan2(dot(projected.xy, direction), -projected.z);
		// Without occluders the horizons lie on the hemisphere around the normal.
		float lowForward = -sin(n);
		float lowBackward = sin(n);
		float horizonForward = lowForward;
		float horizonBackward = lowBackward;
		for (int i = 0; i < SC4_AO_STEPS; i++)
		{
			float t = (i + jitter.y) / SC4_AO_STEPS;
			float2 offset = direction * lerp(1.5, radiusPixels, t * t) * BUFFER_PIXEL_SIZE;

			float3 delta = CameraPosition(TexelCenter(uv + offset)) - center;
			float len = length(delta);
			horizonForward = max(horizonForward, lerp(lowForward, -delta.z / len, saturate((AoRadius - len) / falloff)));

			delta = CameraPosition(TexelCenter(uv - offset)) - center;
			len = length(delta);
			horizonBackward = max(horizonBackward, lerp(lowBackward, -delta.z / len, saturate((AoRadius - len) / falloff)));
		}
		float h1 = acos(clamp(horizonForward, -1.0, 1.0));
		float h0 = -acos(clamp(horizonBackward, -1.0, 1.0));
		visibility += length(projected) * (IntegrateArc(h0, n) + IntegrateArc(h1, n));
	}
	return visibility / SC4_AO_SLICES;
}

float PS_AODenoise(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	return HasCamera() ? Denoise(SC4AO, int2(position.xy), 2, kHalfSize) : 1.0;
}

// Light bouncing between surfaces brightens occlusion on bright materials (Jimenez et al. 2016).
float3 MultiBounce(float visibility, float3 albedo)
{
	float3 a = 2.0404 * albedo - 0.3324;
	float3 b = -4.7951 * albedo + 0.6417;
	float3 c = 2.7552 * albedo + 0.6903;
	return max(visibility, ((visibility * a + b) * visibility + c) * visibility);
}

// ---- Scene: occlusion and haze, in linear color ----------------------------------------------------------

float4 PS_Scene(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = tex2D(SC4Color, texcoord).rgb;
	float raw = RawDepth(texcoord);
	if (!HasCamera() || IsSky(raw)) return float4(color, 1.0);

	float viewDistance = ViewDistance(raw);
	float4 state = ReadState();
	float average = exp2(state.r);

	float visibility = pow(saturate(Upsample(SC4AOBlur, position.xy, viewDistance, 2, kHalfSize)), AoStrength);
	// Lit windows at night are light sources, not surfaces in the shade.
	float emissive = smoothstep(average * 6.0, average * 16.0, dot(color, kLuma));
	color *= lerp(MultiBounce(visibility, color), 1.0, emissive);

	float haze = 1.0 - exp(-HazeDensity * max(viewDistance - state.b, 0.0) * 0.001);
	color = lerp(color, HazeColor * HazeColor * saturate(average / 0.18), haze);
	return float4(color, 1.0);
}

float PS_Luminance(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	return LogLuminance(texcoord);
}

// ---- Glow -----------------------------------------------------------------------------------------------

float4 PS_GlowPrefilter(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float threshold = lerp(GlowThreshold, max(GlowThreshold, 0.85), Daylight(exp2(ReadState().r)));
	float3 sum = 0.0;
	float weight = 0.0;
	for (int y = -1; y <= 1; y += 2)
	for (int x = -1; x <= 1; x += 2)
	{
		float3 color = tex2D(SC4Scene, texcoord + float2(x, y) * BUFFER_PIXEL_SIZE).rgb;
		// Karis average: a single bright pixel, like a moving car's headlights, cannot make the glow flicker.
		float w = 1.0 / (1.0 + max(color.r, max(color.g, color.b)));
		sum += color * w;
		weight += w;
	}
	float3 color = sum / weight;
	float brightness = max(color.r, max(color.g, color.b));
	float knee = threshold * 0.5 + 0.0001;
	float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
	soft = soft * soft / (4.0 * knee);
	return float4(color * max(soft, brightness - threshold) / max(brightness, 0.0001), 1.0);
}

float3 Downsample(sampler source, float2 uv, float2 texel)
{
	float3 center = tex2D(source, uv).rgb;
	float3 inner = tex2D(source, uv + texel * float2(-1.0, -1.0)).rgb + tex2D(source, uv + texel * float2(1.0, -1.0)).rgb
		+ tex2D(source, uv + texel * float2(-1.0, 1.0)).rgb + tex2D(source, uv + texel * float2(1.0, 1.0)).rgb;
	float3 edges = tex2D(source, uv + texel * float2(0.0, -2.0)).rgb + tex2D(source, uv + texel * float2(-2.0, 0.0)).rgb
		+ tex2D(source, uv + texel * float2(2.0, 0.0)).rgb + tex2D(source, uv + texel * float2(0.0, 2.0)).rgb;
	float3 corners = tex2D(source, uv + texel * float2(-2.0, -2.0)).rgb + tex2D(source, uv + texel * float2(2.0, -2.0)).rgb
		+ tex2D(source, uv + texel * float2(-2.0, 2.0)).rgb + tex2D(source, uv + texel * float2(2.0, 2.0)).rgb;
	return center * 0.125 + inner * 0.125 + edges * 0.0625 + corners * 0.03125;
}

float3 UpsampleTent(sampler source, float2 uv, float2 texel)
{
	float3 center = tex2D(source, uv).rgb * 4.0;
	float3 edges = tex2D(source, uv + texel * float2(0.0, -1.0)).rgb + tex2D(source, uv + texel * float2(-1.0, 0.0)).rgb
		+ tex2D(source, uv + texel * float2(1.0, 0.0)).rgb + tex2D(source, uv + texel * float2(0.0, 1.0)).rgb;
	float3 corners = tex2D(source, uv + texel * float2(-1.0, -1.0)).rgb + tex2D(source, uv + texel * float2(1.0, -1.0)).rgb
		+ tex2D(source, uv + texel * float2(-1.0, 1.0)).rgb + tex2D(source, uv + texel * float2(1.0, 1.0)).rgb;
	return (center + edges * 2.0 + corners) / 16.0;
}

float4 PS_Glow2(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(Downsample(SC4Glow1, uv, 2.0 * BUFFER_PIXEL_SIZE), 1.0); }
float4 PS_Glow3(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(Downsample(SC4Glow2, uv, 4.0 * BUFFER_PIXEL_SIZE), 1.0); }
float4 PS_Glow4(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(Downsample(SC4Glow3, uv, 8.0 * BUFFER_PIXEL_SIZE), 1.0); }
float4 PS_Glow5(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(Downsample(SC4Glow4, uv, 16.0 * BUFFER_PIXEL_SIZE), 1.0); }
float4 PS_Glow6(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(Downsample(SC4Glow5, uv, 32.0 * BUFFER_PIXEL_SIZE), 1.0); }
// Added onto the next finer level, which ends up holding the sum of all six.
float4 PS_Glow5Up(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(UpsampleTent(SC4Glow6, uv, 64.0 * BUFFER_PIXEL_SIZE), 0.0); }
float4 PS_Glow4Up(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(UpsampleTent(SC4Glow5, uv, 32.0 * BUFFER_PIXEL_SIZE), 0.0); }
float4 PS_Glow3Up(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(UpsampleTent(SC4Glow4, uv, 16.0 * BUFFER_PIXEL_SIZE), 0.0); }
float4 PS_Glow2Up(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(UpsampleTent(SC4Glow3, uv, 8.0 * BUFFER_PIXEL_SIZE), 0.0); }
float4 PS_Glow1Up(float4 p : SV_Position, float2 uv : TEXCOORD) : SV_Target { return float4(UpsampleTent(SC4Glow2, uv, 4.0 * BUFFER_PIXEL_SIZE), 0.0); }

// ---- Final image ----------------------------------------------------------------------------------------

float3 FetchScene(int2 pixel)
{
	return tex2Dfetch(SC4Scene, clamp(pixel, int2(0, 0), int2(BUFFER_WIDTH - 1, BUFFER_HEIGHT - 1))).rgb;
}

// AMD FidelityFX Contrast Adaptive Sharpening (MIT): sharpens flat detail most, leaves strong edges alone.
float3 Sharpen(int2 pixel)
{
	float3 e = FetchScene(pixel);
	if (Sharpening <= 0.0) return e;
	float3 a = FetchScene(pixel + int2(-1, -1));
	float3 b = FetchScene(pixel + int2(0, -1));
	float3 c = FetchScene(pixel + int2(1, -1));
	float3 d = FetchScene(pixel + int2(-1, 0));
	float3 f = FetchScene(pixel + int2(1, 0));
	float3 g = FetchScene(pixel + int2(-1, 1));
	float3 h = FetchScene(pixel + int2(0, 1));
	float3 i = FetchScene(pixel + int2(1, 1));
	float3 minimum = min(min(min(d, e), min(f, b)), h);
	minimum += min(minimum, min(min(a, c), min(g, i)));
	float3 maximum = max(max(max(d, e), max(f, b)), h);
	maximum += max(maximum, max(max(a, c), max(g, i)));
	float3 amplitude = sqrt(saturate(min(minimum, 2.0 - maximum) / max(maximum, 0.0001)));
	float3 w = amplitude * (-1.0 / lerp(8.0, 5.0, Sharpening));
	return saturate((e + (b + d + f + h) * w) / (1.0 + 4.0 * w));
}

float4 PS_Final(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = Sharpen(int2(position.xy));
	float3 glow = tex2D(SC4Glow1, texcoord).rgb / 6.0;
	color += glow * lerp(GlowNight, GlowDay, Daylight(exp2(ReadState().r)));

	color *= exp2(Exposure);
	float3 balance = float3(1.0 + 0.1 * Temperature, 1.0, 1.0 - 0.1 * Temperature);
	color *= balance / dot(balance, kLuma);
	float luma = dot(color, kLuma);
	float chroma = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
	color = max(lerp(float3(luma, luma, luma), color, Saturation * (1.0 + Vibrance * (1.0 - saturate(chroma)))), 0.0);
	color = 0.18 * pow(color / 0.18, Contrast);
	// Roll off above 0.95 so glow and exposure do not clip hard.
	color = lerp(color, 1.0 - 0.05 * exp((0.95 - color) / 0.05), step(0.95, color));
	color *= 1.0 - Vignette * smoothstep(0.4, 1.2, length((texcoord - 0.5) * float2(BUFFER_ASPECT_RATIO, 1.0)));

	if (DebugView != 0 && DebugView != 3 && !HasCamera())
	{
		color = float3(1.0, 0.0, 1.0);
	}
	else if (DebugView == 1)
	{
		float raw = RawDepth(texcoord);
		color = IsSky(raw) ? 1.0 : pow(saturate(Upsample(SC4AOBlur, position.xy, ViewDistance(raw), 2, kHalfSize)), AoStrength);
	}
	else if (DebugView == 2)
	{
		color = CameraNormal(texcoord, CameraPosition(texcoord)) * 0.5 + 0.5;
	}
	else if (DebugView == 3)
	{
		color = glow;
	}

	// 8-bit output: dither so dark glow and haze gradients do not band.
	float dither = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715)))) - 0.5;
	return float4(LinearToSRGB(saturate(color)) + dither / 255.0, 1.0);
}

// ---- Miniature ------------------------------------------------------------------------------------------

// Signed blur radius in pixels, positive behind the focus. Depth is measured in screen heights, so the sharp
// band covers the same share of the screen at every zoom level.
float BlurRadius(float2 uv, float focus)
{
	float raw = RawDepth(uv);
	float relative = IsSky(raw) ? 1.0 : (ViewDistance(raw) - focus) / ScreenMeters().y;
	return sign(relative) * saturate((abs(relative) - FocusBand) * 4.0) * BUFFER_HEIGHT * 0.025 * MiniatureBlur;
}

float4 PS_MiniatureGather(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 sum = tex2D(SC4Color, texcoord).rgb;
	if (!HasCamera() || MiniatureBlur <= 0.0) return float4(sum, 1.0);

	float focus = ReadState().g;
	float centerRadius = BlurRadius(texcoord, focus);
	float maxRadius = BUFFER_HEIGHT * 0.025 * MiniatureBlur;
	float spacing = maxRadius / sqrt(SC4_DOF_TAPS);
	float count = 1.0;
	// Golden angle spiral, in order of increasing radius (Gustafsson's single pass bokeh).
	for (int i = 0; i < SC4_DOF_TAPS; i++)
	{
		float radius = sqrt((i + 0.5) / SC4_DOF_TAPS) * maxRadius;
		float angle = i * 2.39996323;
		float2 uv = texcoord + float2(cos(angle), sin(angle)) * radius * BUFFER_PIXEL_SIZE;
		float tapRadius = BlurRadius(uv, focus);
		float size = abs(tapRadius);
		// Background blur must not spread over anything sharper in front of it.
		if (tapRadius > centerRadius) size = min(size, abs(centerRadius) * 2.0);
		sum += lerp(sum / count, tex2Dlod(SC4Color, float4(uv, 0.0, 0.0)).rgb, smoothstep(radius - spacing, radius + spacing, size));
		count += 1.0;
	}
	return float4(sum / count, 1.0);
}

float4 PS_MiniatureComposite(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = tex2D(SC4Color, texcoord).rgb;
	if (!HasCamera()) return float4(color, 1.0);

	float sharp = 1.0 - smoothstep(0.5, 2.0, abs(BlurRadius(texcoord, ReadState().g)));
	color = lerp(tex2D(SC4Blur, texcoord).rgb, color, sharp);
	if (ShowFocus) color = lerp(color, float3(0.1, 1.0, 0.2), 0.25 * sharp);
	return float4(color, 1.0);
}

technique SC4 <
	ui_label = "SimCity 4";
	ui_tooltip = "Ambient occlusion, haze, night light glow, sharpening and color for the city view.\n"
	             "Depth effects need SCD3D11 with its ReShade integration.";
>
{
	pass State { VertexShader = PostProcessVS; PixelShader = PS_State; RenderTarget = SC4StateTex; }
	pass StateCopy { VertexShader = PostProcessVS; PixelShader = PS_StateCopy; RenderTarget = SC4StatePrevTex; }
	pass AO { VertexShader = PostProcessVS; PixelShader = PS_AO; RenderTarget = SC4AOTex; }
	pass AODenoise { VertexShader = PostProcessVS; PixelShader = PS_AODenoise; RenderTarget = SC4AOBlurTex; }
	pass Scene { VertexShader = PostProcessVS; PixelShader = PS_Scene; RenderTarget = SC4SceneTex; }
	pass Luminance { VertexShader = PostProcessVS; PixelShader = PS_Luminance; RenderTarget = SC4LumTex; }
	pass GlowPrefilter { VertexShader = PostProcessVS; PixelShader = PS_GlowPrefilter; RenderTarget = SC4Glow1Tex; }
	pass Glow2 { VertexShader = PostProcessVS; PixelShader = PS_Glow2; RenderTarget = SC4Glow2Tex; }
	pass Glow3 { VertexShader = PostProcessVS; PixelShader = PS_Glow3; RenderTarget = SC4Glow3Tex; }
	pass Glow4 { VertexShader = PostProcessVS; PixelShader = PS_Glow4; RenderTarget = SC4Glow4Tex; }
	pass Glow5 { VertexShader = PostProcessVS; PixelShader = PS_Glow5; RenderTarget = SC4Glow5Tex; }
	pass Glow6 { VertexShader = PostProcessVS; PixelShader = PS_Glow6; RenderTarget = SC4Glow6Tex; }
	pass Glow5Up { VertexShader = PostProcessVS; PixelShader = PS_Glow5Up; RenderTarget = SC4Glow5Tex; BlendEnable = true; SrcBlend = ONE; DestBlend = ONE; }
	pass Glow4Up { VertexShader = PostProcessVS; PixelShader = PS_Glow4Up; RenderTarget = SC4Glow4Tex; BlendEnable = true; SrcBlend = ONE; DestBlend = ONE; }
	pass Glow3Up { VertexShader = PostProcessVS; PixelShader = PS_Glow3Up; RenderTarget = SC4Glow3Tex; BlendEnable = true; SrcBlend = ONE; DestBlend = ONE; }
	pass Glow2Up { VertexShader = PostProcessVS; PixelShader = PS_Glow2Up; RenderTarget = SC4Glow2Tex; BlendEnable = true; SrcBlend = ONE; DestBlend = ONE; }
	pass Glow1Up { VertexShader = PostProcessVS; PixelShader = PS_Glow1Up; RenderTarget = SC4Glow1Tex; BlendEnable = true; SrcBlend = ONE; DestBlend = ONE; }
	pass Final { VertexShader = PostProcessVS; PixelShader = PS_Final; }
}

technique SC4Miniature <
	ui_label = "SimCity 4 Miniature";
	ui_tooltip = "Tilt-shift depth of field that makes the city look like a scale model. Place after SimCity 4.";
>
{
	pass State { VertexShader = PostProcessVS; PixelShader = PS_State; RenderTarget = SC4StateTex; }
	pass StateCopy { VertexShader = PostProcessVS; PixelShader = PS_StateCopy; RenderTarget = SC4StatePrevTex; }
	pass Gather { VertexShader = PostProcessVS; PixelShader = PS_MiniatureGather; RenderTarget = SC4BlurTex; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PS_MiniatureComposite; SRGBWriteEnable = true; }
}
