////----------------------------------------------------------------------------------
//// File:        StreamlineSample.cpp
//// SDK Version: 2.0
//// Email:       StreamlineSupport@nvidia.com
//// Site:        http://developer.nvidia.com/
////
//// Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.
////
//// Redistribution and use in source and binary forms, with or without
//// modification, are permitted provided that the following conditions
//// are met:
////  * Redistributions of source code must retain the above copyright
////    notice, this list of conditions and the following disclaimer.
////  * Redistributions in binary form must reproduce the above copyright
////    notice, this list of conditions and the following disclaimer in the
////    documentation and/or other materials provided with the distribution.
////  * Neither the name of NVIDIA CORPORATION nor the names of its
////    contributors may be used to endorse or promote products derived
////    from this software without specific prior written permission.
////
//// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
//// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
//// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
//// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
//// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
//// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
//// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
//// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
////
////----------------------------------------------------------------------------------

#include "StreamlineSample.h"
#include <sstream>
#include <thread>
#include <format>
#include <future>
#include <stb_image_write.h>

#ifdef STREAMLINE_FEATURE_DLSS_RR
#include "lighting_cb.h"
#endif // STREAMLINE_FEATURE_DLSS_RR

#if DONUT_WITH_DX12
#include <d3d12.h>
#include <nvrhi/d3d12.h>
#endif
#if DONUT_WITH_VULKAN
#include <vulkan/vulkan.h>
#include <nvrhi/vulkan.h>
#include <../src/vulkan/vulkan-backend.h>
#endif

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h> // consume_Windows_Foundation_Collections_IVectorView::IndexOf() impl
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.MediaProperties.h> // ImageEncodingProperties::CreateJpeg() impl
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h> // RandomAccessStream::CopyAndCloseAsync impl
#include <winrt/Windows.Media.AppRecording.h>

#include <winrt/Windows.Graphics.Capture.h>

//#include <Windows.UI.Interop.h>
#include <Windows.Graphics.Capture.Interop.h>
#include <Windows.Graphics.Directx.Direct3d11.Interop.h>

#include <wrl.h> // ComPtr<ID3D11Device> impl

#include <wil/resource.h> // wil::shared_event
#include <magic_enum/magic_enum_all.hpp>

#include <xxhash.h>

#include <unordered_set>

using namespace donut;
using namespace donut::math;
using namespace donut::engine;
using namespace donut::render;

//using namespace winrt; // cause ambiguity
using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Devices;
using namespace winrt::Windows::Media::MediaProperties;
using namespace winrt::Windows::Media::AppRecording;
using namespace winrt::Windows::Storage;
//using namespace winrt::Windows::Foundation; // cause ambiguity
namespace winrt_foundation = winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;
using namespace Microsoft::WRL;

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

const std::unordered_map<int, donut::math::uint2> StreamlineSample::ResolutionAliases = {
    { 1, uint2(1920, 1080) },
    { 2, uint2(2560, 1440) },
    { 4, uint2(3840, 2160) },
};

/// typical usage:
/// -EnableHack -DisplayResolution 2560 1440 -ParseJitter -BatchIndex 1 -HackPaths "../media/TEST_SCENE" -StoreOutput -AlignFilename -OutputPath "../media/TEST_SCENE/screenshots"
StreamlineSample::HackOptionDef StreamlineSample::parseHackOptions(int argc, const char* const* argv)
{
    HackOptionDef options;
    // Skip argv[0] and convert to modern format
    std::vector<std::string> argList(argv + 1, argv + argc);
    // whether a arg & value pair is within range, value is given (next arg should NOT have '-' prefix)
    bool isValid;

    // counter to sanity check hackPaths, result written to options.frameCount 
    auto CountEXR = [](const std::filesystem::path& folderPath) -> size_t {
        return std::count_if(std::filesystem::directory_iterator(folderPath), std::filesystem::directory_iterator{}, [](const auto& entry) {
            return entry.path().extension() == ".exr";
        });
    };

    // throw if not valid
    auto ValidateArg = [&argList](size_t idx, size_t numValues, const std::string_view& option) {
        bool isValid = idx + numValues < argList.size();
        auto it = argList.begin() + idx + 1;
        for (; isValid && (it != argList.begin() + idx + numValues); it++)
            isValid &= it->front() != '-';

        if (!isValid)
            log::error("%s requires %d input", option, numValues);
    };

    // parse other options only when global switch "-EnableHack" is set
    bool hackMode = false;
    for (size_t currentArg = 0; currentArg < argList.size(); currentArg++)
    {
        std::string command = argList[currentArg];
        if (command == "-EnableHack")
        {
            // we reset HackOptions otherwise bool fields can't be overwritten to false
            hackMode = true;
            options.enableHack = true;
            continue;
        }
        if (hackMode && command == "-Identifier")
        {
            ValidateArg(currentArg, 1, command);
            options.identifier = argList[currentArg + 1];
            currentArg++;
            continue;
        }

        if (hackMode && (command == "-Resolution" || command == "-DisplayResolution")) {
            /// We also accept alias represent resolution in K, e.g. -Resolution 2.
            /// Thus we can't throw error. Check manually instead
            if (currentArg + 1 < argList.size() && argList[currentArg + 1][0] != '-') {
                int alias = std::stoul(argList[currentArg + 1]);
                if (ResolutionAliases.contains(alias)) {
                    options.displayResolution = ResolutionAliases.at(alias);
                    currentArg++;
                    continue;
                }
            }

            // else, regular inputs require 2 arguments
            ValidateArg(currentArg, 2, command);
            options.displayResolution.x = std::stoul(argList[currentArg + 1]);
            options.displayResolution.y = std::stoul(argList[currentArg + 2]);
            
            currentArg += 2;
            continue;
        }
        if (hackMode && command == "-ParseJitter")
        {
            options.parseJitter = true;
            continue;
        }
        if (hackMode && command == "-HackPaths")
        {
            // We require at least 1 argument
            ValidateArg(currentArg, 1, command);
            options.hackPaths.clear();

            // store 2 paths: default NPP_JI, MVD_JI
            options.hackPaths.push_back(std::filesystem::path(argList[currentArg + 1]));

            bool isGivenBoth = currentArg + 2 < argList.size() && argList[currentArg + 2][0] != '-';
            if (isGivenBoth) {
                options.hackPaths.push_back(argList[currentArg + 2]);
            }
            else {
                // front stores parent path
                options.hackPaths.push_back(options.hackPaths.front().string() + MVDSubdir);
                options.hackPaths.front() += ColorSubdir;
            }

            currentArg += isGivenBoth ? 2 : 1;
            continue;
            // IMPORTANT: internal member frameCount and totalBatches set in PostProcess()
        }
        if (hackMode && command == "-StoreOutput")
        {
            options.storeOutput = true;
            continue;
        }
        if (hackMode && command == "-BatchIndex")
        {
            ValidateArg(currentArg, 1, command);
            options.batchIndex = std::stoull(argList[currentArg + 1]);  // size_t is u long long

            currentArg++;
            continue;
        }

        if (hackMode && command == "-AlignFilename")
        {
            options.alignFilename = true;
            continue;
        }

        if (hackMode && command == "-OutputPath")
        {
            ValidateArg(currentArg, 1, command);
            options.outPath = std::filesystem::path(argList[currentArg + 1]);
            currentArg++;
            continue;
        }
    }

    options.PostProcess();
    return options;
}

bool StreamlineSample::HackOptionDef::PostProcess()
{
    // sanity check
    if (hackPaths.size() < 2)
        log::error("HackPaths not given in cmdline args");
    if (!std::filesystem::exists(hackPaths[0]) || !std::filesystem::exists(hackPaths[1]))
        log::error("Not both of Color Path %s and MVD path %s exist", hackPaths[0].c_str(), hackPaths[1].c_str());

    // If outPath not given, set to <parent of Color Path>/outputs
    if (outPath == "") {
        outPath = std::filesystem::path(hackPaths[0]).parent_path() / "outputs";
    }

	// Defensive: create output folder if not exist
    if (!std::filesystem::exists(outPath)) {
        std::filesystem::create_directories(outPath);
	}

    // Set frameCount to MVD count
    frameCount = static_cast<size_t>(std::count_if(
        std::filesystem::directory_iterator(hackPaths[1]),
        std::filesystem::directory_iterator{},
        [](const auto& entry) {return entry.path().extension() == ".exr"; }
    ));
    // Then set totalBatches
    totalBatches = frameCount / FramesToCapture +
        (frameCount % FramesToCapture > 0); // round up
    if (totalBatches == 0)
        log::error("Found %d .exr in %s", frameCount, hackPaths[1].c_str());
    // And cap batchIndex
    batchIndex = std::min(batchIndex, totalBatches - 1);

    size_t colorCount = 0;  // will count later

    // To check duplicate
    size_t minID = std::numeric_limits<size_t>::max();
    std::unordered_set<std::string> allSeenPrefix;

    for (const auto& it : std::filesystem::directory_iterator(hackPaths[0]))
    {
        const auto& fullPath = it.path();
        if (fullPath.extension() != ".exr")
            continue;

        colorCount++;

        // construct prefix until we hit a numeric frameID token
        std::string pathPrefix = "";
        std::istringstream pathSS(fullPath.stem().generic_string());
        for (std::string token; std::getline(pathSS, token, '_'); ) {
            if (!token.empty() && std::all_of(token.begin(), token.end(), ::isdigit)) {
                minID = std::min(minID, std::stoull(token));
                allSeenPrefix.emplace(pathPrefix);
				maxFrameIDLength = std::max(maxFrameIDLength, token.length());
                break;
            }
            else {
                // not frameID yet
                pathPrefix += token + "_";
            }
        } // end parsing one file
    }
    if (allSeenPrefix.empty())
        log::error("%s has no .exr files", hackPaths[0].c_str());
    else if (allSeenPrefix.size() > 1)
        log::error("Found duplicate filename prefix when `alignFilename` is set: %s and %s",
            allSeenPrefix.begin()->c_str(),
            std::next(allSeenPrefix.begin())->c_str());
    else if (alignFilename || identifier == "") {
        // overwrite identifier even if user has set it; NOTE the hashkey has an extra '_'
        identifier = allSeenPrefix.begin()->data();
        identifier.pop_back();

        baseFrameIndex = minID;
    }


    if (frameCount != colorCount)
        log::error("%s and %s don't have equal .exr count: [%d, %d]", hackPaths[0].c_str(), hackPaths[1].c_str(), frameCount, colorCount);


    return true;
}

void StreamlineSample::SetDLSSMode(sl::DLSSMode& upMode)
{
    // init to illegal value for success check.
    upMode = sl::DLSSMode::eOff;
    // actual upscale ratio under user's setting; TODO: shall we allow inconsistent aspect ratio?
    float xRatio = static_cast<float>(hackOptions.displayResolution.x) / hackOptions.renderResolution.x;
    float yRatio = static_cast<float>(hackOptions.displayResolution.y) / hackOptions.renderResolution.y;
    auto isInRange = [xRatio, yRatio](const RatioTriplet& tri) -> bool {
        return (xRatio >= tri.low && xRatio <= tri.high) && (yRatio >= tri.low && yRatio <= tri.high);
    };

    /// Even though the actual ratio is within [low, high] range, there can be a better choice.
    /// E.g. eMaxQuality (1.5x) has [1.0, 2.0] range, but it's better to use eMaxPerformance (2.0x)
    float absDiff = FLT_MAX;
    for (const auto& [mode, triplet] : UpscaleRatioMap) {
        float diffFrom = std::max(abs(xRatio - triplet.optimal), abs(xRatio - triplet.optimal));
        if (isInRange(triplet) && diffFrom < absDiff) {
            upMode = mode;
            absDiff = diffFrom;

        }
    }

    if (upMode == sl::DLSSMode::eOff) {
        log::error("DLSS failed to determine a usable mode. Display: [%d, %d], Render: [%d, %d] their ratio: [%d, %d]",
            hackOptions.displayResolution.x, hackOptions.displayResolution.y,
            hackOptions.renderResolution.x,  hackOptions.renderResolution.y,
            xRatio, yRatio
        );
    }

    // At last, set DLSS mode string, to be used in export filename. Offset by 1 to remove the prefix 'e'
    hackOptions.modeString = magic_enum::enum_name(upMode).substr(1);
    return;
}

bool StreamlineSample::CreateCaptureDevice()
{
    // Create D3D11 device and Convert it step-by-step to WinRT IDirect3DDevice
    ComPtr<ID3D11Device> device;
    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        nullptr,  // D3D_FEATURE_LEVEL*
        0, // above array size
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        nullptr // ID3D11DeviceContext*
    );

    if (FAILED(hr))
    {
        log::error("Failed to create D3D11 device: 0x%08X", hr);
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    hr = device.As(&dxgiDevice);
    if (FAILED(hr))
    {
        log::error("Failed to convert ID3D11Device to IDXGIDevice: 0x%08X", hr);
        return false;
    }

    winrt::com_ptr<IInspectable> inspectableDevice;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectableDevice.put());
    if (FAILED(hr))
    {
        log::error("Failed to create WinRT device from DXGI device: 0x%08X", hr);
        return false;
    }

    m_captureDevice = inspectableDevice.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    return true;
}

bool StreamlineSample::CreateCaptureItemForWindow()
{
    HWND hwnd = glfwGetWin32Window(GetDeviceManager()->GetWindow());
    if (hwnd == nullptr)
    {
        log::error("Can't get HWND from GLFW window");
        return false;
    }

    //// Another hacky way to init m_captureItem, but doesn't change capture rect size.
    //winrt::Windows::UI::WindowId windowID = { .Value = reinterpret_cast<uint64_t>(hwnd) };
    //m_captureItem = winrt::Windows::Graphics::Capture::GraphicsCaptureItem::TryCreateFromWindowId(windowID);
    
    // Use interop interface to create capture item
    auto interop = winrt::get_activation_factory<winrt::Windows::Graphics::Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    winrt::check_hresult(interop->CreateForWindow(
        hwnd,
        winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        reinterpret_cast<void**>(winrt::put_abi(m_captureItem))
    ));
    auto captureRectSize = m_captureItem.Size();
    hackExportBytesPerFrameFG = static_cast<size_t>(captureRectSize.Width) *
        static_cast<size_t>(captureRectSize.Height) *
		4 * 2; // RGBA16_FLOAT
    return true;
}

bool StreamlineSample::InitializeFramePoolCapture()
{
    // 1. Create D3D11 device
    if (!CreateCaptureDevice() || m_captureDevice == nullptr)
    {
        log::error("Failed to create capture device");
        return false;
    }

    // 2. Create capture item
    if (!CreateCaptureItemForWindow() || m_captureItem == nullptr)
    {
        log::error("Failed to create capture item");
        return false;
    }

    m_captureInitialized = true;
    return true;
}

void StreamlineSample::CleanupFramePoolCapture()
{
    m_captureItem = nullptr;
    m_captureDevice = nullptr;
    m_captureInitialized = false;
}

/**
 * Helper to convert Windows::Graphics::DirectX::Direct3D11 resources to native D3D11 resources
 * 
 * \param object: We will use IDirect3DDevice and IDirect3DSurface as inputs
 */
template<typename T>
static winrt::com_ptr<T> GetDXGIInterfaceFromObject(winrt::Windows::Foundation::IInspectable const& object)
{
    // Cast to the interface access type
    auto access = object.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

    // Get the requested interface
    winrt::com_ptr<T> result;
    winrt::check_hresult(access->GetInterface(winrt::guid_of<T>(), result.put_void()));
    return result;
}

bool StreamlineSample::SaveIfUniqueTexture(winrt::com_ptr<ID3D11Device> device, winrt::com_ptr<ID3D11Texture2D> texture, const std::string& filename)
{
    // Create staging texture
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);

    /// This will happen in windowed mode, i.e. (deviceParams.startFullscreen = false)
    /// E.g. A 2562 x 1453 window will be displayed for 2560 x 1440 display resolution.
    //if (desc.Width != hackOptions.displayResolution.x || desc.Height != hackOptions.displayResolution.y) {
    //    log::warning("Captured FG frame size [%d, %d] mismatches expected display resolution [%d, %d]",
    //        desc.Width, desc.Height,
    //        hackOptions.displayResolution.x, hackOptions.displayResolution.y);
    //    log::warning("Likely due to (1) starting in windowed mode (2) Monitor smaller than specified display resolution. Will skip FG frame capture.");
    //    // To stop retrying another capture
    //    return true;
    //}

    const int width = desc.Width;
    const int height = desc.Height;
    const size_t bytesPerPixel = 4 * 2; // RGBA16_FLOAT
    //const size_t rowPitchBytes = width * bytesPerPixel;

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D> stagingTexture;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, stagingTexture.put());
    if (FAILED(hr))
    {
        log::error("Failed to create staging texture: 0x%08X", hr);
        return false;
    }

    // Copy to staging texture
    winrt::com_ptr<ID3D11DeviceContext> context;
    device->GetImmediateContext(context.put());
    context->CopyResource(stagingTexture.get(), texture.get());

    // Map the staging texture
    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        log::error("Failed to map staging texture: 0x%08X", hr);
        return false;
    }

    // Check if data is contiguous (common optimization)
    //if (mapped.RowPitch != width * bytesPerPixel) {
    //    log::error("Not contiguous texture data: width is %d but row pitch is %d. "
    //        "Try turn on fullscreen mode in main.cpp(%d now)",
    //        width, mapped.RowPitch, GetDeviceManager()->GetDeviceParams().startFullscreen);
    //}
    // returns XXH64_hash_t which is ull
    uint64_t hash64 = XXH64(mapped.pData, width * height * bytesPerPixel, 0 /* use consistent seed */);

    // process and return accordingly
    bool uniqueHash = !hash_bin.contains(hash64);;
    if (uniqueHash) {
        // unique, save it
        uint8_t* slotStart = hackExportMemoryPoolFG.get() + hackExportSlotFG * hackExportBytesPerFrameFG;

        // We already ensure the actually captured window size matches target display resolution.
        std::memcpy(slotStart, mapped.pData, hackExportBytesPerFrameFG);

        FrameData frameData = { slotStart, mapped.RowPitch, width, height, filename };

        hash_bin.emplace(hash64, std::move(frameData));
    }
    else if (hackOptions.totalBatches == 1 
        && GetFrameIndex() >= FramesToWarmup + hackOptions.frameCount ) 
    {
        /// A very rare and special case, total inputs (frameCount) < 15, 
        /// e.g. 10, then the 3 + 15 + 1 frames loaded will be 
        /// (warmup 7 8 9) (capture 0 to 9, 0 to 4), (safety 5),
        /// we need to ignore those duplications 0-4.
        uniqueHash = true;
    }
    
    // final cleanup no matter success or not
    context->Unmap(stagingTexture.get(), 0);
	stagingTexture = nullptr;
    return uniqueHash;
}


void StreamlineSample::CaptureFramePoolHDR(const std::string& filename)
{
    auto d3dDevice = GetDXGIInterfaceFromObject<ID3D11Device>(m_captureDevice);
    winrt::com_ptr<ID3D11DeviceContext> d3dContext;
    d3dDevice->GetImmediateContext(d3dContext.put());

    // Creating our frame pool with CreateFreeThreaded means that we 
    // will be called back from the frame pool's internal worker thread
    // instead of the thread we are currently on. It also disables the
    // DispatcherQueue requirement.
    auto framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
        m_captureDevice,
        DirectXPixelFormat::R16G16B16A16Float,
        1,  // num of buffers, ensure we either store correct frame or fail (don't store)
        m_captureItem.Size());
    auto session = framePool.CreateCaptureSession(m_captureItem);

    wil::shared_event captureEvent(wil::EventOptions::ManualReset);
    Direct3D11CaptureFrame frame{ nullptr };
    framePool.FrameArrived([&frame, captureEvent](auto& framePool, auto&)
        {
            frame = framePool.TryGetNextFrame();

            // Complete the operation
            captureEvent.SetEvent();
        });

    session.StartCapture();

    // repeat until we successfully save a unique new frame
    for (uint32_t rep = 0; rep < DuplicateMaxRetry; rep++) {
        // sync wait, signature:
        // bool wait(DWORD dwMilliseconds = INFINITE, BOOL bAlertable = FALSE) const WI_NOEXCEPT
        captureEvent.wait(CaptureTimeoutMS);

        // We may get nothing within the timeout
        if (frame == nullptr) {
            // Reset for next capture
            captureEvent.ResetEvent();
            continue;
        }

        auto texture = GetDXGIInterfaceFromObject<ID3D11Texture2D>(frame.Surface());
        assert(texture != nullptr);

        if (SaveIfUniqueTexture(d3dDevice, texture, filename)) {
            break;
        }
        else {
            std::this_thread::sleep_for(DuplicateTimeout);
            // Reset for next capture
            frame = nullptr;
            captureEvent.ResetEvent();
        }
    }

    // End the capture
    session.Close();
    framePool.Close();
    return;

}

bool StreamlineSample::MapRenderTargetDataHDR(nvrhi::TextureHandle texture, const std::string& filename)
{
    const auto& desc = texture->getDesc();
    if (desc.format != nvrhi::Format::RGBA16_FLOAT) {
        log::error("MapRenderTargetDataHDR only supports RGBA16_FLOAT texture");
	}
    if (desc.width != hackOptions.displayResolution.x || desc.height != hackOptions.displayResolution.y) {
        log::error("MapRenderTargetDataHDR texture size [%d, %d] mismatches expected display resolution [%d, %d]",
            desc.width, desc.height,
            hackOptions.displayResolution.x, hackOptions.displayResolution.y);
	}

    // Create command list and staging texture
    nvrhi::CommandListHandle commandList = GetDevice()->createCommandList();
    commandList->open();

    nvrhi::StagingTextureHandle stagingTexture = GetDevice()->createStagingTexture(desc, nvrhi::CpuAccessMode::Read);
    commandList->copyTexture(stagingTexture, nvrhi::TextureSlice(), texture, nvrhi::TextureSlice());

    commandList->close();
    GetDevice()->executeCommandList(commandList);

    // Map staging texture - get raw data pointer
    size_t rowPitchBytes = 0;
    const void* rawData = GetDevice()->mapStagingTexture(
        stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitchBytes);

    uint64_t hash64 = XXH64(rawData, desc.width * desc.height * 8 /* bytesPerPixel */, 0 /* use consistent seed */);
    // process and return accordingly
    bool uniqueHash = !hash_bin.contains(hash64);;
    if (uniqueHash) {
        uint8_t* slotStart = hackExportMemoryPoolSR.get() + hackExportSlotSR * hackExportBytesPerFrame;
        std::memcpy(slotStart, rawData, hackExportBytesPerFrame);

        FrameData frameData = { slotStart, rowPitchBytes, desc.width, desc.height, filename};

        hash_bin.emplace(hash64, std::move(frameData));
    }

    // in the end
    GetDevice()->unmapStagingTexture(stagingTexture);
    stagingTexture = nullptr;
    return uniqueHash;
}

void StreamlineSample::DecideExportInfo()
{
    if (!(hackOptions.enableHack && hackOptions.storeOutput))
        return;


	auto frameIdx = GetFrameIndex();

    /// FramesToWarmup (constexpr 3) is to solve the DLSS-G cold start problem.
    /// Without it, we observed the following {frameIdx; SR data from which frame; FG data from which frame;}:
    /// {0; BLACK (window not opened yet); Frame 0;}
    /// {1; Frame 0; Frame 1;}
    /// {2; Frame 1; Frame 1 (This is weird);}
    /// {3; Frame 2; Frame 2.5 (correct);} 
    /// And stays correct afterwards.
    /// 
    /// The solution is to let DLSS-G warmup for 3 frames before we start capturing.
    /// Previous 3 frames are loaded. See LoadHackTextures() on how exactly "previous 3" is defined according to batchIndex.
    /// 
    /// After adjusting raw frameIdx with 3 (let's call it t, t = frameIdx - 3), 
    /// we further need to map it to the actual frameID matching the data being captured.
    /// SR and FG frameID have different mapping logic:
    /// 
    /// SR logic is easy, since we directly read from internal RT AAResolvedColor.
    /// When calling SR capture function at frame t in RenderScene(), SR result of frame t+1 is captured.
    /// 
    /// FG logic is more tricky, since we have to use front-end approach to capture what's being Present().
    /// When calling FG capture function at frame t in afterPresent callback, FG result of frame t-1 is captured, 
    /// 
    /// Putting them together and thinking reversely, for exported data of frame 0 to 14 (FramesToCapture = 15),
	/// SR data come from frameIdx 2 to 16 (t = -1 to 13);
    /// FG data come from frameIdx 4 to 18 (t = +1 to 15);

    /// We use immediately evaluated lambda to enable early return

    /* SR */
    hackExportFilenameSR = [&]() -> std::string {
        constexpr uint32_t RawFrameStart = FramesToWarmup - 1;
        if (frameIdx >= RawFrameStart && frameIdx < RawFrameStart + FramesToCapture) // within range
        {
            uint32_t fid = frameIdx - RawFrameStart // batch-local matching frameID, 0 to 14
                + hackOptions.batchIndex * FramesToCapture; // global matching frameID, 0 to 59

            // if frameCount = 50, frame 50-59 does not exist
            if (fid >= hackOptions.frameCount)
                return "";

            if (hackOptions.alignFilename)
                fid += hackOptions.baseFrameIndex;

            // align frameID to 4 digits, e.g. "3" to "0003" for cleaner folder view.
            std::string frameIdStr = std::string(hackOptions.maxFrameIDLength - std::to_string(fid).length(), '0')
                + std::to_string(fid) + "_";

            return std::filesystem::absolute(hackOptions.outPath).string() + "/" +
                hackOptions.identifier + "_" + frameIdStr + hackOptions.modeString + ".exr";
        }

        return "";
    }();


    /* FG */
	hackExportFilenameFG = [&]() -> std::string {
        constexpr uint32_t RawFrameStart = FramesToWarmup + 1;
        if (frameIdx >= RawFrameStart && frameIdx < RawFrameStart + FramesToCapture) // within range
        {
            // map frame N to frame N - 1
            uint32_t fid = frameIdx - RawFrameStart // batch-local matching frameID, 0 to 14
                + hackOptions.batchIndex * FramesToCapture; // global matching frameID, 0 to 59

            // if frameCount = 50, frame 50-59 does not exist
            if (fid >= hackOptions.frameCount)
                return "";

            if (hackOptions.alignFilename)
                fid += hackOptions.baseFrameIndex;

            // align frameID to 4 digits, e.g. "3" to "0003" for cleaner folder view.
            std::string frameIdStr = std::string(hackOptions.maxFrameIDLength - std::to_string(fid).length(), '0')
                + std::to_string(fid) + "_";

            return std::filesystem::absolute(hackOptions.outPath).string() + "/" +
                hackOptions.identifier + "_" + frameIdStr + hackOptions.modeString + "_fg.exr";
		}

        return "";
	}();

    return;
}

// Constructor
StreamlineSample::StreamlineSample(
    DeviceManager* deviceManager,
    sl::ViewportHandle vpHandle,
    UIData& ui,
    const std::string& sceneName,
    ScriptingConfig scriptingConfig)
    : Super(deviceManager)
    , m_viewport(vpHandle)
    , m_ui(ui)
    , m_BindingCache(deviceManager->GetDevice())
    , m_ScriptingConfig(scriptingConfig)
{

    m_ui.DLSS_Supported = NVWrapper::Get().GetDLSSAvailable();
    m_ui.REFLEX_Supported = NVWrapper::Get().GetReflexAvailable();
    m_ui.NIS_Supported = NVWrapper::Get().GetNISAvailable();
    m_ui.DeepDVC_Supported = NVWrapper::Get().GetDeepDVCAvailable();
    m_ui.DLSSG_Supported = NVWrapper::Get().GetDLSSGAvailable();
#ifdef STREAMLINE_FEATURE_DLSS_RR
    m_ui.DLSSRR_Supported = NVWrapper::Get().GetDLSSRRAvailable();
#endif // STREAMLINE_FEATURE_DLSS_RR
#if STREAMLINE_FEATURE_LATEWARP
    m_ui.Latewarp_Supported = NVWrapper::Get().GetLatewarpAvailable();
#endif

    std::shared_ptr<NativeFileSystem> nativeFS = std::make_shared<NativeFileSystem>();

    std::filesystem::path mediaPath = app::GetDirectoryWithExecutable().parent_path() / "media";
    std::filesystem::path frameworkShaderPath = app::GetDirectoryWithExecutable() / "shaders/framework" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());
    std::filesystem::path appShaderPath = app::GetDirectoryWithExecutable() / "shaders/StreamlineSample" / app::GetShaderTypeName(GetDevice()->getGraphicsAPI());


    m_RootFs = std::make_shared<RootFileSystem>();
    m_RootFs->mount("/media", mediaPath);
    m_RootFs->mount("/shaders/donut", frameworkShaderPath);
    m_RootFs->mount("/native", nativeFS);
#ifdef STREAMLINE_FEATURE_DLSS_RR
    m_RootFs->mount("/shaders/app", appShaderPath);
#endif // STREAMLINE_FEATURE_DLSS_RR
    m_TextureCache = std::make_shared<TextureCache>(GetDevice(), m_RootFs, nullptr);

    m_ShaderFactory = std::make_shared<ShaderFactory>(GetDevice(), m_RootFs, "/shaders");
    m_CommonPasses = std::make_shared<CommonRenderPasses>(GetDevice(), m_ShaderFactory);

    m_OpaqueDrawStrategy = std::make_shared<InstancedOpaqueDrawStrategy>();
    m_TransparentDrawStrategy = std::make_unique<render::TransparentDrawStrategy>();

    const nvrhi::Format shadowMapFormats[] = {
        nvrhi::Format::D24S8,
        nvrhi::Format::D32,
        nvrhi::Format::D16,
        nvrhi::Format::D32S8 };

    const nvrhi::FormatSupport shadowMapFeatures =
        nvrhi::FormatSupport::Texture |
        nvrhi::FormatSupport::DepthStencil |
        nvrhi::FormatSupport::ShaderLoad;

    nvrhi::Format shadowMapFormat = nvrhi::utils::ChooseFormat(GetDevice(), shadowMapFeatures, shadowMapFormats, std::size(shadowMapFormats));

    m_ShadowMap = std::make_shared<CascadedShadowMap>(GetDevice(), 2048, 4, 0, shadowMapFormat);
    m_ShadowMap->SetupProxyViews();

    m_ShadowFramebuffer = std::make_shared<FramebufferFactory>(GetDevice());
    m_ShadowFramebuffer->DepthTarget = m_ShadowMap->GetTexture();

    DepthPass::CreateParameters shadowDepthParams;
    shadowDepthParams.slopeScaledDepthBias = 4.f;
    shadowDepthParams.depthBias = 100;
    m_ShadowDepthPass = std::make_shared<DepthPass>(GetDevice(), m_CommonPasses);
    m_ShadowDepthPass->Init(*m_ShaderFactory, shadowDepthParams);

    m_CommandList = GetDevice()->createCommandList();

    m_FirstPersonCamera.SetMoveSpeed(3.0f);

    SetAsynchronousLoadingEnabled(false);

    if (sceneName.empty())
        SetCurrentSceneName("/media/sponza-plus.scene.json");
    else
        SetCurrentSceneName("/native/" + sceneName);

    if (!InitializeFramePoolCapture()) {
        log::error("Init FramePool resources failed");
    }

#ifdef STREAMLINE_FEATURE_DLSS_RR
    if(GetDevice()->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
    {   
        m_ConstantBuffer = GetDevice()->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(LightingConstants), "LightingConstants", engine::c_MaxRenderPassConstantBufferVersions));
        if (!CreateRayTracingPipeline(*m_ShaderFactory))
        {
            return;
        }
        m_CommandList->open();
        CreateAccelStruct(m_CommandList);
        m_CommandList->close();

        GetDevice()->executeCommandList(m_CommandList);
        GetDevice()->waitForIdle();
    }
#endif // STREAMLINE_FEATURE_DLSS_RR

    // Set the callbacks for Reflex
    deviceManager->m_callbacks.beforeFrame   = [](donut::app::DeviceManager &m, uint32_t f){ NVWrapper::Get().ReflexCallback_Sleep(m, f); };
    deviceManager->m_callbacks.beforeAnimate = [](donut::app::DeviceManager &m, uint32_t f){ NVWrapper::Get().ReflexCallback_SimStart(m, f); };
    deviceManager->m_callbacks.afterAnimate  = [](donut::app::DeviceManager &m, uint32_t f){ NVWrapper::Get().ReflexCallback_SimEnd(m, f); };
    deviceManager->m_callbacks.beforeRender  = [](donut::app::DeviceManager &m, uint32_t f){ NVWrapper::Get().ReflexCallback_RenderStart(m, f); };
    deviceManager->m_callbacks.afterRender   = [](donut::app::DeviceManager &m, uint32_t f){ NVWrapper::Get().ReflexCallback_RenderEnd(m, f); };
    
    deviceManager->m_callbacks.beforePresent = [this](donut::app::DeviceManager& m, uint32_t frameIdx) {
        NVWrapper::Get().ReflexCallback_PresentStart(m, frameIdx);

        DecideExportInfo();
    };

    deviceManager->m_callbacks.afterPresent  = [this](donut::app::DeviceManager &m, uint32_t frameIdx) {

        if (!hackExportFilenameFG.empty()) {
            CaptureFramePoolHDR(hackExportFilenameFG);
            hackExportSlotFG++;
        }

        NVWrapper::Get().ReflexCallback_PresentEnd(m, frameIdx); 
    };

    if (m_ScriptingConfig.Reflex_mode != -1 && NVWrapper::Get().GetReflexAvailable()) {
        static constexpr std::array<int, 3> ValidReflexIndices{ 0,1,2 };
        if (std::find(ValidReflexIndices.begin(), ValidReflexIndices.end(), m_ScriptingConfig.Reflex_mode) != ValidReflexIndices.end()) { // CHECK IF THE DLSS MODE IS VALID
            m_ui.REFLEX_Mode = m_ScriptingConfig.Reflex_mode;
        }
    }

    if (m_ScriptingConfig.Reflex_fpsCap > 0 && NVWrapper::Get().GetReflexAvailable())
        m_ui.REFLEX_CapedFPS = m_ScriptingConfig.Reflex_fpsCap;

    if (m_ScriptingConfig.DLSS_mode != -1 && NVWrapper::Get().GetDLSSAvailable()) {
        static constexpr std::array<int, 6> ValidDLLSIndices{ 0,1,2,3,4,6 };
        if (std::find(ValidDLLSIndices.begin(), ValidDLLSIndices.end(), m_ScriptingConfig.DLSS_mode) != ValidDLLSIndices.end()) { // CHECK IF THE DLSS MODE IS VALID
            m_ui.AAMode = AntiAliasingMode::DLSS;
            m_ui.DLSS_Mode = static_cast<sl::DLSSMode>(m_ScriptingConfig.DLSS_mode);
        }
    }
    m_ui.DLSSPresetsReset();

#ifdef STREAMLINE_FEATURE_DLSS_RR
    if (m_ScriptingConfig.DLSSRR_mode != -1 && NVWrapper::Get().GetDLSSRRLastEnable()) {
        static constexpr std::array<int, 6> ValidDLLSRRIndices{ 0,1,2,3,4,6 };
        if (std::find(ValidDLLSRRIndices.begin(), ValidDLLSRRIndices.end(), m_ScriptingConfig.DLSSRR_mode) != ValidDLLSRRIndices.end()) { // CHECK IF THE DLSSRR_mode MODE IS VALID
            m_ui.DLSSRR_Mode = static_cast<sl::DLSSMode>(m_ScriptingConfig.DLSSRR_mode);
        }
    }
    m_ui.DLSSRRPresetsReset();
#endif // STREAMLINE_FEATURE_DLSS_RR

    if (m_ScriptingConfig.DLSSG_on != -1 && NVWrapper::Get().GetDLSSGAvailable() && NVWrapper::Get().GetReflexAvailable()) {
        if (m_ui.REFLEX_Mode == 0) 
            m_ui.REFLEX_Mode = 1;
        m_ui.DLSSG_mode = sl::DLSSGMode::eOn;
        if (m_ScriptingConfig.DLSSG_numFrameToGenerate != -1)
        {
            m_ui.DLSSG_numFrames = m_ScriptingConfig.DLSSG_numFrameToGenerate + 1;
        }
    }

    if (m_ScriptingConfig.DeepDVC_on != -1 && NVWrapper::Get().GetDeepDVCAvailable()) {
        m_ui.DeepDVC_Mode = sl::DeepDVCMode::eOn;
    }

    if (m_ScriptingConfig.Latewarp_on != -1 && NVWrapper::Get().GetLatewarpAvailable() && NVWrapper::Get().GetReflexAvailable() && NVWrapper::Get().GetPCLAvailable()) {
        if (m_ui.Latewarp_active == 0) 
            m_ui.Latewarp_active = 1;
    }

    if (m_ScriptingConfig.GpuLoad != -1)
    {
        m_ui.GpuLoad = m_ScriptingConfig.GpuLoad;
    }

    //InitializeMediaCapture();
};

StreamlineSample::~StreamlineSample()
{
    //CleanupMediaCaptureAsync();
    CleanupFramePoolCapture();

    NVWrapper::Get().SetViewportHandle(m_viewport);
    NVWrapper::Get().CleanupDLSS(true);
#ifdef STREAMLINE_FEATURE_DLSS_RR
    NVWrapper::Get().CleanupDLSSRR(true);
#endif // STREAMLINE_FEATURE_DLSS_RR
    NVWrapper::Get().CleanupDLSSG(false);

    #if STREAMLINE_FEATURE_LATEWARP
    NVWrapper::Get().CleanupLatewarp(true);
#endif
}

winrt_foundation::IAsyncAction StreamlineSample::CaptureMediaAsync(std::string filename) {
    assert(false, "CaptureMediaAsync() deprecated");
    auto captureStart = std::chrono::high_resolution_clock::now();

    try {
        // Initialize MediaCapture
        m_mediaCapture = MediaCapture();
        auto initSettings = MediaCaptureInitializationSettings();
        initSettings.StreamingCaptureMode(StreamingCaptureMode::Video);
        co_await m_mediaCapture.InitializeAsync(initSettings);

        // Check HDR support
        auto supportedModes = m_mediaCapture.VideoDeviceController().AdvancedPhotoControl().SupportedModes();
        m_hdrSupported = false;
        for (auto&& mode : supportedModes) {
            if (mode == AdvancedPhotoMode::Hdr) {
                m_hdrSupported = true;
                break;
            }
        }

        // Configure capture mode
        AdvancedPhotoMode photoMode = m_hdrSupported ? AdvancedPhotoMode::Hdr : AdvancedPhotoMode::Standard;
        AdvancedPhotoCaptureSettings settings;
        settings.Mode(photoMode);
        m_mediaCapture.VideoDeviceController().AdvancedPhotoControl().Configure(settings);

        // Prepare capture
        m_advancedCapture = co_await m_mediaCapture.PrepareAdvancedPhotoCaptureAsync(
            ImageEncodingProperties::CreateHeif());
    }
    catch (const winrt::hresult_error& ex) {
        log::error("Init failed [0x%08X]: %ls", ex.code(), ex.message().c_str());
        CleanupMediaCaptureAsync();
        co_return;
    }

    if (!m_hdrSupported) {
        log::warning("HDR not supported, using Standard mode");
    }

    try {
        // Capture photo
        auto advancedCapturedPhoto = co_await m_advancedCapture.CaptureAsync();
        auto frame = advancedCapturedPhoto.Frame();

        // Save to file
        std::filesystem::path absolutePath = std::filesystem::absolute(hackOptions.outPath);
        auto tempFolder = co_await StorageFolder::GetFolderFromPathAsync(
            winrt::to_hstring(absolutePath.string()));
        auto photoFile = co_await tempFolder.CreateFileAsync(
            winrt::to_hstring(filename), CreationCollisionOption::ReplaceExisting);
        auto stream = co_await photoFile.OpenAsync(FileAccessMode::ReadWrite);
        co_await RandomAccessStream::CopyAndCloseAsync(frame, stream);
    }
    catch (const winrt::hresult_error& ex) {
        log::error("Capture failed [0x%08X]: %ls", ex.code(), ex.message().c_str());
    }

    CleanupMediaCaptureAsync();

    // Handle minimum display time
    auto elapsedMS = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - captureStart).count();
    if (int64_t remainingWait = StoreDelayMS - elapsedMS; remainingWait > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(remainingWait));
    }
    else {
        log::error("StoreDelayMS=%d too low, capture took %d ms", StoreDelayMS, elapsedMS);
    }
}

winrt_foundation::IAsyncAction StreamlineSample::CaptureAppRecordingAsync(std::string filename) {
    auto captureStart = std::chrono::high_resolution_clock::now();

    try {
        // Get the AppRecordingManager
        auto recordingManager = AppRecordingManager::GetDefault();

        // Check if screenshot is supported
        AppRecordingStatus status = recordingManager.GetStatus();
        if (!status.CanRecord()) {
            log::error("Screenshot not supported in current state");
            co_return;
        }

        // Convert path and prepare storage
        std::filesystem::path absolutePath = std::filesystem::absolute(hackOptions.outPath);
        auto folder = co_await StorageFolder::GetFolderFromPathAsync(
            winrt::to_hstring(absolutePath.string()));

        // Extract filename without extension for prefix
        std::filesystem::path filenamePath(filename);
        std::string filenamePrefix = filenamePath.stem().string();

        // Capture screenshot with HDR option
        auto result = co_await recordingManager.SaveScreenshotToFilesAsync(
            folder,
            winrt::to_hstring(filenamePrefix),
            AppRecordingSaveScreenshotOption::HdrContentVisible,
            { winrt::to_hstring(".png") } // Request PNG format
        );

        if (result.Succeeded()) {
            for (auto const& savedScreenshot : result.SavedScreenshotInfos()) {
                log::info("Screenshot saved: %ls", savedScreenshot.File().Name().c_str());
            }
        }
        else {
            log::error("Screenshot capture failed");
        }
    }
    catch (const winrt::hresult_error& ex) {
        log::error("AppRecording failed [0x%08X]: %ls", ex.code(), ex.message().c_str());
    }

    // Handle minimum display time
    auto elapsedMS = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - captureStart).count();

    if (int64_t remainingWait = StoreDelayMS - elapsedMS; remainingWait > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(remainingWait));
    }
    else {
        log::error("StoreDelayMS=%d too low, capture took %d ms", StoreDelayMS, elapsedMS);
    }
}

// Cleanup
winrt_foundation::IAsyncAction StreamlineSample::CleanupMediaCaptureAsync() {
    if (m_advancedCapture) {
        try {
            co_await m_advancedCapture.FinishAsync();
        }
        catch (...) {
            // Suppress errors during cleanup
        }
        m_advancedCapture = nullptr;
    }

    if (m_mediaCapture) {
        m_mediaCapture.Close();
        m_mediaCapture = nullptr;
    }
}

bool StreamlineSample::LoadHackTextures(std::shared_ptr<donut::engine::TextureCache> textureCache)
{
    // Use a hash map to detect duplication.
    std::map<std::pair<uint32_t, uint32_t>, std::string> allSeenResolution;

    const auto& colorPath = hackOptions.hackPaths[0];
    const auto& mvdPath = hackOptions.hackPaths[1];
    // Must add all (e.g. 60) file paths for batch loading logic, see below.
    auto populatePathList = [](std::filesystem::path folderPath) -> std::vector<std::filesystem::path>
        {
            std::vector<std::filesystem::path> outPaths;
            for (const auto& entry : std::filesystem::directory_iterator(folderPath))
            {
                if (entry.path().extension() == ".exr")
                    outPaths.push_back(entry.path());
            }
            // Sort by frame order (assuming filenames contain frameID and start differing from it)
            std::sort(outPaths.begin(), outPaths.end());

            return outPaths;
        };
    const auto colorFiles = populatePathList(colorPath);
    const auto mvdFiles = populatePathList(mvdPath);
    if (colorFiles.size() != mvdFiles.size()) {
        log::error("Different exr file count in %s (%d) and %s (%d)", 
            colorPath.c_str(), colorFiles.size(),
            mvdPath.c_str(), mvdFiles.size());
    }
    const size_t totalInputFrames = colorFiles.size();

    /// Here, we ALWAYS read 3 (for warm up) + 15 + 1 (for computing last 15th frame correctly) inputs.
    /// However, note that when batchIndex = 0, we MUST use three frame 0 to keep history unpolluted.
    /// E.g. For a 60-frame scene with 4 batches, {batchIndex, warmup frame indices} should be:
	/// 0: {0,0,0}, 1: {12,13,14}, 2: {27,28,29}, 3: {42,43,44}
    std::vector<size_t> indicesToLoad;
    indicesToLoad.reserve(FramesToReplayTotal);
    size_t idxFirstCaptureFrame = 0;
    if (hackOptions.batchIndex == 0) {
        indicesToLoad.insert(indicesToLoad.begin(), { 0u, 0u, 0u });
    }
    else {
        // Now definitely bigger than FramesToCapture = 15
        idxFirstCaptureFrame = hackOptions.batchIndex * FramesToCapture;
        indicesToLoad.insert(indicesToLoad.begin(), { idxFirstCaptureFrame - 3, idxFirstCaptureFrame - 2, idxFirstCaptureFrame -1 });
    }
	assert((indicesToLoad.size() == 3));
    for (size_t i = 0; i < FramesToCapture; ++i) {
        indicesToLoad.push_back((idxFirstCaptureFrame + i) % totalInputFrames);
	}
    indicesToLoad.push_back((idxFirstCaptureFrame + FramesToCapture) % totalInputFrames);

    /// For last batch, e.g. frameCount = 50, we go from 42 to 49 then wrap around
    for (size_t frameIdx : indicesToLoad) {
        auto loadedColor = textureCache->hackLoadColorFromFile(colorFiles[frameIdx].generic_string());
        hackLoadedColorsHDR.emplace_back(loadedColor);
        auto [loadedMV, loadedDepth] = textureCache->hackLoadMVDFromFile(mvdFiles[frameIdx].generic_string());
        hackLoadedMVs.push_back(loadedMV);
        hackLoadedDepths.push_back(loadedDepth);

        // Instead of operator[], which keeps overwriting string value
        allSeenResolution.try_emplace({ loadedColor->width, loadedColor->height }, loadedColor->path);
        allSeenResolution.try_emplace({ loadedMV->width, loadedMV->height }, loadedMV->path);
        allSeenResolution.try_emplace({ loadedDepth->width, loadedDepth->height }, loadedDepth->path);

        if (hackOptions.parseJitter) {
            hackLoadedJitterOffsets.emplace_back(
                textureCache->hackLoadJitterDataFromFilename(colorFiles[frameIdx].stem().generic_string())
            );
        }
    }

    if (allSeenResolution.size() != 1) {
        // log then abort
        log::warning("Input data don't have consistent resolution");
        for (const auto& [res, pathStr] : allSeenResolution) {
            log::warning("%s: [%d, %d]", pathStr, res.first, res.second);
        }
        log::error("StreamlineSample::LoadHackTextures() ABORT");
    }
    const auto& res = allSeenResolution.begin()->first;
    // final piece of hack options
    hackOptions.renderResolution = int2(res.first, res.second);
    
    return true;
}

void StreamlineSample::CaptureBitBlitLDR(HWND hWnd, std::string filename) {
    // Get window dimensions with DPI awareness
    RECT rect;
    GetClientRect(hWnd, &rect);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    // Get actual screen coordinates
    POINT pt = { 0, 0 };
    ClientToScreen(hWnd, &pt);
    RECT screenRect = { pt.x, pt.y, pt.x + width, pt.y + height };

    // Create device contexts
    HDC hdcScreen = GetDC(nullptr); // Use screen DC instead of window DC
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    // Create 32-bit bitmap (matches most displays)
    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t* bgraData = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hdcMem, (BITMAPINFO*)&bmi,
        DIB_RGB_COLORS, reinterpret_cast<void**>(&bgraData), nullptr, 0);

    if (!hBitmap) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    SelectObject(hdcMem, hBitmap);

    // Capture until we get unique new frame
    while (true) {
        BOOL captureSuccess = BitBlt(hdcMem, 0, 0, width, height, hdcScreen, 0, 0, SRCCOPY);
        if (!captureSuccess) {
            DWORD err = GetLastError();
            log::error("BitBlt failed: %d", err);
            // try again
            continue;
        }

        uint64_t hash64 = XXH64(reinterpret_cast<const void*>(bgraData), 
            width * height * 4 /* bytes per pixel, RBGA8_UNORM */, 0 /* use consistent seed */);
        if (!hash_bin.contains(hash64)) {
            FrameData uselessData = { {}, 0, 0, 0, "" };
            hash_bin.emplace(hash64, uselessData);
            break;
        }
        else {
            // duplicate
            std::this_thread::sleep_for(DuplicateTimeout);
        }
    }

    // Get bitmap data directly from DIB section
    const int pixelCount = width * height;
    std::vector<uint8_t> pixels(width * height * 3);

    // Convert BGRA to RGB; BGRA on little-endian (Windows) is 0xAARRGGBB
    for (int i = 0; i < pixelCount; i++) {
        pixels[i * 3 + 0] = static_cast<uint8_t>((bgraData[i] >> 16) & 0xFF); // R
        pixels[i * 3 + 1] = static_cast<uint8_t>((bgraData[i] >> 8) & 0xFF);  // G
        pixels[i * 3 + 2] = static_cast<uint8_t>(bgraData[i] & 0xFF);         // B
    }

    // Save as PNG
    stbi_write_png(filename.c_str(), width, height, 3, pixels.data(), width * 3);

    // Cleanup
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return;
}

void StreamlineSample::SetLatewarpOptions()
{
#if STREAMLINE_FEATURE_LATEWARP
    sl::LatewarpOptions lwOptions;
    lwOptions.latewarpActive = m_ui.Latewarp_active;
    NVWrapper::Get().SetLatewarpOptions(lwOptions);
#endif

    if (!m_View || !m_ViewPrevious || !m_ui.Latewarp_active)
    {
        return;
    }

    sl::ReflexCameraData cameraData{};
    // m_ViewPrevious is set at the end of Render(), so we need to use a local copy
    std::shared_ptr<PlanarView> planarView = std::dynamic_pointer_cast<PlanarView, IView>(m_View);
    std::shared_ptr<PlanarView> planarViewPrev = std::dynamic_pointer_cast<PlanarView, IView>(m_ViewPrevious);
    cameraData.worldToViewMatrix = make_sl_float4x4(affineToHomogeneous(planarView->GetViewMatrix()));
    cameraData.viewToClipMatrix = make_sl_float4x4(planarView->GetProjectionMatrix(false));
    static sl::float4x4 prevRenderedWorldToViewMatrix = cameraData.worldToViewMatrix;
    static sl::float4x4 prevRenderedViewToClipMatrix = cameraData.viewToClipMatrix;
    cameraData.prevRenderedWorldToViewMatrix = prevRenderedWorldToViewMatrix;
    cameraData.prevRenderedViewToClipMatrix = prevRenderedViewToClipMatrix;

    sl::FrameToken *frameToken = NVWrapper::Get().GetCurrentFrameToken();
    NVWrapper::Get().SetReflexCameraData(*frameToken, cameraData);

    prevRenderedWorldToViewMatrix = cameraData.worldToViewMatrix;
    prevRenderedViewToClipMatrix = cameraData.viewToClipMatrix;
}

// Functions of interest

#ifdef STREAMLINE_FEATURE_DLSS_RR
bool StreamlineSample::CreateRayTracingPipeline(engine::ShaderFactory& shaderFactory)
{   
    std::vector<engine::ShaderMacro> macros = {{"REFLECT_MATERIALS", GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? "0" : "1"}};

    m_ShaderLibrary = shaderFactory.CreateShaderLibrary("app/StreamlineSample.hlsl", &macros);

    if (!m_ShaderLibrary)
        return false;

    nvrhi::BindingLayoutDesc globalBindingLayoutDesc;
    globalBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
    globalBindingLayoutDesc.registerSpace = 0;
    globalBindingLayoutDesc.registerSpaceIsDescriptorSet = true;
    globalBindingLayoutDesc.bindingOffsets.setUnorderedAccessViewOffset(400);
    globalBindingLayoutDesc.bindingOffsets.setConstantBufferOffset(300);
    globalBindingLayoutDesc.bindingOffsets.setShaderResourceOffset(200);

    globalBindingLayoutDesc.bindings = {
        { 0, nvrhi::ResourceType::VolatileConstantBuffer },
        { 0, nvrhi::ResourceType::RayTracingAccelStruct },
        { 1, nvrhi::ResourceType::Texture_SRV },
        { 2, nvrhi::ResourceType::Texture_SRV },
        { 3, nvrhi::ResourceType::Texture_SRV },
        { 4, nvrhi::ResourceType::Texture_SRV },
        { 5, nvrhi::ResourceType::Texture_SRV },
        { 0, nvrhi::ResourceType::Texture_UAV },
        { 1, nvrhi::ResourceType::Texture_UAV },
        { 0, nvrhi::ResourceType::Sampler }
    };

    m_GlobalBindingLayout = GetDevice()->createBindingLayout(globalBindingLayoutDesc);

    if (GetDevice()->getGraphicsAPI() != nvrhi::GraphicsAPI::VULKAN)
    {
        nvrhi::BindingLayoutDesc localBindingLayoutDesc;
        localBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
        localBindingLayoutDesc.registerSpace = 1;
        localBindingLayoutDesc.registerSpaceIsDescriptorSet = true;
        localBindingLayoutDesc.bindings = {
            { 0, nvrhi::ResourceType::TypedBuffer_SRV },
            { 1, nvrhi::ResourceType::TypedBuffer_SRV },
            { 2, nvrhi::ResourceType::TypedBuffer_SRV },
            { 3, nvrhi::ResourceType::Texture_SRV },
            { 4, nvrhi::ResourceType::Texture_SRV },
            { 5, nvrhi::ResourceType::Texture_SRV },
            { 6, nvrhi::ResourceType::Texture_SRV },
            { 0, nvrhi::ResourceType::ConstantBuffer }
        };

        m_LocalBindingLayout = GetDevice()->createBindingLayout(localBindingLayoutDesc);
    }

    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.globalBindingLayouts = { m_GlobalBindingLayout };
    pipelineDesc.shaders = {
        { "", m_ShaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration), nullptr },
        { "", m_ShaderLibrary->getShader("ShadowMiss", nvrhi::ShaderType::Miss), nullptr },
        { "", m_ShaderLibrary->getShader("ReflectionMiss", nvrhi::ShaderType::Miss), nullptr }
    };

    pipelineDesc.hitGroups = { 
        { 
            "ShadowHitGroup",
            nullptr, // closestHitShader
            nullptr, // anyHitShader
            nullptr, // intersectionShader
            nullptr, // bindingLayout
            false  // isProceduralPrimitive
        },
        { 
            "ReflectionHitGroup", 
            m_ShaderLibrary->getShader("ReflectionClosestHit", nvrhi::ShaderType::ClosestHit), 
            nullptr, // anyHitShader
            nullptr, // intersectionShader
            GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN ? nullptr : m_LocalBindingLayout, 
            false // isProceduralPrimitive
        },
    };

    pipelineDesc.maxPayloadSize = 2 * sizeof(dm::float4);
    pipelineDesc.maxRecursionDepth = 2;

    m_Pipeline = GetDevice()->createRayTracingPipeline(pipelineDesc);

    m_ShaderTable = m_Pipeline->createShaderTable();
    m_ShaderTable->setRayGenerationShader("RayGen");
    m_ShaderTable->addMissShader("ShadowMiss");
    m_ShaderTable->addMissShader("ReflectionMiss");

    for (const auto& mesh : m_Scene->GetSceneGraph()->GetMeshes())
    {
        for (const auto& geometry : mesh->geometries)
        {
            int hitGroupIndex = m_ShaderTable->addHitGroup("ShadowHitGroup", nullptr);
            assert(hitGroupIndex == geometry->globalGeometryIndex * 2);

            if (GetDevice()->getGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
            {
                m_ShaderTable->addHitGroup("ReflectionHitGroup", nullptr);
            }
            else
            {
                nvrhi::BindingSetDesc bindingSetDesc;
                bindingSetDesc.bindings = {
                    nvrhi::BindingSetItem::TypedBuffer_SRV(
                        0,
                        mesh->buffers->indexBuffer,
                        nvrhi::Format::R32_UINT,
                        nvrhi::BufferRange((mesh->indexOffset + geometry->indexOffsetInMesh) * sizeof(uint32_t), geometry->numIndices * sizeof(uint32_t))),
                    nvrhi::BindingSetItem::TypedBuffer_SRV(
                        1,
                        mesh->buffers->vertexBuffer,
                        nvrhi::Format::RG32_FLOAT,
                        nvrhi::BufferRange((mesh->vertexOffset + geometry->vertexOffsetInMesh) * sizeof(dm::float2) + mesh->buffers->getVertexBufferRange(engine::VertexAttribute::TexCoord1).byteOffset, geometry->numVertices * sizeof(dm::float2))),
                    nvrhi::BindingSetItem::TypedBuffer_SRV(
                        2,
                        mesh->buffers->vertexBuffer,
                        nvrhi::Format::RGBA8_SNORM,
                        nvrhi::BufferRange((mesh->vertexOffset + geometry->vertexOffsetInMesh) * sizeof(uint32_t) + mesh->buffers->getVertexBufferRange(engine::VertexAttribute::Normal).byteOffset, geometry->numVertices * sizeof(uint32_t))),
                    nvrhi::BindingSetItem::Texture_SRV(
                        3,
                        geometry->material->baseOrDiffuseTexture && geometry->material->baseOrDiffuseTexture->texture ? geometry->material->baseOrDiffuseTexture->texture : m_CommonPasses->m_WhiteTexture),
                    nvrhi::BindingSetItem::Texture_SRV(
                        4,
                        geometry->material->metalRoughOrSpecularTexture && geometry->material->metalRoughOrSpecularTexture->texture ? geometry->material->metalRoughOrSpecularTexture->texture : m_CommonPasses->m_WhiteTexture),
                    nvrhi::BindingSetItem::Texture_SRV(
                        5,
                        geometry->material->normalTexture && geometry->material->normalTexture->texture ? geometry->material->normalTexture->texture : m_CommonPasses->m_BlackTexture),
                    nvrhi::BindingSetItem::Texture_SRV(
                        6,
                        geometry->material->occlusionTexture && geometry->material->occlusionTexture->texture ? geometry->material->occlusionTexture->texture : m_CommonPasses->m_WhiteTexture),
                    nvrhi::BindingSetItem::ConstantBuffer(
                        0,
                        geometry->material->materialConstants)
                };

                nvrhi::BindingSetHandle localBindingSet = GetDevice()->createBindingSet(bindingSetDesc, m_LocalBindingLayout);
                m_ShaderTable->addHitGroup("ReflectionHitGroup", localBindingSet);
            }
        }
    }

    return true;
}

void StreamlineSample::CreateAccelStruct(nvrhi::ICommandList* commandList)
{

    for (const auto& mesh : m_Scene->GetSceneGraph()->GetMeshes())
    {
        nvrhi::rt::AccelStructDesc blasDesc;
        blasDesc.setBuildFlags(nvrhi::rt::AccelStructBuildFlags::AllowUpdate);
        blasDesc.isTopLevel = false;

        for (const auto& geometry : mesh->geometries)
        {
            nvrhi::rt::GeometryDesc geometryDesc;
            auto& triangles = geometryDesc.geometryData.triangles;
            triangles.indexBuffer = mesh->buffers->indexBuffer;
            triangles.indexOffset = (mesh->indexOffset + geometry->indexOffsetInMesh) * sizeof(uint32_t);
            triangles.indexFormat = nvrhi::Format::R32_UINT;
            triangles.indexCount = geometry->numIndices;
            triangles.vertexBuffer = mesh->buffers->vertexBuffer;
            triangles.vertexOffset = (mesh->vertexOffset + geometry->vertexOffsetInMesh) * sizeof(dm::float3) + mesh->buffers->getVertexBufferRange(engine::VertexAttribute::Position).byteOffset;
            triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
            triangles.vertexStride = sizeof(dm::float3);
            triangles.vertexCount = geometry->numVertices;
            geometryDesc.geometryType = nvrhi::rt::GeometryType::Triangles;
            geometryDesc.flags = nvrhi::rt::GeometryFlags::Opaque;
            blasDesc.bottomLevelGeometries.push_back(geometryDesc);
            }

        if (!mesh->accelStruct)
            {
                // Create new BLAS if it doesn't exist
                mesh->accelStruct = GetDevice()->createAccelStruct(blasDesc);
            }

        // Build or update the existing BLAS
        nvrhi::utils::BuildBottomLevelAccelStruct(commandList, mesh->accelStruct, blasDesc);

    }

    // Update TLAS every frame
    nvrhi::rt::AccelStructDesc tlasDesc;
    tlasDesc.setBuildFlags(nvrhi::rt::AccelStructBuildFlags::AllowUpdate);
    tlasDesc.isTopLevel = true;

    std::vector<nvrhi::rt::InstanceDesc> instances;
    for (const auto& instance : m_Scene->GetSceneGraph()->GetMeshInstances())
    {
        const auto& mesh = instance->GetMesh();

        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc.bottomLevelAS = mesh->accelStruct;
        assert(instanceDesc.bottomLevelAS);
        instanceDesc.instanceMask = 1;
        instanceDesc.instanceContributionToHitGroupIndex = mesh->geometries[0]->globalGeometryIndex * 2;

        auto node = instance->GetNode();
        assert(node);
        dm::affineToColumnMajor(node->GetLocalToWorldTransformFloat(), instanceDesc.transform);

        instances.push_back(instanceDesc);
    }

    tlasDesc.topLevelMaxInstances = instances.size();
    m_TopLevelAS = GetDevice()->createAccelStruct(tlasDesc);
    commandList->buildTopLevelAccelStruct(m_TopLevelAS, instances.data(), instances.size());
}
#endif // STREAMLINE_FEATURE_DLSS_RR

bool StreamlineSample::SetupView()
{

    if (m_TemporalAntiAliasingPass) m_TemporalAntiAliasingPass->SetJitter(m_ui.TemporalAntiAliasingJitter);

    float2 pixelOffset = m_ui.AAMode != AntiAliasingMode::NONE && m_TemporalAntiAliasingPass ? m_TemporalAntiAliasingPass->GetCurrentPixelOffset() : float2(0.f);

    std::shared_ptr<PlanarView> planarView = std::dynamic_pointer_cast<PlanarView, IView>(m_View);

    dm::affine3 viewMatrix;
    float verticalFov = dm::radians(m_CameraVerticalFov);
    float zNear = 0.01f;
    viewMatrix = m_FirstPersonCamera.GetWorldToViewMatrix();

    bool topologyChanged = false;

    // Render View
    {
        if (!planarView)
        {
            m_View = planarView = std::make_shared<PlanarView>();
            m_ViewPrevious = std::make_shared<PlanarView>();
            topologyChanged = true;
        }

        float4x4 projection = perspProjD3DStyleReverse(verticalFov, float(m_RenderingRectSize.x) / m_RenderingRectSize.y, zNear);

        planarView->SetViewport(nvrhi::Viewport((float) m_RenderingRectSize.x, (float)m_RenderingRectSize.y));
        planarView->SetPixelOffset(pixelOffset);

        planarView->SetMatrices(viewMatrix, projection);
        planarView->UpdateCache();

        if (topologyChanged)
        {
            *std::static_pointer_cast<PlanarView>(m_ViewPrevious) = *std::static_pointer_cast<PlanarView>(m_View);
        }
    }

    // ToneMappingView
    {
        std::shared_ptr<PlanarView> tonemappingPlanarView = std::dynamic_pointer_cast<PlanarView, IView>(m_TonemappingView);

        if (!tonemappingPlanarView)
        {
            m_TonemappingView = tonemappingPlanarView = std::make_shared<PlanarView>();
            topologyChanged = true;
        }

        float4x4 projection = perspProjD3DStyleReverse(verticalFov, float(m_RenderingRectSize.x) / m_RenderingRectSize.y, zNear);

        tonemappingPlanarView->SetViewport(nvrhi::Viewport((float)m_DisplaySize.x, (float)m_DisplaySize.y));
        tonemappingPlanarView->SetMatrices(viewMatrix, projection);
        tonemappingPlanarView->UpdateCache();
    }

    return topologyChanged;
}

void StreamlineSample::CreateRenderPasses(bool& exposureResetRequired, float lodBias)
{
    // Safety measure when we recreate the passes.
    GetDevice()->waitForIdle();

    {
        nvrhi::SamplerDesc samplerdescPoint = m_CommonPasses->m_PointClampSampler->getDesc();
        nvrhi::SamplerDesc samplerdescLinear = m_CommonPasses->m_LinearClampSampler->getDesc();
        nvrhi::SamplerDesc samplerdescLinearWrap = m_CommonPasses->m_LinearWrapSampler->getDesc();
        nvrhi::SamplerDesc samplerdescAniso = m_CommonPasses->m_AnisotropicWrapSampler->getDesc();
        samplerdescPoint.mipBias = lodBias;
        samplerdescLinear.mipBias = lodBias;
        samplerdescLinearWrap.mipBias = lodBias;
        samplerdescAniso.mipBias = lodBias;
        m_CommonPasses->m_PointClampSampler = GetDevice()->createSampler(samplerdescPoint);
        m_CommonPasses->m_LinearClampSampler = GetDevice()->createSampler(samplerdescLinear);
        m_CommonPasses->m_LinearWrapSampler = GetDevice()->createSampler(samplerdescLinearWrap);
        m_CommonPasses->m_AnisotropicWrapSampler = GetDevice()->createSampler(samplerdescAniso);
    }

    uint32_t motionVectorStencilMask = 0x01;

    GBufferFillPass::CreateParameters GBufferParams;
    GBufferParams.enableMotionVectors = true;
    GBufferParams.stencilWriteMask = motionVectorStencilMask;
    m_GBufferPass = std::make_unique<GBufferFillPass>(GetDevice(), m_CommonPasses);
    m_GBufferPass->Init(*m_ShaderFactory, GBufferParams);

    m_DeferredLightingPass = std::make_unique<DeferredLightingPass>(GetDevice(), m_CommonPasses);
    m_DeferredLightingPass->Init(m_ShaderFactory);

    m_SkyPass = std::make_unique<SkyPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->ForwardFramebuffer, *m_View);

    // TAA pass is only used if (m_ui.AAMode == AntiAliasingMode::TEMPORAL), 
    // but we have DLSS, thus no need to tag hack render targets here.
    {
        TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = m_RenderTargets->Depth;
        taaParams.motionVectors = m_RenderTargets->MotionVectors;
        taaParams.unresolvedColor = m_RenderTargets->HdrColor;
        taaParams.resolvedColor = m_RenderTargets->AAResolvedColor;
        taaParams.feedback1 = m_RenderTargets->TemporalFeedback1;
        taaParams.feedback2 = m_RenderTargets->TemporalFeedback2;
        taaParams.motionVectorStencilMask = motionVectorStencilMask;
        taaParams.useCatmullRomFilter = true;

        m_TemporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, *m_View, taaParams);
    }

    m_SsaoPass = std::make_unique<SsaoPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->Depth, m_RenderTargets->GBufferNormals, m_RenderTargets->AmbientOcclusion); //DIFFERENCE
    nvrhi::BufferHandle exposureBuffer = nullptr;
    if (m_ToneMappingPass)
        exposureBuffer = m_ToneMappingPass->GetExposureBuffer();
    else
        exposureResetRequired = true;

    m_BloomPass = std::make_unique<BloomPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->HdrFramebuffer, *m_TonemappingView);

    ToneMappingPass::CreateParameters toneMappingParams;
    toneMappingParams.exposureBufferOverride = exposureBuffer;
    m_ToneMappingPass = std::make_unique<ToneMappingPass>(GetDevice(), m_ShaderFactory, m_CommonPasses, m_RenderTargets->LdrFramebuffer, *m_TonemappingView, toneMappingParams);

    m_PreviousViewsValid = false;
}

void MultiViewportApp::RenderScene(nvrhi::IFramebuffer* framebuffer)
{
    int windowWidth = 0, windowHeight = 0;
    GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);

    uint32_t nViewports = m_ui.getNViewports();
    nViewports = std::max(1u, nViewports); // can't have 0 viewports

    for (uint32_t uV = 0; uV < nViewports; ++uV)
    {
        sl::Extent e{};
        if (uV < m_ui.getNViewports())
        {
            e = m_ui.getExtent(windowWidth, windowHeight, uV);
            if (e.width == 0 || e.height == 0 || (int)e.left >= windowWidth || (int)e.top >= windowHeight) // viewport invalid?
            {
                if (uV > 0) // can't skip the viewport 0
                {
                    continue;
                }
                else
                {
                    e = { 0, 0, static_cast<uint32_t>(windowWidth), static_cast<uint32_t>(windowHeight) };
                }
            }
        }

        // if we don't have this viewport - create it
        if (uV >= m_pViewports.size())
        {
            m_pViewports.push_back(this->createViewport());
        }

        // the extent shouldn't go beyond the window boundary
        e.width = std::min(e.width, windowWidth - e.left);
        e.height = std::min(e.height, windowHeight - e.top);

        m_pViewports[uV]->m_pSample->SetBackBufferExtent(e);
        m_pViewports[uV]->m_pSample->RenderScene(framebuffer);
    }
    // erase all unused viewports
    if (nViewports < m_pViewports.size())
    {
        m_pViewports.resize(nViewports);
    }
}

void StreamlineSample::RenderScene(nvrhi::IFramebuffer* framebuffer)
{
    // INITIALISE

    int windowWidth, windowHeight;
    GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
    nvrhi::Viewport windowViewport = nvrhi::Viewport((float)windowWidth, (float)windowHeight);

    m_Scene->RefreshSceneGraph(GetFrameIndex());

    bool exposureResetRequired = false;
    bool needNewPasses = false;

    uint32_t backbufferWidth = framebuffer->getFramebufferInfo().width;
    uint32_t backbufferHeight = framebuffer->getFramebufferInfo().height;

    // Validate resource extent against full resource size.
    auto isViewportExtentValid = [](const sl::Extent& resourceExtent, uint32_t resourceWidth, uint32_t resourceHeight, const std::string& resourceExtentSrc) -> bool
    {   
        bool validExtent = true;
        std::stringstream errorMsg{};
        errorMsg << "Invalid viewport extent input from " << resourceExtentSrc << ", IF optionally specified by the user! Ignoring it.";

        if (resourceExtent.width == 0 || resourceExtent.height == 0)
        {
            errorMsg << "One of the extent dimensions (" << resourceWidth << " x " << resourceHeight << ") is incorrectly zero.";
            validExtent = false;
        }
        else if (resourceExtent.width > resourceWidth || resourceExtent.height > resourceHeight)
        {
            errorMsg << "Extent size (" << resourceExtent.width << " x " << resourceExtent.height << ") exceeds full resource size (" << resourceWidth << " x " << resourceHeight << ").";
            validExtent = false;
        }
        if (resourceExtent.left >= resourceWidth || resourceExtent.top >= resourceHeight)
        {
            errorMsg << "Extent's base offset (" << resourceExtent.left << ", " << resourceExtent.top << ") is >= either of the resource's dimensions ("
                << resourceWidth << " x " << resourceHeight << ").";
            validExtent = false;
        }
        else if ((resourceExtent.left + resourceExtent.width - 1) >= resourceWidth || (resourceExtent.top + resourceExtent.height - 1) >= resourceHeight)
        {
            errorMsg << "Extent region (" << resourceExtent.left << ", " << resourceExtent.top << ", " << resourceExtent.width << " x "
                << resourceExtent.height << ") overflows full resource size (" << resourceWidth << " x " << resourceHeight << ").";
            validExtent = false;
        }

        if (validExtent)
        {
            log::info("Using viewport extent: ( %d, %d, %d x %d )", resourceExtent.left, resourceExtent.top, resourceExtent.width, resourceExtent.height);
        }
        else
        {
            log::warning(errorMsg.str().c_str());
        }

        return validExtent;
    };

    sl::Extent nullExtent{};
    bool validViewportExtent = (m_backbufferViewportExtent != nullExtent);
    if (validViewportExtent)
    {
        m_DisplaySize = int2(m_backbufferViewportExtent.width, m_backbufferViewportExtent.height);
    }
    else
    {
        m_DisplaySize = int2(windowWidth, windowHeight);
    }

    NVWrapper::Get().SetViewportHandle(m_viewport);

    float lodBias = 0.f;

    // RESIZE (from ui)

    if (m_ui.Resolution_changed) {
        // do not resize to GLFW window size if hack
		if (hackOptions.enableHack) {
            m_ui.Resolution.x = windowWidth;
            m_ui.Resolution.y = windowHeight;
        }
        else {
            glfwSetWindowSize(GetDeviceManager()->GetWindow(), m_ui.Resolution.x, m_ui.Resolution.y);
        }
        m_ui.Resolution_changed = false;
    }
    else {
        m_ui.Resolution.x = windowWidth;
        m_ui.Resolution.y = windowHeight;
    }

    // DeepDVC VRAM Usage
    NVWrapper::Get().QueryDeepDVCState(m_ui.DeepDVC_VRAM);

    // DLSS-G Setup
    if (NVWrapper::Get().GetDLSSGAvailable())
    {
        // Query whether NVWrapper thinks that DLSS-FG is wanted
        bool prevDlssgWanted;
        NVWrapper::Get().Get_DLSSG_SwapChainRecreation(prevDlssgWanted);

        // Query whether the UI "wants" DLSS-FG to be active
        bool dlssgWanted = (m_ui.DLSSG_mode != sl::DLSSGMode::eOff);

        // If there is a change, trigger a swapchain recreation
        if (prevDlssgWanted != dlssgWanted)
        {
            NVWrapper::Get().Set_DLSSG_SwapChainRecreation(dlssgWanted);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgConst.mode) and where we set DLSS-G parameters.  
        auto dlssgConst = sl::DLSSGOptions{};
        dlssgConst.mode = m_ui.DLSSG_mode;
        dlssgConst.numFramesToGenerate = m_ui.DLSSG_numFrames - 1; // ui is multiplier (e.g. 2x), subtract 1 to count generated frames only

        // Explicitly manage DLSS-G resources in order to prevent stutter when
        // temporarily disabled.
        dlssgConst.flags |= sl::DLSSGFlags::eRetainResourcesWhenOff;

        // Turn off DLSSG if we are changing the UI
        if (m_ui.MouseOverUI) {
            dlssgConst.mode = sl::DLSSGMode::eOff;
        }

        if (m_ui.DLSS_Resolution_Mode == RenderingResolutionMode::DYNAMIC)
        {
            dlssgConst.flags |= sl::DLSSGFlags::eDynamicResolutionEnabled;
            dlssgConst.dynamicResWidth = m_DisplaySize.x / 2;
            dlssgConst.dynamicResHeight = m_DisplaySize.y / 2;
        }

        // This is where we query DLSS-G minimum swapchain size
        uint64_t estimatedVramUsage;
        sl::DLSSGStatus status;
        int fps_multiplier;
        int minSize = 0;
        int numFramesMaxMultiplier = 0;
        void* pDLSSGInputsProcessingFence{};
        uint64_t lastPresentDLSSGInputsProcessingFenceValue{};
        auto lastDLSSGFenceValue = NVWrapper::Get().GetDLSSGLastFenceValue();
        NVWrapper::Get().QueryDLSSGState(estimatedVramUsage, fps_multiplier, status, minSize, numFramesMaxMultiplier, pDLSSGInputsProcessingFence, lastPresentDLSSGInputsProcessingFenceValue);
        m_ui.DLSSG_numFramesMaxMultiplier = dm::max(numFramesMaxMultiplier + 1, 2);

        if (static_cast<int>(framebuffer->getFramebufferInfo().width) < minSize || 
            static_cast<int>(framebuffer->getFramebufferInfo().height) < minSize) {
            donut::log::info("Swapchain is too small. DLSSG is disabled.");
            dlssgConst.mode = sl::DLSSGMode::eOff;
        }

        auto dlssgEnabledLastFrame = NVWrapper::Get().GetDLSSGLastEnable();
        NVWrapper::Get().SetDLSSGOptions(dlssgConst);

        auto fenceValue = lastPresentDLSSGInputsProcessingFenceValue;
        // This is where we query DLSS-G FPS, estimated VRAM usage and status
        NVWrapper::Get().QueryDLSSGState(estimatedVramUsage, fps_multiplier, status, minSize, numFramesMaxMultiplier, pDLSSGInputsProcessingFence, lastPresentDLSSGInputsProcessingFenceValue);
        assert(fenceValue == lastPresentDLSSGInputsProcessingFenceValue);

        if (pDLSSGInputsProcessingFence != nullptr)
        {
            if (dlssgEnabledLastFrame)
            {
                if (lastPresentDLSSGInputsProcessingFenceValue == 0 || lastPresentDLSSGInputsProcessingFenceValue > lastDLSSGFenceValue)
                {
                    // This wait is redundant until SL DLSS FG allows SMSCG but done for now for demonstration purposes.
                    // It needs to be queued before any of the inputs are modified in the subsequent command list submission.
                    NVWrapper::Get().QueueGPUWaitOnSyncObjectSet(GetDevice(), nvrhi::CommandQueue::Graphics, pDLSSGInputsProcessingFence, lastPresentDLSSGInputsProcessingFenceValue);
                }
            }
            else
            {
                if (lastPresentDLSSGInputsProcessingFenceValue < lastDLSSGFenceValue)
                {
                    assert(false);
                    log::error("Inputs synchronization fence value retrieved from DLSSGState object out of order: \
                    current frame: %ld, last frame: %ld ", lastPresentDLSSGInputsProcessingFenceValue, lastDLSSGFenceValue);
                }
                else if (lastPresentDLSSGInputsProcessingFenceValue != 0)
                {
                    log::info("DLSSG was inactive in the last preseting frame!");
                }
            }
        }

        m_ui.DLSSG_fps = static_cast<float>(fps_multiplier * 1.0f / GetDeviceManager()->GetAverageFrameTimeSeconds());

        if (status != sl::DLSSGStatus::eOk) {
            if (status & sl::DLSSGStatus::eFailResolutionTooLow)
                m_ui.DLSSG_status = "Resolution Too Low";
            else if (status & sl::DLSSGStatus::eFailReflexNotDetectedAtRuntime)
                m_ui.DLSSG_status = "Reflex Not Detected";
            else if (status & sl::DLSSGStatus::eFailHDRFormatNotSupported)
                m_ui.DLSSG_status = "HDR Format Not Supported";
            else if (status & sl::DLSSGStatus::eFailCommonConstantsInvalid)
                m_ui.DLSSG_status = "Common Constants Invalid";
            else if (status & sl::DLSSGStatus::eFailGetCurrentBackBufferIndexNotCalled)
                m_ui.DLSSG_status = "Common Constants Invalid";
            log::warning("Encountered DLSSG State Error: ", m_ui.DLSSG_status.c_str());
        }
        else {
            m_ui.DLSSG_status = "";
        }
    }

    // After we've actually set DLSS-G on/off, free resources
    if (m_ui.DLSSG_cleanup_needed)
    {
        NVWrapper::Get().CleanupDLSSG(false);
        m_ui.DLSSG_cleanup_needed = false;
    }
#if STREAMLINE_FEATURE_LATEWARP
    if (NVWrapper::Get().GetLatewarpAvailable())
    {
        // Latewarp
        bool prevLatewarpWanted;
        NVWrapper::Get().Get_Latewarp_SwapChainRecreation(prevLatewarpWanted);

        bool latewarpWanted = m_ui.Latewarp_active != 0;

        // If there is a change, trigger a swapchain recreation
        if (prevLatewarpWanted != latewarpWanted)
        {
            NVWrapper::Get().Set_Latewarp_SwapChainRecreation(latewarpWanted);
        }
        
    }
    if (m_ui.Latewarp_cleanup_needed)
    {
        NVWrapper::Get().CleanupLatewarp(true);
        m_ui.Latewarp_cleanup_needed = false;
    }
#endif

    // REFLEX Setup

    auto reflexConst = sl::ReflexOptions{};
    reflexConst.mode = (sl::ReflexMode) m_ui.REFLEX_Mode;
    reflexConst.useMarkersToOptimize = true;
    reflexConst.virtualKey = VK_F13;
    reflexConst.frameLimitUs = m_ui.REFLEX_CapedFPS==0 ? 0 : int(1000000./m_ui.REFLEX_CapedFPS);
    NVWrapper::Get().SetReflexConsts(reflexConst);

    bool flashIndicatorDriverAvailable;
    NVWrapper::Get().QueryReflexStats(m_ui.REFLEX_LowLatencyAvailable, flashIndicatorDriverAvailable, m_ui.REFLEX_Stats);
    NVWrapper::Get().SetReflexFlashIndicator(flashIndicatorDriverAvailable);

    // DLSS SETUP

    //Make sure DLSS is available
    if (m_ui.AAMode == AntiAliasingMode::DLSS && !NVWrapper::Get().GetDLSSAvailable())
    {
        log::warning("DLSS antialiasing is not available. Switching to TAA. ");
        m_ui.AAMode = AntiAliasingMode::TEMPORAL;
    }

#ifdef STREAMLINE_FEATURE_DLSS_RR 
    //Make sure DLSSRR is available
    if (m_ui.DLSSRR_Mode != sl::DLSSMode::eOff && !NVWrapper::Get().GetDLSSRRAvailable())
    {
        log::warning("DLSS RR is not available");
        m_ui.DLSSRR_Mode = sl::DLSSMode::eOff;
    }
#endif // STREAMLINE_FEATURE_DLSS_RR

    // Reset DLSS vars if we stop using it
    if (m_ui.DLSS_Last_AA == AntiAliasingMode::DLSS && m_ui.AAMode != AntiAliasingMode::DLSS) {
        DLSS_Last_Mode = sl::DLSSMode::eOff;
        m_ui.DLSS_Mode = sl::DLSSMode::eOff;
        m_DLSS_Last_DisplaySize = { 0,0 };
        NVWrapper::Get().CleanupDLSS(true); // We can also expressly tell SL to cleanup DLSS resources.
    }
    // If we turn on DLSS then we set its default values
    else if (m_ui.DLSS_Last_AA != AntiAliasingMode::DLSS && m_ui.AAMode == AntiAliasingMode::DLSS) {
        DLSS_Last_Mode = sl::DLSSMode::eBalanced;
        m_ui.DLSS_Mode = sl::DLSSMode::eBalanced;
        m_DLSS_Last_DisplaySize = { 0,0 };
    }
    m_ui.DLSS_Last_AA = m_ui.AAMode;

    // If we are using DLSS set its constants; changed default to ON, DLSSMode::eDLAA
    if ((m_ui.AAMode == AntiAliasingMode::DLSS && m_ui.DLSS_Mode != sl::DLSSMode::eOff))
    {
        sl::DLSSOptions dlssConstants = {};
        dlssConstants.mode = m_ui.DLSS_Mode;
        dlssConstants.outputWidth = m_DisplaySize.x;
        dlssConstants.outputHeight = m_DisplaySize.y;
        dlssConstants.colorBuffersHDR = sl::Boolean::eTrue;
        dlssConstants.sharpness = m_RecommendedDLSSSettings.sharpness;

        if (m_ui.DLSSPresetsAnyNonDefault())
        {
            dlssConstants.dlaaPreset = m_ui.DLSS_presets[static_cast<int>(sl::DLSSMode::eDLAA)];
            dlssConstants.qualityPreset = m_ui.DLSS_presets[static_cast<int>(sl::DLSSMode::eMaxQuality)];
            dlssConstants.balancedPreset = m_ui.DLSS_presets[static_cast<int>(sl::DLSSMode::eBalanced)];
            dlssConstants.performancePreset = m_ui.DLSS_presets[static_cast<int>(sl::DLSSMode::eMaxPerformance)];
            dlssConstants.ultraPerformancePreset = m_ui.DLSS_presets[static_cast<int>(sl::DLSSMode::eUltraPerformance)];
        }

        dlssConstants.useAutoExposure = sl::Boolean::eFalse;

        // Changing presets requires a restart of DLSS
        if (m_ui.DLSSPresetsChanged())
            NVWrapper::Get().CleanupDLSS(true);

        m_ui.DLSSPresetsUpdate();

        NVWrapper::Get().SetDLSSOptions(dlssConstants);

        // Check if we need to update the rendertarget size.
        bool DLSS_resizeRequired = (m_ui.DLSS_Mode != DLSS_Last_Mode) || (m_DisplaySize.x != m_DLSS_Last_DisplaySize.x) || (m_DisplaySize.y != m_DLSS_Last_DisplaySize.y);
        
        // HACK update DLSS mode and housekeeping; DLSS_resizeRequired ensures we don't repeat every frame
        if (DLSS_resizeRequired && hackOptions.enableHack) {
            SetDLSSMode(m_ui.DLSS_Mode);

            /// We ultimately want to force set m_RenderingRectSize, which goes out of control under these 2 settings.
            if (m_ui.DLSS_Resolution_Mode == RenderingResolutionMode::DYNAMIC || 
                m_ui.DLSSRR_Mode != sl::DLSSMode::eOff)
                log::error("RenderingResolutionMode::DYNAMIC or DLSSMode::eOff not supported in hack mode");

            // In the regular `else if (m_ui.AAMode == AntiAliasingMode::DLSS)` below, it's set by
            m_RecommendedDLSSSettings.optimalRenderSize = hackOptions.renderResolution;

            if (DLSS_resizeRequired) {
                // skip calling QueryDLSSOptimalSettings() since it overwrite m_RecommendedDLSSSettings
                DLSS_Last_Mode = m_ui.DLSS_Mode;
                m_DLSS_Last_DisplaySize = m_DisplaySize;
                DLSS_resizeRequired = false;
            }
        }

        if (DLSS_resizeRequired) {
            // Only quality, target width and height matter here
            NVWrapper::Get().QueryDLSSOptimalSettings(m_RecommendedDLSSSettings);
            /// The function above will store correct render size to optimalRenderSize and minRenderSize
            /// e.g. display = 1920x1080, renderSize = 960x540

            if (m_RecommendedDLSSSettings.optimalRenderSize.x <= 0 || m_RecommendedDLSSSettings.optimalRenderSize.y <= 0) {
                m_ui.AAMode = AntiAliasingMode::NONE;
                m_ui.DLSS_Mode = sl::DLSSMode::eBalanced;
                m_RenderingRectSize = m_DisplaySize;
            }
            else {
                DLSS_Last_Mode = m_ui.DLSS_Mode;
                m_DLSS_Last_DisplaySize = m_DisplaySize;
            }
        }

        // in variable ratio mode, pick a random ratio between min and max rendering resolution
        int2 maxSize = m_RecommendedDLSSSettings.maxRenderSize;
        int2 minSize = m_RecommendedDLSSSettings.minRenderSize;
        float texLodXDimension;
        if (m_ui.DLSS_Resolution_Mode == RenderingResolutionMode::DYNAMIC)
        {
            // Even if we request dynamic res, it is possible that the DLSS mode has max==min
            if (any(maxSize != minSize))
            {
                if (m_ui.DLSS_Dynamic_Res_change)
                {
                    m_ui.DLSS_Dynamic_Res_change = false;
                    std::uniform_int_distribution<int> distributionWidth(minSize.x, maxSize.x);
                    int newWidth = distributionWidth(m_Generator);

                    // Height is initially based on width and aspect
                    int newHeight = (int)(newWidth * (float)m_DisplaySize.y / (float)m_DisplaySize.x);

                    // But that height might be too small or too large for the min/max settings of the DLSS
                    // mode (in theory); skip changing the res if it is out of range.
                    // We predict this never to happen. It is more of a safety measure.
                    if (newHeight >= minSize.y && newHeight <= maxSize.y) 
                        m_RenderingRectSize = { newWidth , newHeight };
                }
                else {
                    m_RenderingRectSize = m_RecommendedDLSSSettings.minRenderSize;
                }

                // For dynamic ratio, we want to choose the minimum rendering size
                // to select a Texture LOD that will preserve its sharpness over a large range of rendering resolution.
                // Ideally, the texture LOD would be allowed to be variable as well based on the dynamic scale
                // but we don't support that here yet.
                texLodXDimension = (float)minSize.x;

                // If the OUTPUT buffer resized or the DLSS mode changed, we need to recreate passes in dynamic mode.
                // In fixed resolution DLSS, this just happens when we change DLSS mode because it causes one of the
                // other cases below to hit (likely texLod).
                if (DLSS_resizeRequired) needNewPasses = true;
            }
            else
            {
                m_RenderingRectSize = maxSize;
                texLodXDimension = (float)m_RenderingRectSize.x;
            }
        }
        else if (m_ui.AAMode == AntiAliasingMode::DLSS)
        {
            m_RenderingRectSize = m_RecommendedDLSSSettings.optimalRenderSize;
            texLodXDimension = (float)m_RenderingRectSize.x;
        }

        // Use the formula of the DLSS programming guide for the Texture LOD Bias...
        lodBias = std::log2f(texLodXDimension/m_DisplaySize.x) - 1;
    }
    else {
        sl::DLSSOptions dlssConstants = {};
        dlssConstants.mode = sl::DLSSMode::eOff;
        NVWrapper::Get().SetDLSSOptions(dlssConstants);
        m_RenderingRectSize = m_DisplaySize;
    }

#ifdef STREAMLINE_FEATURE_DLSS_RR 
    // If we are using DLSS-RR set its constants; Default OFF
    if (m_ui.DLSSRR_Mode != sl::DLSSMode::eOff)
    {   

        m_RayReconstructionOptions.dlaaPreset = m_ui.DLSSRR_presets[static_cast<int>(sl::DLSSMode::eDLAA)];
        m_RayReconstructionOptions.qualityPreset = m_ui.DLSSRR_presets[static_cast<int>(sl::DLSSMode::eMaxQuality)];
        m_RayReconstructionOptions.balancedPreset = m_ui.DLSSRR_presets[static_cast<int>(sl::DLSSMode::eBalanced)];
        m_RayReconstructionOptions.performancePreset = m_ui.DLSSRR_presets[static_cast<int>(sl::DLSSMode::eMaxPerformance)];
        m_RayReconstructionOptions.ultraPerformancePreset = m_ui.DLSSRR_presets[static_cast<int>(sl::DLSSMode::eUltraPerformance)];

        m_RayReconstructionOptions.mode = m_ui.DLSSRR_Mode;
        m_RayReconstructionOptions.outputWidth = m_DisplaySize.x;
        m_RayReconstructionOptions.outputHeight = m_DisplaySize.y;
        m_RayReconstructionOptions.colorBuffersHDR = sl::Boolean::eTrue;
        m_RayReconstructionOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;

        NVWrapper::Get().GetDLSSRROptions(m_RayReconstructionOptions, m_RayReconstructionSettings);
        m_RenderingRectSize = {int(m_RayReconstructionSettings.optimalRenderWidth), int(m_RayReconstructionSettings.optimalRenderHeight)};
        
    }
#endif // STREAMLINE_FEATURE_DLSS_RR


    // PASS SETUP
    {
        bool needNewPasses = false;

        // Here, we intentionally leave the renderTargets oversized: (displaySize, displaySize) instead of (m_RenderingRectSize, displaySize), to show the power of sl::Extent
        bool useFullSizeRenderingBuffers = m_ui.DLSS_always_use_extents || (m_ui.DLSS_Resolution_Mode == RenderingResolutionMode::DYNAMIC);

        donut::math::int2 renderSize = useFullSizeRenderingBuffers ? m_DisplaySize : m_RenderingRectSize;
#ifdef STREAMLINE_FEATURE_DLSS_RR
        if(m_ui.DLSSRR_Mode != sl::DLSSMode::eOff) 
            renderSize = {int(m_RayReconstructionSettings.optimalRenderWidth), int(m_RayReconstructionSettings.optimalRenderHeight)};
#endif // STREAMLINE_FEATURE_DLSS_RR

        bool IsUpdateRequired = m_RenderTargets && m_RenderTargets->IsUpdateRequired(renderSize, m_DisplaySize);
        if (!m_RenderTargets || IsUpdateRequired)
        {
            m_BindingCache.Clear();
            m_RenderTargets = nullptr;
            m_RenderTargets = std::make_unique<RenderTargets>();
            m_RenderTargets->Init(GetDevice(), renderSize, m_DisplaySize, framebuffer->getDesc().colorAttachments[0].texture->getDesc().format);

#ifdef STREAMLINE_FEATURE_DLSS_RR
            if(GetDevice()->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
            {
                nvrhi::BindingSetDesc bindingSetDesc;
                bindingSetDesc.bindings = {
                    nvrhi::BindingSetItem::ConstantBuffer(0, m_ConstantBuffer),
                    nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_TopLevelAS),
                    nvrhi::BindingSetItem::Texture_SRV(1, m_RenderTargets->Depth),
                    //nvrhi::BindingSetItem::Texture_SRV(1, hackOptions.enableHack ?
                    //    m_RenderTargets->hackDepth : m_RenderTargets->Depth),
                    nvrhi::BindingSetItem::Texture_SRV(2, m_RenderTargets->GBufferDiffuse),
                    nvrhi::BindingSetItem::Texture_SRV(3, m_RenderTargets->GBufferSpecular),
                    nvrhi::BindingSetItem::Texture_SRV(4, m_RenderTargets->GBufferNormals),
                    nvrhi::BindingSetItem::Texture_SRV(5, m_RenderTargets->GBufferEmissive),
                    nvrhi::BindingSetItem::Texture_UAV(0, m_RenderTargets->HdrColor),
                    //nvrhi::BindingSetItem::Texture_UAV(0, hackOptions.enableHack ?
                    //    m_RenderTargets->hackHdrColor : m_RenderTargets->HdrColor),
                    nvrhi::BindingSetItem::Texture_UAV(1, m_RenderTargets->SpecHitDistance),
                    nvrhi::BindingSetItem::Sampler(0, m_CommonPasses->m_LinearWrapSampler)
                };

                m_BindingSet = GetDevice()->createBindingSet(bindingSetDesc, m_GlobalBindingLayout);
            }
#endif // STREAMLINE_FEATURE_DLSS_RR

            if (IsUpdateRequired && m_ui.NIS_Mode != sl::NISMode::eOff)
            {
                // input and output resources to NIS are part of the render targets.
                // Since render targets are destroyed and recreated above, we need to clean up the plugin to flash stale information.
                NVWrapper::Get().CleanupNIS(false);
            }

            needNewPasses = true;
        }

        // Render scene, change bias
        if (m_ui.DLSS_lodbias_useoveride) lodBias = m_ui.DLSS_lodbias_overide;
        if (m_PreviousLodBias != lodBias)
        {
            needNewPasses = true;
            m_PreviousLodBias = lodBias;
        }

        if (SetupView())
        {
            needNewPasses = true;
        }

        if (needNewPasses)
        {
            CreateRenderPasses(exposureResetRequired, lodBias);
        }

    }

    // BEGIN COMMAND LIST
    m_CommandList->open();

    // DO RESETS
    m_Scene->RefreshBuffers(m_CommandList, GetFrameIndex());

    m_RenderTargets->Clear(m_CommandList);

    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
    // only the very first viewport needs to clear the framebuffer
    if (m_viewport == 0)
    {
        m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
    }

    if (exposureResetRequired)
        m_ToneMappingPass->ResetExposure(m_CommandList, 8.f);

    m_AmbientTop = m_ui.AmbientIntensity * m_ui.SkyParams.skyColor * m_ui.SkyParams.brightness;
    m_AmbientBottom = m_ui.AmbientIntensity * m_ui.SkyParams.groundColor * m_ui.SkyParams.brightness;

    // SHADOW PASS; default ON
    if (m_ui.EnableShadows)
    {
        m_SunLight->shadowMap = m_ShadowMap;
        box3 sceneBounds = m_Scene->GetSceneGraph()->GetRootNode()->GetGlobalBoundingBox();

        frustum projectionFrustum = m_View->GetProjectionFrustum();
        projectionFrustum = projectionFrustum.grow(1.f); // to prevent volumetric light leaking
        const float maxShadowDistance = 100.f;

        dm::affine3 viewMatrixInv = m_View->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix();

        float zRange = length(sceneBounds.diagonal()) * 0.5f;
        m_ShadowMap->SetupForPlanarViewStable(*m_SunLight, projectionFrustum, viewMatrixInv, maxShadowDistance, zRange, zRange, m_ui.CsmExponent);

        m_ShadowMap->Clear(m_CommandList);

        DepthPass::Context context;

        RenderCompositeView(m_CommandList,
            &m_ShadowMap->GetView(), nullptr,
            *m_ShadowFramebuffer,
            m_Scene->GetSceneGraph()->GetRootNode(),
            *m_OpaqueDrawStrategy,
            *m_ShadowDepthPass,
            context,
            "ShadowMap");
    }
    else
    {
        m_SunLight->shadowMap = nullptr;
    }

    // Do CPU Load
    if (m_ui.CpuLoad != 0) {
        auto start = std::chrono::high_resolution_clock::now();
        while ((std::chrono::high_resolution_clock::now() - start).count() / 1e6 < m_ui.CpuLoad);
    }
    
    // GBuffer render
    m_RenderTargets->Clear(m_CommandList);
    donut::render::GBufferFillPass::Context gbufferContext;
    RenderCompositeView(m_CommandList,
                m_View.get(), m_ViewPrevious.get(),
                *m_RenderTargets->GBufferFramebuffer,
                m_Scene->GetSceneGraph()->GetRootNode(),
                *m_OpaqueDrawStrategy,
                *m_GBufferPass,
                gbufferContext,
                "GBufferFill");

    // Earliest time to copy per-frame data to hack RT. Must come after above GBuffer render which clears all RTs.
    if (hackOptions.enableHack) {
        uint32_t hackFrameId = GetFrameIndex() % FramesToReplayTotal;

        const TextureSubresourceData& layoutHDR = hackLoadedColorsHDR[hackFrameId]->dataLayout[0][0];
        m_CommandList->writeTexture(m_RenderTargets->hackHdrColor, 0, 0,
            static_cast<const void*>(hackLoadedColorsHDR[hackFrameId]->data->data()),
            layoutHDR.rowPitch, layoutHDR.depthPitch);

        const TextureSubresourceData& layoutMV = hackLoadedMVs[hackFrameId]->dataLayout[0][0];
        m_CommandList->writeTexture(m_RenderTargets->hackMotionVectors, 0, 0,
            static_cast<const void*>(hackLoadedMVs[hackFrameId]->data->data()),
            layoutMV.rowPitch, layoutMV.depthPitch);

        const TextureSubresourceData& layoutDepth = hackLoadedDepths[hackFrameId]->dataLayout[0][0];
        m_CommandList->writeTexture(m_RenderTargets->hackDepth, 0, 0,
            static_cast<const void*>(hackLoadedDepths[hackFrameId]->data->data()),
            layoutDepth.rowPitch, layoutDepth.depthPitch);

        // MV and depth need to restore resources state after copy; probably because they are not virtual textures
        auto& descHackMV = m_RenderTargets->hackMotionVectors->getDesc();
        auto& descHackDepth = m_RenderTargets->Depth->getDesc();
        m_CommandList->setTextureState(m_RenderTargets->hackMotionVectors, nvrhi::AllSubresources, descHackMV.initialState);
        m_CommandList->setTextureState(m_RenderTargets->hackDepth, nvrhi::AllSubresources, descHackDepth.initialState);
        m_CommandList->commitBarriers();

    }

#ifdef STREAMLINE_FEATURE_DLSS_RR
    // Default On; NOTE that mvec will NOT be rendered when raytracing
    if(m_ui.RayTracing_Mode && GetDevice()->getGraphicsAPI() != nvrhi::GraphicsAPI::D3D11)
    {   
        // Set lighting constants
        LightingConstants constants = {};
        constants.ambientColor = dm::float4(0.037f);
        m_View->FillPlanarViewConstants(constants.view);
        m_SunLight->FillLightConstants(constants.light);
        constants.frameIndex = GetFrameIndex();
        m_CommandList->writeBuffer(m_ConstantBuffer, &constants, sizeof(constants));

        // Setup ray tracing state
        nvrhi::rt::State state;
        state.shaderTable = m_ShaderTable;
        state.bindings = { m_BindingSet };
        m_CommandList->setRayTracingState(state);

        CreateAccelStruct(m_CommandList);

        // Dispatch rays
        nvrhi::rt::DispatchRaysArguments args;
        args.width = backbufferWidth;
        args.height = backbufferHeight;
        m_CommandList->dispatchRays(args);

        if (m_ui.EnableProceduralSky)
        m_SkyPass->Render(m_CommandList, *m_View, *m_SunLight, m_ui.SkyParams);

        // DO BLOOM; default ON
        if (m_ui.EnableBloom) 
            m_BloomPass->Render(m_CommandList, m_RenderTargets->HdrFramebuffer, *m_View, m_RenderTargets->HdrColor, m_ui.BloomSigma, m_ui.BloomAlpha);

        // Render sky (use default params)
        donut::render::SkyParameters skyParams{};
        skyParams.maxLightRadiance = 0.75f;
        m_SkyPass->Render(m_CommandList, *m_View, *m_SunLight, skyParams);
    }

    else
#endif // STREAMLINE_FEATURE_DLSS_RR
    {
        // Deferred Shading

        // DO MOTION VECTORS
        if (m_PreviousViewsValid) 
            m_TemporalAntiAliasingPass->RenderMotionVectors(m_CommandList, *m_View, *m_ViewPrevious);

        // DO SSAO
        nvrhi::ITexture* ambientOcclusionTarget = nullptr;
        if (m_ui.EnableSsao && m_SsaoPass)
        {
            m_SsaoPass->Render(m_CommandList, m_ui.SsaoParams, *m_View);
            ambientOcclusionTarget = m_RenderTargets->AmbientOcclusion;
        }

        // DO DEFERRED
        DeferredLightingPass::Inputs deferredInputs;
        deferredInputs.SetGBuffer(*m_RenderTargets);
        deferredInputs.ambientOcclusion = m_ui.EnableSsao ? m_RenderTargets->AmbientOcclusion : nullptr;
        deferredInputs.ambientColorTop = m_AmbientTop;
        deferredInputs.ambientColorBottom = m_AmbientBottom;
        deferredInputs.lights = &m_Scene->GetSceneGraph()->GetLights();
        deferredInputs.output = m_RenderTargets->HdrColor;

        m_DeferredLightingPass->Render(m_CommandList, *m_View, deferredInputs);
    }
    
    if (m_ui.EnableProceduralSky)
        m_SkyPass->Render(m_CommandList, *m_View, *m_SunLight, m_ui.SkyParams);

    // DO BLOOM; default ON
    if (m_ui.EnableBloom) 
        m_BloomPass->Render(m_CommandList, m_RenderTargets->HdrFramebuffer, *m_View, m_RenderTargets->HdrColor, m_ui.BloomSigma, m_ui.BloomAlpha);

    // SET STREAMLINE CONSTANTS
    {
        // This section of code updates the streamline constants every frame. Regardless of whether we are utilising the streamline plugins, as long as streamline is in use, we must set its constants.

        constexpr float zNear = 0.1f;
        constexpr float zFar = 200.f;

        affine3 viewReprojection = m_View->GetChildView(ViewType::PLANAR, 0)->GetInverseViewMatrix() * m_ViewPrevious->GetViewMatrix();
        float4x4 reprojectionMatrix = inverse(m_View->GetProjectionMatrix(false)) * affineToHomogeneous(viewReprojection) * m_ViewPrevious->GetProjectionMatrix(false);
        float aspectRatio = float(m_RenderingRectSize.x) / float(m_RenderingRectSize.y);
        float4x4 projection = perspProjD3DStyleReverse(dm::radians(m_CameraVerticalFov), aspectRatio, zNear);

        float2 jitterOffset = hackOptions.enableHack ?
            hackLoadedJitterOffsets[GetFrameIndex() % FramesToReplayTotal] :
            std::dynamic_pointer_cast<PlanarView, IView>(m_View)->GetPixelOffset();

        sl::Constants slConstants = {};
        slConstants.cameraAspectRatio = aspectRatio;
        slConstants.cameraFOV = dm::radians(m_CameraVerticalFov);
        slConstants.cameraFar = zFar;
        slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
        slConstants.cameraNear = zNear;
        slConstants.cameraPinholeOffset = { 0.f, 0.f };
        slConstants.cameraPos = make_sl_float3(m_FirstPersonCamera.GetPosition());
        slConstants.cameraFwd = make_sl_float3(m_FirstPersonCamera.GetDir());
        slConstants.cameraUp = make_sl_float3(m_FirstPersonCamera.GetUp());
        slConstants.cameraRight = make_sl_float3(normalize(cross(m_FirstPersonCamera.GetDir(), m_FirstPersonCamera.GetUp())));
        slConstants.cameraViewToClip = make_sl_float4x4(projection);
        slConstants.clipToCameraView = make_sl_float4x4(inverse(projection));
        slConstants.clipToPrevClip = make_sl_float4x4(reprojectionMatrix);
        slConstants.depthInverted = m_View->IsReverseDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        slConstants.jitterOffset = make_sl_float2(jitterOffset);
        slConstants.mvecScale = { 1.0f / m_RenderingRectSize.x , 1.0f / m_RenderingRectSize.y }; // This are scale factors used to normalize mvec (to -1,1) and donut has mvec in pixel space
        slConstants.prevClipToClip = make_sl_float4x4(inverse(reprojectionMatrix));
        slConstants.reset = needNewPasses ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        slConstants.motionVectors3D = sl::Boolean::eFalse;
        slConstants.motionVectorsInvalidValue = FLT_MIN;

        // will cause error if SetSLConsts() on those duplicate frame 0
        //if (GetFrameIndex() > 0 ||
        //    GetDeviceManager()->FramesToWarmup == FramesToWarmup)
        if (GetFrameIndex() > 0)
        {
        }
        NVWrapper::Get().SetSLConsts(slConstants);
    }

	// TAG STREAMLINE RESOURCES
	if (hackOptions.enableHack) {
		NVWrapper::Get().TagResources_General(m_CommandList,
			m_View->GetChildView(ViewType::PLANAR, 0),
			m_RenderTargets->hackMotionVectors,
			m_RenderTargets->hackDepth,
			// PreUIColor is only used as DLSS output target, thus we don't create hack version of it. 
			m_RenderTargets->PreUIColor
		);
	}
	else {
		NVWrapper::Get().TagResources_General(m_CommandList,
			m_View->GetChildView(ViewType::PLANAR, 0),
			m_RenderTargets->MotionVectors,
			m_RenderTargets->Depth,
			m_RenderTargets->PreUIColor
        );
	}

#ifdef STREAMLINE_FEATURE_DLSS_RR
    // Set feature options; DLSSRR_Mode default OFF
    if (m_ui.DLSSRR_Mode != sl::DLSSMode::eOff)
    {   
        dm::float4x4 worldToView = affineToHomogeneous(m_FirstPersonCamera.GetWorldToViewMatrix());
        m_RayReconstructionOptions.worldToCameraView = make_sl_float4x4(worldToView);
        m_RayReconstructionOptions.cameraViewToWorld = make_sl_float4x4(inverse(worldToView));
        NVWrapper::Get().SetDLSSRROptions(m_RayReconstructionOptions);
        m_ui.DLSS_Mode = sl::DLSSMode::eOff;
        
    }
#endif // STREAMLINE_FEATURE_DLSS_RR

    
    // ANTI-ALIASING

    // TAG NIS (Nvidia Image Scaling) RESOURCES, i.e. pre-upscaled and post-upscaled buffers.
    if (hackOptions.enableHack) {
        NVWrapper::Get().TagResources_DLSS_NIS(m_CommandList,
            m_View->GetChildView(ViewType::PLANAR, 0),
            m_RenderTargets->AAResolvedColor,
            m_RenderTargets->hackHdrColor);

    }
    else {
        NVWrapper::Get().TagResources_DLSS_NIS(m_CommandList,
            m_View->GetChildView(ViewType::PLANAR, 0),
            m_RenderTargets->AAResolvedColor,
            m_RenderTargets->HdrColor);
    }

    if (m_ui.AAMode != AntiAliasingMode::NONE) {

        // DO DLSS

        if (m_ui.AAMode == AntiAliasingMode::DLSS && !m_ui.DLSS_DebugShowFullRenderingBuffer)
        {
            NVWrapper::Get().EvaluateDLSS(m_CommandList);
        }

        if (m_ui.AAMode == AntiAliasingMode::DLSS && m_ui.DLSS_DebugShowFullRenderingBuffer) {
            m_CommonPasses->BlitTexture(m_CommandList, m_RenderTargets->AAResolvedFramebuffer->GetFramebuffer(*m_View), m_RenderTargets->HdrColor, &m_BindingCache);
            m_PreviousViewsValid = false;
        }

        // DO TAA
        if (m_ui.AAMode == AntiAliasingMode::TEMPORAL)
        {
            m_TemporalAntiAliasingPass->TemporalResolve(m_CommandList, m_ui.TemporalAntiAliasingParams, m_PreviousViewsValid, *m_View, m_PreviousViewsValid ? *m_ViewPrevious : *m_View);
        }

        m_PreviousViewsValid = true;
    }
    else
    {
        // IF YOU DO NOTHING SPECIAL -> FORWARD TEXTURE from HdrColor to AAResolvedFramebuffer
        m_CommonPasses->BlitTexture(m_CommandList, m_RenderTargets->AAResolvedFramebuffer->GetFramebuffer(*m_View), m_RenderTargets->HdrColor, &m_BindingCache);
        m_PreviousViewsValid = false;
    }

#ifdef STREAMLINE_FEATURE_DLSS_RR
    if (m_ui.DLSSRR_Mode != sl::DLSSMode::eOff)
    {   
		NVWrapper::Get().TagResources_DLSS_RR(
			m_CommandList,
			m_View->GetChildView(ViewType::PLANAR, 0),
			m_RenderTargets->HdrColor,
			m_RenderTargets->GBufferDiffuseRR,
			m_RenderTargets->GBufferSpecularRR,
			m_RenderTargets->GBufferNormalsRR,
			m_RenderTargets->SpecHitDistance,
			m_RenderTargets->AAResolvedColor);

        NVWrapper::Get().EvaluateDLSSRR(m_CommandList);
    }
#endif // STREAMLINE_FEATURE_DLSS_RR

    //DO TONEMAPPING; changed to OFF
    nvrhi::ITexture* texToDisplay;
    if (m_ui.EnableToneMapping)
    {
        auto toneMappingParams = m_ui.ToneMappingParams;
        if (exposureResetRequired)
        {
            toneMappingParams.minAdaptedLuminance = 0.1f;
            toneMappingParams.eyeAdaptationSpeedDown = 0.0f;
        }
        m_ToneMappingPass->SimpleRender(m_CommandList, toneMappingParams, *m_TonemappingView, m_RenderTargets->AAResolvedColor);

        m_CommandList->copyTexture(m_RenderTargets->ColorspaceCorrectionColor, nvrhi::TextureSlice(), m_RenderTargets->LdrColor, nvrhi::TextureSlice());
        texToDisplay = m_RenderTargets->ColorspaceCorrectionColor;
    }
    else {
        texToDisplay = m_RenderTargets->AAResolvedColor;
    }


    m_CommonPasses->BlitTexture(m_CommandList, m_RenderTargets->PreUIFramebuffer->GetFramebuffer(*m_View), texToDisplay, &m_BindingCache);

    //
    // DO NIS; default OFF
    //
    if (m_ui.NIS_Mode != sl::NISMode::eOff) {

        // NIS SETUP
        auto nisConsts = sl::NISOptions{};
        nisConsts.mode = m_ui.NIS_Mode;
        nisConsts.sharpness = m_ui.NIS_Sharpness;
        NVWrapper::Get().SetNISOptions(nisConsts);

        // Use PreUI Color and restore resources state after copy
        m_CommandList->copyTexture(m_RenderTargets->NisColor, nvrhi::TextureSlice(), m_RenderTargets->PreUIColor, nvrhi::TextureSlice());
        auto NISDesc = m_RenderTargets->NisColor->getDesc();
        auto PreUIColorDesc = m_RenderTargets->PreUIColor->getDesc();
        m_CommandList->setTextureState(m_RenderTargets->NisColor, nvrhi::AllSubresources, NISDesc.initialState);
        m_CommandList->setTextureState(m_RenderTargets->PreUIColor, nvrhi::AllSubresources, PreUIColorDesc.initialState);
        m_CommandList->commitBarriers();

        // TAG STREAMLINE RESOURCES
        NVWrapper::Get().TagResources_DLSS_NIS(m_CommandList,
            m_View->GetChildView(ViewType::PLANAR, 0),
            m_RenderTargets->PreUIColor,
            m_RenderTargets->NisColor);

        NVWrapper::Get().EvaluateNIS(m_CommandList);
    }

    // validViewportExtent is false
    NVWrapper::Get().TagResources_DLSS_FG(m_CommandList, validViewportExtent, m_backbufferViewportExtent);

    //
    // DO DEEPDVC; default OFF
    //
    if (m_ui.DeepDVC_Mode != sl::DeepDVCMode::eOff) {
        // DeepDVC SETUP
        auto deepdvcConsts = sl::DeepDVCOptions{};
        deepdvcConsts.mode = m_ui.DeepDVC_Mode;
        deepdvcConsts.intensity = m_ui.DeepDVC_Intensity;
        deepdvcConsts.saturationBoost = m_ui.DeepDVC_SaturationBoost;
        NVWrapper::Get().SetDeepDVCOptions(deepdvcConsts);

        // TAG STREAMLINE RESOURCES
        NVWrapper::Get().TagResources_DeepDVC(m_CommandList,
            m_View->GetChildView(ViewType::PLANAR, 0),
            m_RenderTargets->PreUIColor);
        NVWrapper::Get().EvaluateDeepDVC(m_CommandList);
    }

#if STREAMLINE_FEATURE_LATEWARP
    // default OFF even with LateWarp feature
    if (m_ui.Latewarp_active)
    {
        NVWrapper::Get().TagResources_Latewarp(m_CommandList,
            m_View->GetChildView(ViewType::PLANAR, 0),
            framebufferTexture,
            nullptr,
            nullptr,
            m_backbufferViewportExtent
        );
        NVWrapper::Get().EvaluateLatewarp(*GetDeviceManager(), m_CommandList, m_RenderTargets.get(), m_ui.EnableToneMapping ? m_RenderTargets->ColorspaceCorrectionColor : m_RenderTargets->AAResolvedColor, m_RenderTargets->PreUIColor, m_View->GetChildView(ViewType::PLANAR, 0));
    }
#endif

    // false
    if (validViewportExtent)
    {
        // blit to target framebuffer viewport
        nvrhi::Viewport backBufferViewport
        {
            static_cast<float>(m_backbufferViewportExtent.left),
            static_cast<float>(m_backbufferViewportExtent.left + m_backbufferViewportExtent.width - 1),
            static_cast<float>(m_backbufferViewportExtent.top),
            static_cast<float>(m_backbufferViewportExtent.top + m_backbufferViewportExtent.height - 1),
            0.f,
            1.f
        };
        engine::BlitParameters blitParams{};
        blitParams.targetFramebuffer = framebuffer;
        blitParams.targetViewport = backBufferViewport;
        blitParams.sourceTexture = m_RenderTargets->PreUIColor;

        m_CommonPasses->BlitTexture(m_CommandList, blitParams, &m_BindingCache);
    }
    else
    {
        m_CommandList->copyTexture(framebufferTexture, nvrhi::TextureSlice(), m_RenderTargets->PreUIColor, nvrhi::TextureSlice());
    }

    // DEBUG OVERLAY
    GetDeviceManager()->GetWindowDimensions(windowWidth, windowHeight);
    if (m_ui.VisualiseBuffers) {

        static constexpr int SubWindowNumber = 2;
        static constexpr float SubWindowSpacing = 5.f;

        // If we want to, we can overlay the other textures onto the screen for comparative inspection
        auto displayDebugPiP = [&](nvrhi::TextureHandle texture, int2 pos, float scale) {
            // This snippet is by Manuel Kraemer

            dm::float2 size = dm::float2(float(windowWidth), float(windowHeight - 2.f * SubWindowSpacing)) * scale;

            nvrhi::Viewport viewport = nvrhi::Viewport(
                SubWindowSpacing * (pos.x + 1) + size.x * pos.x,
                SubWindowSpacing * (pos.x + 1) + size.x * (pos.x + 1),
                windowViewport.maxY - SubWindowSpacing * (pos.y + 1) - size.y * (pos.y + 1),
                windowViewport.maxY - SubWindowSpacing * (pos.y + 1) - size.y * pos.y, 0.f, 1.f
            );

            engine::BlitParameters blitParams;
            blitParams.targetFramebuffer = framebuffer;
            blitParams.targetViewport = viewport;
            blitParams.sourceTexture = texture;
            m_CommonPasses->BlitTexture(m_CommandList, blitParams, &m_BindingCache);
        };

        int counter = 0;
        if (hackOptions.enableHack) {
            displayDebugPiP(m_RenderTargets->hackMotionVectors, int2(counter% SubWindowNumber, counter++ / SubWindowNumber), 1 / float(SubWindowNumber));
            displayDebugPiP(m_RenderTargets->hackDepth, int2(counter% SubWindowNumber, counter++ / SubWindowNumber), 1 / float(SubWindowNumber));
        }
        else {
            displayDebugPiP(m_RenderTargets->MotionVectors, int2(counter% SubWindowNumber, counter++ / SubWindowNumber), 1 / float(SubWindowNumber));
            displayDebugPiP(m_RenderTargets->Depth, int2(counter% SubWindowNumber, counter++ / SubWindowNumber), 1 / float(SubWindowNumber));
        }
    }

    // CLOSE COMMANDLIST
    m_CommandList->close();
    GetDevice()->executeCommandList(m_CommandList);

    if (!hackExportFilenameSR.empty()) {
        MapRenderTargetDataHDR(m_RenderTargets->AAResolvedColor, hackExportFilenameSR);
        hackExportSlotSR++;
    }

    // CLEANUP
    {

        m_TemporalAntiAliasingPass->AdvanceFrame();

        std::swap(m_View, m_ViewPrevious);

        m_CameraPreviousMatrix = m_FirstPersonCamera.GetWorldToViewMatrix();

        GetDeviceManager()->SetVsyncEnabled(m_ui.EnableVsync);
    }

    // CLOSE: early close when we store hack output; 
    // run several more frame to avoid strange frame sync error under fullscreen mode, which causes the system to freeze.
    if (hackOptions.storeOutput && GetFrameIndex() == FramesToReplayTotal + 5)
    {
        if (!hash_bin.empty()) {
            log::info("Saving %d captured frames in the end.", hash_bin.size());
            for (const auto& [hashKey, frameData] : hash_bin) {
                // DEBUG: skip FG export
                //if (frameData.filename.find("_fg") != std::string::npos) {
                //    continue;
                //}
                bool success = SaveStagingTextureDataToEXR(
                    frameData.data,
                    frameData.rowPitch,
					frameData.width, frameData.height,
                    frameData.filename
				);
            }
        }
        glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
    }

    if (GetFrameIndex() == m_ScriptingConfig.maxFrames)
        glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(), GLFW_TRUE);
}

// Logistic functions 

std::shared_ptr<TextureCache> StreamlineSample::GetTextureCache()
{
    return m_TextureCache;
}

std::vector<std::string> const& StreamlineSample::GetAvailableScenes() const
{
    return m_SceneFilesAvailable;
}

std::string StreamlineSample::GetCurrentSceneName() const
{
    return m_CurrentSceneName;
}

void StreamlineSample::SetCurrentSceneName(const std::string& sceneName)
{
    if (m_CurrentSceneName == sceneName)
        return;

    m_CurrentSceneName = sceneName;

    BeginLoadingScene(m_RootFs, m_CurrentSceneName);
}

bool StreamlineSample::KeyboardUpdate(int key, int scancode, int action, int mods)
{

    if (key == GLFW_KEY_F13 && action == GLFW_PRESS) {
        // As GLFW abstracts away from Windows messages
        // We instead set the F13 as the PC_Ping key in the constants and compare against that.
        NVWrapper::Get().ReflexTriggerPcPing();
    }
    
    if (key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
        m_ui.EnableAnimations = !m_ui.EnableAnimations;
    }

    m_FirstPersonCamera.KeyboardUpdate(key, scancode, action, mods);
    return true;
}

bool StreamlineSample::MousePosUpdate(double xpos, double ypos)
{
    m_FirstPersonCamera.MousePosUpdate(xpos, ypos);
    return true;
}

bool StreamlineSample::MouseButtonUpdate(int button, int action, int mods)
{

    if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS) {
        NVWrapper::Get().ReflexTriggerFlash();
    }

    m_FirstPersonCamera.MouseButtonUpdate(button, action, mods);
    return true;
}

bool StreamlineSample::MouseScrollUpdate(double xoffset, double yoffset)
{
    m_FirstPersonCamera.MouseScrollUpdate(xoffset, yoffset);
    return true;
}

void StreamlineSample::Animate(float fElapsedTimeSeconds)
{
    m_FirstPersonCamera.Animate(fElapsedTimeSeconds);

    if (m_ToneMappingPass)
        m_ToneMappingPass->AdvanceFrame(fElapsedTimeSeconds);

    if (IsSceneLoaded() && m_ui.EnableAnimations)
    {
        m_WallclockTime += fElapsedTimeSeconds*m_ui.AnimationSpeed;

        for (const auto& anim : m_Scene->GetSceneGraph()->GetAnimations())
        {
            float duration = anim->GetDuration();
            float integral;
            float animationTime = std::modf(m_WallclockTime / duration, &integral) * duration;
            (void)anim->Apply(animationTime);
        }
    }
}

void StreamlineSample::SceneUnloading()
{
    if (m_DeferredLightingPass) m_DeferredLightingPass->ResetBindingCache();
    if (m_GBufferPass) m_GBufferPass->ResetBindingCache();
    if (m_ShadowDepthPass) m_ShadowDepthPass->ResetBindingCache();
    m_BindingCache.Clear();
    m_SunLight.reset();
}

bool StreamlineSample::LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName)
{
    using namespace std::chrono;

    Scene* scene = new Scene(GetDevice(), *m_ShaderFactory, fs, m_TextureCache, nullptr, nullptr);

    auto startTime = high_resolution_clock::now();

    if (scene->Load(fileName))
    {
        m_Scene = std::unique_ptr<Scene>(scene);

        auto endTime = high_resolution_clock::now();
        auto duration = duration_cast<milliseconds>(endTime - startTime).count();
        log::info("Scene loading time: %llu ms", duration);

        return true;
    }

    return false;
}

void StreamlineSample::SceneLoaded()
{
    Super::SceneLoaded();

    m_Scene->FinishedLoading(GetFrameIndex());

    m_WallclockTime = 0.f;
    m_PreviousViewsValid = false;

    for (auto light : m_Scene->GetSceneGraph()->GetLights())
    {
        if (light->GetLightType() == LightType_Directional)
        {
            m_SunLight = std::static_pointer_cast<DirectionalLight>(light);
            break;
        }
    }

    if (!m_SunLight)
    {
        m_SunLight = std::make_shared<DirectionalLight>();
        m_SunLight->angularSize = 0.53f;
        m_SunLight->irradiance = 1.f;

        auto node = std::make_shared<SceneGraphNode>();
        node->SetLeaf(m_SunLight);
        m_SunLight->SetDirection(dm::double3(0.1, -0.9, 0.1));
        m_SunLight->SetName("Sun");
        m_Scene->GetSceneGraph()->Attach(m_Scene->GetSceneGraph()->GetRootNode(), node);
    }

    m_FirstPersonCamera.LookAt(float3(0.f, 1.8f, 0.f), float3(1.f, 1.8f, 0.f));
    m_CameraVerticalFov = 60.f;

}

void StreamlineSample::RenderSplashScreen(nvrhi::IFramebuffer* framebuffer)
{
    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;
    m_CommandList->open();
    m_CommandList->clearTextureFloat(framebufferTexture, nvrhi::AllSubresources, nvrhi::Color(0.f));
    m_CommandList->close();
    GetDevice()->executeCommandList(m_CommandList);
    GetDeviceManager()->SetVsyncEnabled(true);
}

