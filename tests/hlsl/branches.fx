// Branches: on a bool constant, on a computed value (branched, flattened,
// called), nested, with texture fetches inside.
bool UseFog;
bool UseTint;
float4 Tint;
float4 FogColor;
texture Tex;
sampler Samp = sampler_state { Texture = <Tex>; };
float4 VS(float4 p : POSITION) : POSITION { return p; }
float4 Shade(float2 uv, float4 c)
{
    float4 color = tex2D(Samp, uv) * c;
    [branch] if (UseTint) color *= Tint;
    [branch] if (color.a < 0.5)
    {
        color.rgb = lerp(color.rgb, FogColor.rgb, uv.y);
        [flatten] if (UseFog) color.rgb *= 0.5;
    }
    else
    {
        [call] if (color.r > color.g) color.rgb = color.bgr; else color.rgb = 1 - color.rgb;
    }
    return color;
}
float4 PS(float2 uv : TEXCOORD0, float4 c : COLOR0) : COLOR0 { return Shade(uv, c); }
technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }
