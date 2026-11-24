// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "android/jni/DisplayXR.h"
#include "android/jni/DisplayXRPolicy.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>
#include <map>
#include <jni.h>
#include "Common/Log.h"
#include "Common/GPU/thin3d.h"
#include "Common/System/NativeApp.h"
#include "Common/System/System.h"
#include "Common/System/OSD.h"
#include "Core/Config.h"
#include "Core/ConfigValues.h"
#include "Core/System.h"

namespace {
std::atomic<DisplayXRStatus> status{DisplayXRStatus::UNSUPPORTED};
std::atomic<bool> eligible{false}, foreground{false}, camera{false}, active{false};
std::atomic<uint64_t> generation{0};
std::atomic<bool> gameplay{false}, renderCapable{false}, retryRequested{false};
bool HardwareReady(Draw::DrawContext *draw);
std::mutex environmentMutex;
std::string reason;
JavaVM *vm = nullptr;
jweak owner = nullptr;
jweak activity = nullptr;
struct Geometry { int x = 0, y = 0, width = 0, height = 0, display = 0; } geometry;

class Env {
public:
	Env() {
		if (vm && vm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
			attached = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
		}
	}
	~Env() { if (attached) { vm->DetachCurrentThread(); } }
	JNIEnv *env = nullptr;
	bool attached = false;
};

void JavaCall(const char *method) {
	Env e;
	if (!e.env) { return; }
	jobject ref;
	{
		std::lock_guard<std::mutex> lock(environmentMutex);
		ref = e.env->NewLocalRef(owner);
	}
	if (ref) {
		jclass cls = e.env->GetObjectClass(ref);
		jmethodID id = e.env->GetMethodID(cls, method, "()V");
		if (id) { e.env->CallVoidMethod(ref, id); }
		if (e.env->ExceptionCheck()) { e.env->ExceptionClear(); }
		e.env->DeleteLocalRef(cls);
		e.env->DeleteLocalRef(ref);
	}
}
void SetStatus(DisplayXRStatus value, const char *message) {
	{
		std::lock_guard<std::mutex> lock(environmentMutex);
		reason = message;
	}
	status = value;
}
}

DisplayXRStatus DisplayXR_GetStatus() { return status.load(); }
std::string DisplayXR_GetReason() {
	if (status == DisplayXRStatus::AVAILABLE && !DisplayXR_Available()) {
		return "DisplayXR requires buffered hardware rendering with Vulkan and external-memory support.";
	}
	if (DisplayXR_Selected() && DisplayXR_Available() && !camera) {
		return "DisplayXR needs camera permission for eye tracking. Check Android app permissions, then select DisplayXR again.";
	}
	std::lock_guard<std::mutex> lock(environmentMutex);
	return reason;
}
bool DisplayXR_Selected() { return g_Config.iStereoscopyMode == (int)StereoscopyMode::DISPLAY_XR; }
bool DisplayXR_Available() {
	return status == DisplayXRStatus::AVAILABLE && renderCapable && g_Config.iGPUBackend == (int)GPUBackend::VULKAN &&
		!g_Config.bSoftwareRendering && !g_Config.bSkipBufferEffects;
}
bool DisplayXR_Active() { return active.load(); }
std::vector<int> DisplayXR_ModeChoices() {
	return DisplayXRPolicy::ModeChoices(status == DisplayXRStatus::AVAILABLE, g_Config.iStereoscopyMode);
}
void DisplayXR_ModeChanged() {
	NativeResized();
	JavaCall("resizeSurface");
}
void DisplayXR_RequestRetry() {
	retryRequested = true;
	JavaCall("retry");
}
void DisplayXR_Update(Draw::DrawContext *draw) {
	static bool lastActive = false;
	static bool permissionRequested = false;
	static bool failureShown = false;
	static bool defaultAttempted = false;
	static bool lastReady = false;
	if (draw) { renderCapable = HardwareReady(draw); }
	if (retryRequested.exchange(false)) { permissionRequested = false; }
	if (DisplayXR_Available() && !g_Config.bDisplayXRDefaultApplied && !defaultAttempted) {
		defaultAttempted = true;
		if (g_Config.ApplyDisplayXRDefault()) { DisplayXR_ModeChanged(); }
	}
	bool wants3D = DisplayXR_Selected() && DisplayXR_Available() && gameplay && PSP_IsInited() &&
		!g_Config.bSoftwareRendering && !g_Config.bSkipBufferEffects && foreground;
	if (wants3D && !camera && !permissionRequested) {
		permissionRequested = true;
		JavaCall("requestCamera");
	}
	active = wants3D && camera;
	const bool ready = draw ? DisplayXR_CanRender(draw) : lastReady;
	if (active != lastActive || ready != lastReady) {
		lastActive = active;
		lastReady = ready;
		System_PostUIMessage(UIMessage::GPU_CONFIG_CHANGED);
		DisplayXR_ModeChanged();
	}
	if (DisplayXR_Selected() && status == DisplayXRStatus::FAILED && !failureShown) {
		failureShown = true;
		g_OSD.Show(OSDType::MESSAGE_ERROR, "DisplayXR stopped working. Continuing in 2D; check DisplayXR setup.", 6.0);
	} else if (status != DisplayXRStatus::FAILED) {
		failureShown = false;
	}
	if (!DisplayXR_Selected()) {
		permissionRequested = false;
	}
}

void DisplayXR_Gameplay(bool running) {
	if (gameplay.exchange(running) != running) { DisplayXR_Update(); }
}

extern "C" JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_DisplayXRSupport_nativeEnvironment(
	JNIEnv *env, jclass, jobject newOwner, jobject newActivity, jboolean supported, jboolean installed, jboolean resumed, jboolean permission) {
	if (!vm) { env->GetJavaVM(&vm); }
	{
		std::lock_guard<std::mutex> lock(environmentMutex);
		if (owner) { env->DeleteWeakGlobalRef(owner); }
		if (activity) { env->DeleteWeakGlobalRef(activity); }
		owner = env->NewWeakGlobalRef(newOwner);
		activity = env->NewWeakGlobalRef(newActivity);
		generation++;
	}
	eligible = supported;
	foreground = resumed;
	camera = permission;
	if (!supported) {
		SetStatus(DisplayXRStatus::UNSUPPORTED, "");
	} else if (!installed) {
		SetStatus(DisplayXRStatus::MISSING, "Install DisplayXR to use glasses-free 3D on this device.");
	} else if (status != DisplayXRStatus::AVAILABLE) {
		SetStatus(DisplayXRStatus::CHECKING, "Checking DisplayXR. Vulkan is required.");
	}
	if (!resumed) { active = false; }
}
extern "C" JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_DisplayXRSupport_nativeGeometry(
	JNIEnv *, jclass, jint x, jint y, jint width, jint height, jint display) {
	std::lock_guard<std::mutex> lock(environmentMutex);
	geometry = {x, y, width, height, display};
}

#if defined(__aarch64__)
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include "Common/GPU/Vulkan/VulkanContext.h"
#include "Common/GPU/Vulkan/VulkanFramebuffer.h"
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_NO_PROTOTYPES
#include "ext/OpenXR-SDK/include/openxr/openxr.h"
#include "ext/OpenXR-SDK/include/openxr/openxr_platform.h"
#include "android/jni/displayxr/XR_DXR_weave.h"
#include "android/jni/displayxr/XR_DXR_display_info.h"

using namespace PPSSPP_VK;
namespace {
// The loader keeps the raw Context handle it's initialized with and uses it again on later calls
// (runtime_interface.cpp asks the runtime broker through it), so it gets a global ref that's never deleted.
// It also loads the runtime into our process, so it's never unloaded either.
struct Loader {
	void *library = nullptr;
	PFN_xrGetInstanceProcAddr get = nullptr;
	jobject context = nullptr;

	bool Init(JNIEnv *env, jobject app) {
		if (get) { return true; }
		if (!library) { library = dlopen("libopenxr_loader.so", RTLD_NOW | RTLD_LOCAL); }
		if (!library) { return false; }
		auto proc = (PFN_xrGetInstanceProcAddr)dlsym(library, "xrGetInstanceProcAddr");
		PFN_xrInitializeLoaderKHR initialize = nullptr;
		if (!proc || XR_FAILED(proc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initialize)) || !initialize) { return false; }
		if (!context) {
			jclass cls = env->GetObjectClass(app);
			jmethodID getContext = env->GetMethodID(cls, "getApplicationContext", "()Landroid/content/Context;");
			jobject local = getContext ? env->CallObjectMethod(app, getContext) : nullptr;
			env->DeleteLocalRef(cls);
			if (env->ExceptionCheck()) { env->ExceptionClear(); }
			if (local) {
				context = env->NewGlobalRef(local);
				env->DeleteLocalRef(local);
			}
		}
		if (!context) { return false; }
		XrLoaderInitInfoAndroidKHR init{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
		init.applicationVM = vm;
		init.applicationContext = context;
		if (XR_FAILED(initialize((XrLoaderInitInfoBaseHeaderKHR *)&init))) { return false; }
		get = proc;
		return true;
	}
} loader;  // Guarded by xrMutex, like every XR.

struct XR {
	PFN_xrGetInstanceProcAddr get = nullptr;
	PFN_xrCreateInstance create = nullptr;
	PFN_xrDestroyInstance destroy = nullptr;
	PFN_xrEnumerateInstanceExtensionProperties enumerate = nullptr;
	PFN_xrGetSystem getSystem = nullptr;
	PFN_xrCreateSession createSession = nullptr;
	PFN_xrDestroySession destroySession = nullptr;
	PFN_xrWeaveBindWindow2DXR bind = nullptr;
	PFN_xrWeaveSubmitDXR weave = nullptr;
	PFN_xrRequestDisplayModeDXR mode = nullptr;
	XrInstance instance = XR_NULL_HANDLE;
	XrSystemId system = XR_NULL_SYSTEM_ID;
	XrSession session = XR_NULL_HANDLE;
	jobject host = nullptr;

	template<typename T> bool Load(const char *name, T &fn, XrInstance inst) {
		return XR_SUCCEEDED(get(inst, name, (PFN_xrVoidFunction *)&fn)) && fn;
	}
	bool Init(jobject app, bool probe = false) {
		Env e;
		if (!e.env || !app) { return false; }
		host = e.env->NewGlobalRef(app);
		if (!loader.Init(e.env, app)) { return false; }
		get = loader.get;
		if (!Load("xrCreateInstance", create, XR_NULL_HANDLE) ||
			!Load("xrEnumerateInstanceExtensionProperties", enumerate, XR_NULL_HANDLE)) { return false; }
		uint32_t count = 0;
		if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr))) { return false; }
		std::vector<XrExtensionProperties> extensions(count, {XR_TYPE_EXTENSION_PROPERTIES});
		if (XR_FAILED(enumerate(nullptr, count, &count, extensions.data()))) { return false; }
		auto has = [&](const char *name, uint32_t version) {
			for (const auto &ext : extensions) {
				if (!strcmp(ext.extensionName, name) && ext.extensionVersion >= version) { return true; }
			}
			return false;
		};
		if (!has(XR_DXR_WEAVE_EXTENSION_NAME, 20) || !has(XR_DXR_DISPLAY_INFO_EXTENSION_NAME, 22) ||
			!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, 1)) { return false; }
		if (probe) { return true; }
		const char *names[] = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME,
			XR_DXR_WEAVE_EXTENSION_NAME, XR_DXR_DISPLAY_INFO_EXTENSION_NAME};
		XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
		android.applicationVM = vm;
		android.applicationActivity = host;
		XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
		ci.next = &android;
		strcpy(ci.applicationInfo.applicationName, "PPSSPP");
		ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 1, 0);
		ci.enabledExtensionCount = 4;
		ci.enabledExtensionNames = names;
		if (XR_FAILED(create(&ci, &instance))) { return false; }
		if (!Load("xrDestroyInstance", destroy, instance) || !Load("xrGetSystem", getSystem, instance) ||
			!Load("xrCreateSession", createSession, instance) || !Load("xrDestroySession", destroySession, instance) ||
			!Load("xrWeaveBindWindow2DXR", bind, instance) || !Load("xrWeaveSubmitDXR", weave, instance)) { return false; }
		Load("xrRequestDisplayModeDXR", mode, instance);
		XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
		info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		if (XR_FAILED(getSystem(instance, &info, &system))) {
			info.formFactor = XR_FORM_FACTOR_HANDHELD_DISPLAY;
			if (XR_FAILED(getSystem(instance, &info, &system))) { return false; }
		}
		PFN_xrEnumerateDisplaysDXR displays = nullptr;
		if (!Load("xrEnumerateDisplaysDXR", displays, instance)) { return false; }
		count = 0;
		if (XR_FAILED(displays(instance, system, 0, &count, nullptr))) { return false; }
		std::vector<XrDisplayDXR> panels(count, {XR_TYPE_DISPLAY_DXR});
		if (XR_FAILED(displays(instance, system, count, &count, panels.data()))) { return false; }
		for (const auto &panel : panels) {
			if (panel.vendorPluginId[0] && strcmp(panel.vendorPluginId, "sim-display") && strcmp(panel.vendorPluginId, "sim_display")) {
				return true;
			}
		}
		return false;
	}
	bool Session(VulkanContext *vk) {
		PFN_xrGetVulkanGraphicsRequirements2KHR requirements = nullptr;
		if (!Load("xrGetVulkanGraphicsRequirements2KHR", requirements, instance)) { return false; }
		XrGraphicsRequirementsVulkanKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
		if (XR_FAILED(requirements(instance, system, &req))) { return false; }
		PFN_xrGetVulkanGraphicsDevice2KHR graphicsDevice = nullptr;
		if (!Load("xrGetVulkanGraphicsDevice2KHR", graphicsDevice, instance)) { return false; }
		XrVulkanGraphicsDeviceGetInfoKHR deviceInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
		deviceInfo.systemId = system;
		deviceInfo.vulkanInstance = vk->GetInstance();
		VkPhysicalDevice physical = VK_NULL_HANDLE;
		if (XR_FAILED(graphicsDevice(instance, &deviceInfo, &physical)) || physical != vk->GetCurrentPhysicalDevice()) { return false; }
		XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
		binding.instance = vk->GetInstance();
		binding.physicalDevice = vk->GetCurrentPhysicalDevice();
		binding.device = vk->GetDevice();
		binding.queueFamilyIndex = vk->GetGraphicsQueueFamilyIndex();
		XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
		ci.next = &binding;
		ci.systemId = system;
		return XR_SUCCEEDED(createSession(instance, &ci, &session));
	}
	~XR() {
		if (session) {
			if (mode) { mode(session, XR_DISPLAY_MODE_2D_DXR); }
			if (destroySession) { destroySession(session); }
		}
		if (instance && destroy) { destroy(instance); }
		Env e;
		if (host && e.env) { e.env->DeleteGlobalRef(host); }
	}
};
std::mutex xrMutex;
jobject Activity(JNIEnv *env) {
	std::lock_guard<std::mutex> lock(environmentMutex);
	return env->NewLocalRef(activity);
}
}

extern "C" JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_DisplayXRSupport_nativeProbe(JNIEnv *env, jclass, jobject app, jboolean vendorRuntime) {
	const uint64_t version = generation;
	bool ready = false;
	{
		std::lock_guard<std::mutex> lock(xrMutex);
		XR xr;
		ready = vendorRuntime && xr.Init(app, true);
	}
	std::lock_guard<std::mutex> lock(environmentMutex);
	if (version == generation && foreground && eligible) {
		reason = ready ? "DisplayXR is available. Vulkan is required." : "DisplayXR needs setup or an update. Open the installation guide.";
		status = ready ? DisplayXRStatus::AVAILABLE : DisplayXRStatus::NEEDS_SETUP;
	}
}

#include "Common/GPU/Vulkan/VulkanRenderManager.h"
namespace {
struct AndroidBuffers {
	using Allocate = int (*)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
	using Release = void (*)(AHardwareBuffer *);
	using Describe = void (*)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
	Allocate allocate = nullptr;
	Release release = nullptr;
	Describe describe = nullptr;
	AndroidBuffers() {
		void *lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
		if (lib) {
			allocate = (Allocate)dlsym(lib, "AHardwareBuffer_allocate");
			release = (Release)dlsym(lib, "AHardwareBuffer_release");
			describe = (Describe)dlsym(lib, "AHardwareBuffer_describe");
			dlclose(lib);
		}
	}
} buffers;

struct SharedImage {
	VulkanContext *vk;
	AHardwareBuffer *buffer = nullptr;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	int width = 0, height = 0;
	bool valid = false;
	explicit SharedImage(VulkanContext *vk) : vk(vk) {}
	~SharedImage() {
		if (image) { vkDestroyImage(vk->GetDevice(), image, nullptr); }
		if (memory) { vkFreeMemory(vk->GetDevice(), memory, nullptr); }
		if (buffer && buffers.release) { buffers.release(buffer); }
	}
	bool Import(AHardwareBuffer *ahb) {
		buffer = ahb;  // Takes the caller-owned AHardwareBuffer reference, including on failure.
		if (!buffers.describe || !buffers.release) { return false; }
		AHardwareBuffer_Desc desc{};
		buffers.describe(buffer, &desc);
		width = desc.width;
		height = desc.height;
		auto props = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(vk->GetDevice(), "vkGetAndroidHardwareBufferPropertiesANDROID");
		if (!props) { return false; }
		VkAndroidHardwareBufferFormatPropertiesANDROID format{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
		VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
		properties.pNext = &format;
		if (props(vk->GetDevice(), buffer, &properties) != VK_SUCCESS || format.format == VK_FORMAT_UNDEFINED) { return false; }
		VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.pNext = &external;
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = format.format;
		info.extent = {desc.width, desc.height, 1};
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		if (vkCreateImage(vk->GetDevice(), &info, nullptr, &image) != VK_SUCCESS) { return false; }
		VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dedicated.image = image;
		VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
		import.buffer = buffer;
		import.pNext = &dedicated;
		VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		alloc.pNext = &import;
		alloc.allocationSize = properties.allocationSize;
		if (!vk->MemoryTypeFromProperties(properties.memoryTypeBits, 0, &alloc.memoryTypeIndex) ||
			vkAllocateMemory(vk->GetDevice(), &alloc, nullptr, &memory) != VK_SUCCESS ||
			vkBindImageMemory(vk->GetDevice(), image, memory, 0) != VK_SUCCESS) { return false; }
		valid = true;
		return true;
	}
	bool Create(int w, int h) {
		if (!buffers.allocate) { return false; }
		AHardwareBuffer_Desc desc{};
		desc.width = w;
		desc.height = h;
		desc.layers = 1;
		desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
		desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
		AHardwareBuffer *ahb = nullptr;
		return buffers.allocate(&desc, &ahb) == 0 && Import(ahb);
	}
};

void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout before, VkImageLayout after,
	uint32_t from = VK_QUEUE_FAMILY_IGNORED, uint32_t to = VK_QUEUE_FAMILY_IGNORED) {
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.oldLayout = before;
	barrier.newLayout = after;
	barrier.srcAccessMask = from == VK_QUEUE_FAMILY_FOREIGN_EXT ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = to == VK_QUEUE_FAMILY_FOREIGN_EXT ? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	if (before == VK_IMAGE_LAYOUT_UNDEFINED) { barrier.srcAccessMask = 0; }
	barrier.srcQueueFamilyIndex = from;
	barrier.dstQueueFamilyIndex = to;
	barrier.image = image;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
		0, nullptr, 0, nullptr, 1, &barrier);
}
void Blit(VkCommandBuffer cmd, VkImage src, int sw, int sh, VkImage dst, int dw, int dh) {
	VkImageBlit blit{};
	blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	blit.dstSubresource = blit.srcSubresource;
	blit.srcOffsets[1] = {sw, sh, 1};
	blit.dstOffsets[1] = {dw, dh, 1};
	vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
}

struct Capture {
	Draw::Framebuffer *atlas = nullptr;
	VKRFramebuffer *framebuffer = nullptr;
	~Capture() { if (atlas) { atlas->Release(); } }
};
class Renderer {
public:
	explicit Renderer(VulkanContext *vk) : vk(vk) {}
	~Renderer() {
		Stop();
		for (auto &slot : slots) {
			if (slot.fence) { vkDestroyFence(vk->GetDevice(), slot.fence, nullptr); }
			if (slot.pool) { vkDestroyCommandPool(vk->GetDevice(), slot.pool, nullptr); }
			if (slot.ready) { vkDestroySemaphore(vk->GetDevice(), slot.ready, nullptr); }
			if (slot.done) { vkDestroySemaphore(vk->GetDevice(), slot.done, nullptr); }
		}
	}
	struct Slot {
		std::shared_ptr<Capture> capture;
		std::shared_ptr<SharedImage> input, overlay, output;
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer cmd[2]{};
		VkFence fence = VK_NULL_HANDLE;
		VkSemaphore ready = VK_NULL_HANDLE, done = VK_NULL_HANDLE;
		bool submitted = false;
	} slots[VulkanContext::MAX_INFLIGHT_FRAMES];
	VulkanContext *vk;
	std::unique_ptr<XR> xr;
	uint64_t sessionGeneration = 0;
	std::shared_ptr<SharedImage> output;

	void Stop() {
		for (auto &slot : slots) {
			if (slot.submitted) {
				vkWaitForFences(vk->GetDevice(), 1, &slot.fence, VK_TRUE, UINT64_MAX);
				slot.submitted = false;
			}
			slot.input.reset();
			slot.overlay.reset();
			slot.output.reset();
			if (slot.ready) { vkDestroySemaphore(vk->GetDevice(), slot.ready, nullptr); }
			if (slot.done) { vkDestroySemaphore(vk->GetDevice(), slot.done, nullptr); }
			slot.ready = slot.done = VK_NULL_HANDLE;
		}
		if (xr) {
			std::lock_guard<std::mutex> lock(xrMutex);
			xr.reset();
		}
		output.reset();
	}
	bool InitSlot(Slot &slot, int width, int height) {
		if (slot.submitted) {
			if (vkWaitForFences(vk->GetDevice(), 1, &slot.fence, VK_TRUE, 1000000000ULL) != VK_SUCCESS) { return false; }
			slot.submitted = false;
		}
		slot.output.reset();
		if (!slot.pool || !slot.fence || !slot.ready || !slot.done || !slot.cmd[0] || !slot.cmd[1]) {
			if (slot.pool) { vkDestroyCommandPool(vk->GetDevice(), slot.pool, nullptr); }
			if (slot.fence) { vkDestroyFence(vk->GetDevice(), slot.fence, nullptr); }
			if (slot.ready) { vkDestroySemaphore(vk->GetDevice(), slot.ready, nullptr); }
			if (slot.done) { vkDestroySemaphore(vk->GetDevice(), slot.done, nullptr); }
			slot.pool = VK_NULL_HANDLE;
			slot.fence = VK_NULL_HANDLE;
			slot.ready = slot.done = VK_NULL_HANDLE;
			slot.cmd[0] = slot.cmd[1] = VK_NULL_HANDLE;
			VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			pool.queueFamilyIndex = vk->GetGraphicsQueueFamilyIndex();
			pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
			if (vkCreateCommandPool(vk->GetDevice(), &pool, nullptr, &slot.pool) != VK_SUCCESS) { return false; }
			VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			alloc.commandPool = slot.pool;
			alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			alloc.commandBufferCount = 2;
			if (vkAllocateCommandBuffers(vk->GetDevice(), &alloc, slot.cmd) != VK_SUCCESS) { return false; }
			VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			if (vkCreateFence(vk->GetDevice(), &fence, nullptr, &slot.fence) != VK_SUCCESS) { return false; }
			VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
			exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
			VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			sem.pNext = &exportInfo;
			if (vkCreateSemaphore(vk->GetDevice(), &sem, nullptr, &slot.ready) != VK_SUCCESS) { return false; }
			sem.pNext = nullptr;
			if (vkCreateSemaphore(vk->GetDevice(), &sem, nullptr, &slot.done) != VK_SUCCESS) { return false; }
		}
		if (!slot.input || !slot.input->valid || !slot.overlay || !slot.overlay->valid || slot.input->width != width || slot.input->height != height) {
			slot.input = std::make_shared<SharedImage>(vk);
			slot.overlay = std::make_shared<SharedImage>(vk);
			if (!slot.input->Create(width, height) || !slot.overlay->Create(width, height)) { return false; }
		}
		return vkResetCommandPool(vk->GetDevice(), slot.pool, 0) == VK_SUCCESS;
	}
	bool Session() {
		if (xr && sessionGeneration != generation) { Stop(); }
		if (xr) { return true; }
		Env e;
		if (!e.env) { return false; }
		const uint64_t version = generation;
		jobject app = Activity(e.env);
		std::lock_guard<std::mutex> lock(xrMutex);
		auto next = std::make_unique<XR>();
		bool ok = next->Init(app) && next->Session(vk);
		if (app) { e.env->DeleteLocalRef(app); }
		if (!ok || version != generation) { return false; }
		sessionGeneration = version;
		xr = std::move(next);
		return true;
	}
	bool Present(int index, std::shared_ptr<Capture> capture, Geometry geo, VkImage backbuffer, VkSemaphore complete) {
		const int w = vk->GetBackbufferWidth(), h = vk->GetBackbufferHeight();
		if (geo.width != w || geo.height != h || w <= 0 || h <= 0 || !Session()) { return false; }
		auto &slot = slots[index];
		if (!InitSlot(slot, w, h)) { return false; }
		slot.capture = std::move(capture);
		auto fb = slot.capture->framebuffer;
		const uint32_t family = vk->GetGraphicsQueueFamilyIndex();
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		if (vkBeginCommandBuffer(slot.cmd[0], &begin) != VK_SUCCESS) { return false; }
		Barrier(slot.cmd[0], fb->color.image, fb->color.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		Barrier(slot.cmd[0], backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		for (auto *target : {slot.input.get(), slot.overlay.get()}) {
			Barrier(slot.cmd[0], target->image, target->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				target->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
				target->layout == VK_IMAGE_LAYOUT_UNDEFINED ? VK_QUEUE_FAMILY_IGNORED : family);
		}
		Blit(slot.cmd[0], fb->color.image, w, h, slot.input->image, w, h);
		Blit(slot.cmd[0], backbuffer, w, h, slot.overlay->image, w, h);
		Barrier(slot.cmd[0], fb->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fb->color.layout);
		Barrier(slot.cmd[0], backbuffer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		for (auto *target : {slot.input.get(), slot.overlay.get()}) {
			Barrier(slot.cmd[0], target->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_FOREIGN_EXT);
			target->layout = VK_IMAGE_LAYOUT_GENERAL;
		}
		if (vkEndCommandBuffer(slot.cmd[0]) != VK_SUCCESS) { return false; }
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount = 1;
		submit.pWaitSemaphores = &complete;
		submit.pWaitDstStageMask = &stage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &slot.cmd[0];
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &slot.ready;
		if (vkQueueSubmit(vk->GetGraphicsQueue(), 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) { return false; }
		struct PresentSignal {
			VkQueue queue;
			VkSemaphore semaphore;
			Slot &slot;
			VkDevice device;
			bool pending = true;
			~PresentSignal() {
				if (pending) {
					VkSubmitInfo signal{VK_STRUCTURE_TYPE_SUBMIT_INFO};
					signal.signalSemaphoreCount = 1;
					signal.pSignalSemaphores = &semaphore;
					vkResetFences(device, 1, &slot.fence);
					slot.submitted = vkQueueSubmit(queue, 1, &signal, slot.fence) == VK_SUCCESS;
				}
			}
		} presentSignal{vk->GetGraphicsQueue(), complete, slot, vk->GetDevice()};
		// This fence also covers the previous output read, since both transfers use our graphics queue.
		auto exportFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(vk->GetDevice(), "vkGetSemaphoreFdKHR");
		auto importFd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(vk->GetDevice(), "vkImportSemaphoreFdKHR");
		int fd = -1;
		VkSemaphoreGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
		fdInfo.semaphore = slot.ready;
		fdInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
		bool ok = exportFd && importFd && exportFd(vk->GetDevice(), &fdInfo, &fd) == VK_SUCCESS;
		XrWeaveWindowGeometryDXR geometryInfo{XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR};
		geometryInfo.windowOriginOnScreen = {geo.x, geo.y};
		geometryInfo.clientSize = {w, h};
		geometryInfo.displayId = geo.display;
		XrWeaveBindWindowInfoDXR bindInfo{XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR};
		bindInfo.next = &geometryInfo;
		XrWeaveSubmitHandlesDXR handles{XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR};
		handles.inputKind = XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR;
		handles.overlayKind = XR_WEAVE_HANDLE_KIND_AHARDWAREBUFFER_DXR;
		XrWeaveSubmitOverlaysDXR overlays{XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR};
		overlays.next = &handles;
		overlays.overlayTexture = slot.overlay->buffer;
		XrWeaveSubmitSyncDXR sync{XR_TYPE_WEAVE_SUBMIT_SYNC_DXR};
		sync.next = &overlays;
		sync.acquireFenceFd = fd;
		XrWeaveSubmitInfoDXR info{XR_TYPE_WEAVE_SUBMIT_INFO_DXR};
		info.next = &sync;
		info.inputTexture = slot.input->buffer;
		info.rect.extent = {w, h};
		info.firstChunk = XR_TRUE;
		XrWeaveOutputSyncDXR release{XR_TYPE_WEAVE_OUTPUT_SYNC_DXR};
		release.releaseFenceFd = -1;
		XrWeaveOutputDXR out{XR_TYPE_WEAVE_OUTPUT_DXR};
		out.next = &release;
		{
			std::lock_guard<std::mutex> lock(xrMutex);
			ok = ok && XR_SUCCEEDED(xr->bind(xr->session, &bindInfo)) && XR_SUCCEEDED(xr->weave(xr->session, &info, &out));
		}
		if (ok) {
			fd = -1;  // The service owns it after a successful submit.
			if (out.weavedTexture) {
				auto next = std::make_shared<SharedImage>(vk);
				ok = next->Import((AHardwareBuffer *)out.weavedTexture);
				next->layout = VK_IMAGE_LAYOUT_GENERAL;
				if (ok) { output = std::move(next); }
			}
			ok = ok && output && out.width == (uint32_t)w && out.height == (uint32_t)h;
		}
		if (fd >= 0) { close(fd); }
		VkSemaphore wait = VK_NULL_HANDLE;
		if (release.releaseFenceFd >= 0) {
			VkImportSemaphoreFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
			imported.semaphore = slot.done;
			imported.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
			imported.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
			imported.fd = release.releaseFenceFd;
			if (importFd && importFd(vk->GetDevice(), &imported) == VK_SUCCESS) {
				wait = slot.done;
			} else {
				pollfd fence{release.releaseFenceFd, POLLIN, 0};
				poll(&fence, 1, 1000);
				close(release.releaseFenceFd);
				ok = false;
			}
		}
		if (vkBeginCommandBuffer(slot.cmd[1], &begin) != VK_SUCCESS) { return false; }
		Barrier(slot.cmd[1], backbuffer, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		if (ok) {
			slot.output = output;
			Barrier(slot.cmd[1], output->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_FOREIGN_EXT, family);
			Blit(slot.cmd[1], output->image, w, h, backbuffer, w, h);
			Barrier(slot.cmd[1], output->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, family, VK_QUEUE_FAMILY_FOREIGN_EXT);
		} else {
			// Defined mono output on the failure frame; normal rendering resumes next frame.
			Barrier(slot.cmd[1], fb->color.image, fb->color.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			Blit(slot.cmd[1], fb->color.image, w / 2, h, backbuffer, w, h);
			Barrier(slot.cmd[1], fb->color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fb->color.layout);
		}
		Barrier(slot.cmd[1], backbuffer, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
		if (vkEndCommandBuffer(slot.cmd[1]) != VK_SUCCESS) { return false; }
		submit.waitSemaphoreCount = wait ? 1 : 0;
		submit.pWaitSemaphores = &wait;
		submit.pCommandBuffers = &slot.cmd[1];
		submit.pSignalSemaphores = &complete;
		vkResetFences(vk->GetDevice(), 1, &slot.fence);
		if (vkQueueSubmit(vk->GetGraphicsQueue(), 1, &submit, slot.fence) != VK_SUCCESS) { return false; }
		presentSignal.pending = false;
		slot.submitted = true;
		return ok;
	}
	std::shared_ptr<Capture> atlases[VulkanContext::MAX_INFLIGHT_FRAMES];
};
std::mutex renderersMutex;
std::map<VulkanContext *, std::unique_ptr<Renderer>> renderers;
}

namespace {
bool HardwareReady(Draw::DrawContext *draw) {
	if (!eligible || draw->GetShaderLanguageDesc().shaderLanguage != GLSL_VULKAN || !draw->GetDeviceCaps().multiViewSupported) { return false; }
	auto vk = (VulkanContext *)draw->GetNativeObject(Draw::NativeObject::CONTEXT);
	if (!vk || vk->GetPresentation() || !vk->SupportsDisplayXRPresent()) { return false; }
	for (const char *name : {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}) {
		const auto &extensions = vk->GetDeviceExtensionsEnabled();
		if (std::none_of(extensions.begin(), extensions.end(), [&](const char *value) { return !strcmp(value, name); })) { return false; }
	}
	return true;
}
}
void DisplayXR_DeviceExtensions(VulkanContext *vk) {
	if (eligible) {
		for (const char *name : {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
			VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}) {
			if (vk->IsDeviceExtensionAvailable(name)) { vk->EnableDeviceExtension(name, 0); }
		}
	}
}
bool DisplayXR_CanRender(Draw::DrawContext *draw) {
	if (!active || !draw || !DisplayXR_Available()) { return false; }
	auto vk = (VulkanContext *)draw->GetNativeObject(Draw::NativeObject::CONTEXT);
	if (!vk || vk->GetPresentation() || !vk->SupportsDisplayXRPresent()) { return false; }
	std::lock_guard<std::mutex> lock(environmentMutex);
	return geometry.width == vk->GetBackbufferWidth() && geometry.height == vk->GetBackbufferHeight();
}
bool DisplayXR_Begin(Draw::DrawContext *draw, int width, int height) {
	if (!DisplayXR_CanRender(draw) || !DisplayXR_Selected()) { return false; }
	auto vk = (VulkanContext *)draw->GetNativeObject(Draw::NativeObject::CONTEXT);
	if (!vk || vk->GetPresentation()) { return false; }
	for (const char *name : {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
		VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME}) {
		const auto &enabled = vk->GetDeviceExtensionsEnabled();
		if (std::none_of(enabled.begin(), enabled.end(), [&](const char *s) { return !strcmp(s, name); })) {
			active = false;
			SetStatus(DisplayXRStatus::FAILED, "DisplayXR requires Vulkan external-memory support. Restart after installing DisplayXR.");
			return false;
		}
	}
	Geometry geo;
	{
		std::lock_guard<std::mutex> lock(environmentMutex);
		geo = geometry;
	}
	if (geo.width != width || geo.height != height) { return false; }
	std::lock_guard<std::mutex> lock(renderersMutex);
	auto &renderer = renderers[vk];
	if (!renderer) { renderer = std::make_unique<Renderer>(vk); }
	const int frame = vk->GetCurFrame();
	auto &capture = renderer->atlases[frame];
	int oldWidth = 0, oldHeight = 0;
	if (capture) { draw->GetFramebufferDimensions(capture->atlas, &oldWidth, &oldHeight); }
	if (!capture || oldWidth != width || oldHeight != height) {
		auto next = std::make_shared<Capture>();
		next->atlas = draw->CreateFramebuffer({width, height, 1, 1, 0, false, "displayxr_atlas"});
		if (!next->atlas) { return false; }
		next->framebuffer = (VKRFramebuffer *)draw->GetNativeObject(Draw::NativeObject::ANDROID_FRAMEBUFFER, next->atlas);
		capture = std::move(next);
	}
	draw->BindFramebufferAsRenderTarget(capture->atlas, {Draw::RPAction::CLEAR, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE, 0}, "DisplayXR atlas");
	auto manager = (VulkanRenderManager *)draw->GetNativeObject(Draw::NativeObject::RENDER_MANAGER);
	Renderer *render = renderer.get();
	const uint64_t version = generation;
	manager->SetAndroidPresent([render, frame, capture, geo, version](VkImage image, VkSemaphore semaphore) {
		if (!foreground || version != generation) { render->Stop(); return; }
		if (!render->Present(frame, capture, geo, image, semaphore) && foreground && version == generation) {
			active = false;
			SetStatus(DisplayXRStatus::FAILED, "DisplayXR output failed. Check runtime setup, permissions and native display resolution.");
		}
	});
	return true;
}
void DisplayXR_End(Draw::DrawContext *draw) {
	draw->BindFramebufferAsRenderTarget(nullptr, {Draw::RPAction::CLEAR, Draw::RPAction::DONT_CARE, Draw::RPAction::DONT_CARE, 0}, "DisplayXR UI");
}
void DisplayXR_Present(VulkanContext *vk, int, void *, void *) {
	std::lock_guard<std::mutex> lock(renderersMutex);
	auto it = renderers.find(vk);
	if (it != renderers.end()) { it->second->Stop(); }
}
void DisplayXR_Destroy(VulkanContext *vk) {
	std::lock_guard<std::mutex> lock(renderersMutex);
	renderers.erase(vk);
}
#else
namespace { bool HardwareReady(Draw::DrawContext *) { return false; } }
extern "C" JNIEXPORT void JNICALL Java_org_ppsspp_ppsspp_DisplayXRSupport_nativeProbe(JNIEnv *, jclass, jobject, jboolean) {}
void DisplayXR_DeviceExtensions(VulkanContext *) {}
bool DisplayXR_CanRender(Draw::DrawContext *) { return false; }
bool DisplayXR_Begin(Draw::DrawContext *, int, int) { return false; }
void DisplayXR_End(Draw::DrawContext *) {}
void DisplayXR_Present(VulkanContext *, int, void *, void *) {}
void DisplayXR_Destroy(VulkanContext *) {}
#endif
