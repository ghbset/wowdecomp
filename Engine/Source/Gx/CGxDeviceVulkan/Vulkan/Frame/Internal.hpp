// The frame's shared internals: its constants, its state and the helpers every part of Vulkan/Frame/ uses.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Frame.hpp"

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Context.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Loader.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Log.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <atomic>
#include <mutex>
#include <cstdint>

// The frame lives in Vulkan/Frame/, one file per responsibility, over this one header. Nothing here
// is visible outside it: the public face is Vulkan/Frame.hpp.
//
//   Objects   the device's own objects (the command pool, the slots' buffers, fences, semaphores)
//             and the retirement list
//   Images    the swapchain-derived objects (images, views, semaphores, the scene image), the acquire
//   Targets   the render passes per colour format, the depth buffers, the framebuffers, the target
//   Uploads   the pre-pass and outside-the-pass command buffers, the staging ring
//   Readback  copies and read-backs off the scene image
//   Frame.cpp opening a frame and its pass, the clear, the submit and the present
namespace gxvk::vk::frame::detail
{
    namespace context = gxvk::vk::context;
    namespace loader  = gxvk::vk::loader;
    namespace memory  = gxvk::vk::memory;
    namespace texture = gxvk::vk::texture;

    using gxvk::log::Write;
    using gxvk::log::Debug;
    using gxvk::log::Info;
    using gxvk::log::Warn;
    using gxvk::log::Error;
    using gxvk::vk::ResultName;
    using gxvk::vk::frame::kFramesInFlight;

    /// A swapchain with more images than this is refused rather than half-tracked.
    constexpr uint32_t kMaxImages = 8;

    /// The retirement list: starts at kRetiredFirst entries and doubles up to kMaxRetired. Full
    /// means leaked (counted), never destroyed early.
    constexpr uint32_t kRetiredFirst = 1024;
    constexpr uint32_t kMaxRetired   = 65536;

    /// The staging ring: chunks of kStagingChunk bytes per slot, up to kMaxStagingChunks; a request
    /// past them, or larger than a chunk, gets a buffer of its own for the frame.
    constexpr VkDeviceSize kStagingChunk     = 8u * 1024u * 1024u;
    constexpr uint32_t     kMaxStagingChunks = 64;
    constexpr uint32_t     kMaxDedicated     = 256;
    constexpr VkDeviceSize kStagingAlign     = 16;

    /// A ring chunk kept past the frame's need for this many frames is given back.
    constexpr uint32_t kStagingTrimFrames = 600;

    /// Colour formats a target may have (one pass set each), depth buffers (one per target size),
    /// framebuffers (one per target image, layer and depth buffer) kept at once.
    constexpr uint32_t kMaxPassFormats  = 8;
    constexpr uint32_t kMaxDepthBuffers = 8;
    constexpr uint32_t kMaxFramebuffers = 64;

    /// The scene's colour format: the engine's Argb8888 (its back buffer's).
    constexpr VkFormat kSceneFormat = VK_FORMAT_B8G8R8A8_UNORM;

    /// How long an acquire may block before the frame goes on without an image (a surface in a
    /// fullscreen handoff can hold the acquire for as long as it likes).
    constexpr uint64_t kAcquireTimeoutNs = 1000000000ull;

    struct FrameSlot
    {
        VkCommandBuffer cmd            = nullptr;   ///< the main buffer: the render passes and the draws
        VkCommandBuffer pre            = nullptr;   ///< the pre-pass buffer: uploads ahead of the draws
        VkSemaphore     imageAvailable = VK_NULL_HANDLE;
        VkFence         fence          = VK_NULL_HANDLE;
        uint64_t        serial         = 0;

        memory::Buffer chunks[kMaxStagingChunks];
        uint32_t       chunkCount = 0;
        uint32_t       chunkUsed  = 0;
        VkDeviceSize   cursor     = 0;
        uint32_t       idleFrames = 0;
        memory::Buffer dedicated[kMaxDedicated];
        uint32_t       dedicatedCount = 0;
    };

    /// One resource waiting for its frame to complete before it is destroyed.
    struct Retired
    {
        memory::Buffer buffer;
        texture::Image image;
        VkFramebuffer  framebuffer = VK_NULL_HANDLE;
        VkImageView    view        = VK_NULL_HANDLE;
        VkImage        rawImage    = VK_NULL_HANDLE;   ///< a depth buffer's image, with its block
        memory::Block  rawBlock;
        uint64_t       serial      = 0;
    };

    /// The four pass variants of one colour format: index bit 0 = colour cleared on load, bit 1 =
    /// depth cleared on load; otherwise loaded.
    struct PassSet
    {
        VkFormat     colour  = VK_FORMAT_UNDEFINED;
        VkRenderPass pass[4] = {};
    };

    /// A depth buffer of one size, shared by every target of that size.
    struct DepthBuffer
    {
        VkImage       image       = VK_NULL_HANDLE;
        VkImageView   view        = VK_NULL_HANDLE;
        memory::Block block;
        VkExtent2D    extent      = { 0, 0 };
        bool          initialised = false;   ///< a pass has written it: later passes may LOAD it
        uint64_t      lastUsed    = 0;
    };

    /// A framebuffer over one target (image, layer) and the depth buffer of its size.
    struct Framebuffer
    {
        VkImage       image    = VK_NULL_HANDLE;
        uint32_t      layer    = 0;
        VkImageView   view     = VK_NULL_HANDLE;   ///< a 2D view of the layer, owned here
        uint32_t      depth    = 0;                ///< index into the depth table
        VkFramebuffer fb       = VK_NULL_HANDLE;
        VkFormat      format   = VK_FORMAT_UNDEFINED;
        VkExtent2D    extent   = { 0, 0 };
        uint64_t      lastUsed = 0;
    };

    /// What the next pass opens on.
    struct Target
    {
        const texture::Image* image = nullptr;   ///< null: the scene
        uint32_t              layer = 0;
    };

    struct State
    {
        VkDevice              device = nullptr;
        const loader::Device* dapi   = nullptr;
        VkCommandPool         pool   = VK_NULL_HANDLE;
        FrameSlot             slots[kFramesInFlight];

        // --- swapchain-derived ----------------------------------------------------------------------
        uint32_t      swapGen    = UINT32_MAX;
        uint32_t      imageCount = 0;
        VkExtent2D    extent     = { 0, 0 };
        VkFormat      format     = VK_FORMAT_UNDEFINED;
        VkImage       images[kMaxImages]         = {};
        VkImageView   views[kMaxImages]          = {};
        VkSemaphore   renderFinished[kMaxImages] = {};   ///< per IMAGE: only re-acquiring an image proves its present consumed it
        VkFence       imagesInFlight[kMaxImages] = {};
        uint64_t      imageSerial[kMaxImages]    = {};   ///< RENDERTHREAD: the serial imagesInFlight[i]'s slot had then
        texture::Image scene;                            ///< the offscreen colour image the present pass shows

        // --- targets ----------------------------------------------------------------------------------
        VkFormat    depthFormat = VK_FORMAT_UNDEFINED;
        PassSet     passes[kMaxPassFormats];
        uint32_t    passCount = 0;
        uint32_t    passGen   = 0;
        DepthBuffer depths[kMaxDepthBuffers];
        uint32_t    depthCount = 0;
        Framebuffer fbs[kMaxFramebuffers];
        uint32_t    fbCount = 0;

        // --- the frame being recorded ---------------------------------------------------------------
        bool     open      = false;
        bool     passOpen  = false;
        uint32_t fi        = 0;
        uint64_t serial    = 0;
        uint64_t completed = 0;
        uint64_t submitted = 0;
        bool     acquired  = false;
        bool     noImage   = false;
        uint32_t image     = 0;
        Target   target;                      ///< what the next pass opens on
        uint32_t passFb        = UINT32_MAX;  ///< the framebuffer of the open pass
        bool     sceneTouched  = false;       ///< a pass was opened on the scene this frame
        bool        queryActive = false;      ///< an occlusion query is begun on the open pass
        VkQueryPool queryPool   = VK_NULL_HANDLE;
        uint32_t    queryIndex  = 0;

        /// Clears asked for the current target before its pass is open: become its load ops.
        bool  clearColourPending = false;
        bool  clearDepthPending  = false;
        float clearColour[4]     = { 0.0f, 0.0f, 0.0f, 1.0f };

        bool     rtPresent = false;   ///< RENDERTHREAD: Present() wants the render thread to present this frame
        bool     rebuild     = false;
        bool     surfaceLost = false;   ///< the rebuild must remake the surface too
        bool     failed      = false;
        uint64_t presentedCount = 0;

        Retired* retired      = nullptr;
        uint32_t retiredCount = 0;
        uint32_t retiredCap   = 0;

        bool loggedStagingFail = false;
        bool loggedNoPresent   = false;
        bool loggedNoScene     = false;

        gxvk::vk::frame::Stats stats;
    };
    extern State s;

    // Frame.cpp
    bool EnsureFrame();
    bool BeginPass();
    void EndPass();
    void FinishFrame(bool present);

    // Objects.cpp
    void Fail(const char* what, VkResult r);
    bool EnsureDeviceObjects(const context::Handles& h);
    void DestroyDeviceObjects();
    void Poll();
    void CollectRetired(bool all);
    void Retire(Retired& entry, uint64_t lastUsed);

    // Images.cpp
    bool EnsureImageObjects(const context::Handles& h);
    void DropImageObjects();
    bool Acquire();

    // Targets.cpp
    bool ChooseDepthFormat(const context::Handles& h);
    const PassSet* PassesFor(VkFormat colour);
    uint32_t FramebufferFor(const Target& t, VkFormat format, VkExtent2D extent);
    bool TargetView(const Target& t, const texture::Image*& image, VkFormat& format, VkExtent2D& extent);
    void DestroyTargets();
    void DropFramebuffersOf(VkImage image, bool retire);

    // Thread.cpp -- RENDERTHREAD: the render thread (submit + present off the main thread)
    extern std::atomic<uint64_t> gSubmitted;
    /// Held by the main thread around a slot fence's reset and its new serial, and by the render
    /// thread around its wait on another slot's fence for a swapchain image (imagesInFlight).
    extern std::mutex gFenceLock;   ///< the last serial actually given to vkQueueSubmit, on either thread
    bool RtWanted();   ///< the CVar is on: Present() leaves the acquire to the render thread
    bool RtHandOff(uint32_t fi, uint64_t serial, bool wantPresent);   ///< false: not threaded, submit here
    void RtWait(uint64_t serial);   ///< until the render thread is done with `serial` (or idle)
    void RtCollect();               ///< picks up the render thread's rebuild / surface-lost / failure flags
    void RtStop(bool destroy);
    uint64_t RtPresented();

    // Uploads.cpp
    void StagingFrameBegin(FrameSlot& slot);
    void StagingFrameEnd(FrameSlot& slot);
    void StagingDestroy(FrameSlot& slot);
}
