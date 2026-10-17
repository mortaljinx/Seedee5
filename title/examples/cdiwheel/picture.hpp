/*
 * Seedee5 - the picture filters: the emulator's frame, filtered on the GPU into an image the size it is shown at.
 *
 *   Default    the frame as it is, scaled by the kit (no pass here)
 *   Sharp      crisp pixels without shimmer
 *   Smooth     bicubic
 *   Enhanced   for digital video: an edge-preserving clean-up of MPEG's blocks and ringing, contrast-adaptive
 *              sharpening (AMD's CAS / FSR's RCAS idea), both at the source size, then an anti-ringing bicubic upscale
 *   Scanlines  the picture's lines with dark gaps between them
 *   CRT        a TV: beams, colour bleed, phosphor mask, glow, a curved screen with rounded corners, in linear light
 *   Pixel art  xBR-style: the corners of texels cut along diagonal edges, sharp elsewhere
 *   FSR        for digital video: the clean-up, then an edge-adaptive upscale after AMD FSR 1 (EASU), then
 *              contrast-adaptive sharpening (as FSR 1's RCAS)
 *   FMV clean-up (light, medium, strong)
 *              for the digital video games: MPEG's 8x8 blocks and ringing removed at the video's own size by
 *              non-local means (each pixel averaged with the pixels around it whose neighbourhood looks the same),
 *              a light sharpening, then an upscale that is smooth on video and crisp on text and drawn graphics
 *              (whose edges go from one colour to the other in a single pixel; MPEG spreads video's edges). In tests on a frame compressed as a CD-i's
 *              MPEG is, this beat a small super-resolution network (the network kept the blocks).
 *
 * The output is made at the screen's own pixels (3840x2160 on the console: the kit draws in 1920x1080 units at
 * twice that), so the CRT's mask and the scanlines are drawn at the TV's real resolution. If images that large
 * cannot be made, the largest is 1920x1080 and the kit scales it, as before 1.10.
 *
 * One compute shader (picture.comp, its SPIR-V in picture_spv.h). Each new emulator frame is filtered once, into
 * the output image of the same slot (two, as the video images, for the smooth-motion blend); the kit then draws the
 * output 1:1. Synchronous, like the upload before it: a few tenths of a millisecond on the console's GPU.
 * Included by cdiwheel.cpp (after VideoSurface).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "picture_spv.h"

// (new ones go at the end: the saved choices are these numbers)
enum class Picture { normal, sharp, smooth, enhanced, scanlines, crt, pixelart, fsr, fmvLight, fmvMedium, fmvStrong, count };

static const char *pictureName(Picture p)
{
	switch (p) {
	case Picture::normal: return "Default";
	case Picture::sharp: return "Sharp";
	case Picture::smooth: return "Smooth";
	case Picture::enhanced: return "Enhanced (video)";
	case Picture::scanlines: return "Scanlines";
	case Picture::crt: return "CRT";
	case Picture::pixelart: return "Pixel art (xBR)";
	case Picture::fsr: return "FSR (video)";
	case Picture::fmvLight: return "FMV clean-up: light";
	case Picture::fmvMedium: return "FMV clean-up: medium";
	case Picture::fmvStrong: return "FMV clean-up: strong";
	default: return "?";
	}
}

class PictureFilter
{
public:
	static constexpr int kOutW = 3840, kOutH = 2160;   // the largest a picture is shown at: 16:9, full screen, 4K
	static constexpr int kSmallW = 1920, kSmallH = 1080; // if 4K images cannot be made
	int maxW{ kOutW }, maxH{ kOutH };                    // the output images' size, as made

	VkImageView outView[2]{};
	bool ready{ false };
	std::string why; // the step that failed, for the log

	struct Params {
		float rect[4];
		float texSize[2];
		float outSize[2];
		int32_t mode;
		int32_t lines;
		float strength;
		int32_t dbl;
	};

	bool create(VkPhysicalDevice physical, VkDevice device, VkQueue q, uint32_t family, const VkImageView srcViews[2], int srcW, int srcH)
	{
		dev = device;
		queue = q;
		srcSize[0] = srcW;
		srcSize[1] = srcH;
		vkGetPhysicalDeviceMemoryProperties(physical, &mem);
		// the formats must be writable from a shader
		VkFormatProperties fp;
		vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R8G8B8A8_UNORM, &fp);
		storageFeature = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0; // (logged; tried anyway)
		// the screen-sized images at 4K; at 1080p if those cannot be made
		why = "the output images";
		bool big = false;
		for (const int scale : { 2, 1 }) {
			maxW = kSmallW * scale;
			maxH = kSmallH * scale;
			if (makeImage(maxW, maxH, out[0], outMem[0], outView[0]) && makeImage(maxW, maxH, out[1], outMem[1], outView[1]) &&
				makeImage(maxW, maxH, tmpB, tmpBMem, tmpBView)) {
				big = true;
				break;
			}
			dropImage(out[0], outMem[0], outView[0]);
			dropImage(out[1], outMem[1], outView[1]);
			dropImage(tmpB, tmpBMem, tmpBView);
		}
		if (!big) {
			return false;
		}
		why = "the working images";
		if (!makeImage(srcW, srcH, tmpA, tmpAMem, tmpAView) || !makeImage(srcW, srcH, tmpC, tmpCMem, tmpCView)) {
			return false;
		}

		VkSamplerCreateInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		si.magFilter = VK_FILTER_LINEAR;
		si.minFilter = VK_FILTER_LINEAR;
		si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		si.maxLod = 0.0f;
		why = "the sampler";
		if (vkCreateSampler(dev, &si, nullptr, &sampler) != VK_SUCCESS) {
			return false;
		}

		VkDescriptorSetLayoutBinding b[2]{};
		b[0].binding = 0;
		b[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		b[0].descriptorCount = 1;
		b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		b[1].binding = 1;
		b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		b[1].descriptorCount = 1;
		b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		VkDescriptorSetLayoutCreateInfo dl{};
		dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		dl.bindingCount = 2;
		dl.pBindings = b;
		why = "the descriptor set layout";
		if (vkCreateDescriptorSetLayout(dev, &dl, nullptr, &setLayout) != VK_SUCCESS) {
			return false;
		}
		VkPushConstantRange pr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params) };
		VkPipelineLayoutCreateInfo pl{};
		pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pl.setLayoutCount = 1;
		pl.pSetLayouts = &setLayout;
		pl.pushConstantRangeCount = 1;
		pl.pPushConstantRanges = &pr;
		why = "the pipeline layout";
		if (vkCreatePipelineLayout(dev, &pl, nullptr, &pipeLayout) != VK_SUCCESS) {
			return false;
		}
		VkShaderModuleCreateInfo sm{};
		sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		sm.codeSize = sizeof(cdi_picture_spv);
		sm.pCode = cdi_picture_spv;
		VkShaderModule module;
		why = "the shader module";
		if (vkCreateShaderModule(dev, &sm, nullptr, &module) != VK_SUCCESS) {
			return false;
		}
		VkComputePipelineCreateInfo cp{};
		cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		cp.stage.module = module;
		cp.stage.pName = "main";
		cp.layout = pipeLayout;
		const VkResult pr2 = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cp, nullptr, &pipeline);
		vkDestroyShaderModule(dev, module, nullptr);
		if (pr2 != VK_SUCCESS) {
			why = "the compute pipeline (VkResult " + std::to_string(static_cast<int>(pr2)) + ")";
			return false;
		}

		// the descriptor sets: [0,1] video -> out, [2,3] video -> tmpA (clean-up), [4] tmpA -> tmpB (sharpen),
		// [5,6] tmpB -> out (upscale)
		VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSets } };
		VkDescriptorPoolCreateInfo dp{};
		dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		dp.maxSets = kSets;
		dp.poolSizeCount = 2;
		dp.pPoolSizes = ps;
		why = "the descriptor pool";
		if (vkCreateDescriptorPool(dev, &dp, nullptr, &descPool) != VK_SUCCESS) {
			return false;
		}
		VkDescriptorSetLayout layouts[kSets];
		for (auto &l : layouts) {
			l = setLayout;
		}
		VkDescriptorSetAllocateInfo da{};
		da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		da.descriptorPool = descPool;
		da.descriptorSetCount = kSets;
		da.pSetLayouts = layouts;
		why = "the descriptor sets";
		if (vkAllocateDescriptorSets(dev, &da, sets) != VK_SUCCESS) {
			return false;
		}
		const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, gen = VK_IMAGE_LAYOUT_GENERAL;
		// [0,1] video -> tmpC (prepare), [2,3] tmpC -> out (one-pass modes), [4] tmpC -> tmpA (clean-up),
		// [5] tmpA -> tmpB (sharpen / EASU), [6,7] tmpB -> out
		for (int i = 0; i < 2; i++) {
			bind(sets[i], srcViews[i], ro, tmpCView);
			bind(sets[2 + i], tmpCView, gen, outView[i]);
			bind(sets[6 + i], tmpBView, gen, outView[i]);
		}
		bind(sets[4], tmpCView, gen, tmpAView);
		bind(sets[5], tmpAView, gen, tmpBView);

		VkCommandPoolCreateInfo pci{};
		pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		pci.queueFamilyIndex = family;
		why = "the command pool";
		if (vkCreateCommandPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) {
			return false;
		}
		VkCommandBufferAllocateInfo cai{};
		cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cai.commandPool = pool;
		cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cai.commandBufferCount = 1;
		VkFenceCreateInfo fci{};
		fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		if (vkAllocateCommandBuffers(dev, &cai, &cmd) != VK_SUCCESS || vkCreateFence(dev, &fci, nullptr, &fence) != VK_SUCCESS) {
			return false;
		}

		// the layouts they live in: the outputs as the kit reads them, the working images GENERAL
		begin();
		for (int i = 0; i < 2; i++) {
			barrier(out[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
				VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		}
		barrier(tmpA, VK_IMAGE_LAYOUT_UNDEFINED, gen, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		barrier(tmpB, VK_IMAGE_LAYOUT_UNDEFINED, gen, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		barrier(tmpC, VK_IMAGE_LAYOUT_UNDEFINED, gen, 0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		why = "the first submit";
		ready = submit();
		if (ready) {
			why = storageFeature ? "" : "(the driver does not list RGBA8 storage images, but they were made)";
		}
		return ready;
	}

	// Filters video slot `slot` (the part x, y, w, h of it; the picture's scanlines `lines`) into out[slot], at
	// outW x outH. The video image is in SHADER_READ_ONLY_OPTIMAL, as after an upload.
	// dbl: bit 0, the frame's pixels are two columns wide; bit 1, two rows tall (filters work on the real pixels)
	bool run(int slot, Picture mode, const int rectIn[4], int dbl, int outW, int outH)
	{
		const int sx = (dbl & 1) ? 2 : 1, sy = (dbl & 2) ? 2 : 1;
		const int rect[4] = { 0, 0, rectIn[2] / sx, rectIn[3] / sy }; // where the prepared picture is, in tmpC
		const int lines = rect[3] > 400 ? rect[3] / 2 : rect[3];
		if (!ready || mode == Picture::normal || slot < 0 || slot > 1 || rect[2] < 4 || rect[3] < 4) {
			return false;
		}
		outW = std::clamp(outW, 16, maxW);
		outH = std::clamp(outH, 16, maxH);
		Params pc{};
		pc.rect[0] = static_cast<float>(rect[0]);
		pc.rect[1] = static_cast<float>(rect[1]);
		pc.rect[2] = static_cast<float>(rect[2]);
		pc.rect[3] = static_cast<float>(rect[3]);
		pc.texSize[0] = static_cast<float>(srcSize[0]);
		pc.texSize[1] = static_cast<float>(srcSize[1]);
		pc.outSize[0] = static_cast<float>(outW);
		pc.outSize[1] = static_cast<float>(outH);
		pc.lines = lines;
		pc.strength = 0.5f;
		const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, gen = VK_IMAGE_LAYOUT_GENERAL;
		const VkAccessFlags rw = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		const VkPipelineStageFlags cs = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

		begin();
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
		{ // the part shown, one texel per picture pixel, into tmpC
			Params pp = pc;
			pp.mode = 20;
			pp.dbl = dbl;
			for (int k = 0; k < 4; k++) {
				pp.rect[k] = static_cast<float>(rectIn[k]);
			}
			dispatch(sets[slot], pp, rect[2], rect[3]);
			barrier(tmpC, gen, gen, rw, rw, cs, cs);
		}
		// the output becomes writable
		barrier(out[slot], ro, gen, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, cs);
		if (mode == Picture::enhanced) {
			pc.mode = 10; // clean-up, at the source's size, into tmpA
			pc.strength = 0.5f;
			dispatch(sets[4], pc, rect[2], rect[3]);
			barrier(tmpA, gen, gen, rw, rw, cs, cs);
			pc.mode = 13; // sharpen, at the source's size, tmpA -> tmpB (same texel positions)
			pc.strength = 0.8f;
			dispatch(sets[5], pc, rect[2], rect[3]);
			barrier(tmpB, gen, gen, rw, rw, cs, cs);
			pc.mode = 12; // upscale, tmpB -> out (tmpB is maxW x maxH: its own texel size)
			pc.texSize[0] = static_cast<float>(maxW);
			pc.texSize[1] = static_cast<float>(maxH);
			dispatch(sets[6 + slot], pc, outW, outH);
		} else if (mode == Picture::fmvLight || mode == Picture::fmvMedium || mode == Picture::fmvStrong) {
			pc.mode = 15; // non-local means, at the source's size, tmpC -> tmpA
			pc.strength = mode == Picture::fmvLight ? 0.0f : mode == Picture::fmvMedium ? 0.5f : 1.0f;
			dispatch(sets[4], pc, rect[2], rect[3]);
			barrier(tmpA, gen, gen, rw, rw, cs, cs);
			pc.mode = 13; // a light sharpening at the source's size, tmpA -> tmpB
			pc.strength = 0.3f;
			dispatch(sets[5], pc, rect[2], rect[3]);
			barrier(tmpB, gen, gen, rw, rw, cs, cs);
			pc.mode = 16; // the upscale: smooth video, crisp text, tmpB -> out
			pc.texSize[0] = static_cast<float>(maxW);
			pc.texSize[1] = static_cast<float>(maxH);
			dispatch(sets[6 + slot], pc, outW, outH);
		} else if (mode == Picture::fsr) {
			pc.mode = 10; // clean-up, at the source's size, into tmpA
			pc.strength = 0.5f;
			dispatch(sets[4], pc, rect[2], rect[3]);
			barrier(tmpA, gen, gen, rw, rw, cs, cs);
			pc.mode = 14; // EASU, tmpA -> tmpB at the output's size
			dispatch(sets[5], pc, outW, outH);
			barrier(tmpB, gen, gen, rw, rw, cs, cs);
			pc.mode = 11; // RCAS-like sharpening at the output's size, tmpB -> out
			pc.strength = 0.5f;
			dispatch(sets[6 + slot], pc, outW, outH);
		} else {
			pc.mode = mode == Picture::sharp ? 1 : mode == Picture::smooth ? 2 : mode == Picture::scanlines ? 3 : mode == Picture::pixelart ? 5 : 4;
			dispatch(sets[2 + slot], pc, outW, outH);
		}
		barrier(out[slot], gen, ro, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, cs, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
		return submit();
	}

	// The caller has waited for the device to be idle, and has dropped the kit's handles for outView.
	void destroy()
	{
		if (!dev) {
			return;
		}
		if (fence) vkDestroyFence(dev, fence, nullptr);
		if (pool) vkDestroyCommandPool(dev, pool, nullptr);
		if (descPool) vkDestroyDescriptorPool(dev, descPool, nullptr);
		if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
		if (pipeLayout) vkDestroyPipelineLayout(dev, pipeLayout, nullptr);
		if (setLayout) vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
		if (sampler) vkDestroySampler(dev, sampler, nullptr);
		for (int i = 0; i < 2; i++) {
			dropImage(out[i], outMem[i], outView[i]);
		}
		dropImage(tmpA, tmpAMem, tmpAView);
		dropImage(tmpB, tmpBMem, tmpBView);
		dropImage(tmpC, tmpCMem, tmpCView);
		*this = PictureFilter();
	}

private:
	static constexpr uint32_t kSets = 8;
	bool storageFeature{ true };
	VkDevice dev{ VK_NULL_HANDLE };
	VkQueue queue{ VK_NULL_HANDLE };
	VkPhysicalDeviceMemoryProperties mem{};
	int srcSize[2]{};
	VkImage out[2]{}, tmpA{}, tmpB{}, tmpC{};
	VkDeviceMemory outMem[2]{}, tmpAMem{}, tmpBMem{}, tmpCMem{};
	VkImageView tmpAView{}, tmpBView{}, tmpCView{};
	VkSampler sampler{};
	VkDescriptorSetLayout setLayout{};
	VkPipelineLayout pipeLayout{};
	VkPipeline pipeline{};
	VkDescriptorPool descPool{};
	VkDescriptorSet sets[kSets]{};
	VkCommandPool pool{};
	VkCommandBuffer cmd{};
	VkFence fence{};

	void dropImage(VkImage &im, VkDeviceMemory &m, VkImageView &v)
	{
		if (v) vkDestroyImageView(dev, v, nullptr);
		if (im) vkDestroyImage(dev, im, nullptr);
		if (m) vkFreeMemory(dev, m, nullptr);
		v = VK_NULL_HANDLE;
		im = VK_NULL_HANDLE;
		m = VK_NULL_HANDLE;
	}

	bool makeImage(int w, int h, VkImage &image, VkDeviceMemory &memory, VkImageView &view)
	{
		VkImageCreateInfo ici{};
		ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = VK_FORMAT_R8G8B8A8_UNORM;
		ici.extent = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // (transfer: tests read them back)
		ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
		ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		if (vkCreateImage(dev, &ici, nullptr, &image) != VK_SUCCESS) {
			return false;
		}
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, image, &req);
		VkMemoryAllocateInfo alloc{};
		alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc.allocationSize = req.size;
		alloc.memoryTypeIndex = VideoSurface::findMemory(mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		if (alloc.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS ||
			vkBindImageMemory(dev, image, memory, 0) != VK_SUCCESS) {
			return false;
		}
		VkImageViewCreateInfo vci{};
		vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		vci.image = image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = ici.format;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		return vkCreateImageView(dev, &vci, nullptr, &view) == VK_SUCCESS;
	}

	void bind(VkDescriptorSet set, VkImageView from, VkImageLayout fromLayout, VkImageView to)
	{
		VkDescriptorImageInfo in{ sampler, from, fromLayout };
		VkDescriptorImageInfo outInfo{ VK_NULL_HANDLE, to, VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet w[2]{};
		w[0].sType = w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		w[0].dstSet = w[1].dstSet = set;
		w[0].dstBinding = 0;
		w[0].descriptorCount = 1;
		w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		w[0].pImageInfo = &in;
		w[1].dstBinding = 1;
		w[1].descriptorCount = 1;
		w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		w[1].pImageInfo = &outInfo;
		vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
	}

	void dispatch(VkDescriptorSet set, const Params &pc, int w, int h)
	{
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout, 0, 1, &set, 0, nullptr);
		vkCmdPushConstants(cmd, pipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
		vkCmdDispatch(cmd, static_cast<uint32_t>((w + 7) / 8), static_cast<uint32_t>((h + 7) / 8), 1);
	}

	void begin()
	{
		vkResetCommandBuffer(cmd, 0);
		VkCommandBufferBeginInfo bi{};
		bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		vkBeginCommandBuffer(cmd, &bi);
	}

	void barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
		VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
	{
		VkImageMemoryBarrier b{};
		b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		b.srcAccessMask = srcAccess;
		b.dstAccessMask = dstAccess;
		b.oldLayout = from;
		b.newLayout = to;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = image;
		b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
	}

	bool submit()
	{
		vkEndCommandBuffer(cmd);
		VkSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &cmd;
		if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
			return false;
		}
		vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
		vkResetFences(dev, 1, &fence);
		return true;
	}
};
