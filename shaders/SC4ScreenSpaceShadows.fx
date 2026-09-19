/*=============================================================================

    SC4ScreenSpaceShadows.fx

    Screen-space directional shadows for SimCity 4, for use with the SCD3D11
    driver's ReShade add-on.

    WHY THIS WORKS WELL HERE
    ------------------------
    SC4's camera is orthographic, and SCD3D11 binds the scene depth to the
    DEPTH semantic already encoded so that ReShade.fxh's linearization returns
    SC4's own linear depth. Under an orthographic projection a directional
    light projects to a *constant* screen-space direction with a *constant*
    depth slope, for every pixel in the frame. The shadow test therefore
    reduces to a straight 2D walk with a fixed depth increment per step -
    essentially a heightfield horizon test, which is the best-behaved variant
    of screen-space shadowing.

    It also does not care what it is shadowing. Buildings, BAT props, True3D
    (RKT0) props, network tiles and puzzle pieces are all just depth, so this
    covers the cases the game's own decal shadows cannot.

    INTENDED USE
    ------------
    Turn the game's own shadows off first, otherwise objects that already get
    decal shadows are darkened twice:

        rp rendershadows 0

    Then tune fSunAngle per camera rotation (see below).

    TUNING ORDER
    ------------
    1. Set iDebugView = 1 (shadow mask) so you can see what you are doing.
    2. Set fSunAngle so shadows point the same way as the terrain lighting.
    3. Set fSunSlope until shadows detach cleanly from tall objects without
       creeping up their sides. If the driver scale is unavailable, tune
       fSunDepthRate instead.
    4. Raise fDepthBias just until acne disappears, no further.
    5. Set iDebugView = 0 and tune fStrength / fShadowTint to taste.

    CAMERA ROTATION
    ---------------
    SC4 rotates the view in 90 degree steps, so the sun's screen-space angle
    changes with it. Set fSunAngle per rotation; the four values are 90
    degrees apart, so note one and add/subtract 90. SC4 does not expose a
    directional fixed-function light for the city view, so the driver cannot
    derive this angle from render state.

=============================================================================*/

#include "ReShade.fxh"

//=============================================================================
// User interface
//=============================================================================

uniform float fSunAngle <
    ui_type     = "slider";
    ui_category = "Sun";
    ui_label    = "Sun angle (screen)";
    ui_tooltip  = "Screen-space direction from a shaded pixel TOWARD the sun.\n"
                  "0 = right, 90 = up, 180 = left, 270 = down.\n"
                  "Shadows fall in the opposite direction.\n"
                  "Add or subtract 90 when you rotate the camera.";
    ui_min = 0.0; ui_max = 360.0; ui_step = 1.0;
> = 135.0;

uniform float fDepthScale <
    ui_type     = "slider";
    ui_category = "Calibration";
    ui_label    = "Depth scale  << CALIBRATE FIRST";
    ui_tooltip  = "Multiplies every depth sample before it is used.\n\n"
                  "SC4's orthographic depth range covers the whole city, so\n"
                  "the depth difference between a prop's top and the ground\n"
                  "beneath it is a tiny fraction of the buffer's range. Left\n"
                  "unscaled, every other setting here is unusable and the\n"
                  "shadow mask comes out empty.\n\n"
                  "HOW TO SET IT:\n"
                  "1. Debug view = 'Depth contrast'.\n"
                  "2. Raise 'Debug contrast gain' until object edges are\n"
                  "   crisp and bright, but the flat terrain is still dark.\n"
                  "3. Put THAT SAME NUMBER here.\n\n"
                  "That gain is roughly 1 / (typical object depth delta), so\n"
                  "copying it here puts object-sized depth steps near 1.0,\n"
                  "which is what the trace defaults below assume.\n\n"
                  "Do NOT use 'Depth bands' for this - those are absolute\n"
                  "depth contours, so they shift as you pan and tell you\n"
                  "about the scene's total range, not about object sizes.";
    ui_min = 1.0; ui_max = 50000.0; ui_step = 1.0;
> = 500.0;

uniform bool bInvertMarch <
    ui_category = "Calibration";
    ui_label    = "Invert march direction";
    ui_tooltip  = "The trace assumes that moving toward the sun moves toward\n"
                  "the camera, so the ray's depth DEcreases. If that sign is\n"
                  "backwards for SC4's projection, nothing is ever occluded\n"
                  "and the mask stays black.\n\n"
                  "If the mask is empty after calibrating Depth scale, toggle\n"
                  "this. It costs nothing to try and is the second most likely\n"
                  "cause of an empty mask.";
> = false;

uniform float fSunDepthRate <
    ui_type     = "slider";
    ui_category = "Sun";
    ui_label    = "Sun elevation (depth rate)";
    ui_tooltip  = "How much SCALED depth the ray gains over one full screen\n"
                  "height of travel toward the sun - the sun's height angle\n"
                  "expressed in depth units.\n\n"
                  "This is relative to Depth scale, so calibrate that first.\n\n"
                  "Too low  : shadows run too far and smear (the artifact the\n"
                  "           game's own decal path produces for True3D props).\n"
                  "Too high : shadows are stubby and cling to object bases.\n\n"
                  "With Depth scale calibrated as described, start around 2\n"
                  "and sweep the whole range - this is the parameter most\n"
                  "likely to need a value far from its default.";
    ui_min = 0.001; ui_max = 50.0; ui_step = 0.001;
> = 2.0;

uniform float fShadowLength <
    ui_type     = "slider";
    ui_category = "Tracing";
    ui_label    = "Max shadow length";
    ui_tooltip  = "Longest shadow that can be cast, as a fraction of screen\n"
                  "height. Directly sets cost: this is how far each pixel\n"
                  "marches. Keep it just above your tallest building's shadow.";
    ui_min = 0.01; ui_max = 0.50; ui_step = 0.005;
> = 0.10;

uniform int iSteps <
    ui_type     = "slider";
    ui_category = "Tracing";
    ui_label    = "Trace steps";
    ui_tooltip  = "Samples per pixel along the ray. Higher is more accurate\n"
                  "and more expensive. Under-sampling shows up as shadows\n"
                  "breaking into dashes; jitter hides some of it.";
    ui_min = 4; ui_max = 64;
> = 24;

uniform float fDepthBias <
    ui_type     = "slider";
    ui_category = "Tracing";
    ui_label    = "Depth bias";
    ui_tooltip  = "Offset before a sample counts as an occluder. Removes\n"
                  "self-shadowing acne on surfaces facing the sun.\n"
                  "Too high detaches shadows from their objects.\n\n"
                  "In calibrated units, so a few percent of an object-sized\n"
                  "depth step is the right ballpark.";
    ui_min = 0.0; ui_max = 5.0; ui_step = 0.001;
> = 0.05;

uniform float fThickness <
    ui_type     = "slider";
    ui_category = "Tracing";
    ui_label    = "Occluder thickness";
    ui_tooltip  = "The depth buffer stores only the nearest surface, so it\n"
                  "cannot tell a real occluder from an unrelated object far\n"
                  "in front. A sample only counts if it is within this much\n"
                  "depth of the ray.\n\n"
                  "Too high : distant objects cast phantom shadows.\n"
                  "Too low  : TALL objects stop casting entirely, while short\n"
                  "           ones still work - the most common reason that\n"
                  "           'some things cast and some do not'.\n\n"
                  "SC4 can take a generous value. The scene is effectively a\n"
                  "heightfield, so something much nearer the camera is usually\n"
                  "genuinely higher and genuinely occluding - unlike in a\n"
                  "first-person game, where this test earns its keep.\n"
                  "Raise it until tall objects cast, then back off if phantom\n"
                  "shadows appear.";
    ui_min = 0.001; ui_max = 200.0; ui_step = 0.01;
> = 20.0;

uniform bool bJitter <
    ui_category = "Tracing";
    ui_label    = "Jitter ray start";
    ui_tooltip  = "Offsets each pixel's first sample by noise, trading\n"
                  "banding for fine grain. The blur cleans up the grain.\n"
                  "Leave on unless you are debugging.";
> = true;

uniform float fSoftnessNear <
    ui_type     = "slider";
    ui_category = "Softness";
    ui_label    = "Softness at contact";
    ui_tooltip  = "Blur radius in pixels where a shadow meets its object.\n"
                  "Keep small - contact shadows should stay sharp.";
    ui_min = 0.0; ui_max = 8.0; ui_step = 0.1;
> = 1.0;

uniform float fSoftnessFar <
    ui_type     = "slider";
    ui_category = "Softness";
    ui_label    = "Softness at full length";
    ui_tooltip  = "Blur radius in pixels at the far end of a shadow.\n"
                  "Fakes a penumbra and matches SC4's soft decal shadows.";
    ui_min = 0.0; ui_max = 24.0; ui_step = 0.1;
> = 6.0;

uniform float fStrength <
    ui_type     = "slider";
    ui_category = "Appearance";
    ui_label    = "Shadow strength";
    ui_min = 0.0; ui_max = 1.0; ui_step = 0.01;
> = 0.70;

uniform float3 fShadowTint <
    ui_type     = "color";
    ui_category = "Appearance";
    ui_label    = "Shadow tint";
    ui_tooltip  = "Multiplied into shadowed pixels. Slightly blue reads as\n"
                  "skylight fill and matches SC4's own shadow colour better\n"
                  "than neutral grey.";
> = float3(0.48, 0.52, 0.64);

uniform float fDepthCutoff <
    ui_type     = "slider";
    ui_category = "Appearance";
    ui_label    = "Sky / void cutoff";
    ui_tooltip  = "Pixels at or beyond this depth are left alone, so the area\n"
                  "outside the map does not get shaded.";
    ui_min = 0.5; ui_max = 1.0; ui_step = 0.001;
> = 0.999;

uniform int iDebugView <
    ui_type  = "combo";
    ui_category = "Debug";
    ui_label = "Debug view";
    ui_tooltip = "DIAGNOSING AN EMPTY MASK - work down this list:\n\n"
                 "1. 'Technique alive' - if the screen does NOT go magenta,\n"
                 "   the effect is not running at all. Nothing else matters;\n"
                 "   check that it is enabled and that the add-on renders it.\n\n"
                 "2. 'Raw depth' - if this is flat black or flat white, the\n"
                 "   DEPTH semantic is not reaching the shader. Fix the depth\n"
                 "   binding before touching anything else.\n\n"
                 "3. 'Depth bands' - raise Depth scale until bands appear.\n"
                 "   No bands at any scale means depth has no usable variation.\n\n"
                 "4. 'Depth contrast' - shows where depth changes between\n"
                 "   neighbouring pixels. Objects should outline themselves.\n"
                 "   If poles show but wires do not, thin alpha-tested geometry\n"
                 "   is missing from the depth buffer - a driver-side issue,\n"
                 "   not a shader one.\n\n"
                 "5. 'Shadow mask' - only meaningful once 1-4 all pass.";
    ui_items = "Off\0Shadow mask\0Penumbra factor\0Raw depth\0Depth bands\0Depth contrast\0Technique alive\0Driver scale status\0";
> = 0;

uniform float fDebugContrast <
    ui_type     = "slider";
    ui_category = "Debug";
    ui_label    = "Debug contrast gain";
    ui_tooltip  = "Amplification for the 'Depth contrast' view only.";
    ui_min = 1.0; ui_max = 20000.0; ui_step = 1.0;
> = 500.0;

//=============================================================================
// Driver-fed uniforms
//
// These carry a "source" annotation, so ReShade hides them from the UI and
// never writes them itself - the SCD3D11 add-on sets them, using the same
// enumerate_uniform_variables + annotation match already used for
// "bufready_depth" in cGDriver_ReShade.cpp.
//
// The add-on derives both scales from SC4's orthographic projection. The sun
// direction remains fSunAngle because SC4 bakes its city lighting into vertex
// colours and does not enable a directional fixed-function light to inspect.
// While the add-on does not set these, bSC4DriverScale stays false and the
// manual depth calibration controls above are used instead.
//=============================================================================

uniform bool bSC4DriverScale < source = "sc4_sun_valid"; > = false;
uniform float fSC4DepthScale < source = "sc4_depth_scale"; > = 1.0;
uniform float fSC4WorldPerScreenHeight < source = "sc4_world_per_screen_height"; > = 1.0;

uniform float fSunSlope <
    ui_type     = "slider";
    ui_category = "Sun";
    ui_label    = "Sun slope (rise / run)";
    ui_tooltip  = "How fast the ray climbs, as a pure ratio: world height\n"
                  "gained per world distance travelled toward the sun.\n\n"
                  "Because it is a ratio it is ZOOM INDEPENDENT - the driver\n"
                  "supplies the world-units-per-screen scale, so this value\n"
                  "stays correct at every zoom level once set.\n\n"
                  "1.0 is a 45 degree sun. Lower = longer shadows.\n\n"
                  "It also absorbs SC4's fixed camera elevation, so treat it\n"
                  "as a number to tune by eye rather than a real sun angle.\n\n"
                  "Only used when the driver is supplying projection scales.";
    ui_min = 0.05; ui_max = 8.0; ui_step = 0.01;
> = 1.0;

//=============================================================================
// Resources
//=============================================================================

// R = shadow (0 lit, 1 shadowed), G = penumbra factor (0 contact, 1 far).
texture2D texSC4Shadow  { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG8; };
sampler2D sSC4Shadow    { Texture = texSC4Shadow;  AddressU = CLAMP; AddressV = CLAMP; };

texture2D texSC4ShadowH { Width = BUFFER_WIDTH; Height = BUFFER_HEIGHT; Format = RG8; };
sampler2D sSC4ShadowH   { Texture = texSC4ShadowH; AddressU = CLAMP; AddressV = CLAMP; };

//=============================================================================
// Helpers
//=============================================================================

// Interleaved gradient noise - cheap, stable per pixel, no texture needed.
float IGN(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// Every depth read goes through here so the calibration scale is applied
// consistently. Raw depth stays in 0..1; scaled depth is what the trace
// parameters are expressed in.
float SampleDepthRaw(float2 uv)
{
    return ReShade::GetLinearizedDepth(uv);
}

// When the driver supplies a scale, depth comes back in world units (metres),
// which is what makes the thresholds below zoom-independent.
float ActiveDepthScale()
{
    return bSC4DriverScale ? fSC4DepthScale : fDepthScale;
}

float SampleDepth(float2 uv)
{
    return ReShade::GetLinearizedDepth(uv) * ActiveDepthScale();
}

// Screen-space direction toward the sun, in pixels, and the matching depth
// change over one screen height. The angle remains user-supplied; when the
// driver scale is present, convert the zoom-independent slope to world units.
void GetSunVectors(out float2 dirPixels, out float depthRate)
{
    float angle = fSunAngle;
    depthRate   = bSC4DriverScale ? (fSunSlope * fSC4WorldPerScreenHeight)
                                  : fSunDepthRate;

    float a = radians(angle);
    // Texture V grows downward, so negate the Y component to make 90 = up.
    dirPixels = float2(cos(a), -sin(a));
}

//=============================================================================
// Pass 1 - trace
//=============================================================================

float2 PS_Trace(float4 vpos : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
    // Cutoff is tested against RAW depth, so it stays meaningful regardless
    // of how the calibration scale is set.
    if (SampleDepthRaw(texcoord) >= fDepthCutoff)
        return float2(0.0, 0.0);

    float centerDepth = SampleDepth(texcoord);

    float2 dirPixels;
    float  depthRate;
    GetSunVectors(dirPixels, depthRate);

    // March length in pixels, and the per-step increments.
    float  marchPixels = fShadowLength * BUFFER_HEIGHT;
    float  stepPixels  = marchPixels / (float)iSteps;

    float2 stepUV = dirPixels * stepPixels * float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT);

    // Depth the ray gains per step. Moving toward the sun means moving up in
    // the world, which under SC4's isometric view should mean moving toward
    // the camera, so depth decreases. bInvertMarch flips that assumption.
    float stepDepth = depthRate * (stepPixels / (float)BUFFER_HEIGHT);
    if (bInvertMarch)
        stepDepth = -stepDepth;

    // Break up banding by starting each pixel at a different sub-step offset.
    float offset = bJitter ? IGN(vpos.xy) : 0.0;

    float2 uv        = texcoord    + stepUV    * offset;
    float  rayDepth  = centerDepth - stepDepth * offset;

    float shadow   = 0.0;
    float penumbra = 0.0;

    [loop]
    for (int i = 0; i < iSteps; ++i)
    {
        uv       += stepUV;
        rayDepth -= stepDepth;

        // Left the screen - no occluder can be found beyond this point.
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            break;

        float sceneDepth = SampleDepth(uv);

        // Occluded when the scene surface sits nearer the camera than the
        // ray, i.e. it rises above the line to the sun.
        float delta = bInvertMarch ? (sceneDepth - rayDepth)
                                   : (rayDepth - sceneDepth);

        if (delta > fDepthBias && delta < fThickness)
        {
            shadow   = 1.0;
            // How far along the ray the hit was: drives penumbra width.
            penumbra = ((float)i + offset) / (float)iSteps;
            break;
        }
    }

    return float2(shadow, penumbra);
}

//=============================================================================
// Passes 2 and 3 - separable blur, radius scaled by penumbra
//=============================================================================

float2 BlurAxis(float2 texcoord, float2 axis)
{
    float2 center = tex2D(sSC4ShadowH, texcoord).rg;

    // Radius grows with distance from the occluder, faking a penumbra.
    float radius = lerp(fSoftnessNear, fSoftnessFar, center.g);

    if (radius < 0.01)
        return center;

    float2 texel = float2(BUFFER_RCP_WIDTH, BUFFER_RCP_HEIGHT) * axis * radius;

    // 9-tap Gaussian.
    const float weight[5] = { 0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162 };

    float2 sum = center * weight[0];

    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        float o = (float)i * 0.5;
        sum += tex2D(sSC4ShadowH, texcoord + texel * o).rg * weight[i];
        sum += tex2D(sSC4ShadowH, texcoord - texel * o).rg * weight[i];
    }

    return sum;
}

float2 PS_BlurX(float4 vpos : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
    return BlurAxis(texcoord, float2(1.0, 0.0));
}

float2 PS_BlurY(float4 vpos : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
    return BlurAxis(texcoord, float2(0.0, 1.0));
}

// The blur reads sSC4ShadowH, so pass 2 needs the trace result copied there.
float2 PS_Copy(float4 vpos : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
    return tex2D(sSC4Shadow, texcoord).rg;
}

//=============================================================================
// Pass 4 - composite
//=============================================================================

float3 PS_Composite(float4 vpos : SV_POSITION, float2 texcoord : TEXCOORD) : SV_TARGET
{
    float3 color = tex2D(ReShade::BackBuffer, texcoord).rgb;
    float2 data  = tex2D(sSC4Shadow, texcoord).rg;
    float  depth = SampleDepthRaw(texcoord);

    // Proves the technique is running even when depth is broken.
    if (iDebugView == 6) return float3(1.0, 0.0, 1.0);

    if (iDebugView == 1) return data.rrr;
    if (iDebugView == 2) return data.ggg;
    if (iDebugView == 3) return depth.rrr;

    // Repeating bands across the depth range. The band count is a direct
    // readout of how much depth variation the scene actually has: if one
    // band covers the whole terrain, depth needs far more scaling.
    if (iDebugView == 4)
        return frac(depth * ActiveDepthScale()).rrr;

    // Green = the driver is supplying projection scales, so manual depth
    // calibration is ignored. Red = falling back to the calibration sliders.
    if (iDebugView == 7)
        return bSC4DriverScale ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);

    // Depth difference against neighbours, amplified. Geometry should
    // outline itself here.
    if (iDebugView == 5)
    {
        float dx = SampleDepthRaw(texcoord + float2(BUFFER_RCP_WIDTH, 0.0)) - depth;
        float dy = SampleDepthRaw(texcoord + float2(0.0, BUFFER_RCP_HEIGHT)) - depth;
        return saturate(sqrt(dx * dx + dy * dy) * fDebugContrast).rrr;
    }

    if (depth >= fDepthCutoff)
        return color;

    float shadow = saturate(data.r) * fStrength;

    return lerp(color, color * fShadowTint, shadow);
}

//=============================================================================

technique SC4ScreenSpaceShadows <
    ui_label   = "SC4 Screen-Space Shadows";
    ui_tooltip = "Directional shadows traced against the depth buffer.\n\n"
                 "Covers everything that is drawn, including True3D props and\n"
                 "puzzle pieces, which the game's own decal shadows cannot.\n\n"
                 "Run 'rp rendershadows 0' first to avoid double shadowing.";
>
{
    pass Trace
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Trace;
        RenderTarget = texSC4Shadow;
    }

    pass Stage
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Copy;
        RenderTarget = texSC4ShadowH;
    }

    pass BlurX
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_BlurX;
        RenderTarget = texSC4Shadow;
    }

    pass Restage
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Copy;
        RenderTarget = texSC4ShadowH;
    }

    pass BlurY
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_BlurY;
        RenderTarget = texSC4Shadow;
    }

    pass Composite
    {
        VertexShader = PostProcessVS;
        PixelShader  = PS_Composite;
    }
}
