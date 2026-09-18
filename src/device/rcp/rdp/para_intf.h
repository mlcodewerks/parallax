#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <libretro_vulkan.h>

#ifdef __cplusplus
extern "C"
{
#endif

void vk_rasterize(void);
void vk_process_commands(void);
bool vk_init(void);
void vk_destroy(void);
void vk_context_destroy(void);
unsigned vk_frame_width(void);
unsigned vk_frame_height(void);
void vk_set_hw_render_interface(const struct retro_hw_render_interface_vulkan *iface);
bool vk_create_device(struct retro_vulkan_context *frontend_context,
                      VkInstance instance,
                      VkPhysicalDevice gpu,
                      VkSurfaceKHR surface,
                      PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                      const char **required_device_extensions,
                      unsigned num_required_device_extensions,
                      const char **required_device_layers,
                      unsigned num_required_device_layers,
                      const VkPhysicalDeviceFeatures *required_features);
const VkApplicationInfo *vk_get_application_info(void);
void video_render(void);
void screen_swap(bool blank);
void init_framebuffer(int width, int height);

#ifdef __cplusplus
}
#endif
