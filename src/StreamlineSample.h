////----------------------------------------------------------------------------------
//// File:        StreamlineSample.h
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

#pragma once

#include "NVWrapper.h"
#include "RenderTargets.h"
#include "UIData.h"
#include <random>
#include <chrono>
#include <unordered_map>

// From Donut
#include <donut/core/vfs/VFS.h>
#include <donut/core/log.h>
#include <donut/engine/CommonRenderPasses.h>
#include <donut/engine/FramebufferFactory.h>
#include <donut/engine/Scene.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/render/BloomPass.h>
#include <donut/render/CascadedShadowMap.h>
#include <donut/render/DeferredLightingPass.h>
#include <donut/render/DepthPass.h>
#include <donut/render/DrawStrategy.h>
#include <donut/render/ForwardShadingPass.h>
#include <donut/render/GBufferFillPass.h>
#include <donut/render/LightProbeProcessingPass.h>
#include <donut/render/PixelReadbackPass.h>
#include <donut/render/SkyPass.h>
#include <donut/render/SsaoPass.h>
#include <donut/render/TemporalAntiAliasingPass.h>
#include <donut/render/ToneMappingPasses.h>
#include <donut/app/ApplicationBase.h>
#include <donut/app/Camera.h>
#include <donut/app/DeviceManager.h>
#include <nvrhi/utils.h>

#include <winrt/windows.media.capture.h>
#include <winrt/Windows.Graphics.Capture.h>

using namespace donut::math;
using namespace donut::app;
using namespace donut::vfs;
using namespace donut::engine;
using namespace donut::render;

struct ScriptingConfig {

    // Control at start behavior
    int maxFrames = -1;
    int DLSS_mode = -1;
    int DLSSRR_mode = -1;
    int Reflex_mode = -1;
    int Reflex_fpsCap = -1;
    int DLSSG_on = -1;
    int DLSSG_numFrameToGenerate = -1;
    int DeepDVC_on = -1;
    int Latewarp_on = -1;
    int GpuLoad = -1;
    sl::Extent viewportExtent{};

    ScriptingConfig(int argc, const char* const* argv)
    {

        for (int i = 1; i < argc; i++)
        {
            //MaxFrames
            if (!strcmp(argv[i], "-maxFrames"))
            {
                maxFrames = std::stoi(argv[++i]);
            }

            // DLSS
            else if (!strcmp(argv[i], "-DLSS_mode"))
            {
                DLSS_mode = std::stoi(argv[++i]);
            }

            // DLSSRR
            else if (!strcmp(argv[i], "-DLSSRR_mode"))
            {
                DLSSRR_mode = std::stoi(argv[++i]);
            }

            // Reflex
            else if (!strcmp(argv[i], "-Reflex_mode"))
            {
                Reflex_mode = std::stoi(argv[++i]);
            }
            else if (!strcmp(argv[i], "-Reflex_fpsCap"))
            {
                Reflex_fpsCap = std::stoi(argv[++i]);
            }

            // DLSSG
            else if (!strcmp(argv[i], "-DLSSG_on"))
            {
                DLSSG_on = 1;
            }
            else if (!strcmp(argv[i], "-DLSSG_numFrameToGenerate"))
            {
                int ret = sscanf(argv[++i], "%d", &DLSSG_numFrameToGenerate);
                assert(ret == 1);
            }
            // DeepDVC
            else if (!strcmp(argv[i], "-DeepDVC_on"))
            {
                DeepDVC_on = 1;
            }

            // Latewarp
            else if (!strcmp(argv[i], "-Latewarp_on"))
            {
                Latewarp_on = 1;
            }

            else if (!strcmp(argv[i], "-viewport"))
            {
                int ret = sscanf(argv[++i], "(%d,%d,%dx%d)", &viewportExtent.left, &viewportExtent.top, &viewportExtent.width, &viewportExtent.height);
                assert(ret == 4);
            }
            else if (!strcmp(argv[i], "-GpuLoad"))
            {
                int ret = sscanf(argv[++i], "%d", &GpuLoad);
                assert(ret == 1);
            }
        }
    }
};

class StreamlineSample : public ApplicationBase
{
private:
    typedef ApplicationBase Super;

    // Main Command queue and binding cache
    nvrhi::CommandListHandle                        m_CommandList;
    BindingCache                                    m_BindingCache;

    // Filesystem and scene
    std::shared_ptr<RootFileSystem>                 m_RootFs;
    std::vector<std::string>                        m_SceneFilesAvailable;
    std::string                                     m_CurrentSceneName;
    std::shared_ptr<Scene>				            m_Scene;
    float                                           m_WallclockTime = 0.f;
                                                    
    // Render Passes                                
    std::shared_ptr<ShaderFactory>                  m_ShaderFactory;
    std::shared_ptr<DirectionalLight>               m_SunLight;
    std::shared_ptr<CascadedShadowMap>              m_ShadowMap;
    std::shared_ptr<FramebufferFactory>             m_ShadowFramebuffer;
    std::shared_ptr<DepthPass>                      m_ShadowDepthPass;
    std::shared_ptr<InstancedOpaqueDrawStrategy>    m_OpaqueDrawStrategy;
    std::unique_ptr<GBufferFillPass>                m_GBufferPass;
    std::unique_ptr<DeferredLightingPass>           m_DeferredLightingPass;
    std::unique_ptr<SkyPass>                        m_SkyPass;
    std::unique_ptr<TemporalAntiAliasingPass>       m_TemporalAntiAliasingPass;
    std::unique_ptr<BloomPass>                      m_BloomPass;
    std::unique_ptr<ToneMappingPass>                m_ToneMappingPass;
    std::unique_ptr<SsaoPass>                       m_SsaoPass;
    std::shared_ptr<TransparentDrawStrategy>        m_TransparentDrawStrategy;

    // RenderTargets
    std::unique_ptr<RenderTargets>                  m_RenderTargets;

    //Views
    std::shared_ptr<IView>                          m_View;
    bool                                            m_PreviousViewsValid = false;
    std::shared_ptr<IView>                          m_ViewPrevious;
    std::shared_ptr<IView>                          m_TonemappingView;

    // Camera
    FirstPersonCamera                               m_FirstPersonCamera;
    float                                           m_CameraVerticalFov = 60.f;

    // Ray Tracing
    static constexpr size_t                         JITTER_SEQUENCE_LENGTH = 16;

    nvrhi::ShaderLibraryHandle                      m_ShaderLibrary;
    nvrhi::rt::PipelineHandle                       m_Pipeline;
    nvrhi::rt::ShaderTableHandle                    m_ShaderTable;
    nvrhi::BindingLayoutHandle                      m_GlobalBindingLayout;
    nvrhi::BindingLayoutHandle                      m_LocalBindingLayout;
    nvrhi::BindingSetHandle                         m_BindingSet;

    nvrhi::rt::AccelStructHandle                    m_BottomLevelAS;
    nvrhi::rt::AccelStructHandle                    m_TopLevelAS;

    nvrhi::BufferHandle                             m_ConstantBuffer;
    bool                                            CreateRayTracingPipeline(donut::engine::ShaderFactory& shaderFactory);
    void                                            CreateAccelStruct(nvrhi::ICommandList* commandList);

    // UI
    UIData& m_ui;
    donut::math::int2                               m_DLSS_Last_DisplaySize = { 0,0 };
    float3                                          m_AmbientTop = 0.f;
    float3                                          m_AmbientBottom = 0.f;

    // For Streamline
    int2                                            m_RenderingRectSize = { 0, 0 };
    int2                                            m_DisplaySize;
    NVWrapper::DLSSSettings                         m_RecommendedDLSSSettings;
#ifdef STREAMLINE_FEATURE_DLSS_RR
    sl::DLSSDOptions                                m_RayReconstructionOptions;
    sl::DLSSDOptimalSettings                        m_RayReconstructionSettings;
#endif //STREAMLINE_FEATURE_DLSS_RR
    std::default_random_engine                      m_Generator;
    float                                           m_PreviousLodBias;
    affine3                                         m_CameraPreviousMatrix;

    bool                                            m_presentStarted = false;

    sl::ViewportHandle                              m_viewport{};
    sl::Extent                                      m_backbufferViewportExtent{};

    // Scripting Behavior
    ScriptingConfig                                 m_ScriptingConfig;

    sl::DLSSMode                                    DLSS_Last_Mode = sl::DLSSMode::eOff;
    sl::DLSSMode                                    DLSSRR_Last_Mode = sl::DLSSMode::eOff;
    donut::math::int2                               m_DLSSRR_Last_DisplaySize = { 0,0 };

    // see Attempt 6
#pragma region MediaCapture 
    winrt::Windows::Media::Capture::MediaCapture m_mediaCapture{ nullptr };
    winrt::Windows::Media::Capture::AdvancedPhotoCapture m_advancedCapture{ nullptr };
    bool m_hdrSupported = false;

    winrt::Windows::Foundation::IAsyncAction CleanupMediaCaptureAsync();
#pragma endregion

    // see Attempt 8
#pragma region FramePoolCapture
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_captureItem{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_captureDevice{ nullptr };
    bool m_captureInitialized = false;

    bool CreateCaptureDevice();
    bool CreateCaptureItemForWindow();
    /**
     * @brief Calls CreateCaptureDevice() and CreateCaptureItemForWindow()
     * @return success or not
     */
    bool InitializeFramePoolCapture();
    void CleanupFramePoolCapture();
    
    /**
     * @brief Helper of Attempt 8 CaptureFramePoolHDR(). 
     * Should be called in a while(true) loop, i.e. repeat until unique. It does 2 things:
     * 
     * 1) map the given ID3D11Texture2D to a staging texture(D3D11_MAPPED_SUBRESOURCE).
     * 2) IF frame is unique (by checking image hash), call SaveStagingTextureDataToEXR()
     * in TextureCache.h to save exr file using tinyexr.
     * @return True if texture is a unique new one and saved to exr file successfully. False if duplicate.
     */
    bool SaveIfUniqueTexture(
        winrt::com_ptr<ID3D11Device> device,
        winrt::com_ptr<ID3D11Texture2D> texture,
        const std::string& filename);
#pragma endregion

public:

#pragma region Hack

    struct RatioTriplet {
        float low;
        float high;
        float optimal;
    };

    /**
     * Alias of display resolution in "K"
     * Used as a handy cmdline-arg alternative of 3 typical choices
     * 1K, 2K, and 4K
     *
     * Users can type "-Resolution 2" other than "-Resolution 2560 1440“
     */
    static const std::unordered_map<int, donut::math::uint2> ResolutionAliases;

    static const std::unordered_map<sl::DLSSMode, RatioTriplet> UpscaleRatioMap;

    // general runtime options
    struct HackOptionDef
    {
        bool enableHack = false;
        /// If true, a jitter pair will be parsed from each input filename.
        /// e.g. NPP_beauty_2472_0000_0_-0.40563965_-0.35599041.exr gives (-0.40563965, -0.35599041)
        bool parseJitter = false;
        bool storeOutput = false;
        /// This helps filesystem and image viewers to sort them in order. @see PostProcess() below.
        bool alignFilename = false;

        donut::math::uint2 displayResolution = { 2560, 1440 };
        std::filesystem::path outPath = "";
        /// An optional custom identifier for hack export filename, used as the prefix. i.e. <identifier>_<frameID>_<modeString>.exr
        /// Overwritten by input filename prefix if `alignFilename` is true or itself is not set.
        std::string identifier = "";
        /// Stores full paths to Color data and MV + Depth data (almost always NPP_JI and MVD_JI for us).
        std::vector<std::filesystem::path> hackPaths = {};
        /// Because of limitation of frame capture, we have this multi-run batch captures approach. This index to which batch of current run.
        size_t batchIndex = 0;

        /// INTERNAL, determined by exr files count in hackPaths
        size_t frameCount = 0;
        /// INTERNAL When `alignFilename`, we get the base frame index also from filename, 
        /// such that the 1st exported frame is not "0000" but "2472".
        size_t baseFrameIndex = 0;
        size_t totalBatches = 0;
        // INTERNAL, determined by (equal to) input exr size
        donut::math::int2 renderResolution;
        // INTERNAL, determined by display/render resolution
        std::string modeString;
        // INTERNAL: determined by filenames, to format frameID in filenames with leading zeros
        size_t maxFrameIDLength = 0;

        /**
         * @brief Do the following AFTER setup the struct from json config and cmdline:
         *
         * 1. If outPath not given, set to <parent of Color Path>/outputs
         *
         * 2. Set frameCount to MVD count.
         *
         * 3. Iterate thru color testdata files. Extract prefix and frameID from each filename. Store the smallest frameID;
         * ensure all files have the same prefix. If identifier is not given, set to prefix.
         *
         * 4. If alignFilename, set baseFrameIndex to smallest frameID and overwrite identifier to prefix.
         *
         * e.g. For "NPP_beauty_2472_0000_0_-0.40563965_-0.35599041.exr" being the first frame, its reference has name "NPP_beauty_2472.exr" (in NPP_GT/).
         * The identifier will be set to "NPP_beauty" and baseFrameIndex to 2072.
         * In this way, we output filenames "NPP_beauty_2472_[Quality | Quality_fg].exr", which helps filesystem and image viewers to sort them in order.
         *
         * @return true if nothing worng.
         * @throw CauldronError if a) Color count and MVD count mismatch or any is empty. b) Color filenames have more than 1 prefix.
         */
        bool PostProcess();
    } hackOptions;
    // constexpr variables for hack

    // Modify them if necessary:
    static constexpr char ColorSubdir[] = "/NPP_JI";
    static constexpr char MVDSubdir[]   = "/MVD_JI";

    constexpr static uint64_t CaptureTimeoutMS = 100;
    /**
     * @brief INTERNAL, for sleep bubble trick.
     * Each Present Callback should take 2 * StoreDelay seconds, and Present() will evenly
     * space the display time of rendered frame and FG frame to StoreDelay
     *
     * UPDATE: Previously we have this StoreDelayMS bubble (each rendered frame or FG frame) displays
     * for StoreDelayMS ms to give us enough time to **accurately** capture frame and avoid duplicates.
     * Now with image hash to check duplication, we repeat capture until getting a **precisely** new frame.
     */
    [[deprecated("Sleep bubble trick should NOT be used when having image hash")]]
    constexpr static int64_t StoreDelayMS = 2000;
    /**
     * @brief INTERNAL, timeout before trying another capture and see if it's a new frame.
     *
     * NOTE: dlfg.cpp (closed source) has a 100ms timeout before reset frame timer,
     * thus DuplicateTimeout * DuplicateMaxRetry cannot exceed 100ms, otherwise the app freezes.
     */
    constexpr static std::chrono::milliseconds DuplicateTimeout{ 50 };
    constexpr static uint32_t DuplicateMaxRetry = 5;
    /**
     * @brief INTERNAL, used for DLSS-G cold start problem.
     * See comments in DecideExportInfo() body to see how it works
     */
    constexpr static uint32_t FramesToWarmup = 3;
    constexpr static uint32_t FramesToCapture = 15;
    /// plus one more safety frame in the end to ensure last captured frame is correctly computed.
    constexpr static uint32_t FramesToReplayTotal = 19;

    // read-only data storage to copy from; copy dst are RTs defined in RenderTargets.h and GBuffer.h
    std::vector<std::shared_ptr<donut::engine::TextureData>> hackLoadedColorsHDR;
    std::vector<std::shared_ptr<donut::engine::TextureData>> hackLoadedMVs;
    std::vector<std::shared_ptr<donut::engine::TextureData>> hackLoadedDepths;
    std::vector<donut::math::float2> hackLoadedJitterOffsets;

    struct FrameData {
        const uint8_t* data;
        const uint32_t rowPitch;
        const int width;
        const int height;
        const std::string filename;
    };

    /// MemoryPool for exporting

    std::unique_ptr<uint8_t[]> hackExportMemoryPoolSR;
    std::unique_ptr<uint8_t[]> hackExportMemoryPoolFG;
    uint32_t hackExportSlotSR = 0;
	uint32_t hackExportSlotFG = 0;
    size_t hackExportBytesPerFrame = 0;

    // DEBUG
    size_t hackExportBytesPerFrameFG = 0;

    std::string hackExportFilenameSR = ""; // Set by DecideExportInfo() in beforePresent callback
    std::string hackExportFilenameFG = ""; // Set by DecideExportInfo() in beforePresent callback

    /**
     * @brief Stores image by xxhash XXH64(). Used for duplication detection after capture before export.
     */
    std::unordered_map<uint64_t, FrameData> hash_bin;

    inline void InitMemoryPool() {
        // ~63.3MB
        hackExportBytesPerFrame = hackOptions.displayResolution.x * hackOptions.displayResolution.y *
			8 /* RGBA16_FLOAT bytes per pixel */;

        hackExportMemoryPoolSR = std::make_unique<uint8_t[]>(FramesToCapture * hackExportBytesPerFrame);
        hackExportMemoryPoolFG = std::make_unique<uint8_t[]>((FramesToCapture + 1) * hackExportBytesPerFrameFG);
    }

    /**
     * @brief Called per frame in beforePresent callback to decide whther to capture SR and FG frame data.
     * 
     * By "capture", we mean:
     * 
     * (1) Call MapRenderTargetDataHDR() in RenderScene() to get SR frame data directly from AAResolvedColor.
     * 
	 * (2) Call CaptureFramePoolHDR() in afterPresent callback to get FG frame data from front-end window.
     * 
     * Set strings hackExportFilenameSR and hackExportFilenameFG to the final export filenames.
	 * Their respective frameID is adjusted with different logic, see comments in the function body.
     * 
     * If not within the respective capture frames range, set to empty for these 2 capture functions to skip.
     */
    void DecideExportInfo();

    /**
     * Load test inputs from disk. Also, it sets hackOptions.renderResolution to the input resolution.
     * 
     * \throw log::error() if not all images have the same resolution.
     * \return Success or not
     */
    bool LoadHackTextures(std::shared_ptr<donut::engine::TextureCache> textureCache);

    /**
     * Compute the actual display/render ratio then set DLSSMode to the one with optimal ratio closest to it,
     * while ensuring this actual ratio is in the range. Also set hackOptions.modeString.
     * 
     * \see StreamlineSample::UpscaleRatioMap
     * \throw log::error() if none of selectable modes meets the requirement.
     * \param [out] upMode
     */
    void SetDLSSMode(sl::DLSSMode& upMode);

    /**
     * @brief Parse from Cmdline
     * Will be called in App scope BEFORE any StreamlineSample instance is created.
     * That global option will be manually copied to the instance right before 
     * calling LoadHackTextures() above.
     */
    static HackOptionDef parseHackOptions(int argc, const char* const* argv);

    /**
     * @brief Attempt 4 (SUCCESS): Save screenshot from frontend by passing the GLFW window to Windows API.
     * Finally we find a way to save FG frames.
     * 
     * \param hWnd A Windows handle of the GLFW window get by glfwGetWin32Window() a GLFWwindow*
	 * \param StoreDelayMS Need a delay to ensure successful capture of presented frame. See HackOptionDef::StoreDelayMS.
     */
    void CaptureBitBlitLDR(HWND hWnd, std::string filename);

    /**
     * @brief Attempt 6: Save screenshot with winrt AdvancedPhotoCapture class?
     * No, I spent 2 days making it work, and finally realized it's media capture 
     * (i.e. taking a photo of you using the camera) instead of screen capture.
     */
    [[deprecated("NOT screen capture, deprecated")]]
    winrt::Windows::Foundation::IAsyncAction CaptureMediaAsync(std::string filename);

    /**
     * @brief Attempt 7: Save screenshot with winrt Windows.Media.AppRecording
     * Unfortunately, capture is not supported in our Win32 app. It's primarily for UWP apps.
     */
    [[deprecated("NOT supported, deprecated")]]
    winrt::Windows::Foundation::IAsyncAction CaptureAppRecordingAsync(std::string filename);

    /**
     * @brief Attempt 8 (SUCCESS): Capture with winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool
     * Unlike attempt 4 (success) that uses BitBlit() which captures the LDR screen,
     * here we get a Direct3D11CaptureFrame that supports RGBA16_FLOAT HDR format.
     *  
     * We install Windows Implementation Library (wil) and use wil::shared_event to handle FrameArrived()
     */
    void CaptureFramePoolHDR(const std::string& filename);

    /**
     * @brief Attempt 8 Amendment: Given a backend RenderTarget, map to raw data and add to hash_bin if no duplicate found.
     *
     * Using front-end approach CaptureFramePoolHDR() on SR export is unnecessary,
     * as we can directly read from AAResolvedColor. Also, calling CaptureFramePoolHDR() on both SR and FG
     * export causes timing issue and often results in several wrong frames.
     * 
     * @param texture Should be m_RenderTargets->AAResolvedColor, but can be used on other RGBA16_FLOAT RTs as well.
     * @param filename 
     * @return 
     */
    bool MapRenderTargetDataHDR(nvrhi::TextureHandle texture, const std::string& filename);

#pragma endregion

public:
    StreamlineSample(DeviceManager* deviceManager, sl::ViewportHandle vpHandle, UIData& ui, const std::string& sceneName, ScriptingConfig scriptingConfig);
    ~StreamlineSample();

    // Functions of interest
    bool SetupView();
    void CreateRenderPasses(bool& exposureResetRequired, float lodBias);
    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer) override;

    void SetBackBufferExtent(sl::Extent &backBufferExtent)
    {
        m_backbufferViewportExtent = backBufferExtent;
    }

    // Logistic functions 
    std::shared_ptr<TextureCache> GetTextureCache();
    std::vector<std::string> const& GetAvailableScenes() const;
    std::string GetCurrentSceneName() const;
    void SetCurrentSceneName(const std::string& sceneName);
    std::shared_ptr<ShaderFactory> GetShaderFactory() const { return m_ShaderFactory; };
    std::shared_ptr<donut::vfs::IFileSystem> GetRootFs() const { return m_RootFs; };

    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override;
    virtual bool MousePosUpdate(double xpos, double ypos) override;
    virtual bool MouseButtonUpdate(int button, int action, int mods) override;
    virtual bool MouseScrollUpdate(double xoffset, double yoffset) override;
    virtual void SetLatewarpOptions() override;
    virtual void Render(nvrhi::IFramebuffer* backBufferFramebuffer) override { RenderScene(backBufferFramebuffer); };
    virtual void Animate(float fElapsedTimeSeconds) override;
    virtual void SceneUnloading() override;
    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName) override;
    virtual void SceneLoaded() override;
    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override;

};

struct ViewportData
{
    std::shared_ptr<StreamlineSample> m_pSample;
};

struct MultiViewportApp : public ApplicationBase
{
    MultiViewportApp(DeviceManager* deviceManager, UIData& ui, const std::string& sceneName, ScriptingConfig scriptingConfig):
        Super(deviceManager),
        m_pDeviceManager(deviceManager),
        m_ui(ui),
        m_sceneName(sceneName),
        m_scripting(scriptingConfig)
    {
        m_pViewports.push_back(createViewport());
        SceneLoaded();
    }

    std::shared_ptr<ShaderFactory> GetShaderFactory() const { return m_pViewports[0]->m_pSample->GetShaderFactory(); };
    std::shared_ptr<StreamlineSample> getASample() const { return m_pViewports[0]->m_pSample; }

    virtual void RenderScene(nvrhi::IFramebuffer* framebuffer) override;
    virtual bool KeyboardUpdate(int key, int scancode, int action, int mods) override
    {
        return m_pViewports[0]->m_pSample->KeyboardUpdate(key, scancode, action, mods);
    }
    virtual bool MousePosUpdate(double xpos, double ypos) override
    {
        return m_pViewports[0]->m_pSample->MousePosUpdate(xpos, ypos);
    }
    virtual bool MouseButtonUpdate(int button, int action, int mods) override
    {
        return m_pViewports[0]->m_pSample->MouseButtonUpdate(button, action, mods);
    }
    virtual bool MouseScrollUpdate(double xoffset, double yoffset) override
    {
        return m_pViewports[0]->m_pSample->MouseScrollUpdate(xoffset, yoffset);
    }
    virtual void SetLatewarpOptions() override { getASample()->SetLatewarpOptions(); }
    virtual void Render(nvrhi::IFramebuffer* frameBuffer) override { getASample()->Render(frameBuffer); }
    virtual void Animate(float fElapsedTimeSeconds) override
    {
        for (uint32_t uV = 0; uV < m_pViewports.size(); ++uV)
        {
            m_pViewports[uV]->m_pSample->Animate(fElapsedTimeSeconds);
        }
    }
    virtual void SceneUnloading() override
    {
        m_pViewports[0]->m_pSample->SceneUnloading();
    }
    virtual bool LoadScene(std::shared_ptr<IFileSystem> fs, const std::filesystem::path& fileName) override
    {
        return m_pViewports[0]->m_pSample->LoadScene(fs, fileName);
    }
    virtual void SceneLoaded() override
    {
        Super::SceneLoaded();
    }
    virtual void RenderSplashScreen(nvrhi::IFramebuffer* framebuffer) override
    {
        m_pViewports[0]->m_pSample->RenderSplashScreen(framebuffer);
    }
    virtual bool ShouldRenderUnfocused() override
    { 
        return true; 
    }

	inline size_t getViewportCount() const { return m_pViewports.size(); }

private:
    typedef ApplicationBase Super;
    std::shared_ptr<ViewportData> createViewport()
    {
        std::shared_ptr<ViewportData> pVp = std::make_shared<ViewportData>();
        pVp->m_pSample = std::make_shared <StreamlineSample>(
            m_pDeviceManager, sl::ViewportHandle(m_nViewportsCreated++), m_ui, m_sceneName, m_scripting);
        return pVp;
    }
    uint32_t m_nViewportsCreated = 0;
    DeviceManager* m_pDeviceManager = nullptr;
    UIData& m_ui;
    std::string m_sceneName;
    ScriptingConfig m_scripting;
    std::vector<std::shared_ptr<ViewportData>> m_pViewports;
};
