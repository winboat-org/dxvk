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
