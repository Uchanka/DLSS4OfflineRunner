# DLSS Offline Runner

Based on StreamlineSample. For more info on the original app by Nvidia, see <https://github.com/NVIDIA-RTX/Streamline_Sample>

## "Must Read" Contents

This doc contains both important usage guide and optional technical details. Here we only list out the "must read" ones.

- [Project Setup](#project-setup-adapted-from-original-readme)
- [Cmdline Args](#cmdline-args)
- [Test Machines and Possible Issue](#test-machines-and-possible-issue)
- [Cmdline Option `BatchIndex` and Automated Script](#cmdline-option-batchindex-and-automated-script)

## Project Setup (Adapted from original README)

1. Ensure you have CMake 3.20+ and the vulkan sdk (run `vulkaninfo`) on your system.
2. Ensure that a VK-compatible dxc.exe is available in your system `PATH`.  The best way to do this is to install a recent (1.2.198.1 or newer) Vulkan SDK from [official site](https://vulkan.lunarg.com/sdk/home#windows) and ensure that its `bin` directory is in the build machine's system `PATH`.
3. Clone this repository (you probably have done so), then run: `git submodule update --init --recursive`
4. Go to Streamline repo and download the [SDK Release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.9.0). Unzip the folder and put everything into `streamline/` folder in project root. i.e. `streamline/package.bat` should exist.
5. Switch to **dlss-release** branch.
6. Run `make.bat` and fix any error in the CMake configure.
7. Open the solution in `_build/`.
8. ~~Install Windows Implementation Library (wil). See section below for how to.~~ UPDATE: it has been added as a git submodule and will be immediately usable.
9. Build Solution.
10. Make sure you read and follow [Cmdline Args section](#cmdline-args) before starting any serious/actual run.

## Cmdline Args

Unlike our AMD FSR Offline Runner, this app doesn't have loading config from json feature. The only way to specify runtime options is through cmdline args.

LATEST UPDATE: we deprecated the `OutputMaxCount` option for these 2 reasons:

1. It is more for a developer convenience (myself) but has little help in production use case. It should be removed in a "Release" product anyway.
2. To solve a tricky bug, we adpoted a multi-run approach (see [this section](#bypass-dlfg-frame-rate-check) if you are interested), and keep supporting `OutputMaxCount` would make handling frame number very tricky.

We have the following options, adapted from FSR Offline Runner README:

- [Global Switch] EnableHack: default false. If false, the app will run in its original behavior, rendering Sponza Palace.
- DisplayResolution: accepts 2 formats. See [this section](#resolution-system-in-dlss) for more details. TL;DR: User specifies display resolution in cmdline, render resolution is auto determined by the pre-scaled input image size, and DLSS mode is internally decided to pass check by DLSS.
  - `-DisplayResolution <width> <height>`. This is the general option and gives full flexibility. i.e. Except for extreme resolutions like 10x7, 19200x10800, users can choose any value, like 2880x1620. ***BUT NOTE: The aspect ratio should be consistent with that of input image data.***
  - `-DisplayResolution <alias>`, where `<alias>` accepts valid values 1, 2, and 4. This is a handy alternative for the common 1K, 2K, and 4K settings.
  - [***FUTURE TODO***] Unfortunately, for now precise FG export can only be seen in 1K, 2K, and 4K display resolution under fullscreen mode. I will remove this bullet after fully enabling custom resolution support.
- [Optional] ParseJitter: whether to parse and use the jitter data from input filenames, default false. Ensure filenames have it before setting it to true. (Hint: currently only NPP_JI files have it.)
- BatchIndex: each batch is 15 frames captured. For example, a 60-frame scene requires 4 runs with BatchIndex from 0 to 3. See [this section](#bypass-dlfg-frame-rate-check) for more details.
- HackPaths: accepts 2 formats.
  - A single relative path to the scene testdata root, e.g. *"../media/TEST_SCENE"*. The paths to frame color data and MVD data (encoded MVs and Depths) will be constructed automatically by appending "NPP_JI" and "MVD_JI" respectively, which are the defaults when UE generates testdata.
  - A pair of relative paths to the color data and MVD data. In cmdline they are spaced: `-HackPaths <color path> <MVD path>`.
- [Optional] StoreOutput: whether to export SR and SR+FG output frames to .exr files, default false.
- [Optional **if StoreOutput not given**] OutputPath: a relative path to the exported frames folder, e.g. *"../media/TEST_SCENE/outputs"*. We recommend using an output folder in the testdata root.
- [Optional] AlignFilename: if set, export filenames will be in order when put together with the reference result (should be in *NPP_GT*). A (reference, SR output, FG output) triplet would look like (*NPP_beauty_2472.exr, NPP_beauty_2472_Quality.exr, NPP_beauty_2472_Quality_fg.exr*). This helps looking at them in timely order with image viewer. When false, the frameID in the export filenames will start from 0000.
- [Optional] Identifier: a custom identifier for export filenames. If not given or AlignFilename is set to true, it will be the common prefix of all color data, e.g. *NPP_beauty*.
- BatchIndex: each batch is 15 frames captured. For example, a 60-frame scene requires 4 runs with BatchIndex from 0 to 3. See [this section](#bypass-dlfg-frame-rate-check) for more details.

```shell
# cd to <Project Root>/_bin
./StreamlineSample.exe  -EnableHack \
                        -DisplayResolution 4 \
                        -ParseJitter \
                        -BatchIndex 0 \
                        -HackPaths "../media/TEST_SCENE" \
                        -StoreOutput \
                        -AlignFilename
                        -OutputPath "../media/TEST_SCENE/screenshots"
```

Also, it would be very convenient to set them up in the VS Debugger such that each test run is a one-click. Put this single-line arg list to StreamlineSample  Property Pages, in Configuration Properties -> Debugging -> Commandline Arguments. Adjust if needed. Make sure the `OutputPath` (e.g. `media/TEST_SCENE/screenshots`) exists.

```shell
-EnableHack -DisplayResolution 4 -ParseJitter -BatchIndex 0 -HackPaths "../media/TEST_SCENE" -StoreOutput -AlignFilename -OutputPath "../media/TEST_SCENE/screenshots"
```

## Resolution System in DLSS

Both DLSS and AMD's FSR **internally** determine render resolution based on user-selected display resolution + SR mode (DLSS calls it DLSS mode, FSR calls it scale preset). Though these ratios of DLSS were missing from the code base or online, we tested them out. They are the same as AMD's except for `Balanced` mode, which is a strange value around 1.724 (sqrt 3 is 1.732)

```cpp
/// From FSR
switch (m_ScalePreset)
{
case FSRScalePreset::NativeAA:
    m_UpscaleRatio = 1.0f;
    break;
case FSRScalePreset::Quality:
    m_UpscaleRatio = 1.5f;
    break;
case FSRScalePreset::Balanced:
    m_UpscaleRatio = 1.7f;
    break;
case FSRScalePreset::Performance:
    m_UpscaleRatio = 2.0f;
    break;
case FSRScalePreset::UltraPerformance:
    m_UpscaleRatio = 3.0f;
    break;
case FSRScalePreset::Custom:
default:
    // Leave the upscale ratio at whatever it was
    break;
}
```

This is the natual choice for a game, but not ideal for our test purpose: we want to specify both render resolution (from input image resolution) and display resolution.

Without intervention, both FSR and DLSS can compute a render resolution we don't want (should be the same as input image size). The most typical example is the 1K -> 2K setting (2560x1440 is 1.33 rather than 1.5 times of 1920x1080 BTW).
Since there is no mode with a 1.33x upscale ratio, we can't expect render resolution to be set to 1920x1080.

The bad news is, DLSS mode gets actually involved in the pipeline, and thus we must treat it correctly. FSR preset is the opposite because it's merely an alias for users to choose from, and FSR would internally compute render resolution from it in normal case. So for our offline runner, we can directly overwrites render resolution with input image resolution and forget about the preset (by setting it to Custom).
The good news is, DLSS allows the actual display/render ratio to be different from the internal ratio above, as long as the actual ratio is within a range. We tested them out as below.

```cpp
/// We use 4K (3840x2160) display size and reverse engineer on SLWrapper::QueryDLSSOptimalSettings()
/// to get these values. Each comment line is the [Max, Min, Optimal] display width from the debugger.
/// 3840 / [Max, Min, Optimal] to get [Min, Max, Optimal] RatioTriplet. (NOTE the order)
const std::unordered_map<sl::DLSSMode, StreamlineSample::RatioTriplet> StreamlineSample::UpscaleRatioMap = {
    // 3840, 3802, 3840
    { sl::DLSSMode::eDLAA,              { 1.0f, 1.1f, 1.0f } },
    // 3840，1920, 2560
    { sl::DLSSMode::eMaxQuality,        { 1.0f, 2.0f, 1.5f } },
    // 3840, 1920, 2227
    { sl::DLSSMode::eBalanced,          { 1.0f, 2.0f, 1.724f } },
    // 3840, 1920, 1920
    { sl::DLSSMode::eMaxPerformance,    { 1.0f, 2.0f, 2.0f } },
    // 1280, 1280, 1280
    { sl::DLSSMode::eUltraPerformance,  { 3.0f, 3.0f, 3.0f } },
};
```

If out of range, like having actual ratio 3.0 but use `eMaxQuality`, error will be thrown.

> [20-29-30][streamline][info][tid:57940][289s:264ms:511us]commonEntry.cpp:1059[ngxLog] [NGXDLAA::EvaluateFeature:1086] Error: Dynamic scaling error for PerfQuality Mode (NVSDK_NGX_PerfQuality_Value_MaxQuality,2). RenderSubrect (1920x1080) outside of Min (2880x1620) and Max (5760x3240) dynamic res.

Thus, our design is to allow users to directly specify display resolution but disallow choosing DLSS mode.
The app will decide and set the appropriate DLSS mode from display resolution and input image resolution. The tie-breaker will be the optimal ratio, the 3rd element in the `RatioTriplet` above. In this way, we ensure DLSS (at least in this app) compute the render resolution equal to our input image resolution.

Lastly, a quick inspection on the data above tells us:

1. The display resolution being set MUST be 1.0x to 2.0x of the input image resolution, length-wise.
2. EXCEPT for a single case when the ratio is EXACTLY 3.0x. In `eUltraPerformance` mode, input data with 640x360, 853x480 and 1280x720 resolution should be passed to 1K, 2K, 4K setting respectively.

## Tips

### Turn Off Fullscreen mode

Since we uses a frontend approach to capture screenshot, we have to turn on fullscreen mode to get 3840 x 2160 output. But if you stop on a break point or the app encounters an error, fullscreen will cause machine to halt (you have to Ctrl + Alt + Del and Sign Out and log in again), it's much easier to turn it off when doing test runs.

Go to `src\main.cpp` line 271 to turn it off.

### Temporarily Disable Hack or StoreOutput

If you want to disable hack or disable export output, you can go to `src\main.cpp` line 264.

The former is useful if you want to play with the GUI control panel, because we turned it off when hack is on (otherwise screen capture will include the GUI), or when you simply want to see how the original scene is rendered.

The latter is useful if you want to fix issues unrelated to export. We levarage `sleep_for()` to correctly capture screenshot, thus turning it off can save your time. Also, when export is off, the app keeps running instead of closing after running all needed frames.

## Test Machines and Possible Issue

This offline runner has been tested on the following 2 machines:

- (MT Arch Lab Machine) i5-12400, 6 cores 12 threads 2.50-4.00 GHz; RTX 5080; 4K 60Hz monitor.
- (My game PC) AMD R9-9950X3D, 16 cores 32 threads 4.3-5.7 GHz; RTX 5090; 4K 240Hz.

Hardware-dependent and resolution-dependent issues have occurred and may occur in the future on different machines.

For example, a found-and-fixed issue is: out of the 60 exported FG outputs, there could ***randomly*** be 1 or 2 files holding rendered frames instead of FG frames.

If any issue occurs, including the above one, and is untolerable, please contact me (<haoxuan.wang@mthreads.com>)

## From LDR to HDR

Previously we used Windows API `BitBlit()` which only supports LDR capture. Now, we leveraged `winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool` to capture the window content to a DX11 texture, which supports true HDR `RGBA16_FLOAT` format.

A even bigger improvement is frame consistency, i.e. how we ensure **frameN.exr** really captures frameN contents. Previously, we used the "sleep bubble" trick to give us enough time to take captures, by extending the `Present()` time for each frame from 1/60 sec (if 60FPS) to several seconds.

Since we don't really care about execution speed, this should be acceptable if still giving consistent frames. However, HDR capture function is not synchronous (blocking). At the same time, the lowest-level declaration-only `Present()` from DXGI API is an async call.

```cpp
IDXGISwapChain : public IDXGIDeviceSubObject
{
public:
    virtual HRESULT STDMETHODCALLTYPE Present( 
        /* [in] */ UINT SyncInterval,
        /* [in] */ UINT Flags) = 0;
/// other function declarations
}
```

This means `bool presentSuccess = Present();` immediately returns. Then, `afterPresent()` callback where we capture screen and force sleep gets executed. It works under control when we use `BitBlit()`, a sync function which blocks and captures the frame displayed at that exact moment. However, our HDR capture has a more complex logic (event trigger and catcher) and is not **immediate**.

As a result, without special treatment, we end up unpredictably capture frame N-1 or frame N data for frame N exported image. It's unpredictable since the error can happen or not happen on different machines, and even across different runs on the same machine. The cause is likely that the actual display async action being queued by frame N-1 `Present()` happens after frame N starts its `TryGetNextFrame()` catcher, thus catching frame N-1.

Our solution is to completely replace this hacky "sleep bubble" trick with something much more robust: duplication detection with image hash.
The key is not to simply compare with the previous frame hash, but to maintain a bin of these hashes. In the end, we ensure the bin has 2N hashes, N for rendered frames and FG frames each. This immediately ensures we get all 2N frames from the renderer, each exactly once.

When a duplication occurs, which should be very common because we don't keep the long "sleep bubble", the capture thread simply sleeps for `DuplicateTimeout`. In theory, this could be as short as frame rate (e.g. 1/60 sec), but we found giving it a slightly bigger value (current choice is 50 ms = 0.05 sec) is better.

## Bypass DLFG Frame Rate Check

Previously when debugging, to speed up each run, we only loaded 10/60 of total input frames (and this is where the deprecated `OutputMaxCount` option originated). After using all 60 inputs, we found a critical issue:

```console
WARNING: [13-53-08][streamline][warn][tid:30096][41s:619ms:032us]dlfgPresent.cpp:1260[presentCommon] Frame rate over 100.00ms, resetting frame timer
```

After some careful experiments, we found that this 100 ms or 10 FPS redline would be reached if we do ANYTHING additional in a regular pipeline. The result of frame timer being reset is that dlfg will disable presenting FG frames, and thus breaks our screen capture design entirely.

We've tried to postpone the file IO to app shutdown, since taking screen capture is essentially a 2-step screen-to-memory + memory-to-file operation. But screen-to-memory itself must be done on-the-fly, which alone would make the frame rate too slow.

Fortunately, frame rate can only be measured after some frames have been presented, and this number for dlfg is about 20. Thus, our solution is what I called "multi-run batch captures" approach. Put it simple, each run of the app only captures 15 frames before dlfg notices the app is running slow. And we launch the app multiple times but loading in different input batches. Together they become full N frames of output.

## Cmdline Option `BatchIndex` and Automated Script

A cmdline option `BatchIndex` has been added to support the "multi-run batch captures" approach. **NOTE: When running with VS Debugger, users should know the total number of input frames and pass in the correct 0-indexed `BatchIndex`.** Fortunately, this easy counting + index computing can be automated by a script, which is the typical use case.

This helpful script [run_OneScene.bat](run_OneScene.bat) completes all batch runs of a scene. It's the go-to choice in production case.

The other script [run_MultiTimes.bat](run_MultiTimes.bat) simply repeats that several times. And it should be used only for confirming correctness on certain machines, not for production. Well, if you intend to run multiple scenes (multiple sets of input data) and thus need a multi-scene master script (which we didn't provide), then this run_MultiTimes.bat should be a good reference.

The script itself accepts input image paths from cmdline. i.e. For example, you can use

```sh
.\run_OneScene.bat ".\media\TEST_SCENE\NPP_JI" "..\media\TEST_SCENE\outputs"
```

Also note that if you don't copy the reference images in **NPP_GT** to `OutputPath` (to leverage the `AlignFilename` feature, default on in the script), you may want to change these 2 lines in the script.

```bat
rem set AlignFilename=
set AlignFilename=-AlignFilename

rem set "Has_Ref=" if you DON'T want to copy references in NPP_GT to OUTPUT_ROOT (for comparison)
set "Has_Ref=y"
```

Our script does NOT support other app cmdline options except for `HackPaths` and `OutputPath`. To change other options, please change the script or VS Debugging property page.

Final words: low-level edge-case details like

- scene with frame count NOT a multiple of 15 (frames per batch)
- head/tail frame correctness (mostly to ensure a correct frame history when DLSS starts to compute output)

are handled and users don't need to worry about them.
