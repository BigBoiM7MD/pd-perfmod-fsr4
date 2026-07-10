// Red<->blue swap for the Proton (vkd3d) path.
// FSR4 always writes R8G8B8A8_UNORM (RGBA) byte order. The Proton swapchain
// backbuffer PRESENTS as B8G8R8A8 (BGRA) even though REFramework's GetDesc()
// reports R8G8B8A8 (28). REFramework then does a raw CopyResource (no channel
// reorder) into that backbuffer. So we must hand it BGRA-ordered bytes.
// This pass reads the RGBA FSR output and stores (b,g,r,a) into an RGBA-typed
// texture, i.e. the stored bytes are BGRA-ordered -> correct on present.
// SRV read + RGBA UAV store are baseline D3D12 (no Typed UAV Load cap needed).
Texture2D<float4> g_input : register(t0);
RWTexture2D<float4> g_output : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID) {
    float4 c = g_input.Load(int3(tid.xy, 0));
    // swap R and B: out = (b, g, r, a)
    g_output[tid.xy] = float4(c.b, c.g, c.r, c.a);
}
