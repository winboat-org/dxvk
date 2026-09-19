#pragma once

#include <vulkan/vulkan_core.h>
#include <vector>

namespace dxvk {

  // Owned, exact template for the vehicle's dedicated external-memory source.
  // The input and its pNext/array pointers are borrowed only during init.
  // This seam currently supports ordinary exclusive, single-layer WSI images;
  // unsupported creation chains must fail the import, never be reconstructed
  // from D3D11 bind flags. External-memory info is added by the import backend.
  class DxvkHeliosImageImport {
  public:
    bool init(const VkImageCreateInfo& source) {
      constexpr VkImageCreateFlags supportedFlags = VK_IMAGE_CREATE_ALIAS_BIT
        | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
      if (source.sType != VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO
       || source.imageType != VK_IMAGE_TYPE_2D
       || source.tiling != VK_IMAGE_TILING_OPTIMAL
       || source.sharingMode != VK_SHARING_MODE_EXCLUSIVE
       || source.initialLayout != VK_IMAGE_LAYOUT_UNDEFINED
       || source.format == VK_FORMAT_UNDEFINED
       || !source.extent.width || !source.extent.height || source.extent.depth != 1
       || source.mipLevels != 1 || source.arrayLayers != 1
       || source.samples != VK_SAMPLE_COUNT_1_BIT
       || (source.flags & ~supportedFlags)
       || !(source.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
       || (source.queueFamilyIndexCount && !source.pQueueFamilyIndices))
        return false;

      m_info = source;
      m_info.pNext = nullptr;
      m_families.clear();
      m_formats.clear();
      m_formatList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
      bool haveFormatList = false;
      bool haveExternal = false;
      for (auto* next = static_cast<const VkBaseInStructure*>(source.pNext);
           next; next = next->pNext) {
        switch (next->sType) {
          case VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: {
            const auto* ext = reinterpret_cast<const VkExternalMemoryImageCreateInfo*>(next);
            // New WSI sources name DMA_BUF explicitly, matching the renderer
            // resource-id allocation/import. Keep the older OPAQUE_FD template
            // ABI accepted, but never accept mixed or unrelated handle types.
            if (haveExternal
             || (ext->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
              && ext->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
              return false;
            haveExternal = true;
          } break;
          case VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO: {
            const auto* list = reinterpret_cast<const VkImageFormatListCreateInfo*>(next);
            if (haveFormatList || (list->viewFormatCount && !list->pViewFormats))
              return false;
            haveFormatList = true;
            if (list->viewFormatCount)
              m_formats.assign(list->pViewFormats, list->pViewFormats + list->viewFormatCount);
          } break;
          default:
            // Compression-control, protected/concurrent images, etc. require
            // matching helper-device feature/ownership support. Refuse loudly
            // through the caller's existing import-failure counter.
            return false;
        }
      }

      if (source.queueFamilyIndexCount)
        m_families.assign(source.pQueueFamilyIndices,
          source.pQueueFamilyIndices + source.queueFamilyIndexCount);
      m_info.pQueueFamilyIndices = m_families.empty() ? nullptr : m_families.data();
      if (haveFormatList) {
        m_formatList.viewFormatCount = uint32_t(m_formats.size());
        m_formatList.pViewFormats = m_formats.empty() ? nullptr : m_formats.data();
        m_info.pNext = &m_formatList;
      }
      return true;
    }

    const VkImageCreateInfo& info() const { return m_info; }
    uint32_t formatCount() const { return uint32_t(m_formats.size()); }
    const VkFormat* formats() const { return m_formats.empty() ? nullptr : m_formats.data(); }

    DxvkHeliosImageImport() = default;
    DxvkHeliosImageImport(const DxvkHeliosImageImport&) = delete;
    DxvkHeliosImageImport& operator=(const DxvkHeliosImageImport&) = delete;

  private:
    VkImageCreateInfo m_info = { };
    VkImageFormatListCreateInfo m_formatList = { };
    std::vector<uint32_t> m_families;
    std::vector<VkFormat> m_formats;
  };
}
