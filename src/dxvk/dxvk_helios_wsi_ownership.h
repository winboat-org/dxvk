#pragma once
#include <vulkan/vulkan_core.h>

namespace dxvk {
  // The producer and helper are different Vulkan instances on the same host
  // device/driver. Keep GENERAL on both halves, with the whole source range.
  inline VkImageMemoryBarrier2 heliosWsiSourceBarrier(
      VkImage image, VkImageSubresourceRange range, uint32_t family, bool acquire) {
    VkImageMemoryBarrier2 barrier = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    barrier.srcStageMask = acquire ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = acquire ? 0 : VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.dstStageMask = acquire ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE;
    barrier.dstAccessMask = acquire ? VK_ACCESS_2_TRANSFER_READ_BIT : 0;
    barrier.oldLayout = barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = acquire ? VK_QUEUE_FAMILY_EXTERNAL : family;
    barrier.dstQueueFamilyIndex = acquire ? family : VK_QUEUE_FAMILY_EXTERNAL;
    barrier.image = image;
    barrier.subresourceRange = range;
    return barrier;
  }
}
