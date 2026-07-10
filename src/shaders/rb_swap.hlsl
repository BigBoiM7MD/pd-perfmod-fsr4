// rb_swap.hlsl - red<->blue channel swap compute shader (SRV in, UAV out).
//
// WHY THIS EXISTS:
//   FSR4 always writes its output in RGBA channel order. Under vkd3d-proton
//   (Linux/Proton) the swapchain backbuffer is B8G8R8A8_UNORM (BGRA) and
//   REFramework copies our output to it with a RAW byte-for-byte
//   ID3D12GraphicsCommandList::CopyResource (REFramework ResourceCopier.cpp).
//   vkd3d implements that copy as vkCmdCopyImage2, which does NOT reorder
//   channels, so FSR4's RGBA bytes land in a BGRA buffer and red/blue swap.
//
//   This pass runs AFTER ffxDispatch. It reads the FSR4 output (created as
//   R8G8B8A8_UNORM, DXGI 28) through an SRV and writes the R/B-swapped result
//   into a second R8G8B8A8_UNORM texture via a UAV store. Storing (b,g,r,a)
//   into an RGBA texture makes the stored bytes BGRA-ordered, so the later raw
//   CopyResource to the BGRA backbuffer produces correct colors.
//
//   Design notes for vkd3d safety:
//     * Read via SRV Load (always supported) - NOT a typed UAV load, which
//       would require the optional "Typed UAV Load Additional Formats" feature
//       that vkd3d does not reliably provide.
//     * Write via UAV store to R8G8B8A8_UNORM, which IS in the D3D12 baseline
//       guaranteed typed-store set. We never create a BGRA-typed UAV.
//
//   On native Windows the swapchain format already matches, so the backend
//   does NOT dispatch this pass and Windows behaviour is unchanged.

Texture2D<float4>   Src : register(t0);
RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void CSMain(uint3 tid : SV_DispatchThreadID)
{
    uint w, h;
    Dst.GetDimensions(w, h);
    if (tid.x >= w || tid.y >= h)
        return;

    float4 c = Src.Load(int3(tid.xy, 0));
    Dst[tid.xy] = float4(c.b, c.g, c.r, c.a);
}
