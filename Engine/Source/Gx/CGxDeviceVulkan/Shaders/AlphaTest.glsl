// D3D's alpha test as the engine enables it, for the fragment programs: GREATEREQUAL against an
// 8-bit reference (pc.params.x), on the shader output rounded to 8 bits. A reference of 0 is the
// test turned off. Included after Push.glsl.
void AlphaTest(float alpha)
{
    if (pc.params.x > 0.0 && floor(alpha * 255.0 + 0.5) < pc.params.x) discard;
}
