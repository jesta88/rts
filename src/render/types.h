#pragma once

#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;

VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VkImageView)
VK_DEFINE_HANDLE(VkSampler)

#undef VK_DEFINE_HANDLE