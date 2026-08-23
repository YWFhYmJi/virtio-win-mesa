struct VSOut {
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VSOut VS(uint vertex_id : SV_VertexID)
{
    static const float2 pos[3] = {
        float2(-0.9f, -0.9f),
        float2( 0.9f, -0.9f),
        float2( 0.0f,  0.9f),
    };
    static const float2 uv[3] = {
        float2(0.0f, 0.0f),
        float2(1.0f, 0.0f),
        float2(0.5f, 1.0f),
    };

    VSOut output;
    output.position = float4(pos[vertex_id], 0.5f, 1.0f);
    output.color = float4(uv[vertex_id], 0.0f, 1.0f);
    return output;
}
