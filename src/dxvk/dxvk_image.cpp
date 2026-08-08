#include "dxvk_image.h"

#include "dxvk_buffer.h"
#include "dxvk_device.h"

#include <cstdlib>

namespace dxvk {

  typedef struct VkImportMemoryResourceInfoMESA {
    VkStructureType sType;
    const void* pNext;
    uint32_t resourceId;
  } VkImportMemoryResourceInfoMESA;

  constexpr VkStructureType VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA_HELIOS =
    VkStructureType(1000384002);
  
  namespace {
    // Default ON. Helios shared resources are always KMT-only: the UMD is the
    // only thing that hosts this engine, and it used to force
    // HELIOS_DXVK_KMT_SHARED=1 into its own environment before any DXVK device
    // existed, so this could not be false in any shipping configuration. The
    // env var survives only as the `=0` disable for a standalone DXVK build.
    bool heliosKmtOnlySharedResources() {
      const char* value = std::getenv("HELIOS_DXVK_KMT_SHARED");
      return !(value && value[0] == '0');
    }

    // Helios black-desktop localization diagnostic (13th session): when set,
    // device-local cross-process shared IMPORTS are replaced by a PRIVATE
    // device-local image cleared to solid magenta (see dxvk_image.cpp import
    // path + d3d11_initializer InitHeliosMagentaTexture).
    bool heliosDebugMagenta() {
      const char* value = std::getenv("HELIOS_DEBUG_MAGENTA");
      return value && value[0] == '1' && value[1] == '\0';
    }

    // Helios experimental device-local alias refresh. Keep it opt-in: enabling
    // this globally makes DWM rebuild every imported display surface through a
    // second image and currently crash-loops composition during initialization.
    // The committed direct-import path remains the production baseline.
    bool heliosDevlocalStaging() {
      const char* value = std::getenv("HELIOS_DEVLOCAL_STAGING");
      return value && value[0] == '1' && value[1] == '\0';
    }
  }
  
  DxvkKeyedMutex::DxvkKeyedMutex(
      const Rc<DxvkDevice>& device,
            uint64_t        initialValue,
            bool            ntShared)
  : m_vkd(device->vkd()) {
    DxvkFenceCreateInfo fenceInfo;
    fenceInfo.initialValue = 0;
    fenceInfo.sharedType = ntShared
      ? VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
      : VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT;
    m_fence = device->createFence(fenceInfo);
    if (!m_fence)
      throw DxvkError("DxvkKeyedMutex: Failed to create fence");

    D3DKMT_CREATEKEYEDMUTEX2 create = { };
    create.Flags.NtSecuritySharing = ntShared;
    create.InitialValue = initialValue;
    NTSTATUS createStatus = D3DKMTCreateKeyedMutex2(&create);
    if (createStatus)
      throw DxvkError("DxvkKeyedMutex: Failed to create mutex");

    m_kmtLocal = create.hKeyedMutex;
    m_kmtGlobal = create.hSharedHandle;
  }


  DxvkKeyedMutex::DxvkKeyedMutex(
      const Rc<DxvkDevice>& device,
            Rc<DxvkFence>&& fence,
            D3DKMT_HANDLE   kmtLocal,
            D3DKMT_HANDLE   kmtGlobal)
  : m_vkd(device->vkd()),
    m_fence(fence),
    m_kmtLocal(kmtLocal),
    m_kmtGlobal(kmtGlobal) {
  }

    
  DxvkKeyedMutex::~DxvkKeyedMutex() {
    if (m_kmtLocal) {
      D3DKMT_DESTROYKEYEDMUTEX destroy = { };
      destroy.hKeyedMutex = m_kmtLocal;
      D3DKMTDestroyKeyedMutex(&destroy);
    }
  }

  bool DxvkKeyedMutex::hasVulkanSyncObject() const {
    return m_fence != nullptr && m_fence->kmtLocal() != 0;
  }

  HRESULT DxvkKeyedMutex::AcquireSync(UINT64 key, DWORD  milliseconds) {
    if (m_owned.load(std::memory_order_acquire)) {
      return DXGI_ERROR_INVALID_CALL;
    }

    LARGE_INTEGER timeout = { };
    D3DKMT_ACQUIREKEYEDMUTEX2 acquire = { };
    acquire.hKeyedMutex = m_kmtLocal;
    acquire.Key = key;
    acquire.pTimeout = &timeout;
    timeout.QuadPart = milliseconds * -10000;

    NTSTATUS status = D3DKMTAcquireKeyedMutex2(&acquire);
    if (status == STATUS_TIMEOUT)
      return WAIT_TIMEOUT;
    if (status) {
      Logger::warn("DxvkKeyedMutex::AcquireSync: D3DKMTAcquireKeyedMutex2 failed");
      return DXGI_ERROR_INVALID_CALL;
    }

    if (hasVulkanSyncObject()) {
      VkSemaphore semaphore = m_fence->handle();
      VkSemaphoreWaitInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
      info.semaphoreCount = 1;
      info.pSemaphores = &semaphore;
      info.pValues = &acquire.FenceValue;

      VkResult waitVr = m_vkd->vkWaitSemaphores(m_vkd->device(), &info, -1);
      if (waitVr) {
        Logger::warn("DxvkKeyedMutex::AcquireSync: Failed to wait semaphore");
        return DXGI_ERROR_INVALID_CALL;
      }
    }

    m_fenceValue = acquire.FenceValue;
    m_owned.store(true, std::memory_order_release);
    return S_OK;
  }


  HRESULT DxvkKeyedMutex::ReleaseSync(UINT64 key) {
    if (!m_owned.load(std::memory_order_acquire)) {
      return DXGI_ERROR_INVALID_CALL;
    }

    const uint64_t nextFenceValue = m_fenceValue + 1;

    D3DKMT_RELEASEKEYEDMUTEX2 release = { };
    release.hKeyedMutex = m_kmtLocal;
    release.Key = key;
    release.FenceValue = nextFenceValue;

    NTSTATUS releaseStatus = D3DKMTReleaseKeyedMutex2(&release);
    if (releaseStatus) {
      Logger::warn("D3D11DXGIKeyedMutex::ReleaseSync: D3DKMTReleaseKeyedMutex2 failed.");
      return DXGI_ERROR_INVALID_CALL;
    }

    if (hasVulkanSyncObject()) {
      VkSemaphoreSignalInfo info = { VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO };
      info.semaphore = m_fence->handle();
      info.value = nextFenceValue;

      VkResult signalVr = m_vkd->vkSignalSemaphore(m_vkd->device(), &info);
      if (signalVr) {
        Logger::warn("D3D11DXGIKeyedMutex::ReleaseSync: Failed to signal semaphore");
        return DXGI_ERROR_INVALID_CALL;
      }
    }

    m_fenceValue = nextFenceValue;
    m_owned.store(false, std::memory_order_release);
    return S_OK;
  }


  DxvkImage::DxvkImage(
          DxvkDevice*           device,
    const DxvkImageCreateInfo&  createInfo,
          DxvkMemoryAllocator&  allocator,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(allocator),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_info          (createInfo) {
    m_heliosDevice = device;
    m_allocator->registerResource(this);

    copyFormatList(createInfo.viewFormatCount, createInfo.viewFormats);
    m_unifiedLayoutAvailable = device->features().khrUnifiedImageLayouts.unifiedImageLayouts;

    // Assign debug name to image
    if (device->debugFlags().test(DxvkDebugFlag::Capture)) {
      m_debugName = createDebugName(createInfo.debugName);
      m_info.debugName = m_debugName.c_str();
    } else {
      m_info.debugName = nullptr;
    }

    // Always enable depth-stencil attachment usage for depth-stencil
    // formats since some internal operations rely on it. Read-only
    // versions of these make little sense to begin with.
    if (lookupFormatInfo(createInfo.format)->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
      m_info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    // Check whether the unified layout path can be used once
    // we know all image properties
    m_unifiedLayoutEnabled = canUseUnifiedLayout(*device);

    if (m_unifiedLayoutEnabled)
      m_info.layout = VK_IMAGE_LAYOUT_GENERAL;

    // Determine whether the image is shareable before creating the resource
    VkImageCreateInfo imageInfo = getImageCreateInfo(DxvkImageUsageInfo());
    m_shared = canShareImage(device, imageInfo, m_info.sharing);

    m_globalLayout = (m_info.sharing.mode != DxvkSharedHandleMode::Import)
      ? m_info.initialLayout : m_info.layout;

    assignStorage(allocateStorage());

    // Helios GDI staging: a normal Import image is tracked as already-GENERAL
    // because its bound external memory carries the creator's GENERAL-layout
    // content. A staged image is instead a PRIVATE device-local surface created
    // in initialLayout (UNDEFINED) with no content — so it must be tracked as
    // UNDEFINED, not GENERAL, or DXVK assumes it is already GENERAL and never
    // emits the UNDEFINED->GENERAL transition. The host VkImage then stays
    // UNDEFINED at the composition draw (VUID-vkCmdDraw-None-09600) and NVIDIA
    // loses the device. Tracking the real layout makes every transition (the
    // init/refresh copy AND dwm's sample) correct.
    if (m_heliosGdiStaged || m_heliosDebugMagenta)
      m_globalLayout = m_info.initialLayout;

    // Helios: createImageResource returns null (rather than throwing) when the
    // backing memory allocation fails — e.g. the venus/host side refusing the
    // export-memory blob. A storage-less image is a time bomb (AVs at
    // ExportImageInfo/initImage/assignStorage); fail creation cleanly instead
    // so D3D11CreateTexture2D returns an error the runtime already handles.
    // Unregister first: a throwing ctor never runs ~DxvkImage, which would
    // leave a dangling pointer in the allocator's resource map.
    if (m_storage == nullptr) {
      m_allocator->unregisterResource(this);
      throw DxvkError("DxvkImage: failed to allocate backing storage");
    }

    // Helios diagnostic (black-desktop critique, 13th session): dump the full
    // Vulkan image create-info for every SHARED surface so a cross-process
    // creator<->opener VkImageCreateInfo mismatch can be diffed by resid/geometry.
    // The device-local shared-import path (firefox/wallpaper/icons, mem_type=1)
    // samples black; the leading mechanism is that the opener rebuilds a plain
    // RTV|SRV texture from a lossy meta trailer that omits usage / MUTABLE_FORMAT /
    // viewFormatList / tiling, so NVIDIA decodes the aliased OPAQUE_FD memory with
    // a different layout/compression than the creator's swapchain backbuffer.
    // Correlate an EXPORT line (creator) with an IMPORT line (opener) by resid
    // (same global res_id) or by ext/fmt/allocSize.
    if (m_info.sharing.mode != DxvkSharedHandleMode::None) {
      const char* modeStr =
        m_info.sharing.mode == DxvkSharedHandleMode::Import ? "IMPORT" :
        m_info.sharing.mode == DxvkSharedHandleMode::Export ? "EXPORT" : "NONE";
      Logger::info(str::format("DxvkImage: SHARED create-info mode=", modeStr,
        " resid=", m_info.sharing.heliosResourceId,
        " ext=", m_info.extent.width, "x", m_info.extent.height,
        " fmt=", uint32_t(m_info.format),
        " tiling=", uint32_t(m_info.tiling),
        " mips=", m_info.mipLevels, " layers=", m_info.numLayers,
        " viewFmts=", m_info.viewFormatCount,
        " allocSize=", m_info.sharing.heliosAllocSize,
        " memType=", m_info.sharing.heliosMemoryTypeIndex,
        " staged=", (m_heliosGdiStaged ? 1u : 0u),
        " alias=", (m_info.heliosDirectImportAlias ? 1u : 0u),
        " scanoutTarget=", (m_info.heliosLinearScanoutTarget ? 1u : 0u),
        " directOptimal=", (m_info.heliosDirectOptimalScanout ? 1u : 0u),
        " crossContextOptimal=", (m_info.heliosCrossContextOptimal ? 1u : 0u),
        " layout=", uint32_t(m_info.layout),
        " usage=0x", std::hex, uint32_t(m_info.usage),
        " flags=0x", uint32_t(m_info.flags), std::dec));
    }
  }


  DxvkImage::DxvkImage(
          DxvkDevice*           device,
    const DxvkImageCreateInfo&  createInfo,
          VkImage               imageHandle,
          DxvkMemoryAllocator&  allocator,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(allocator),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_info          (createInfo),
    m_stableAddress (true),
    m_globalLayout  (createInfo.initialLayout) {
    m_allocator->registerResource(this);

    copyFormatList(createInfo.viewFormatCount, createInfo.viewFormats);

    // Create backing storage for existing image resource
    DxvkAllocationInfo allocationInfo = { };
    allocationInfo.resourceCookie = cookie();

    VkImageCreateInfo imageInfo = getImageCreateInfo(DxvkImageUsageInfo());
    assignStorage(m_allocator->importImageResource(imageInfo, allocationInfo, imageHandle));
  }


  DxvkImage::~DxvkImage() {
    m_allocator->unregisterResource(this);
  }


  bool DxvkImage::canRelocate() const {
    return !m_imageInfo.mapPtr && !m_shared && !m_stableAddress
        && !(m_info.flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT);
  }


  HANDLE DxvkImage::sharedHandle() const {
    HANDLE handle = INVALID_HANDLE_VALUE;

    if (!m_shared)
      return INVALID_HANDLE_VALUE;

    if (heliosKmtOnlySharedResources()) {
      return INVALID_HANDLE_VALUE;
    }

#ifdef _WIN32
    if (!m_vkd->vkGetMemoryWin32HandleKHR) {
      Logger::warn("DxvkImage::sharedHandle: vkGetMemoryWin32HandleKHR unavailable");
      return INVALID_HANDLE_VALUE;
    }

    DxvkResourceMemoryInfo memoryInfo = m_storage->getMemoryInfo();

    VkMemoryGetWin32HandleInfoKHR handleInfo = { VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR };
    handleInfo.handleType = m_info.sharing.type;
    handleInfo.memory = memoryInfo.memory;

    if (m_vkd->vkGetMemoryWin32HandleKHR(m_vkd->device(), &handleInfo, &handle) != VK_SUCCESS)
      Logger::warn("DxvkImage::DxvkImage: Failed to get shared handle for image");
#endif

    return handle;
  }


  DxvkSparsePageTable* DxvkImage::getSparsePageTable() {
    return m_storage->getSparsePageTable();
  }


  Rc<DxvkResourceAllocation> DxvkImage::relocateStorage(
          DxvkAllocationModes         mode) {
    if (!canRelocate())
      return nullptr;

    return allocateStorageWithUsage(DxvkImageUsageInfo(), mode);
  }


  uint64_t DxvkImage::getSubresourceAddressAt(uint32_t mip, uint32_t layer, VkOffset3D coord) const {
    // For 2D and 3D images, use morton codes to linearize the address ranges
    // of pixel blocks. This helps reduce false positives in common use cases
    // where the application copies aligned power-of-two blocks around.
    uint64_t base = getSubresourceStartAddress(mip, layer);

    if (likely(m_info.type == VK_IMAGE_TYPE_2D))
      return base + bit::interleave(coord.x, coord.y);

    // For 1D we can simply use the pixel coordinate as-is
    if (m_info.type == VK_IMAGE_TYPE_1D)
      return base + coord.x;

    // 3D is uncommon, but there are different use cases. Assume that if the
    // format is block-compressed, the app will access one layer at a time.
    if (formatInfo()->flags.test(DxvkFormatFlag::BlockCompressed))
      return base + bit::interleave(coord.x, coord.y) + (uint64_t(coord.z) << 32u);

    // Otherwise, it may want to copy actual 3D blocks around.
    return base + bit::interleave(coord.x, coord.y, coord.z);
  }


  Rc<DxvkImageView> DxvkImage::createView(
    const DxvkImageViewKey& info) {
    std::unique_lock lock(m_viewMutex);

    auto entry = m_views.emplace(std::piecewise_construct,
      std::make_tuple(info), std::make_tuple(this, info));

    return &entry.first->second;
  }


  Rc<DxvkResourceAllocation> DxvkImage::allocateStorage() {
    return allocateStorageWithUsage(DxvkImageUsageInfo(), 0u);
  }


  Rc<DxvkResourceAllocation> DxvkImage::allocateStorageWithUsage(
    const DxvkImageUsageInfo&         usageInfo,
          DxvkAllocationModes         mode) {
    const DxvkFormatInfo* formatInfo = lookupFormatInfo(m_info.format);
    small_vector<VkFormat, 4> localViewFormats;

    VkImageCreateInfo imageInfo = getImageCreateInfo(usageInfo);

    // Set up view format list so that drivers can better enable
    // compression. Skip for planar formats due to validation errors.
    VkImageFormatListCreateInfo formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };

    if (!(formatInfo->aspectMask & VK_IMAGE_ASPECT_PLANE_0_BIT)) {
      if (usageInfo.viewFormatCount) {
        for (uint32_t i = 0; i < m_viewFormats.size(); i++)
          localViewFormats.push_back(m_viewFormats[i]);

        for (uint32_t i = 0; i < usageInfo.viewFormatCount; i++) {
          if (!isViewCompatible(usageInfo.viewFormats[i]))
            localViewFormats.push_back(usageInfo.viewFormats[i]);
        }

        formatList.viewFormatCount = localViewFormats.size();
        formatList.pViewFormats = localViewFormats.data();
      } else {
        formatList.viewFormatCount = m_viewFormats.size();
        formatList.pViewFormats = m_viewFormats.data();
      }
    }

    if ((m_info.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && formatList.viewFormatCount)
      formatList.pNext = std::exchange(imageInfo.pNext, &formatList);

    // Helios gets WDDM allocation/resource KMT handles from d3d10umddi and
    // stamps them into DXVK after resource creation. This bridge protocol is
    // distinct from native VK_KHR_external_memory_win32, even when the ICD
    // exposes that extension to other Vulkan applications. Virglrenderer will
    // only bind a VkDeviceMemory to a HOST3D blob if that memory was allocated
    // as exportable. Use Venus' renderer-side opaque-fd handle type for the
    // Vulkan pNext chain while suppressing DXVK's Win32 handle retrieval below.
    const bool heliosKmtShared = heliosKmtOnlySharedResources();
    bool useVulkanExternalMemory = m_shared && !heliosKmtShared;
    bool useHeliosRendererExternalMemory = m_shared && heliosKmtShared;
    // Scan-out surfaces use DMA_BUF; ordinary shared surfaces retain the
    // renderer opaque-fd handle. Both externalInfo.handleTypes and
    // sharedExport.handleTypes key off this.
    VkExternalMemoryHandleTypeFlagBits heliosRendererHandleType =
      (m_info.heliosScanoutPrimary || m_info.heliosLinearScanoutTarget
       || m_info.heliosDirectOptimalScanout || m_info.heliosCrossContextOptimal)
        ? VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
        : VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    // Helios GDI staging (approach A): the KMD backs standard allocations (the
    // GDI redirection/shadow surfaces the kernel executor CPU-writes through
    // the BAR) with HOST_VISIBLE memory. Binding that memory directly to dwm's
    // sampled image forces either an illegal device-local bind (VUID-01615 →
    // black desktop) or a linear image whose driver-chosen row pitch cannot
    // match the executor's fixed cross_adapter_pitch (diagonal shear). Instead,
    // import the host-visible bytes as a BUFFER and copy them — pitch-correct —
    // into a private device-local OPTIMAL sampled image each frame (see
    // DxvkContext::acquireSharedImagesFromExternal). Gate tightly: only 2D,
    // single mip/layer/sample, 4-byte-per-texel (BGRA-class) host-visible
    // imports, because the executor always writes 4 bytes/texel at a
    // round_up(width*4,256) byte stride.
    const bool heliosImportCandidate = useHeliosRendererExternalMemory
     && !m_info.heliosDirectImportAlias
     && !m_info.heliosLinearScanoutTarget
     && !m_info.heliosCrossContextOptimal
     && m_info.sharing.mode == DxvkSharedHandleMode::Import
     && m_info.sharing.heliosResourceId
     && m_info.sharing.heliosAllocSize
     && m_info.type == VK_IMAGE_TYPE_2D
     && m_info.mipLevels == 1u
     && m_info.numLayers == 1u
     && m_info.sampleCount == VK_SAMPLE_COUNT_1_BIT
     && m_heliosStagingBuffer == nullptr
     && m_heliosStagingImage == nullptr;

    const bool heliosHostVisibleImport = heliosImportCandidate
     && m_allocator->isHostVisibleMemoryType(m_info.sharing.heliosMemoryTypeIndex);

    if (heliosHostVisibleImport
     && lookupFormatInfo(m_info.format)->elementSize == 4u) {
      // Best-effort: ANY failure here — importVenusStagingBuffer returning null
      // OR throwing — must fall through to the direct host-visible import path
      // below, never fail this texture's creation. A failed OpenSharedResource
      // crash-loops dwm (0x8898008d fail-fast); a sheared-but-present fallback
      // is strictly better. The staged path is committed to (flags flipped)
      // only after every fallible step has succeeded.
      try {
        auto stagingAlloc = m_allocator->importVenusStagingBuffer(
          m_info.sharing.heliosAllocSize,
          m_info.sharing.heliosMemoryTypeIndex,
          m_info.sharing.heliosResourceId);

        if (stagingAlloc) {
          DxvkBufferCreateInfo bufferInfo = { };
          bufferInfo.size   = m_info.sharing.heliosAllocSize;
          bufferInfo.usage  = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
          bufferInfo.stages = VK_PIPELINE_STAGE_TRANSFER_BIT;
          bufferInfo.access = VK_ACCESS_TRANSFER_READ_BIT;

          Rc<DxvkBuffer> staging = new DxvkBuffer(m_allocator->device(),
            bufferInfo, std::move(stagingAlloc), *m_allocator,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

          // All fallible steps done — commit to the staged path. The sampled
          // image is now a private device-local surface: drop the
          // external-memory setup so it allocates ordinary device-local memory.
          m_heliosStagingBuffer = std::move(staging);
          m_heliosGdiStaged = true;
          useHeliosRendererExternalMemory = false;

          // Source row-pitch alignment for the per-frame copyBufferToImage: the
          // host-visible GDI executor writes LINEAR rows at round_up(width*4,256).
          // (This buffer path is host-visible-only now — device-local blobs hold
          // the host driver's OPTIMAL tiled layout, which no linear pitch can
          // decode; they take the alias-image path below instead.)
          m_heliosStagedRowAlign = 256u;

          Logger::info(str::format("DxvkImage: GDI staging enabled (", m_info.extent.width,
            "x", m_info.extent.height, ", resid ", m_info.sharing.heliosResourceId, ")"));
        } else {
          Logger::warn("DxvkImage: GDI staging buffer import failed, "
            "falling back to direct host-visible image import");
        }
      } catch (const DxvkError& e) {
        Logger::warn(str::format("DxvkImage: GDI staging setup failed, "
          "falling back to direct import: ", e.message()));
        m_heliosStagingBuffer = nullptr;
        m_heliosGdiStaged = false;
      }
    } else if (heliosImportCandidate
            && !heliosHostVisibleImport
            && heliosDevlocalStaging()
            && lookupFormatInfo(m_info.format)->aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) {
      // Device-local staging v2 (alias-image copy). The STEP-0 probes proved the
      // shared blob CONTAINS the creator's pixels, but in the host driver's
      // OPTIMAL (tiled) layout — a linear buffer copy at ANY pitch scrambles it
      // (there is no pitch). The only correct GPU-side read is through an IMAGE
      // bound to the same memory with the creator's create-info: create a
      // direct-bind alias image of the imported venus resource and source the
      // per-frame refresh from vkCmdCopyImage(alias -> private sampled image),
      // which detiles in hardware. Transfer reads of direct imports are the
      // historically-working path (the IDD's CopyResource capture delivered the
      // taskbar for months); it is dwm's SAMPLED reads of direct imports that
      // come up black. Any color format qualifies (A8 text masks included —
      // image copies are pitch-free).
      try {
        DxvkImageCreateInfo aliasInfo = m_info;
        aliasInfo.heliosDirectImportAlias = VK_TRUE;
        aliasInfo.usage  |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        aliasInfo.stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
        aliasInfo.access |= VK_ACCESS_TRANSFER_READ_BIT;
        // initialLayout stays UNDEFINED: VkImageCreateInfo.initialLayout must be
        // UNDEFINED/PREINITIALIZED (VUID 00993; external-memory images require
        // UNDEFINED, VUID 01443 — overriding it to GENERAL here tripped host
        // validation and undefined NVIDIA behavior). Content is NOT discarded:
        // Import-mode images track m_globalLayout = info().layout (GENERAL), so
        // dxvk never emits an UNDEFINED->X discard transition on the alias.
        aliasInfo.debugName = "helios_devlocal_alias";

        Rc<DxvkImage> alias = new DxvkImage(m_heliosDevice, aliasInfo,
          *m_allocator, m_properties);

        // All fallible steps done — commit. The sampled image becomes a private
        // device-local surface (no venus import of its own).
        m_heliosStagingImage = std::move(alias);
        m_heliosGdiStaged = true;
        useHeliosRendererExternalMemory = false;

        Logger::info(str::format("DxvkImage: device-local ALIAS staging enabled (",
          m_info.extent.width, "x", m_info.extent.height,
          ", resid ", m_info.sharing.heliosResourceId, ")"));
      } catch (const DxvkError& e) {
        Logger::warn(str::format("DxvkImage: device-local alias staging setup failed, "
          "falling back to direct import: ", e.message()));
        m_heliosStagingImage = nullptr;
        m_heliosGdiStaged = false;
      }
    }

    // Helios magenta LOCALIZATION diagnostic (13th session): when HELIOS_DEBUG_MAGENTA
    // is set, take the DEVICE-LOCAL cross-process shared import (firefox/wallpaper/icons —
    // the black surfaces that fell through the host-visible staging gate above) and make it
    // a PRIVATE device-local image cleared to solid MAGENTA (see InitHeliosMagentaTexture)
    // instead of importing the creator's memory. If dwm then shows magenta where the desktop
    // was black, dwm samples the exact image we hand it and the sample/compose/IDD chain is
    // intact => the black is a content-DELIVERY bug (the import does not carry the creator's
    // pixels). If still black, dwm samples a different surface. Device-local imports only
    // (host-visible ones already took the working staging path above).
    if (useHeliosRendererExternalMemory
     && m_info.sharing.mode == DxvkSharedHandleMode::Import
     && m_info.sharing.heliosResourceId
     && !m_heliosGdiStaged
     && !m_info.heliosDirectImportAlias
     && heliosDebugMagenta()) {
      m_heliosDebugMagenta = true;
      useHeliosRendererExternalMemory = false;  // private device-local; no venus import
      Logger::info(str::format("DxvkImage: DEBUG MAGENTA private image (", m_info.extent.width,
        "x", m_info.extent.height, ", resid ", m_info.sharing.heliosResourceId, ")"));
    }

    // Set up external memory parameters for shared images
    VkExternalMemoryImageCreateInfo externalInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };

    if (useVulkanExternalMemory || useHeliosRendererExternalMemory) {
      externalInfo.pNext = std::exchange(imageInfo.pNext, &externalInfo);
      externalInfo.handleTypes = useHeliosRendererExternalMemory
        ? heliosRendererHandleType
        : m_info.sharing.type;
    }

    // Set up shared memory properties
    void* sharedMemoryInfo = nullptr;

    VkExportMemoryAllocateInfo sharedExport = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    VkImportMemoryWin32HandleInfoKHR sharedImportWin32 = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
    VkImportMemoryResourceInfoMESA heliosImportResource = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA_HELIOS };

    if ((useVulkanExternalMemory || useHeliosRendererExternalMemory)
     && m_info.sharing.mode == DxvkSharedHandleMode::Export) {
      sharedExport.pNext = std::exchange(sharedMemoryInfo, &sharedExport);
      sharedExport.handleTypes = useHeliosRendererExternalMemory
        ? heliosRendererHandleType
        : m_info.sharing.type;
    }

    if (useVulkanExternalMemory && m_info.sharing.mode == DxvkSharedHandleMode::Import) {
      sharedImportWin32.pNext = std::exchange(sharedMemoryInfo, &sharedImportWin32);
      sharedImportWin32.handleType = m_info.sharing.type;
      sharedImportWin32.handle = m_info.sharing.handle;
    }

    if (useHeliosRendererExternalMemory && m_info.sharing.mode == DxvkSharedHandleMode::Import) {
      heliosImportResource.pNext = std::exchange(sharedMemoryInfo, &heliosImportResource);
      if (m_info.sharing.heliosResourceId) {
        // Typed import path: the venus resid + creator's allocation identity
        // arrive explicitly (KMD open-identity ABI via the UMD bridge).
        heliosImportResource.resourceId = m_info.sharing.heliosResourceId;
      } else {
        // Legacy HANDLE-punned resid. Should be unreachable now that the UMD
        // passes the typed identity; loud so any surviving caller is found.
        Logger::warn("DxvkImage: KMT import without typed venus identity (legacy HANDLE-punned resid)");
        heliosImportResource.resourceId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(m_info.sharing.handle));
      }
    }

    DxvkAllocationInfo allocationInfo = { };
    allocationInfo.resourceCookie = cookie();
    allocationInfo.properties = m_properties;
    allocationInfo.mode = mode;
    allocationInfo.forceDedicated = m_shared && heliosKmtShared;

    if (useHeliosRendererExternalMemory && m_info.sharing.mode == DxvkSharedHandleMode::Import) {
      // Import with the creator's exact venus allocation size and memory type
      // (see DxvkSharedHandleInfo::heliosAllocSize).
      allocationInfo.importSizeOverride = m_info.sharing.heliosAllocSize;
      allocationInfo.importMemoryTypeIndex = m_info.sharing.heliosMemoryTypeIndex;
    }
    if (useVulkanExternalMemory && m_info.sharing.mode != DxvkSharedHandleMode::None)
      allocationInfo.handleType = m_info.sharing.type;

    if (m_info.transient)
      allocationInfo.mode.set(DxvkAllocationMode::NoDedicated);

    return m_allocator->createImageResource(imageInfo,
      allocationInfo, sharedMemoryInfo);
  }


  Rc<DxvkResourceAllocation> DxvkImage::assignStorage(
          Rc<DxvkResourceAllocation>&& resource) {
    return assignStorageWithUsage(std::move(resource), DxvkImageUsageInfo());
  }


  Rc<DxvkResourceAllocation> DxvkImage::assignStorageWithUsage(
          Rc<DxvkResourceAllocation>&& resource,
    const DxvkImageUsageInfo&         usageInfo) {
    // Helios: never assign null storage — the getImageInfo() deref below AV'd
    // DWM (fault at assignStorageWithUsage+0x284) when an import/allocation
    // failure path handed a null resource through. Keep the current storage
    // and hand back nothing for the caller to destroy.
    if (resource == nullptr) {
      Logger::err("DxvkImage::assignStorageWithUsage: null storage, ignoring assignment");
      return nullptr;
    }

    Rc<DxvkResourceAllocation> old = std::move(m_storage);

    // Self-assignment is possible here if we
    // just update the image properties
    bool invalidateViews = false;
    m_storage = std::move(resource);

    if (m_storage != old) {
      m_imageInfo = m_storage->getImageInfo();

      if (unlikely(m_info.debugName))
        updateDebugName();

      invalidateViews = true;
    }

    if ((m_info.access | usageInfo.access) != m_info.access)
      invalidateViews = true;

    m_info.flags |= usageInfo.flags;
    m_info.usage |= usageInfo.usage;
    m_info.stages |= usageInfo.stages;
    m_info.access |= usageInfo.access;

    if (usageInfo.colorSpace != VK_COLOR_SPACE_MAX_ENUM_KHR)
      m_info.colorSpace = usageInfo.colorSpace;

    for (uint32_t i = 0; i < usageInfo.viewFormatCount; i++) {
      if (!isViewCompatible(usageInfo.viewFormats[i]))
        m_viewFormats.push_back(usageInfo.viewFormats[i]);
    }

    if (!m_viewFormats.empty()) {
      m_info.viewFormatCount = m_viewFormats.size();
      m_info.viewFormats = m_viewFormats.data();
    }

    // If feedback loops are enabled and the unified layout extension is
    // not natively supported, we need to disable the unified layout path
    if (m_info.usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
      m_unifiedLayoutEnabled = m_unifiedLayoutEnabled && m_unifiedLayoutAvailable;

    if (usageInfo.layout != VK_IMAGE_LAYOUT_UNDEFINED && !m_unifiedLayoutEnabled) {
      m_info.layout = usageInfo.layout;
      invalidateViews = true;
    }

    m_stableAddress |= usageInfo.stableGpuAddress;

    if (invalidateViews)
      m_version += 1u;

    if (!(m_properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
        !(m_shared && heliosKmtOnlySharedResources())) {
      auto common = m_properties & m_storage->getMemoryProperties();

      updateResidencyStatus((common & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
        ? DxvkResourceResidency::Resident
        : DxvkResourceResidency::Evicted);
    }

    return old;
  }


  bool DxvkImage::isInitialized(const VkImageSubresource& subresource) const {
    VkImageLayout layout = queryLayout(subresource);

    return layout != VK_IMAGE_LAYOUT_UNDEFINED
        && layout != VK_IMAGE_LAYOUT_PREINITIALIZED;
  }


  bool DxvkImage::isInitialized(const VkImageSubresourceRange& subresources) const {
    if (m_globalLayout != VK_IMAGE_LAYOUT_MAX_ENUM) {
      return m_globalLayout != VK_IMAGE_LAYOUT_UNDEFINED
          && m_globalLayout != VK_IMAGE_LAYOUT_PREINITIALIZED;
    } else {
      // Check each individual subresource layout
      VkImageAspectFlags aspects = subresources.aspectMask;

      while (aspects) {
        VkImageSubresource subresource = { };
        subresource.aspectMask = vk::getNextAspect(aspects);
        subresource.mipLevel = subresources.baseMipLevel;
        subresource.arrayLayer = subresources.baseArrayLayer;

        for (uint32_t m = 0u; m < subresources.levelCount; m++) {
          uint32_t index = computeSubresourceIndex(subresource);

          for (uint32_t l = 0u; l < subresources.layerCount; l++) {
            VkImageLayout layout = m_localLayouts[index + l];

            if (layout == VK_IMAGE_LAYOUT_UNDEFINED
             || layout == VK_IMAGE_LAYOUT_PREINITIALIZED)
              return false;
          }

          subresource.mipLevel += 1u;
        }
      }

      return true;
    }
  }


  VkImageLayout DxvkImage::queryLayout(const VkImageSubresourceRange& subresources) const {
    // Check whether the entire resource is in the same layout
    if (m_globalLayout != VK_IMAGE_LAYOUT_MAX_ENUM)
      return m_globalLayout;

    VkImageSubresource subresource = { };
    subresource.aspectMask = subresources.aspectMask & (subresources.aspectMask - 1u);
    subresource.mipLevel = subresources.baseMipLevel;
    subresource.arrayLayer = subresources.baseArrayLayer;

    VkImageLayout baseLayout = queryLayout(subresource);

    // If only one subresource is included in the range, return its layout
    VkImageAspectFlags nonplanarAspects = VK_IMAGE_ASPECT_COLOR_BIT
                                        | VK_IMAGE_ASPECT_DEPTH_BIT
                                        | VK_IMAGE_ASPECT_STENCIL_BIT;

    if (subresources.levelCount == 1u
     && subresources.layerCount == 1u
     && (subresources.aspectMask & nonplanarAspects))
      return baseLayout;

    // Otherwise, check whether all subresources have the same layout
    VkImageAspectFlags aspects = subresources.aspectMask;

    while (aspects) {
      subresource.aspectMask = vk::getNextAspect(aspects);
      subresource.mipLevel = subresources.baseMipLevel;

      for (uint32_t m = 0u; m < subresources.levelCount; m++) {
        uint32_t index = computeSubresourceIndex(subresource);

        for (uint32_t l = 0u; l < subresources.layerCount; l++) {
          if (m_localLayouts[index + l] != baseLayout)
            return VK_IMAGE_LAYOUT_MAX_ENUM;
        }

        subresource.mipLevel += 1u;
      }
    }

    return baseLayout;
  }


  void DxvkImage::trackLayout(const VkImageSubresourceRange& subresources, VkImageLayout layout) {
    // Nothing to do if the layout doesn't change
    if (m_globalLayout == layout)
      return;

    if (subresources == getAvailableSubresources()) {
      // Entire resource is in the same layout
      m_globalLayout = layout;
    } else {
      if (m_globalLayout != VK_IMAGE_LAYOUT_MAX_ENUM) {
        // If previously the entire resource was in the same layout,
        // we need to update all subresource entries to that layout
        if (m_localLayouts.empty())
          m_localLayouts.resize(computeSubresourceCount());

        for (size_t i = 0u; i < m_localLayouts.size(); i++)
          m_localLayouts[i] = m_globalLayout;

        m_globalLayout = VK_IMAGE_LAYOUT_MAX_ENUM;
      }

      // Update entries contained in the subresource range
      VkImageAspectFlags aspects = subresources.aspectMask;

      while (aspects) {
        VkImageSubresource subresource;
        subresource.aspectMask = vk::getNextAspect(aspects);
        subresource.mipLevel = subresources.baseMipLevel;
        subresource.arrayLayer = subresources.baseArrayLayer;

        for (uint32_t m = 0u; m < subresources.levelCount; m++) {
          uint32_t index = computeSubresourceIndex(subresource);

          for (uint32_t l = 0u; l < subresources.layerCount; l++)
            m_localLayouts[index + l] = layout;

          subresource.mipLevel += 1u;
        }
      }
    }
  }


  void DxvkImage::setDebugName(const char* name) {
    if (likely(!m_info.debugName))
      return;

    m_debugName = createDebugName(name);
    m_info.debugName = m_debugName.c_str();

    updateDebugName();
  }


  void DxvkImage::updateDebugName() {
    if (m_storage->flags().test(DxvkAllocationFlag::OwnsImage)) {
      VkDebugUtilsObjectNameInfoEXT nameInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
      nameInfo.objectType = VK_OBJECT_TYPE_IMAGE;
      nameInfo.objectHandle = vk::getObjectHandle(m_imageInfo.image);
      nameInfo.pObjectName = m_info.debugName;

      m_vkd->vkSetDebugUtilsObjectNameEXT(m_vkd->device(), &nameInfo);
    }
  }


  std::string DxvkImage::createDebugName(const char* name) const {
    return str::format(vk::isValidDebugName(name) ? name : "Image", " (", cookie(), ")");
  }


  VkImageCreateInfo DxvkImage::getImageCreateInfo(
    const DxvkImageUsageInfo&         usageInfo) const {
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.flags = m_info.flags | usageInfo.flags;
    info.imageType = m_info.type;
    info.format = m_info.format;
    info.extent = m_info.extent;
    info.mipLevels = m_info.mipLevels;
    info.arrayLayers = m_info.numLayers;
    info.samples = m_info.sampleCount;
    info.tiling = m_info.tiling;
    info.usage = m_info.usage | usageInfo.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = m_info.initialLayout;

    return info;
  }


  void DxvkImage::copyFormatList(uint32_t formatCount, const VkFormat* formats) {
    m_viewFormats.resize(formatCount);

    for (uint32_t i = 0; i < formatCount; i++)
      m_viewFormats[i] = formats[i];

    m_info.viewFormats = m_viewFormats.data();
  }


  bool DxvkImage::canShareImage(DxvkDevice* device, const VkImageCreateInfo& createInfo, const DxvkSharedHandleInfo& sharingInfo) const {
    if (sharingInfo.mode == DxvkSharedHandleMode::None)
      return false;

    // The Helios UMD bridge represents D3D KMT sharing with renderer-side
    // OPAQUE_FD/DMA_BUF memory and stamps the WDDM handles afterwards. It must
    // keep using that path when the ICD also advertises native Win32 external
    // memory: OPAQUE_WIN32_KMT is intentionally not a Vulkan-capable handle
    // type in Venus, so sending it through the native format query would mark
    // the image non-shared and allocate non-exportable memory.
    if (heliosKmtOnlySharedResources()) {
      if (createInfo.flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) {
        Logger::err("Failed to create shared resource: Sharing sparse resources not supported");
        return false;
      }

      Logger::warn("Helios KMT shared resource path: using renderer-backed external memory");
      return true;
    }

    if (!device->features().khrExternalMemoryWin32) {
      Logger::err("Failed to create shared resource: VK_KHR_EXTERNAL_MEMORY_WIN32 not supported");
      return false;
    }

    if (createInfo.flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) {
      Logger::err("Failed to create shared resource: Sharing sparse resources not supported");
      return false;
    }

    DxvkFormatQuery formatQuery = { };
    formatQuery.format = createInfo.format;
    formatQuery.type = createInfo.imageType;
    formatQuery.tiling = createInfo.tiling;
    formatQuery.usage = createInfo.usage;
    formatQuery.flags = createInfo.flags;
    formatQuery.handleType = sharingInfo.type;

    auto limits = device->getFormatLimits(formatQuery);

    if (!limits)
      return false;

    VkExternalMemoryFeatureFlagBits requiredFeature = sharingInfo.mode == DxvkSharedHandleMode::Export
      ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT
      : VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;

    if (!(limits->externalFeatures & requiredFeature)) {
      Logger::err("Failed to create shared resource: Image cannot be shared");
      return false;
    }

    return true;
  }


  bool DxvkImage::canUseUnifiedLayout(const DxvkDevice& device) const {
    if (m_unifiedLayoutAvailable)
      return true;

    // Always respect the config option if the extension is not supported
    if (!device.config().enableUnifiedImageLayout)
      return false;

    // Speshul case
    if (m_info.usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
      return false;

    // On RDNA1/2 we can enable the unified path for everything
    // that doesn't involve feedback loops or MSAA.
    if (device.properties().vk12.driverID == VK_DRIVER_ID_MESA_RADV
     && device.properties().vk13.minSubgroupSize == 32u)
      return m_info.sampleCount == VK_SAMPLE_COUNT_1_BIT;

    return false;
  }





  DxvkImageView::DxvkImageView(
          DxvkImage*                image,
    const DxvkImageViewKey&         key)
  : m_image   (image),
    m_key     (key) {
    // If the view does not define a layout, figure out a suitable
    // layout based on image view usage and image properties. This
    // will be good enough in most situations.
    if (!m_key.layout) {
      switch (m_key.usage & ~VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) {
        case VK_IMAGE_USAGE_SAMPLED_BIT:
          m_key.layout = (m_image->formatInfo()->aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
            ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          break;

        case VK_IMAGE_USAGE_STORAGE_BIT:
          m_key.layout = VK_IMAGE_LAYOUT_GENERAL;
          break;

        case VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT:
          m_key.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          break;

        case VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT:
          m_key.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
          break;

        default:
          break;
      }
    }

    updateProperties();
  }


  DxvkImageView::~DxvkImageView() {

  }


  const DxvkDescriptor* DxvkImageView::createView(VkImageViewType type) const {
    constexpr VkImageUsageFlags ViewUsage =
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_STORAGE_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;

    // If we're not allowed to reinterpret the view type, behave
    // as-if no resource was bound at all
    if (!m_key.allowTypeMismatch && type != m_key.viewType)
      return nullptr;

    // Legalize view usage. We allow creating transfer-only view
    // objects so that some internal APIs can be more consistent.
    DxvkImageViewKey key = m_key;
    key.viewType = type;
    key.layout = getLayout();
    key.allowTypeMismatch = VK_FALSE;

    if (!(key.usage & ViewUsage))
      return nullptr;

    // If the image has feedback loops enabled, forward the required
    // usage flags to the view as well.
    if (key.usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      if (m_image->info().usage & VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT)
        key.usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT | VK_IMAGE_USAGE_SAMPLED_BIT;

      if (m_image->info().usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)
        key.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    }

    // Only use one layer for non-arrayed view types
    if (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_2D)
      key.layerCount = 1u;

    switch (m_image->info().type) {
      case VK_IMAGE_TYPE_1D: {
        // Trivial, just validate that view types are compatible
        if (type != VK_IMAGE_VIEW_TYPE_1D && type != VK_IMAGE_VIEW_TYPE_1D_ARRAY)
          return nullptr;
      } break;

      case VK_IMAGE_TYPE_2D: {
        if (type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
          // Ensure that the image is compatible with cube maps
          if (key.layerCount < 6 || !(m_image->info().flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT))
            return nullptr;

          // Adjust layer count to make sure it's a multiple of 6
          key.layerCount = type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY
            ? key.layerCount - key.layerCount % 6u : 6u;
        } else if (type != VK_IMAGE_VIEW_TYPE_2D && type != VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
          return nullptr;
        }
      } break;

      case VK_IMAGE_TYPE_3D: {
        if (type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_2D_ARRAY) {
          // Ensure that the image is actually compatible with 2D views
          if (!(m_image->info().flags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT))
            return nullptr;

          // In case the view's native type is 3D, we can only create 2D compat
          // views if there is only one mip and with the full set of array layers.
          if (m_key.viewType == VK_IMAGE_VIEW_TYPE_3D) {
            if (m_key.mipCount != 1u)
              return nullptr;

            key.layerIndex = 0u;
            key.layerCount = type == VK_IMAGE_VIEW_TYPE_2D_ARRAY
              ? m_image->mipLevelExtent(key.mipIndex).depth : 1u;
          }
        } else if (type != VK_IMAGE_VIEW_TYPE_3D) {
          return nullptr;
        }
      } break;

      default:
        return nullptr;
    }

    // We need to expose RT and UAV swizzles to the backend,
    // but cannot legally pass them down to Vulkan
    if ((key.usage & ~VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT) != VK_IMAGE_USAGE_SAMPLED_BIT)
      key.packedSwizzle = 0u;

    return m_image->m_storage->createImageView(key);
  }


  void DxvkImageView::updateViews() {
    // Latch updated image properties
    updateProperties();

    // Update all views that are not currently null
    for (uint32_t i = 0; i < m_views.size(); i++) {
      if (m_views[i])
        m_views[i] = createView(VkImageViewType(i));
    }

    m_version = m_image->m_version;
  }


  void DxvkImageView::updateProperties() {
    m_properties.samples = m_image->info().sampleCount;
    m_properties.access = m_image->info().access;
  }

}
