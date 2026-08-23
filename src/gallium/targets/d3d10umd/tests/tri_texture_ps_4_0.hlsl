Texture2D<float4> texture0 : register(t0);
SamplerState sampler0 : register(s0);

struct PSIn {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 PS(PSIn input) : SV_Target
{
    return texture0.Sample(sampler0, saturate(input.color.xy));
}
