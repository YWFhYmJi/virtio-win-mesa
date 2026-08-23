Texture3D<float4> texture0 : register(t0);
SamplerState sampler0 : register(s0);

struct PSIn {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 PS(PSIn input) : SV_Target
{
    return texture0.Sample(sampler0, float3(saturate(input.color.xy), 0.5f));
}
