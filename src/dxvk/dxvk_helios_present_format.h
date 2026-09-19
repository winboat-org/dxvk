#pragma once

#include <vulkan/vulkan_core.h>

namespace dxvk {
  // WSI presents encoded bytes. sRGB sources use an UNORM flip-model
  // destination, but a compatible transfer preserves the bytes without
  // sampling (and without requiring SAMPLED usage on the dedicated source).
  inline bool heliosPresentCopyPreservesBytes(VkFormat dst, VkFormat src) {
    return dst == src
      || (dst == VK_FORMAT_B8G8R8A8_UNORM && src == VK_FORMAT_B8G8R8A8_SRGB)
      || (dst == VK_FORMAT_B8G8R8A8_SRGB && src == VK_FORMAT_B8G8R8A8_UNORM)
      || (dst == VK_FORMAT_R8G8B8A8_UNORM && src == VK_FORMAT_R8G8B8A8_SRGB)
      || (dst == VK_FORMAT_R8G8B8A8_SRGB && src == VK_FORMAT_R8G8B8A8_UNORM);
  }
}
