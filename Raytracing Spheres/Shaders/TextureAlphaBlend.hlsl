Texture2D<float4> g_foreground : register(t0);

RWTexture2D<float3> g_background : register(u0);

#define ROOT_SIGNATURE \
	"DescriptorTable(SRV(t0))," \
	"DescriptorTable(UAV(u0))"

[RootSignature(ROOT_SIGNATURE)]
[numthreads(16, 16, 1)]
void main(uint2 pixelCoordinate : SV_DispatchThreadID) {
	uint2 pixelDimensions;
	g_foreground.GetDimensions(pixelDimensions.x, pixelDimensions.y);
	if (pixelCoordinate.x >= pixelDimensions.x || pixelCoordinate.y >= pixelDimensions.y) return;

	g_background[pixelCoordinate] = lerp(g_background[pixelCoordinate], g_foreground[pixelCoordinate].rgb, g_foreground[pixelCoordinate].a);
}
