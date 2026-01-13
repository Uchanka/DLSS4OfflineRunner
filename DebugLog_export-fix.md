# Debug Log of Branch *export-fix*

## Machines

- 5090 Machine: RTX 5090 + AMD 9950X3D + 240 HZ HDR
- 5080 Machine: RTX 5080 + i5-12400 + 60 HZ HDR

Context: TEST_SCENE (character jumping up); base frameID is 2472; 60 total frames (2472 to 2531); 4 batches; reference 4k images stored in `TEST_SCENE/NPP_GT`; input 1k images stored in `TEST_SCENE/NPP_GI`

## Bug 1: Batch Initial SR Export Is Offsetted

Found on 5090 Machine, to be tested on 5080 Machine.

### Behavior

Observe: The 0th SR export of each batch (2472, 2487, 2502, 2517) does NOT fully overlap with the reference stored in NPP_GT. In addition, it overlaps with neither the previous frame (from previous batch) nor the next frame SR export. Meanwhile, both neighbors overlap with their reference.

Conclusion: The 0th SR export wrongly stores FG frame data of its front neighbor (frame N-1). This is confirmed by image diff on all 4 0th exports.

### Fix

Instead of calling CaptureFramePoolHDR(), which is a front-end approach, on both SR and FG, we map SR frame data directly from RenderTarget AAResolvedColor. See `MapRenderTargetDataHDR()`.

## Bug 2: Inconsistent Color Statistics (CAN'T RESOLVE)

Found on 5090 Machine, probably also exists on 5080 Machine.

### Behavior

Observe: For the reference image and the exported image, a {ref, SR export} pair of any frame N look different and have different color stats. E.g., the {Min, Mean, Max} color-value triplet of frame-2500 reference is {8.42e-03, 0.214, 3.730}, but that of frame-2500 SR export is {9.53e-03, 0.234, 2.020}.

### Cause

We use RGBA16_FLOAT format for all HDR buffers except for swapchain backbuffers (BB) which uses RGBA10_UNORM. Though we never perform manual data conversion, the tonemapping pass, which likely does PQ conversion from RGBA16_FLOAT to RGBA10_UNORM, causes inconsistency in luminance. 

However, it's impossible to use RGBA16_FLOAT format throughout, because previously we found that

```cpp
    /// RGBA16_FLOAT will "disable" DLSSG, and R11G11B10_FLOAT cannot be handled by dx12.
    deviceParams.swapChainFormat = nvrhi::Format::R10G10B10A2_UNORM;
```

This is confirmed by [Nvidia ProgrammingGuideDLSS_G doc](streamline\docs\ProgrammingGuideDLSS_G.md\#120-dlss-g-and-hdr):
> **IMPORTANT:**
> DLSS-G currently does NOT support FP16 pixel format and scRGB color space because it is too expensive in terms of compute and bandwidth cost.

## Bug 3: Memory Fragmentation (TO COMPLETE)

Found on 500 Machine only

### Bevavior

Observe: Across different whole-scene runs (from the batch script), we ***ALWAYS*** get 36 zero-byte export out of 120 in total.
However, the set of zero-byte frames can be different for each run.

### Cause

A closer inspection tells us more. TODO: complete explanation. 

NPP_beauty_2511_MaxPerformance_fg.exr
NPP_beauty_2512_MaxPerformance.exr

NPP_beauty_2531_MaxPerformance.exr
NPP_beauty_2531_MaxPerformance_fg.exr

NPP_beauty_2483_MaxPerformance_fg.exr
NPP_beauty_2484_MaxPerformance.exr

NPP_beauty_2501_MaxPerformance.exr
NPP_beauty_2501_MaxPerformance_fg.exr

## Bug 4: Fragile Fullscreen Mode (SCRATCH PAPER FOR NOW)

3844 x 2177 3840 x 2160; 4 x 17
2562 x 1453 2560 x 1440; 2 x 13

### Failed Attempt

```cpp
void StreamlineSample::ComputeWindowBorders(HWND hWnd)
{
    WindowBorderInfo& borders = windowBorderInfo;

    RECT windowRect, clientRect;
    GetWindowRect(hWnd, &windowRect);
    GetClientRect(hWnd, &clientRect);

    POINT clientTopLeft = { 0, 0 };
    ClientToScreen(hWnd, &clientTopLeft);

    RECT clientRectScreen = {
        clientTopLeft.x,
        clientTopLeft.y,
        clientTopLeft.x + (clientRect.right - clientRect.left),
        clientTopLeft.y + (clientRect.bottom - clientRect.top)
    };

    borders.left   = static_cast<uint32_t>(clientRectScreen.left - windowRect.left);
    borders.top    = static_cast<uint32_t>(clientRectScreen.top - windowRect.top);
    borders.right  = static_cast<uint32_t>(windowRect.right - clientRectScreen.right);
    borders.bottom = static_cast<uint32_t>(windowRect.bottom - clientRectScreen.bottom);
}
```
