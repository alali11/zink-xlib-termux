/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_device.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "panvk_private.h"

#include "decode.h"

#include "pan_encoder.h"
#include "pan_props.h"
#include "pan_samples.h"
#include "pan_util.h"

#include "vk_cmd_enqueue_entrypoints.h"
#include "vk_common_entrypoints.h"

#include <fcntl.h>
#include <libsync.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <sys/mman.h>
#include <sys/sysinfo.h>

#include "drm-uapi/panfrost_drm.h"

#include "util/disk_cache.h"
#include "util/strtod.h"
#include "util/u_debug.h"
#include "vk_drm_syncobj.h"
#include "vk_format.h"
#include "vk_util.h"

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include "wayland-drm-client-protocol.h"
#include <wayland-client.h>
#endif

#include "panvk_cs.h"

static int
panvk_device_get_cache_uuid(uint16_t family, void *uuid)
{
   uint32_t mesa_timestamp;
   uint16_t f = family;

   if (!disk_cache_get_function_timestamp(panvk_device_get_cache_uuid,
                                          &mesa_timestamp))
      return -1;

   memset(uuid, 0, VK_UUID_SIZE);
   memcpy(uuid, &mesa_timestamp, 4);
   memcpy((char *)uuid + 4, &f, 2);
   snprintf((char *)uuid + 6, VK_UUID_SIZE - 10, "pan");
   return 0;
}

static void
panvk_get_driver_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
   snprintf(uuid, VK_UUID_SIZE, "panfrost");
}

static void
panvk_get_device_uuid(void *uuid)
{
   memset(uuid, 0, VK_UUID_SIZE);
}

static const struct debug_control panvk_debug_options[] = {
   {"startup", PANVK_DEBUG_STARTUP},
   {"nir", PANVK_DEBUG_NIR},
   {"trace", PANVK_DEBUG_TRACE},
   {"sync", PANVK_DEBUG_SYNC},
   {"afbc", PANVK_DEBUG_AFBC},
   {"linear", PANVK_DEBUG_LINEAR},
   {"dump", PANVK_DEBUG_DUMP},
   {"no_known_warn", PANVK_DEBUG_NO_KNOWN_WARN},
   {NULL, 0}};

#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
#define PANVK_USE_WSI_PLATFORM
#endif

#define PANVK_API_VERSION VK_MAKE_VERSION(1, 0, VK_HEADER_VERSION)

VKAPI_ATTR VkResult VKAPI_CALL
panvk_EnumerateInstanceVersion(uint32_t *pApiVersion)
{
   *pApiVersion = PANVK_API_VERSION;
   return VK_SUCCESS;
}

static const struct vk_instance_extension_table panvk_instance_extensions = {
   .KHR_get_physical_device_properties2 = true,
   .EXT_debug_report = true,
   .EXT_debug_utils = true,

#ifdef PANVK_USE_WSI_PLATFORM
   .KHR_surface = true,
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
   .KHR_wayland_surface = true,
#endif
#ifndef VK_USE_PLATFORM_WIN32_KHR
   .EXT_headless_surface = true,
#endif
};

static void
panvk_get_device_extensions(const struct panvk_physical_device *device,
                            struct vk_device_extension_table *ext)
{
   *ext = (struct vk_device_extension_table){
      .KHR_copy_commands2 = true,
      .KHR_shader_expect_assume = true,
      .KHR_storage_buffer_storage_class = true,
      .KHR_descriptor_update_template = true,
#ifdef PANVK_USE_WSI_PLATFORM
      .KHR_swapchain = true,
#endif
      .KHR_synchronization2 = true,
      .KHR_variable_pointers = true,
      .EXT_custom_border_color = true,
      .EXT_index_type_uint8 = true,
      .EXT_vertex_attribute_divisor = true,
   };
}

static void
panvk_get_features(const struct panvk_physical_device *device,
                   struct vk_features *features)
{
   *features = (struct vk_features){
      /* Vulkan 1.0 */
      .robustBufferAccess = true,
      .fullDrawIndexUint32 = true,
      .independentBlend = true,
      .logicOp = true,
      .wideLines = true,
      .largePoints = true,
      .textureCompressionETC2 = true,
      .textureCompressionASTC_LDR = true,
      .shaderUniformBufferArrayDynamicIndexing = true,
      .shaderSampledImageArrayDynamicIndexing = true,
      .shaderStorageBufferArrayDynamicIndexing = true,
      .shaderStorageImageArrayDynamicIndexing = true,

      /* Vulkan 1.1 */
      .storageBuffer16BitAccess = false,
      .uniformAndStorageBuffer16BitAccess = false,
      .storagePushConstant16 = false,
      .storageInputOutput16 = false,
      .multiview = false,
      .multiviewGeometryShader = false,
      .multiviewTessellationShader = false,
      .variablePointersStorageBuffer = true,
      .variablePointers = true,
      .protectedMemory = false,
      .samplerYcbcrConversion = false,
      .shaderDrawParameters = false,

      /* Vulkan 1.2 */
      .samplerMirrorClampToEdge = false,
      .drawIndirectCount = false,
      .storageBuffer8BitAccess = false,
      .uniformAndStorageBuffer8BitAccess = false,
      .storagePushConstant8 = false,
      .shaderBufferInt64Atomics = false,
      .shaderSharedInt64Atomics = false,
      .shaderFloat16 = false,
      .shaderInt8 = false,

      .descriptorIndexing = false,
      .shaderInputAttachmentArrayDynamicIndexing = false,
      .shaderUniformTexelBufferArrayDynamicIndexing = false,
      .shaderStorageTexelBufferArrayDynamicIndexing = false,
      .shaderUniformBufferArrayNonUniformIndexing = false,
      .shaderSampledImageArrayNonUniformIndexing = false,
      .shaderStorageBufferArrayNonUniformIndexing = false,
      .shaderStorageImageArrayNonUniformIndexing = false,
      .shaderInputAttachmentArrayNonUniformIndexing = false,
      .shaderUniformTexelBufferArrayNonUniformIndexing = false,
      .shaderStorageTexelBufferArrayNonUniformIndexing = false,
      .descriptorBindingUniformBufferUpdateAfterBind = false,
      .descriptorBindingSampledImageUpdateAfterBind = false,
      .descriptorBindingStorageImageUpdateAfterBind = false,
      .descriptorBindingStorageBufferUpdateAfterBind = false,
      .descriptorBindingUniformTexelBufferUpdateAfterBind = false,
      .descriptorBindingStorageTexelBufferUpdateAfterBind = false,
      .descriptorBindingUpdateUnusedWhilePending = false,
      .descriptorBindingPartiallyBound = false,
      .descriptorBindingVariableDescriptorCount = false,
      .runtimeDescriptorArray = false,

      .samplerFilterMinmax = false,
      .scalarBlockLayout = false,
      .imagelessFramebuffer = false,
      .uniformBufferStandardLayout = false,
      .shaderSubgroupExtendedTypes = false,
      .separateDepthStencilLayouts = false,
      .hostQueryReset = false,
      .timelineSemaphore = false,
      .bufferDeviceAddress = true,
      .bufferDeviceAddressCaptureReplay = false,
      .bufferDeviceAddressMultiDevice = false,
      .vulkanMemoryModel = false,
      .vulkanMemoryModelDeviceScope = false,
      .vulkanMemoryModelAvailabilityVisibilityChains = false,
      .shaderOutputViewportIndex = false,
      .shaderOutputLayer = false,
      .subgroupBroadcastDynamicId = false,

      /* Vulkan 1.3 */
      .robustImageAccess = false,
      .inlineUniformBlock = false,
      .descriptorBindingInlineUniformBlockUpdateAfterBind = false,
      .pipelineCreationCacheControl = false,
      .privateData = true,
      .shaderDemoteToHelperInvocation = false,
      .shaderTerminateInvocation = false,
      .subgroupSizeControl = false,
      .computeFullSubgroups = false,
      .synchronization2 = true,
      .textureCompressionASTC_HDR = false,
      .shaderZeroInitializeWorkgroupMemory = false,
      .dynamicRendering = false,
      .shaderIntegerDotProduct = false,
      .maintenance4 = false,

      /* VK_EXT_index_type_uint8 */
      .indexTypeUint8 = true,

      /* VK_EXT_vertex_attribute_divisor */
      .vertexAttributeInstanceRateDivisor = true,
      .vertexAttributeInstanceRateZeroDivisor = true,

      /* VK_EXT_depth_clip_enable */
      .depthClipEnable = true,

      /* VK_EXT_4444_formats */
      .formatA4R4G4B4 = true,
      .formatA4B4G4R4 = true,

      /* VK_EXT_custom_border_color */
      .customBorderColors = true,
      .customBorderColorWithoutFormat = true,

      /* VK_KHR_shader_expect_assume */
      .shaderExpectAssume = true,
   };
}

VkResult panvk_physical_device_try_create(struct vk_instance *vk_instance,
                                          struct _drmDevice *drm_device,
                                          struct vk_physical_device **out);

static void
panvk_physical_device_finish(struct panvk_physical_device *device)
{
   panvk_wsi_finish(device);

   pan_kmod_dev_destroy(device->kmod.dev);
   if (device->master_fd != -1)
      close(device->master_fd);

   vk_physical_device_finish(&device->vk);
}

static void
panvk_destroy_physical_device(struct vk_physical_device *device)
{
   panvk_physical_device_finish((struct panvk_physical_device *)device);
   vk_free(&device->instance->alloc, device);
}

static void *
panvk_kmod_zalloc(const struct pan_kmod_allocator *allocator, size_t size,
                  bool transient)
{
   const VkAllocationCallbacks *vkalloc = allocator->priv;

   return vk_zalloc(vkalloc, size, 8,
                    transient ? VK_SYSTEM_ALLOCATION_SCOPE_COMMAND
                              : VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
}

static void
panvk_kmod_free(const struct pan_kmod_allocator *allocator, void *data)
{
   const VkAllocationCallbacks *vkalloc = allocator->priv;

   return vk_free(vkalloc, data);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateInstance(const VkInstanceCreateInfo *pCreateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkInstance *pInstance)
{
   struct panvk_instance *instance;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);

   pAllocator = pAllocator ?: vk_default_allocator();
   instance = vk_zalloc(pAllocator, sizeof(*instance), 8,
                        VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!instance)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct vk_instance_dispatch_table dispatch_table;

   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &panvk_instance_entrypoints, true);
   vk_instance_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_instance_entrypoints, false);
   result = vk_instance_init(&instance->vk, &panvk_instance_extensions,
                             &dispatch_table, pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(pAllocator, instance);
      return vk_error(NULL, result);
   }

   instance->kmod.allocator = (struct pan_kmod_allocator){
      .zalloc = panvk_kmod_zalloc,
      .free = panvk_kmod_free,
      .priv = &instance->vk.alloc,
   };

   instance->vk.physical_devices.try_create_for_drm =
      panvk_physical_device_try_create;
   instance->vk.physical_devices.destroy = panvk_destroy_physical_device;

   instance->debug_flags =
      parse_debug_string(getenv("PANVK_DEBUG"), panvk_debug_options);

   if (instance->debug_flags & PANVK_DEBUG_STARTUP)
      vk_logi(VK_LOG_NO_OBJS(instance), "Created an instance");

   VG(VALGRIND_CREATE_MEMPOOL(instance, 0, false));

   *pInstance = panvk_instance_to_handle(instance);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyInstance(VkInstance _instance,
                      const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_instance, instance, _instance);

   if (!instance)
      return;

   vk_instance_finish(&instance->vk);
   vk_free(&instance->vk.alloc, instance);
}

static VkResult
panvk_physical_device_init(struct panvk_physical_device *device,
                           struct panvk_instance *instance,
                           drmDevicePtr drm_device)
{
   const char *path = drm_device->nodes[DRM_NODE_RENDER];
   VkResult result = VK_SUCCESS;
   drmVersionPtr version;
   int fd;
   int master_fd = -1;

   if (!getenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER")) {
      return vk_errorf(
         instance, VK_ERROR_INCOMPATIBLE_DRIVER,
         "WARNING: panvk is not a conformant vulkan implementation, "
         "pass PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 if you know what you're doing.");
   }

   fd = open(path, O_RDWR | O_CLOEXEC);
   if (fd < 0) {
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "failed to open device %s", path);
   }

   version = drmGetVersion(fd);
   if (!version) {
      close(fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "failed to query kernel driver version for device %s",
                       path);
   }

   if (strcmp(version->name, "panfrost")) {
      drmFreeVersion(version);
      close(fd);
      return vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                       "device %s does not use the panfrost kernel driver",
                       path);
   }

   drmFreeVersion(version);

   if (instance->debug_flags & PANVK_DEBUG_STARTUP)
      vk_logi(VK_LOG_NO_OBJS(instance), "Found compatible device '%s'.", path);

   struct vk_device_extension_table supported_extensions;
   panvk_get_device_extensions(device, &supported_extensions);

   struct vk_features supported_features;
   panvk_get_features(device, &supported_features);

   struct vk_physical_device_dispatch_table dispatch_table;
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &panvk_physical_device_entrypoints, true);
   vk_physical_device_dispatch_table_from_entrypoints(
      &dispatch_table, &wsi_physical_device_entrypoints, false);

   result =
      vk_physical_device_init(&device->vk, &instance->vk, &supported_extensions,
                              &supported_features, NULL, &dispatch_table);

   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail;
   }

   if (instance->vk.enabled_extensions.KHR_display) {
      master_fd = open(drm_device->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
      if (master_fd >= 0) {
         /* TODO: free master_fd is accel is not working? */
      }
   }

   device->master_fd = master_fd;

   device->kmod.dev = pan_kmod_dev_create(fd, PAN_KMOD_DEV_FLAG_OWNS_FD,
                                          &instance->kmod.allocator);
   pan_kmod_dev_query_props(device->kmod.dev, &device->kmod.props);

   unsigned arch = pan_arch(device->kmod.props.gpu_prod_id);

   device->model = panfrost_get_model(device->kmod.props.gpu_prod_id,
                                      device->kmod.props.gpu_variant);
   device->formats.all = panfrost_format_table(arch);
   device->formats.blendable = panfrost_blendable_format_table(arch);

   if (arch <= 5 || arch >= 8) {
      result = vk_errorf(instance, VK_ERROR_INCOMPATIBLE_DRIVER,
                         "%s not supported", device->model->name);
      goto fail;
   }

   memset(device->name, 0, sizeof(device->name));
   sprintf(device->name, "%s", device->model->name);

   if (panvk_device_get_cache_uuid(device->kmod.props.gpu_prod_id,
                                   device->cache_uuid)) {
      result = vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                         "cannot generate UUID");
      goto fail_close_device;
   }

   vk_warn_non_conformant_implementation("panvk");

   panvk_get_driver_uuid(&device->device_uuid);
   panvk_get_device_uuid(&device->device_uuid);

   device->drm_syncobj_type = vk_drm_syncobj_get_type(device->kmod.dev->fd);
   /* We don't support timelines in the uAPI yet and we don't want it getting
    * suddenly turned on by vk_drm_syncobj_get_type() without us adding panvk
    * code for it first.
    */
   device->drm_syncobj_type.features &= ~VK_SYNC_FEATURE_TIMELINE;

   device->sync_types[0] = &device->drm_syncobj_type;
   device->sync_types[1] = NULL;
   device->vk.supported_sync_types = device->sync_types;

   result = panvk_wsi_init(device);
   if (result != VK_SUCCESS) {
      vk_error(instance, result);
      goto fail_close_device;
   }

   return VK_SUCCESS;

fail_close_device:
   pan_kmod_dev_destroy(device->kmod.dev);
fail:
   if (fd != -1)
      close(fd);
   if (master_fd != -1)
      close(master_fd);
   return result;
}

VkResult
panvk_physical_device_try_create(struct vk_instance *vk_instance,
                                 struct _drmDevice *drm_device,
                                 struct vk_physical_device **out)
{
   struct panvk_instance *instance =
      container_of(vk_instance, struct panvk_instance, vk);

   if (!(drm_device->available_nodes & (1 << DRM_NODE_RENDER)) ||
       drm_device->bustype != DRM_BUS_PLATFORM)
      return VK_ERROR_INCOMPATIBLE_DRIVER;

   struct panvk_physical_device *device =
      vk_zalloc(&instance->vk.alloc, sizeof(*device), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!device)
      return vk_error(instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result = panvk_physical_device_init(device, instance, drm_device);
   if (result != VK_SUCCESS) {
      vk_free(&instance->vk.alloc, device);
      return result;
   }

   *out = &device->vk;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice,
                                   VkPhysicalDeviceProperties2 *pProperties)
{
   VK_FROM_HANDLE(panvk_physical_device, pdevice, physicalDevice);

   /* HW supports MSAA 4, 8 and 16, but we limit ourselves to MSAA 4 for now. */
   VkSampleCountFlags sample_counts =
      VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;

   const VkPhysicalDeviceLimits limits = {
      /* Maximum texture dimension is 2^16. */
      .maxImageDimension1D = (1 << 16),
      .maxImageDimension2D = (1 << 16),
      .maxImageDimension3D = (1 << 16),
      .maxImageDimensionCube = (1 << 16),
      .maxImageArrayLayers = (1 << 16),

      /* Currently limited by the 1D texture size, which is 2^16.
       * TODO: If we expose buffer views as 2D textures, we can increase the
       * limit.
       */
      .maxTexelBufferElements = (1 << 16),

      /* Each uniform entry is 16-byte and the number of entries is encoded in a
       * 12-bit field, with the minus(1) modifier, which gives 2^20.
       */
      .maxUniformBufferRange = 1 << 20,

      /* Storage buffer access is lowered to globals, so there's no limit here,
       * except for the SW-descriptor we use to encode storage buffer
       * descriptors, where the size is a 32-bit field.
       */
      .maxStorageBufferRange = UINT32_MAX,

      /* 128 bytes of push constants, so we're aligned with the minimum Vulkan
       * requirements.
       */
      .maxPushConstantsSize = 128,

      /* There's no HW limit here. Should we advertize something smaller? */
      .maxMemoryAllocationCount = UINT32_MAX,

      /* Again, no hardware limit, but most drivers seem to advertive 64k. */
      .maxSamplerAllocationCount = 64 * 1024,

      /* A cache line. */
      .bufferImageGranularity = 64,

      /* Sparse binding not supported yet. */
      .sparseAddressSpaceSize = 0,

      /* Software limit. Pick the minimum required by Vulkan, because Bifrost
       * GPUs don't have unified descriptor tables, which forces us to
       * agregatte all descriptors from all sets and dispatch them to per-type
       * descriptor tables emitted at draw/dispatch time.
       * The more sets we support the more copies we are likely to have to do
       * at draw time.
       */
      .maxBoundDescriptorSets = 4,

      /* MALI_RENDERER_STATE::sampler_count is 16-bit. */
      .maxPerStageDescriptorSamplers = UINT16_MAX,
      .maxDescriptorSetSamplers = UINT16_MAX,

      /* MALI_RENDERER_STATE::uniform_buffer_count is 8-bit. We reserve 32 slots
       * for our internal UBOs.
       */
      .maxPerStageDescriptorUniformBuffers = UINT8_MAX - 32,
      .maxDescriptorSetUniformBuffers = UINT8_MAX - 32,

      /* SSBOs are limited by the size of a uniform buffer which contains our
       * panvk_ssbo_desc objects.
       * panvk_ssbo_desc is 16-byte, and each uniform entry in the Mali UBO is
       * 16-byte too. The number of entries is encoded in a 12-bit field, with
       * a minus(1) modifier, which gives a maximum of 2^12 SSBO
       * descriptors.
       */
      .maxPerStageDescriptorStorageBuffers = 1 << 12,
      .maxDescriptorSetStorageBuffers = 1 << 12,

      /* MALI_RENDERER_STATE::sampler_count is 16-bit. */
      .maxPerStageDescriptorSampledImages = UINT16_MAX,
      .maxDescriptorSetSampledImages = UINT16_MAX,

      /* MALI_ATTRIBUTE::buffer_index is 9-bit, and each image takes two
       * MALI_ATTRIBUTE_BUFFER slots, which gives a maximum of (1 << 8) images.
       */
      .maxPerStageDescriptorStorageImages = 1 << 8,
      .maxDescriptorSetStorageImages = 1 << 8,

      /* A maximum of 8 color render targets, and one depth-stencil render
       * target.
       */
      .maxPerStageDescriptorInputAttachments = 9,
      .maxDescriptorSetInputAttachments = 9,

      /* Could be the sum of all maxPerStageXxx values, but we limit ourselves
       * to 2^16 to make things simpler.
       */
      .maxPerStageResources = 1 << 16,

      /* Software limits to keep VkCommandBuffer tracking sane. */
      .maxDescriptorSetUniformBuffersDynamic = 16,
      .maxDescriptorSetStorageBuffersDynamic = 8,

      /* Software limit to keep VkCommandBuffer tracking sane. The HW supports
       * up to 2^9 vertex attributes.
       */
      .maxVertexInputAttributes = 16,
      .maxVertexInputBindings = 16,

      /* MALI_ATTRIBUTE::offset is 32-bit. */
      .maxVertexInputAttributeOffset = UINT32_MAX,

      /* MALI_ATTRIBUTE_BUFFER::stride is 32-bit. */
      .maxVertexInputBindingStride = UINT32_MAX,

      /* 32 vec4 varyings. */
      .maxVertexOutputComponents = 128,

      /* Tesselation shaders not supported. */
      .maxTessellationGenerationLevel = 0,
      .maxTessellationPatchSize = 0,
      .maxTessellationControlPerVertexInputComponents = 0,
      .maxTessellationControlPerVertexOutputComponents = 0,
      .maxTessellationControlPerPatchOutputComponents = 0,
      .maxTessellationControlTotalOutputComponents = 0,
      .maxTessellationEvaluationInputComponents = 0,
      .maxTessellationEvaluationOutputComponents = 0,

      /* Geometry shaders not supported. */
      .maxGeometryShaderInvocations = 0,
      .maxGeometryInputComponents = 0,
      .maxGeometryOutputComponents = 0,
      .maxGeometryOutputVertices = 0,
      .maxGeometryTotalOutputComponents = 0,

      /* 32 vec4 varyings. */
      .maxFragmentInputComponents = 128,

      /* 8 render targets. */
      .maxFragmentOutputAttachments = 8,

      /* We don't support dual source blending yet. */
      .maxFragmentDualSrcAttachments = 0,

      /* 8 render targets, 2^12 storage buffers and 2^8 storage images (see
       * above).
       */
      .maxFragmentCombinedOutputResources = 8 + (1 << 12) + (1 << 8),

      /* MALI_LOCAL_STORAGE::wls_size_{base,scale} allows us to have up to
       * (7 << 30) bytes of shared memory, but we cap it to 32K as it doesn't
       * really make sense to expose this amount of memory, especially since
       * it's backed by global memory anyway.
       */
      .maxComputeSharedMemorySize = 32768,

      /* Software limit to meet Vulkan 1.0 requirements. We split the
       * dispatch in several jobs if it's too big.
       */
      .maxComputeWorkGroupCount = {65535, 65535, 65535},

      /* We have 10 bits to encode the local-size, and there's a minus(1)
       * modifier, so, a size of 1 takes no bit.
       */
      .maxComputeWorkGroupInvocations = 1 << 10,
      .maxComputeWorkGroupSize = {1 << 10, 1 << 10, 1 << 10},

      /* 8-bit subpixel precision. */
      .subPixelPrecisionBits = 8,
      .subTexelPrecisionBits = 8,
      .mipmapPrecisionBits = 8,

      /* Software limit. */
      .maxDrawIndexedIndexValue = UINT32_MAX,

      /* Make it one for now. */
      .maxDrawIndirectCount = 1,

      .maxSamplerLodBias = 255,
      .maxSamplerAnisotropy = 16,
      .maxViewports = 1,

      /* Same as the framebuffer limit. */
      .maxViewportDimensions = {(1 << 14), (1 << 14)},

      /* Encoded in a 16-bit signed integer. */
      .viewportBoundsRange = {INT16_MIN, INT16_MAX},
      .viewportSubPixelBits = 0,

      /* Align on a page. */
      .minMemoryMapAlignment = 4096,

      /* Some compressed texture formats require 128-byte alignment. */
      .minTexelBufferOffsetAlignment = 64,

      /* Always aligned on a uniform slot (vec4). */
      .minUniformBufferOffsetAlignment = 16,

      /* Lowered to global accesses, which happen at the 32-bit granularity. */
      .minStorageBufferOffsetAlignment = 4,

      /* Signed 4-bit value. */
      .minTexelOffset = -8,
      .maxTexelOffset = 7,
      .minTexelGatherOffset = -8,
      .maxTexelGatherOffset = 7,
      .minInterpolationOffset = -0.5,
      .maxInterpolationOffset = 0.5,
      .subPixelInterpolationOffsetBits = 8,

      .maxFramebufferWidth = (1 << 14),
      .maxFramebufferHeight = (1 << 14),
      .maxFramebufferLayers = 256,
      .framebufferColorSampleCounts = sample_counts,
      .framebufferDepthSampleCounts = sample_counts,
      .framebufferStencilSampleCounts = sample_counts,
      .framebufferNoAttachmentsSampleCounts = sample_counts,
      .maxColorAttachments = 8,
      .sampledImageColorSampleCounts = sample_counts,
      .sampledImageIntegerSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .sampledImageDepthSampleCounts = sample_counts,
      .sampledImageStencilSampleCounts = sample_counts,
      .storageImageSampleCounts = VK_SAMPLE_COUNT_1_BIT,
      .maxSampleMaskWords = 1,
      .timestampComputeAndGraphics = false,
      .timestampPeriod = 0,
      .maxClipDistances = 0,
      .maxCullDistances = 0,
      .maxCombinedClipAndCullDistances = 0,
      .discreteQueuePriorities = 1,
      .pointSizeRange = {0.125, 4095.9375},
      .lineWidthRange = {0.0, 7.9921875},
      .pointSizeGranularity = (1.0 / 16.0),
      .lineWidthGranularity = (1.0 / 128.0),
      .strictLines = false,
      .standardSampleLocations = true,
      .optimalBufferCopyOffsetAlignment = 64,
      .optimalBufferCopyRowPitchAlignment = 64,
      .nonCoherentAtomSize = 64,
   };

   pProperties->properties = (VkPhysicalDeviceProperties){
      .apiVersion = PANVK_API_VERSION,
      .driverVersion = vk_get_driver_version(),

      /* Arm vendor ID. */
      .vendorID = 0x13b5,

      /* Collect arch_major, arch_minor, arch_rev and product_major,
       * as done by the Arm driver.
       */
      .deviceID = pdevice->kmod.props.gpu_prod_id << 16,
      .deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
      .limits = limits,
      .sparseProperties = {0},
   };

   strcpy(pProperties->properties.deviceName, pdevice->name);
   memcpy(pProperties->properties.pipelineCacheUUID, pdevice->cache_uuid,
          VK_UUID_SIZE);

   VkPhysicalDeviceVulkan11Properties core_1_1 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
      .deviceLUIDValid = false,
      .pointClippingBehavior = VK_POINT_CLIPPING_BEHAVIOR_ALL_CLIP_PLANES,
      .maxMultiviewViewCount = 0,
      .maxMultiviewInstanceIndex = 0,
      .protectedNoFault = false,
      /* Make sure everything is addressable by a signed 32-bit int, and
       * our largest descriptors are 96 bytes. */
      .maxPerSetDescriptors = (1ull << 31) / 96,
      /* Our buffer size fields allow only this much */
      .maxMemoryAllocationSize = 0xFFFFFFFFull,
   };
   memcpy(core_1_1.driverUUID, pdevice->driver_uuid, VK_UUID_SIZE);
   memcpy(core_1_1.deviceUUID, pdevice->device_uuid, VK_UUID_SIZE);

   const VkPhysicalDeviceVulkan12Properties core_1_2 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
   };

   const VkPhysicalDeviceVulkan13Properties core_1_3 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
   };

   vk_foreach_struct(ext, pProperties->pNext) {
      if (vk_get_physical_device_core_1_1_property_ext(ext, &core_1_1))
         continue;
      if (vk_get_physical_device_core_1_2_property_ext(ext, &core_1_2))
         continue;
      if (vk_get_physical_device_core_1_3_property_ext(ext, &core_1_3))
         continue;

      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR: {
         VkPhysicalDevicePushDescriptorPropertiesKHR *properties =
            (VkPhysicalDevicePushDescriptorPropertiesKHR *)ext;
         properties->maxPushDescriptors = 0;
         break;
      }
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_EXT: {
         VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *properties =
            (VkPhysicalDeviceVertexAttributeDivisorPropertiesEXT *)ext;
         /* We will have to restrict this a bit for multiview */
         properties->maxVertexAttribDivisor = UINT32_MAX;
         break;
      }
      default:
         break;
      }
   }
}

static const VkQueueFamilyProperties panvk_queue_family_properties = {
   .queueFlags =
      VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT,
   .queueCount = 1,
   .timestampValidBits = 0,
   .minImageTransferGranularity = {1, 1, 1},
};

VKAPI_ATTR void VKAPI_CALL
panvk_GetPhysicalDeviceQueueFamilyProperties2(
   VkPhysicalDevice physicalDevice, uint32_t *pQueueFamilyPropertyCount,
   VkQueueFamilyProperties2 *pQueueFamilyProperties)
{
   VK_OUTARRAY_MAKE_TYPED(VkQueueFamilyProperties2, out, pQueueFamilyProperties,
                          pQueueFamilyPropertyCount);

   vk_outarray_append_typed(VkQueueFamilyProperties2, &out, p)
   {
      p->queueFamilyProperties = panvk_queue_family_properties;
   }
}

static uint64_t
panvk_get_system_heap_size()
{
   struct sysinfo info;
   sysinfo(&info);

   uint64_t total_ram = (uint64_t)info.totalram * info.mem_unit;

   /* We don't want to burn too much ram with the GPU.  If the user has 4GiB
    * or less, we use at most half.  If they have more than 4GiB, we use 3/4.
    */
   uint64_t available_ram;
   if (total_ram <= 4ull * 1024 * 1024 * 1024)
      available_ram = total_ram / 2;
   else
      available_ram = total_ram * 3 / 4;

   return available_ram;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetPhysicalDeviceMemoryProperties2(
   VkPhysicalDevice physicalDevice,
   VkPhysicalDeviceMemoryProperties2 *pMemoryProperties)
{
   pMemoryProperties->memoryProperties = (VkPhysicalDeviceMemoryProperties){
      .memoryHeapCount = 1,
      .memoryHeaps[0].size = panvk_get_system_heap_size(),
      .memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT,
      .memoryTypeCount = 1,
      .memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT |
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      .memoryTypes[0].heapIndex = 0,
   };
}

static VkResult
panvk_queue_init(struct panvk_device *device, struct panvk_queue *queue,
                 int idx, const VkDeviceQueueCreateInfo *create_info)
{
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(device->vk.physical);

   VkResult result = vk_queue_init(&queue->vk, &device->vk, create_info, idx);
   if (result != VK_SUCCESS)
      return result;

   struct drm_syncobj_create create = {
      .flags = DRM_SYNCOBJ_CREATE_SIGNALED,
   };

   int ret = drmIoctl(device->vk.drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
   if (ret) {
      vk_queue_finish(&queue->vk);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   unsigned arch = pan_arch(phys_dev->kmod.props.gpu_prod_id);

   switch (arch) {
   case 6:
      queue->vk.driver_submit = panvk_v6_queue_submit;
      break;
   case 7:
      queue->vk.driver_submit = panvk_v7_queue_submit;
      break;
   default:
      unreachable("Unsupported architecture");
   }

   queue->sync = create.handle;
   return VK_SUCCESS;
}

static void
panvk_queue_finish(struct panvk_queue *queue)
{
   vk_queue_finish(&queue->vk);
}

struct panvk_priv_bo *
panvk_priv_bo_create(struct panvk_device *dev, size_t size, uint32_t flags,
                     const struct VkAllocationCallbacks *alloc,
                     VkSystemAllocationScope scope)
{
   int ret;
   struct panvk_priv_bo *priv_bo =
      vk_zalloc2(&dev->vk.alloc, alloc, sizeof(*priv_bo), 8, scope);

   if (!priv_bo)
      return NULL;

   struct pan_kmod_bo *bo =
      pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, size, flags);
   if (!bo)
      goto err_free_priv_bo;

   priv_bo->bo = bo;
   priv_bo->dev = dev;

   if (!(flags & PAN_KMOD_BO_FLAG_NO_MMAP)) {
      priv_bo->addr.host = pan_kmod_bo_mmap(
         bo, 0, pan_kmod_bo_size(bo), PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
      if (priv_bo->addr.host == MAP_FAILED)
         goto err_put_bo;
   }

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = PAN_KMOD_VM_MAP_AUTO_VA,
         .size = pan_kmod_bo_size(bo),
      },
      .map = {
         .bo = priv_bo->bo,
         .bo_offset = 0,
      },
   };

   ret = pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   if (ret)
      goto err_munmap_bo;

   priv_bo->addr.dev = op.va.start;

   if (dev->debug.decode_ctx) {
      pandecode_inject_mmap(dev->debug.decode_ctx, priv_bo->addr.dev,
                            priv_bo->addr.host, pan_kmod_bo_size(priv_bo->bo),
                            NULL);
   }

   return priv_bo;

err_munmap_bo:
   if (priv_bo->addr.host) {
      ret = os_munmap(priv_bo->addr.host, pan_kmod_bo_size(bo));
      assert(!ret);
   }

err_put_bo:
   pan_kmod_bo_put(bo);

err_free_priv_bo:
   vk_free2(&dev->vk.alloc, alloc, priv_bo);
   return NULL;
}

void
panvk_priv_bo_destroy(struct panvk_priv_bo *priv_bo,
                      const VkAllocationCallbacks *alloc)
{
   if (!priv_bo)
      return;

   struct panvk_device *dev = priv_bo->dev;

   if (dev->debug.decode_ctx) {
      pandecode_inject_free(dev->debug.decode_ctx, priv_bo->addr.dev,
                            pan_kmod_bo_size(priv_bo->bo));
   }

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {
         .start = priv_bo->addr.dev,
         .size = pan_kmod_bo_size(priv_bo->bo),
      },
   };
   ASSERTED int ret =
      pan_kmod_vm_bind(dev->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   assert(!ret);

   if (priv_bo->addr.host) {
      ret = os_munmap(priv_bo->addr.host, pan_kmod_bo_size(priv_bo->bo));
      assert(!ret);
   }

   pan_kmod_bo_put(priv_bo->bo);
   vk_free2(&dev->vk.alloc, alloc, priv_bo);
}

/* Always reserve the lower 32MB. */
#define PANVK_VA_RESERVE_BOTTOM 0x2000000ull

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateDevice(VkPhysicalDevice physicalDevice,
                   const VkDeviceCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkDevice *pDevice)
{
   VK_FROM_HANDLE(panvk_physical_device, physical_device, physicalDevice);
   struct panvk_instance *instance =
      to_panvk_instance(physical_device->vk.instance);
   VkResult result;
   struct panvk_device *device;

   device = vk_zalloc2(&instance->vk.alloc, pAllocator, sizeof(*device), 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!device)
      return vk_error(physical_device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const struct vk_device_entrypoint_table *dev_entrypoints;
   const struct vk_command_buffer_ops *cmd_buffer_ops;
   struct vk_device_dispatch_table dispatch_table;
   unsigned arch = pan_arch(physical_device->kmod.props.gpu_prod_id);

   switch (arch) {
   case 6:
      dev_entrypoints = &panvk_v6_device_entrypoints;
      cmd_buffer_ops = &panvk_v6_cmd_buffer_ops;
      break;
   case 7:
      dev_entrypoints = &panvk_v7_device_entrypoints;
      cmd_buffer_ops = &panvk_v7_cmd_buffer_ops;
      break;
   default:
      unreachable("Unsupported architecture");
   }

   /* For secondary command buffer support, overwrite any command entrypoints
    * in the main device-level dispatch table with
    * vk_cmd_enqueue_unless_primary_Cmd*.
    */
   vk_device_dispatch_table_from_entrypoints(
      &dispatch_table, &vk_cmd_enqueue_unless_primary_device_entrypoints, true);

   vk_device_dispatch_table_from_entrypoints(&dispatch_table, dev_entrypoints,
                                             false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &panvk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(&dispatch_table,
                                             &wsi_device_entrypoints, false);

   /* Populate our primary cmd_dispatch table. */
   vk_device_dispatch_table_from_entrypoints(&device->cmd_dispatch,
                                             dev_entrypoints, true);
   vk_device_dispatch_table_from_entrypoints(&device->cmd_dispatch,
                                             &panvk_device_entrypoints, false);
   vk_device_dispatch_table_from_entrypoints(
      &device->cmd_dispatch, &vk_common_device_entrypoints, false);

   result = vk_device_init(&device->vk, &physical_device->vk, &dispatch_table,
                           pCreateInfo, pAllocator);
   if (result != VK_SUCCESS) {
      vk_free(&device->vk.alloc, device);
      return result;
   }

   /* Must be done after vk_device_init() because this function memset(0) the
    * whole struct.
    */
   device->vk.command_dispatch_table = &device->cmd_dispatch;
   device->vk.command_buffer_ops = cmd_buffer_ops;

   device->kmod.allocator = (struct pan_kmod_allocator){
      .zalloc = panvk_kmod_zalloc,
      .free = panvk_kmod_free,
      .priv = &device->vk.alloc,
   };
   device->kmod.dev =
      pan_kmod_dev_create(dup(physical_device->kmod.dev->fd),
                          PAN_KMOD_DEV_FLAG_OWNS_FD, &device->kmod.allocator);

   if (instance->debug_flags & PANVK_DEBUG_TRACE)
      device->debug.decode_ctx = pandecode_create_context(false);

   /* 32bit address space, with the lower 32MB reserved. We clamp
    * things so it matches kmod VA range limitations.
    */
   uint64_t user_va_start = panfrost_clamp_to_usable_va_range(
      device->kmod.dev, PANVK_VA_RESERVE_BOTTOM);
   uint64_t user_va_end =
      panfrost_clamp_to_usable_va_range(device->kmod.dev, 1ull << 32);

   device->kmod.vm =
      pan_kmod_vm_create(device->kmod.dev, PAN_KMOD_VM_FLAG_AUTO_VA,
                         user_va_start, user_va_end - user_va_start);

   device->tiler_heap = panvk_priv_bo_create(
      device, 128 * 1024 * 1024,
      PAN_KMOD_BO_FLAG_NO_MMAP | PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT,
      &device->vk.alloc, VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);

   device->sample_positions = panvk_priv_bo_create(
      device, panfrost_sample_positions_buffer_size(), 0, &device->vk.alloc,
      VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   panfrost_upload_sample_positions(device->sample_positions->addr.host);

   vk_device_set_drm_fd(&device->vk, device->kmod.dev->fd);

   panvk_arch_dispatch(arch, meta_init, device);

   for (unsigned i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
      const VkDeviceQueueCreateInfo *queue_create =
         &pCreateInfo->pQueueCreateInfos[i];
      uint32_t qfi = queue_create->queueFamilyIndex;
      device->queues[qfi] =
         vk_alloc(&device->vk.alloc,
                  queue_create->queueCount * sizeof(struct panvk_queue), 8,
                  VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
      if (!device->queues[qfi]) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      memset(device->queues[qfi], 0,
             queue_create->queueCount * sizeof(struct panvk_queue));

      device->queue_count[qfi] = queue_create->queueCount;

      for (unsigned q = 0; q < queue_create->queueCount; q++) {
         result =
            panvk_queue_init(device, &device->queues[qfi][q], q, queue_create);
         if (result != VK_SUCCESS)
            goto fail;
      }
   }

   *pDevice = panvk_device_to_handle(device);
   return VK_SUCCESS;

fail:
   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   panvk_arch_dispatch(pan_arch(physical_device->kmod.props.gpu_prod_id),
                       meta_cleanup, device);
   panvk_priv_bo_destroy(device->tiler_heap, &device->vk.alloc);
   panvk_priv_bo_destroy(device->sample_positions, &device->vk.alloc);
   pan_kmod_vm_destroy(device->kmod.vm);
   pan_kmod_dev_destroy(device->kmod.dev);

   vk_free(&device->vk.alloc, device);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyDevice(VkDevice _device, const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_physical_device *physical_device =
      to_panvk_physical_device(device->vk.physical);

   if (!device)
      return;

   for (unsigned i = 0; i < PANVK_MAX_QUEUE_FAMILIES; i++) {
      for (unsigned q = 0; q < device->queue_count[i]; q++)
         panvk_queue_finish(&device->queues[i][q]);
      if (device->queue_count[i])
         vk_object_free(&device->vk, NULL, device->queues[i]);
   }

   panvk_arch_dispatch(pan_arch(physical_device->kmod.props.gpu_prod_id),
                       meta_cleanup, device);
   panvk_priv_bo_destroy(device->tiler_heap, &device->vk.alloc);
   panvk_priv_bo_destroy(device->sample_positions, &device->vk.alloc);
   pan_kmod_vm_destroy(device->kmod.vm);

   if (device->debug.decode_ctx)
      pandecode_destroy_context(device->debug.decode_ctx);

   pan_kmod_dev_destroy(device->kmod.dev);
   vk_free(&device->vk.alloc, device);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_EnumerateInstanceLayerProperties(uint32_t *pPropertyCount,
                                       VkLayerProperties *pProperties)
{
   *pPropertyCount = 0;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_QueueWaitIdle(VkQueue _queue)
{
   VK_FROM_HANDLE(panvk_queue, queue, _queue);
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);

   if (vk_device_is_lost(&dev->vk))
      return VK_ERROR_DEVICE_LOST;

   struct drm_syncobj_wait wait = {
      .handles = (uint64_t)(uintptr_t)(&queue->sync),
      .count_handles = 1,
      .timeout_nsec = INT64_MAX,
      .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL,
   };
   int ret;

   ret = drmIoctl(queue->vk.base.device->drm_fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);
   assert(!ret);

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_EnumerateInstanceExtensionProperties(const char *pLayerName,
                                           uint32_t *pPropertyCount,
                                           VkExtensionProperties *pProperties)
{
   if (pLayerName)
      return vk_error(NULL, VK_ERROR_LAYER_NOT_PRESENT);

   return vk_enumerate_instance_extension_properties(
      &panvk_instance_extensions, pPropertyCount, pProperties);
}

PFN_vkVoidFunction
panvk_GetInstanceProcAddr(VkInstance _instance, const char *pName)
{
   VK_FROM_HANDLE(panvk_instance, instance, _instance);
   return vk_instance_get_proc_addr(&instance->vk, &panvk_instance_entrypoints,
                                    pName);
}

/* The loader wants us to expose a second GetInstanceProcAddr function
 * to work around certain LD_PRELOAD issues seen in apps.
 */
PUBLIC
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName)
{
   return panvk_GetInstanceProcAddr(instance, pName);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_AllocateMemory(VkDevice _device,
                     const VkMemoryAllocateInfo *pAllocateInfo,
                     const VkAllocationCallbacks *pAllocator,
                     VkDeviceMemory *pMem)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_device_memory *mem;
   bool can_be_exported = false;
   VkResult result;

   assert(pAllocateInfo->sType == VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);

   if (pAllocateInfo->allocationSize == 0) {
      /* Apparently, this is allowed */
      *pMem = VK_NULL_HANDLE;
      return VK_SUCCESS;
   }

   const VkExportMemoryAllocateInfo *export_info =
      vk_find_struct_const(pAllocateInfo->pNext, EXPORT_MEMORY_ALLOCATE_INFO);

   if (export_info) {
      if (export_info->handleTypes &
          ~(VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT |
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
         return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      else if (export_info->handleTypes)
         can_be_exported = true;
   }

   mem = vk_device_memory_create(&device->vk, pAllocateInfo, pAllocator,
                                 sizeof(*mem));
   if (mem == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkImportMemoryFdInfoKHR *fd_info =
      vk_find_struct_const(pAllocateInfo->pNext, IMPORT_MEMORY_FD_INFO_KHR);

   if (fd_info && !fd_info->handleType)
      fd_info = NULL;

   if (fd_info) {
      assert(
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
         fd_info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

      /*
       * TODO Importing the same fd twice gives us the same handle without
       * reference counting.  We need to maintain a per-instance handle-to-bo
       * table and add reference count to panvk_bo.
       */
      mem->bo = pan_kmod_bo_import(device->kmod.dev, fd_info->fd, 0);
      if (!mem->bo) {
         result = vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
         goto err_destroy_mem;
      }
   } else {
      mem->bo = pan_kmod_bo_alloc(device->kmod.dev,
                                  can_be_exported ? NULL : device->kmod.vm,
                                  pAllocateInfo->allocationSize, 0);
      if (!mem->bo) {
         result = vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
         goto err_destroy_mem;
      }
   }

   /* Always GPU-map at creation time. */
   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_MAP,
      .va = {
         .start = PAN_KMOD_VM_MAP_AUTO_VA,
         .size = pan_kmod_bo_size(mem->bo),
      },
      .map = {
         .bo = mem->bo,
         .bo_offset = 0,
      },
   };

   int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   if (ret) {
      result = vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      goto err_put_bo;
   }

   mem->addr.dev = op.va.start;

   if (fd_info) {
      /* From the Vulkan spec:
       *
       *    "Importing memory from a file descriptor transfers ownership of
       *    the file descriptor from the application to the Vulkan
       *    implementation. The application must not perform any operations on
       *    the file descriptor after a successful import."
       *
       * If the import fails, we leave the file descriptor open.
       */
      close(fd_info->fd);
   }

   if (device->debug.decode_ctx) {
      pandecode_inject_mmap(device->debug.decode_ctx, mem->addr.dev, NULL,
                            pan_kmod_bo_size(mem->bo), NULL);
   }

   *pMem = panvk_device_memory_to_handle(mem);

   return VK_SUCCESS;

err_put_bo:
   pan_kmod_bo_put(mem->bo);

err_destroy_mem:
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
   return result;
}

VKAPI_ATTR void VKAPI_CALL
panvk_FreeMemory(VkDevice _device, VkDeviceMemory _mem,
                 const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, _mem);

   if (mem == NULL)
      return;

   if (device->debug.decode_ctx) {
      pandecode_inject_free(device->debug.decode_ctx, mem->addr.dev,
                            pan_kmod_bo_size(mem->bo));
   }

   struct pan_kmod_vm_op op = {
      .type = PAN_KMOD_VM_OP_TYPE_UNMAP,
      .va = {
         .start = mem->addr.dev,
         .size = pan_kmod_bo_size(mem->bo),
      },
   };

   ASSERTED int ret =
      pan_kmod_vm_bind(device->kmod.vm, PAN_KMOD_VM_OP_MODE_IMMEDIATE, &op, 1);
   assert(!ret);

   pan_kmod_bo_put(mem->bo);
   vk_device_memory_destroy(&device->vk, pAllocator, &mem->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_MapMemory2KHR(VkDevice _device, const VkMemoryMapInfoKHR *pMemoryMapInfo,
                    void **ppData)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryMapInfo->memory);

   if (mem == NULL) {
      *ppData = NULL;
      return VK_SUCCESS;
   }

   const VkDeviceSize offset = pMemoryMapInfo->offset;
   const VkDeviceSize size = vk_device_memory_range(
      &mem->vk, pMemoryMapInfo->offset, pMemoryMapInfo->size);

   /* From the Vulkan spec version 1.0.32 docs for MapMemory:
    *
    *  * If size is not equal to VK_WHOLE_SIZE, size must be greater than 0
    *    assert(size != 0);
    *  * If size is not equal to VK_WHOLE_SIZE, size must be less than or
    *    equal to the size of the memory minus offset
    */
   assert(size > 0);
   assert(offset + size <= mem->bo->size);

   if (size != (size_t)size) {
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "requested size 0x%" PRIx64 " does not fit in %u bits",
                       size, (unsigned)(sizeof(size_t) * 8));
   }

   /* From the Vulkan 1.2.194 spec:
    *
    *    "memory must not be currently host mapped"
    */
   if (mem->addr.host)
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object already mapped.");

   void *addr = pan_kmod_bo_mmap(mem->bo, 0, pan_kmod_bo_size(mem->bo),
                                 PROT_READ | PROT_WRITE, MAP_SHARED, NULL);
   if (addr == MAP_FAILED)
      return vk_errorf(device, VK_ERROR_MEMORY_MAP_FAILED,
                       "Memory object couldn't be mapped.");

   mem->addr.host = addr;
   *ppData = mem->addr.host + offset;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_UnmapMemory2KHR(VkDevice _device,
                      const VkMemoryUnmapInfoKHR *pMemoryUnmapInfo)
{
   VK_FROM_HANDLE(panvk_device_memory, mem, pMemoryUnmapInfo->memory);

   if (mem->addr.host) {
      ASSERTED int ret =
         os_munmap((void *)mem->addr.host, pan_kmod_bo_size(mem->bo));

      assert(!ret);
      mem->addr.host = NULL;
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_FlushMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                              const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_InvalidateMappedMemoryRanges(VkDevice _device, uint32_t memoryRangeCount,
                                   const VkMappedMemoryRange *pMemoryRanges)
{
   return VK_SUCCESS;
}

VKAPI_ATTR VkDeviceAddress VKAPI_CALL
panvk_GetBufferDeviceAddress(VkDevice _device,
                             const VkBufferDeviceAddressInfo *pInfo)
{
   VK_FROM_HANDLE(panvk_buffer, buffer, pInfo->buffer);

   return buffer->dev_addr;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetBufferMemoryRequirements2(VkDevice device,
                                   const VkBufferMemoryRequirementsInfo2 *pInfo,
                                   VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(panvk_buffer, buffer, pInfo->buffer);

   const uint64_t alignment = 64;
   const uint64_t size = align64(buffer->vk.size, alignment);

   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = alignment;
   pMemoryRequirements->memoryRequirements.size = size;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetImageMemoryRequirements2(VkDevice device,
                                  const VkImageMemoryRequirementsInfo2 *pInfo,
                                  VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(panvk_image, image, pInfo->image);

   const uint64_t alignment = 4096;
   const uint64_t size = panvk_image_get_total_size(image);

   pMemoryRequirements->memoryRequirements.memoryTypeBits = 1;
   pMemoryRequirements->memoryRequirements.alignment = alignment;
   pMemoryRequirements->memoryRequirements.size = size;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetImageSparseMemoryRequirements2(
   VkDevice device, const VkImageSparseMemoryRequirementsInfo2 *pInfo,
   uint32_t *pSparseMemoryRequirementCount,
   VkSparseImageMemoryRequirements2 *pSparseMemoryRequirements)
{
   panvk_stub();
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetDeviceMemoryCommitment(VkDevice device, VkDeviceMemory memory,
                                VkDeviceSize *pCommittedMemoryInBytes)
{
   *pCommittedMemoryInBytes = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_BindBufferMemory2(VkDevice device, uint32_t bindInfoCount,
                        const VkBindBufferMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(panvk_device_memory, mem, pBindInfos[i].memory);
      VK_FROM_HANDLE(panvk_buffer, buffer, pBindInfos[i].buffer);
      struct pan_kmod_bo *old_bo = buffer->bo;

      assert(mem != NULL);

      buffer->bo = pan_kmod_bo_get(mem->bo);
      buffer->dev_addr = mem->addr.dev + pBindInfos[i].memoryOffset;

      /* FIXME: Only host map for index buffers so we can do the min/max
       * index retrieval on the CPU. This is all broken anyway and the
       * min/max search should be done with a compute shader that also
       * patches the job descriptor accordingly (basically an indirect draw).
       *
       * Make sure this goes away as soon as we fixed indirect draws.
       */
      if (buffer->vk.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
         VkDeviceSize offset = pBindInfos[i].memoryOffset;
         VkDeviceSize pgsize = getpagesize();
         off_t map_start = offset & ~(pgsize - 1);
         off_t map_end = offset + buffer->vk.size;
         void *map_addr =
            pan_kmod_bo_mmap(mem->bo, map_start, map_end - map_start,
                             PROT_WRITE, MAP_SHARED, NULL);

         assert(map_addr != MAP_FAILED);
         buffer->host_ptr = map_addr + (offset & pgsize);
      }

      pan_kmod_bo_put(old_bo);
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_BindImageMemory2(VkDevice device, uint32_t bindInfoCount,
                       const VkBindImageMemoryInfo *pBindInfos)
{
   for (uint32_t i = 0; i < bindInfoCount; ++i) {
      VK_FROM_HANDLE(panvk_image, image, pBindInfos[i].image);
      VK_FROM_HANDLE(panvk_device_memory, mem, pBindInfos[i].memory);
      struct pan_kmod_bo *old_bo = image->bo;

      assert(mem);
      image->bo = pan_kmod_bo_get(mem->bo);
      image->pimage.data.base = mem->addr.dev;
      image->pimage.data.offset = pBindInfos[i].memoryOffset;
      /* Reset the AFBC headers */
      if (drm_is_afbc(image->pimage.layout.modifier)) {
         /* Transient CPU mapping */
         void *base = pan_kmod_bo_mmap(mem->bo, 0, pan_kmod_bo_size(mem->bo),
                                       PROT_WRITE, MAP_SHARED, NULL);

         assert(base != MAP_FAILED);

         for (unsigned layer = 0; layer < image->pimage.layout.array_size;
              layer++) {
            for (unsigned level = 0; level < image->pimage.layout.nr_slices;
                 level++) {
               void *header = base + image->pimage.data.offset +
                              (layer * image->pimage.layout.array_stride) +
                              image->pimage.layout.slices[level].offset;
               memset(header, 0,
                      image->pimage.layout.slices[level].afbc.header_size);
            }
         }

         ASSERTED int ret = os_munmap(base, pan_kmod_bo_size(mem->bo));
         assert(!ret);
      }

      pan_kmod_bo_put(old_bo);
   }

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateEvent(VkDevice _device, const VkEventCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkEvent *pEvent)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_event *event = vk_object_zalloc(
      &device->vk, pAllocator, sizeof(*event), VK_OBJECT_TYPE_EVENT);
   if (!event)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct drm_syncobj_create create = {
      .flags = 0,
   };

   int ret = drmIoctl(device->vk.drm_fd, DRM_IOCTL_SYNCOBJ_CREATE, &create);
   if (ret)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   event->syncobj = create.handle;
   *pEvent = panvk_event_to_handle(event);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyEvent(VkDevice _device, VkEvent _event,
                   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_event, event, _event);

   if (!event)
      return;

   struct drm_syncobj_destroy destroy = {.handle = event->syncobj};
   drmIoctl(device->vk.drm_fd, DRM_IOCTL_SYNCOBJ_DESTROY, &destroy);

   vk_object_free(&device->vk, pAllocator, event);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetEventStatus(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_event, event, _event);
   bool signaled;

   struct drm_syncobj_wait wait = {
      .handles = (uintptr_t)&event->syncobj,
      .count_handles = 1,
      .timeout_nsec = 0,
      .flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
   };

   int ret = drmIoctl(device->vk.drm_fd, DRM_IOCTL_SYNCOBJ_WAIT, &wait);
   if (ret) {
      if (errno == ETIME)
         signaled = false;
      else {
         assert(0);
         return VK_ERROR_DEVICE_LOST; /* TODO */
      }
   } else
      signaled = true;

   return signaled ? VK_EVENT_SET : VK_EVENT_RESET;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_SetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_event, event, _event);

   struct drm_syncobj_array objs = {
      .handles = (uint64_t)(uintptr_t)&event->syncobj,
      .count_handles = 1};

   /* This is going to just replace the fence for this syncobj with one that
    * is already in signaled state. This won't be a problem because the spec
    * mandates that the event will have been set before the vkCmdWaitEvents
    * command executes.
    * https://www.khronos.org/registry/vulkan/specs/1.2/html/chap6.html#commandbuffers-submission-progress
    */
   if (drmIoctl(device->vk.drm_fd, DRM_IOCTL_SYNCOBJ_SIGNAL, &objs))
      return VK_ERROR_DEVICE_LOST;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_ResetEvent(VkDevice _device, VkEvent _event)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_event, event, _event);

   struct drm_syncobj_array objs = {
      .handles = (uint64_t)(uintptr_t)&event->syncobj,
      .count_handles = 1};

   if (drmIoctl(device->vk.drm_fd, DRM_IOCTL_SYNCOBJ_RESET, &objs))
      return VK_ERROR_DEVICE_LOST;

   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_CreateBuffer(VkDevice _device, const VkBufferCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   struct panvk_buffer *buffer;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);

   buffer =
      vk_buffer_create(&device->vk, pCreateInfo, pAllocator, sizeof(*buffer));
   if (buffer == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   *pBuffer = panvk_buffer_to_handle(buffer);

   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroyBuffer(VkDevice _device, VkBuffer _buffer,
                    const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_buffer, buffer, _buffer);

   if (!buffer)
      return;

   if (buffer->host_ptr) {
      VkDeviceSize pgsize = getpagesize();
      uintptr_t map_start = (uintptr_t)buffer->host_ptr & ~(pgsize - 1);
      uintptr_t map_end =
         ALIGN_POT((uintptr_t)buffer->host_ptr + buffer->vk.size, pgsize);
      ASSERTED int ret = os_munmap((void *)map_start, map_end - map_start);

      assert(!ret);
      buffer->host_ptr = NULL;
   }

   pan_kmod_bo_put(buffer->bo);
   vk_buffer_destroy(&device->vk, pAllocator, &buffer->vk);
}

VKAPI_ATTR void VKAPI_CALL
panvk_DestroySampler(VkDevice _device, VkSampler _sampler,
                     const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_sampler, sampler, _sampler);

   if (!sampler)
      return;

   vk_sampler_destroy(&device->vk, pAllocator, &sampler->vk);
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdKHR(VkDevice _device, const VkMemoryGetFdInfoKHR *pGetFdInfo,
                     int *pFd)
{
   VK_FROM_HANDLE(panvk_device, device, _device);
   VK_FROM_HANDLE(panvk_device_memory, memory, pGetFdInfo->memory);

   assert(pGetFdInfo->sType == VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR);

   /* At the moment, we support only the below handle types. */
   assert(
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT ||
      pGetFdInfo->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);

   int prime_fd = pan_kmod_bo_export(memory->bo);
   if (prime_fd < 0)
      return vk_error(device, VK_ERROR_OUT_OF_DEVICE_MEMORY);

   *pFd = prime_fd;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_GetMemoryFdPropertiesKHR(VkDevice _device,
                               VkExternalMemoryHandleTypeFlagBits handleType,
                               int fd,
                               VkMemoryFdPropertiesKHR *pMemoryFdProperties)
{
   assert(handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
   pMemoryFdProperties->memoryTypeBits = 1;
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetPhysicalDeviceExternalSemaphoreProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalSemaphoreInfo *pExternalSemaphoreInfo,
   VkExternalSemaphoreProperties *pExternalSemaphoreProperties)
{
   if ((pExternalSemaphoreInfo->handleType ==
           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT ||
        pExternalSemaphoreInfo->handleType ==
           VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT)) {
      pExternalSemaphoreProperties->exportFromImportedHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT |
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->compatibleHandleTypes =
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT |
         VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      pExternalSemaphoreProperties->externalSemaphoreFeatures =
         VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT |
         VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
   } else {
      pExternalSemaphoreProperties->exportFromImportedHandleTypes = 0;
      pExternalSemaphoreProperties->compatibleHandleTypes = 0;
      pExternalSemaphoreProperties->externalSemaphoreFeatures = 0;
   }
}

VKAPI_ATTR void VKAPI_CALL
panvk_GetPhysicalDeviceExternalFenceProperties(
   VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalFenceInfo *pExternalFenceInfo,
   VkExternalFenceProperties *pExternalFenceProperties)
{
   pExternalFenceProperties->exportFromImportedHandleTypes = 0;
   pExternalFenceProperties->compatibleHandleTypes = 0;
   pExternalFenceProperties->externalFenceFeatures = 0;
}
