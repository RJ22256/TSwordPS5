/*
 * TSword PS5 - milestone 1: the console check. It boots as TSword's PS5 title, draws (the template's lantern, so the
 * GPU path is seen working), and checks on this console what the Tears of the Kingdom recompilation's runtime
 * (valentderah/totk-pc-recomp) needs: its Vulkan 1.3 extensions and features - VK_EXT_external_memory_host above all,
 * as it hands the game's memory pools to the GPU in place -, the guest's address space as it lays it out (guest
 * address = host address, all below 2^39: a 6 GB stack and heap block and a 64 GB alias region), that memory imported
 * into Vulkan, write-protection faults (how it learns which pages the CPU wrote) and a signal to a running thread (how
 * it preempts one), the title folder, the CPU. Its report is /app0/tsword-check.txt (FTP:
 * /data/homebrew/PPSA99778/tsword-check.txt) and on screen.
 *
 * Made from the PS5 Vulkan Template starter.
 *
 * A textured, lit glTF model from the title folder, drawn with dynamic
 * rendering into the swapchain image and the base class's depth buffer. The
 * base class does the rest: the display, the frames, the camera on the pad's
 * sticks, and the settings window, moved through with the D-pad (ps5/README.md).
 * ps5/tools/new-title.py makes a new title from the samples' foundation with
 * this file as its program: replace the model, and grow the class.
 *
 * Copyright (C) 2026 Mihawk
 *
 * This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
 */

#include "vulkanexamplebase.h"
#include "VulkanglTFModel.h"

#include <chrono>
#include <cstdarg>
#include <sys/mman.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <csignal>
#include <pthread.h>
#include <setjmp.h>
#ifdef __PROSPERO__
#include <ps5platform/memory.h>
#include <ps5platform/shm.h>
#endif

#ifdef __PROSPERO__
#define TSWORD_ROOT "/app0"
#else
#define TSWORD_ROOT "."
#endif

// ---- the check: each line of the report, and a verdict per part for the screen
struct CheckLine { std::string text; int state; };  // state: 0 info, 1 ok, 2 missing / failed
static std::vector<CheckLine> g_check;
static FILE *g_report;
static void check_line(int state, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check_line(int state, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	g_check.push_back({ buf, state });
	if (g_report) {
		fprintf(g_report, "%s%s\n", state == 1 ? "[ok] " : state == 2 ? "[!!] " : "     ", buf);
		fflush(g_report);
	}
	fprintf(stderr, "[tsword] %s%s\n", state == 1 ? "ok: " : state == 2 ? "MISSING: " : "", buf);
}

static bool file_there(const std::string &p)
{
	struct stat st;
	return stat(p.c_str(), &st) == 0;
}

// What the TOTK runtime's Vulkan back end (runtime/vk_backend.c) uses: needed, or used when there
struct WantedExt { const char *name; bool needed; const char *what; };
static const WantedExt k_wanted[] = {
	{ "VK_EXT_external_memory_host", true, "the game's memory pools given to the GPU in place" },
	{ "VK_KHR_push_descriptor", true, "every draw's descriptors" },
	{ "VK_EXT_transform_feedback", true, "the game's transform feedback" },
	{ "VK_EXT_custom_border_color", true, "the game's samplers" },
	{ "VK_EXT_robustness2", true, "null descriptors" },
	{ "VK_EXT_extended_dynamic_state", false, "" },
	{ "VK_EXT_extended_dynamic_state2", false, "" },
	{ "VK_EXT_extended_dynamic_state3", false, "" },
	{ "VK_EXT_vertex_input_dynamic_state", false, "" },
	{ "VK_EXT_graphics_pipeline_library", false, "pipelines without stutter (fast link)" },
	{ "VK_KHR_pipeline_library", false, "" },
	{ "VK_EXT_conditional_rendering", false, "" },
	{ "VK_EXT_depth_clip_control", false, "OpenGL depth" },
	{ "VK_EXT_provoking_vertex", false, "" },
	{ "VK_EXT_fragment_shader_interlock", false, "" },
	{ "VK_KHR_dynamic_rendering_local_read", false, "framebuffer reads" },
	{ "VK_EXT_attachment_feedback_loop_layout", false, "framebuffer reads" },
	{ "VK_EXT_memory_budget", false, "" },
	{ "VK_EXT_calibrated_timestamps", false, "" },
	{ "VK_EXT_device_fault", false, "crash reports" },
	{ "VK_KHR_display", false, "the console's display" },
};

class VulkanExample : public VulkanExampleBase
{
public:
	vkglTF::Model model;

	// The settings window's choices
	bool spinModel{ true };
	bool moveLight{ false };
	bool wireframe{ false };
	float modelAngle{ 0.0f };
	float lightAngle{ 0.0f };

	struct UniformData {
		glm::mat4 projection;
		glm::mat4 view;
		glm::mat4 model;
		glm::vec4 lightDirection;
		glm::vec4 cameraPosition;
	} uniformData;
	std::array<vks::Buffer, maxConcurrentFrames> uniformBuffers;

	struct {
		VkPipeline solid{ VK_NULL_HANDLE };
		VkPipeline wireframe{ VK_NULL_HANDLE };
	} pipelines;
	VkPipelineLayout pipelineLayout{ VK_NULL_HANDLE };
	VkDescriptorSetLayout descriptorSetLayout{ VK_NULL_HANDLE };
	std::array<VkDescriptorSet, maxConcurrentFrames> descriptorSets{};

	VulkanExample() : VulkanExampleBase()
	{
		title = "TSword";
		resetCamera();
		// Vulkan 1.3's dynamic rendering: no render pass or framebuffer objects.
		// The base class's helpers (beginDynamicRendering) transition the depth
		// buffer's stencil too, so it asks for a depth and stencil format.
		apiVersion = VK_API_VERSION_1_3;
		useDynamicRendering = true;
		requiresStencil = true;
		deviceCreatepNextChain = &baseDynamicRenderingFeatures;
	}

	~VulkanExample()
	{
		if (device) {
			vkDestroyPipeline(device, pipelines.solid, nullptr);
			vkDestroyPipeline(device, pipelines.wireframe, nullptr);
			vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
			vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
			for (auto& buffer : uniformBuffers) {
				buffer.destroy();
			}
		}
	}

	void resetCamera()
	{
		camera.type = Camera::CameraType::lookat;
		camera.setPosition(glm::vec3(0.0f, 0.0f, -3.2f));
		camera.setRotation(glm::vec3(-12.0f, 30.0f, 0.0f));
		camera.setPerspective(50.0f, (float)width / (float)height, 0.05f, 64.0f);
	}

	bool hostMemExt{ false };
	void getEnabledFeatures() override
	{
		// (the import test below needs VK_EXT_external_memory_host on the device)
		uint32_t en = 0;
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &en, nullptr);
		std::vector<VkExtensionProperties> ex(en);
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &en, ex.data());
		for (auto &e : ex)
			if (!strcmp(e.extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) hostMemExt = true;
		if (hostMemExt) enabledDeviceExtensions.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
		if (deviceFeatures.samplerAnisotropy) {
			enabledFeatures.samplerAnisotropy = VK_TRUE;
		}
		// Wireframe needs non-solid fill modes; the setting is offered only with them
		if (deviceFeatures.fillModeNonSolid) {
			enabledFeatures.fillModeNonSolid = VK_TRUE;
		}
	}

	void loadAssets()
	{
		// Khronos's Lantern (CC0), 1.6 tall and centred (ps5/ASSETS.md): replace it with yours
		const uint32_t glTFLoadingFlags = vkglTF::FileLoadingFlags::PreTransformVertices | vkglTF::FileLoadingFlags::FlipY;
		model.loadFromFile(getAssetPath() + "models/ps5/lantern/lantern_starter.gltf", vulkanDevice, queue, glTFLoadingFlags);
	}

	void setupDescriptors()
	{
		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, maxConcurrentFrames),
		};
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, maxConcurrentFrames);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));
		// Set 0: the frame's uniforms. Set 1 is the model's base colour texture (vkglTF's layout)
		const std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0),
		};
		VkDescriptorSetLayoutCreateInfo descriptorLayout = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayout));
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayout, 1);
		for (size_t i = 0; i < uniformBuffers.size(); i++) {
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSets[i]));
			VkWriteDescriptorSet write = vks::initializers::writeDescriptorSet(descriptorSets[i], VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformBuffers[i].descriptor);
			vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
		}
	}

	void preparePipelines()
	{
		const std::array<VkDescriptorSetLayout, 2> setLayouts = { descriptorSetLayout, vkglTF::descriptorSetLayoutImage };
		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(setLayouts.data(), static_cast<uint32_t>(setLayouts.size()));
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

		VkPipelineInputAssemblyStateCreateInfo inputAssemblyState = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		// No culling: glTF models are often open, and the shader lights both sides
		VkPipelineRasterizationStateCreateInfo rasterizationState = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendState = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilState = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportState = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleState = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicState = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {
			loadShader(getShadersPath() + "tsword/model.vert.spv", VK_SHADER_STAGE_VERTEX_BIT),
			loadShader(getShadersPath() + "tsword/model.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT),
		};
		// With dynamic rendering the pipeline names its attachments' formats instead of a render pass
		VkPipelineRenderingCreateInfo renderingCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
			.colorAttachmentCount = 1,
			.pColorAttachmentFormats = &swapChain.colorFormat,
			.depthAttachmentFormat = depthFormat,
			.stencilAttachmentFormat = depthFormat,
		};
		VkGraphicsPipelineCreateInfo pipelineCI{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.pNext = &renderingCreateInfo,
			.stageCount = static_cast<uint32_t>(shaderStages.size()),
			.pStages = shaderStages.data(),
			.pVertexInputState = vkglTF::Vertex::getPipelineVertexInputState({ vkglTF::VertexComponent::Position, vkglTF::VertexComponent::Normal, vkglTF::VertexComponent::UV }),
			.pInputAssemblyState = &inputAssemblyState,
			.pViewportState = &viewportState,
			.pRasterizationState = &rasterizationState,
			.pMultisampleState = &multisampleState,
			.pDepthStencilState = &depthStencilState,
			.pColorBlendState = &colorBlendState,
			.pDynamicState = &dynamicState,
			.layout = pipelineLayout,
		};
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.solid));
		if (enabledFeatures.fillModeNonSolid) {
			rasterizationState.polygonMode = VK_POLYGON_MODE_LINE;
			VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.wireframe));
		}
	}

	void prepareUniformBuffers()
	{
		for (auto& buffer : uniformBuffers) {
			VK_CHECK_RESULT(vulkanDevice->createBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buffer, sizeof(UniformData), &uniformData));
			VK_CHECK_RESULT(buffer.map());
		}
	}

	void updateUniformBuffers()
	{
		uniformData.projection = camera.matrices.perspective;
		uniformData.view = camera.matrices.view;
		uniformData.model = glm::rotate(glm::mat4(1.0f), modelAngle, glm::vec3(0.0f, 1.0f, 0.0f));
		// The light comes from above, left and in front; the base class's world has Y down
		const glm::mat4 lightTurn = glm::rotate(glm::mat4(1.0f), lightAngle, glm::vec3(0.0f, 1.0f, 0.0f));
		uniformData.lightDirection = lightTurn * glm::vec4(glm::normalize(glm::vec3(-0.45f, 0.75f, -0.5f)), 0.0f);
		uniformData.cameraPosition = glm::inverse(camera.matrices.view)[3];
		memcpy(uniformBuffers[currentBuffer].mapped, &uniformData, sizeof(uniformData));
	}

	bool checked{ false };
	int checkOk{ 0 }, checkBad{ 0 };

	// The game's memory pools as Vulkan memory, in place (VK_EXT_external_memory_host): what the runtime's back end
	// does with every pool the game makes
	void importCheck(void *p, uint64_t size)
	{
		if (!hostMemExt) { check_line(2, "host memory import: VK_EXT_external_memory_host not offered"); return; }
		auto props_fn = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
		VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
		VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
		p2.pNext = &hp;
		vkGetPhysicalDeviceProperties2(physicalDevice, &p2);
		check_line(0, "host memory import alignment: %llu bytes", (unsigned long long)hp.minImportedHostPointerAlignment);
		VkMemoryHostPointerPropertiesEXT mp{ VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
		VkResult r = props_fn ? props_fn(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, p, &mp) : VK_ERROR_EXTENSION_NOT_PRESENT;
		if (r != VK_SUCCESS || !mp.memoryTypeBits) {
			check_line(2, "host memory import: the guest's memory isn't importable (%d, types 0x%x)", r, mp.memoryTypeBits);
			return;
		}
		uint32_t type = 0;
		while (!(mp.memoryTypeBits & (1u << type))) type++;
		VkImportMemoryHostPointerInfoEXT imp{ VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT };
		imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
		imp.pHostPointer = p;
		VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
		ai.pNext = &imp;
		ai.allocationSize = size;
		ai.memoryTypeIndex = type;
		VkDeviceMemory mem = VK_NULL_HANDLE;
		r = vkAllocateMemory(device, &ai, nullptr, &mem);
		check_line(r == VK_SUCCESS ? 1 : 2, "host memory import: %llu MB of the guest's memory imported as Vulkan memory (type %u, types 0x%x): %d",
		           (unsigned long long)(size >> 20), type, mp.memoryTypeBits, r);
		if (mem) vkFreeMemory(device, mem, nullptr);
	}

	// Which pages the CPU wrote (the runtime's write watch): a page made read-only, written, the fault caught, the page
	// made writable again and the write done
	static inline std::atomic<int> s_faults{ 0 };
	static inline void *s_wp_page{ nullptr };
	static void wp_handler(int, siginfo_t *si, void *)
	{
		uint8_t *a = (uint8_t *)si->si_addr, *pg = (uint8_t *)s_wp_page;
		if (pg && a >= pg && a < pg + 0x4000) {
			s_faults++;
			mprotect(pg, 0x4000, PROT_READ | PROT_WRITE);
			return;
		}
		signal(SIGSEGV, SIG_DFL);
	}
	void writeWatchCheck(void *page)
	{
		struct sigaction sa{}, old_segv{}, old_bus{};
		sa.sa_sigaction = wp_handler;
		sa.sa_flags = SA_SIGINFO;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGSEGV, &sa, &old_segv);
		sigaction(SIGBUS, &sa, &old_bus);
		s_wp_page = page;
		int pr = mprotect(page, 0x4000, PROT_READ);
		if (pr == 0) {
			volatile uint32_t *w = (volatile uint32_t *)page;
			w[3] = 0x12345678;
		}
		bool ok = pr == 0 && s_faults.load() >= 1 && ((volatile uint32_t *)page)[3] == 0x12345678;
		check_line(ok ? 1 : 2, "write watch: a read-only guest page written, the fault caught and the write done (mprotect %d, %d faults)",
		           pr, s_faults.load());
		s_wp_page = nullptr;
		sigaction(SIGSEGV, &old_segv, nullptr);
		sigaction(SIGBUS, &old_bus, nullptr);
	}

	// Preemption: a signal to a thread running guest code, handled on that thread (the runtime's asynchronous
	// preemption interrupts a thread there)
	static inline std::atomic<int> s_sig_hits{ 0 };
	static inline std::atomic<bool> s_spin{ true };
	static void sig_handler(int) { s_sig_hits++; }
	static void *spinner(void *)
	{
		while (s_spin.load(std::memory_order_relaxed)) {}
		return nullptr;
	}
	void signalCheck()
	{
		struct sigaction sa{}, old{};
		sa.sa_handler = sig_handler;
		sigemptyset(&sa.sa_mask);
		int sr = sigaction(SIGUSR1, &sa, &old);
		pthread_t t;
		s_spin = true;
		int cr = pthread_create(&t, nullptr, spinner, nullptr);
		int kr = -1;
		if (cr == 0) {
			usleep(20000);
			kr = pthread_kill(t, SIGUSR1);
			for (int i = 0; i < 100 && !s_sig_hits.load(); i++) usleep(1000);
			s_spin = false;
			pthread_join(t, nullptr);
		}
		check_line(s_sig_hits.load() ? 1 : 0, "a signal to a busy thread: %s (sigaction %d, pthread_kill %d)%s",
		           s_sig_hits.load() ? "handled on it" : "not handled", sr, kr,
		           s_sig_hits.load() ? "" : " - preemption falls back to the runtime's safe points");
		sigaction(SIGUSR1, &old, nullptr);
	}

	void runCheck()
	{
		std::string root = TSWORD_ROOT;
		g_report = fopen((root + "/tsword-check.txt").c_str(), "w");
		if (g_report) chmod((root + "/tsword-check.txt").c_str(), 0666);
		check_line(0, "TSword PS5 milestone 1 - the console check (PPSA99778)");
		// -- the GPU
		VkPhysicalDeviceProperties pp{};
		vkGetPhysicalDeviceProperties(physicalDevice, &pp);
		check_line(0, "GPU: %s, Vulkan %u.%u.%u, driver %u", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
		           VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), pp.driverVersion);
		check_line(pp.apiVersion >= VK_API_VERSION_1_3 ? 1 : 2, "Vulkan 1.3 (TSword needs it)");
		uint32_t n = 0;
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, nullptr);
		std::vector<VkExtensionProperties> ext(n);
		vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &n, ext.data());
		auto has = [&](const char *name) {
			for (auto &e : ext) if (!strcmp(e.extensionName, name)) return true;
			return false;
		};
		for (const WantedExt &w : k_wanted)
			check_line(has(w.name) ? 1 : (w.needed ? 2 : 0), "%s%s%s%s", w.name, has(w.name) ? "" : " - not offered",
			           *w.what ? ": " : "", w.what);
		// features TSword turns on
		VkPhysicalDeviceTransformFeedbackFeaturesEXT tf{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
		VkPhysicalDeviceCustomBorderColorFeaturesEXT cb{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
		cb.pNext = &tf;
		VkPhysicalDeviceVulkan13Features v13{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
		v13.pNext = &cb;
		VkPhysicalDeviceVulkan12Features v12{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
		v12.pNext = &v13;
		VkPhysicalDeviceFeatures2 f2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
		f2.pNext = &v12;
		vkGetPhysicalDeviceFeatures2(physicalDevice, &f2);
		const VkPhysicalDeviceFeatures &f = f2.features;
		struct { const char *name; VkBool32 v; bool needed; } feats[] = {
			{ "dynamicRendering", v13.dynamicRendering, true },
			{ "synchronization2", v13.synchronization2, true },
			{ "maintenance4", v13.maintenance4, false },
			{ "transformFeedback", tf.transformFeedback, true },
			{ "geometryStreams", tf.geometryStreams, false },
			{ "customBorderColors", cb.customBorderColors, true },
			{ "customBorderColorWithoutFormat", cb.customBorderColorWithoutFormat, false },
			{ "timelineSemaphore", v12.timelineSemaphore, true },
			{ "bufferDeviceAddress", v12.bufferDeviceAddress, false },
			{ "descriptorIndexing", v12.descriptorIndexing, false },
			{ "scalarBlockLayout", v12.scalarBlockLayout, false },
			{ "shaderFloat16", v12.shaderFloat16, false },
			{ "geometryShader", f.geometryShader, true },
			{ "tessellationShader", f.tessellationShader, true },
			{ "depthClamp", f.depthClamp, true },
			{ "depthBiasClamp", f.depthBiasClamp, false },
			{ "logicOp", f.logicOp, false },
			{ "dualSrcBlend", f.dualSrcBlend, false },
			{ "independentBlend", f.independentBlend, true },
			{ "multiViewport", f.multiViewport, false },
			{ "wideLines", f.wideLines, false },
			{ "samplerAnisotropy", f.samplerAnisotropy, false },
			{ "textureCompressionBC", f.textureCompressionBC, false },
			{ "textureCompressionASTC_LDR", f.textureCompressionASTC_LDR, false },
			{ "shaderInt64", f.shaderInt64, false },
			{ "shaderFloat64", f.shaderFloat64, false },
			{ "shaderStorageImageWriteWithoutFormat", f.shaderStorageImageWriteWithoutFormat, false },
			{ "fragmentStoresAndAtomics", f.fragmentStoresAndAtomics, true },
			{ "vertexPipelineStoresAndAtomics", f.vertexPipelineStoresAndAtomics, false },
		};
		for (auto &x : feats) check_line(x.v ? 1 : (x.needed ? 2 : 0), "feature %s%s", x.name, x.v ? "" : " - no");
		VkPhysicalDeviceMemoryProperties mp{};
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &mp);
		for (uint32_t i = 0; i < mp.memoryHeapCount; i++)
			check_line(0, "memory heap %u: %llu MB%s", i, (unsigned long long)(mp.memoryHeaps[i].size >> 20),
			           (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device-local)" : "");
		check_line(0, "all %u device extensions:", n);
		{
			std::string line;
			for (auto &e : ext) {
				if (line.size() > 200) { check_line(0, "  %s", line.c_str()); line.clear(); }
				line += e.extensionName;
				line += ' ';
			}
			if (!line.empty()) check_line(0, "  %s", line.c_str());
		}
		// -- the CPU
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);
		check_line(0, "CPU: %ld logical processors", cpus);
		__builtin_cpu_init();
		bool v3 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2");
		check_line(v3 ? 1 : 2, "AVX2, FMA, BMI2 (the game code's x86-64-v3)");
		// -- memory: the TOTK runtime's layout (its runtime/memory.c): guest address = host address, below 2^39. The
		// stack and heap block (6 GB) from 4 GB up, the alias region (64 GB, 4 GB aligned), each a reservation committed
		// page by page; then about 3.5 GB of the block backed and written, imported into Vulkan, write-protected
#ifdef __PROSPERO__
		struct ps5_memory_stats ms{};
		if (ps5_memory_query(&ms))
			check_line(0, "direct memory: %llu MB in all, %llu MB free (largest %llu MB), flexible %llu MB free",
			           (unsigned long long)(ms.total_bytes >> 20), (unsigned long long)(ms.free_bytes >> 20),
			           (unsigned long long)(ms.largest_bytes >> 20), (unsigned long long)(ms.flexible_bytes >> 20));
		const uint64_t GiB = 1ull << 30, top = 1ull << 39;
		auto reserve_low = [&](uint64_t size, uint64_t align, uint64_t from, uint64_t *at) {
			for (uint64_t a = (from + align - 1) / align * align; a + size <= top; a += align)
				if (ps5_vrange_reserve_at((void *)(uintptr_t)a, size) == 0) { *at = a; return true; }
			return false;
		};
		uint64_t blk = 0, alias = 0;
		bool rb = reserve_low(6 * GiB, GiB, 4 * GiB, &blk);
		check_line(rb ? 1 : 2, "the stack and heap block: 6 GB reserved below 2^39%s0x%llx", rb ? " at " : " - nowhere, last tried ",
		           (unsigned long long)blk);
		bool ra = reserve_low(64 * GiB, 4 * GiB, rb ? blk + 6 * GiB : 4 * GiB, &alias);
		check_line(ra ? 1 : 2, "the alias region: 64 GB reserved below 2^39%s0x%llx", ra ? " at " : " - nowhere ",
		           (unsigned long long)alias);
		if (ra) ps5_vrange_release((void *)(uintptr_t)alias, 64 * GiB);
		if (rb) {
			const uint64_t lo = blk;
			auto t0 = std::chrono::steady_clock::now();
			uint64_t done = 0, want = 3584ull << 20, step = 16ull << 20;
			int cr = 0;
			for (uint64_t a = lo; done < want; a += step, done += step) {
				cr = ps5_vrange_commit((void *)(uintptr_t)a, step, PROT_READ | PROT_WRITE);
				if (cr) break;
				volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)a;
				for (uint64_t o = 0; o < step; o += 0x10000) p[o] = (uint8_t)o;  // (each 64 KiB unit touched)
			}
			double ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			check_line(cr == 0 ? 1 : 2, "%llu MB of it backed and written (%.0f ms)%s", (unsigned long long)(done >> 20), ms_,
			           cr ? " - a commit failed" : "");
			if (done >= (64ull << 20)) {
				importCheck((void *)(uintptr_t)lo, 32ull << 20);
				writeWatchCheck((void *)(uintptr_t)(lo + (48ull << 20)));
			}
			ps5_vrange_release((void *)(uintptr_t)lo, 6 * GiB);
		}
#else
		check_line(0, "memory: not checked off the console");
#endif
		signalCheck();
		// -- the title folder: where the game files go (copied there by TSword's PS5 export on the PC, later)
		check_line(file_there(root + "/eboot.bin") || file_there(root + "/sce_sys") ? 1 : 2, "title folder %s", root.c_str());
		check_line(0, "game files %s: %s", (root + "/tsword/romfs").c_str(),
		           file_there(root + "/tsword/romfs") ? "there" : "not there yet (fine for milestone 1)");
		{
			std::string t = root + "/tsword-write-test.bin";
			FILE *w = fopen(t.c_str(), "wb");
			bool ok = false;
			if (w) {
				std::vector<char> buf(8 << 20, 'w');
				auto t0 = std::chrono::steady_clock::now();
				ok = fwrite(buf.data(), 1, buf.size(), w) == buf.size();
				fclose(w);
				double ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
				check_line(ok ? 1 : 2, "an 8 MB file written in the title folder (%.0f ms)", ms_);
				unlink(t.c_str());
			} else {
				check_line(2, "the title folder isn't writable");
			}
		}
		for (auto &c : g_check) c.state == 1 ? checkOk++ : c.state == 2 ? checkBad++ : 0;
		check_line(0, "done: %d ok, %d missing or failed", checkOk, checkBad);
		if (g_report) { fclose(g_report); g_report = nullptr; }
		checked = true;
	}

	void prepare() override
	{
		VulkanExampleBase::prepare();
		runCheck();
		loadAssets();
		prepareUniformBuffers();
		setupDescriptors();
		preparePipelines();
		prepared = true;
	}

	void buildCommandBuffer()
	{
		VkCommandBuffer cmdBuffer = drawCmdBuffers[currentBuffer];
		VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
		VK_CHECK_RESULT(vkBeginCommandBuffer(cmdBuffer, &cmdBufInfo));
		// Clears the swapchain image and the depth buffer, with the barriers they need
		beginDynamicRendering(cmdBuffer);
		VkViewport viewport = vks::initializers::viewport((float)width, (float)height, 0.0f, 1.0f);
		vkCmdSetViewport(cmdBuffer, 0, 1, &viewport);
		VkRect2D scissor = vks::initializers::rect2D(width, height, 0, 0);
		vkCmdSetScissor(cmdBuffer, 0, 1, &scissor);
		vkCmdBindDescriptorSets(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descriptorSets[currentBuffer], 0, nullptr);
		vkCmdBindPipeline(cmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, wireframe ? pipelines.wireframe : pipelines.solid);
		// Each material's base colour texture is bound as set 1
		model.draw(cmdBuffer, vkglTF::RenderFlags::BindImages, pipelineLayout);
		drawUI(cmdBuffer);
		endDynamicRendering(cmdBuffer);
		VK_CHECK_RESULT(vkEndCommandBuffer(cmdBuffer));
	}

	void render() override
	{
		if (!prepared) {
			return;
		}
		VulkanExampleBase::prepareFrame();
		if (spinModel) {
			modelAngle += 0.4f * frameTimer;
		}
		if (moveLight) {
			lightAngle += 0.8f * frameTimer;
		}
		updateUniformBuffers();
		buildCommandBuffer();
		VulkanExampleBase::submitFrame();
	}

	void OnUpdateUIOverlay(vks::UIOverlay *overlay) override
	{
		if (checked && overlay->header("TSword PS5 - console check")) {
			overlay->text("%d ok, %d missing or failed", checkOk, checkBad);
			overlay->text("Report: /data/homebrew/PPSA99778/tsword-check.txt");
			for (auto &c : g_check)
				if (c.state == 2) overlay->text("MISSING: %s", c.text.c_str());
		}
		if (overlay->header("Settings")) {
			overlay->checkBox("Spin the model", &spinModel);
			overlay->checkBox("Move the light", &moveLight);
			if (enabledFeatures.fillModeNonSolid) {
				overlay->checkBox("Wireframe", &wireframe);
			}
			if (overlay->button("Reset the camera")) {
				resetCamera();
			}
			if (overlay->button("Quit")) {
				quit = true;
			}
		}
	}
};

VULKAN_EXAMPLE_MAIN()
