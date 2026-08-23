Texture2D<float4> texture1 : register(t1);
SamplerState sampler1 : register(s1);

struct PSIn {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 PS(PSIn input) : SV_Target
{
    return texture1.Sample(sampler1, saturate(input.color.xy));
}
