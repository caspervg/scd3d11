/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Sunlight for SimCity 4: dynamic sun shadows and sun rays in place of SC4's static shadows.
//
// SC4 draws every building's shadow as a pre-rendered decal on the terrain and bakes hill shadows into the terrain
// colors, all for one fixed sun. While this technique is enabled SCD3D11 switches those off and hands over SC4's
// sun and time of day (scd3d11_sun, scd3d11_light, scd3d11_up). Then:
//  - Every building, tree and hill casts its shadow from the depth of the scene itself. The orthographic camera
//    makes the search towards the sun a straight march with a constant step on screen.
//  - The sun can move: with SC4's clock, at a chosen time, or as a timelapse. Surfaces are relit by the change in
//    sunlight: SC4's own sun divides out the lighting baked into its art, the moving sun lights it again, so
//    facades brighten and darken as the sun comes round and the shade keeps only the sky's blue light.
//  - Sun rays: the haze between the city and the camera scatters sunlight wherever it is not in a building's
//    shadow, drawing shafts past the towers, strongest when looking towards a low sun.
//
// Order in ReShade: SMAA (optional), SimCity 4 Sunlight, SimCity 4, SimCity 4 Miniature.

#include "SimCity4.fxh"

#ifndef SC4_SHADOW_STEPS
	#define SC4_SHADOW_STEPS 48 // Depth samples along each pixel's ray towards the sun.
#endif
#ifndef SC4_RAY_SAMPLES
	#define SC4_RAY_SAMPLES 10 // Points along each view ray through the haze.
#endif
#ifndef SC4_RAY_STEPS
	#define SC4_RAY_STEPS 16 // Depth samples along each of those points' rays towards the sun.
#endif

uniform int SunPath <
	ui_category = "Sun"; ui_label = "Sun";
	ui_tooltip = "SimCity 4's sun matches the lighting baked into the buildings.\n"
	             "The others move it through the day; noon is SimCity 4's sun.";
	ui_type = "combo"; ui_items = "SimCity 4's sun\0SimCity 4's clock\0Fixed time\0Timelapse\0";
> = 0;
uniform float SunLower <
	ui_category = "Sun"; ui_label = "Lower the sun";
	ui_tooltip = "Golden hour for SimCity 4's sun: longer shadows and warmer light.";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.35;
uniform float TimeOfDay <
	ui_category = "Sun"; ui_label = "Time of day";
	ui_tooltip = "The fixed time, and where a timelapse starts. The sun rises at 6:00 and sets at 19:00.";
	ui_type = "slider"; ui_min = 5.0; ui_max = 20.0; ui_step = 0.25;
> = 17.5;
uniform float TimelapseSpeed <
	ui_category = "Sun"; ui_label = "Timelapse speed";
	ui_tooltip = "Hours per minute.";
	ui_type = "slider"; ui_min = 0.1; ui_max = 20.0; ui_step = 0.1;
> = 3.0;

uniform float ShadowStrength <
	ui_category = "Shadows"; ui_label = "Strength";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 1.0;
uniform float ShadowSoftness <
	ui_category = "Shadows"; ui_label = "Softness";
	ui_tooltip = "Penumbrae that widen with distance from the building casting the shadow. 0.3 is the real sun's size.";
	ui_type = "slider"; ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.3;
uniform float ShadowReach <
	ui_category = "Shadows"; ui_label = "Reach";
	ui_tooltip = "The longest shadow, as a share of the screen height.";
	ui_type = "slider"; ui_min = 0.1; ui_max = 1.0; ui_step = 0.01;
> = 0.5;
uniform float TallestBuilding <
	ui_category = "Shadows"; ui_label = "Tallest building (m)";
	ui_tooltip = "Rays towards the sun stop once they rise above this, which keeps their samples close together.";
	ui_type = "slider"; ui_min = 20.0; ui_max = 1000.0; ui_step = 10.0;
> = 350.0;

uniform float DirectLight <
	ui_category = "Light"; ui_label = "Sunlight";
	ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
> = 1.0;
uniform float BakedAmbient <
	ui_category = "Light"; ui_label = "Sky share";
	ui_tooltip = "How much of SimCity 4's baked lighting is sky light rather than sunlight: how bright the shade is.\n"
	             "Clear sky: 0.2 to 0.3. Hazy: 0.4 and up.";
	ui_type = "slider"; ui_min = 0.1; ui_max = 0.9; ui_step = 0.01;
> = 0.25;

uniform float RayStrength <
	ui_category = "Sun rays"; ui_label = "Strength";
	ui_type = "slider"; ui_min = 0.0; ui_max = 2.0; ui_step = 0.01;
> = 0.8;
uniform float RayHaze <
	ui_category = "Sun rays"; ui_label = "Haze per km";
	ui_type = "slider"; ui_min = 0.0; ui_max = 8.0; ui_step = 0.05;
> = 1.2;
uniform float RayHeight <
	ui_category = "Sun rays"; ui_label = "Haze height (m)";
	ui_tooltip = "Thickness of the hazy air over the city. Towers taller than this cut the rays less.";
	ui_type = "slider"; ui_min = 20.0; ui_max = 600.0; ui_step = 5.0;
> = 150.0;

uniform int DebugView <
	ui_category = "Debug"; ui_label = "Show";
	ui_tooltip = "Magenta means SCD3D11 did not provide the city camera and sun: update it, or this is not the city view.";
	ui_type = "combo"; ui_items = "Image\0Sun shadows\0Sun rays\0Relighting\0";
> = 0;

// Set by SCD3D11 while the city view renders, zero otherwise, all in view space: towards the sun along SC4's shadow
// direction (w: SC4's time of day in hours), towards the sun SC4 lights its terrain and models with (w: 1 by day,
// 0 at night), and world up.
uniform float4 SC4Sun < source = "scd3d11_sun"; >;
uniform float4 SC4Light < source = "scd3d11_light"; >;
uniform float4 SC4Up < source = "scd3d11_up"; >;
uniform float Timer < source = "timer"; >;

texture SC4SunShadowTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = R16F; };
texture SC4SunShadowBlurTex { Width = BUFFER_WIDTH / 2; Height = BUFFER_HEIGHT / 2; Format = R16F; };
texture SC4SunRaysTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = R16F; };
texture SC4SunRaysBlurTex { Width = BUFFER_WIDTH / 4; Height = BUFFER_HEIGHT / 4; Format = R16F; };
texture SC4SunLumTex { Width = 256; Height = 256; Format = R16F; MipLevels = 9; };
// Log average luminance, daylight (SC4's day or night, eased), timer of the last update.
texture SC4SunStateTex { Format = RGBA32F; };
texture SC4SunStatePrevTex { Format = RGBA32F; };

sampler SC4SunShadow { Texture = SC4SunShadowTex; };
sampler SC4SunShadowBlur { Texture = SC4SunShadowBlurTex; };
sampler SC4SunRays { Texture = SC4SunRaysTex; };
sampler SC4SunRaysBlur { Texture = SC4SunRaysBlurTex; };
sampler SC4SunLum { Texture = SC4SunLumTex; };
sampler SC4SunState { Texture = SC4SunStateTex; };
sampler SC4SunStatePrev { Texture = SC4SunStatePrevTex; };

static const int2 kHalfSize = int2(BUFFER_WIDTH / 2, BUFFER_HEIGHT / 2);
static const int2 kQuarterSize = int2(BUFFER_WIDTH / 4, BUFFER_HEIGHT / 4);
// How deep behind a surface the depth buffer is trusted to be solid: about a building's depth.
static const float kShadowThickness = 40.0;

bool HasSun()
{
	return HasCamera() && SC4Up.w > 0.0;
}

float3 CameraUp()
{
	return EyeToCamera(SC4Up.xyz);
}

float Hours()
{
	if (SunPath == 1) return SC4Sun.w;
	if (SunPath == 3) return 5.0 + 15.0 * frac(max(TimeOfDay - 5.0 + Timer / 60000.0 * TimelapseSpeed, 0.0) / 15.0);
	return TimeOfDay;
}

// Where the sun path puts a sun SC4 draws along `reference` (camera space). xyz: towards it in camera space;
// w: its height as a share of the reference's, 1 for SC4's own sun and 0 on the horizon.
float4 MoveSun(float3 reference)
{
	float3 up = CameraUp();
	float referenceElevation = asin(clamp(dot(reference, up), -1.0, 1.0));
	float3 towards = normalize(reference - up * dot(reference, up) + float3(1e-5, 0.0, 0.0));
	float height = 1.0 - SunLower;
	float azimuth = 0.0;
	if (SunPath != 0)
	{
		// SC4's sun stands at 12:30; it rises at 6:00 and sets at 19:00 a quarter turn to either side.
		float day = clamp((Hours() - 12.5) / 6.5, -1.3, 1.3);
		azimuth = day * kPi * 0.5;
		height = cos(day * kPi * 0.5);
	}
	float elevation = referenceElevation * height;
	float3 horizontal = towards * cos(azimuth) + cross(up, towards) * sin(azimuth);
	return float4(normalize(horizontal * cos(elevation) + up * sin(elevation)), height);
}

// Casts the shadows, along SC4's shadow direction.
float4 ShadowSun()
{
	return MoveSun(EyeToCamera(SC4Sun.xyz));
}

// Lights the surfaces, along the direction SC4 lit them with, so SC4's own sun changes nothing.
float4 LightSun()
{
	return MoveSun(EyeToCamera(SC4Light.xyz));
}

// White at SimCity 4's own sun height, reddening as the sun sinks and gone below the horizon.
float3 SunColor(float height)
{
	float3 color = height < 0.25
		? lerp(float3(1.0, 0.42, 0.16), float3(1.0, 0.75, 0.50), saturate(height / 0.25))
		: lerp(float3(1.0, 0.75, 0.50), float3(1.0, 1.0, 1.0), saturate((height - 0.25) / 0.6));
	return color * smoothstep(-0.05, 0.08, height);
}

// Sky light: neutral by day, dimmer and bluer at dusk.
float3 SkyLight(float height)
{
	return lerp(float3(0.45, 0.52, 0.75), float3(1.0, 1.0, 1.0), smoothstep(-0.1, 0.7, height));
}

// Length of a view ray through the haze layer over the city.
float HazeSpan()
{
	return RayHeight / max(-CameraUp().z, 0.25);
}

// 1 where the sun reaches a point, 0 where the depth buffer holds something between the point and the sun.
float SunReaches(float2 uv, float3 position, float3 ray, int steps, float jitter)
{
	float metersPerPixel = MetersPerPixel();
	float pixelsPerMeter = length(ray.xy) / metersPerPixel;
	if (pixelsPerMeter < 0.01) return 1.0; // sun straight behind the camera: shadows hide behind their casters
	float2 uvPerMeter = 1.0 / ScreenMeters();
	float start = 1.5 / pixelsPerMeter;
	// Nothing is taller than the tallest building, so a ray above it cannot be blocked any more.
	float reach = min(BUFFER_HEIGHT * ShadowReach / pixelsPerMeter, TallestBuilding / max(dot(ray, CameraUp()), 0.02));
	for (int i = 0; i < steps; i++)
	{
		float t = (i + jitter) / steps;
		float along = lerp(start, reach, t * t);
		float2 sampleUV = uv + ray.xy * along * uvPerMeter;
		if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) break;
		// Positive when the depth buffer holds a surface in front of this point on the ray.
		float gap = position.z + ray.z * along - ViewDistance(RawDepth(sampleUV));
		if (gap > metersPerPixel * 2.0 + along * 0.01 && gap < kShadowThickness) return 0.0;
	}
	return 1.0;
}

// ---- State ----------------------------------------------------------------------------------------------

float PS_Luminance(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	return LogLuminance(texcoord);
}

float4 PS_State(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float4 previous = tex2Dfetch(SC4SunStatePrev, int2(0, 0));
	float elapsed = Timer - previous.a;
	// By elapsed time: SC4 redraws the city view at no fixed rate.
	bool first = previous.a <= 0.0 || elapsed < 0.0;
	float average = tex2Dlod(SC4SunLum, float4(0.5, 0.5, 0.0, 8.0)).r;
	// SC4 switches between day and night at once; the sunlight fades over a couple of seconds instead.
	return float4(lerp(previous.r, average, first ? 1.0 : 1.0 - exp(-elapsed / 250.0)),
		lerp(previous.g, SC4Light.w, first ? 1.0 : 1.0 - exp(-elapsed / 1500.0)), 0.0, Timer);
}

float4 PS_StateCopy(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	return tex2Dfetch(SC4SunState, int2(0, 0));
}

// ---- Shadows, half resolution ---------------------------------------------------------------------------

float PS_Shadow(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	if (!HasSun() || ShadowStrength <= 0.0) return 1.0;

	int2 pixel = int2(position.xy);
	float2 uv = CoarseTexelUV(pixel, 2);
	float raw = RawDepth(uv);
	float4 sun = ShadowSun();
	if (IsSky(raw) || sun.w <= 0.0) return 1.0;

	float3 center = float3(uv * ScreenMeters(), ViewDistance(raw));
	// Faces turned away from the sun get no direct light from relighting already.
	if (dot(CameraNormal(uv, center), sun.xyz) <= 0.0) return 1.0;

	// Aim at a random point of a widened sun disc; the denoise averages that into penumbrae that widen with
	// distance from the caster.
	float3 jitter = float3(Bayer(pixel), Bayer(int2(pixel.y + 1, pixel.x + 2)), Bayer(int2(pixel.x + 3, pixel.y + 1)));
	float3 tangent = normalize(cross(sun.xyz, abs(sun.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
	float3 bitangent = cross(sun.xyz, tangent);
	float angle = jitter.x * 2.0 * kPi;
	float3 ray = normalize(sun.xyz + (tangent * cos(angle) + bitangent * sin(angle)) * sqrt(jitter.y) * ShadowSoftness * 0.016);
	return SunReaches(uv, center, ray, SC4_SHADOW_STEPS, jitter.z);
}

float PS_ShadowDenoise(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	return HasSun() ? Denoise(SC4SunShadow, int2(position.xy), 2, kHalfSize) : 1.0;
}

// ---- Sun rays, quarter resolution -----------------------------------------------------------------------

// The share of the haze along this pixel's view ray that the sun reaches.
float PS_Rays(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	if (!HasSun() || RayStrength <= 0.0 || RayHaze <= 0.0) return 0.0;

	int2 pixel = int2(position.xy);
	float2 uv = CoarseTexelUV(pixel, 4);
	float raw = RawDepth(uv);
	float4 sun = ShadowSun();
	if (sun.w <= 0.0) return 0.0;

	float3 surface = float3(uv * ScreenMeters(), ViewDistance(IsSky(raw) ? 0.999999 : raw));
	float span = HazeSpan();
	float2 jitter = float2(Bayer(pixel), Bayer(int2(pixel.y + 2, pixel.x + 1)));
	float lit = 0.0;
	for (int i = 0; i < SC4_RAY_SAMPLES; i++)
	{
		// Orthographic: every point of the view ray lies on this same pixel, only closer to the camera.
		float3 air = surface - float3(0.0, 0.0, span * (i + jitter.x) / SC4_RAY_SAMPLES);
		lit += SunReaches(uv, air, sun.xyz, SC4_RAY_STEPS, frac(jitter.y + i * 0.618034));
	}
	return lit / SC4_RAY_SAMPLES;
}

float PS_RaysDenoise(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	return HasSun() ? Denoise(SC4SunRays, int2(position.xy), 4, kQuarterSize) : 0.0;
}

// ---- Composite ------------------------------------------------------------------------------------------

float4 PS_Composite(float4 position : SV_Position, float2 texcoord : TEXCOORD) : SV_Target
{
	float3 color = tex2D(SC4Color, texcoord).rgb;
	float dither = frac(52.9829189 * frac(dot(position.xy, float2(0.06711056, 0.00583715)))) - 0.5;
	if (!HasSun())
	{
		if (DebugView != 0) color = float3(1.0, 0.0, 1.0);
		return float4(LinearToSRGB(saturate(color)) + dither / 255.0, 1.0);
	}

	float raw = RawDepth(texcoord);
	float viewDistance = ViewDistance(IsSky(raw) ? 0.999999 : raw);
	float4 sun = LightSun();
	float4 state = tex2Dfetch(SC4SunState, int2(0, 0));
	float average = exp2(state.r);
	// SC4's night has no sun, whatever time the sun path is set to.
	float day = state.g;
	float3 sunColor = SunColor(sun.w) * DirectLight;

	float shadow = 1.0;
	float3 relight = 1.0;
	if (!IsSky(raw))
	{
		float3 normal = CameraNormal(texcoord, float3(texcoord * ScreenMeters(), viewDistance));
		shadow = lerp(1.0, Upsample(SC4SunShadowBlur, position.xy, viewDistance, 2, kHalfSize), ShadowStrength);
		// SC4's art is lit by SC4's sun: divide that lighting out, then light with the moving sun and its shadows.
		float baked = BakedAmbient + (1.0 - BakedAmbient) * saturate(dot(normal, EyeToCamera(SC4Light.xyz)));
		float3 lit = BakedAmbient * SkyLight(sun.w) + (1.0 - BakedAmbient) * saturate(dot(normal, sun.xyz)) * shadow * sunColor;
		// Lit windows at night are light sources, not surfaces the sun lights.
		float emissive = smoothstep(average * 6.0, average * 16.0, dot(color, kLuma));
		relight = lerp(1.0, min(lit / max(baked, 0.05), 3.0), day * (1.0 - emissive));
		color *= relight;
	}

	// Haze scatters sunlight towards the camera wherever the sun reaches it, most when looking into a low sun.
	float rays = Upsample(SC4SunRaysBlur, position.xy, viewDistance, 4, kQuarterSize);
	float scatter = (1.0 - exp(-RayHaze * 0.001 * HazeSpan())) * RayStrength * day;
	float phase = lerp(0.3, 2.5, pow(saturate(sun.z * 0.5 + 0.5), 3.0));
	color = color * (1.0 - scatter * 0.35) + sunColor * rays * phase * scatter;

	if (DebugView == 1) color = shadow;
	else if (DebugView == 2) color = rays;
	else if (DebugView == 3) color = relight * 0.5;

	return float4(LinearToSRGB(saturate(color)) + dither / 255.0, 1.0);
}

technique SC4Sunlight <
	ui_label = "SimCity 4 Sunlight";
	ui_tooltip = "Dynamic sun shadows and sun rays that replace SimCity 4's static shadows while enabled.\n"
	             "Needs SCD3D11 with its ReShade integration. Place before SimCity 4.";
	scd3d11_replaces_shadows = true;
>
{
	pass Luminance { VertexShader = PostProcessVS; PixelShader = PS_Luminance; RenderTarget = SC4SunLumTex; }
	pass State { VertexShader = PostProcessVS; PixelShader = PS_State; RenderTarget = SC4SunStateTex; }
	pass StateCopy { VertexShader = PostProcessVS; PixelShader = PS_StateCopy; RenderTarget = SC4SunStatePrevTex; }
	pass Shadow { VertexShader = PostProcessVS; PixelShader = PS_Shadow; RenderTarget = SC4SunShadowTex; }
	pass ShadowDenoise { VertexShader = PostProcessVS; PixelShader = PS_ShadowDenoise; RenderTarget = SC4SunShadowBlurTex; }
	pass Rays { VertexShader = PostProcessVS; PixelShader = PS_Rays; RenderTarget = SC4SunRaysTex; }
	pass RaysDenoise { VertexShader = PostProcessVS; PixelShader = PS_RaysDenoise; RenderTarget = SC4SunRaysBlurTex; }
	pass Composite { VertexShader = PostProcessVS; PixelShader = PS_Composite; }
}
