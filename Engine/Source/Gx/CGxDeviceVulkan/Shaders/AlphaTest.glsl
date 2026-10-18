// D3D's alpha test as the engine enables it, for the fragment programs: GREATEREQUAL against an
// 8-bit reference (pc.params.x), on the shader output rounded to 8 bits. A reference of 0 is the
// test turned off. A negative reference is gxAlphaToCoverage (an MSAA pipeline with
// alphaToCoverageEnable): alpha is sharpened to a one-pixel ramp around -reference, so the coverage
// mask anti-aliases the cut-out edge instead of dithering it; an uncovered pixel still discards.
// Included after Push.glsl.
void AlphaTest(inout float alpha)
{
    if (pc.params.x < 0.0)
    {
        alpha = clamp((alpha + pc.params.x / 255.0) / max(fwidth(alpha), 1e-4) + 0.5, 0.0, 1.0);
        if (alpha <= 0.0) discard;
    }
    else if (pc.params.x > 0.0 && floor(alpha * 255.0 + 0.5) < pc.params.x) discard;
}
