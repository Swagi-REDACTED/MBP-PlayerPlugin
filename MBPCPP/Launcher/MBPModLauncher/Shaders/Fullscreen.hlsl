struct VS_OUTPUT
{
    float4 pos : SV_POSITION;
    float4 col : COLOR0;
    float2 uv  : TEXCOORD0;
};

VS_OUTPUT VSMain(uint vertexId : SV_VertexID)
{
    // One full-screen triangle. The coordinates deliberately extend to 2 so
    // rasterization covers the viewport without a vertex/index buffer.
    const float2 uv[3] = {
        float2(0.0, 0.0),
        float2(2.0, 0.0),
        float2(0.0, 2.0)
    };

    VS_OUTPUT output;
    output.uv = uv[vertexId];
    output.pos = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    output.col = float4(1.0, 1.0, 1.0, 1.0);
    return output;
}
