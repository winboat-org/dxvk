#include "../src/dxvk/dxvk_helios_image_import.h"
#include "../src/dxvk/dxvk_helios_present_format.h"
#include "../src/dxvk/dxvk_helios_wsi_ownership.h"
#include <cstdio>
#include <cstdlib>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

static VkImageCreateInfo sourceInfo() {
  VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  ci.flags = VK_IMAGE_CREATE_ALIAS_BIT;
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_B8G8R8A8_UNORM;
  ci.extent = { 941, 1030, 1 };
  ci.mipLevels = 1;
  ci.arrayLayers = 1;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  return ci;
}

int main() {
  const VkImage image = (VkImage)(uintptr_t)0x1234;
  const VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  const auto acquire = dxvk::heliosWsiSourceBarrier(image, range, 7, true);
  const auto release = dxvk::heliosWsiSourceBarrier(image, range, 7, false);
  CHECK(acquire.image == image && release.image == image);
  CHECK(acquire.oldLayout == VK_IMAGE_LAYOUT_GENERAL && acquire.newLayout == release.oldLayout);
  CHECK(release.newLayout == VK_IMAGE_LAYOUT_GENERAL);
  CHECK(acquire.srcQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL && acquire.dstQueueFamilyIndex == 7);
  CHECK(release.srcQueueFamilyIndex == 7 && release.dstQueueFamilyIndex == VK_QUEUE_FAMILY_EXTERNAL);
  CHECK(!acquire.srcAccessMask && acquire.dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);
  CHECK(release.srcAccessMask == VK_ACCESS_2_MEMORY_READ_BIT && !release.dstAccessMask);
  CHECK(acquire.subresourceRange.aspectMask == range.aspectMask && release.subresourceRange.layerCount == 1);
  dxvk::DxvkHeliosImageImport plain;
  auto ci = sourceInfo();
  CHECK(plain.init(ci));
  CHECK(plain.info().flags == ci.flags && plain.info().usage == ci.usage);
  CHECK(plain.info().format == ci.format && plain.info().extent.height == 1030);
  CHECK(!plain.info().pNext && !plain.formatCount());
  dxvk::DxvkHeliosImageImport srgb;
  ci.format = VK_FORMAT_B8G8R8A8_SRGB;
  CHECK(srgb.init(ci));
  CHECK(srgb.info().format == VK_FORMAT_B8G8R8A8_SRGB);
  CHECK(!(srgb.info().usage & VK_IMAGE_USAGE_SAMPLED_BIT));
  CHECK(dxvk::heliosPresentCopyPreservesBytes(VK_FORMAT_B8G8R8A8_UNORM, ci.format));
  CHECK(dxvk::heliosPresentCopyPreservesBytes(ci.format, VK_FORMAT_B8G8R8A8_UNORM));
  CHECK(dxvk::heliosPresentCopyPreservesBytes(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB));
  CHECK(dxvk::heliosPresentCopyPreservesBytes(VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM));
  CHECK(!dxvk::heliosPresentCopyPreservesBytes(VK_FORMAT_R8G8B8A8_UNORM, ci.format));
  CHECK(!dxvk::heliosPresentCopyPreservesBytes(VK_FORMAT_A2B10G10R10_UNORM_PACK32, ci.format));

  dxvk::DxvkHeliosImageImport owned;
  {
    uint32_t families[] = { 0 };
    VkFormat viewFormats[] = { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB };
    VkImageFormatListCreateInfo list = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    list.viewFormatCount = 2;
    list.pViewFormats = viewFormats;
    VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    ext.pNext = &list;
    ci = sourceInfo();
    ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    ci.pNext = &ext;
    ci.queueFamilyIndexCount = 1;
    ci.pQueueFamilyIndices = families;
    CHECK(owned.init(ci));
    CHECK(ci.pNext == &ext && ext.pNext == &list && list.pViewFormats == viewFormats);
    families[0] = 99;
    viewFormats[0] = VK_FORMAT_UNDEFINED;
    CHECK(owned.info().pQueueFamilyIndices[0] == 0);
    CHECK(owned.formats()[0] == VK_FORMAT_B8G8R8A8_UNORM);
  }
  const auto* copiedList = static_cast<const VkImageFormatListCreateInfo*>(owned.info().pNext);
  CHECK(copiedList && copiedList->viewFormatCount == 2 && !copiedList->pNext);
  CHECK(copiedList->pViewFormats[1] == VK_FORMAT_B8G8R8A8_SRGB);
  CHECK(owned.info().flags == (VK_IMAGE_CREATE_ALIAS_BIT | VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT));

  dxvk::DxvkHeliosImageImport rejected;
  ci = sourceInfo(); ci.sharingMode = VK_SHARING_MODE_CONCURRENT;
  CHECK(!rejected.init(ci));
  ci = sourceInfo(); ci.flags |= VK_IMAGE_CREATE_PROTECTED_BIT;
  CHECK(!rejected.init(ci));
  ci = sourceInfo(); ci.queueFamilyIndexCount = 1;
  CHECK(!rejected.init(ci));
  VkImageFormatListCreateInfo badList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
  badList.viewFormatCount = 1;
  ci = sourceInfo(); ci.pNext = &badList;
  CHECK(!rejected.init(ci));
  badList.viewFormatCount = 0; badList.pNext = &badList;
  CHECK(!rejected.init(ci));
  VkExternalMemoryImageCreateInfo badExt = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
  badExt.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
  ci.pNext = &badExt;
  // The corrected WSI producer uses the resource-id import's actual handle
  // type. OPAQUE_FD remains accepted above for older helper callers.
  CHECK(rejected.init(ci));
  CHECK(rejected.info().format == ci.format && rejected.info().usage == ci.usage);
  badExt.handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  CHECK(!rejected.init(ci));
  badExt.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  CHECK(!rejected.init(ci));
  VkImageCompressionControlEXT unsupported = { VK_STRUCTURE_TYPE_IMAGE_COMPRESSION_CONTROL_EXT };
  ci.pNext = &unsupported;
  CHECK(!rejected.init(ci));
  std::puts("PASS: exact vehicle image template, deep copies, ownership pair and loud refusals");
}
