Texture1D texture0 : register(t0);

float miplevel;

float4 PS(float4 position : SV_POSITION) : SV_Target
{
    float2 p;

    texture0.GetDimensions(miplevel, p.x, p.y);
    p.y = miplevel;
    p *= float2(position.x / 640.0f, 1.0f);
    return texture0.Load(int2(p));
}
