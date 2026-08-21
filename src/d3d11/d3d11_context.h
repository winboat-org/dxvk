#pragma once

#include <algorithm>
#include <cstdlib>
#include <type_traits>
#include <vector>

#include "../dxvk/dxvk_adapter.h"
#include "../dxvk/dxvk_cs.h"
#include "../dxvk/dxvk_device.h"
#include "../dxvk/dxvk_staging.h"

#include "../d3d10/d3d10_multithread.h"

#include "../util/util_flush.h"

#include "d3d11_annotation.h"
#include "d3d11_buffer.h"
#include "d3d11_cmd.h"
#include "d3d11_context_ext.h"
#include "d3d11_context_state.h"
#include "d3d11_device_child.h"
#include "d3d11_texture.h"

namespace dxvk {

  class D3D11DeferredContext;
  class D3D11ImmediateContext;

  // The retained sampler cache deliberately covers the complete D3D11
  // shader-stage/slot domain. Any future representation change must revisit
  // the cache rather than silently retaining only a prefix of state.
  static_assert(D3D11ShaderTypeCount == 6u);
  static_assert(D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT == 16u);

  /**
   * \brief Helios command-list fast-path kill switch
   *
   * Default ON. `HELIOS_DXVK_CL_FAST=0` restores stock behavior: every
   * FinishCommandList records a trailing reset sweep into the list and
   * every ExecuteCommandList emits its own reset sweep — the constant
   * per-command-list costs that swamp the single dxvk-cs consumer at the
   * D3D11 runtime's BUILD_2 granularity (Phase C measurement,
   * tmp/handoff-perf-structural/reports/p2-phase-c-outcome.md).
   */
  inline bool heliosClFastPath() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_FAST");
      return !(env && env[0] == '0');
    }();
    return enabled;
  }

  /**
   * \brief DDI-only deferred-context logical reset
   *
   * Default ON. This is separately marked by the Helios DDI bridge: public
   * DXVK deferred contexts retain stock reset and lifetime behaviour. Set
   * HELIOS_DXVK_DDI_LOGICAL_RESET=0 for a direct rollback.
   */
  inline bool heliosDdiLogicalReset() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_DDI_LOGICAL_RESET");
      return !(env && env[0] == '0');
    }();
    return heliosClFastPath() && enabled;
  }

  /**
   * \brief Helios command-list sampler-ref retention
   *
   * **Default ON since 2026-08-05.** It retains a private reference per-context
   * sampler slot across the BUILD_2 logical clear that follows
   * FinishCommandList(FALSE) or ExecuteCommandList(FALSE). The API-visible
   * state is still cleared; a matching later SetSamplers moves that retained
   * reference back without an atomic AddRef/Release. This is deliberately
   * subordinate to the command-list fast path, and is never used by ClearState
   * or an ordinary context-state reset.
   *
   * This is the single largest lever in the whole command-list path, and it
   * shipped OFF by mistake: sampler private-reference teardown — not command
   * lists in general — was the historical sub-50 fps failure mode. A
   * same-build, same-boot isolation sweep (2026-08-03) moved GT1 from
   * **53.609 to 181.938 fps** by setting only this variable
   * (`tmp/perf/gt1-18eb-000-r1.txt` vs `gt1-18eb-001-r1.txt`; analysis in
   * `tmp/handoff-perf-structural/reports/p3-227-recovery-outcome.md`). Every
   * accepted score since has run with it on, supplied by the test VM's
   * registry — so the code default disagreeing with it meant a fresh install
   * shipped the slow path.
   *
   * `HELIOS_DXVK_CL_RETAIN_SAMPLER_REFS=0` is the A/B disable.
   */
  inline bool heliosClRetainSamplerRefs() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_RETAIN_SAMPLER_REFS");
      return !(env && env[0] == '0');
    }();
    return heliosClFastPath() && enabled;
  }

  /**
   * \brief Helios command-list inline replay
   *
   * **Default ON since 2026-08-05.** With the fast path enabled, one-chunk
   * deferred command lists are replayed from the immediate context's open CS
   * chunk instead of creating one CS-queue entry per list. The replay's final
   * binding footprint is retained until the next execute boundary or ordinary
   * CS command, so this changes transport granularity without exposing list
   * state.
   *
   * It removes the remaining per-list CS-queue dispatch, which is the dominant
   * transport cost when the runtime hands us millions of tiny lists: in the
   * accepted 224.964 fps run, **1,830,697 of 1,841,743 executes were one-chunk
   * inline admissions** (`tmp/handoff-perf-structural/reports/p3-227-recovery-outcome.md`).
   * Like the sampler retention above, it was ON in every accepted run via the
   * test VM's registry while the code default said OFF.
   *
   * `HELIOS_DXVK_CL_INLINE_REPLAY=0` is the A/B disable.
   */
  inline bool heliosClInlineReplay() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_INLINE_REPLAY");
      return !(env && env[0] == '0');
    }();
    return heliosClFastPath() && enabled;
  }

  /**
   * \brief Helios inline-replay byte-accounting experiment
   *
   * Default OFF. `HELIOS_DXVK_CL_REPLAY_BYTE_ACCOUNTING=1` replaces the
   * inline replay wrapper-count batching policy with an exact occupied-byte
   * budget. It remains subordinate to inline replay and retains a separate
   * finite wrapper ceiling for pathologically tiny command lists.
   */
  inline bool heliosClReplayByteAccounting() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_REPLAY_BYTE_ACCOUNTING");
      return env && env[0] == '1' && env[1] == '\0';
    }();
    return heliosClInlineReplay() && enabled;
  }

  /**
   * \brief Helios D3D11 weak-flush ceiling experiment
   *
   * Default OFF. `HELIOS_DXVK_FLUSH_TRACKER_MAX64=1` raises only this
   * D3D11 context's weak/synchronizing pending-submission ceiling from 20 to
   * 64 logical chunks. The process-latched knob is intentionally independent
   * of command-list transport policy.
   */
  inline bool heliosFlushTrackerMax64() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_FLUSH_TRACKER_MAX64");
      return env && env[0] == '1' && env[1] == '\0';
    }();
    return enabled;
  }

  /**
   * \brief Helios D3D11 local-allocation-cache fallback experiment
   *
   * Default OFF. `HELIOS_DXVK_LOCAL_ALLOC_CACHE_FALLBACK=1` retries only an
   * unusable D3D11 local cache without DEVICE_LOCAL in its cache request.
   * The cache is selected once per process; buffer allocation properties and
   * the allocator's normal per-allocation fallback remain unchanged.
   */
  inline bool heliosLocalAllocCacheFallback() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_LOCAL_ALLOC_CACHE_FALLBACK");
      return env && env[0] == '1' && env[1] == '\0';
    }();
    return enabled;
  }

  /**
   * \brief Helios command-list bulk cleanup experiment
   *
   * Default ON. `HELIOS_DXVK_CL_BULK_RESET=0` restores the scalar cleanup
   * calls. The bulk form clears the same captured binding ranges and has the
   * same final DXVK state and descriptor/pipeline dirty masks; it only folds
   * repeated range-local dirty-bit updates into one operation.
   */
  inline bool heliosClBulkStateReset() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_BULK_RESET");
      return !(env && env[0] == '0');
    }();
    return enabled;
  }

  /**
   * \brief Helios DDI command-list recycle cache
   *
   * Default ON. `HELIOS_DXVK_CL_RECYCLE_CACHE=0` still follows the BUILD_2
   * recycle callback lifetime, but drops the retired command list at the
   * deferred-context handoff instead of retaining it for the next recording.
   * This is deliberately independent of the fast command-list path so a
   * cache A/B does not alter command-stream semantics.
   */
  inline bool heliosClRecycleCache() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_RECYCLE_CACHE");
      return !(env && env[0] == '0');
    }();
    return enabled;
  }

  /**
   * \brief Helios DDI command-list recycle-cache diagnostics
   *
   * Default OFF. `HELIOS_DXVK_CL_RECYCLE_STATS=1` logs per-deferred-context
   * cache totals at actual context destruction. The counters are ordinary
   * per-context integers and are never touched in a timed default run.
   */
  inline bool heliosClRecycleStats() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_RECYCLE_STATS");
      return env && env[0] != '0';
    }();
    return enabled;
  }

  /**
   * \brief Helios command-list diagnostic counters
   *
   * Default OFF. `HELIOS_DXVK_CL_STATS=1` enables bounded per-device
   * aggregate counters for a separate diagnostic run. Keeping this disabled
   * for performance A/Bs avoids cross-thread atomic cache-line traffic from
   * the runtime's millions of FinishCommandList calls.
   */
  inline bool heliosClStats() {
    static const bool enabled = [] {
      const char* env = std::getenv("HELIOS_DXVK_CL_STATS");
      return env && env[0] != '0';
    }();
    return enabled;
  }

  /**
   * \brief Helios: tail state of this context's emitted CS stream
   *
   * Tracks what the last CS-visible operation left the DXVK context's
   * binding state as, so redundant full reset sweeps can be elided at
   * command-list boundaries. Stream-relative: this describes the state the
   * dxvk-cs thread WILL be in once it consumes everything emitted so far,
   * regardless of how far it has actually gotten.
   */
  enum class D3D11HeliosCsState : uint32_t {
    /// Anything may be bound (normal immediate rendering)
    Unknown     = 0,
    /// The last CS-visible operation was a full reset sweep (or a command
    /// list ending in its recorded trailing sweep) and nothing has been
    /// emitted since — another sweep would be redundant
    Clean       = 1,
    /// A fast-path list left bindings live. The exact range that it touched
    /// is retained alongside this state until the next execute boundary or
    /// ordinary CS command consumes it.
    ClLeftover  = 2,
  };

  /**
   * \brief Command-list isolation reset intent
   *
   * Logical binding resets may retain bounded DXVK binding references behind
   * a new epoch. Physical resets are real application lifetime boundaries and
   * must release those references. Keeping the intent explicit prevents a
   * ClearState or context-state swap from accidentally taking the fast path.
   */
  enum class D3D11CommandListResetMode : uint32_t {
    Physical,
    LogicalBindings,
  };

  enum D3D11HeliosDdiLogicalScalar : uint16_t {
    D3D11HeliosDdiIaLayout  = 1u << 0,
    D3D11HeliosDdiIaIndex   = 1u << 1,
    D3D11HeliosDdiOmDsv     = 1u << 2,
    D3D11HeliosDdiOmBlend   = 1u << 3,
    D3D11HeliosDdiOmDepth   = 1u << 4,
    D3D11HeliosDdiRsState   = 1u << 5,
    D3D11HeliosDdiPredicate = 1u << 6,
  };

  template<bool IsDeferred>
  struct D3D11ContextObjectForwarder;

  /**
   * \brief Object forwarder for immediate contexts
   *
   * Binding methods can use this to efficiently bind objects
   * to the DXVK context without redundant reference counting.
   */
  template<>
  struct D3D11ContextObjectForwarder<false> {
    template<typename T>
    static T&& move(T& object) {
      return std::move(object);
    }
  };

  /**
   * \brief Object forwarder for deferred contexts
   *
   * This forwarder will create a copy of the object passed
   * into it, so that CS chunks can be reused if necessary.
   */
  template<>
  struct D3D11ContextObjectForwarder<true> {
    template<typename T>
    static T move(const T& object) {
      return object;
    }
  };

  /**
   * \brief Common D3D11 device context implementation
   *
   * Implements all common device context methods, but since this is
   * templates with the actual context type (deferred or immediate),
   * all methods can call back into context-specific methods without
   * having to use virtual methods.
   */
  template<typename ContextType>
  class D3D11CommonContext : public D3D11DeviceChild<ID3D11DeviceContext4> {
    constexpr static bool IsDeferred = std::is_same_v<ContextType, D3D11DeferredContext>;
    using Forwarder = D3D11ContextObjectForwarder<IsDeferred>;

    template<typename T> friend class D3D11DeviceContextExt;
    template<typename T> friend class D3D11UserDefinedAnnotation;

    // Use a local staging buffer to handle tiny uploads, most
    // of the time we're fine with hitting the global allocator
    constexpr static VkDeviceSize StagingBufferSize = 256ull << 10;
  protected:
    // Compile-time debug flag to force lazy binding on (True) or off (False)
    constexpr static Tristate DebugLazyBinding = Tristate::Auto;
  public:
    
    D3D11CommonContext(
            D3D11Device*            pParent,
      const Rc<DxvkDevice>&         Device,
            UINT                    ContextFlags,
            DxvkCsChunkFlags        CsFlags);

    ~D3D11CommonContext();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID  riid,
            void**  ppvObject);

    D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE GetType();

    UINT STDMETHODCALLTYPE GetContextFlags();
    
    void STDMETHODCALLTYPE ClearState();

    void STDMETHODCALLTYPE DiscardResource(ID3D11Resource *pResource);

    void STDMETHODCALLTYPE DiscardView(ID3D11View* pResourceView);

    void STDMETHODCALLTYPE DiscardView1(
            ID3D11View*                      pResourceView,
      const D3D11_RECT*                      pRects,
            UINT                             NumRects);

    void STDMETHODCALLTYPE DiscardViewBase(
            ID3D11View*                      pResourceView,
      const D3D11_RECT*                      pRects,
            UINT                             NumRects);

    void STDMETHODCALLTYPE CopySubresourceRegion(
            ID3D11Resource*                   pDstResource,
            UINT                              DstSubresource,
            UINT                              DstX,
            UINT                              DstY,
            UINT                              DstZ,
            ID3D11Resource*                   pSrcResource,
            UINT                              SrcSubresource,
      const D3D11_BOX*                        pSrcBox);

    void STDMETHODCALLTYPE CopySubresourceRegion1(
            ID3D11Resource*                   pDstResource,
            UINT                              DstSubresource,
            UINT                              DstX,
            UINT                              DstY,
            UINT                              DstZ,
            ID3D11Resource*                   pSrcResource,
            UINT                              SrcSubresource,
      const D3D11_BOX*                        pSrcBox,
            UINT                              CopyFlags);

    void STDMETHODCALLTYPE CopySubresourceRegionBase(
            ID3D11Resource*                   pDstResource,
            UINT                              DstSubresource,
            UINT                              DstX,
            UINT                              DstY,
            UINT                              DstZ,
            ID3D11Resource*                   pSrcResource,
            UINT                              SrcSubresource,
      const D3D11_BOX*                        pSrcBox,
            UINT                              CopyFlags);

    void STDMETHODCALLTYPE CopyResource(
            ID3D11Resource*                   pDstResource,
            ID3D11Resource*                   pSrcResource);

    void STDMETHODCALLTYPE CopyStructureCount(
            ID3D11Buffer*                     pDstBuffer,
            UINT                              DstAlignedByteOffset,
            ID3D11UnorderedAccessView*        pSrcView);

    void STDMETHODCALLTYPE ClearRenderTargetView(
            ID3D11RenderTargetView*           pRenderTargetView,
      const FLOAT                             ColorRGBA[4]);

    void STDMETHODCALLTYPE ClearUnorderedAccessViewUint(
            ID3D11UnorderedAccessView*        pUnorderedAccessView,
      const UINT                              Values[4]);

    void STDMETHODCALLTYPE ClearUnorderedAccessViewFloat(
            ID3D11UnorderedAccessView*        pUnorderedAccessView,
      const FLOAT                             Values[4]);

    void STDMETHODCALLTYPE ClearDepthStencilView(
            ID3D11DepthStencilView*           pDepthStencilView,
            UINT                              ClearFlags,
            FLOAT                             Depth,
            UINT8                             Stencil);

    void STDMETHODCALLTYPE ClearView(
            ID3D11View                        *pView,
      const FLOAT                             Color[4],
      const D3D11_RECT                        *pRect,
            UINT                              NumRects);

    void STDMETHODCALLTYPE GenerateMips(
            ID3D11ShaderResourceView*         pShaderResourceView);

    void STDMETHODCALLTYPE ResolveSubresource(
            ID3D11Resource*                   pDstResource,
            UINT                              DstSubresource,
            ID3D11Resource*                   pSrcResource,
            UINT                              SrcSubresource,
            DXGI_FORMAT                       Format);

    void STDMETHODCALLTYPE UpdateSubresource(
            ID3D11Resource*                   pDstResource,
            UINT                              DstSubresource,
      const D3D11_BOX*                        pDstBox,
      const void*                             pSrcData,
            UINT                              SrcRowPitch,
            UINT                              SrcDepthPitch);

    void STDMETHODCALLTYPE UpdateSubresource1(
            ID3D11Resource*                   pDstResource,
            UINT                              DstSubresource,
      const D3D11_BOX*                        pDstBox,
      const void*                             pSrcData,
            UINT                              SrcRowPitch,
            UINT                              SrcDepthPitch,
            UINT                              CopyFlags);

    void STDMETHODCALLTYPE DrawAuto();

    void STDMETHODCALLTYPE Draw(
            UINT            VertexCount,
            UINT            StartVertexLocation);

    void STDMETHODCALLTYPE DrawIndexed(
            UINT            IndexCount,
            UINT            StartIndexLocation,
            INT             BaseVertexLocation);

    void STDMETHODCALLTYPE DrawInstanced(
            UINT            VertexCountPerInstance,
            UINT            InstanceCount,
            UINT            StartVertexLocation,
            UINT            StartInstanceLocation);

    void STDMETHODCALLTYPE DrawIndexedInstanced(
            UINT            IndexCountPerInstance,
            UINT            InstanceCount,
            UINT            StartIndexLocation,
            INT             BaseVertexLocation,
            UINT            StartInstanceLocation);

    void STDMETHODCALLTYPE DrawIndexedInstancedIndirect(
            ID3D11Buffer*   pBufferForArgs,
            UINT            AlignedByteOffsetForArgs);

    void STDMETHODCALLTYPE DrawInstancedIndirect(
            ID3D11Buffer*   pBufferForArgs,
            UINT            AlignedByteOffsetForArgs);

    void STDMETHODCALLTYPE Dispatch(
            UINT            ThreadGroupCountX,
            UINT            ThreadGroupCountY,
            UINT            ThreadGroupCountZ);

    void STDMETHODCALLTYPE DispatchIndirect(
            ID3D11Buffer*   pBufferForArgs,
            UINT            AlignedByteOffsetForArgs);

    void STDMETHODCALLTYPE IASetInputLayout(
            ID3D11InputLayout*                pInputLayout);

    void STDMETHODCALLTYPE IASetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY          Topology);

    void STDMETHODCALLTYPE IASetVertexBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppVertexBuffers,
      const UINT*                             pStrides,
      const UINT*                             pOffsets);

    void STDMETHODCALLTYPE IASetIndexBuffer(
            ID3D11Buffer*                     pIndexBuffer,
            DXGI_FORMAT                       Format,
            UINT                              Offset);

    void STDMETHODCALLTYPE IAGetInputLayout(
            ID3D11InputLayout**               ppInputLayout);

    void STDMETHODCALLTYPE IAGetPrimitiveTopology(
            D3D11_PRIMITIVE_TOPOLOGY*         pTopology);

    void STDMETHODCALLTYPE IAGetVertexBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppVertexBuffers,
            UINT*                             pStrides,
            UINT*                             pOffsets);

    void STDMETHODCALLTYPE IAGetIndexBuffer(
            ID3D11Buffer**                    ppIndexBuffer,
            DXGI_FORMAT*                      pFormat,
            UINT*                             pOffset);

    void STDMETHODCALLTYPE VSSetShader(
            ID3D11VertexShader*               pVertexShader,
            ID3D11ClassInstance* const*       ppClassInstances,
            UINT                              NumClassInstances);

    void STDMETHODCALLTYPE VSSetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers);

     void STDMETHODCALLTYPE VSSetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers,
      const UINT*                             pFirstConstant,
      const UINT*                             pNumConstants);

    void STDMETHODCALLTYPE VSSetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView* const*  ppShaderResourceViews);

    void STDMETHODCALLTYPE VSSetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState* const*        ppSamplers);

    void STDMETHODCALLTYPE VSGetShader(
            ID3D11VertexShader**              ppVertexShader,
            ID3D11ClassInstance**             ppClassInstances,
            UINT*                             pNumClassInstances);

    void STDMETHODCALLTYPE VSGetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers);

    void STDMETHODCALLTYPE VSGetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers,
            UINT*                             pFirstConstant,
            UINT*                             pNumConstants);

    void STDMETHODCALLTYPE VSGetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView**        ppShaderResourceViews);

    void STDMETHODCALLTYPE VSGetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState**              ppSamplers);

    void STDMETHODCALLTYPE HSSetShader(
            ID3D11HullShader*                 pHullShader,
            ID3D11ClassInstance* const*       ppClassInstances,
            UINT                              NumClassInstances);

    void STDMETHODCALLTYPE HSSetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers);

    void STDMETHODCALLTYPE HSSetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers,
      const UINT*                             pFirstConstant,
      const UINT*                             pNumConstants);

    void STDMETHODCALLTYPE HSSetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView* const*  ppShaderResourceViews);

    void STDMETHODCALLTYPE HSSetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState* const*        ppSamplers);

    void STDMETHODCALLTYPE HSGetShader(
            ID3D11HullShader**                ppHullShader,
            ID3D11ClassInstance**             ppClassInstances,
            UINT*                             pNumClassInstances);

    void STDMETHODCALLTYPE HSGetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers);

     void STDMETHODCALLTYPE HSGetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers,
            UINT*                             pFirstConstant,
            UINT*                             pNumConstants);

    void STDMETHODCALLTYPE HSGetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView**        ppShaderResourceViews);

    void STDMETHODCALLTYPE HSGetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState**              ppSamplers);

    void STDMETHODCALLTYPE DSSetShader(
            ID3D11DomainShader*               pDomainShader,
            ID3D11ClassInstance* const*       ppClassInstances,
            UINT                              NumClassInstances);

    void STDMETHODCALLTYPE DSSetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers);

    void STDMETHODCALLTYPE DSSetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers,
      const UINT*                             pFirstConstant,
      const UINT*                             pNumConstants);

    void STDMETHODCALLTYPE DSSetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView* const*  ppShaderResourceViews);

    void STDMETHODCALLTYPE DSSetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState* const*        ppSamplers);

    void STDMETHODCALLTYPE DSGetShader(
            ID3D11DomainShader**              ppDomainShader,
            ID3D11ClassInstance**             ppClassInstances,
            UINT*                             pNumClassInstances);

    void STDMETHODCALLTYPE DSGetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers);

     void STDMETHODCALLTYPE DSGetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers,
            UINT*                             pFirstConstant,
            UINT*                             pNumConstants);

    void STDMETHODCALLTYPE DSGetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView**        ppShaderResourceViews);

    void STDMETHODCALLTYPE DSGetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState**              ppSamplers);

    void STDMETHODCALLTYPE GSSetShader(
            ID3D11GeometryShader*             pShader,
            ID3D11ClassInstance* const*       ppClassInstances,
            UINT                              NumClassInstances);

    void STDMETHODCALLTYPE GSSetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers);

    void STDMETHODCALLTYPE GSSetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers,
      const UINT*                             pFirstConstant,
      const UINT*                             pNumConstants);

    void STDMETHODCALLTYPE GSSetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView* const*  ppShaderResourceViews);

    void STDMETHODCALLTYPE GSSetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState* const*        ppSamplers);

    void STDMETHODCALLTYPE GSGetShader(
            ID3D11GeometryShader**            ppGeometryShader,
            ID3D11ClassInstance**             ppClassInstances,
            UINT*                             pNumClassInstances);

    void STDMETHODCALLTYPE GSGetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers);

     void STDMETHODCALLTYPE GSGetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers,
            UINT*                             pFirstConstant,
            UINT*                             pNumConstants);

    void STDMETHODCALLTYPE GSGetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView**        ppShaderResourceViews);

    void STDMETHODCALLTYPE GSGetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState**              ppSamplers);

    void STDMETHODCALLTYPE PSSetShader(
            ID3D11PixelShader*                pPixelShader,
            ID3D11ClassInstance* const*       ppClassInstances,
            UINT                              NumClassInstances);

    void STDMETHODCALLTYPE PSSetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers);

    void STDMETHODCALLTYPE PSSetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers,
      const UINT*                             pFirstConstant,
      const UINT*                             pNumConstants);

    void STDMETHODCALLTYPE PSSetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView* const*  ppShaderResourceViews);

    void STDMETHODCALLTYPE PSSetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState* const*        ppSamplers);

    void STDMETHODCALLTYPE PSGetShader(
            ID3D11PixelShader**               ppPixelShader,
            ID3D11ClassInstance**             ppClassInstances,
            UINT*                             pNumClassInstances);

    void STDMETHODCALLTYPE PSGetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers);

    void STDMETHODCALLTYPE PSGetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers,
            UINT*                             pFirstConstant,
            UINT*                             pNumConstants);

    void STDMETHODCALLTYPE PSGetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView**        ppShaderResourceViews);

    void STDMETHODCALLTYPE PSGetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState**              ppSamplers);

    void STDMETHODCALLTYPE CSSetShader(
            ID3D11ComputeShader*              pComputeShader,
            ID3D11ClassInstance* const*       ppClassInstances,
            UINT                              NumClassInstances);

    void STDMETHODCALLTYPE CSSetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers);

    void STDMETHODCALLTYPE CSSetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers,
      const UINT*                             pFirstConstant,
      const UINT*                             pNumConstants);

    void STDMETHODCALLTYPE CSSetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView* const*  ppShaderResourceViews);

    void STDMETHODCALLTYPE CSSetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState* const*        ppSamplers);

    void STDMETHODCALLTYPE CSSetUnorderedAccessViews(
            UINT                              StartSlot,
            UINT                              NumUAVs,
            ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
      const UINT*                             pUAVInitialCounts);

    void STDMETHODCALLTYPE CSGetShader(
            ID3D11ComputeShader**             ppComputeShader,
            ID3D11ClassInstance**             ppClassInstances,
            UINT*                             pNumClassInstances);

    void STDMETHODCALLTYPE CSGetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers);

    void STDMETHODCALLTYPE CSGetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers,
            UINT*                             pFirstConstant,
            UINT*                             pNumConstants);

    void STDMETHODCALLTYPE CSGetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView**        ppShaderResourceViews);

    void STDMETHODCALLTYPE CSGetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState**              ppSamplers);

    void STDMETHODCALLTYPE CSGetUnorderedAccessViews(
            UINT                              StartSlot,
            UINT                              NumUAVs,
            ID3D11UnorderedAccessView**       ppUnorderedAccessViews);

    void STDMETHODCALLTYPE OMSetRenderTargets(
            UINT                              NumViews,
            ID3D11RenderTargetView* const*    ppRenderTargetViews,
            ID3D11DepthStencilView*           pDepthStencilView);

    void STDMETHODCALLTYPE OMSetRenderTargetsAndUnorderedAccessViews(
            UINT                              NumRTVs,
            ID3D11RenderTargetView* const*    ppRenderTargetViews,
            ID3D11DepthStencilView*           pDepthStencilView,
            UINT                              UAVStartSlot,
            UINT                              NumUAVs,
            ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
      const UINT*                             pUAVInitialCounts);

    void STDMETHODCALLTYPE OMSetBlendState(
            ID3D11BlendState*                 pBlendState,
      const FLOAT                             BlendFactor[4],
            UINT                              SampleMask);

    void STDMETHODCALLTYPE OMSetDepthStencilState(
            ID3D11DepthStencilState*          pDepthStencilState,
            UINT                              StencilRef);

    void STDMETHODCALLTYPE OMGetRenderTargets(
            UINT                              NumViews,
            ID3D11RenderTargetView**          ppRenderTargetViews,
            ID3D11DepthStencilView**          ppDepthStencilView);

    void STDMETHODCALLTYPE OMGetRenderTargetsAndUnorderedAccessViews(
            UINT                              NumRTVs,
            ID3D11RenderTargetView**          ppRenderTargetViews,
            ID3D11DepthStencilView**          ppDepthStencilView,
            UINT                              UAVStartSlot,
            UINT                              NumUAVs,
            ID3D11UnorderedAccessView**       ppUnorderedAccessViews);

    void STDMETHODCALLTYPE OMGetBlendState(
            ID3D11BlendState**                ppBlendState,
            FLOAT                             BlendFactor[4],
            UINT*                             pSampleMask);

    void STDMETHODCALLTYPE OMGetDepthStencilState(
            ID3D11DepthStencilState**         ppDepthStencilState,
            UINT*                             pStencilRef);

    void STDMETHODCALLTYPE RSSetState(
            ID3D11RasterizerState*            pRasterizerState);

    void STDMETHODCALLTYPE RSSetViewports(
            UINT                              NumViewports,
      const D3D11_VIEWPORT*                   pViewports);

    void STDMETHODCALLTYPE RSSetScissorRects(
            UINT                              NumRects,
      const D3D11_RECT*                       pRects);

    void STDMETHODCALLTYPE RSGetState(
            ID3D11RasterizerState**           ppRasterizerState);

    void STDMETHODCALLTYPE RSGetViewports(
            UINT*                             pNumViewports,
            D3D11_VIEWPORT*                   pViewports);

    void STDMETHODCALLTYPE RSGetScissorRects(
            UINT*                             pNumRects,
            D3D11_RECT*                       pRects);

    void STDMETHODCALLTYPE SOSetTargets(
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppSOTargets,
      const UINT*                             pOffsets);

    void STDMETHODCALLTYPE SOGetTargets(
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppSOTargets);

    void STDMETHODCALLTYPE SOGetTargetsWithOffsets(
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppSOTargets,
            UINT*                             pOffsets);

    void STDMETHODCALLTYPE SetPredication(
            ID3D11Predicate*                  pPredicate,
            BOOL                              PredicateValue);

    void STDMETHODCALLTYPE GetPredication(
            ID3D11Predicate**                 ppPredicate,
            BOOL*                             pPredicateValue);

    void STDMETHODCALLTYPE SetResourceMinLOD(
            ID3D11Resource*                   pResource,
            FLOAT                             MinLOD);

    FLOAT STDMETHODCALLTYPE GetResourceMinLOD(
            ID3D11Resource*                   pResource);

    void STDMETHODCALLTYPE CopyTiles(
            ID3D11Resource*                   pTiledResource,
      const D3D11_TILED_RESOURCE_COORDINATE*  pTileRegionStartCoordinate,
      const D3D11_TILE_REGION_SIZE*           pTileRegionSize,
            ID3D11Buffer*                     pBuffer,
            UINT64                            BufferStartOffsetInBytes,
            UINT                              Flags);

    HRESULT STDMETHODCALLTYPE CopyTileMappings(
            ID3D11Resource*                   pDestTiledResource,
      const D3D11_TILED_RESOURCE_COORDINATE*  pDestRegionCoordinate,
            ID3D11Resource*                   pSourceTiledResource,
      const D3D11_TILED_RESOURCE_COORDINATE*  pSourceRegionCoordinate,
      const D3D11_TILE_REGION_SIZE*           pTileRegionSize,
            UINT                              Flags);

    HRESULT STDMETHODCALLTYPE ResizeTilePool(
            ID3D11Buffer*                     pTilePool,
            UINT64                            NewSizeInBytes);

    void STDMETHODCALLTYPE TiledResourceBarrier(
            ID3D11DeviceChild*                pTiledResourceOrViewAccessBeforeBarrier,
            ID3D11DeviceChild*                pTiledResourceOrViewAccessAfterBarrier);

    HRESULT STDMETHODCALLTYPE UpdateTileMappings(
            ID3D11Resource*                   pTiledResource,
            UINT                              NumRegions,
      const D3D11_TILED_RESOURCE_COORDINATE*  pRegionCoordinates,
      const D3D11_TILE_REGION_SIZE*           pRegionSizes,
            ID3D11Buffer*                     pTilePool,
            UINT                              NumRanges,
      const UINT*                             pRangeFlags,
      const UINT*                             pRangeTileOffsets,
      const UINT*                             pRangeTileCounts,
            UINT                              Flags);

    void STDMETHODCALLTYPE UpdateTiles(
            ID3D11Resource*                   pDestTiledResource,
      const D3D11_TILED_RESOURCE_COORDINATE*  pDestTileRegionStartCoordinate,
      const D3D11_TILE_REGION_SIZE*           pDestTileRegionSize,
      const void*                             pSourceTileData,
            UINT                              Flags);

    BOOL STDMETHODCALLTYPE IsAnnotationEnabled();

    void STDMETHODCALLTYPE SetMarkerInt(
            LPCWSTR                           pLabel,
            INT                               Data);

    void STDMETHODCALLTYPE BeginEventInt(
            LPCWSTR                           pLabel,
            INT                               Data);

    void STDMETHODCALLTYPE EndEvent();

    void STDMETHODCALLTYPE GetHardwareProtectionState(
            BOOL*                             pHwProtectionEnable);

    void STDMETHODCALLTYPE SetHardwareProtectionState(
            BOOL                              HwProtectionEnable);

    void STDMETHODCALLTYPE TransitionSurfaceLayout(
            IDXGIVkInteropSurface*    pSurface,
      const VkImageSubresourceRange*  pSubresources,
            VkImageLayout             OldLayout,
            VkImageLayout             NewLayout);

  protected:

    D3D11DeviceContextExt<ContextType>        m_contextExt;
    D3D11UserDefinedAnnotation<ContextType>   m_annotation;

    Rc<DxvkDevice>              m_device;

    D3D11ContextState           m_state;

    // Enabled only through the UMD bridge for its private DDI deferred
    // contexts. Active means m_state contains bounded retained ownership from
    // earlier logical clears, while m_heliosDdiValidity marks current slots.
    bool                        m_heliosDdiLogicalResetEnabled = false;
    bool                        m_heliosDdiLogicalStateActive = false;
    D3D11HeliosDdiLogicalState  m_heliosDdiValidity;

    // Private references retained only by the opt-in BUILD_2 logical-reset
    // path. Reusing D3D11SamplerBindings gives this cache exactly the active
    // state's six-stage, 16-slot shape; it is never consulted by GetSamplers.
    D3D11SamplerBindings        m_heliosRetainedSamplers;
    UINT                        m_flags;

    DxvkStagingBuffer           m_staging;

    D3D11CmdType                m_csDataType = D3D11CmdType::None;

    DxvkCsChunkFlags            m_csFlags;
    DxvkCsChunkRef              m_csChunk;
    DxvkCsDataBlock*            m_csData = nullptr;

    uint64_t                    m_estimatedCost = 0u;

    // Helios: CS-stream tail state for reset-sweep elision. Both immediate
    // and deferred contexts record command-list boundaries, so each tracks
    // the tail of its own stream independently. When the tail is
    // ClLeftover, CPU D3D11 state may already have been logically cleared by
    // ExecuteCommandList(FALSE), so m_state cannot reconstruct its footprint.
    D3D11HeliosCsState          m_heliosCsState = D3D11HeliosCsState::Unknown;
    D3D11MaxUsedBindings         m_heliosCsLeftoverBindings = { };

    DxvkLocalAllocationCache    m_allocationCache;

    D3D11ShaderStageState<Rc<DxvkBuffer>> m_instanceData;

    DxvkCsChunkRef AllocCsChunk();
    
    DxvkBufferSlice AllocStagingBuffer(
            VkDeviceSize                      Size);

    void ApplyDirtyConstantBuffers(
            D3D11ShaderType                   Stage,
      const D3D11BindingMask&                 BoundMask,
            D3D11BindingMask&                 DirtyMask);

    void ApplyDirtySamplers(
            D3D11ShaderType                   Stage,
      const D3D11BindingMask&                 BoundMask,
            D3D11BindingMask&                 DirtyMask);

    void ApplyDirtyShaderResources(
            D3D11ShaderType                   Stage,
      const D3D11BindingMask&                 BoundMask,
            D3D11BindingMask&                 DirtyMask);

    void ApplyDirtyUnorderedAccessViews(
            D3D11ShaderType                   Stage,
      const D3D11BindingMask&                 BoundMask,
            D3D11BindingMask&                 DirtyMask);

    void ApplyDirtyGraphicsBindings();

    void ApplyDirtyComputeBindings();

    void ApplyInputLayout();
    
    void ApplyPrimitiveTopology();
    
    void ApplyBlendState();
    
    void ApplyBlendFactor();
    
    void ApplyDepthStencilState();
    
    void ApplyStencilRef();
    
    void ApplyRasterizerState();
    
    void ApplyRasterizerSampleCount();

    void ApplyViewportState();

    void BatchDraw(
      const VkDrawIndirectCommand&            draw);

    void BatchDrawIndexed(
      const VkDrawIndexedIndirectCommand&     draw);

    template<D3D11ShaderType ShaderStage>
    void BindShader(
      const D3D11CommonShader*                pShaderModule);

    void BindFramebuffer();

    void BindDrawBuffers(
            D3D11Buffer*                      pBufferForArgs,
            D3D11Buffer*                      pBufferForCount);

    void BindVertexBuffer(
            UINT                              Slot,
            D3D11Buffer*                      pBuffer,
            UINT                              Offset,
            UINT                              Stride);

    void BindVertexBufferRange(
            UINT                              Slot,
            D3D11Buffer*                      pBuffer,
            UINT                              Offset,
            UINT                              Stride);

    void BindIndexBuffer(
            D3D11Buffer*                      pBuffer,
            UINT                              Offset,
            DXGI_FORMAT                       Format);

    void BindIndexBufferRange(
            D3D11Buffer*                      pBuffer,
            UINT                              Offset,
            DXGI_FORMAT                       Format);

    void BindXfbBuffer(
            UINT                              Slot,
            D3D11Buffer*                      pBuffer,
            UINT                              Offset);

    void BindConstantBuffer(
            D3D11ShaderType                   ShaderStage,
            UINT                              Slot,
            D3D11Buffer*                      pBuffer,
            UINT                              Offset,
            UINT                              Length);

    void BindConstantBufferRange(
            D3D11ShaderType                   ShaderStage,
            UINT                              Slot,
            UINT                              Offset,
            UINT                              Length);

    void BindSampler(
            D3D11ShaderType                   ShaderStage,
            UINT                              Slot,
            D3D11SamplerState*                pSampler);

    void BindShaderResource(
            D3D11ShaderType                   ShaderStage,
            UINT                              Slot,
            D3D11ShaderResourceView*          pResource);

    void BindUnorderedAccessView(
            D3D11ShaderType                   ShaderStage,
            UINT                              Slot,
            D3D11UnorderedAccessView*         pUav);

    void ClearImageView(
            Rc<DxvkImageView>                 View,
      const FLOAT                             Color[4],
      const D3D11_RECT*                       pRects,
            UINT                              NumRects);

    void ClearBufferView(
            Rc<DxvkBufferView>                View,
      const FLOAT                             Color[4],
      const D3D11_RECT*                       pRects,
            UINT                              NumRects);

    VkClearValue ConvertColorValue(
      const FLOAT                             Color[4],
      const DxvkFormatInfo*                   pFormatInfo);

    VkRect2D ConvertRect(
            D3D11_RECT                        Rect,
            VkExtent2D                        Extent);

    void CopyBuffer(
            D3D11Buffer*                      pDstBuffer,
            VkDeviceSize                      DstOffset,
            D3D11Buffer*                      pSrcBuffer,
            VkDeviceSize                      SrcOffset,
            VkDeviceSize                      ByteCount);

    void CopyImage(
            D3D11CommonTexture*               pDstTexture,
      const VkImageSubresourceLayers*         pDstLayers,
            VkOffset3D                        DstOffset,
            D3D11CommonTexture*               pSrcTexture,
      const VkImageSubresourceLayers*         pSrcLayers,
            VkOffset3D                        SrcOffset,
            VkExtent3D                        SrcExtent);

    void CopyTiledResourceData(
            ID3D11Resource*                   pResource,
      const D3D11_TILED_RESOURCE_COORDINATE*  pRegionCoordinate,
      const D3D11_TILE_REGION_SIZE*           pRegionSize,
            DxvkBufferSlice                   BufferSlice,
            UINT                              Flags);

    template<typename T>
    bool DirtyBindingGeneric(
            D3D11ShaderType                   ShaderStage,
            T                                 BoundMask,
            T&                                DirtyMask,
            T                                 DirtyBit,
            bool                              IsNull);

    bool DirtyConstantBuffer(
            D3D11ShaderType                   ShaderStage,
            uint32_t                          Slot,
            bool                              IsNull);

    bool DirtySampler(
            D3D11ShaderType                   ShaderStage,
            uint32_t                          Slot,
            bool                              IsNull);

    bool DirtyShaderResource(
            D3D11ShaderType                   ShaderStage,
            uint32_t                          Slot,
            bool                              IsNull);

    bool DirtyComputeUnorderedAccessView(
            uint32_t                          Slot,
            bool                              IsNull);

    bool DirtyGraphicsUnorderedAccessView(
            uint32_t                          Slot);

    void DiscardBuffer(
            ID3D11Resource*                   pResource);

    void DiscardTexture(
            ID3D11Resource*                   pResource,
            UINT                              Subresource);

    template<D3D11ShaderType ShaderStage>
    void GetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer**                    ppConstantBuffers,
            UINT*                             pFirstConstant,
            UINT*                             pNumConstants);

    template<D3D11ShaderType ShaderStage>
    void GetShaderResources(
            UINT                              StartSlot,
            UINT                              NumViews,
            ID3D11ShaderResourceView**        ppShaderResourceViews);

    template<D3D11ShaderType ShaderStage>
    void GetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState**              ppSamplers);

    DxvkGlobalPipelineBarrier GetTiledResourceDependency(
            ID3D11DeviceChild*                pObject);

    D3D11MaxUsedBindings GetMaxUsedBindings();

    bool HasDirtyComputeBindings();

    bool HasDirtyGraphicsBindings();

    void ResetCommandListState(
      D3D11CommandListResetMode Mode = D3D11CommandListResetMode::Physical);

    /**
     * \brief Consumes a command-list tail before another CS-visible command
     *
     * The retained footprint is exact for the command list that physically
     * precedes this command. It must be swept before an ordinary command can
     * observe its bindings. The template preserves EmitCs<false>'s no-flush
     * contract when the sweep is inserted by that path.
     */
    void HeliosNoteCsEmit(bool AllowFlush);

    void SetHeliosClLeftover(const D3D11MaxUsedBindings& UsedBindings) {
      m_heliosCsLeftoverBindings = UsedBindings;
      m_heliosCsState = D3D11HeliosCsState::ClLeftover;
    }

    void EmitCommandListStateReset(
      const D3D11MaxUsedBindings&       UsedBindings,
            D3D11CommandListResetMode    Mode,
            bool                         AllowFlush = true);

    // RetainSamplerRefs is valid only for the explicitly selected
    // ExecuteCommandList(FALSE)/FinishCommandList(FALSE) transitions. All
    // ordinary reset paths drain both active and retained private refs.
    void ResetContextState(bool RetainSamplerRefs = false);

    // DDI-only Finish(FALSE) fast path. This clears logical state and fixed
    // validity maps without releasing retained slot ownership. Normal reset
    // paths still drain every private reference.
    void ResetHeliosDdiLogicalState();

    template<uint32_t Size>
    bool HeliosDdiSlotWasInvalid(D3D11HeliosLogicalSlots<Size>& slots, uint32_t slot) {
      if constexpr (!IsDeferred)
        return false;

      if (likely(!m_heliosDdiLogicalStateActive)
       || likely(slots.test(slot, m_heliosDdiValidity.generation)))
        return false;

      slots.set(slot, m_heliosDdiValidity.generation);
      return true;
    }

    template<uint32_t Size>
    bool HeliosDdiSlotIsValid(const D3D11HeliosLogicalSlots<Size>& slots, uint32_t slot) const {
      if constexpr (!IsDeferred)
        return true;

      return !m_heliosDdiLogicalStateActive
        || slots.test(slot, m_heliosDdiValidity.generation);
    }

    bool HeliosDdiScalarWasInvalid(uint16_t bit) {
      if constexpr (!IsDeferred)
        return false;

      if (likely(!m_heliosDdiLogicalStateActive) || likely(m_heliosDdiValidity.scalarMask & bit))
        return false;

      m_heliosDdiValidity.scalarMask |= bit;
      return true;
    }

    bool HeliosDdiScalarIsValid(uint16_t bit) const {
      if constexpr (!IsDeferred)
        return true;

      return !m_heliosDdiLogicalStateActive || (m_heliosDdiValidity.scalarMask & bit);
    }

    bool HeliosDdiShaderWasInvalid(D3D11ShaderType stage) {
      if constexpr (!IsDeferred)
        return false;

      uint32_t bit = 1u << uint32_t(stage);
      if (likely(!m_heliosDdiLogicalStateActive) || likely(m_heliosDdiValidity.shaderMask & bit))
        return false;

      m_heliosDdiValidity.shaderMask |= bit;
      return true;
    }

    bool HeliosDdiShaderIsValid(D3D11ShaderType stage) const {
      if constexpr (!IsDeferred)
        return true;

      return !m_heliosDdiLogicalStateActive
        || (m_heliosDdiValidity.shaderMask & (1u << uint32_t(stage)));
    }

    void RetainHeliosSamplerRefs();

    void ResetDirtyTracking();

    void ResetStagingBuffer();

    template<D3D11ShaderType ShaderStage, typename T>
    void ResolveSrvHazards(
            T*                                pView);

    template<typename T>
    void ResolveCsSrvHazards(
            T*                                pView);

    template<typename T>
    void ResolveOmSrvHazards(
            T*                                pView);

    bool ResolveOmRtvHazards(
            D3D11UnorderedAccessView*         pView);

    void ResolveOmUavHazards(
            D3D11RenderTargetView*            pView);

    void RestoreCommandListState();
    
    void RestoreConstantBuffers(
            D3D11ShaderType                   Stage);
    
    void RestoreSamplers(
            D3D11ShaderType                   Stage);

    void RestoreShaderResources(
            D3D11ShaderType                   Stage);

    void RestoreUnorderedAccessViews(
            D3D11ShaderType                   Stage);

    template<D3D11ShaderType ShaderStage>
    void SetConstantBuffers(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers);

    template<D3D11ShaderType ShaderStage>
    void SetConstantBuffers1(
            UINT                              StartSlot,
            UINT                              NumBuffers,
            ID3D11Buffer* const*              ppConstantBuffers,
      const UINT*                             pFirstConstant,
      const UINT*                             pNumConstants);

    template<D3D11ShaderType ShaderStage>
    void SetShaderResources(
            UINT                              StartSlot,
            UINT                              NumResources,
            ID3D11ShaderResourceView* const*  ppResources);

    template<D3D11ShaderType ShaderStage>
    void SetSamplers(
            UINT                              StartSlot,
            UINT                              NumSamplers,
            ID3D11SamplerState* const*        ppSamplers);

    void SetRenderTargetsAndUnorderedAccessViews(
            UINT                              NumRTVs,
            ID3D11RenderTargetView* const*    ppRenderTargetViews,
            ID3D11DepthStencilView*           pDepthStencilView,
            UINT                              UAVStartSlot,
            UINT                              NumUAVs,
            ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
      const UINT*                             pUAVInitialCounts);

    void SetDrawBuffers(
            ID3D11Buffer*                     pBufferForArgs,
            ID3D11Buffer*                     pBufferForCount);

    void SyncImage(
      const Rc<DxvkImage>&                    DstImage,
      const VkImageSubresourceLayers&         DstLayers,
      const Rc<DxvkImage>&                    SrcImage,
      const VkImageSubresourceLayers&         SrcLayers);

    bool TestRtvUavHazards(
            UINT                              NumRTVs,
            ID3D11RenderTargetView* const*    ppRTVs,
            UINT                              NumUAVs,
            ID3D11UnorderedAccessView* const* ppUAVs);

    template<D3D11ShaderType ShaderStage>
    bool TestSrvHazards(
            D3D11ShaderResourceView*          pView);

    void TrackResourceSequenceNumber(
            ID3D11Resource*                   pResource);

    void UpdateBuffer(
            D3D11Buffer*                      pDstBuffer,
            UINT                              Offset,
            UINT                              Length,
      const void*                             pSrcData);

    void UpdateTexture(
            D3D11CommonTexture*               pDstTexture,
            UINT                              DstSubresource,
      const D3D11_BOX*                        pDstBox,
      const void*                             pSrcData,
            UINT                              SrcRowPitch,
            UINT                              SrcDepthPitch);

    void UpdateImage(
            D3D11CommonTexture*               pDstTexture,
      const VkImageSubresource*               pDstSubresource,
            VkOffset3D                        DstOffset,
            VkExtent3D                        DstExtent,
            DxvkBufferSlice                   StagingBuffer);

    void UpdateResource(
            ID3D11Resource*                   pDstResource,
            UINT                              DstSubresource,
      const D3D11_BOX*                        pDstBox,
      const void*                             pSrcData,
            UINT                              SrcRowPitch,
            UINT                              SrcDepthPitch,
            UINT                              CopyFlags);

    void UpdateUnorderedAccessViewCounter(
            D3D11UnorderedAccessView*         pUav,
            uint32_t                          CounterValue);

    bool ValidateRenderTargets(
            UINT                              NumViews,
            ID3D11RenderTargetView* const*    ppRenderTargetViews,
            ID3D11DepthStencilView*           pDepthStencilView);

    template<D3D11ShaderType ShaderStage>
    void SetClassInstances(
      const D3D11CommonShader*                pShader,
            ID3D11ClassInstance* const*       ppClassInstances,
            UINT                              NumClassInstances);

    template<D3D11ShaderType ShaderStage>
    void GetClassInstances(
            ID3D11ClassInstance**             ppClassInstances,
            UINT*                             pNumClassInstances);

    Rc<DxvkBuffer> AllocInstanceDataBuffer(
            D3D11ShaderType                   ShaderStage);

    force_inline void AddCost(uint64_t Value) {
      m_estimatedCost += Value;
    }

    static DxvkInputAssemblyState InitDefaultPrimitiveTopology();

    static DxvkRasterizerState InitDefaultRasterizerState();

    static DxvkDepthStencilState InitDefaultDepthStencilState();

    static DxvkMultisampleState InitDefaultMultisampleState(
            UINT                              SampleMask);

    static DxvkLogicOpState InitDefaultLogicOpState();

    static DxvkBlendMode InitDefaultBlendState();

    template<bool AllowFlush = true, typename Cmd>
    void EmitCs(Cmd&& command) {
      // Every CS-visible command funnels through here or EmitCsCmd. In
      // particular, a deferred parent must consume a nested child's retained
      // tail before it records its own next command.
      if (unlikely(m_heliosCsState != D3D11HeliosCsState::Unknown))
        HeliosNoteCsEmit(AllowFlush);

      if (unlikely(m_csDataType != D3D11CmdType::None)) {
        m_csData = nullptr;
        m_csDataType = D3D11CmdType::None;
      }

      if (unlikely(!m_csChunk->push(command))) {
        GetTypedContext()->EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();

        if constexpr (!IsDeferred && AllowFlush)
          GetTypedContext()->ConsiderFlush(GpuFlushType::ImplicitWeakHint);

        m_csChunk->push(command);
      }
    }

    template<typename M, bool AllowFlush = true, typename Cmd>
    void EmitCsCmd(D3D11CmdType type, size_t count, Cmd&& command) {
      // See EmitCs. Cmd-buffer payloads are equally CS-visible state from
      // the viewpoint of a later nested command-list execution.
      if (unlikely(m_heliosCsState != D3D11HeliosCsState::Unknown))
        HeliosNoteCsEmit(AllowFlush);

      m_csDataType = type;
      m_csData = m_csChunk->pushCmd<M, Cmd>(command, count);

      if (unlikely(!m_csData)) {
        GetTypedContext()->EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();

        if constexpr (!IsDeferred && AllowFlush)
          GetTypedContext()->ConsiderFlush(GpuFlushType::ImplicitWeakHint);

        // We must record this command after the potential
        // flush since the caller may still access the data
        m_csData = m_csChunk->pushCmd<M, Cmd>(command, count);
      }
    }

    void FlushCsChunk() {
      if (likely(!m_csChunk->empty())) {
        m_csData = nullptr;
        m_csDataType = D3D11CmdType::None;

        GetTypedContext()->EmitCsChunk(std::move(m_csChunk));
        m_csChunk = AllocCsChunk();
      }
    }

    template<typename T>
    const D3D11CommonShader* GetCommonShader(T* pShader) const {
      return pShader != nullptr ? pShader->GetCommonShader() : nullptr;
    }

    static uint32_t GetIndirectCommandStride(const D3D11CmdDrawIndirectData* cmdData, VkDeviceSize offset, uint32_t minStride) {
      if (likely(cmdData->stride))
        return cmdData->offset + cmdData->count * cmdData->stride == offset ? cmdData->stride : 0;

      VkDeviceSize stride = offset - cmdData->offset;
      return stride >= minStride && stride <= 32u ? uint32_t(stride) : 0u;
    }

  private:

    ContextType* GetTypedContext() {
      return static_cast<ContextType*>(this);
    }

    D3D10DeviceLock LockContext() {
      return GetTypedContext()->LockContext();
    }

  };
  
}
