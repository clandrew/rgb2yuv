RWTexture2D<float4> rgb : register(u0);
RWTexture2D<float> yuv_luminance : register(u1);
RWTexture2D<float2> yuv_chrominance : register(u2);

uint2 imageSize : register(b0);

void OutputY(uint3 groupID, uint3 threadID)
{
    int srcX = (groupID.x * 64) + threadID.x;
    int srcY = groupID.y;

    if (srcX >= imageSize.x)
        return;

    if (srcY >= imageSize.y)
        return;

    int2 srcCoord = int2(srcX, srcY);

    float r = rgb[srcCoord].r;
    float g = rgb[srcCoord].g;
    float b = rgb[srcCoord].b;

    // Reference: https://docs.microsoft.com/en-us/windows/win32/medfound/recommended-8-bit-yuv-formats-for-video-rendering
    float y = 0.256788 * r + 0.504129 * g + 0.097906 * b;

    y = max(y, 0.0f);
    y = min(y, 1.0f);

    float luminance = y;
    int2 dstCoord = srcCoord;
    yuv_luminance[dstCoord] = luminance;
}

void OutputUV(uint3 groupID, uint3 threadID)
{
    int ql = (groupID.x * 128) + (threadID.x * 2); // quad left
    int qt = (groupID.y * 2); // quad top

    if (ql >= imageSize.x)
        return;

    if (qt >= imageSize.y)
        return;

    int2 src0 = int2(ql + 0, qt + 0); // top left
    int2 src1 = int2(ql + 1, qt + 0); // top right
    int2 src2 = int2(ql + 0, qt + 1); // bottom left
    int2 src3 = int2(ql + 1, qt + 1); // bottom right

    float3 rgb0 = rgb[src0].rgb;
    float3 rgb1 = rgb[src1].rgb;
    float3 rgb2 = rgb[src2].rgb;
    float3 rgb3 = rgb[src3].rgb;

    float3 avg = (rgb0 + rgb1 + rgb2 + rgb3) / 4.0f;

    float r = avg.r;
    float g = avg.g;
    float b = avg.b;

    float u = -0.148223 * r - 0.290993 * g + 0.439216 * b;
    float v = 0.439216 * r - 0.367788 * g - 0.071427 * b;

    u = max(u, 0.0f);
    v = max(v, 0.0f);
    u = min(u, 1.0f);
    v = min(v, 1.0f);

    float2 chrominance = float2(u, v);
    int2 destCoord = int2((groupID.x * 64) + threadID.x, groupID.y);
    yuv_chrominance[destCoord] = chrominance;
}

[numthreads(64, 1, 1)]
void main( uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID )
{
    OutputY(groupID, threadID);

    OutputUV(groupID, threadID);
}