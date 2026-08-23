Texture2D<float4> texture0 : register(t0);
SamplerState sampler0 : register(s0);

cbuffer TextureColor : register(b0) {
    float4 colorMul;
    float4 colorAdd;
};

struct PSIn {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 PS(PSIn input) : SV_Target
{
    float4 texel = texture0.Sample(sampler0, saturate(input.color.xy));
    return texel * colorMul + colorAdd;
}
