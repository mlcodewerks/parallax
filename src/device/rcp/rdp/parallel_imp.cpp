#include "parallel_imp.h"
#include <cstring>
#include <memory>
#include <vector>
#include "rdp_device.hpp"
#include "context.hpp"
#include "device.hpp"
#include "gfxstructdefs.h"
#include "para_intf.h"
#include "Gfx #1.3.h"
#include "common.h"
#include "gfxstructdefs.h"
#include <libretro_vulkan.h>
#include "renderer_options.h"
extern "C" {
#include "rdp_core.h"
}
#include "performance_cores.h"

unsigned rdram_size = 8 * 1024 * 1024;

using namespace Vulkan;
using namespace std;

static int cmd_cur;
static int cmd_ptr;
static uint32_t cmd_data[0x00040000 >> 2];

static unique_ptr<RDP::CommandProcessor> frontend;
static unique_ptr<Device> device;
static unique_ptr<Context> context;
static const struct retro_hw_render_interface_vulkan *vk_hw;
static vector<retro_vulkan_image> retro_images;
static vector<Vulkan::ImageHandle> retro_image_handles;

int32_t vk_rescaling;
bool vk_ssreadbacks;
bool vk_ssdither;
bool running = false;
unsigned width, height;

bool skip_swap_clear;
static bool vk_initialized;
struct RenderAffinity
{
    performance_affinity state;
    RenderAffinity() { performance_core_enter(0, &state); }
    ~RenderAffinity() { performance_core_leave(&state); }
};

static const unsigned cmd_len_lut[64] = {
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	4,
	6,
	12,
	14,
	12,
	14,
	20,
	22,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	2,
	2,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
	1,
};

#include <libretro.h>

static unsigned vk_sync_frame_count()
{
	unsigned count = 0;
	uint32_t mask;

	if (!vk_hw || !vk_hw->get_sync_index_mask)
		return 0;

	mask = vk_hw->get_sync_index_mask(vk_hw->handle);
	for (unsigned i = 0; i < 32; ++i)
		if (mask & (UINT32_C(1) << i))
			++count;
	return count;
}

static unsigned vk_sync_frame_slots()
{
	unsigned slots = 0;
	uint32_t mask;

	if (!vk_hw || !vk_hw->get_sync_index_mask)
		return 0;

	mask = vk_hw->get_sync_index_mask(vk_hw->handle);
	for (unsigned i = 0; i < 32; ++i)
		if (mask & (UINT32_C(1) << i))
			slots = i + 1;
	return slots;
}

static bool vk_publish_scanout(unsigned &out_width, unsigned &out_height)
{
	RDP::ScanoutOptions opts = {};
	Vulkan::ImageHandle image;
	unsigned index;
	unsigned slots;
	struct retro_vulkan_image *retro_image;

	out_width = 0;
	out_height = 0;
	if (!running || !frontend || !vk_hw || !vk_hw->set_image || !vk_hw->get_sync_index)
		return false;

	opts.persist_frame_on_invalid_input = true;
	opts.crop_rect.top = 1;
	opts.crop_rect.bottom = 1;
	opts.crop_rect.enable = renderer_settings.overscan;
	opts.vi.aa = renderer_settings.vi_filter;
	opts.vi.scale = renderer_settings.vi_filter;
	opts.vi.dither_filter = renderer_settings.dedither;
	opts.vi.divot_filter = renderer_settings.blur;
	opts.downscale_steps = renderer_settings.downscale;

	image = frontend->scanout(opts);
	if (!image || !image->get_width() || !image->get_height())
		return false;

	slots = vk_sync_frame_slots();
	if (slots == 0)
		return false;
	if (retro_images.size() != slots)
	{
		retro_images.resize(slots);
		retro_image_handles.resize(slots);
	}

	index = vk_hw->get_sync_index(vk_hw->handle);
	if (index >= retro_images.size())
		return false;

	retro_image = &retro_images[index];
	memset(retro_image, 0, sizeof(*retro_image));
	retro_image->image_view = image->get_view().get_view();
	retro_image->image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	retro_image->create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	retro_image->create_info.image = image->get_image();
	retro_image->create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	retro_image->create_info.format = image->get_format();
	retro_image->create_info.components.r = VK_COMPONENT_SWIZZLE_R;
	retro_image->create_info.components.g = VK_COMPONENT_SWIZZLE_G;
	retro_image->create_info.components.b = VK_COMPONENT_SWIZZLE_B;
	retro_image->create_info.components.a = VK_COMPONENT_SWIZZLE_A;
	retro_image->create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	retro_image->create_info.subresourceRange.baseMipLevel = 0;
	retro_image->create_info.subresourceRange.levelCount = 1;
	retro_image->create_info.subresourceRange.baseArrayLayer = 0;
	retro_image->create_info.subresourceRange.layerCount = 1;

	/* paraLLEl-RDP and the libretro frontend share the same graphics queue.
	 * Granite's queue lock serializes submissions; scanout() transitions this
	 * image to SHADER_READ_ONLY before publishing it. */
	vk_hw->set_image(vk_hw->handle, retro_image, 0, nullptr, VK_QUEUE_FAMILY_IGNORED);
	retro_image_handles[index] = image;
	out_width = image->get_width();
	out_height = image->get_height();
	return true;
}

void vk_rasterize()
{
    RenderAffinity affinity;
	unsigned frame_width = 0;
	unsigned frame_height = 0;

	if (!running || !frontend)
	{
		screen_swap(true);
		return;
	}

	if (vk_hw && vk_hw->wait_sync_index)
		vk_hw->wait_sync_index(vk_hw->handle);

	frontend->set_vi_register(RDP::VIRegister::Control, *GET_GFX_INFO(VI_STATUS_REG));
	frontend->set_vi_register(RDP::VIRegister::Origin, *GET_GFX_INFO(VI_ORIGIN_REG));
	frontend->set_vi_register(RDP::VIRegister::Width, *GET_GFX_INFO(VI_WIDTH_REG));
	frontend->set_vi_register(RDP::VIRegister::Intr, *GET_GFX_INFO(VI_INTR_REG));
	frontend->set_vi_register(RDP::VIRegister::VCurrentLine, *GET_GFX_INFO(VI_V_CURRENT_LINE_REG));
	frontend->set_vi_register(RDP::VIRegister::Timing, *GET_GFX_INFO(VI_TIMING_REG));
	frontend->set_vi_register(RDP::VIRegister::VSync, *GET_GFX_INFO(VI_V_SYNC_REG));
	frontend->set_vi_register(RDP::VIRegister::HSync, *GET_GFX_INFO(VI_H_SYNC_REG));
	frontend->set_vi_register(RDP::VIRegister::Leap, *GET_GFX_INFO(VI_LEAP_REG));
	frontend->set_vi_register(RDP::VIRegister::HStart, *GET_GFX_INFO(VI_H_START_REG));
	frontend->set_vi_register(RDP::VIRegister::VStart, *GET_GFX_INFO(VI_V_START_REG));
	frontend->set_vi_register(RDP::VIRegister::VBurst, *GET_GFX_INFO(VI_V_BURST_REG));
	frontend->set_vi_register(RDP::VIRegister::XScale, *GET_GFX_INFO(VI_X_SCALE_REG));
	frontend->set_vi_register(RDP::VIRegister::YScale, *GET_GFX_INFO(VI_Y_SCALE_REG));

	RDP::Quirks quirks;
	quirks.set_native_texture_lod(renderer_settings.native_lod);
	quirks.set_native_resolution_tex_rect(renderer_settings.native_tex_rect);
	frontend->set_quirks(quirks);

	if (vk_publish_scanout(frame_width, frame_height))
	{
		width = frame_width;
		height = frame_height;
		screen_swap(false);
	}
	else
	{
		screen_swap(true);
	}

	frontend->begin_frame_context();
}

void vk_process_commands()
{
    RenderAffinity affinity;
	if (running)
	{

		const uint32_t DP_CURRENT = *GET_GFX_INFO(DPC_CURRENT_REG) & 0x00FFFFF8;
		const uint32_t DP_END = *GET_GFX_INFO(DPC_END_REG) & 0x00FFFFF8;

		int length = DP_END - DP_CURRENT;
		if (length <= 0)
			return;

		length = unsigned(length) >> 3;
		if ((cmd_ptr + length) & ~(0x0003FFFF >> 3))
			return;

		uint32_t offset = DP_CURRENT;
		if (*GET_GFX_INFO(DPC_STATUS_REG) & DP_STATUS_XBUS_DMA)
		{
			do
			{
				offset &= 0xFF8;
				cmd_data[2 * cmd_ptr + 0] = *reinterpret_cast<const uint32_t *>(SP_DMEM + offset);
				cmd_data[2 * cmd_ptr + 1] = *reinterpret_cast<const uint32_t *>(SP_DMEM + offset + 4);
				offset += sizeof(uint64_t);
				cmd_ptr++;
			} while (--length > 0);
		}
		else
		{
			if (DP_END > 0x7ffffff || DP_CURRENT > 0x7ffffff)
			{
				return;
			}
			else
			{
				do
				{
					offset &= 0xFFFFF8;
                    const uint32_t *synthetic = rdp_hle_command_buffer(offset);
                    cmd_data[2 * cmd_ptr + 0] = synthetic ? synthetic[0] : *reinterpret_cast<const uint32_t *>(DRAM + offset);
                    cmd_data[2 * cmd_ptr + 1] = synthetic ? synthetic[1] : *reinterpret_cast<const uint32_t *>(DRAM + offset + 4);
					offset += sizeof(uint64_t);
					cmd_ptr++;
				} while (--length > 0);
			}
		}

		while (cmd_cur - cmd_ptr < 0)
		{
			uint32_t w1 = cmd_data[2 * cmd_cur];
			uint32_t command = (w1 >> 24) & 63;
			int cmd_length = cmd_len_lut[command];

			if (cmd_ptr - cmd_cur - cmd_length < 0)
			{
				*GET_GFX_INFO(DPC_CURRENT_REG) = *GET_GFX_INFO(DPC_END_REG);
				return;
			}

			if (command >= 8 && frontend)
				frontend->enqueue_command(cmd_length * 2, &cmd_data[2 * cmd_cur]);

			if (RDP::Op(command) == RDP::Op::SyncFull)
			{
				/* Finish GPU writes before exposing the full-sync interrupt. */
				frontend->wait_for_timeline(frontend->signal_timeline());
				*gfx_info.MI_INTR_REG |= DP_INTERRUPT;
				gfx_info.CheckInterrupts();
			}

			cmd_cur += cmd_length;
		}

		cmd_ptr = 0;
		cmd_cur = 0;
		*GET_GFX_INFO(DPC_CURRENT_REG) = *GET_GFX_INFO(DPC_END_REG);
	}
}

void vk_destroy()
{
	if (!vk_initialized)
		return;

	running = false;
	retro_image_handles.clear();
	retro_images.clear();
	frontend.reset();
	device.reset();
	vk_initialized = false;
}

void vk_set_hw_render_interface(const struct retro_hw_render_interface_vulkan *iface)
{
	vk_hw = iface;
}

void vk_context_destroy()
{
	vk_destroy();
	vk_hw = nullptr;
	context.reset();
}

bool vk_create_device(struct retro_vulkan_context *frontend_context,
                      VkInstance instance,
                      VkPhysicalDevice gpu,
                      VkSurfaceKHR surface,
                      PFN_vkGetInstanceProcAddr get_instance_proc_addr,
                      const char **required_device_extensions,
                      unsigned num_required_device_extensions,
                      const char **required_device_layers,
                      unsigned num_required_device_layers,
                      const VkPhysicalDeviceFeatures *required_features)
{
	Vulkan::Context::SystemHandles handles = {};
	(void)required_device_layers;
	(void)num_required_device_layers;

	if (!frontend_context || !Vulkan::Context::init_loader(get_instance_proc_addr))
		return false;

	context.reset(new Context);
	context->set_system_handles(handles);
	if (!context->init_device_from_instance(instance, gpu, surface,
	                                        required_device_extensions,
	                                        num_required_device_extensions,
	                                        required_features))
	{
		context.reset();
		return false;
	}

	frontend_context->gpu = context->get_gpu();
	frontend_context->device = context->get_device();
	frontend_context->queue = context->get_queue_info().queues[Vulkan::QUEUE_INDEX_GRAPHICS];
	frontend_context->queue_family_index =
	    context->get_queue_info().family_indices[Vulkan::QUEUE_INDEX_GRAPHICS];
	frontend_context->presentation_queue = frontend_context->queue;
	frontend_context->presentation_queue_family_index = frontend_context->queue_family_index;

	/* The libretro frontend owns the VkDevice returned by negotiation. */
	context->release_device();
	return true;
}

const VkApplicationInfo *vk_get_application_info(void)
{
	static const VkApplicationInfo app_info = {
	    VK_STRUCTURE_TYPE_APPLICATION_INFO,
	    nullptr,
	    "ultra64vm paraLLEl-RDP",
	    0,
	    "Granite",
	    0,
	    VK_API_VERSION_1_1,
	};
	return &app_info;
}

bool vk_init()
{
	unsigned sync_frames;
	unsigned slots;
	RDP::CommandProcessorFlags flags = 0;
	struct performance_affinity affinity;

	running = false;
	if (!context || !vk_hw)
		return false;
	if (vk_hw->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
	    vk_hw->interface_version < RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION)
		return false;

	device.reset(new Device);
	device->set_context(*context);

	sync_frames = vk_sync_frame_count();
	slots = vk_sync_frame_slots();
	if (sync_frames == 0 || slots == 0)
	{
		device.reset();
		return false;
	}
	device->init_frame_contexts(sync_frames);
	if (vk_hw->lock_queue && vk_hw->unlock_queue)
	{
		device->set_queue_lock(
		    []() { vk_hw->lock_queue(vk_hw->handle); },
		    []() { vk_hw->unlock_queue(vk_hw->handle); });
	}

	retro_images.clear();
	retro_image_handles.clear();
	retro_images.resize(slots);
	retro_image_handles.resize(slots);

	if (renderer_settings.upscale == 2) flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT;
	if (renderer_settings.upscale == 4) flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT;
	if (renderer_settings.upscale == 8) flags |= RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT;
	if (renderer_settings.ss_readbacks) flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT;
	if (renderer_settings.ss_dither) flags |= RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;
	rdram_size = *gfx_info.RDRAM_SIZE;
	performance_core_enter(0, &affinity);
	frontend.reset(new RDP::CommandProcessor(*device, reinterpret_cast<void *>(gfx_info.RDRAM),
	                                         0, rdram_size, rdram_size / 2, flags));
	performance_core_leave(&affinity);
	if (!frontend->device_is_supported())
	{
		frontend.reset();
		device.reset();
		return false;
	}

	running = true;
	vk_initialized = true;
	RDP::Quirks quirks;
	quirks.set_native_texture_lod(renderer_settings.native_lod);
	quirks.set_native_resolution_tex_rect(renderer_settings.native_tex_rect);
	frontend->set_quirks(quirks);
	cmd_cur = cmd_ptr = 0;
	width = height = 0;
	return true;
}

unsigned vk_frame_width(void)
{
	return width;
}

unsigned vk_frame_height(void)
{
	return height;
}

void screen_swap(bool blank)
{
	libretro_swap_buffer = !blank;
}
