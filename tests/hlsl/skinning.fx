// Skinning: four bones from blend indices, constants addressed by a0, a
// normal transformed and normalized, lighting with pow and exp.
float4x3 Bones[56];
float4x4 ViewProj;
float3 LightDir;
float SpecPower;
struct VSIn { float4 pos : POSITION; float3 normal : NORMAL; float4 weights : BLENDWEIGHT; float4 indices : BLENDINDICES; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; float fog : TEXCOORD1; float size : PSIZE; };
VSOut VS(VSIn v)
{
    float4x3 skin = 0;
    for (int k = 0; k < 4; k++) skin += Bones[(int)v.indices[k]] * v.weights[k];
    float3 world = mul(v.pos, skin);
    float3 n = normalize(mul(v.normal, (float3x3)skin));
    VSOut o;
    o.pos = mul(float4(world, 1), ViewProj);
    float diffuse = saturate(dot(n, -LightDir));
    o.color = float4(diffuse.xxx + pow(diffuse, SpecPower), 1);
    o.uv = v.uv;
    o.fog = exp2(-o.pos.z * 0.01) + log(abs(world.y) + 1);
    o.size = max(1, 64 / o.pos.w);
    return o;
}
float4 PS(float4 c : COLOR0) : COLOR0 { return c; }
technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }
