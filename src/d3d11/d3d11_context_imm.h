#pragma once

#include "../util/util_time.h"

#include "../util/sync/sync_signal.h"

#include "d3d11_context.h"
#include "d3d11_state_object.h"
#include "d3d11_video.h"

namespace dxvk {
  
  class D3D11Buffer;
  class D3D11CommonTexture;

  class D3D11ImmediateContext : public D3D11CommonContext<D3D11ImmediateContext> {
    friend class D3D11CommonContext<D3D11ImmediateContext>;
    friend class D3D11SwapChain;
    friend class D3D11VideoContext;
    friend class D3D11DXGIKeyedMutex;
  public:
    
    D3D11ImmediateContext(
            D3D11Device*    pParent,
      const Rc<DxvkDevice>& Device);
    ~D3D11ImmediateContext();
    
    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject);

    HRESULT STDMETHODCALLTYPE GetData(
            ID3D11Asynchronous*         pAsync,
            void*                       pData,
            UINT                        DataSize,
            UINT                        GetDataFlags);
    
    void STDMETHODCALLTYPE Begin(
            ID3D11Asynchronous*         pAsync);
    
    void STDMETHODCALLTYPE End(
            ID3D11Asynchronous*         pAsync);
    
    void STDMETHODCALLTYPE Flush();
    
    void STDMETHODCALLTYPE Flush1(
            D3D11_CONTEXT_TYPE          ContextType,
            HANDLE                      hEvent);

    HRESULT STDMETHODCALLTYPE Signal(
            ID3D11Fence*                pFence,
            UINT64                      Value);
    
    HRESULT STDMETHODCALLTYPE Wait(
            ID3D11Fence*                pFence,
            UINT64                      Value);

    void STDMETHODCALLTYPE ExecuteCommandList(
            ID3D11CommandList*  pCommandList,
            BOOL                RestoreContextState);
    
    HRESULT STDMETHODCALLTYPE FinishCommandList(
            BOOL                RestoreDeferredContextState,
            ID3D11CommandList   **ppCommandList);
    
    HRESULT STDMETHODCALLTYPE Map(
            ID3D11Resource*             pResource,
            UINT                        Subresource,
            D3D11_MAP                   MapType,
            UINT                        MapFlags,
            D3D11_MAPPED_SUBRESOURCE*   pMappedResource);
    
    void STDMETHODCALLTYPE Unmap(
            ID3D11Resource*             pResource,
            UINT                        Subresource);
            
    void STDMETHODCALLTYPE SwapDeviceContextState(
            ID3DDeviceContextState*           pState,
            ID3DDeviceContextState**          ppPreviousState);

    void Acquire11on12Resource(
            ID3D11Resource*             pResource,
            VkImageLayout               SrcLayout);

    void Release11on12Resource(
            ID3D11Resource*             pResource,
            VkImageLayout               DstLayout);

    void SynchronizeCsThread(
            uint64_t                          SequenceNumber);

    D3D10Multithread& GetMultithread() {
        return m_multithread;
    }

    D3D10DeviceLock LockContext() {
      return m_multithread.AcquireLock();
    }

    void InjectCsChunk(
            DxvkCsQueue                 Queue,
            DxvkCsChunkRef&&            Chunk,
            bool                        Synchronize);

    template<typename Fn>
    void InjectCs(
            DxvkCsQueue                 Queue,
            Fn&&                        Command) {
      // This intentionally bypasses the D3D11 EmitCs tail funnel: injected
      // work is not a D3D11 command and can be ordered ahead of the current
      // stream. All current callers are initialization, debug-name, or
      // latency-tracker operations that do not bind or observe D3D11 context
      // state. A future injected command that does either must use EmitCs (or
      // explicitly consume the retained command-list tail under the context
      // lock) instead of this escape hatch.
      auto chunk = AllocCsChunk();
      chunk->push(std::move(Command));

      InjectCsChunk(Queue, std::move(chunk), false);
    }

    /**
     * \brief Helios: bounded wait for the current frame's GPU completion
     *
     * Flushes pending work and waits — bounded by \c TimeoutUs — until the
     * flush's submission completes on the GPU (m_submissionFence reaches the
     * flush's submission id; the fence signals at GPU completion, the same
     * mechanism as the frame-latency event). Present-path ordering: nothing
     * in this stack makes dwm's venus rendering visible to dxgkrnl as DMA,
     * so no fence orders the IddCx consumer's copy against in-flight GPU
     * writes of the just-presented buffer — the gate closes that window
     * deterministically when it completes in time. On timeout (CS backlog /
     * slow GPU) it returns false and the caller proceeds: a rare one-frame
     * ghost self-heals at the next per-acquire refresh, and presents stay
     * bounded instead of reintroducing multi-second churn dips.
     * \returns \c true if the frame completed within the timeout
     */
    bool HeliosWaitFrameComplete(uint64_t TimeoutUs);

    /**
     * \brief Helios: waits until the current frame has been SUBMITTED
     *
     * The present-ordering handshake the KMD actually requires: flush, wait for
     * the CS thread to record the flush chunk, then wait for the submission
     * thread to perform the vkQueueSubmit. After this returns, every Venus
     * command of the frame has reached the wire and been given a fence, so the
     * watermark DxgkDdiRender samples covers the frame and the KMD holds the
     * scanout refresh until those fences retire.
     *
     * This is strictly weaker -- and far cheaper -- than
     * \ref HeliosWaitFrameComplete, which waits for GPU completion and thereby
     * removes all CPU/GPU overlap. It takes no timeout because it waits only on
     * guest CPU threads; there is no slow-GPU case to bound.
     */
    void HeliosWaitFrameSubmitted();

    /**
     * \brief Helios: record a present-fence signal on the open command list
     *
     * Emits a timeline-semaphore signal that rides the CURRENT recording
     * chunk, i.e. it submits WITH the frame's remaining work and signals —
     * at host GPU completion, via the ICD's ring>=1 wire fence + retire
     * thread — once everything recorded so far has executed (WS1 #4
     * producer side). No flush and no wait happen here: the present path's
     * existing Flush right after this call submits frame + signal together.
     */
    void HeliosSignalPresentFence(
      const Rc<DxvkFence>&        Fence,
            uint64_t              Value);

    /**
     * \brief Helios: image-level copy for the dcomp present vehicle
     *
     * Records a full-subresource copyImage on the open command list, at the
     * DXVK image level so the vehicle can source the LIVE storage of an
     * imported frame (the direct-bind staging ALIAS for device-local
     * imports) instead of the D3D11 texture's private image, which is only
     * refreshed at command-list start when a prior read armed it — the COM
     * CopySubresourceRegion path would read a stale (frame-1: undefined)
     * private image. DxvkContext::copyImage fires the bounded copy-time
     * consumer present-wait for Import-mode sources (6eab004c), which is
     * exactly what orders this copy against the producing ICD's GPU writes.
     */
    void HeliosCopyExternalFrame(
      const Rc<DxvkImage>&        DstImage,
      const Rc<DxvkImage>&        SrcImage,
            VkExtent3D            Extent);

    /**
     * \brief Helios: ordered snapshot copy for the D4b scanout ring
     *
     * Records a full-subresource copy of the presented primary into a
     * snapshot-ring image on the open command list, at present position, so
     * the copy rides the frame's own command stream: it executes after the
     * frame's draws and before anything of frame N+1 (queue order — no waits,
     * no stalls). A full-extent OPTIMAL->OPTIMAL same-format copy takes
     * DxvkContext::copyImageHw, which handles the layout transitions
     * internally, and the copy-time consumer present-wait no-ops for
     * non-import sources, so no consumer wait is armed here. When the ring's
     * scan-out-safe format differs from the primary, this uses the explicit
     * numeric framebuffer conversion rather than a raw bit copy. The destination
     * is a scanout-flagged image, so heliosEmitScanoutReuseWaits gates this
     * list on any still-in-flight host readback of the slot (the D4a acquire
     * as the snapshot-overwrite backstop).
     */
    bool HeliosCopyPresentSnapshot(
      const Rc<DxvkImage>&        DstImage,
      const Rc<DxvkImage>&        SrcImage,
            VkExtent3D            Extent,
            bool                  WindowedBltReservation);

    /**
     * \brief Helios: format-converting DXGI blit
     *
     * Records an explicit framebuffer conversion instead of D3D11's regular
     * CopySubresourceRegion bit copy. The caller has already validated that
     * both operands are single-sampled color images and that the region fits.
     */
    void HeliosConvertImage(
      const Rc<DxvkImage>&        DstImage,
            VkImageSubresourceLayers DstSubresource,
            VkOffset3D            DstOffset,
      const Rc<DxvkImage>&        SrcImage,
            VkImageSubresourceLayers SrcSubresource,
            VkOffset3D            SrcOffset,
            VkExtent3D            Extent);

    /**
     * \brief Helios: inject a command ordered after all recorded work
     *
     * Dispatches the current recording chunk to the CS queue, then appends
     * the command on the ordered queue — WITHOUT synchronizing the CS
     * thread. Plain InjectCs bypasses the open chunk (commands recorded
     * before the call would execute after it); SynchronizeCsThread gives
     * the same ordering but blocks the caller behind the whole CS queue,
     * which measured up to 1.9 s per present during login churn
     * (rotate-perf, 18th session) — the "occasional framerate dips".
     */
    template<typename Fn>
    void InjectCsOrderedAfterPending(Fn&& Command) {
      D3D10DeviceLock lock = LockContext();

      FlushCsChunk();
      InjectCs(DxvkCsQueue::Ordered, std::move(Command));
    }

  private:
    
    DxvkCsThread            m_csThread;
    uint64_t                m_csSeqNum = 0ull;

    // Inline command-list replay may fold multiple logical CS chunks into
    // one ordered queue entry. This offset keeps GpuFlushTracker's chunk
    // cadence in the original logical timeline; resource waits keep using
    // the physical CS sequence above.
    uint64_t                m_heliosFlushChunkOffset = 0ull;

    // The default arm remains at 16 replay wrappers. The opt-in byte arm
    // uses the same 256 KiB worst-case budget for full chunks, while its
    // independent 256-wrapper ceiling bounds pathological tiny lists.
    uint64_t                m_heliosInlineReplayBytes = 0ull;
    uint32_t                m_heliosInlineReplayChunkCount = 0u;
    static constexpr uint32_t MaxHeliosInlineReplayChunks = 16u;
    static constexpr uint32_t MaxHeliosInlineReplayByteAccountingChunks = 256u;
    static constexpr uint64_t MaxHeliosInlineReplayByteAccountingBytes
      = uint64_t(MaxHeliosInlineReplayChunks) * DxvkCsChunkSize;

    uint32_t                m_mappedImageCount = 0u;

    Rc<sync::CallbackFence> m_submissionFence;
    uint64_t                m_submissionId = 0ull;
    DxvkSubmitStatus        m_submitStatus;

    uint64_t                m_flushSeqNum = 0ull;
    GpuFlushTracker         m_flushTracker;

    Rc<sync::Fence>         m_stagingBufferFence;

    VkDeviceSize            m_discardMemoryCounter = 0u;
    VkDeviceSize            m_discardMemoryOnFlush = 0u;

    D3D10Multithread        m_multithread;
    D3D11VideoContext       m_videoContext;

    Com<D3D11DeviceContextState, false> m_stateObject;

    D3DDestructionNotifier  m_destructionNotifier;

    std::string             m_flushReason;

    bool                    m_hasPendingUnresolvedPass = false;

    HRESULT MapBuffer(
            D3D11Buffer*                pResource,
            D3D11_MAP                   MapType,
            UINT                        MapFlags,
            D3D11_MAPPED_SUBRESOURCE*   pMappedResource);
    
    HRESULT MapImage(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource,
            D3D11_MAP                   MapType,
            UINT                        MapFlags,
            D3D11_MAPPED_SUBRESOURCE*   pMappedResource);
    
    void UnmapImage(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource);
    
    void ReadbackImageBuffer(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource);

    void UpdateDirtyImageRegion(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource,
      const D3D11_COMMON_TEXTURE_REGION* pRegion);

    void UpdateMappedBuffer(
            D3D11Buffer*                pDstBuffer,
            UINT                        Offset,
            UINT                        Length,
      const void*                       pSrcData,
            UINT                        CopyFlags);

    void SynchronizeDevice();

    void EndFrame(
            Rc<DxvkLatencyTracker>      LatencyTracker);
    
    bool WaitForResource(
      const DxvkPagedResource&          Resource,
            uint64_t                    SequenceNumber,
            D3D11_MAP                   MapType,
            UINT                        MapFlags);
    
    void EmitCsChunk(DxvkCsChunkRef&& chunk);

    void TrackTextureSequenceNumber(
            D3D11CommonTexture*         pResource,
            UINT                        Subresource);

    void TrackBufferSequenceNumber(
            D3D11Buffer*                pResource);

    uint64_t GetCurrentSequenceNumber();

    uint64_t GetFlushTrackerChunkId();

    uint64_t GetPendingCsChunks();

    void ApplyDirtyNullBindings();

    void ConsiderFlush(
            GpuFlushType                FlushType);

    void ExecuteFlush(
            GpuFlushType                FlushType,
            HANDLE                      hEvent,
            BOOL                        Synchronize);

    void ThrottleAllocation();

    void ThrottleDiscard(
            VkDeviceSize                Size);

    void NotifyRenderPassBoundary(
            bool                        IsMultisampled);

    void NotifyResolve();

    void RequestFlush(
            D3D11_CONTEXT_TYPE          ContextType,
            HANDLE                      hEvent);

    DxvkStagingBufferStats GetStagingMemoryStatistics();

    static GpuFlushType GetMaxFlushType(
            D3D11Device*    pParent,
      const Rc<DxvkDevice>& Device);

  };
  
}
