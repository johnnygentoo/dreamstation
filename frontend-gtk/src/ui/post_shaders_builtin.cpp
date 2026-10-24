// SPDX-License-Identifier: GPL-3.0-or-later
//
// Built-in post-processing shaders, written for DreamStation in the
// Dolphin/DuckStation GLSL post-shader dialect (see post_chain.hpp).
#include <map>
#include <string>

namespace ds::frontend {

const std::map<std::string, std::string>& builtin_post_shaders() {
    static const std::map<std::string, std::string> shaders = {

{"dreamstation/sharp-bilinear", R"glsl(/*
[configuration]

[OptionRangeFloat]
GUIName = Sharpness
OptionName = sharpness
MinValue = 1.0
MaxValue = 8.0
StepAmount = 0.5
DefaultValue = 4.0
*/
// Crisp edges without nearest-neighbour shimmer: the interpolation ramp
// between two source pixels is narrowed instead of removed.
void main()
{
    float2 src   = GetOriginalSize();
    float2 px    = GetCoordinates() * src;
    float2 texel = floor(px - 0.5) + 0.5;
    float2 f     = px - texel;
    float2 scale = max(GetResolution() / src, float2(1.0, 1.0)) * (GetOption(sharpness) / 4.0);
    f = clamp((f - 0.5) * scale + 0.5, 0.0, 1.0);
    SetOutput(float4(SampleLocation((texel + f) / src).rgb, 1.0));
}
)glsl"},

{"dreamstation/crt", R"glsl(/*
[configuration]

[OptionRangeFloat]
GUIName = Scanline Strength
OptionName = scan_strength
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.25

[OptionRangeFloat]
GUIName = Mask Strength
OptionName = mask_strength
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.2

[OptionRangeFloat]
GUIName = Curvature
OptionName = curvature
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.1
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Vignette
OptionName = vignette
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.25

[OptionRangeFloat]
GUIName = Brightness
OptionName = gain
MinValue = 0.8
MaxValue = 1.6
StepAmount = 0.02
DefaultValue = 1.12

[OptionRangeInteger]
GUIName = Scanline Count (0 = native 480)
OptionName = lines
MinValue = 0
MaxValue = 1080
StepAmount = 120
DefaultValue = 0
*/
// A television tube: scanlines, an aperture-grille RGB mask, gentle barrel
// curvature and a vignette.
float2 curve(float2 uv, float amount)
{
    uv = uv * 2.0 - 1.0;
    float2 off = abs(uv.yx) / float2(6.0, 5.0);
    uv += uv * off * off * amount;
    return uv * 0.5 + 0.5;
}

void main()
{
    float2 uv = curve(GetCoordinates(), GetOption(curvature));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        SetOutput(float4(0.0, 0.0, 0.0, 1.0));
        return;
    }
    float3 col = SampleLocation(uv).rgb;

    float count = GetOption(lines) > 0 ? float(GetOption(lines)) : GetNativeSize().y;
    float scan  = sin(uv.y * count * 3.14159265) * 0.5 + 0.5;
    col *= lerp(1.0 - GetOption(scan_strength), 1.0, pow(scan, 1.5));

    float m = mod(GetFragCoord().x, 3.0);
    float3 mask = (m < 1.0) ? float3(1.0, 0.0, 0.0) : (m < 2.0) ? float3(0.0, 1.0, 0.0)
                                                                 : float3(0.0, 0.0, 1.0);
    col *= lerp(float3(1.0, 1.0, 1.0), mask * 1.35 + 0.55, GetOption(mask_strength));

    float2 v  = uv * (1.0 - uv.yx);
    float vig = pow(clamp(v.x * v.y * 22.0, 0.0, 1.0), GetOption(vignette));
    col *= vig;

    SetOutput(float4(saturate(col * GetOption(gain)), 1.0));
}
)glsl"},

{"dreamstation/scanlines", R"glsl(/*
[configuration]

[OptionRangeFloat]
GUIName = Strength
OptionName = strength
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.35

[OptionRangeInteger]
GUIName = Line Count (0 = native 480)
OptionName = lines
MinValue = 0
MaxValue = 1080
StepAmount = 120
DefaultValue = 0
*/
// Plain horizontal scanlines, nothing else.
void main()
{
    float2 uv   = GetCoordinates();
    float count = GetOption(lines) > 0 ? float(GetOption(lines)) : GetNativeSize().y;
    float scan  = sin(uv.y * count * 3.14159265) * 0.5 + 0.5;
    float3 col  = Sample().rgb * lerp(1.0 - GetOption(strength), 1.0, scan);
    SetOutput(float4(col, 1.0));
}
)glsl"},

{"dreamstation/fxaa", R"glsl(/*
[configuration]

[OptionRangeFloat]
GUIName = Edge Threshold
OptionName = edge_threshold
MinValue = 0.03
MaxValue = 0.33
StepAmount = 0.01
DefaultValue = 0.12

[OptionRangeFloat]
GUIName = Blend Amount
OptionName = blend
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.75
*/
// Fast approximate anti-aliasing: find luminance edges and blend along them.
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

void main()
{
    float2 uv = GetCoordinates();
    float2 px = GetInvResolution();
    float3 c  = SampleLocation(uv).rgb;
    float lM  = luma(c);
    float lNW = luma(SampleLocation(uv + float2(-1.0, -1.0) * px).rgb);
    float lNE = luma(SampleLocation(uv + float2( 1.0, -1.0) * px).rgb);
    float lSW = luma(SampleLocation(uv + float2(-1.0,  1.0) * px).rgb);
    float lSE = luma(SampleLocation(uv + float2( 1.0,  1.0) * px).rgb);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    if (lMax - lMin < max(0.0312, lMax * GetOption(edge_threshold)))
    {
        SetOutput(float4(c, 1.0));
        return;
    }
    float2 dir = float2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 0.0078125);
    float inv = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * inv, float2(-8.0, -8.0), float2(8.0, 8.0)) * px;
    float3 a = 0.5 * (SampleLocation(uv + dir * (1.0 / 3.0 - 0.5)).rgb +
                      SampleLocation(uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    float3 b = a * 0.5 + 0.25 * (SampleLocation(uv + dir * -0.5).rgb +
                                 SampleLocation(uv + dir * 0.5).rgb);
    float lB = luma(b);
    float3 aa = (lB < lMin || lB > lMax) ? a : b;
    SetOutput(float4(lerp(c, aa, GetOption(blend)), 1.0));
}
)glsl"},

{"dreamstation/brightness", R"glsl(/*
[configuration]

[OptionRangeFloat]
GUIName = Brightness Scale
OptionName = BRIGHTNESS_SCALE
MinValue = 0.1
MaxValue = 5.0
StepAmount = 0.1
DefaultValue = 1.0
*/
// Scales luminance only, leaving hue and saturation alone.
void main()
{
    float3 c = Sample().rgb;
    float  y = dot(c, float3(0.299, 0.587, 0.114));
    float  scaled = clamp(y * GetOption(BRIGHTNESS_SCALE), 0.0, 1.0);
    SetOutput(float4(clamp(c + (scaled - y), 0.0, 1.0), 1.0));
}
)glsl"},

{"hyllian/ddt", R"glsl(//    Hyllian's DDT Shader
//
//    Copyright (C) 2011-2024 Hyllian - sergiogdb@gmail.com
//
//    Permission is hereby granted, free of charge, to any person obtaining a copy
//    of this software and associated documentation files (the "Software"), to deal
//    in the Software without restriction, including without limitation the rights
//    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//    copies of the Software, and to permit persons to whom the Software is
//    furnished to do so, subject to the following conditions:
//
//    The above copyright notice and this permission notice shall be included in
//    all copies or substantial portions of the Software.
//
//    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//    THE SOFTWARE.
//
// DreamStation change: the grid is the size of the image this stage receives,
// not the console's 480 lines, so a high-resolution image is never reduced.
/*
[configuration]

[OptionRangeFloat]
GUIName = Bilinear Fallback Threshold
OptionName = BIL_FALLBACK
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.6

[/configuration]
*/

const vec3 Y = vec3(0.2126729, 0.7151522, 0.0721750);

float luma(vec3 color)
{
    return dot(color, Y);
}

vec3 bilinear(float p, float q, vec3 A, vec3 B, vec3 C, vec3 D)
{
    return ((1-p)*(1-q)*A + p*(1-q)*B + (1-p)*q*C + p*q*D);
}

void main()
{
    vec2 texCoord = GetCoordinates();
    vec2 nativeSize = vec2(textureSize(samp0, 0));

    vec2 loc = texCoord*nativeSize;
    vec2 pos = fract(loc) - vec2(0.5, 0.5); // pos = pixel position
    vec2 dir = sign(pos); // dir = pixel direction

    vec2 dx = vec2(1.0/nativeSize.x, 0.0);
    vec2 dy = vec2(0.0, 1.0/nativeSize.y);

    vec2 g1 = dir*dx;
    vec2 g2 = dir*dy;

    vec2 tc = (floor(loc)+vec2(0.5,0.5))/nativeSize;

    vec3 A = SampleLocation(tc       ).rgb;
    vec3 B = SampleLocation(tc +g1   ).rgb;
    vec3 C = SampleLocation(tc    +g2).rgb;
    vec3 D = SampleLocation(tc +g1+g2).rgb;

    float a = luma(A);
    float b = luma(B);
    float c = luma(C);
    float d = luma(D);

    float p = abs(pos.x);
    float q = abs(pos.y);

    float k = distance(pos,g1);
    float l = distance(pos,g2);

    float wd1 = abs(a-d);
    float wd2 = abs(b-c);

    vec3 color = bilinear(p, q, A, B, C, D);

    if ( wd1 < wd2 )
    {
        if (k < l)
        {
            C = A + D - B;
        }
        else
        {
            B = A + D - C;
        }
    }
    else if (wd1 > wd2)
    {
        D = B + C - A;
    }

    vec3 ddt = bilinear(p, q, A, B, C, D);

    color = mix(color, ddt, smoothstep(0.0, BIL_FALLBACK, abs(wd2-wd1)));

    SetOutput(vec4(color, 1.0));
}
)glsl"},

{"dreamstation/color", R"glsl(/*
[configuration]

[OptionRangeFloat]
GUIName = Brightness
OptionName = brightness
MinValue = 0.5
MaxValue = 1.5
StepAmount = 0.02
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Contrast
OptionName = contrast
MinValue = 0.5
MaxValue = 1.5
StepAmount = 0.02
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Saturation
OptionName = saturation
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.05
DefaultValue = 1.0

[OptionRangeFloat]
GUIName = Gamma
OptionName = gamma
MinValue = 0.5
MaxValue = 2.0
StepAmount = 0.02
DefaultValue = 1.0
*/
// Brightness, contrast, saturation and gamma.
void main()
{
    float3 c = Sample().rgb;
    c = (c - 0.5) * GetOption(contrast) + 0.5;
    c *= GetOption(brightness);
    float grey = dot(c, float3(0.299, 0.587, 0.114));
    c = lerp(float3(grey, grey, grey), c, GetOption(saturation));
    c = pow(max(c, float3(0.0, 0.0, 0.0)), float3(1.0, 1.0, 1.0) / GetOption(gamma));
    SetOutput(float4(saturate(c), 1.0));
}
)glsl"},

    };
    return shaders;
}

} // namespace ds::frontend
