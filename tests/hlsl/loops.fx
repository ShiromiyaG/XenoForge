// Loops: a count from an int constant, a break, nesting, a loop counter
// used to index constants (aL), and one the compiler must keep as a loop.
int Count;
int Inner;
float4 Weights[16];
float Threshold;
float4 VS(float4 p : POSITION) : POSITION { return p; }
float4 PS(float2 uv : TEXCOORD0, float4 c : COLOR0) : COLOR0
{
    float4 sum = 0;
    [loop] for (int i = 0; i < Count; i++)
    {
        sum += Weights[i] * uv.x;
        if (sum.x > Threshold) break;
        [loop] for (int j = 0; j < Inner; j++) sum.y += c.y * j;
    }
    return sum;
}
technique T { pass P { VertexShader = compile vs_3_0 VS(); PixelShader = compile ps_3_0 PS(); } }
