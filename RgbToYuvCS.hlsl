RWTexture2D<float4> rgb : register(u0);
RWTexture2D<float> yuv_luminance : register(u1);
RWTexture2D<float2> yuv_chrominance : register(u2);

[numthreads(1, 1, 1)]
void main( uint3 groupID : SV_GroupID )
{
    int2 coord = int2(groupID.x, groupID.y);

    float r = rgb[coord].r;
    float g = rgb[coord].g;
    float b = rgb[coord].b;

    // Reference: https://docs.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering
    float y = 0.256788 * r + 0.504129 * g + 0.097906 * b;

    y = max(y, 0.0f);
    y = min(y, 1.0f);

    float luminance = y;
    yuv_luminance[coord] = luminance;

    if (coord.x % 2 != 0 || coord.y % 2 != 0)
    {
        return;
    }

    int2 halfcoord = int2(coord.x / 2, coord.y / 2);

    float u = -0.148223 * r - 0.290993 * g + 0.439216 * b;
    float v = 0.439216 * r - 0.367788 * g - 0.071427 * b;

    u = max(u, 0.0f);
    v = max(v, 0.0f);
    u = min(u, 1.0f);
    v = min(v, 1.0f);

    float2 chrominance = float2(u, v);
    yuv_chrominance[halfcoord] = chrominance;
}