struct VSOut {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

cbuffer VertexIdCBuffer : register(b0) {
    float4 scaleOffset;
    float4 colorMul;
};

VSOut VS(uint vertex_id : SV_VertexID)
{
    static const float2 positions[3] = {
        float2(-0.9f, -0.9f),
        float2( 0.9f, -0.9f),
        float2( 0.0f,  0.9f),
    };
    static const float4 colors[3] = {
        float4(0.8f, 0.0f, 0.0f, 1.0f),
        float4(0.0f, 0.9f, 0.0f, 1.0f),
        float4(0.0f, 0.0f, 0.7f, 1.0f),
    };

    VSOut output;
    output.position.xy = positions[vertex_id] * scaleOffset.xy + scaleOffset.zw;
    output.position.zw = float2(0.5f, 1.0f);
    output.color = colors[vertex_id] * colorMul;
    return output;
}
