// D3D9's fixed-function texture stages 0 and 1 as the engine configures them, for the fragment
// programs. Included after Push.glsl.
//
// One texture stage op (CGxDeviceD3d's s_texOp / s_texArgs tables) on one channel set. `t` is the
// stage's texture, `c` the current colour (the diffuse at stage 0, stage 0's result at stage 1), `ca`
// the current alpha, `da` the diffuse alpha.
//
//   0 Mod                MODULATE          texture * current
//   1 Mod2x              MODULATE2X        2 * texture * current
//   2 Add                ADD               texture + current
//   3 PassThru           SELECTARG2        current (the texture is ignored)
//   4 Decal              BLENDCURRENTALPHA current * ca + texture * (1 - ca)
//   5 Fade               BLENDDIFFUSEALPHA texture * da + current * (1 - da)
//
// Every result saturates, as a D3D stage's does.
vec3 StageOp(uint op, vec3 t, vec3 c, float ca, float da)
{
    vec3 r;
    if      (op == 0u) r = t * c;
    else if (op == 1u) r = 2.0 * t * c;
    else if (op == 2u) r = t + c;
    else if (op == 3u) r = c;
    else if (op == 4u) r = c * ca + t * (1.0 - ca);
    else               r = t * da + c * (1.0 - da);
    return clamp(r, 0.0, 1.0);
}

// Both stages over the diffuse colour: stage 0 when it has a texture, then stage 1 when it has one
// too (D3D disables a stage without a texture, and every stage after a disabled one).
vec4 Stages(vec4 diffuse, vec4 t0, vec4 t1)
{
    uint textured = uint(pc.params.y + 0.5);
    uint colourOps = uint(pc.params.z + 0.5);
    uint alphaOps  = uint(pc.params.w + 0.5);
    vec4 cur = diffuse;
    if ((textured & 1u) != 0u)
    {
        cur.rgb = StageOp(colourOps & 7u, t0.rgb, diffuse.rgb, diffuse.a, diffuse.a);
        cur.a   = StageOp(alphaOps & 7u, t0.aaa, diffuse.aaa, diffuse.a, diffuse.a).x;
        if ((textured & 2u) != 0u)
        {
            vec4 prev = cur;
            cur.rgb = StageOp(colourOps >> 3u, t1.rgb, prev.rgb, prev.a, diffuse.a);
            cur.a   = StageOp(alphaOps >> 3u, t1.aaa, prev.aaa, prev.a, diffuse.a).x;
        }
    }
    return cur;
}
