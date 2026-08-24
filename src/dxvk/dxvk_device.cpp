#include "dxvk_device.h"
#include "dxvk_helios_feed_trace.h"
#include "dxvk_instance.h"
#include "dxvk_latency_builtin.h"
#include "dxvk_latency_reflex.h"
#include "dxvk_shader_cache.h"
#include "dxvk_shader_ir.h"

namespace dxvk {

  /// HELIOS: how many times a waitForResource stalled with the submission
  /// queue fully drained (see the loud check in waitForResource). Not
  /// necessarily fatal — a multi-threaded caller can still be rescued by
  /// another thread submitting the holding command list, and dwm trips this
  /// once per start — so read it as a rate, not as a pass/fail. Process-wide
  /// rather than per-device: the condition is about command-list submission,
  /// not a property of one device.
  static std::atomic<uint32_t> s_unsatisfiableWaits = { 0u };

  DxvkDevice::DxvkDevice(
    const Rc<DxvkInstance>&         instance,
    const Rc<DxvkAdapter>&          adapter,
    const Rc<vk::DeviceFn>&         vkd,
    const DxvkDeviceCapabilities&   caps,
    const DxvkDeviceQueueSet&       queues,
    const DxvkQueueCallback&        queueCallback,
    const DxvkHeliosOuterOps&       heliosOuterOps)
  : m_options           (instance->options()),
    m_instance          (instance),
    m_adapter           (adapter),
    m_vkd               (vkd),
    m_debugFlags        (instance->debugFlags()),
    m_queues            (queues),
    m_features          (caps.getFeatures()),
    m_properties        (caps.getProperties()),
    m_heliosOuterOps    (heliosOuterOps),
    m_perfHints         (getPerfHints()),
    m_objects           (this),
    m_checkpoints       (this),
    m_submissionQueue   (this, queueCallback) {

    // Construct the fixed trace storage before any producer can enqueue work
    // when the targeted feed trace is enabled. This keeps the hot sites to
    // relaxed atomic updates and timestamp reads only.
    helios_feed::initialize();

    if (adapter->kmtLocal()) {
      D3DKMT_CREATEDEVICE create = { };
      create.hAdapter = adapter->kmtLocal();
      if (D3DKMTCreateDevice(&create))
        Logger::warn("Failed to create D3DKMT device");
      else
        m_kmtLocal = create.hDevice;
    }

    determineShaderOptions();

    if (env::getEnvVar("DXVK_SHADER_CACHE") != "0" && DxvkShader::getShaderDumpPath().empty())
      m_shaderCache = DxvkShaderCache::getInstance();

    logBindingModel();
  }
  
  
  DxvkDevice::~DxvkDevice() {
    if (m_kmtLocal) {
      D3DKMT_DESTROYDEVICE destroy = { };
      destroy.hDevice = m_kmtLocal;
      D3DKMTDestroyDevice(&destroy);
    }

    // If we are being destroyed during/after DLL process detachment
    // from TerminateProcess, etc, our CS threads are already destroyed
    // and we cannot synchronize against them.
    // The best we can do is just wait for the Vulkan device to be idle.
    if (this_thread::isInModuleDetachment())
      return;

    // Wait for all pending Vulkan commands to be
    // executed before we destroy any resources.
    this->waitForIdle();

    // Stop workers explicitly in order to prevent
    // access to structures that are being destroyed.
    m_objects.pipelineManager().stopWorkerThreads();

    // Re-dumps deliberately overwrite the PID-specific file: a process may
    // create probe devices before its workload device. Module detachment
    // returned above and never attempts file I/O.
    helios_feed::dump();
  }


  void* DxvkDevice::beginHeliosOuterSubmit() const {
    if (!m_instance->isRecordOnlyDirect())
      return reinterpret_cast<void*>(uintptr_t(1));
    if (!m_heliosOuterOps)
      return nullptr;
    return m_heliosOuterOps.begin(m_heliosOuterOps.context);
  }


  VkResult DxvkDevice::finishHeliosOuterSubmit(
          void*       scope,
          VkResult    lowerResult) const {
    if (!m_instance->isRecordOnlyDirect())
      return lowerResult;
    if (!scope || !m_heliosOuterOps)
      return VK_ERROR_DEVICE_LOST;
    return m_heliosOuterOps.finish(m_heliosOuterOps.context, scope, lowerResult);
  }


  VkResult DxvkDevice::joinHeliosOuterSubmit() const {
    if (!m_instance->isRecordOnlyDirect())
      return VK_SUCCESS;
    if (!m_heliosOuterOps)
      return VK_ERROR_DEVICE_LOST;
    return m_heliosOuterOps.join(m_heliosOuterOps.context);
  }


  VkMemoryRequirements DxvkDevice::queryBufferMemoryRequirements(
    const DxvkBufferCreateInfo&       createInfo) const {
    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.flags = createInfo.flags;
    info.usage = createInfo.usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    info.size  = createInfo.size;
    getSharingMode().fill(info);

    VkDeviceBufferMemoryRequirements query = { VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS };
    query.pCreateInfo = &info;

    VkMemoryRequirements2 requirements = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    m_vkd->vkGetDeviceBufferMemoryRequirements(m_vkd->device(), &query, &requirements);
    return requirements.memoryRequirements;
  }


  VkResult DxvkDevice::createHeliosOuterAllocation(
          VkDeviceSize                  bytes,
          VkMemoryPropertyFlags        memoryProperties,
          HeliosResourceAssociationV1* association) const {
    if (!m_instance->isRecordOnlyDirect())
      return VK_ERROR_FEATURE_NOT_PRESENT;
    if (!bytes || !association || !m_heliosOuterOps) {
      Logger::err(str::format("Helios outer allocation refused: ops=",
        bool(m_heliosOuterOps), " bytes=", bytes));
      return VK_ERROR_DEVICE_LOST;
    }

    *association = { };
    VkResult result = m_heliosOuterOps.allocate(
      m_heliosOuterOps.context,
      bytes,
      bool(memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT),
      bool(memoryProperties & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
      association);
    if (result != VK_SUCCESS
     || !validateHeliosOuterAssociation(*association, bytes, memoryProperties)) {
      Logger::err(str::format("Helios outer allocation refused: result=", result,
        " bytes=", bytes, " props=0x", std::hex, memoryProperties, std::dec,
        " token=", association->outer_allocation_token,
        " generation=", association->device_generation,
        " assocBytes=", association->outer_allocation_bytes,
        " flags=0x", std::hex, association->association_flags,
        " sType=0x", uint32_t(association->s_type),
        " structBytes=", std::dec, association->struct_bytes,
        " abi=", association->abi_version,
        " pkgGen=0x", std::hex, association->package_generation, std::dec,
        " cpuMap=", association->cpu_mapping ? 1 : 0));
      const uint64_t deviceGeneration = association->device_generation;
      const uint64_t outerAllocationToken = association->outer_allocation_token;
      if (result == VK_SUCCESS && deviceGeneration && outerAllocationToken) {
        void* scope = beginHeliosOuterAllocationTeardown(
          deviceGeneration, outerAllocationToken);
        VkResult teardownResult = scope
          ? finishHeliosOuterSubmit(scope, VK_SUCCESS)
          : VK_ERROR_DEVICE_LOST;
        retireHeliosOuterAllocation(deviceGeneration,
          outerAllocationToken, teardownResult);
      }
      *association = { };
      return VK_ERROR_DEVICE_LOST;
    }
    return VK_SUCCESS;
  }


  bool DxvkDevice::validateHeliosOuterAssociation(
    const HeliosResourceAssociationV1& association,
          VkDeviceSize                 bytes,
          VkMemoryPropertyFlags        memoryProperties) const {
    const bool hostVisible = memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    const bool hasCpuMapping = (association.association_flags
      & HELIOS_RESOURCE_ASSOCIATION_FLAG_CPU_MAPPING) != 0;
    const uintptr_t cpuMapping = reinterpret_cast<uintptr_t>(association.cpu_mapping);

    uint32_t failed = 0;
    if (!bytes)                                                        failed |= 1u << 0;
    if (association.s_type != HELIOS_RESOURCE_ASSOCIATION_STRUCTURE_TYPE) failed |= 1u << 1;
    if (association.struct_bytes != HELIOS_RESOURCE_ASSOCIATION_BYTES) failed |= 1u << 2;
    if (association.abi_version != HELIOS_RESOURCE_ASSOCIATION_ABI_VERSION) failed |= 1u << 3;
    if (association.reserved)                                          failed |= 1u << 4;
    if (association.package_generation != HELIOS_PACKAGE_GENERATION)   failed |= 1u << 5;
    if (!association.device_generation)                                failed |= 1u << 6;
    if (!association.outer_allocation_token)                           failed |= 1u << 7;
    if (association.outer_allocation_bytes < bytes)                    failed |= 1u << 8;
    if (association.association_flags & ~HELIOS_RESOURCE_ASSOCIATION_FLAG_MASK) failed |= 1u << 9;
    if (association.reserved1)                                         failed |= 1u << 10;
    if (!!association.cpu_mapping != hasCpuMapping)                    failed |= 1u << 11;
    if (hostVisible && !hasCpuMapping)                                 failed |= 1u << 12;
    if (hasCpuMapping && ((cpuMapping & 4095u)
      || association.outer_allocation_bytes > uint64_t(UINTPTR_MAX - cpuMapping)))
                                                                       failed |= 1u << 13;
    if (failed) {
      Logger::err(str::format("Helios association validate terms=0x", std::hex,
        failed, std::dec, " bytes=", bytes, " assocBytes=",
        association.outer_allocation_bytes, " props=0x", std::hex,
        memoryProperties, " flags=0x", association.association_flags, std::dec,
        " cpuMap=", association.cpu_mapping ? 1 : 0));
    }
    return !failed;
  }


  void* DxvkDevice::beginHeliosOuterAllocationTeardown(
          uint64_t     deviceGeneration,
          uint64_t     outerAllocationToken) const {
    if (!m_instance->isRecordOnlyDirect()
     || !deviceGeneration || !outerAllocationToken || !m_heliosOuterOps)
      return nullptr;
    return m_heliosOuterOps.teardownBegin(m_heliosOuterOps.context,
      deviceGeneration, outerAllocationToken);
  }


  VkResult DxvkDevice::retireHeliosOuterAllocation(
          uint64_t     deviceGeneration,
          uint64_t     outerAllocationToken,
          VkResult     teardownResult) const {
    if (!m_instance->isRecordOnlyDirect())
      return teardownResult;
    if (!deviceGeneration || !outerAllocationToken
     || !m_heliosOuterOps)
      return VK_ERROR_DEVICE_LOST;
    return m_heliosOuterOps.retire(m_heliosOuterOps.context,
      deviceGeneration, outerAllocationToken, teardownResult);
  }


  VkSubresourceLayout DxvkDevice::queryImageSubresourceLayout(
    const DxvkImageCreateInfo&        createInfo,
    const VkImageSubresource&         subresource) {
    VkImageFormatListCreateInfo formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };

    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.flags = createInfo.flags;
    info.imageType = createInfo.type;
    info.format = createInfo.format;
    info.extent = createInfo.extent;
    info.mipLevels = createInfo.mipLevels;
    info.arrayLayers = createInfo.numLayers;
    info.samples = createInfo.sampleCount;
    info.tiling = VK_IMAGE_TILING_LINEAR;
    info.usage = createInfo.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED;

    if (createInfo.viewFormatCount && (createInfo.viewFormatCount > 1u || createInfo.viewFormats[0] != createInfo.format)) {
      formatList.viewFormatCount = createInfo.viewFormatCount;
      formatList.pViewFormats = createInfo.viewFormats;

      info.pNext = &formatList;
    }

    VkImageSubresource2KHR subresourceInfo = { VK_STRUCTURE_TYPE_IMAGE_SUBRESOURCE_2_KHR };
    subresourceInfo.imageSubresource = subresource;

    // The exact DWM primary is plain LINEAR. Query its COLOR layout directly;
    // rowPitch and offset are the values SET_SCANOUT_BLOB must publish.

    VkDeviceImageSubresourceInfoKHR query = { VK_STRUCTURE_TYPE_DEVICE_IMAGE_SUBRESOURCE_INFO_KHR };
    query.pCreateInfo = &info;
    query.pSubresource = &subresourceInfo;

    VkSubresourceLayout2KHR layout = { VK_STRUCTURE_TYPE_SUBRESOURCE_LAYOUT_2_KHR };
    m_vkd->vkGetDeviceImageSubresourceLayoutKHR(m_vkd->device(), &query, &layout);
    return layout.subresourceLayout;
  }


  bool DxvkDevice::isUnifiedMemoryArchitecture() const {
    return m_adapter->isUnifiedMemoryArchitecture();
  }


  bool DxvkDevice::canUseGraphicsPipelineLibrary() const {
    // Without graphicsPipelineLibraryIndependentInterpolationDecoration, we
    // cannot use this effectively in many games since no client API provides
    // interpoation qualifiers in vertex shaders.
    return m_features.extGraphicsPipelineLibrary.graphicsPipelineLibrary
        && m_properties.extGraphicsPipelineLibrary.graphicsPipelineLibraryIndependentInterpolationDecoration
        && m_options.enableGraphicsPipelineLibrary != Tristate::False;
  }


  bool DxvkDevice::canUseSampleLocations(VkSampleCountFlags samples) const {
    return (m_features.extSampleLocations)
        && (m_features.extExtendedDynamicState3.extendedDynamicState3SampleLocationsEnable)
        && (m_properties.extSampleLocations.variableSampleLocations)
        && (m_properties.extSampleLocations.sampleLocationSampleCounts & samples) == samples;
  }


  bool DxvkDevice::mustTrackPipelineLifetime() const {
    switch (m_options.trackPipelineLifetime) {
      case Tristate::True:
        return canUseGraphicsPipelineLibrary();

      case Tristate::False:
        return false;

      default:
      case Tristate::Auto:
        if (!env::is32BitHostPlatform() || !canUseGraphicsPipelineLibrary())
          return false;

        // Disable lifetime tracking for drivers that do not have any
        // significant issues with 32-bit address space to begin with
        if (m_adapter->matchesDriver(VK_DRIVER_ID_MESA_RADV_KHR))
          return false;

        return true;
    }
  }


  DxvkFramebufferSize DxvkDevice::getDefaultFramebufferSize() const {
    return DxvkFramebufferSize {
      m_properties.core.properties.limits.maxFramebufferWidth,
      m_properties.core.properties.limits.maxFramebufferHeight,
      m_properties.core.properties.limits.maxFramebufferLayers };
  }


  VkPipelineStageFlags DxvkDevice::getShaderPipelineStages() const {
    VkPipelineStageFlags result = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                                | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT
                                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    
    if (m_features.core.features.geometryShader)
      result |= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    
    if (m_features.core.features.tessellationShader) {
      result |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT
             |  VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    }

    return result;
  }
  
  
  Rc<DxvkCommandList> DxvkDevice::createCommandList() {
    Rc<DxvkCommandList> cmdList = m_recycledCommandLists.retrieveObject();
    
    if (cmdList == nullptr)
      cmdList = new DxvkCommandList(this);
    
    return cmdList;
  }


  Rc<DxvkContext> DxvkDevice::createContext() {
    return new DxvkContext(this);
  }


  Rc<DxvkEvent> DxvkDevice::createGpuEvent() {
    return new DxvkEvent(this);
  }


  Rc<DxvkQuery> DxvkDevice::createGpuQuery(
          VkQueryType           type,
          VkQueryControlFlags   flags,
          uint32_t              index) {
    return new DxvkQuery(this, type, flags, index);
  }


  Rc<DxvkGpuQuery> DxvkDevice::createRawQuery(
          VkQueryType           type) {
    return m_objects.queryPool().allocQuery(type);
  }


  Rc<DxvkFence> DxvkDevice::createFence(
    const DxvkFenceCreateInfo& fenceInfo) {
    return new DxvkFence(this, fenceInfo);
  }


  Rc<DxvkBuffer> DxvkDevice::createBuffer(
    const DxvkBufferCreateInfo& createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkBuffer(this, createInfo, m_objects.memoryManager(), memoryType);
  }
  
  
  Rc<DxvkImage> DxvkDevice::createImage(
    const DxvkImageCreateInfo&  createInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(this, createInfo, m_objects.memoryManager(), memoryType);
  }


  VkImage DxvkDevice::createImageForMemoryRequirements(
    const DxvkImageCreateInfo&  createInfo,
          VkMemoryRequirements2& requirements) {
    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.flags = createInfo.flags;
    info.imageType = createInfo.type;
    info.format = createInfo.format;
    info.extent = createInfo.extent;
    info.mipLevels = createInfo.mipLevels;
    info.arrayLayers = createInfo.numLayers;
    info.samples = createInfo.sampleCount;
    info.tiling = createInfo.tiling;
    info.usage = createInfo.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = createInfo.initialLayout;

    VkImageFormatListCreateInfo formatList = {
      VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    if ((createInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)
     && createInfo.viewFormatCount) {
      formatList.viewFormatCount = createInfo.viewFormatCount;
      formatList.pViewFormats = createInfo.viewFormats;
      info.pNext = &formatList;
    }

    VkImage image = VK_NULL_HANDLE;
    VkResult vr = m_vkd->vkCreateImage(m_vkd->device(), &info, nullptr, &image);
    if (vr != VK_SUCCESS) {
      Logger::err(str::format(
        "Helios image-requirements preflight: vkCreateImage failed vr=", int32_t(vr),
        " format=", uint32_t(info.format),
        " extent=", info.extent.width, "x", info.extent.height, "x", info.extent.depth,
        " flags=0x", std::hex, info.flags,
        " usage=0x", info.usage));
      return VK_NULL_HANDLE;
    }

    VkImageMemoryRequirementsInfo2 query = {
      VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    query.image = image;
    m_vkd->vkGetImageMemoryRequirements2(
      m_vkd->device(), &query, &requirements);
    if (!requirements.memoryRequirements.size) {
      Logger::err(str::format(
        "Helios image-requirements preflight: zero requirements",
        " format=", uint32_t(info.format),
        " extent=", info.extent.width, "x", info.extent.height, "x", info.extent.depth,
        " flags=0x", std::hex, info.flags,
        " usage=0x", info.usage,
        " alignment=0x", requirements.memoryRequirements.alignment,
        " memoryTypeBits=0x", requirements.memoryRequirements.memoryTypeBits));
      m_vkd->vkDestroyImage(m_vkd->device(), image, nullptr);
      return VK_NULL_HANDLE;
    }
    return image;
  }


  Rc<DxvkImage> DxvkDevice::adoptImage(
    const DxvkImageCreateInfo&  createInfo,
          VkImage               image,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(this, createInfo,
      m_objects.memoryManager(), memoryType, image);
  }


  void DxvkDevice::destroyImageForMemoryRequirements(
          VkImage               image) {
    if (image)
      m_vkd->vkDestroyImage(m_vkd->device(), image, nullptr);
  }
  
  
  Rc<DxvkSampler> DxvkDevice::createSampler(
    const DxvkSamplerKey&         createInfo) {
    return m_objects.samplerPool().createSampler(createInfo);
  }


  DxvkLocalAllocationCache DxvkDevice::createAllocationCache(
          VkBufferUsageFlags    bufferUsage,
          VkMemoryPropertyFlags propertyFlags) {
    return m_objects.memoryManager().createAllocationCache(bufferUsage, propertyFlags);
  }


  Rc<DxvkSparsePageAllocator> DxvkDevice::createSparsePageAllocator() {
    return new DxvkSparsePageAllocator(m_objects.memoryManager());
  }


  const DxvkPipelineLayout* DxvkDevice::createBuiltInPipelineLayout(
          DxvkPipelineLayoutFlags         flags,
          VkShaderStageFlags              pushDataStages,
          VkDeviceSize                    pushDataSize,
          uint32_t                        bindingCount,
    const DxvkDescriptorSetLayoutBinding* bindings) {
    DxvkPipelineLayoutKey key(DxvkPipelineLayoutType::BuiltIn, flags);

    if (pushDataSize) {
      key.addStages(pushDataStages);

      DxvkPushDataBlock pushData(pushDataStages,
        0u, pushDataSize, sizeof(uint32_t), 0u);

      key.addPushData(pushData);
    }

    if (bindingCount) {
      DxvkDescriptorSetLayoutKey setLayoutKey;

      for (uint32_t i = 0; i < bindingCount; i++) {
        key.addStages(bindings[i].getStageMask());
        setLayoutKey.add(bindings[i]);
      }

      const auto* layout = m_objects.pipelineManager().createDescriptorSetLayout(setLayoutKey);
      key.setDescriptorSetLayouts(1, &layout);
    }

    return m_objects.pipelineManager().createPipelineLayout(key);
  }


  VkPipeline DxvkDevice::createBuiltInComputePipeline(
    const DxvkPipelineLayout*             layout,
    const util::DxvkBuiltInShaderStage&   stage) {
    auto mappingInfo = layout->getMappingInfo();

    VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    moduleInfo.codeSize = stage.size;
    moduleInfo.pCode = stage.code;

    if (canUseDescriptorHeap())
      moduleInfo.pNext = &mappingInfo;

    VkPipelineCreateFlags2CreateInfo pipelineFlags = { VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };

    if (canUseDescriptorHeap())
      pipelineFlags.flags |= VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

    if (canUseDescriptorBuffer())
      pipelineFlags.flags |= VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT;

    VkComputePipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    pipelineInfo.layout = layout->getPipelineLayout();
    pipelineInfo.basePipelineIndex = -1;

    if (pipelineFlags.flags)
      pipelineFlags.pNext = std::exchange(pipelineInfo.pNext, &pipelineFlags);

    VkPipelineShaderStageCreateInfo& stageInfo = pipelineInfo.stage;
    stageInfo = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, &moduleInfo };
    stageInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stageInfo.pName = "main";
    stageInfo.pSpecializationInfo = stage.spec;

    VkPipeline pipeline = VK_NULL_HANDLE;

    VkResult vr = m_vkd->vkCreateComputePipelines(m_vkd->device(),
      VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

    if (vr)
      throw DxvkError(str::format("Failed to create built-in compute pipeline: ", vr));

    return pipeline;
  }


  VkPipeline DxvkDevice::createBuiltInGraphicsPipeline(
    const DxvkPipelineLayout*             layout,
    const util::DxvkBuiltInGraphicsState& state) {
    constexpr size_t MaxStages = 3u;

    auto mappingInfo = layout->getMappingInfo();

    // Build shader stage infos
    small_vector<std::pair<VkShaderStageFlagBits, util::DxvkBuiltInShaderStage>, MaxStages> stages;

    if (state.vs.code) stages.push_back({ VK_SHADER_STAGE_VERTEX_BIT,   state.vs });
    if (state.gs.code) stages.push_back({ VK_SHADER_STAGE_GEOMETRY_BIT, state.gs });
    if (state.fs.code) stages.push_back({ VK_SHADER_STAGE_FRAGMENT_BIT, state.fs });

    small_vector<VkShaderModuleCreateInfo, MaxStages> moduleInfos;

    for (size_t i = 0; i < stages.size(); i++) {
      auto& info = moduleInfos.emplace_back();
      info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      info.codeSize = stages[i].second.size;
      info.pCode = stages[i].second.code;

      if (canUseDescriptorHeap())
        info.pNext = &mappingInfo;
    }

    small_vector<VkPipelineShaderStageCreateInfo, MaxStages> stageInfos;

    for (size_t i = 0; i < stages.size(); i++) {
      auto& info = stageInfos.emplace_back();
      info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      info.pNext = &moduleInfos[i];
      info.stage = stages[i].first;
      info.pName = "main";
      info.pSpecializationInfo = stages[i].second.spec;
    }

    // Attachment format infos, useful to set up state
    auto depthFormatInfo = lookupFormatInfo(state.depthFormat);

    // Find highest non-null color attachment
    uint32_t colorAttachmentCount = 0u;

    for (uint32_t i = 0u; i < MaxNumRenderTargets; i++) {
      if (state.colorFormats[i])
        colorAttachmentCount = i + 1u;
    }

    // Default vertex input state
    VkPipelineVertexInputStateCreateInfo viState = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

    // Default input assembly state using triangle list
    VkPipelineInputAssemblyStateCreateInfo iaState = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    iaState.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Default viewport state, needs to be defined even if everything is dynamic
    VkPipelineViewportStateCreateInfo vpState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };

    // Default rasterization state
    VkPipelineRasterizationStateCreateInfo rsState = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rsState.cullMode          = VK_CULL_MODE_NONE;
    rsState.frontFace         = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rsState.polygonMode       = VK_POLYGON_MODE_FILL;
    rsState.depthClampEnable  = state.depthFormat != VK_FORMAT_UNDEFINED;
    rsState.lineWidth         = 1.0f;

    // Multisample state. Enables rendering to all samples at once.
    uint32_t sampleMask = (1u << uint32_t(state.sampleCount)) - 1u;

    VkPipelineMultisampleStateCreateInfo msState = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    msState.rasterizationSamples  = state.sampleCount;
    msState.pSampleMask           = &sampleMask;
    msState.sampleShadingEnable   = VK_FALSE;
    msState.minSampleShading      = 1.0f;

    // Default depth-stencil state, enables depth and stencil write-through
    VkPipelineDepthStencilStateCreateInfo dsState = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };

    if (state.depthFormat && (depthFormatInfo->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
      dsState.depthTestEnable   = VK_TRUE;
      dsState.depthWriteEnable  = VK_TRUE;
      dsState.depthCompareOp    = VK_COMPARE_OP_ALWAYS;
    }

    if (state.depthFormat && (depthFormatInfo->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT)) {
      VkStencilOpState stencil = { };
      stencil.passOp      = VK_STENCIL_OP_REPLACE;
      stencil.failOp      = VK_STENCIL_OP_REPLACE;
      stencil.depthFailOp = VK_STENCIL_OP_REPLACE;
      stencil.compareOp   = VK_COMPARE_OP_ALWAYS;
      stencil.compareMask = 0xffffffffu;
      stencil.writeMask   = 0xffffffffu;

      dsState.stencilTestEnable = VK_TRUE;
      dsState.front       = stencil;
      dsState.back        = stencil;
    }

    // Default blend state, only used if color attachments are present
    std::array<VkPipelineColorBlendAttachmentState, MaxNumRenderTargets> cbAttachments = { };

    for (auto& cbAttachment : cbAttachments) {
      cbAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                                  | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    }

    VkPipelineColorBlendStateCreateInfo cbState = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cbState.attachmentCount = colorAttachmentCount;
    cbState.pAttachments = state.cbAttachment ? state.cbAttachment : cbAttachments.data();

    // Prepare dynamic states
    small_vector<VkDynamicState, 4> dynamicStates;
    dynamicStates.push_back(VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT);
    dynamicStates.push_back(VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT);

    for (uint32_t i = 0; i < state.dynamicStateCount; i++)
      dynamicStates.push_back(state.dynamicStates[i]);

    VkPipelineDynamicStateCreateInfo dyState = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyState.dynamicStateCount = dynamicStates.size();
    dyState.pDynamicStates = dynamicStates.data();

    // Build rendering attachment info
    VkPipelineRenderingCreateInfo renderingInfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };

    if (colorAttachmentCount) {
      renderingInfo.colorAttachmentCount = colorAttachmentCount;
      renderingInfo.pColorAttachmentFormats = state.colorFormats.data();
    }

    if (state.depthFormat && (depthFormatInfo->aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT))
      renderingInfo.depthAttachmentFormat = state.depthFormat;

    if (state.depthFormat && (depthFormatInfo->aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT))
      renderingInfo.stencilAttachmentFormat = state.depthFormat;

    VkPipelineCreateFlags2CreateInfo pipelineFlags = { VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO };

    if (canUseDescriptorHeap())
      pipelineFlags.flags |= VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT;

    if (canUseDescriptorBuffer())
      pipelineFlags.flags |= VK_PIPELINE_CREATE_2_DESCRIPTOR_BUFFER_BIT_EXT;

    VkGraphicsPipelineCreateInfo pipelineInfo = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &renderingInfo };
    pipelineInfo.stageCount = stageInfos.size();
    pipelineInfo.pStages = stageInfos.data();
    pipelineInfo.pVertexInputState = state.viState ? state.viState : &viState;
    pipelineInfo.pInputAssemblyState = state.iaState ? state.iaState : &iaState;
    pipelineInfo.pViewportState = &vpState;
    pipelineInfo.pRasterizationState = state.rsState ? state.rsState : &rsState;
    pipelineInfo.pMultisampleState = &msState;
    pipelineInfo.pDepthStencilState = state.depthFormat ? (state.dsState ? state.dsState : &dsState) : nullptr;
    pipelineInfo.pColorBlendState = colorAttachmentCount ? &cbState : nullptr;
    pipelineInfo.pDynamicState = &dyState;
    pipelineInfo.layout = layout->getPipelineLayout();
    pipelineInfo.basePipelineIndex = -1;

    if (pipelineFlags.flags)
      pipelineFlags.pNext = std::exchange(pipelineInfo.pNext, &pipelineFlags);

    VkPipeline pipeline = VK_NULL_HANDLE;

    VkResult vr = m_vkd->vkCreateGraphicsPipelines(m_vkd->device(),
      VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

    if (vr)
      throw DxvkError(str::format("Failed to create built-in graphics pipeline: ", vr));

    return pipeline;
  }


  DxvkStatCounters DxvkDevice::getStatCounters() {
    DxvkPipelineCount pipe = m_objects.pipelineManager().getPipelineCount();
    DxvkPipelineWorkerStats workers = m_objects.pipelineManager().getWorkerStats();
    
    DxvkStatCounters result;
    result.setCtr(DxvkStatCounter::PipeCountGraphics, pipe.numGraphicsPipelines);
    result.setCtr(DxvkStatCounter::PipeCountLibrary,  pipe.numGraphicsLibraries);
    result.setCtr(DxvkStatCounter::PipeCountCompute,  pipe.numComputePipelines);
    result.setCtr(DxvkStatCounter::PipeTasksDone,     workers.tasksCompleted);
    result.setCtr(DxvkStatCounter::PipeTasksTotal,    workers.tasksTotal);
    result.setCtr(DxvkStatCounter::GpuIdleTicks,      m_submissionQueue.gpuIdleTicks());

    std::lock_guard<sync::Spinlock> lock(m_statLock);
    result.merge(m_statCounters);
    return result;
  }
  
  
  Rc<DxvkShader> DxvkDevice::createCachedShader(
    const std::string&                    name,
    const DxvkIrShaderCreateInfo&         createInfo,
    const Rc<DxvkIrShaderConverter>&      converter) {
    Rc<DxvkIrShader> shader = nullptr;

    if (m_shaderCache && !converter)
      shader = m_shaderCache->lookupShader(name, createInfo);

    if (!shader && converter) {
      shader = new DxvkIrShader(createInfo, converter);

      if (m_shaderCache)
        m_shaderCache->addShader(shader);
    }

    return shader;
  }


  Rc<DxvkBuffer> DxvkDevice::importBuffer(
    const DxvkBufferCreateInfo& createInfo,
    const DxvkBufferImportInfo& importInfo,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkBuffer(this, createInfo,
      importInfo, m_objects.memoryManager(), memoryType);
  }


  Rc<DxvkImage> DxvkDevice::importImage(
    const DxvkImageCreateInfo&  createInfo,
          VkImage               image,
          VkMemoryPropertyFlags memoryType) {
    return new DxvkImage(this, createInfo, image,
      m_objects.memoryManager(), memoryType);
  }


  DxvkMemoryStats DxvkDevice::getMemoryStats(uint32_t heap) {
    return m_objects.memoryManager().getMemoryStats(heap);
  }


  DxvkSharedAllocationCacheStats DxvkDevice::getMemoryAllocationStats(DxvkMemoryAllocationStats& stats) {
    m_objects.memoryManager().getAllocationStats(stats);
    return m_objects.memoryManager().getAllocationCacheStats();
  }


  uint32_t DxvkDevice::getCurrentFrameId() const {
    return m_statCounters.getCtr(DxvkStatCounter::QueuePresentCount);
  }
  
  
  void DxvkDevice::registerShader(const Rc<DxvkShader>& shader) {
    m_objects.pipelineManager().registerShader(shader);
  }
  
  
  void DxvkDevice::requestCompileShader(
    const Rc<DxvkShader>&           shader) {
    m_objects.pipelineManager().requestCompileShader(shader);
  }


  Rc<DxvkLatencyTracker> DxvkDevice::createLatencyTracker(
    const Rc<Presenter>&            presenter) {
    if (m_options.latencySleep == Tristate::False)
      return nullptr;

    if (m_options.latencySleep == Tristate::Auto) {
      if (m_features.nvLowLatency2)
        return new DxvkReflexLatencyTrackerNv(presenter);
      else
        return nullptr;
    }

    return new DxvkBuiltInLatencyTracker(presenter,
      m_options.latencyTolerance, m_features.nvLowLatency2);
  }


  void DxvkDevice::presentImage(
    const Rc<Presenter>&            presenter,
    const Rc<DxvkLatencyTracker>&   tracker,
          uint64_t                  frameId,
          DxvkSubmitStatus*         status) {
    DxvkPresentInfo presentInfo = { };
    presentInfo.presenter = presenter;
    presentInfo.frameId = frameId;

    DxvkLatencyInfo latencyInfo;
    latencyInfo.tracker = tracker;
    latencyInfo.frameId = frameId;

    m_submissionQueue.present(presentInfo, latencyInfo, status);
    
    std::lock_guard<sync::Spinlock> statLock(m_statLock);
    m_statCounters.addCtr(DxvkStatCounter::QueuePresentCount, 1);
  }


  void DxvkDevice::submitCommandList(
    const Rc<DxvkCommandList>&      commandList,
    const Rc<DxvkLatencyTracker>&   tracker,
          uint64_t                  frameId,
          DxvkSubmitStatus*         status) {
    DxvkSubmitInfo submitInfo = { };
    submitInfo.cmdList = commandList;

    DxvkLatencyInfo latencyInfo;
    latencyInfo.tracker = tracker;
    latencyInfo.frameId = frameId;

    m_submissionQueue.submit(submitInfo, latencyInfo, status);

    std::lock_guard<sync::Spinlock> statLock(m_statLock);
    m_statCounters.merge(commandList->statCounters());
  }
  
  
  VkResult DxvkDevice::waitForSubmission(DxvkSubmitStatus* status) {
    VkResult result = status->result.load();

    if (result == VK_NOT_READY) {
      m_submissionQueue.synchronizeSubmission(status);
      result = status->result.load();
    }

    return result;
  }


  void DxvkDevice::waitForFence(sync::Fence& fence, uint64_t value) {
    if (fence.value() >= value)
      return;

    auto t0 = dxvk::high_resolution_clock::now();

    fence.wait(value);

    auto t1 = dxvk::high_resolution_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

    m_statCounters.addCtr(DxvkStatCounter::GpuSyncCount, 1);
    m_statCounters.addCtr(DxvkStatCounter::GpuSyncTicks, us.count());
  }


  void DxvkDevice::waitForResource(const DxvkPagedResource& resource, DxvkAccess access) {
    if (resource.isInUse(access)) {
      auto t0 = dxvk::high_resolution_clock::now();
      bool reportedStuck = false;

      // HELIOS: bail out once the device is lost — after loss nothing is
      // guaranteed to release the resource's usage refs, and an unbounded
      // wait here wedges the calling thread for the life of the process
      // (2026-07-05: dwm compositor stuck in Map for 25+ minutes). Mapped
      // contents are undefined on a lost device anyway.
      m_submissionQueue.synchronizeUntil([this, &resource, access, &reportedStuck] {
        if (!resource.isInUse(access)
         || m_submissionQueue.getLastError() == VK_ERROR_DEVICE_LOST)
          return true;

        // HELIOS: name the stalled wait instead of hanging silently. Both
        // submission stages empty means nothing is queued and nothing is in
        // flight, so no completion can come from this queue as things stand,
        // and the reference must be held by a command list that has not been
        // submitted.
        //
        // Whether that is terminal depends on who else can submit it:
        //  - single-threaded caller  -> terminal. ROADMAP WS1 defect 0w: the
        //    Start menu's XAML thread parked here forever inside
        //    D3D11Initializer::SyncSharedTexture and the only thread that
        //    could have submitted the holding list was the blocked one.
        //    Diagnosed only by minidumping the process and decoding
        //    m_useCount by hand; this line makes the next one say so itself.
        //  - multi-threaded caller   -> often transient. dwm trips this once
        //    per start and recovers, because another of its threads submits
        //    the list and releases the reference.
        // So this is a loud warning, NOT a proof of deadlock. A repeating or
        // never-cleared occurrence is the one that matters.
        //
        // Deliberately does NOT change behaviour — bounding the wait would
        // return with the reference still held and hand the caller a resource
        // whose pending work never completed. The fix belongs at the site that
        // holds the reference.
        if (m_submissionQueue.isDrainedLocked() && !reportedStuck) {
          reportedStuck = true;

          Logger::err(str::format(
            "DxvkDevice: waitForResource STALLED: resource ", &resource,
            " still in use (read=", resource.isInUse(DxvkAccess::Read),
            " write=", resource.isInUse(DxvkAccess::Write),
            " trackId=", resource.getTrackId(),
            ") with the submission queue fully drained. Nothing pending can release "
            "it; unless another thread submits the command list holding it, this "
            "wait cannot return. occurrences=",
            s_unsatisfiableWaits.fetch_add(1u) + 1u));
        }

        return false;
      });

      if (resource.isInUse(access))
        Logger::warn("DxvkDevice: waitForResource aborted: device lost with resource still in use");

      auto t1 = dxvk::high_resolution_clock::now();
      auto us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);

      std::lock_guard<sync::Spinlock> lock(m_statLock);
      m_statCounters.addCtr(DxvkStatCounter::GpuSyncCount, 1);
      m_statCounters.addCtr(DxvkStatCounter::GpuSyncTicks, us.count());
    }
  }
  
  
  void DxvkDevice::waitForIdle() {
    m_submissionQueue.waitForIdle();

    // The record-only submission queue already performed the one exact HQC1
    // join and released the lists whose resource refs it protected.
    if (m_instance->isRecordOnlyDirect())
      return;

    m_submissionQueue.lockDeviceQueue();

    if (m_vkd->vkDeviceWaitIdle(m_vkd->device()) != VK_SUCCESS)
      Logger::err("DxvkDevice: waitForIdle: Operation failed");

    m_submissionQueue.unlockDeviceQueue();
  }
  
  
  DxvkDevicePerfHints DxvkDevice::getPerfHints() {
    DxvkDevicePerfHints hints;

    // RADV properly fuses depth-stencil copies now
    hints.preferFbDepthStencilCopy = m_features.extShaderStencilExport
      && (m_adapter->matchesDriver(VK_DRIVER_ID_MESA_RADV_KHR, Version(), Version(25, 3, 99))
       || m_adapter->matchesDriver(VK_DRIVER_ID_AMD_OPEN_SOURCE_KHR)
       || m_adapter->matchesDriver(VK_DRIVER_ID_AMD_PROPRIETARY_KHR));

    // Older Nvidia drivers sometimes use the wrong format
    // to interpret the clear color in render pass clears.
    hints.renderPassClearFormatBug = m_adapter->matchesDriver(
      VK_DRIVER_ID_NVIDIA_PROPRIETARY, Version(), Version(560, 28, 3));

    // There's a similar bug that affects resolve attachments
    hints.renderPassResolveFormatBug = m_adapter->matchesDriver(
      VK_DRIVER_ID_NVIDIA_PROPRIETARY);

    // On tilers we need to respect render passes some more. Most of
    // these drivers probably can't run DXVK anyway, but might as well
    bool tilerMode = m_adapter->matchesDriver(VK_DRIVER_ID_MESA_TURNIP)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_QUALCOMM_PROPRIETARY)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_MESA_HONEYKRISP)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_MOLTENVK)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_MESA_PANVK)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_ARM_PROPRIETARY)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_MESA_V3DV)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_BROADCOM_PROPRIETARY)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA)
                  || m_adapter->matchesDriver(VK_DRIVER_ID_IMAGINATION_PROPRIETARY);

    applyTristate(tilerMode, m_options.tilerMode);
    hints.preferRenderPassOps = tilerMode;
    hints.preferCachedMemory = tilerMode;

    // Compute-based mip generation has some potential for performance
    // regressions or driver issues. Just enable it on Nvidia and RADV
    // (GFX10+) for now, where it is proven to work.
    hints.preferComputeMipGen = (m_adapter->matchesDriver(VK_DRIVER_ID_NVIDIA_PROPRIETARY_KHR)
                             || (m_adapter->matchesDriver(VK_DRIVER_ID_MESA_RADV)
                              && m_properties.vk13.minSubgroupSize == 32u));

    // On AMD we can expect it to be optimal to simply pass the heap offset
    // to descriptor memory through as-is to avoid some ALU. Some other
    // vendors are more sensitive towards knowing descriptor alignment.
    hints.preferDescriptorByteOffsets = m_adapter->matchesDriver(VK_DRIVER_ID_MESA_RADV)
                                     || m_adapter->matchesDriver(VK_DRIVER_ID_AMD_OPEN_SOURCE)
                                     || m_adapter->matchesDriver(VK_DRIVER_ID_AMD_PROPRIETARY);

    return hints;
  }


  void DxvkDevice::recycleCommandList(const Rc<DxvkCommandList>& cmdList) {
    m_recycledCommandLists.returnObject(cmdList);
  }


  void DxvkDevice::determineShaderOptions() {
    m_shaderOptions.minStorageBufferAlignment =
      m_properties.core.properties.limits.minStorageBufferOffsetAlignment;

    if (m_features.vk12.shaderFloat16)
      m_shaderOptions.flags.set(DxvkShaderCompileFlag::Supports16BitArithmetic);

    if (m_features.vk11.storagePushConstant16 && m_features.vk12.storagePushConstant8)
      m_shaderOptions.flags.set(DxvkShaderCompileFlag::SupportsSubDwordPushData);

    // Need to tag typed storage image loads with the format on some devices
    auto r32Features = getFormatFeatures(VK_FORMAT_R32_SFLOAT).optimal
                     & getFormatFeatures(VK_FORMAT_R32_UINT).optimal
                     & getFormatFeatures(VK_FORMAT_R32_SINT).optimal;

    if (!(r32Features & VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT))
      m_shaderOptions.flags.set(DxvkShaderCompileFlag::TypedR32LoadRequiresFormat);

    // Intel's hardware sin/cos is so inaccurate that it causes rendering issues in some games
    bool lowerSinCos = m_adapter->matchesDriver(VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA)
                    || m_adapter->matchesDriver(VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS);
    applyTristate(lowerSinCos, m_options.lowerSinCos);


    if (lowerSinCos)
      m_shaderOptions.flags.set(DxvkShaderCompileFlag::LowerSinCos);

    // RADV generally does the right thing for f32tof16 and int conversions by default
    if (!m_adapter->matchesDriver(VK_DRIVER_ID_MESA_RADV)) {
      m_shaderOptions.flags.set(
        DxvkShaderCompileFlag::LowerFtoI,
        DxvkShaderCompileFlag::LowerF32toF16);
    }

    // On AMD, push constant BDA will not be worse than going through a descriptor
    if (m_adapter->matchesDriver(VK_DRIVER_ID_MESA_RADV)
     || m_adapter->matchesDriver(VK_DRIVER_ID_AMD_OPEN_SOURCE)
     || m_adapter->matchesDriver(VK_DRIVER_ID_AMD_PROPRIETARY))
      m_shaderOptions.flags.set(DxvkShaderCompileFlag::LowerInBoundsCbvToBda);

    // Converting unsigned integers to float should return an unsigned float,
    // but Nvidia drivers prior to 580 don't agree
    if (m_adapter->matchesDriver(VK_DRIVER_ID_NVIDIA_PROPRIETARY, Version(), Version(580u, 0u, 0u)))
      m_shaderOptions.flags.set(DxvkShaderCompileFlag::LowerItoF);

    // Forward UBO device limit as-is
    m_shaderOptions.maxUniformBufferSize = m_properties.core.properties.limits.maxUniformBufferRange;
    m_shaderOptions.maxUniformBufferCount = m_properties.core.properties.limits.maxPerStageDescriptorUniformBuffers < MaxNumUniformBufferSlots
      ? int32_t(m_properties.core.properties.limits.maxPerStageDescriptorUniformBuffers)
      : -1;

    // ANV up to mesa 25.0.2 breaks when we *don't* explicitly write point size
    if (m_adapter->matchesDriver(VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA, Version(), Version(25, 0, 3)))
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::ExportPointSize);

    if (m_features.nvRawAccessChains.shaderRawAccessChains)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsNvRawAccessChains);

    // Mesa drivers generally optimize large constant arrays to a buffer, some other
    // drivers do not and suffer a significant performance loss. Enable lowering on
    // those drivers.
    if (!m_adapter->matchesDriver(VK_DRIVER_ID_MESA_RADV)
     && !m_adapter->matchesDriver(VK_DRIVER_ID_MESA_NVK)
     && !m_adapter->matchesDriver(VK_DRIVER_ID_MESA_TURNIP)
     && !m_adapter->matchesDriver(VK_DRIVER_ID_MESA_HONEYKRISP)
     && !m_adapter->matchesDriver(VK_DRIVER_ID_MESA_LLVMPIPE)
     && !m_adapter->matchesDriver(VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA))
      m_shaderOptions.flags.set(DxvkShaderCompileFlag::LowerConstantArrays);

    // Set up float control feature flags
    if (m_properties.vk12.shaderSignedZeroInfNanPreserveFloat16)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsSzInfNanPreserve16);
    if (m_properties.vk12.shaderSignedZeroInfNanPreserveFloat32)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsSzInfNanPreserve32);
    if (m_properties.vk12.shaderSignedZeroInfNanPreserveFloat64)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsSzInfNanPreserve64);

    if (m_properties.vk12.shaderRoundingModeRTEFloat16)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsRte16);
    if (m_properties.vk12.shaderRoundingModeRTEFloat32)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsRte32);
    if (m_properties.vk12.shaderRoundingModeRTEFloat64)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsRte64);

    if (m_properties.vk12.shaderRoundingModeRTZFloat16)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsRtz16);
    if (m_properties.vk12.shaderRoundingModeRTZFloat32)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsRtz32);
    if (m_properties.vk12.shaderRoundingModeRTZFloat64)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsRtz64);

    if (m_properties.vk12.shaderDenormFlushToZeroFloat16)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsDenormFlush16);
    if (m_properties.vk12.shaderDenormFlushToZeroFloat32)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsDenormFlush32);
    if (m_properties.vk12.shaderDenormFlushToZeroFloat64)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsDenormFlush64);

    if (m_properties.vk12.shaderDenormPreserveFloat16)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsDenormPreserve16);
    if (m_properties.vk12.shaderDenormPreserveFloat32)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsDenormPreserve32);
    if (m_properties.vk12.shaderDenormPreserveFloat64)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsDenormPreserve64);

    if (m_properties.vk12.roundingModeIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::IndependentRoundMode);

    if (m_properties.vk12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::IndependentDenormMode);

    if (m_features.khrShaderFloatControls2.shaderFloatControls2)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsFloatControls2);

    if (canUseDescriptorHeap())
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsDescriptorHeap);

    // Set up resource indexing flags
    if (m_features.core.features.shaderUniformBufferArrayDynamicIndexing &&
        m_features.core.features.shaderSampledImageArrayDynamicIndexing &&
        m_features.core.features.shaderStorageBufferArrayDynamicIndexing &&
        m_features.core.features.shaderStorageImageArrayDynamicIndexing &&
        m_features.vk12.shaderUniformTexelBufferArrayDynamicIndexing &&
        m_features.vk12.shaderStorageTexelBufferArrayDynamicIndexing &&
        m_features.vk12.shaderUniformBufferArrayNonUniformIndexing &&
        m_features.vk12.shaderSampledImageArrayNonUniformIndexing &&
        m_features.vk12.shaderStorageBufferArrayNonUniformIndexing &&
        m_features.vk12.shaderStorageImageArrayNonUniformIndexing &&
        m_features.vk12.shaderUniformTexelBufferArrayNonUniformIndexing &&
        m_features.vk12.shaderStorageTexelBufferArrayNonUniformIndexing)
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsResourceIndexing);

    // Descriptor heap implicitly also enables resource indexing
    if (canUseDescriptorHeap())
      m_shaderOptions.spirv.set(DxvkShaderSpirvFlag::SupportsResourceIndexing);
  }


  void DxvkDevice::logBindingModel() {
    const char* descriptorModel = "Legacy";

    if (canUseDescriptorHeap())
      descriptorModel = "Descriptor heap";
    else if (canUseDescriptorBuffer())
      descriptorModel = "Descriptor buffer";

    Logger::info(str::format("Binding model: ", descriptorModel));
  }

}
