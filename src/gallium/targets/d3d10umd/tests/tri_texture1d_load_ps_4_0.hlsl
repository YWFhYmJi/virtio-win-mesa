Texture1D<float4> texture0 : register(t0);

cbuffer TextureLoadControl : register(b0) {
    float4 control;
};

struct PSIn {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

float4 PS(PSIn input) : SV_Target
{
    uint mipLevel = (uint)control.x;
    uint width;
    uint levels;

    texture0.GetDimensions(mipLevel, width, levels);

    uint x = min((uint)(saturate(input.color.x) * width), width - 1);
    return texture0.Load(int2(x, mipLevel));
}
