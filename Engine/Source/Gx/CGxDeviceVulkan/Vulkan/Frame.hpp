// The frame: two command buffers per frame slot (a pre-pass for uploads ahead of the draws, the
// main one the draws land in), the render targets the draws land on -- the scene (an offscreen
// colour image at the swapchain's size with a depth buffer, shown by the present pass) and the
// engine's render-target textures -- the render passes and framebuffers over them, the staging ring
// for this frame's uploads, the acquire, the submit and the present, and the frame serials that say
// which GPU work is done.
#pragma once

#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Memory.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Texture.hpp"
#include "Engine/Source/Gx/CGxDeviceVulkan/Vulkan/Vk.hpp"

#include <cstdint>

// Two frames in flight. A frame is opened by the first Pass(), PrePass(), Outside(), Stage() or
// Present(), and closed by Present(): Present() ends the render pass, records the present pass
// (the scene image onto the acquired swapchain image, through the gamma ramp), submits the slot's
// two command buffers (pre-pass first) with the slot's fence, and presents. Each frame has a serial;
// a resource stamped with the serial of the frame that last named it can be asked about through
// Completed() and WaitSerial(), which is how the pools know when a buffer may be rewritten and the
// textures when an image may be destroyed.
//
// Targets: every colour target -- the scene, a render-target texture, one face of a cube map --
// rests in SHADER_READ_ONLY_OPTIMAL between passes, so a draw samples it with no barrier of its own;
// the pass over it is one of four variants (colour CLEAR or LOAD, depth CLEAR or LOAD) of the same
// attachment layout, compatible with each other, so one pipeline serves them all. Each target of a
// given size gets a depth buffer of the device's depth format.
namespace gxvk::vk::frame
{
    constexpr uint32_t kFramesInFlight = 2;

    /**
     * The target the next draws land on: a render-target texture's image (one layer of it, for a
     * cube face) or, with null, the scene. Ends an open pass when the target changes.
     *
     * @param const texture::Image* image : an image made with COLOR_ATTACHMENT usage, or null
     * @param uint32 layer : the array layer (cube face) drawn into
     * @param uint32 level : the mip level drawn into (RTT MIPS, Gx.h GxRenderTargetSet); 0 for a cube face
     */
    void SetTarget(const texture::Image* image, uint32_t layer, uint32_t level = 0);

    /// The scene image (B8G8R8A8, the swapchain's size), or null before the swapchain exists.
    const texture::Image* Scene();

    /// The render-target image the next draws land on, or null for the scene.
    const texture::Image* Target();

    /// The depth format every target's depth buffer has (chosen at the first frame).
    VkFormat DepthFormat();

    /// An image is going away: every framebuffer over it is retired with the frame that last used it.
    void ForgetImage(const texture::Image& image);

    /**
     * The engine's clear of the current target. With its pass open the colour and depth are
     * cleared in place over `rect` (vkCmdClearAttachments), as D3D's Clear clears the viewport
     * rectangle between draws; before the pass is open a whole-target clear becomes the pass's load
     * op, and a partial one opens the pass and clears in place.
     *
     * @param uint32 mask : bit 0 colour, bit 1 depth
     * @param float[4] colour : r, g, b, a in 0..1
     * @param VkRect2D* rect : the rectangle, inside the target; null for the whole target
     */
    void Clear(uint32_t mask, const float colour[4], const VkRect2D* rect);

    /**
     * The frame's main command buffer with its render pass open on the current target: what a draw
     * records into. Opens the frame and the pass on first call; after Outside() ended the pass,
     * resumes it with the target's contents kept. Null when the target cannot be had (no swapchain
     * for the scene, a failed frame): the draw is dropped.
     */
    VkCommandBuffer Pass();

    /**
     * The frame's main command buffer OUTSIDE the render pass, for a transfer into a resource this
     * frame's draws already named: ends the pass if it is open (the next Pass() resumes it). Opens
     * the frame. While a query is active the pre-pass buffer is handed out instead, so the pass
     * stays open. Null when the frame cannot open.
     */
    VkCommandBuffer Outside();

    /**
     * The frame's pre-pass command buffer: recorded ahead of the frame's draws and submitted before
     * the main buffer. For a resource nothing in this frame has named yet. Opens the frame.
     */
    VkCommandBuffer PrePass();

    /**
     * Host-visible bytes for an upload recorded in this frame, from the slot's staging ring (reused
     * once the frame completes) or, past the ring, from a buffer of their own retired with the
     * frame. The caller writes `mapped`; the bytes are flushed at the submit.
     *
     * @return bool ok : false when no memory could be had (logged once) or the frame cannot open
     */
    bool Stage(VkDeviceSize bytes, VkBuffer& buffer, VkDeviceSize& offset, void*& mapped);

    /**
     * Copies a rectangle of the scene into one level and layer of an image (D3D's StretchRect off
     * the back buffer), outside the pass; the image ends in SHADER_READ_ONLY_OPTIMAL.
     *
     * @param bool linear : filter when the rectangles differ in size
     * @return bool ok
     */
    bool CopyFromScene(texture::Image& dst, uint32_t layer, uint32_t level, const VkRect2D& src, const VkRect2D& dstRect, bool linear);

    /**
     * VFOG (CGxDevice::DeviceCopySceneDepth): copies the DEPTH aspect of the scene's depth buffer
     * (the swapchain's size; shared with any render target of that size) into `dst`, outside the
     * pass. `dst` is a texture::CreateDepthCopy() image of DepthFormat() at the scene's extent; it
     * ends in SHADER_READ_ONLY_OPTIMAL, the depth buffer back in DEPTH_STENCIL_ATTACHMENT_OPTIMAL
     * (the next Pass() loads it). Refused while a query pins the pass, or when no pass wrote the
     * scene's depth this frame.
     *
     * @return bool ok
     */
    bool CopyDepthFromScene(texture::Image& dst);

    /**
     * Reads a rectangle of the scene back to the host, as B, G, R, A bytes per pixel: submits the
     * frame's work so far, waits for it, copies. The frame is closed afterwards; the next Present()
     * opens a new one and shows the scene as it stands.
     *
     * @param const VkRect2D& rect : inside the scene
     * @param uint8_t* out : rect.extent.width * rect.extent.height * 4 bytes
     * @return bool ok
     */
    bool ReadScene(const VkRect2D& rect, uint8_t* out);

    /**
     * Begins an occlusion query on the current pass (opened if need be). One query at a time. A
     * query cannot span two render pass instances, so while it is active Outside() hands out the
     * pre-pass buffer instead of ending the pass, and if the pass has to end anyway (a target
     * change, a read-back) the frame ends the query first.
     *
     * @param VkQueryPool pool, uint32 index : the query, reset by the caller ahead of the frame
     * @param bool precise : VK_QUERY_CONTROL_PRECISE_BIT (the device enabled occlusionQueryPrecise)
     * @return bool begun
     */
    bool BeginQuery(VkQueryPool pool, uint32_t index, bool precise);

    /**
     * Ends the active query (nothing if the frame already ended it).
     *
     * @return uint64 serial : the frame the query's commands are recorded in; 0 when no query was begun
     */
    uint64_t EndQuery();

    /// Whether a query is active on the open frame.
    bool QueryActive();

    /// Submits the open frame's work so far without presenting (the next call opens a new frame
    /// on the same targets, their contents kept): what a blocking query read needs.
    void Flush();

    /// The render pass the current target's draws are recorded in; pipelines are built against it.
    VkRenderPass RenderPass();

    /// Bumped when every render pass is recreated: pipelines built against an older generation are
    /// no longer compatible.
    uint32_t PassGeneration();

    /// The sample count of the current target's passes (the pipelines' rasterizationSamples): the
    /// scene's MSAA count, 1 for every texture target.
    uint32_t PassSamples();

    /// gxMultisample (1/2/4/8). Takes effect when the scene image is next made -- every format
    /// change rebuilds the swapchain, and with it the scene -- clamped to the adapter's counts.
    void SetSamples(uint32_t samples);

    /// The extent of the current target.
    VkExtent2D Extent();

    /// The slot of the open frame (0 .. kFramesInFlight - 1), or of the frame that opens next.
    uint32_t Slot();

    /// The serial of the open frame, or of the frame that opens next.
    uint64_t Serial();

    /// The highest serial known complete on the GPU.
    uint64_t Completed();

    /// The last serial submitted.
    uint64_t Submitted();

    /// Whether `serial` is the open frame's.
    bool UsedThisFrame(uint64_t serial);

    /**
     * Waits until the frame of `serial` has completed. The open frame cannot be waited for.
     *
     * @return bool done : false for the open frame or a failed wait
     */
    bool WaitSerial(uint64_t serial);

    /// Destroys a buffer once the frame that last named it has completed. Zeroes `buffer`.
    void RetireBuffer(memory::Buffer& buffer, uint64_t lastUsed);

    /// As RetireBuffer(), for an image (texture::DestroyIdle once its frame has completed).
    void RetireImage(texture::Image& image, uint64_t lastUsed);

    /**
     * Ends the frame: with `visible`, the scene gets its pass if nothing opened one (so the clear
     * colour is what the window shows) and the present pass puts it on the acquired swapchain image;
     * the frame is submitted and presented.
     *
     * @param bool visible : the window can show a frame (not minimised, non-empty client area)
     * @return bool rebind : the swapchain went stale (out of date, suboptimal, a lost surface --
     *         dropped here with the swapchain -- or a RequestRebuild()) and the frame has dropped
     *         what named it; the caller rebuilds it with context::Rebind()
     */
    bool Present(bool visible);

    /// A WM_SIZE (a resize, a restore): the swapchain is stale, rebuild it once at the next Present.
    void RequestRebuild();

    /**
     * Finishes an open frame (presenting it when it acquired an image), waits for the device,
     * drops everything that names the swapchain (the scene and its depth buffer included). Before
     * context::ReleaseWindow() and context::Teardown(). Safe when nothing stands.
     */
    void ReleaseSwapchain();

    /// Everything: ReleaseSwapchain(), the retired resources, the render passes, the depth buffers,
    /// the framebuffers, the staging ring, the slots and the command pool. Before texture::Shutdown()
    /// and memory::Shutdown().
    void Destroy();

    /**
     * RENDERTHREAD: with `on`, the frames Present() ends are handed to a render thread that acquires,
     * records the present pass, submits and presents, while the main thread records the next frame
     * into the other slot. Off (the default) stops the thread after it drained. Read every frame from
     * the renderThread CVar.
     */
    void SetThreaded(bool on);

    /// RENDERTHREAD: waits until the render thread has submitted (and presented) everything handed
    /// to it. Anything on the main thread that needs the queue or the swapchain calls it first.
    void Drain();

    /// Frames presented since the device came up.
    uint64_t Count();

    /// A Vulkan call failed in a way the frame cannot recover from; nothing is presented any more.
    bool Failed();

    struct Stats
    {
        uint64_t frames          = 0;   ///< submitted
        uint64_t presented       = 0;
        uint64_t passes          = 0;   ///< render passes begun (resumes included)
        uint64_t passResumes     = 0;   ///< passes begun with the target's contents kept
        uint64_t targetPasses    = 0;   ///< passes on a render-target texture
        uint64_t clearsInPass    = 0;   ///< clears recorded inside an open pass
        uint64_t noImage         = 0;   ///< Pass() calls with no target to be had
        uint64_t serialWaits     = 0;   ///< WaitSerial() calls that blocked
        uint64_t retired         = 0;   ///< resources destroyed through Retire*()
        uint64_t retireLeaked    = 0;   ///< neither destroyed nor listed (list full)
        uint32_t retirePeak      = 0;   ///< the longest waiting list
        uint64_t stagedBytes     = 0;   ///< handed out by Stage()
        uint64_t stagedDedicated = 0;   ///< Stage() requests answered with a buffer of their own
        uint32_t stagingChunks   = 0;   ///< ring chunks alive, all slots
        uint32_t stagingPeak     = 0;
        uint32_t depthBuffers    = 0;   ///< alive
        uint32_t framebuffers    = 0;   ///< alive
        uint64_t sceneCopies     = 0;   ///< CopyFromScene() calls
        uint64_t depthCopies     = 0;   ///< CopyDepthFromScene() calls (VFOG)
        uint64_t sceneReads      = 0;   ///< ReadScene() calls (each submits and waits)
        uint64_t queries         = 0;   ///< queries begun
        uint64_t queriesCut      = 0;   ///< queries the frame ended itself (the pass had to end)
        uint64_t flushes         = 0;   ///< Flush() calls
    };
    void GetStats(Stats& out);
}
