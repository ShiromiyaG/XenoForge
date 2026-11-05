// Textures: cube, 3D, explicit LOD, bias, gradients, projection, a vertex
// texture fetch, a kill, depth and four colors out.
texture T2; texture T3; texture TC; texture TV;
sampler S2 = sampler_state { Texture = <T2>; };
sampler S3 = sampler_state { Texture = <T3>; };
samplerCUBE SC = sampler_state { Texture = <TC>; };
sampler SV = sampler_state { Texture = <TV>; };
float4x4 ViewProj;
struct VSOut { float4 pos : POSITION; float4 uv : TEXCOORD0; float3 dir : TEXCOORD1; };
VSOut VS(float4 p : POSITION, float2 uv : TEXCOORD0)
{
    VSOut o;
    float height = tex2Dlod(SV, float4(uv, 0, 0)).r;
    o.pos = mul(p + float4(0, height, 0, 0), ViewProj);
    o.uv = float4(uv, o.pos.zw);
    o.dir = p.xyz;
    return o;
}
struct PSOut { float4 c0 : COLOR0; float4 c1 : COLOR1; float4 c2 : COLOR2; float4 c3 : COLOR3; float depth : DEPTH; };
PSOut PS(VSOut i)
{
    float4 a = tex2D(S2, i.uv.xy);
    clip(a.a - 0.25);
    PSOut o;
    o.c0 = a;
    o.c1 = texCUBE(SC, normalize(i.dir));
    o.c2 = tex3D(S3, i.uv.xyz) + tex2Dlod(S2, float4(i.uv.xy, 0, 2));
    o.c3 = tex2Dbias(S2, float4(i.uv.xy, 0, -1)) + tex2Dgrad(S2, i.uv.xy, ddx(i.uv.xy) * 2, ddy(i.uv.xy)) + tex2Dproj(S2, i.uv);
    o.depth = saturate(i.uv.z / i.uv.w);
    return o;
}
technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }
