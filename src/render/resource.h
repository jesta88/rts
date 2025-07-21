#pragma once

#include "../system/core.h"
#include "types.h"

#define WC_MAX_BINDLESS_RESOURCES 16384
#define WC_MAX_MESHES 4096
#define WC_MAX_MATERIALS 1024

typedef struct WC_Buffer WC_Buffer;
typedef struct WC_Texture WC_Texture;
typedef struct WC_GpuResources WC_GpuResources;

// Mesh data stored in GPU buffers
typedef struct {
	uint32_t vertexOffset;
	uint32_t vertexCount;
	uint32_t indexOffset;
	uint32_t indexCount;
	uint32_t materialIndex;
	float boundingSphere[4]; // xyz center, w radius
} WC_GpuMeshData;

// Material data
typedef struct {
	uint32_t albedoTextureIndex;
	uint32_t normalTextureIndex;
	uint32_t metallicRoughnessTextureIndex;
	uint32_t pad;
} WC_GpuMaterialData;

// Instance data for draw indirect
typedef struct {
	float transform[16]; // 4x4 matrix
	uint32_t meshIndex;
	uint32_t instanceID;
	uint32_t pad[2];
} WC_GpuInstanceData;

int wc_gpu_resource_init(VkDevice device, VmaAllocator allocator);
void wc_gpu_resource_quit();

uint32_t wc_gpu_resource_add_mesh(
	const float* vertices,
	uint32_t vertexCount,
	uint32_t vertexStride,
	const uint32_t* indices,
	uint32_t indexCount,
	uint32_t materialIndex,
	const float* boundingSphere
);
uint32_t wc_gpu_resource_add_texture(VkImageView imageView, VkSampler sampler);

void wc_gpu_resource_update_descriptors(void);