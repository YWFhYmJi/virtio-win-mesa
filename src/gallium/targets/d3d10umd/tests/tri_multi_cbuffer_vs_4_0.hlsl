struct VSIn {
    float4 position : POSITION;
    float4 color : COLOR;
};

struct VSOut {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

cbuffer ScaleOffset : register(b1) {
    float4 scaleOffset;
};

cbuffer ColorMul : register(b3) {
    float4 colorMul;
};

VSOut VS(VSIn input)
{
    VSOut output;

    output.position.xy = input.position.xy * scaleOffset.xy + scaleOffset.zw;
    output.position.zw = input.position.zw;
    output.color = input.color * colorMul;
    return output;
}
