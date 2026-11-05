// Arithmetic the hardware does its own way: sin and cos, frac, floor,
// division, reciprocal square roots, min and max, comparisons to selects,
// derivatives, sign, step, smoothstep, a cross product.
float4 A; float4 B;
float4 VS(float4 p : POSITION) : POSITION { return p; }
float4 PS(float4 v : TEXCOORD0, float4 w : TEXCOORD1) : COLOR0
{
    float4 r;
    r.x = sin(v.x) * cos(v.y) + frac(v.z * 3.7) - floor(w.x);
    r.y = v.x / (w.y + 0.001) + rsqrt(abs(w.z) + 0.01) + sqrt(abs(v.w));
    r.z = (v.x > w.x ? A.x : B.x) + min(v.y, w.y) * max(v.z, w.z) + sign(v.w) + step(0.5, w.w);
    r.w = smoothstep(0.1, 0.9, v.x) + length(cross(v.xyz, w.xyz)) + abs(ddx(v.x)) + abs(ddy(w.y));
    return r * A + B;
}
technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }
