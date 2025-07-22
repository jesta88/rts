#include "render.h"

#include "../system/app.h"
#include "../system/memory.h"
#include "allocator.h"
#include "resource.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdio.h>
#include <string.h>

typedef struct
{
	float viewMatrix[16];
	float projMatrix[16];
	float viewProjMatrix[16];
	float frustumPlanes[24]; // 6 planes * 4 floats
	uint32_t screenWidth;
	uint32_t screenHeight;
	uint32_t frameIndex;
	uint32_t pad;
} WC_GpuSceneData;

typedef struct
{
	// Render passes
	VkRenderPass visibilityPass;
	VkRenderPass shadingPass;

	// Pipelines
	VkPipeline visibilityPipeline;
	VkPipeline shadingPipeline;
	VkPipelineLayout pipelineLayout;

	// Visibility buffer (stores instanceID + triangleID)
	VkImage visibilityImage;
	VmaAllocation visibilityAllocation;
	VkImageView visibilityView;

	// Depth buffer
	VkImage depthImage;
	VmaAllocation depthAllocation;
	VkImageView depthView;

	// Scene data buffer
	VkBuffer sceneDataBuffer;
	VmaAllocation sceneDataAllocation;

	// Command buffers
	VkCommandBuffer commandBuffers[2]; // Double buffering

	// Framebuffers
	VkFramebuffer visibilityFramebuffer;
	VkFramebuffer* shadingFramebuffers; // One per swapchain image

	// Sync objects
	VkSemaphore imageAvailableSemaphores[2];
	VkSemaphore renderFinishedSemaphores[2];
	VkFence inFlightFences[2];

	uint32_t currentFrame;
	VkExtent2D extent;

	WC_GpuResources* bindlessResources;
	VmaAllocator allocator;
} WC_VisibilityBuffer;

typedef struct WC_QueueFamilyIndices
{
	int graphics_family;
	int present_family;
	int compute_family;
	int transfer_family;
} WC_QueueFamilyIndices;

// Instance and device
static VkInstance s_instance;
static VkDebugUtilsMessengerEXT debugMessenger;
static VkSurfaceKHR surface;
static VkPhysicalDevice physicalDevice;
static WC_QueueFamilyIndices s_queue_family_indices;
static VkDevice device;
static VkQueue graphicsQueue;
static VkQueue presentQueue;
static uint32_t graphicsQueueFamilyIndex;
static uint32_t presentQueueFamilyIndex;

// Allocator
static VmaAllocator allocator;

// Swapchain
static VkSwapchainKHR swapchain;
static VkExtent2D swapchainExtent;
static uint32_t swapchainImageCount;
static VkImage* swapchainImages;
static VkImageView* swapchainImageViews;
static VkRenderPass renderPass;
static VkFramebuffer* swapchainFramebuffers;

// Commands
static VkCommandPool commandPool;
static VkCommandBuffer* commandBuffers;

// Descriptor and pipeline
VkDescriptorSetLayout descriptorSetLayout;
VkDescriptorPool descriptorPool;
VkDescriptorSet descriptorSet;
VkPipelineLayout pipelineLayout;
VkPipeline meshPipeline;

// Buffers
VkBuffer transformBuffer;
VmaAllocation transformAlloc;
VkBuffer visibilityBuffer;
VmaAllocation visibilityAlloc;

const char* s_validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
#ifdef NDEBUG
const int s_enable_validation = 0;
#else
const int s_enable_validation = 1;
#endif

// Required device extensions
const char* s_device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_MESH_SHADER_EXTENSION_NAME,
									 VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
									 VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};

int createInstance(void);
int setupDebugMessenger(void);
int createSurface(void);
int pickPhysicalDevice(void);
int createLogicalDevice(void);

int createSwapchain(void);
int createImageViews(void);
int createRenderPass(void);
int createFramebuffers(void);

void createDescriptorSetLayout(void);
void createDescriptorPool(void);
void createDescriptorSet(void);
void createPipeline(void);
void updateDescriptorSet(void);

int createCommandPool(void);
int createCommandBuffers(void);

int createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VkBuffer* buffer,
				 VmaAllocation* allocation);
VkShaderModule loadShaderModule(VkDevice device, const char* filepath);

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
													VkDebugUtilsMessageTypeFlagsEXT messageType,
													const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
	SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Vulkan: %s\n", pCallbackData->pMessage);
	return VK_FALSE;
}

int wc_render_init(void)
{
	int window_width, window_height;
	wc_app_get_window_size(&window_width, &window_height);

	if (volkInitialize() != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to initialize volk\n");
		return EXIT_FAILURE;
	}

	createInstance();
	volkLoadInstance(s_instance);
	setupDebugMessenger();
	createSurface();
	pickPhysicalDevice();
	createLogicalDevice();
	volkLoadDevice(device);

	VmaAllocatorCreateInfo allocator_create_info = {};
	allocator_create_info.physicalDevice = physicalDevice;
	allocator_create_info.device = device;
	allocator_create_info.instance = s_instance;
	allocator_create_info.vulkanApiVersion = VK_API_VERSION_1_4;
	if (vmaCreateAllocator(&allocator_create_info, &allocator) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Vulkan memory allocator\n");
		return EXIT_FAILURE;
	}

	createSwapchain();
	createImageViews();
	createRenderPass();
	createFramebuffers();

	createDescriptorSetLayout();
	createPipeline();
	createDescriptorPool();
	createDescriptorSet();
	updateDescriptorSet();

	createCommandPool();
	createCommandBuffers();

	return 0;
}

void wc_render_draw(void)
{
	uint32_t imageIndex;
	vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, &imageIndex);
	VkSubmitInfo submitInfo = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &commandBuffers[imageIndex];
	vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
	VkPresentInfoKHR presentInfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &swapchain;
	presentInfo.pImageIndices = &imageIndex;
	vkQueuePresentKHR(presentQueue, &presentInfo);
	vkQueueWaitIdle(presentQueue);
}

void wc_render_quit(void)
{
	vkDeviceWaitIdle(device);
	vmaDestroyBuffer(allocator, transformBuffer, transformAlloc);
	vmaDestroyBuffer(allocator, visibilityBuffer, visibilityAlloc);

	vkDestroyPipeline(device, meshPipeline, NULL);
	vkDestroyPipelineLayout(device, pipelineLayout, NULL);
	vkDestroyDescriptorPool(device, descriptorPool, NULL);
	vkDestroyDescriptorSetLayout(device, descriptorSetLayout, NULL);

	vkFreeCommandBuffers(device, commandPool, swapchainImageCount, commandBuffers);
	vkDestroyCommandPool(device, commandPool, NULL);

	for (uint32_t i = 0; i < swapchainImageCount; i++)
	{
		vkDestroyFramebuffer(device, swapchainFramebuffers[i], NULL);
		vkDestroyImageView(device, swapchainImageViews[i], NULL);
	}
	wc_free(swapchainFramebuffers);
	wc_free(swapchainImageViews);
	wc_free(swapchainImages);

	vkDestroyRenderPass(device, renderPass, NULL);
	vkDestroySwapchainKHR(device, swapchain, NULL);
	vmaDestroyAllocator(allocator);
	vkDestroyDevice(device, NULL);
	if (s_enable_validation)
		vkDestroyDebugUtilsMessengerEXT(s_instance, debugMessenger, NULL);
	vkDestroySurfaceKHR(s_instance, surface, NULL);
	vkDestroyInstance(s_instance, NULL);
}

int createInstance(void)
{
	// Application info
	VkApplicationInfo application_info = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application_info.pApplicationName = "RTS Renderer";
	application_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	application_info.pEngineName = "CustomEngine";
	application_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	application_info.apiVersion = VK_API_VERSION_1_4;

	uint32_t required_instance_extension_count;
	const char* const* required_instance_extensions = SDL_Vulkan_GetInstanceExtensions(&required_instance_extension_count);

	if (required_instance_extensions == NULL)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to get Vulkan instance extensions\n");
		return EXIT_FAILURE;
	}

	uint32_t extension_count = required_instance_extension_count;
	if (s_enable_validation)
		extension_count++;

	const char** extensions = wc_malloc(extension_count * sizeof(const char*));
	int first_extension_index = 0;
	if (s_enable_validation)
	{
		extensions[0] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		first_extension_index++;
	}
	SDL_memcpy(&extensions[first_extension_index], required_instance_extensions,
			   required_instance_extension_count * sizeof(const char*));

	VkInstanceCreateInfo instance_create_info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instance_create_info.pApplicationInfo = &application_info;
	instance_create_info.enabledExtensionCount = extension_count;
	instance_create_info.ppEnabledExtensionNames = extensions;

	if (s_enable_validation)
	{
		instance_create_info.enabledLayerCount = 1;
		instance_create_info.ppEnabledLayerNames = s_validation_layers;
	}
	else
	{
		instance_create_info.enabledLayerCount = 0;
	}

	if (vkCreateInstance(&instance_create_info, NULL, &s_instance) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Vulkan instance\n");
		return EXIT_FAILURE;
	}
	wc_free(extensions);

	return EXIT_SUCCESS;
}

int setupDebugMessenger(void)
{
	if (!s_enable_validation)
		return EXIT_SUCCESS;
	VkDebugUtilsMessengerCreateInfoEXT create_info = {.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
	create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
							  VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	create_info.pfnUserCallback = debugCallback;
	create_info.pUserData = NULL;

	if (vkCreateDebugUtilsMessengerEXT(s_instance, &create_info, NULL, &debugMessenger) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Vulkan debug messenger\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int createSurface(void)
{
	SDL_Window* window = wc_app_get_window_handle();
	if (!SDL_Vulkan_CreateSurface(window, s_instance, NULL, &surface))
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create Vulkan surface: %s\n", SDL_GetError());
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

bool checkDeviceExtensionSupport(VkPhysicalDevice device)
{
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, NULL, &extensionCount, NULL);
	VkExtensionProperties* availableExtensions = wc_malloc(sizeof(VkExtensionProperties) * extensionCount);
	vkEnumerateDeviceExtensionProperties(device, NULL, &extensionCount, availableExtensions);
	for (size_t i = 0; i < sizeof(s_device_extensions) / sizeof(s_device_extensions[0]); i++)
	{
		bool found = false;
		for (uint32_t j = 0; j < extensionCount; j++)
		{
			if (SDL_strcmp(s_device_extensions[i], availableExtensions[j].extensionName) == 0)
			{
				found = true;
				break;
			}
		}
		if (!found)
		{
			wc_free(availableExtensions);
			return false;
		}
	}
	wc_free(availableExtensions);
	return true;
}

WC_QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device)
{
	WC_QueueFamilyIndices indices = {.graphics_family = -1, .present_family = -1, .compute_family = -1, .transfer_family = -1};

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, NULL);
	VkQueueFamilyProperties* queueFamilies = wc_malloc(sizeof(VkQueueFamilyProperties) * queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies);
	for (uint32_t i = 0; i < queueFamilyCount; i++)
	{
		if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			indices.graphics_family = (int)i;
		}
		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
		if (presentSupport)
		{
			indices.present_family = (int)i;
		}
		if (indices.graphics_family >= 0 && indices.present_family >= 0)
			break;
	}
	wc_free(queueFamilies);
	return indices;
}

bool isDeviceSuitable(VkPhysicalDevice device)
{
	WC_QueueFamilyIndices indices = findQueueFamilies(device);
	bool extensionsSupported = checkDeviceExtensionSupport(device);
	bool swapChainAdequate = false;
	if (extensionsSupported)
	{
		uint32_t formatCount, presentModeCount;
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, NULL);
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, NULL);
		swapChainAdequate = formatCount > 0 && presentModeCount > 0;
	}
	return indices.graphics_family >= 0 && indices.present_family >= 0 && extensionsSupported && swapChainAdequate;
}

int pickPhysicalDevice(void)
{
	uint32_t deviceCount = 0;
	vkEnumeratePhysicalDevices(s_instance, &deviceCount, NULL);
	if (deviceCount == 0)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to find GPUs with Vulkan support\n");
		return EXIT_FAILURE;
	}
	VkPhysicalDevice* devices = wc_malloc(sizeof(VkPhysicalDevice) * deviceCount);
	vkEnumeratePhysicalDevices(s_instance, &deviceCount, devices);
	for (uint32_t i = 0; i < deviceCount; i++)
	{
		if (isDeviceSuitable(devices[i]))
		{
			physicalDevice = devices[i];
			break;
		}
	}
	wc_free(devices);
	if (physicalDevice == VK_NULL_HANDLE)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to find a suitable GPU\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int createLogicalDevice(void)
{
	WC_QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
	float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueCreateInfos[2];
	uint32_t queueCount = 0;
	if (indices.graphics_family == indices.present_family)
	{
		queueCreateInfos[0] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
														.queueFamilyIndex = indices.graphics_family,
														.queueCount = 1,
														.pQueuePriorities = &queuePriority};
		queueCount = 1;
	}
	else
	{
		queueCreateInfos[0] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
														.queueFamilyIndex = indices.graphics_family,
														.queueCount = 1,
														.pQueuePriorities = &queuePriority};
		queueCreateInfos[1] = (VkDeviceQueueCreateInfo){.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
														.queueFamilyIndex = indices.present_family,
														.queueCount = 1,
														.pQueuePriorities = &queuePriority};
		queueCount = 2;
	}
	VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
	descriptorIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	descriptorIndexing.runtimeDescriptorArray = VK_TRUE;
	VkPhysicalDeviceMeshShaderFeaturesEXT meshFeatures = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT,
														  .pNext = &descriptorIndexing};
	meshFeatures.meshShader = VK_TRUE;
	meshFeatures.taskShader = VK_TRUE;
	VkPhysicalDeviceFeatures2 features2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &meshFeatures};
	VkDeviceCreateInfo createInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	createInfo.queueCreateInfoCount = queueCount;
	createInfo.pQueueCreateInfos = queueCreateInfos;
	createInfo.enabledExtensionCount = sizeof(s_device_extensions) / sizeof(*s_device_extensions);
	createInfo.ppEnabledExtensionNames = s_device_extensions;
	if (s_enable_validation)
	{
		createInfo.enabledLayerCount = 1;
		createInfo.ppEnabledLayerNames = s_validation_layers;
	}
	createInfo.pNext = &features2;

	if (vkCreateDevice(physicalDevice, &createInfo, NULL, &device) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create logical device\n");
		return EXIT_FAILURE;
	}
	vkGetDeviceQueue(device, indices.graphics_family, 0, &graphicsQueue);
	vkGetDeviceQueue(device, indices.present_family, 0, &presentQueue);
	graphicsQueueFamilyIndex = indices.graphics_family;
	presentQueueFamilyIndex = indices.present_family;
	return EXIT_SUCCESS;
}

static VkSurfaceFormatKHR chooseSwapSurfaceFormat(const VkSurfaceFormatKHR* availableFormats, uint32_t formatCount)
{
	for (uint32_t i = 0; i < formatCount; i++)
	{
		if (availableFormats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
			availableFormats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			return availableFormats[i];
		}
	}
	return availableFormats[0];
}

static VkPresentModeKHR chooseSwapPresentMode(const VkPresentModeKHR* availablePresentModes, uint32_t presentModeCount)
{
	for (uint32_t i = 0; i < presentModeCount; i++)
	{
		if (availablePresentModes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
		{
			return availablePresentModes[i];
		}
	}
	return VK_PRESENT_MODE_FIFO_KHR;
}

static VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR capabilities)
{
	int width, height;
	wc_app_get_window_size(&width, &height);

	if (capabilities.currentExtent.width != UINT32_MAX)
	{
		return capabilities.currentExtent;
	}

	VkExtent2D actual = {width, height};
	if (actual.width < capabilities.minImageExtent.width)
		actual.width = capabilities.minImageExtent.width;
	else if (actual.width > capabilities.maxImageExtent.width)
		actual.width = capabilities.maxImageExtent.width;
	if (actual.height < capabilities.minImageExtent.height)
		actual.height = capabilities.minImageExtent.height;
	else if (actual.height > capabilities.maxImageExtent.height)
		actual.height = capabilities.maxImageExtent.height;
	return actual;
}

int createSwapchain(void)
{
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

	// Query drawable size for high-DPI
	SDL_Window* window = wc_app_get_window_handle();
	int width, height;
	SDL_GetWindowSizeInPixels(window, &width, &height);
	VkExtent2D actualExtent = {width, height};
	if (caps.currentExtent.width != UINT32_MAX)
		actualExtent = caps.currentExtent;
	else
	{
		actualExtent.width =
			actualExtent.width < caps.minImageExtent.width
				? caps.minImageExtent.width
				: (actualExtent.width > caps.maxImageExtent.width ? caps.maxImageExtent.width : actualExtent.width);
		actualExtent.height =
			actualExtent.height < caps.minImageExtent.height
				? caps.minImageExtent.height
				: (actualExtent.height > caps.maxImageExtent.height ? caps.maxImageExtent.height : actualExtent.height);
	}
	swapchainExtent = actualExtent;

	uint32_t fmtCount, pmCount;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, NULL);
	VkSurfaceFormatKHR* formats = wc_malloc(sizeof(VkSurfaceFormatKHR) * fmtCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, formats);
	vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &pmCount, NULL);
	VkPresentModeKHR* modes = wc_malloc(sizeof(VkPresentModeKHR) * pmCount);
	vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &pmCount, modes);

	VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(formats, fmtCount);
	VkPresentModeKHR presentMode = chooseSwapPresentMode(modes, pmCount);
	wc_free(formats);
	wc_free(modes);

	uint32_t imgCount = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount)
		imgCount = caps.maxImageCount;

	VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
	ci.surface = surface;
	ci.minImageCount = imgCount;
	ci.imageFormat = surfaceFormat.format;
	ci.imageColorSpace = surfaceFormat.colorSpace;
	ci.imageExtent = swapchainExtent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	uint32_t qf[] = {graphicsQueueFamilyIndex, presentQueueFamilyIndex};
	if (graphicsQueueFamilyIndex != presentQueueFamilyIndex)
	{
		ci.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		ci.queueFamilyIndexCount = 2;
		ci.pQueueFamilyIndices = qf;
	}
	else
		ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	ci.presentMode = presentMode;
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = VK_NULL_HANDLE;

	if (vkCreateSwapchainKHR(device, &ci, NULL, &swapchain) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "swapchain creation failed\n");
		return EXIT_FAILURE;
	}

	vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, NULL);
	swapchainImages = wc_malloc(sizeof(VkImage) * swapchainImageCount);
	vkGetSwapchainImagesKHR(device, swapchain, &swapchainImageCount, swapchainImages);
	return EXIT_SUCCESS;
}

int createImageViews(void)
{
	swapchainImageViews = wc_malloc(sizeof(VkImageView) * swapchainImageCount);
	for (uint32_t i = 0; i < swapchainImageCount; i++)
	{
		VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		info.image = swapchainImages[i];
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = VK_FORMAT_B8G8R8A8_SRGB;
		info.components = (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY};
		info.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		if (vkCreateImageView(device, &info, NULL, &swapchainImageViews[i]) != VK_SUCCESS)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "image view creation failed\n");
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

int createRenderPass(void)
{
	VkAttachmentDescription colorAttachment = {0};
	colorAttachment.format = VK_FORMAT_B8G8R8A8_SRGB;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	VkAttachmentReference colorRef = {0};
	colorRef.attachment = 0;
	colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkSubpassDescription subpass = {0};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colorRef;

	VkRenderPassCreateInfo rpInfo = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	rpInfo.attachmentCount = 1;
	rpInfo.pAttachments = &colorAttachment;
	rpInfo.subpassCount = 1;
	rpInfo.pSubpasses = &subpass;

	if (vkCreateRenderPass(device, &rpInfo, NULL, &renderPass) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "render pass creation failed\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int createFramebuffers(void)
{
	swapchainFramebuffers = wc_malloc(sizeof(VkFramebuffer) * swapchainImageCount);
	for (uint32_t i = 0; i < swapchainImageCount; i++)
	{
		VkImageView attachments[] = {swapchainImageViews[i]};
		VkFramebufferCreateInfo fbInfo = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		fbInfo.renderPass = renderPass;
		fbInfo.attachmentCount = 1;
		fbInfo.pAttachments = attachments;
		fbInfo.width = swapchainExtent.width;
		fbInfo.height = swapchainExtent.height;
		fbInfo.layers = 1;
		if (vkCreateFramebuffer(device, &fbInfo, NULL, &swapchainFramebuffers[i]) != VK_SUCCESS)
		{
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "framebuffer creation failed\n");
			return EXIT_FAILURE;
		}
	}
	return EXIT_SUCCESS;
}

int createCommandPool(void)
{
	VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = graphicsQueueFamilyIndex;
	if (vkCreateCommandPool(device, &poolInfo, NULL, &commandPool) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "command pool creation failed\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int createCommandBuffers(void)
{
	commandBuffers = wc_malloc(sizeof(VkCommandBuffer) * swapchainImageCount);
	VkCommandBufferAllocateInfo allocInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = swapchainImageCount;
	if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "command buffer allocation failed\n");
		return EXIT_FAILURE;
	}

	for (uint32_t i = 0; i < swapchainImageCount; i++)
	{
		VkCommandBufferBeginInfo beginInfo = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		vkBeginCommandBuffer(commandBuffers[i], &beginInfo);

		VkRenderPassBeginInfo rpBegin = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		rpBegin.renderPass = renderPass;
		rpBegin.framebuffer = swapchainFramebuffers[i];
		rpBegin.renderArea.extent = swapchainExtent;
		VkClearValue clear = {.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
		rpBegin.clearValueCount = 1;
		rpBegin.pClearValues = &clear;
		vkCmdBeginRenderPass(commandBuffers[i], &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

		vkCmdBindPipeline(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, meshPipeline);
		vkCmdBindDescriptorSets(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSet, 0,
								NULL);
		vkCmdDrawMeshTasksEXT(commandBuffers[i], 100, 1, 1);

		vkCmdEndRenderPass(commandBuffers[i]);
		vkEndCommandBuffer(commandBuffers[i]);
	}

	return EXIT_SUCCESS;
}

int createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage, VkBuffer* buffer,
				 VmaAllocation* allocation)
{
	VkBufferCreateInfo bufferInfo = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	VmaAllocationCreateInfo allocCreateInfo = {};
	allocCreateInfo.usage = memoryUsage;
	if (vmaCreateBuffer(allocator, &bufferInfo, &allocCreateInfo, buffer, allocation, NULL) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to allocate buffer\n");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

// --- Descriptor + Pipeline setup ---
void createDescriptorSetLayout(void)
{
	VkDescriptorSetLayoutBinding bindings[2] = {};
	// transforms
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;
	// visibility
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags = VK_SHADER_STAGE_MESH_BIT_EXT;

	VkDescriptorSetLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	layoutInfo.bindingCount = 2;
	layoutInfo.pBindings = bindings;
	vkCreateDescriptorSetLayout(device, &layoutInfo, NULL, &descriptorSetLayout);
}

void createPipeline(void)
{
	// Load mesh and fragment shaders (SPIR-V) -- assume functions loadShaderModule()
	VkShaderModule meshSM = loadShaderModule(device, "mesh.spv");
	VkShaderModule fragSM = loadShaderModule(device, "frag.spv");

	VkPipelineShaderStageCreateInfo stages[2] = {};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
	stages[0].module = meshSM;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = fragSM;
	stages[1].pName = "main";

	VkPipelineLayoutCreateInfo layoutInfo = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layoutInfo.setLayoutCount = 1;
	layoutInfo.pSetLayouts = &descriptorSetLayout;
	vkCreatePipelineLayout(device, &layoutInfo, NULL, &pipelineLayout);

	VkGraphicsPipelineCreateInfo pipeInfo = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	pipeInfo.stageCount = 2;
	pipeInfo.pStages = stages;
	pipeInfo.renderPass = renderPass;
	pipeInfo.layout = pipelineLayout;
	// other pipeline state (viewport, raster, blend) omitted for brevity
	vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, NULL, &meshPipeline);

	vkDestroyShaderModule(device, meshSM, NULL);
	vkDestroyShaderModule(device, fragSM, NULL);
}

void createDescriptorPool(void)
{
	VkDescriptorPoolSize sizes[2] = {};
	sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[0].descriptorCount = 1;
	sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[1].descriptorCount = 1;

	VkDescriptorPoolCreateInfo poolInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	poolInfo.maxSets = 1;
	poolInfo.poolSizeCount = 2;
	poolInfo.pPoolSizes = sizes;
	vkCreateDescriptorPool(device, &poolInfo, NULL, &descriptorPool);
}

void createDescriptorSet(void)
{
	VkDescriptorSetAllocateInfo allocInfo = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	allocInfo.descriptorPool = descriptorPool;
	allocInfo.descriptorSetCount = 1;
	allocInfo.pSetLayouts = &descriptorSetLayout;
	vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);
}

void updateDescriptorSet(void)
{
	createBuffer(sizeof(float) * 16 * 100, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, &transformBuffer,
				 &transformAlloc);
	createBuffer(sizeof(uint32_t) * 100, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU, &visibilityBuffer,
				 &visibilityAlloc);

	VkDescriptorBufferInfo bufInfos[2] = {};
	bufInfos[0].buffer = transformBuffer;
	bufInfos[0].offset = 0;
	bufInfos[0].range = VK_WHOLE_SIZE;
	bufInfos[1].buffer = visibilityBuffer;
	bufInfos[1].offset = 0;
	bufInfos[1].range = VK_WHOLE_SIZE;

	VkWriteDescriptorSet writes[2] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = descriptorSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].pBufferInfo = &bufInfos[0];
	writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[1].dstSet = descriptorSet;
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].pBufferInfo = &bufInfos[1];

	vkUpdateDescriptorSets(device, 2, writes, 0, NULL);
}

VkShaderModule loadShaderModule(VkDevice device, const char* filepath)
{
	FILE* file = fopen(filepath, "rb");
	if (!file)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open shader file: %s", filepath);
		return VK_NULL_HANDLE;
	}
	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	rewind(file);
	// Size must be a multiple of 4
	if (size % 4 != 0)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Shader file size not aligned: %s", filepath);
		return VK_NULL_HANDLE;
	}
	char* code = wc_malloc(size);
	if (fread(code, 1, size, file) != size)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read shader file: %s", filepath);
		return VK_NULL_HANDLE;
	}
	fclose(file);

	VkShaderModuleCreateInfo createInfo = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
	createInfo.codeSize = size;
	createInfo.pCode = (const uint32_t*)code;

	VkShaderModule shaderModule;
	if (vkCreateShaderModule(device, &createInfo, NULL, &shaderModule) != VK_SUCCESS)
	{
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create shader module from file: %s", filepath);
		return VK_NULL_HANDLE;
	}

	wc_free(code);
	return shaderModule;
}
