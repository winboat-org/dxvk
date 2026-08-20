#include "d3d11_context_def.h"
#include "d3d11_device.h"

#include "../dxvk/dxvk_helios_feed_trace.h"

namespace dxvk {

  constexpr size_t MaxHeliosRecycledCommandLists = 64u;

  std::vector<Com<D3D11CommandList>> CreateHeliosRecycledCommandListCache() {
    std::vector<Com<D3D11CommandList>> result;
    if (heliosClRecycleCache())
      result.reserve(MaxHeliosRecycledCommandLists);
    return result;
  }
  
  D3D11DeferredContext::D3D11DeferredContext(
          D3D11Device*    pParent,
    const Rc<DxvkDevice>& Device,
          UINT            ContextFlags)
  : D3D11CommonContext<D3D11DeferredContext>(pParent, Device, ContextFlags, 0u),
    m_recycledCommandLists(CreateHeliosRecycledCommandListCache()),
    m_commandList(CreateCommandList()),
    m_destructionNotifier(this) {
    ResetContextState();
  }


  D3D11DeferredContext::~D3D11DeferredContext() {
    // This is the actual lifetime endpoint for the DDI-owned cache. Logical
    // RecycleCreateDeferredContext deliberately retains it for the next
    // recording; ordinary vector destruction here releases every cached COM
    // list and therefore preserves private-data/notifier destruction.
    uint64_t drained = m_recycledCommandLists.size();
    m_recycledCommandLists.clear();

    if (unlikely(heliosClRecycleStats())) {
      Logger::info(str::format(
        "D3D11: Helios command-list recycle cache: hits=", m_heliosRecycleHits,
        " misses=", m_heliosRecycleMisses,
        " admitted=", m_heliosRecycleAdmitted,
        " dropped=", m_heliosRecycleDropped,
        " drained=", drained));
    }
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::QueryInterface(REFIID riid, void** ppvObject) {
    if (riid == __uuidof(ID3DDestructionNotifier)) {
      *ppvObject = ref(&m_destructionNotifier);
      return S_OK;
    }

    return D3D11CommonContext<D3D11DeferredContext>::QueryInterface(riid, ppvObject);
  }


  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::GetData(
          ID3D11Asynchronous*               pAsync,
          void*                             pData,
          UINT                              DataSize,
          UINT                              GetDataFlags) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D11: GetData called on a deferred context");

    return DXGI_ERROR_INVALID_CALL;
  }


  void STDMETHODCALLTYPE D3D11DeferredContext::Begin(
          ID3D11Asynchronous*         pAsync) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(!pAsync))
      return;

    Com<D3D11Query, false> query(static_cast<D3D11Query*>(pAsync));

    if (unlikely(!query->IsScoped()))
      return;

    auto entry = std::find(
      m_queriesBegun.begin(),
      m_queriesBegun.end(), query);

    if (unlikely(entry != m_queriesBegun.end()))
      return;

    EmitCs([cQuery = query]
    (DxvkContext* ctx) {
      cQuery->Begin(ctx);
    });

    m_queriesBegun.push_back(std::move(query));
  }


  void STDMETHODCALLTYPE D3D11DeferredContext::End(
          ID3D11Asynchronous*         pAsync) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(!pAsync))
      return;

    Com<D3D11Query, false> query(static_cast<D3D11Query*>(pAsync));

    if (query->IsScoped()) {
      auto entry = std::find(
        m_queriesBegun.begin(),
        m_queriesBegun.end(), query);

      if (likely(entry != m_queriesBegun.end())) {
        m_queriesBegun.erase(entry);
      } else {
        EmitCs([cQuery = query]
        (DxvkContext* ctx) {
          cQuery->Begin(ctx);
        });
      }
    }

    m_commandList->AddQuery(query.ptr());

    EmitCs([cQuery = std::move(query)]
    (DxvkContext* ctx) {
      cQuery->End(ctx);
    });
  }


  void STDMETHODCALLTYPE D3D11DeferredContext::Flush() {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D11: Flush called on a deferred context");
  }


  void STDMETHODCALLTYPE D3D11DeferredContext::Flush1(
          D3D11_CONTEXT_TYPE          ContextType,
          HANDLE                      hEvent) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D11: Flush1 called on a deferred context");
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::Signal(
          ID3D11Fence*                pFence,
          UINT64                      Value) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D11: Signal called on a deferred context");

    return DXGI_ERROR_INVALID_CALL;
  }


  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::Wait(
          ID3D11Fence*                pFence,
          UINT64                      Value) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D11: Wait called on a deferred context");

    return DXGI_ERROR_INVALID_CALL;
  }


  void STDMETHODCALLTYPE D3D11DeferredContext::ExecuteCommandList(
          ID3D11CommandList*  pCommandList,
          BOOL                RestoreContextState) {
    D3D10DeviceLock lock = LockContext();

    // Clear state so that the command list can't observe any current context
    // state. When the previous nested list left a retained tail,
    // ResetCommandListState consumes its exact saved footprint; this one
    // sweep is also the required isolation before the child below.
    if (!heliosClFastPath() || m_heliosCsState != D3D11HeliosCsState::Clean)
      ResetCommandListState(heliosClFastPath()
        ? D3D11CommandListResetMode::LogicalBindings
        : D3D11CommandListResetMode::Physical);

    auto commandList = static_cast<D3D11CommandList*>(pCommandList);

    // Helios: an empty fast-path list splices nothing (and AddCommandList's
    // tail arithmetic assumes non-empty lists). The recorded sweep above and
    // the state reset below still uphold the API's clear-state semantics.
    if (likely(!commandList->IsEmpty())) {
      // Flush any outstanding commands so that
      // we don't mess up the execution order
      FlushCsChunk();

      // Record any chunks from the given command list into the
      // current command list and deal with context state
      m_chunkId = m_commandList->AddCommandList(commandList);

      // Do not immediately materialize a fast child's cleanup. Its exact
      // final-tail footprint is carried by this parent list, recursively, so
      // the next Execute pre-reset or ordinary EmitCs can consume it in order.
      // This is vital after Execute(FALSE), which clears the CPU shadow while
      // the child's bindings remain live in this deferred CS stream.
      if (unlikely(!commandList->EndsClean()))
        SetHeliosClLeftover(commandList->GetUsedBindings());
      else {
        m_heliosCsState = D3D11HeliosCsState::Clean;
        m_heliosCsLeftoverBindings = { };
      }
    }

    // Restore deferred context state
    if (RestoreContextState)
      RestoreCommandListState();
    else
      ResetContextState(heliosClRetainSamplerRefs());
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::FinishCommandList(
          BOOL                RestoreDeferredContextState,
          ID3D11CommandList   **ppCommandList) {
    D3D10DeviceLock lock = LockContext();
    helios_feed::deferredFinish();

    // End all queries that were left active by the app
    FinalizeQueries();

    if (likely(heliosClFastPath())) {
      // Omit a trailing sweep only while this recorded stream actually ends
      // with unclean state. A nested child may be that final state even though
      // Execute(FALSE) has reset this context's CPU shadow, so copy its saved
      // footprint rather than taking a historical union of every child.
      const bool endsClean = m_heliosCsState == D3D11HeliosCsState::Clean;
      m_commandList->SetEndsClean(endsClean);

      if (!endsClean) {
        m_commandList->SetUsedBindings(
          m_heliosCsState == D3D11HeliosCsState::ClLeftover
            ? m_heliosCsLeftoverBindings
            : GetMaxUsedBindings());
      }
    } else {
      // Clean up command list state so that the any state changed
      // by this command list does not affect the calling context.
      // This also ensures that the command list is never empty.
      ResetCommandListState();
    }

    // Make sure all commands are visible to the command list
    FlushCsChunk();

    if (unlikely(heliosClStats())) {
      m_parent->NoteHeliosCommandListFinished(
        m_commandList->IsEmpty(),
        m_commandList->GetChunkCount());
    }
    
    if (ppCommandList)
      *ppCommandList = m_commandList.ref();

    // Create a clean command list, and if requested, restore all
    // previously set context state. Otherwise, reset the context.
    // Any use of ExecuteCommandList will reset command list state
    // before the command list is actually executed.
    m_commandList = CreateCommandList();
    m_chunkId = 0;
    // This is a new independent stream. Its eventual execution need not be
    // adjacent to the list just finished, so it cannot inherit that list's
    // clean tail.
    m_heliosCsState = D3D11HeliosCsState::Unknown;
    m_heliosCsLeftoverBindings = { };
    
    if (RestoreDeferredContextState)
      RestoreCommandListState();
    else if (m_heliosDdiLogicalResetEnabled && heliosDdiLogicalReset()
          && !heliosClRetainSamplerRefs())
      ResetHeliosDdiLogicalState();
    else
      ResetContextState(heliosClRetainSamplerRefs());
    
    m_mappedResources.clear();
    ResetStagingBuffer();
    return S_OK;
  }
  
  
  HRESULT STDMETHODCALLTYPE D3D11DeferredContext::Map(
          ID3D11Resource*             pResource,
          UINT                        Subresource,
          D3D11_MAP                   MapType,
          UINT                        MapFlags,
          D3D11_MAPPED_SUBRESOURCE*   pMappedResource) {
    D3D10DeviceLock lock = LockContext();

    if (unlikely(!pResource || !pMappedResource))
      return E_INVALIDARG;

    if (MapType == D3D11_MAP_WRITE_DISCARD)
      helios_feed::mapWriteDiscard();
    if (MapFlags & D3D11_MAP_FLAG_DO_NOT_WAIT)
      helios_feed::mapDoNotWait();
    
    if (likely(MapType == D3D11_MAP_WRITE_DISCARD)) {
      D3D11_RESOURCE_DIMENSION resourceDim;
      pResource->GetType(&resourceDim);

      return likely(resourceDim == D3D11_RESOURCE_DIMENSION_BUFFER)
        ? MapBuffer(pResource, pMappedResource)
        : MapImage(pResource, Subresource, pMappedResource);
    } else if (likely(MapType == D3D11_MAP_WRITE_NO_OVERWRITE)) {
      // The resource must be mapped with D3D11_MAP_WRITE_DISCARD
      // before it can be mapped with D3D11_MAP_WRITE_NO_OVERWRITE.
      D3D11_RESOURCE_DIMENSION resourceDim;
      pResource->GetType(&resourceDim);

      if (likely(resourceDim == D3D11_RESOURCE_DIMENSION_BUFFER)) {
        D3D11_MAPPED_SUBRESOURCE sr = FindMapEntry(static_cast<D3D11Buffer*>(pResource)->GetCookie());
        pMappedResource->pData = sr.pData;

        if (unlikely(!sr.pData))
          return D3D11_ERROR_DEFERRED_CONTEXT_MAP_WITHOUT_INITIAL_DISCARD;

        pMappedResource->RowPitch = sr.RowPitch;
        pMappedResource->DepthPitch = sr.DepthPitch;
        return S_OK;
      } else {
        // Images cannot be mapped with NO_OVERWRITE
        pMappedResource->pData = nullptr;
        return E_INVALIDARG;
      }
    } else {
      // Not allowed on deferred contexts
      pMappedResource->pData = nullptr;
      return E_INVALIDARG;
    }
  }
  
  
  void STDMETHODCALLTYPE D3D11DeferredContext::Unmap(
          ID3D11Resource*             pResource,
          UINT                        Subresource) {
    // No-op, updates are committed in Map
  }
  
  
  void STDMETHODCALLTYPE D3D11DeferredContext::SwapDeviceContextState(
          ID3DDeviceContextState*           pState,
          ID3DDeviceContextState**          ppPreviousState) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D11: SwapDeviceContextState called on a deferred context");
  }


  HRESULT D3D11DeferredContext::MapBuffer(
          ID3D11Resource*               pResource,
          D3D11_MAPPED_SUBRESOURCE*     pMappedResource) {
    D3D11Buffer* pBuffer = static_cast<D3D11Buffer*>(pResource);

    if (unlikely(pBuffer->GetMapMode() == D3D11_COMMON_BUFFER_MAP_MODE_NONE)) {
      Logger::err("D3D11: Cannot map a device-local buffer");
      pMappedResource->pData = nullptr;
      return E_INVALIDARG;
    }

    const bool exactOuterAllocation =
      pBuffer->GetBuffer()->info().heliosAssociation.outer_allocation_token != 0u;
    if (exactOuterAllocation) {
      auto bufferSlice = AllocStagingBuffer(pBuffer->Desc()->ByteWidth);
      pMappedResource->pData = bufferSlice.mapPtr(0);
      EmitCs([
        cDstBuffer = pBuffer->GetBuffer(),
        cSrcSlice  = std::move(bufferSlice)
      ] (DxvkContext* ctx) {
        ctx->copyBuffer(
          cDstBuffer, 0u,
          cSrcSlice.buffer(), cSrcSlice.offset(), cSrcSlice.length());
      });
    } else {
      auto bufferSlice = pBuffer->AllocSlice(&m_allocationCache);
      pMappedResource->pData = bufferSlice->mapPtr();
      EmitCs([
        cDstBuffer = pBuffer->GetBuffer(),
        cDstSlice  = std::move(bufferSlice)
      ] (DxvkContext* ctx) {
        ctx->invalidateBuffer(cDstBuffer, Rc<DxvkResourceAllocation>(cDstSlice));
      });
    }

    pMappedResource->RowPitch     = pBuffer->Desc()->ByteWidth;
    pMappedResource->DepthPitch   = pBuffer->Desc()->ByteWidth;

    AddMapEntry(pBuffer->GetCookie(), *pMappedResource);
    return S_OK;
  }
  
  
  HRESULT D3D11DeferredContext::MapImage(
          ID3D11Resource*               pResource,
          UINT                          Subresource,
          D3D11_MAPPED_SUBRESOURCE*     pMappedResource) {
    D3D11CommonTexture* pTexture = GetCommonTexture(pResource);
    
    if (unlikely(Subresource >= pTexture->CountSubresources())) {
      pMappedResource->pData = nullptr;
      return E_INVALIDARG;
    }

    if (unlikely(pTexture->Desc()->Usage != D3D11_USAGE_DYNAMIC)) {
      pMappedResource->pData = nullptr;
      return E_INVALIDARG;
    }

    VkFormat packedFormat = pTexture->GetPackedFormat();
    auto formatInfo = lookupFormatInfo(packedFormat);
    auto layout = pTexture->GetSubresourceLayout(formatInfo->aspectMask, Subresource);

    if (pTexture->GetMapMode() == D3D11_COMMON_TEXTURE_MAP_MODE_DIRECT
     && !pTexture->GetImage()->info().heliosAssociation.outer_allocation_token) {
      auto storage = pTexture->AllocStorage();
      auto mapPtr = storage->mapPtr();

      EmitCs([
        cImage = pTexture->GetImage(),
        cStorage = std::move(storage)
      ] (DxvkContext* ctx) {
        ctx->invalidateImage(cImage, Rc<DxvkResourceAllocation>(cStorage), VK_IMAGE_LAYOUT_PREINITIALIZED);
      });

      pMappedResource->RowPitch   = layout.RowPitch;
      pMappedResource->DepthPitch = layout.DepthPitch;
      pMappedResource->pData      = mapPtr;
      return S_OK;
    } else {
      auto dataSlice = AllocStagingBuffer(layout.Size);

      pMappedResource->RowPitch   = layout.RowPitch;
      pMappedResource->DepthPitch = layout.DepthPitch;
      pMappedResource->pData      = dataSlice.mapPtr(0);

      auto subresource = pTexture->GetSubresourceFromIndex(formatInfo->aspectMask, Subresource);
      auto mipExtent = pTexture->MipLevelExtent(subresource.mipLevel);

      UpdateImage(pTexture, &subresource,
        VkOffset3D { 0, 0, 0 }, mipExtent,
        std::move(dataSlice));

      return S_OK;
    }
  }
  
  
  void D3D11DeferredContext::UpdateMappedBuffer(
          D3D11Buffer*                  pDstBuffer,
          UINT                          Offset,
          UINT                          Length,
    const void*                         pSrcData,
          UINT                          CopyFlags) {
    void* mapPtr = nullptr;

    if (unlikely(CopyFlags == D3D11_COPY_NO_OVERWRITE))
      mapPtr = FindMapEntry(pDstBuffer->GetCookie()).pData;

    if (likely(!mapPtr)) {
      // The caller validates the map mode, so we can
      // safely ignore the MapBuffer return value here
      D3D11_MAPPED_SUBRESOURCE mapInfo;
      MapBuffer(pDstBuffer, &mapInfo);
      AddMapEntry(pDstBuffer->GetCookie(), mapInfo);
      mapPtr = mapInfo.pData;
    }

    std::memcpy(reinterpret_cast<char*>(mapPtr) + Offset, pSrcData, Length);
  }


  void D3D11DeferredContext::FinalizeQueries() {
    // Query-end commands do not modify D3D11 binding state. If this stream
    // already ended clean (or we first consume a retained child tail), retain
    // that stronger fact after the final query end; an Unknown pre-existing
    // tail remains Unknown conservatively.
    const bool preservesCleanTail =
      m_heliosCsState != D3D11HeliosCsState::Unknown;
    const bool hasQueries = !m_queriesBegun.empty();

    for (auto& query : m_queriesBegun) {
      m_commandList->AddQuery(query.ptr());

      EmitCs([cQuery = std::move(query)]
      (DxvkContext* ctx) {
        cQuery->End(ctx);
      });
    }

    m_queriesBegun.clear();

    if (hasQueries && preservesCleanTail)
      m_heliosCsState = D3D11HeliosCsState::Clean;
  }


  bool D3D11DeferredContext::RecycleCommandList(D3D11CommandList* pCommandList) {
    if (unlikely(!pCommandList
              || !pCommandList->IsReusableBy(this)
              || !heliosClRecycleCache())) {
      if (unlikely(heliosClRecycleStats()))
        m_heliosRecycleDropped += 1u;
      return false;
    }

    // This callback is serialized by the runtime for this deferred context.
    // Reset before taking the cache reference so chunks/queries/resources are
    // released immediately; the cache holds only the empty command-list
    // object and its vector capacity.
    if (unlikely(m_recycledCommandLists.size() >= MaxHeliosRecycledCommandLists)) {
      if (unlikely(heliosClRecycleStats()))
        m_heliosRecycleDropped += 1u;
      return false;
    }

    pCommandList->ResetForReuse();
    // Capacity was reserved during DC construction, so this performs no
    // allocation. attach moves the one IC hCL escrow reference directly into
    // the cache: no AddRef/Release pair on the shared command-list object.
    m_recycledCommandLists.push_back(Com<D3D11CommandList>::attach(pCommandList));

    if (unlikely(heliosClRecycleStats()))
      m_heliosRecycleAdmitted += 1u;
    return true;
  }


  Com<D3D11CommandList> D3D11DeferredContext::CreateCommandList() {
    if (likely(heliosClRecycleCache()) && !m_recycledCommandLists.empty()) {
      Com<D3D11CommandList> commandList = std::move(m_recycledCommandLists.back());
      m_recycledCommandLists.pop_back();

      // The handoff reset it, but repeat the narrow reset here so a future
      // cache producer cannot accidentally hand recording state back to us.
      commandList->ResetForReuse();

      if (unlikely(heliosClRecycleStats()))
        m_heliosRecycleHits += 1u;
      return commandList;
    }

    if (unlikely(heliosClRecycleStats()))
      m_heliosRecycleMisses += 1u;
    return new D3D11CommandList(m_parent, m_flags, this);
  }
  
  
  void D3D11DeferredContext::EmitCsChunk(DxvkCsChunkRef&& chunk) {
    m_chunkId = m_commandList->AddChunk(std::move(chunk), m_estimatedCost);
    m_estimatedCost = 0u;
  }


  uint64_t D3D11DeferredContext::GetCurrentChunkId() const {
    return m_csChunk->empty() ? m_chunkId : m_chunkId + 1;
  }


  void D3D11DeferredContext::TrackTextureSequenceNumber(
          D3D11CommonTexture*         pResource,
          UINT                        Subresource) {
    m_commandList->TrackResourceUsage(
      pResource->GetInterface(),
      pResource->GetDimension(),
      Subresource, GetCurrentChunkId());
  }


  void D3D11DeferredContext::TrackBufferSequenceNumber(
          D3D11Buffer*                pResource) {
    m_commandList->TrackResourceUsage(pResource,
      D3D11_RESOURCE_DIMENSION_BUFFER, 0,
      GetCurrentChunkId());
  }


  D3D11_MAPPED_SUBRESOURCE D3D11DeferredContext::FindMapEntry(
          uint64_t                      Cookie) {
    // Recently mapped resources as well as entries with
    // up-to-date map infos will be located at the end
    // of the resource array, so scan in reverse order.
    size_t size = m_mappedResources.size();

    for (size_t i = 1; i <= size; i++) {
      const auto& entry = m_mappedResources[size - i];

      if (entry.ResourceCookie == Cookie)
        return entry.MapInfo;
    }

    return D3D11_MAPPED_SUBRESOURCE();
  }

  void D3D11DeferredContext::AddMapEntry(
          uint64_t                      Cookie,
    const D3D11_MAPPED_SUBRESOURCE&     MapInfo) {
    m_mappedResources.push_back({ Cookie, MapInfo });
  }

}
