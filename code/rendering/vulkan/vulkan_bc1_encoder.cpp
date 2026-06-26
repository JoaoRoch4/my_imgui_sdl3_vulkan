#include "pch.hpp" // NOLINT
#include "vulkan_bc1_encoder.hpp"

#include "debug_log.hpp"
#include "image_ops.hpp" // bc1_size
#include "vulkan_context.hpp"

// Generated header — array of SPIR-V words produced from bc1_encode.comp.
// The CMake bc1_shader target writes this under build/<...>/generated/shaders/.
#include "bc1_encode_spv.hpp"


namespace {

uint32_t find_memory_type(VkPhysicalDevice physical_device, uint32_t type_filter, VkMemoryPropertyFlags properties) {
	VkPhysicalDeviceMemoryProperties mem_props;
	vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);
	for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
		bool const is_supported   = static_cast<bool>(type_filter & (1u << i));
		bool const has_properties = (mem_props.memoryTypes[i].propertyFlags & properties) == properties;
		if (is_supported && has_properties)
			return i;
	}
	return UINT32_MAX;
}

bool create_storage_buffer(vulkan_context &vk, VkDeviceSize bytes, VkMemoryPropertyFlags props,
	VkBuffer &out_buf, VkDeviceMemory &out_mem) {
	VkBufferCreateInfo info {};
	info.sType  = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size   = bytes;
	info.usage  = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
		| VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (vkCreateBuffer(vk.device, &info, vk.allocator, &out_buf) != VK_SUCCESS)
		return false;

	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(vk.device, out_buf, &req);
	VkMemoryAllocateInfo alloc {};
	alloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	alloc.allocationSize  = req.size;
	alloc.memoryTypeIndex = find_memory_type(vk.physical_device, req.memoryTypeBits, props);
	if (alloc.memoryTypeIndex == UINT32_MAX
		|| vkAllocateMemory(vk.device, &alloc, vk.allocator, &out_mem) != VK_SUCCESS) {
		vkDestroyBuffer(vk.device, out_buf, vk.allocator);
		out_buf = VK_NULL_HANDLE;
		return false;
	}
	vkBindBufferMemory(vk.device, out_buf, out_mem, 0);
	return true;
}

} // namespace

VulkanBc1Encoder::VulkanBc1Encoder()  = default;
VulkanBc1Encoder::~VulkanBc1Encoder() = default;

VulkanBc1Encoder &VulkanBc1Encoder::instance() {
	static VulkanBc1Encoder g;
	return g;
}

bool VulkanBc1Encoder::is_ready() const noexcept { return m_ready; }

std::size_t VulkanBc1Encoder::in_flight_count() const noexcept {
	std::lock_guard lock(m_queue_mutex);
	return m_in_flight.size() + m_pending.size();
}

bool VulkanBc1Encoder::setup(vulkan_context &vk) {
	if (vk.device == VK_NULL_HANDLE)
		return false;

	// 1. Shader module from the embedded SPIR-V.
	VkShaderModuleCreateInfo smci {};
	smci.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	smci.codeSize = sizeof(bc1_encode_spv);
	smci.pCode    = bc1_encode_spv;
	if (vkCreateShaderModule(vk.device, &smci, vk.allocator, &m_shader) != VK_SUCCESS) {
		APP_DEBUG_LOG("[bc1_gpu] vkCreateShaderModule failed");
		return false;
	}

	// 2. Descriptor set layout: binding 0 = input SSBO (RGBA), binding 1 = output SSBO (BC1).
	std::array<VkDescriptorSetLayoutBinding, 2> bindings {};
	bindings[0].binding         = 0;
	bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
	bindings[1].binding         = 1;
	bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[1].descriptorCount = 1;
	bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

	VkDescriptorSetLayoutCreateInfo dslci {};
	dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	dslci.bindingCount = static_cast<uint32_t>(bindings.size());
	dslci.pBindings    = bindings.data();
	if (vkCreateDescriptorSetLayout(vk.device, &dslci, vk.allocator, &m_desc_layout) != VK_SUCCESS) {
		APP_DEBUG_LOG("[bc1_gpu] vkCreateDescriptorSetLayout failed");
		shutdown(vk);
		return false;
	}

	// 3. Pipeline layout: descriptor + 16-byte push-constant (uvec4 dims).
	VkPushConstantRange pcr {};
	pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	pcr.offset     = 0;
	pcr.size       = 16; // uvec4
	VkPipelineLayoutCreateInfo plci {};
	plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	plci.setLayoutCount         = 1;
	plci.pSetLayouts            = &m_desc_layout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges    = &pcr;
	if (vkCreatePipelineLayout(vk.device, &plci, vk.allocator, &m_pipe_layout) != VK_SUCCESS) {
		APP_DEBUG_LOG("[bc1_gpu] vkCreatePipelineLayout failed");
		shutdown(vk);
		return false;
	}

	// 4. Compute pipeline.
	VkPipelineShaderStageCreateInfo stage {};
	stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = m_shader;
	stage.pName  = "main";
	VkComputePipelineCreateInfo cpci {};
	cpci.sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cpci.stage  = stage;
	cpci.layout = m_pipe_layout;
	if (vkCreateComputePipelines(vk.device, vk.pipeline_cache, 1, &cpci, vk.allocator, &m_pipeline) != VK_SUCCESS) {
		APP_DEBUG_LOG("[bc1_gpu] vkCreateComputePipelines failed");
		shutdown(vk);
		return false;
	}

	// 5. Persistent command pool + descriptor pool sized for the in-flight cap × per-batch worst case.
	VkCommandPoolCreateInfo cpcr {};
	cpcr.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	cpcr.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	cpcr.queueFamilyIndex = vk.queue_family;
	if (vkCreateCommandPool(vk.device, &cpcr, vk.allocator, &m_cmd_pool) != VK_SUCCESS) {
		APP_DEBUG_LOG("[bc1_gpu] vkCreateCommandPool failed");
		shutdown(vk);
		return false;
	}

	// Each slot consumes 2 SSBO descriptors; size the pool for the maximum we'd ever
	// have outstanding (in-flight batches * a generous slots-per-batch upper bound).
	constexpr uint32_t k_slots_per_batch_cap = 256; // batches over this just send fewer
	uint32_t const     max_sets              = static_cast<uint32_t>(k_max_in_flight_batches) * k_slots_per_batch_cap;
	VkDescriptorPoolSize dps {};
	dps.type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	dps.descriptorCount = max_sets * 2;
	VkDescriptorPoolCreateInfo dpci {};
	dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	dpci.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	dpci.maxSets       = max_sets;
	dpci.poolSizeCount = 1;
	dpci.pPoolSizes    = &dps;
	if (vkCreateDescriptorPool(vk.device, &dpci, vk.allocator, &m_desc_pool) != VK_SUCCESS) {
		APP_DEBUG_LOG("[bc1_gpu] vkCreateDescriptorPool failed");
		shutdown(vk);
		return false;
	}

	m_ready = true;
	APP_DEBUG_LOG("[bc1_gpu] encoder ready");
	return true;
}

void VulkanBc1Encoder::shutdown(vulkan_context &vk) {
	if (vk.device == VK_NULL_HANDLE) {
		m_ready = false;
		return;
	}
	// Wait for any in-flight batches before tearing down resources they reference.
	{
		std::lock_guard lock(m_queue_mutex);
		for (auto &batch : m_in_flight) {
			if (batch.fence != VK_NULL_HANDLE)
				vkWaitForFences(vk.device, 1, &batch.fence, VK_TRUE, UINT64_MAX);
			for (auto &slot : batch.slots) {
				destroy_slot(vk, *slot);
				fail_request(slot->request);
			}
			if (batch.fence != VK_NULL_HANDLE)
				vkDestroyFence(vk.device, batch.fence, vk.allocator);
			if (batch.cmd != VK_NULL_HANDLE)
				vkFreeCommandBuffers(vk.device, m_cmd_pool, 1, &batch.cmd);
		}
		m_in_flight.clear();
		for (auto &p : m_pending)
			fail_request(p);
		m_pending.clear();
	}

	if (m_desc_pool != VK_NULL_HANDLE) {
		vkDestroyDescriptorPool(vk.device, m_desc_pool, vk.allocator);
		m_desc_pool = VK_NULL_HANDLE;
	}
	if (m_cmd_pool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(vk.device, m_cmd_pool, vk.allocator);
		m_cmd_pool = VK_NULL_HANDLE;
	}
	if (m_pipeline != VK_NULL_HANDLE) {
		vkDestroyPipeline(vk.device, m_pipeline, vk.allocator);
		m_pipeline = VK_NULL_HANDLE;
	}
	if (m_pipe_layout != VK_NULL_HANDLE) {
		vkDestroyPipelineLayout(vk.device, m_pipe_layout, vk.allocator);
		m_pipe_layout = VK_NULL_HANDLE;
	}
	if (m_desc_layout != VK_NULL_HANDLE) {
		vkDestroyDescriptorSetLayout(vk.device, m_desc_layout, vk.allocator);
		m_desc_layout = VK_NULL_HANDLE;
	}
	if (m_shader != VK_NULL_HANDLE) {
		vkDestroyShaderModule(vk.device, m_shader, vk.allocator);
		m_shader = VK_NULL_HANDLE;
	}
	m_ready = false;
}

void VulkanBc1Encoder::fail_request(Pending &p) {
	try {
		p.promise.set_value(std::unexpected(img::ImageError::EncodeFailed));
	} catch (std::future_error const &) {
		// promise already satisfied — ignore.
	}
}

std::future<VulkanBc1Encoder::Result> VulkanBc1Encoder::submit(img::ImageBuffer src) {
	std::promise<Result> promise;
	auto                 fut = promise.get_future();

	if (!m_ready || !src.valid() || src.channels != 4) {
		promise.set_value(std::unexpected(img::ImageError::EncodeFailed));
		return fut;
	}

	std::lock_guard lock(m_queue_mutex);
	if (m_in_flight.size() >= k_max_in_flight_batches) {
		// Soft cap reached — fall back to CPU on this request.
		promise.set_value(std::unexpected(img::ImageError::EncodeFailed));
		return fut;
	}
	m_pending.push_back({std::move(src), std::move(promise)});
	return fut;
}

bool VulkanBc1Encoder::allocate_slot_buffers(vulkan_context &vk, Slot &s, VkDeviceSize in_bytes, VkDeviceSize out_bytes) {
	VkMemoryPropertyFlags const host_props
		= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (!create_storage_buffer(vk, in_bytes, host_props, s.in_buf, s.in_mem))
		return false;
	if (!create_storage_buffer(vk, out_bytes, host_props, s.out_buf, s.out_mem)) {
		vkDestroyBuffer(vk.device, s.in_buf, vk.allocator);
		vkFreeMemory(vk.device, s.in_mem, vk.allocator);
		s.in_buf = VK_NULL_HANDLE;
		s.in_mem = VK_NULL_HANDLE;
		return false;
	}
	s.in_capacity  = in_bytes;
	s.out_capacity = out_bytes;
	return true;
}

void VulkanBc1Encoder::destroy_slot(vulkan_context &vk, Slot &s) {
	if (s.desc_set != VK_NULL_HANDLE) {
		vkFreeDescriptorSets(vk.device, m_desc_pool, 1, &s.desc_set);
		s.desc_set = VK_NULL_HANDLE;
	}
	if (s.in_buf != VK_NULL_HANDLE) {
		vkDestroyBuffer(vk.device, s.in_buf, vk.allocator);
		s.in_buf = VK_NULL_HANDLE;
	}
	if (s.in_mem != VK_NULL_HANDLE) {
		vkFreeMemory(vk.device, s.in_mem, vk.allocator);
		s.in_mem = VK_NULL_HANDLE;
	}
	if (s.out_buf != VK_NULL_HANDLE) {
		vkDestroyBuffer(vk.device, s.out_buf, vk.allocator);
		s.out_buf = VK_NULL_HANDLE;
	}
	if (s.out_mem != VK_NULL_HANDLE) {
		vkFreeMemory(vk.device, s.out_mem, vk.allocator);
		s.out_mem = VK_NULL_HANDLE;
	}
}

void VulkanBc1Encoder::pump_once(vulkan_context &vk) {
	if (!m_ready)
		return;

	std::lock_guard lock(m_queue_mutex);

	// 1. Reap any in-flight batches whose fence has signalled.  Fulfil their
	//    promises by reading back the output buffer.
	while (!m_in_flight.empty()) {
		Batch &b      = m_in_flight.front();
		VkResult st   = vkGetFenceStatus(vk.device, b.fence);
		if (st != VK_SUCCESS)
			break;
		for (auto &slot_ptr : b.slots) {
			Slot &s   = *slot_ptr;
			void *p   = nullptr;
			Bytes out;
			if (vkMapMemory(vk.device, s.out_mem, 0, s.expected_bytes, 0, &p) == VK_SUCCESS) {
				out.resize(s.expected_bytes);
				std::memcpy(out.data(), p, s.expected_bytes);
				vkUnmapMemory(vk.device, s.out_mem);
				s.request.promise.set_value(std::move(out));
			} else {
				fail_request(s.request);
			}
			destroy_slot(vk, s);
		}
		vkDestroyFence(vk.device, b.fence, vk.allocator);
		vkFreeCommandBuffers(vk.device, m_cmd_pool, 1, &b.cmd);
		m_in_flight.pop_front();
	}

	// 2. If there's headroom, drain the pending queue into a new batch.
	if (m_pending.empty() || m_in_flight.size() >= k_max_in_flight_batches)
		return;

	Batch batch;
	// Allocate the cmdbuf.
	VkCommandBufferAllocateInfo cbai {};
	cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	cbai.commandPool        = m_cmd_pool;
	cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	cbai.commandBufferCount = 1;
	if (vkAllocateCommandBuffers(vk.device, &cbai, &batch.cmd) != VK_SUCCESS) {
		// Out of resources — fail all pending and bail.
		for (auto &p : m_pending)
			fail_request(p);
		m_pending.clear();
		return;
	}

	VkCommandBufferBeginInfo begin {};
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkBeginCommandBuffer(batch.cmd, &begin);
	vkCmdBindPipeline(batch.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);

	// Drain pending into slots.  Hard cap at 64 per batch (descriptor-pool sizing).
	constexpr std::size_t k_max_slots_per_batch = 256;
	while (!m_pending.empty() && batch.slots.size() < k_max_slots_per_batch) {
		auto slot = std::make_unique<Slot>();
		slot->request = std::move(m_pending.front());
		m_pending.pop_front();

		int const W  = slot->request.src.width;
		int const H  = slot->request.src.height;
		int const bx = (W + 3) / 4;
		int const by = (H + 3) / 4;
		slot->block_count    = static_cast<std::size_t>(bx) * static_cast<std::size_t>(by);
		slot->expected_bytes = img::ops::bc1_size(W, H);
		if (slot->block_count > k_max_blocks_per_image) {
			fail_request(slot->request);
			continue;
		}

		VkDeviceSize const in_bytes  = static_cast<VkDeviceSize>(W) * H * 4;
		VkDeviceSize const out_bytes = static_cast<VkDeviceSize>(slot->expected_bytes);
		if (!allocate_slot_buffers(vk, *slot, in_bytes, out_bytes)) {
			fail_request(slot->request);
			continue;
		}

		// Upload RGBA into the input host-visible buffer.
		void *map = nullptr;
		if (vkMapMemory(vk.device, slot->in_mem, 0, in_bytes, 0, &map) != VK_SUCCESS) {
			destroy_slot(vk, *slot);
			fail_request(slot->request);
			continue;
		}
		std::memcpy(map, slot->request.src.data.data(), static_cast<std::size_t>(in_bytes));
		vkUnmapMemory(vk.device, slot->in_mem);
		// Release the CPU buffer now — the GPU has its own copy.
		slot->request.src = img::ImageBuffer {};

		// Allocate a descriptor set and wire the two storage buffers.
		VkDescriptorSetAllocateInfo dsai {};
		dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		dsai.descriptorPool     = m_desc_pool;
		dsai.descriptorSetCount = 1;
		dsai.pSetLayouts        = &m_desc_layout;
		if (vkAllocateDescriptorSets(vk.device, &dsai, &slot->desc_set) != VK_SUCCESS) {
			destroy_slot(vk, *slot);
			fail_request(slot->request);
			continue;
		}
		std::array<VkDescriptorBufferInfo, 2> dbi {};
		dbi[0].buffer = slot->in_buf;
		dbi[0].offset = 0;
		dbi[0].range  = in_bytes;
		dbi[1].buffer = slot->out_buf;
		dbi[1].offset = 0;
		dbi[1].range  = out_bytes;
		std::array<VkWriteDescriptorSet, 2> writes {};
		writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[0].dstSet          = slot->desc_set;
		writes[0].dstBinding      = 0;
		writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[0].descriptorCount = 1;
		writes[0].pBufferInfo     = &dbi[0];
		writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[1].dstSet          = slot->desc_set;
		writes[1].dstBinding      = 1;
		writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[1].descriptorCount = 1;
		writes[1].pBufferInfo     = &dbi[1];
		vkUpdateDescriptorSets(vk.device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

		// Record the dispatch for this slot.
		vkCmdBindDescriptorSets(batch.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipe_layout, 0, 1, &slot->desc_set, 0,
			nullptr);
		uint32_t const dims[4] = {static_cast<uint32_t>(W), static_cast<uint32_t>(H), static_cast<uint32_t>(bx),
			static_cast<uint32_t>(by)};
		vkCmdPushConstants(batch.cmd, m_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, dims);
		vkCmdDispatch(batch.cmd, static_cast<uint32_t>(bx), static_cast<uint32_t>(by), 1);

		batch.slots.push_back(std::move(slot));
	}

	if (batch.slots.empty()) {
		vkFreeCommandBuffers(vk.device, m_cmd_pool, 1, &batch.cmd);
		return;
	}

	vkEndCommandBuffer(batch.cmd);

	VkFenceCreateInfo fci {};
	fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	vkCreateFence(vk.device, &fci, vk.allocator, &batch.fence);

	VkSubmitInfo si {};
	si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers    = &batch.cmd;
	if (vk.queue_submit(1, &si, batch.fence) != VK_SUCCESS) {
		for (auto &slot_ptr : batch.slots) {
			destroy_slot(vk, *slot_ptr);
			fail_request(slot_ptr->request);
		}
		vkDestroyFence(vk.device, batch.fence, vk.allocator);
		vkFreeCommandBuffers(vk.device, m_cmd_pool, 1, &batch.cmd);
		return;
	}
	m_in_flight.push_back(std::move(batch));
}
