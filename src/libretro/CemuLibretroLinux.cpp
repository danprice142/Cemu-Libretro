// Cemu libretro core - main implementation
// Routes video/audio/input through libretro frontend callbacks

#include "libretro.h"

#include "config/CemuConfig.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "config/NetworkSettings.h"

#include "Cafe/CafeSystem.h"
#include "Cafe/TitleList/TitleList.h"
#include "Cafe/TitleList/TitleInfo.h"
#include "Cafe/TitleList/SaveList.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Renderer/OpenGL/OpenGLRenderer.h"
#ifdef ENABLE_VULKAN
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanRenderer.h"
#include "Cafe/HW/Latte/Renderer/Vulkan/VulkanAPI.h"
#include "libretro_vulkan.h"
#endif

#include "audio/IAudioAPI.h"
#include "input/InputManager.h"
#include "input/emulated/VPADController.h"

#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"

#include "util/crypto/aes128.h"
#include "util/helpers/helpers.h"

#include "Cafe/GraphicPack/GraphicPack2.h"
#include "Cafe/GameProfile/GameProfile.h"

#include "interface/WindowSystem.h"

#include "LibretroAudioAPI.h"

// GL function needed for framebuffer readback (glBindFramebuffer is in Cemu's glext.h)
extern "C" {
extern void glReadPixels(int x, int y, int width, int height, unsigned int format, unsigned int type, void* pixels);
}
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif

#include <mutex>
#include <condition_variable>
#include <atomic>

// GLX for shared context creation (Linux)
// X11 Bool conflicts with Cemu enums, so we define it before including GLX
#ifndef Bool
#define Bool int
#define CEMU_DEFINED_BOOL
#endif
#include <X11/Xlib.h>
#include <GL/glx.h>
#ifdef CEMU_DEFINED_BOOL
#undef Bool
#endif
#include <dlfcn.h>

// Shared GL context for Cemu GPU thread
static Display* s_glx_display = nullptr;
static GLXDrawable s_glx_drawable = 0;
static GLXContext s_glx_shared_context = nullptr;  // created for GPU thread
static GLXContext s_glx_frontend_context = nullptr; // RetroArch's context
static bool s_gpu_context_made_current = false;

// ============================================================================
// CafeSystem implementation for libretro
// ============================================================================

class LibretroSystemImplementation : public CafeSystem::SystemImplementation
{
public:
	void CafeRecreateCanvas() override
	{
		// In libretro, the canvas is managed by the frontend - nothing to do
	}
};

static LibretroSystemImplementation s_systemImpl;

// ============================================================================
// Globals
// ============================================================================

static retro_environment_t environ_cb = nullptr;
static retro_video_refresh_t video_cb = nullptr;
static retro_audio_sample_t audio_cb = nullptr;
static retro_audio_sample_batch_t audio_batch_cb = nullptr;
static retro_input_poll_t input_poll_cb = nullptr;
static retro_input_state_t input_state_cb = nullptr;
static retro_log_printf_t log_cb = nullptr;

static bool s_game_loaded = false;
static bool s_initialized = false;
static bool s_emu_initialized = false;
static std::string s_game_path;

// Frontend GL objects for blitting (reset on context destroy/resize)
static GLuint s_frontend_read_fbo = 0;
static GLuint s_frontend_read_rbo_attached = 0;
static GLuint s_frontend_upload_tex = 0;

// Frame synchronization
static std::mutex s_frame_mutex;
static std::condition_variable s_frame_cv;
std::atomic_bool s_frame_ready{false};
static std::atomic_bool s_shutting_down{false};

// Framebuffer for software readback
static constexpr uint32_t SCREEN_WIDTH = 1280;
static constexpr uint32_t SCREEN_HEIGHT = 720;
std::vector<uint32_t> s_libretro_framebuffer(SCREEN_WIDTH * SCREEN_HEIGHT);
static auto& s_framebuffer = s_libretro_framebuffer; // alias for existing OpenGL code
static bool s_use_hw_render = false;
static bool s_hw_render_initialized = false;
static bool s_core_options_supported = false;

enum class SelectedGraphicsAPI { OpenGL, Vulkan };
static SelectedGraphicsAPI s_graphics_api = SelectedGraphicsAPI::OpenGL;

static retro_hw_render_callback s_hw_render{};

#ifdef ENABLE_VULKAN
// Vulkan HW render interface
static const struct retro_hw_render_interface_vulkan* s_vk_interface = nullptr;
static struct retro_vulkan_image s_vk_present_image{};
static retro_vulkan_context s_vk_context{};
static bool s_vk_device_created = false;

static const VkApplicationInfo* libretro_vk_get_application_info()
{
	static VkApplicationInfo app_info{};
	app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	app_info.pApplicationName = "Cemu";
	app_info.applicationVersion = VK_MAKE_VERSION(2, 6, 0);
	app_info.pEngineName = "Cemu";
	app_info.engineVersion = VK_MAKE_VERSION(2, 6, 0);
	app_info.apiVersion = VK_API_VERSION_1_1;
	return &app_info;
}

static bool libretro_vk_create_device(
	struct retro_vulkan_context* context,
	VkInstance instance,
	VkPhysicalDevice gpu,
	VkSurfaceKHR surface,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr,
	const char** required_device_extensions,
	unsigned num_required_device_extensions,
	const char** required_device_layers,
	unsigned num_required_device_layers,
	const VkPhysicalDeviceFeatures* required_features)
{
	if (log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: Vulkan create_device called (gpu=%p, surface=%p)\n", gpu, surface);

	// Load instance functions
	if (!InitializeGlobalVulkan())
		return false;
	if (!InitializeInstanceVulkan(instance))
		return false;

	// Find suitable physical device if not provided
	if (gpu == VK_NULL_HANDLE)
	{
		uint32_t count = 0;
		vkEnumeratePhysicalDevices(instance, &count, nullptr);
		std::vector<VkPhysicalDevice> devices(count);
		vkEnumeratePhysicalDevices(instance, &count, devices.data());
		for (auto& d : devices)
		{
			VkPhysicalDeviceProperties props;
			vkGetPhysicalDeviceProperties(d, &props);
			if (props.apiVersion >= VK_API_VERSION_1_1)
			{
				gpu = d;
				break;
			}
		}
		if (gpu == VK_NULL_HANDLE && !devices.empty())
			gpu = devices[0];
	}

	// Find graphics queue family
	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueFamilyCount, nullptr);
	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu, &queueFamilyCount, queueFamilies.data());

	uint32_t graphicsFamily = 0;
	for (uint32_t i = 0; i < queueFamilyCount; i++)
	{
		if (queueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			graphicsFamily = i;
			break;
		}
	}

	// Build extension list: RetroArch required + Cemu needs
	std::vector<const char*> extensions;
	for (unsigned i = 0; i < num_required_device_extensions; i++)
		extensions.push_back(required_device_extensions[i]);

	// Add Cemu-specific extensions if available
	uint32_t extCount = 0;
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, nullptr);
	std::vector<VkExtensionProperties> availableExts(extCount);
	vkEnumerateDeviceExtensionProperties(gpu, nullptr, &extCount, availableExts.data());

	auto hasExt = [&](const char* name) {
		for (auto& e : availableExts)
			if (strcmp(e.extensionName, name) == 0) return true;
		return false;
	};

	const char* cemuExtensions[] = {
		VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
		VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME,
		"VK_EXT_pipeline_creation_cache_control",
		"VK_EXT_custom_border_color",
		"VK_EXT_pipeline_robustness",
		"VK_KHR_shader_float_controls",
	};
	for (auto ext : cemuExtensions)
	{
		if (hasExt(ext))
			extensions.push_back(ext);
	}

	// Device features
	VkPhysicalDeviceFeatures features{};
	if (required_features)
		features = *required_features;
	features.independentBlend = VK_TRUE;
	features.samplerAnisotropy = VK_TRUE;
	features.imageCubeArray = VK_TRUE;
	features.logicOp = VK_TRUE;
	features.geometryShader = VK_TRUE;
	features.occlusionQueryPrecise = VK_TRUE;
	features.depthClamp = VK_TRUE;
	features.depthBiasClamp = VK_TRUE;

	float queuePriority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo{};
	queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queueInfo.queueFamilyIndex = graphicsFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &queuePriority;

	// Enable transform feedback features if available
	VkPhysicalDeviceTransformFeedbackFeaturesEXT tfFeatures{};
	tfFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT;
	tfFeatures.transformFeedback = VK_TRUE;

	VkDeviceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	createInfo.queueCreateInfoCount = 1;
	createInfo.pQueueCreateInfos = &queueInfo;
	createInfo.enabledExtensionCount = (uint32_t)extensions.size();
	createInfo.ppEnabledExtensionNames = extensions.data();
	createInfo.pEnabledFeatures = &features;
	if (hasExt(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME))
		createInfo.pNext = &tfFeatures;

	VkDevice device;
	VkResult result = vkCreateDevice(gpu, &createInfo, nullptr, &device);
	if (result != VK_SUCCESS)
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Cemu: Failed to create Vulkan device: %d\n", result);
		return false;
	}

	InitializeDeviceVulkan(device);

	VkQueue queue;
	vkGetDeviceQueue(device, graphicsFamily, 0, &queue);

	context->gpu = gpu;
	context->device = device;
	context->queue = queue;
	context->queue_family_index = graphicsFamily;
	context->presentation_queue = queue;
	context->presentation_queue_family_index = graphicsFamily;

	s_vk_context = *context;
	s_vk_device_created = true;

	if (log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: Vulkan device created (queue family %u)\n", graphicsFamily);
	return true;
}

static void libretro_vk_destroy_device()
{
	if (log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: Vulkan destroy_device called\n");
	s_vk_device_created = false;
}

static struct retro_hw_render_context_negotiation_interface_vulkan s_vk_negotiation{
	RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN,
	RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN_VERSION,
	libretro_vk_get_application_info,
	libretro_vk_create_device,
	libretro_vk_destroy_device,
};
#endif

// ============================================================================
// OpenGL Canvas Callbacks for libretro
// ============================================================================

class LibretroGLCanvasCallbacks : public OpenGLCanvasCallbacks
{
public:
	LibretroGLCanvasCallbacks()
	{
		SetOpenGLCanvasCallbacks(this);
	}

	~LibretroGLCanvasCallbacks()
	{
		ClearOpenGLCanvasCallbacks();
	}

	bool HasPadViewOpen() const override
	{
		return false;
	}

	bool MakeCurrent(bool padView) override
	{
		if (padView)
			return false;
		if (!s_hw_render_initialized || s_shutting_down)
			return false;
		// Make our shared GL context current on the GPU thread
		if (s_glx_shared_context && s_glx_display && s_glx_drawable && !s_gpu_context_made_current)
		{
			int result = glXMakeCurrent(s_glx_display, s_glx_drawable, s_glx_shared_context);
			if (result)
			{
				s_gpu_context_made_current = true;
				if (log_cb)
					log_cb(RETRO_LOG_INFO, "Cemu: GPU thread GL context made current successfully\n");
			}
			else
			{
				if (log_cb)
					log_cb(RETRO_LOG_ERROR, "Cemu: Failed to make GPU thread GL context current\n");
				return false;
			}
		}
		return true;
	}

	void SwapBuffers(bool swapTV, bool swapDRC) override
	{
		if (s_shutting_down)
			return;
		if (swapTV || swapDRC)
		{
			// Read pixels from our backbuffer FBO on the GPU thread (where GL context is valid)
			extern GLuint libretro_getBackbufferFBO(int, int);
			GLuint fbo = libretro_getBackbufferFBO(SCREEN_WIDTH, SCREEN_HEIGHT);
			glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, fbo);
			glReadPixels(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, GL_BGRA, GL_UNSIGNED_BYTE, s_framebuffer.data());

			std::lock_guard lock(s_frame_mutex);
			s_frame_ready = true;
			s_frame_cv.notify_one();
		}
	}
};

static std::unique_ptr<LibretroGLCanvasCallbacks> s_gl_callbacks;

// ============================================================================
// Libretro input state
// ============================================================================

struct LibretroInputState
{
	int16_t buttons[16]{};
	int16_t left_x = 0, left_y = 0;
	int16_t right_x = 0, right_y = 0;
	bool touch_pressed = false;
	int16_t touch_x = 0; // -0x7fff..0x7fff
	int16_t touch_y = 0;
};

static LibretroInputState s_input_state;

// ============================================================================
// Forward declarations from main.cpp
// ============================================================================

extern void CemuCommonInit();
extern std::atomic_bool g_isGPUInitFinished;

// ============================================================================
// Helper: Initialize paths for libretro
// ============================================================================

static void libretro_init_paths()
{
	const char* system_dir = nullptr;
	const char* save_dir = nullptr;

	if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
	{
		// Use system/Cemu as the main directory
	}
	else
	{
		system_dir = ".";
	}

	if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
	{
		// Use save dir for MLC
	}
	else
	{
		save_dir = system_dir;
	}

	fs::path sysPath = fs::path(system_dir) / "Cemu";
	fs::path savePath = fs::path(save_dir) / "Cemu";

	std::error_code ec;
	fs::create_directories(sysPath, ec);
	fs::create_directories(savePath, ec);
	fs::create_directories(sysPath / "keys", ec);
	fs::create_directories(sysPath / "shaderCache", ec);
	fs::create_directories(savePath / "mlc01", ec);

	std::set<fs::path> failedWriteAccess;
	ActiveSettings::SetPaths(
		false,               // not portable
		sysPath / "Cemu",    // executable path (dummy)
		sysPath,             // user data path
		sysPath,             // config path
		sysPath / "shaderCache", // cache path
		sysPath,             // data path
		failedWriteAccess
	);
}

// ============================================================================
// Helper: Create libretro audio device
// ============================================================================

static void libretro_init_audio()
{
	std::unique_lock lock(g_audioMutex);
	g_tvAudio = std::make_unique<LibretroAudioAPI>(48000, 2, 256, 16);
	g_tvAudio->Play();
	// Don't create pad audio - it shares the same static buffer as TV
	// and mixing both causes distortion
}

// ============================================================================
// Libretro API implementation
// ============================================================================

// ============================================================================
// Core options helpers (matching danprice/Cemu-Libretro)
// ============================================================================

static const char* libretro_get_option_value(const char* key)
{
	if (!s_core_options_supported || !environ_cb || !key)
		return nullptr;
	retro_variable var{key, nullptr};
	if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
		return nullptr;
	return var.value;
}

static bool libretro_iequals(const char* a, const char* b)
{
	if (!a || !b)
		return false;
	for (; *a && *b; ++a, ++b)
	{
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
			return false;
	}
	return *a == *b;
}

static bool libretro_parse_enabled_disabled(const char* v, bool& out)
{
	if (!v) return false;
	if (libretro_iequals(v, "enabled") || libretro_iequals(v, "true") || libretro_iequals(v, "1") || libretro_iequals(v, "on"))
	{ out = true; return true; }
	if (libretro_iequals(v, "disabled") || libretro_iequals(v, "false") || libretro_iequals(v, "0") || libretro_iequals(v, "off"))
	{ out = false; return true; }
	return false;
}

static std::optional<CPUMode> libretro_parse_cpu_mode(const char* v)
{
	if (!v) return std::nullopt;
	if (libretro_iequals(v, "auto")) return CPUMode::Auto;
	if (libretro_iequals(v, "singlecore_interpreter")) return CPUMode::SinglecoreInterpreter;
	if (libretro_iequals(v, "singlecore_recompiler")) return CPUMode::SinglecoreRecompiler;
	if (libretro_iequals(v, "multicore_recompiler")) return CPUMode::MulticoreRecompiler;
	return std::nullopt;
}

static std::optional<PrecompiledShaderOption> libretro_parse_precompiled_shaders(const char* v)
{
	if (!v) return std::nullopt;
	if (libretro_iequals(v, "auto")) return PrecompiledShaderOption::Auto;
	if (libretro_iequals(v, "enabled")) return PrecompiledShaderOption::Enable;
	if (libretro_iequals(v, "disabled")) return PrecompiledShaderOption::Disable;
	return std::nullopt;
}

static std::optional<CafeConsoleLanguage> libretro_parse_console_language(const char* v)
{
	if (!v) return std::nullopt;
	if (libretro_iequals(v, "Japanese")) return CafeConsoleLanguage::JA;
	if (libretro_iequals(v, "English")) return CafeConsoleLanguage::EN;
	if (libretro_iequals(v, "French")) return CafeConsoleLanguage::FR;
	if (libretro_iequals(v, "German")) return CafeConsoleLanguage::DE;
	if (libretro_iequals(v, "Italian")) return CafeConsoleLanguage::IT;
	if (libretro_iequals(v, "Spanish")) return CafeConsoleLanguage::ES;
	if (libretro_iequals(v, "Chinese")) return CafeConsoleLanguage::ZH;
	if (libretro_iequals(v, "Korean")) return CafeConsoleLanguage::KO;
	if (libretro_iequals(v, "Dutch")) return CafeConsoleLanguage::NL;
	if (libretro_iequals(v, "Portuguese")) return CafeConsoleLanguage::PT;
	if (libretro_iequals(v, "Russian")) return CafeConsoleLanguage::RU;
	if (libretro_iequals(v, "Taiwanese")) return CafeConsoleLanguage::TW;
	return std::nullopt;
}

static bool libretro_parse_internal_resolution(const char* v, unsigned& outWidth, unsigned& outHeight)
{
	if (!v) return false;
	if (libretro_iequals(v, "1280x720")) { outWidth = 1280; outHeight = 720; return true; }
	if (libretro_iequals(v, "1920x1080")) { outWidth = 1920; outHeight = 1080; return true; }
	if (libretro_iequals(v, "2560x1440")) { outWidth = 2560; outHeight = 1440; return true; }
	if (libretro_iequals(v, "3840x2160")) { outWidth = 3840; outHeight = 2160; return true; }
	return false;
}

static void libretro_apply_core_options()
{
	if (!environ_cb || !s_core_options_supported)
		return;

	auto& cfg = GetConfig();

	// Async shader compilation
	if (const char* v = libretro_get_option_value("cemu_async_shader_compile"))
	{
		bool b;
		if (libretro_parse_enabled_disabled(v, b))
			cfg.async_compile = b;
	}

	// GX2DrawDone sync
	if (const char* v = libretro_get_option_value("cemu_gx2drawdone_sync"))
	{
		bool b;
		if (libretro_parse_enabled_disabled(v, b))
			cfg.gx2drawdone_sync = b;
	}

	// Console language
	if (const char* v = libretro_get_option_value("cemu_console_language"))
	{
		auto lang = libretro_parse_console_language(v);
		if (lang.has_value())
			cfg.console_language = lang.value();
	}

	// Upscale filter
	if (const char* v = libretro_get_option_value("cemu_upscale_filter"))
	{
		if (libretro_iequals(v, "linear")) cfg.upscale_filter = kLinearFilter;
		else if (libretro_iequals(v, "bicubic")) cfg.upscale_filter = kBicubicFilter;
		else if (libretro_iequals(v, "bicubic_hermite")) cfg.upscale_filter = kBicubicHermiteFilter;
		else if (libretro_iequals(v, "nearest")) cfg.upscale_filter = kNearestNeighborFilter;
	}

	// Downscale filter
	if (const char* v = libretro_get_option_value("cemu_downscale_filter"))
	{
		if (libretro_iequals(v, "linear")) cfg.downscale_filter = kLinearFilter;
		else if (libretro_iequals(v, "bicubic")) cfg.downscale_filter = kBicubicFilter;
		else if (libretro_iequals(v, "bicubic_hermite")) cfg.downscale_filter = kBicubicHermiteFilter;
		else if (libretro_iequals(v, "nearest")) cfg.downscale_filter = kNearestNeighborFilter;
	}

	// Fullscreen scaling
	if (const char* v = libretro_get_option_value("cemu_fullscreen_scaling"))
	{
		if (libretro_iequals(v, "keep_aspect")) cfg.fullscreen_scaling = 0;
		else if (libretro_iequals(v, "stretch")) cfg.fullscreen_scaling = 1;
	}

	// CPU mode & precompiled shaders (ActiveSettings overrides)
	ActiveSettings::SetLibretroCPUModeOverride(libretro_parse_cpu_mode(libretro_get_option_value("cemu_cpu_mode")));
	ActiveSettings::SetLibretroPrecompiledShadersOverride(libretro_parse_precompiled_shaders(libretro_get_option_value("cemu_precompiled_shaders")));

	// Internal resolution
	if (const char* v = libretro_get_option_value("cemu_internal_resolution"))
	{
		unsigned newWidth, newHeight;
		if (libretro_parse_internal_resolution(v, newWidth, newHeight))
		{
			auto& windowInfo = WindowSystem::GetWindowInfo();
			windowInfo.width = newWidth;
			windowInfo.height = newHeight;
			windowInfo.phys_width = newWidth;
			windowInfo.phys_height = newHeight;
		}
	}

	// Thread quantum
	if (const char* v = libretro_get_option_value("cemu_thread_quantum"))
	{
		int quantum = atoi(v);
		if (quantum >= 1000 && quantum <= 536870912)
		{
			extern uint32 ppcThreadQuantum;
			ppcThreadQuantum = (uint32)quantum;
		}
	}

	// Audio latency
	if (const char* v = libretro_get_option_value("cemu_audio_latency"))
	{
		int delay = atoi(v);
		if (delay >= 1 && delay <= 8)
			cfg.audio_delay = delay;
	}

	// Accurate shader multiplication
	if (const char* v = libretro_get_option_value("cemu_accurate_shader_mul"))
	{
		bool enabled;
		if (libretro_parse_enabled_disabled(v, enabled))
		{
			if (g_current_game_profile)
				g_current_game_profile->SetAccurateShaderMul(enabled ? AccurateShaderMulOption::True : AccurateShaderMulOption::False);
		}
	}

	// Shader fast math
	if (const char* v = libretro_get_option_value("cemu_shader_fast_math"))
	{
		bool enabled;
		if (libretro_parse_enabled_disabled(v, enabled))
		{
			if (g_current_game_profile)
				g_current_game_profile->SetShaderFastMath(enabled);
		}
	}

	// VSync
	if (const char* v = libretro_get_option_value("cemu_vsync"))
	{
		bool enabled;
		if (libretro_parse_enabled_disabled(v, enabled))
			cfg.vsync = enabled ? 1 : 0;
	}

	// Shader compile notification
	if (const char* v = libretro_get_option_value("cemu_shader_compile_notification"))
	{
		bool enabled;
		if (libretro_parse_enabled_disabled(v, enabled))
			cfg.notification.shader_compiling = enabled;
	}

	// USB Device emulation
	if (const char* v = libretro_get_option_value("cemu_emulate_skylander_portal"))
	{
		bool enabled;
		if (libretro_parse_enabled_disabled(v, enabled))
			cfg.emulated_usb_devices.emulate_skylander_portal = enabled;
	}
	if (const char* v = libretro_get_option_value("cemu_emulate_infinity_base"))
	{
		bool enabled;
		if (libretro_parse_enabled_disabled(v, enabled))
			cfg.emulated_usb_devices.emulate_infinity_base = enabled;
	}
	if (const char* v = libretro_get_option_value("cemu_emulate_dimensions_toypad"))
	{
		bool enabled;
		if (libretro_parse_enabled_disabled(v, enabled))
			cfg.emulated_usb_devices.emulate_dimensions_toypad = enabled;
	}
}

RETRO_API void retro_set_environment(retro_environment_t cb)
{
	environ_cb = cb;

	// Declare that we need a game file
	bool no_game = false;
	cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_game);

	// Set up core options (matching danprice/Cemu-Libretro Windows core where applicable)
	static const struct retro_variable variables[] = {
		{"cemu_cpu_mode", "CPU Mode; auto|singlecore_interpreter|singlecore_recompiler|multicore_recompiler"},
		{"cemu_console_language", "Console Language; English|Japanese|French|German|Italian|Spanish|Chinese|Korean|Dutch|Portuguese|Russian|Taiwanese"},
		{"cemu_async_shader_compile", "Async Shader Compile; enabled|disabled"},
		{"cemu_gx2drawdone_sync", "GX2DrawDone Sync; enabled|disabled"},
		{"cemu_precompiled_shaders", "Precompiled shaders; auto|enabled|disabled"},
		{"cemu_accurate_shader_mul", "Accurate Shader Multiplication; enabled|disabled"},
		{"cemu_shader_fast_math", "Shader Fast Math; enabled|disabled"},
		{"cemu_upscale_filter", "Upscale filter; linear|bicubic|bicubic_hermite|nearest"},
		{"cemu_downscale_filter", "Downscale filter; linear|bicubic|bicubic_hermite|nearest"},
		{"cemu_internal_resolution", "Internal Resolution; 1280x720|1920x1080|2560x1440|3840x2160"},
		{"cemu_fullscreen_scaling", "Fullscreen scaling; keep_aspect|stretch"},
		{"cemu_thread_quantum", "Thread Quantum; 45000|20000|60000|80000|100000"},
		{"cemu_audio_latency", "Audio Latency; 2|1|3|4"},
		{"cemu_vsync", "VSync; disabled|enabled"},
		{"cemu_shader_compile_notification", "Shader Compile Notification; enabled|disabled"},
		{"cemu_emulate_skylander_portal", "Emulate Skylander Portal; disabled|enabled"},
		{"cemu_emulate_infinity_base", "Emulate Infinity Base; disabled|enabled"},
		{"cemu_emulate_dimensions_toypad", "Emulate Dimensions Toypad; disabled|enabled"},
		{"cemu_skip_draw_on_dupe", "Skip Draw on Duplicate Frames; disabled|enabled"},
#ifdef ENABLE_VULKAN
		{"cemu_gpu_api", "Graphics API (restart); OpenGL|Vulkan"},
#endif
		{nullptr, nullptr},
	};
	s_core_options_supported = cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)variables);

	// Set up logging
	struct retro_log_callback logging;
	if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
		log_cb = logging.log;
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

RETRO_API void retro_init()
{
	if (s_initialized)
		return;

	libretro_init_paths();

	// Configure settings
	GetConfigHandle().SetFilename(ActiveSettings::GetConfigPath("settings.xml").generic_wstring());
	if (fs::exists(ActiveSettings::GetConfigPath("settings.xml")))
		GetConfigHandle().Load();

	// Select graphics API based on core option
	s_graphics_api = SelectedGraphicsAPI::OpenGL;
#ifdef ENABLE_VULKAN
	if (const char* v = libretro_get_option_value("cemu_gpu_api"))
	{
		if (libretro_iequals(v, "vulkan"))
		{
			if (InitializeGlobalVulkan() && g_vulkan_available)
			{
				s_graphics_api = SelectedGraphicsAPI::Vulkan;
				GetConfig().graphic_api = kVulkan;
				if (log_cb)
					log_cb(RETRO_LOG_INFO, "Cemu: Vulkan graphics API selected\n");
			}
			else if (log_cb)
				log_cb(RETRO_LOG_WARN, "Cemu: Vulkan not available, falling back to OpenGL\n");
		}
	}
#endif
	if (s_graphics_api == SelectedGraphicsAPI::OpenGL)
		GetConfig().graphic_api = kOpenGL;

	ActiveSettings::Init();

	// Set system implementation
	CafeSystem::SetImplementation(&s_systemImpl);

	s_initialized = true;
}

// retro_deinit is defined after retro_unload_game

RETRO_API unsigned retro_api_version()
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info* info)
{
	info->library_name = "Cemu";
	info->library_version = "2.6";
	info->need_fullpath = true;
	info->valid_extensions = "wud|wux|wua|iso|rpx|elf";
	info->block_extract = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info* info)
{
	info->geometry.base_width = SCREEN_WIDTH;
	info->geometry.base_height = SCREEN_HEIGHT;
	info->geometry.max_width = SCREEN_WIDTH * 4;
	info->geometry.max_height = SCREEN_HEIGHT * 4;
	info->geometry.aspect_ratio = 16.0f / 9.0f;
	info->timing.fps = 60.0;
	info->timing.sample_rate = 48000.0;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
	// Port 0 = VPAD (Wii U GamePad)
	// Ports 1-4 = WPAD (Wii Remote / Pro Controller)
}

RETRO_API void retro_reset()
{
	if (!s_game_loaded || s_game_path.empty())
		return;

	const char* corePath = nullptr;
	if (environ_cb)
		environ_cb(RETRO_ENVIRONMENT_GET_LIBRETRO_PATH, &corePath);

	// Prepare relaunch command BEFORE exit
	std::string cmd;
	if (corePath)
	{
		cmd = "sh -c 'sleep 1 && retroarch -L \"";
		cmd += corePath;
		cmd += "\" \"";
		cmd += s_game_path;
		cmd += "\"' &";
	}

	// Launch relaunch process and immediately exit
	// _exit() skips all destructors/atexit — no Vulkan cleanup crash
	if (!cmd.empty())
		system(cmd.c_str());
	_exit(0);
}

static void libretro_launch_game()
{
	if (s_game_path.empty() || s_emu_initialized)
		return;

	if (log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: Initializing emulator...\n");

	// Initialize emulator common systems
	CemuCommonInit();

	// Load graphic packs (includes workarounds like NSMBU crash fix)
	{
		fs::path gpPath = ActiveSettings::GetUserDataPath("graphicPacks");
		cemuLog_log(LogType::Force, "Searching for graphic packs in: {}", _pathToUtf8(gpPath));
		std::error_code ec;
		bool exists = fs::exists(gpPath, ec);
		cemuLog_log(LogType::Force, "Graphic packs directory exists: {}", exists);
	}
	GraphicPack2::LoadAll();
	// Enable all graphic packs that have default=1 (workarounds etc.)
	for (auto& gp : GraphicPack2::GetGraphicPacks())
	{
		if (gp->IsDefaultEnabled() && !gp->IsEnabled())
			gp->SetEnabled(true);
	}
	if (log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: Loaded %d graphic packs\n", (int)GraphicPack2::GetGraphicPacks().size());

	// Apply core options before launch
	libretro_apply_core_options();

	// Init audio through libretro
	libretro_init_audio();

	// Prepare the game
	fs::path gamePath = s_game_path;
	CafeSystem::PREPARE_STATUS_CODE status;

	// Try as a title first (WUD/WUX/WUA/folder)
	TitleInfo launchTitle{gamePath};
	if (launchTitle.IsValid())
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "Cemu: Valid title detected, launching via TitleId\n");

		CafeTitleList::AddTitleFromPath(gamePath);
		CafeTitleList::WaitForMandatoryScan();

		TitleId baseTitleId;
		if (!CafeTitleList::FindBaseTitleId(launchTitle.GetAppTitleId(), baseTitleId))
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "Cemu: Could not find base title ID\n");
			return;
		}

		status = CafeSystem::PrepareForegroundTitle(baseTitleId);
	}
	else
	{
		// Fall back to standalone RPX/ELF
		CafeTitleFileType fileType = DetermineCafeSystemFileType(gamePath);
		if (fileType == CafeTitleFileType::RPX || fileType == CafeTitleFileType::ELF)
		{
			if (log_cb)
				log_cb(RETRO_LOG_INFO, "Cemu: Launching as standalone RPX/ELF\n");
			status = CafeSystem::PrepareForegroundTitleFromStandaloneRPX(gamePath);
		}
		else
		{
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "Cemu: Unsupported file format\n");
			return;
		}
	}

	if (status != CafeSystem::PREPARE_STATUS_CODE::SUCCESS)
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Cemu: Failed to prepare game (status %d)\n", (int)status);
		return;
	}

	// Launch the title
	CafeSystem::LaunchForegroundTitle();

	// Wait for GPU init
	while (!g_isGPUInitFinished)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));

	// Keep TV screen as default (DRC is black for some games)

	s_emu_initialized = true;
	s_game_loaded = true;

	if (log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: Game loaded successfully - %s\n", CafeSystem::GetForegroundTitleName().c_str());
}

static std::atomic_bool s_launch_thread_running{false};

static void libretro_create_shared_gl_context()
{
	// Capture the frontend's GL context info so we can create a shared context for GPU thread
	s_glx_display = glXGetCurrentDisplay();
	s_glx_drawable = glXGetCurrentDrawable();
	s_glx_frontend_context = glXGetCurrentContext();

	if (!s_glx_display || !s_glx_frontend_context)
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Cemu: Cannot get current GLX context (display=%p ctx=%p)\n",
				s_glx_display, s_glx_frontend_context);
		return;
	}

	if (log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: Frontend GL context: display=%p drawable=0x%lx ctx=%p\n",
			s_glx_display, (unsigned long)s_glx_drawable, s_glx_frontend_context);

	// Get the FBConfig used by the frontend context
	// We need this to create a compatible shared context
	int screenNum = DefaultScreen(s_glx_display);

	// Use glXCreateContextAttribsARB for core profile context
	typedef GLXContext (*glXCreateContextAttribsARBProc)(Display*, GLXFBConfig, GLXContext, int, const int*);
	glXCreateContextAttribsARBProc _glXCreateContextAttribsARB =
		(glXCreateContextAttribsARBProc)glXGetProcAddress((const GLubyte*)"glXCreateContextAttribsARB");

	if (!_glXCreateContextAttribsARB)
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Cemu: glXCreateContextAttribsARB not available\n");
		return;
	}

	// Get the FBConfig ID from the frontend's current context
	int fbconfig_id = 0;
	glXQueryContext(s_glx_display, s_glx_frontend_context, GLX_FBCONFIG_ID, &fbconfig_id);

	// Find matching FBConfig
	int nelements = 0;
	GLXFBConfig* configs = glXGetFBConfigs(s_glx_display, screenNum, &nelements);
	if (!configs || nelements == 0)
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Cemu: No GLX FBConfigs available\n");
		return;
	}

	GLXFBConfig chosen_config = configs[0]; // fallback
	for (int i = 0; i < nelements; i++)
	{
		int id = 0;
		glXGetFBConfigAttrib(s_glx_display, configs[i], GLX_FBCONFIG_ID, &id);
		if (id == fbconfig_id)
		{
			chosen_config = configs[i];
			break;
		}
	}

	// Create OpenGL 4.5 Core Profile shared context
	int context_attribs[] = {
		GLX_CONTEXT_MAJOR_VERSION_ARB, 4,
		GLX_CONTEXT_MINOR_VERSION_ARB, 5,
		GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
		0
	};

	s_glx_shared_context = _glXCreateContextAttribsARB(
		s_glx_display, chosen_config, s_glx_frontend_context, 1 /*direct*/, context_attribs);

	XFree(configs);

	if (s_glx_shared_context)
	{
		if (log_cb)
			log_cb(RETRO_LOG_INFO, "Cemu: Created shared GL 4.5 context for GPU thread: %p\n", s_glx_shared_context);
	}
	else
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "Cemu: Failed to create shared GL context\n");
	}
}

static void libretro_context_reset()
{
	s_hw_render_initialized = true;

	// Reset frontend GL objects - context was recreated, old objects are invalid
	s_frontend_read_fbo = 0;
	s_frontend_read_rbo_attached = 0;
	s_frontend_upload_tex = 0;

	// Create or update shared GL context for GPU thread (OpenGL only)
	if (s_graphics_api == SelectedGraphicsAPI::OpenGL)
	{
		if (!s_glx_shared_context)
		{
			libretro_create_shared_gl_context();
		}
		else
		{
			// Context restored (e.g. fullscreen toggle) - update drawable for MakeCurrent
			s_glx_display = glXGetCurrentDisplay();
			s_glx_drawable = glXGetCurrentDrawable();
			s_gpu_context_made_current = false;
			if (log_cb)
				log_cb(RETRO_LOG_INFO, "Cemu: Context restored, updating drawable=0x%lx\n",
					(unsigned long)s_glx_drawable);
		}
	}

	// Only create renderer on first call - subsequent calls are context restores
	if (!g_renderer)
	{
#ifdef ENABLE_VULKAN
		if (s_graphics_api == SelectedGraphicsAPI::Vulkan)
		{
			// Get Vulkan HW render interface from RetroArch
			const struct retro_hw_render_interface* iface = nullptr;
			if (environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) && iface &&
				iface->interface_type == RETRO_HW_RENDER_INTERFACE_VULKAN)
			{
				s_vk_interface = (const struct retro_hw_render_interface_vulkan*)iface;
				if (log_cb)
					log_cb(RETRO_LOG_INFO, "Cemu: Got Vulkan HW render interface (device=%p queue=%p)\n",
						(void*)s_vk_interface->device, (void*)s_vk_interface->queue);

				// Create VulkanRenderer using the shared device
				auto vkRenderer = new VulkanRenderer(
					s_vk_interface->instance,
					s_vk_interface->gpu,
					s_vk_interface->device,
					s_vk_interface->queue,
					s_vk_interface->queue_index);
				g_renderer.reset(vkRenderer);

				// Create presentation image
				vkRenderer->CreatePresentationImage(SCREEN_WIDTH, SCREEN_HEIGHT);

				if (log_cb)
					log_cb(RETRO_LOG_INFO, "Cemu: VulkanRenderer created with shared device\n");
			}
			else
			{
				if (log_cb)
					log_cb(RETRO_LOG_ERROR, "Cemu: Failed to get Vulkan HW render interface\n");
			}
		}
		else
#endif
		{
			s_gl_callbacks = std::make_unique<LibretroGLCanvasCallbacks>();
			g_renderer = std::make_unique<OpenGLRenderer>();
		}
	}

	// Set window info
	auto& windowInfo = WindowSystem::GetWindowInfo();
	windowInfo.width = SCREEN_WIDTH;
	windowInfo.height = SCREEN_HEIGHT;
	windowInfo.phys_width = SCREEN_WIDTH;
	windowInfo.phys_height = SCREEN_HEIGHT;
	windowInfo.dpi_scale = 1.0;
	windowInfo.app_active = true;

	// Launch game in a separate thread so we don't block the frontend render loop
	// (loading from rclone/network can take a long time)
	if (!s_launch_thread_running && !s_emu_initialized)
	{
		s_launch_thread_running = true;
		std::thread([]() {
			libretro_launch_game();
			s_launch_thread_running = false;
		}).detach();
	}
}

static void libretro_context_destroy()
{
	s_hw_render_initialized = false;
	s_frontend_read_fbo = 0;
	s_frontend_read_rbo_attached = 0;
	s_gpu_context_made_current = false;

	// Vulkan: do NOT clean up here — GPU thread may still be using Vulkan objects.
	// Cleanup happens via _exit() in retro_unload_game/retro_reset.
}

RETRO_API bool retro_load_game(const struct retro_game_info* game)
{
	if (!game || !game->path)
		return false;

	// Set up pixel format
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
	if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "XRGB8888 pixel format not supported\n");
		return false;
	}

	// Set up HW render context based on selected graphics API
#ifdef ENABLE_VULKAN
	if (s_graphics_api == SelectedGraphicsAPI::Vulkan)
	{
		// Set negotiation interface so RetroArch lets us create the VkDevice
		environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE, &s_vk_negotiation);

		s_hw_render.context_type = RETRO_HW_CONTEXT_VULKAN;
		s_hw_render.version_major = VK_API_VERSION_MAJOR(VK_API_VERSION_1_1);
		s_hw_render.version_minor = VK_API_VERSION_MINOR(VK_API_VERSION_1_1);
		s_hw_render.context_reset = libretro_context_reset;
		s_hw_render.context_destroy = libretro_context_destroy;
		s_hw_render.cache_context = false;
		s_hw_render.debug_context = false;

		if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_render))
		{
			s_use_hw_render = true;
			if (log_cb)
				log_cb(RETRO_LOG_INFO, "Cemu: Vulkan HW render context requested\n");
		}
		else
		{
			if (log_cb)
				log_cb(RETRO_LOG_WARN, "Cemu: Vulkan not supported by frontend, falling back to OpenGL\n");
			s_graphics_api = SelectedGraphicsAPI::OpenGL;
			GetConfig().graphic_api = kOpenGL;
		}
	}
	if (s_graphics_api == SelectedGraphicsAPI::OpenGL)
#endif
	{
		// OpenGL path
		s_hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
		s_hw_render.version_major = 4;
		s_hw_render.version_minor = 5;
		s_hw_render.context_reset = libretro_context_reset;
		s_hw_render.context_destroy = libretro_context_destroy;
		s_hw_render.bottom_left_origin = true;
		s_hw_render.depth = true;
		s_hw_render.stencil = true;
		s_hw_render.cache_context = true;

		// Request shared GL context so Cemu GPU thread can use it from another thread
		environ_cb(RETRO_ENVIRONMENT_SET_HW_SHARED_CONTEXT, nullptr);

		if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_render))
		{
			s_use_hw_render = true;
			if (log_cb)
				log_cb(RETRO_LOG_INFO, "Cemu: Using OpenGL 4.5 HW rendering\n");
		}
		else
		{
			// Try lower GL version
			s_hw_render.context_type = RETRO_HW_CONTEXT_OPENGL_CORE;
			s_hw_render.version_major = 4;
			s_hw_render.version_minor = 1;
			if (environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &s_hw_render))
			{
				s_use_hw_render = true;
				if (log_cb)
					log_cb(RETRO_LOG_INFO, "Cemu: Using OpenGL 4.1 HW rendering\n");
			}
			else
			{
				if (log_cb)
					log_cb(RETRO_LOG_ERROR, "Cemu: HW rendering not available - OpenGL 4.1+ required\n");
				return false;
			}
		}
	}

	// Register input descriptors
	static const struct retro_input_descriptor input_desc[] = {
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "A" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "B" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "X" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Y" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "ZL" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "ZR" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "+" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "-" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "L Stick" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "R Stick" },
		{ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X,     "Touchscreen X" },
		{ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y,     "Touchscreen Y" },
		{ 0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED, "Touchscreen Press" },
		{ 0, 0, 0, 0, NULL },
	};
	environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void*)input_desc);

	// Store game path - actual launch happens in context_reset when GL is ready
	s_game_path = game->path;

	// Vulkan: context_reset is called by RetroArch after RETRO_HW_CONTEXT_VULKAN is set up

	return true;
}

RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info* info, size_t num_info)
{
	return false;
}

RETRO_API void retro_unload_game()
{
	if (!s_game_loaded)
		return;

#ifdef ENABLE_VULKAN
	if (s_graphics_api == SelectedGraphicsAPI::Vulkan)
	{
		// Vulkan shared device cannot be cleanly unloaded
		_exit(0);
	}
#endif

	// OpenGL: signal shutdown and let threads wind down
	s_shutting_down = true;
	{
		std::lock_guard lock(s_frame_mutex);
		s_frame_ready = true;
		s_frame_cv.notify_one();
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	_exit(0);
}

RETRO_API void retro_deinit()
{
	// If unload didn't fully clean up, force exit
	if (s_emu_initialized)
		_exit(0);
	s_initialized = false;
}

// ============================================================================
// Input mapping
// ============================================================================

static void libretro_poll_input()
{
	if (!input_poll_cb || !input_state_cb)
		return;

	input_poll_cb();

	auto& state = s_input_state;

	// Map libretro joypad buttons to Wii U GamePad
	// Debug: log any button press
	static int s_poll_log = 0;
	int16_t a_val = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
	if (a_val && s_poll_log++ < 10 && log_cb)
		log_cb(RETRO_LOG_INFO, "Cemu: input_state_cb JOYPAD_B=%d\n", a_val);
	state.buttons[VPADController::kButtonId_A] = a_val; // B = Wii U A (east button)
	state.buttons[VPADController::kButtonId_B] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A); // A = Wii U B (south button)
	state.buttons[VPADController::kButtonId_X] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y); // Y = Wii U X (north button)
	state.buttons[VPADController::kButtonId_Y] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X); // X = Wii U Y (west button)

	state.buttons[VPADController::kButtonId_L] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
	state.buttons[VPADController::kButtonId_R] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
	state.buttons[VPADController::kButtonId_ZL] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
	state.buttons[VPADController::kButtonId_ZR] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);

	state.buttons[VPADController::kButtonId_Plus] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
	state.buttons[VPADController::kButtonId_Minus] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);

	state.buttons[VPADController::kButtonId_Up] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
	state.buttons[VPADController::kButtonId_Down] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
	state.buttons[VPADController::kButtonId_Left] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
	state.buttons[VPADController::kButtonId_Right] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);

	state.buttons[VPADController::kButtonId_StickL] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
	state.buttons[VPADController::kButtonId_StickR] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);

	// Analog sticks
	state.left_x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
	state.left_y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
	state.right_x = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
	state.right_y = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

	// Touchscreen (mouse/pointer mapped to GamePad touchscreen)
	state.touch_pressed = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED) != 0;
	if (state.touch_pressed)
	{
		state.touch_x = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X);
		state.touch_y = input_state_cb(0, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y);
	}
}

// Expose input state to the VPAD emulation
// Called from vpad.cpp VPADRead in libretro mode

bool libretro_get_button_state(uint32_t button_id)
{
	if (button_id >= 16)
		return false;
	return s_input_state.buttons[button_id] != 0;
}

void libretro_get_analog_state(float* lx, float* ly, float* rx, float* ry)
{
	*lx = s_input_state.left_x / 32767.0f;
	*ly = -(s_input_state.left_y / 32767.0f);
	*rx = s_input_state.right_x / 32767.0f;
	*ry = -(s_input_state.right_y / 32767.0f);
}

bool libretro_get_touch_state(uint16_t* x, uint16_t* y)
{
	if (!s_input_state.touch_pressed)
		return false;
	// Convert from RETRO_DEVICE_POINTER range (-0x7fff..0x7fff) to GamePad touchscreen (0..853, 0..479)
	*x = (uint16_t)(((int32_t)s_input_state.touch_x + 0x7fff) * 853 / (2 * 0x7fff));
	*y = (uint16_t)(((int32_t)s_input_state.touch_y + 0x7fff) * 479 / (2 * 0x7fff));
	return true;
}

// ============================================================================
// Main run loop
// ============================================================================

// GL function pointers for blitting in retro_run (frontend GL context)
// Use function pointers to avoid conflicts with CemuGL namespace
typedef void (*PFNGLBINDFRAMEBUFFERPROC_)(unsigned int, unsigned int);
typedef void (*PFNGLGENFRAMEBUFFERSPROC_)(int, unsigned int*);
typedef void (*PFNGLFRAMEBUFFERRENDERBUFFERPROC_)(unsigned int, unsigned int, unsigned int, unsigned int);
typedef void (*PFNGLBLITFRAMEBUFFERPROC_)(int, int, int, int, int, int, int, int, unsigned int, unsigned int);

static PFNGLBINDFRAMEBUFFERPROC_ s_glBindFramebuffer = nullptr;
static PFNGLGENFRAMEBUFFERSPROC_ s_glGenFramebuffers = nullptr;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC_ s_glFramebufferRenderbuffer = nullptr;
static PFNGLBLITFRAMEBUFFERPROC_ s_glBlitFramebuffer = nullptr;

static void libretro_load_blit_gl_funcs()
{
	if (s_glBindFramebuffer) return;
	s_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC_)glXGetProcAddress((const GLubyte*)"glBindFramebuffer");
	s_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC_)glXGetProcAddress((const GLubyte*)"glGenFramebuffers");
	s_glFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC_)glXGetProcAddress((const GLubyte*)"glFramebufferRenderbuffer");
	s_glBlitFramebuffer = (PFNGLBLITFRAMEBUFFERPROC_)glXGetProcAddress((const GLubyte*)"glBlitFramebuffer");
}

#define GL_COLOR_BUFFER_BIT_ 0x00004000
#define GL_NEAREST_ 0x2600
#define GL_COLOR_ATTACHMENT0_ 0x8CE0
#define GL_RENDERBUFFER_ 0x8D41
#define GL_DRAW_FRAMEBUFFER_ 0x8CA9

// Get the shared renderbuffer from the GPU thread's FBO
extern GLuint libretro_getBackbufferRBO();

RETRO_API void retro_run()
{
	if (!s_game_loaded)
	{
		video_cb(NULL, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
		return;
	}

	// Check if core options changed
	bool options_updated = false;
	if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) && options_updated)
		libretro_apply_core_options();

	// Poll input
	libretro_poll_input();

	// Wait for frame from GPU thread
	{
		std::unique_lock lock(s_frame_mutex);
		s_frame_cv.wait_for(lock, std::chrono::milliseconds(33), [] {
			return s_frame_ready.load();
		});
		s_frame_ready = false;
	}

#ifdef ENABLE_VULKAN
	// Vulkan: present via HW render interface
	if (s_graphics_api == SelectedGraphicsAPI::Vulkan)
	{
		if (s_vk_interface)
		{
			auto* vkRenderer = VulkanRenderer::GetInstance();
			if (vkRenderer && vkRenderer->m_presentImageView)
			{
				// Set the presentation image for RetroArch to display
				s_vk_present_image.image_view = vkRenderer->m_presentImageView;
				s_vk_present_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				s_vk_present_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
				s_vk_present_image.create_info.image = vkRenderer->m_presentImage;
				s_vk_present_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
				s_vk_present_image.create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
				s_vk_present_image.create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

				s_vk_interface->set_image(s_vk_interface->handle, &s_vk_present_image,
					0, nullptr, VK_QUEUE_FAMILY_IGNORED);
			}
		}
		video_cb(RETRO_HW_FRAME_BUFFER_VALID, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
		LibretroAudioAPI::FlushAudio(audio_batch_cb);
		return;
	}
#endif

	// OpenGL: Upload CPU framebuffer (from GPU thread's glReadPixels) to RetroArch's HW FBO
	{
		typedef void (*PFNGLGENTEXTURESPROC_)(int, unsigned int*);
		typedef void (*PFNGLBINDTEXTUREPROC_)(unsigned int, unsigned int);
		typedef void (*PFNGLTEXIMAGE2DPROC_)(unsigned int, int, int, int, int, int, unsigned int, unsigned int, const void*);
		typedef void (*PFNGLTEXPARAMETERIPROC_)(unsigned int, unsigned int, int);
		typedef void (*PFNGLENABLEPROC_)(unsigned int);
		typedef void (*PFNGLDISABLEPROC_)(unsigned int);
		typedef void (*PFNGLVIEWPORTPROC_)(int, int, int, int);
		typedef void (*PFNGLDRAWARRAYSPROC_)(unsigned int, int, int);
		typedef unsigned int (*PFNGLGETERRORPROC_)();

		static PFNGLGENTEXTURESPROC_ _glGenTextures = nullptr;
		static PFNGLBINDTEXTUREPROC_ _glBindTexture = nullptr;
		static PFNGLTEXIMAGE2DPROC_ _glTexImage2D = nullptr;
		static PFNGLTEXPARAMETERIPROC_ _glTexParameteri = nullptr;
		static PFNGLENABLEPROC_ _glEnable = nullptr;
		static PFNGLDISABLEPROC_ _glDisable = nullptr;
		static PFNGLVIEWPORTPROC_ _glViewport = nullptr;
		static PFNGLDRAWARRAYSPROC_ _glDrawArrays = nullptr;

		if (!_glGenTextures)
		{
			_glGenTextures = (PFNGLGENTEXTURESPROC_)glXGetProcAddress((const GLubyte*)"glGenTextures");
			_glBindTexture = (PFNGLBINDTEXTUREPROC_)glXGetProcAddress((const GLubyte*)"glBindTexture");
			_glTexImage2D = (PFNGLTEXIMAGE2DPROC_)glXGetProcAddress((const GLubyte*)"glTexImage2D");
			_glTexParameteri = (PFNGLTEXPARAMETERIPROC_)glXGetProcAddress((const GLubyte*)"glTexParameteri");
			_glEnable = (PFNGLENABLEPROC_)glXGetProcAddress((const GLubyte*)"glEnable");
			_glDisable = (PFNGLDISABLEPROC_)glXGetProcAddress((const GLubyte*)"glDisable");
			_glViewport = (PFNGLVIEWPORTPROC_)glXGetProcAddress((const GLubyte*)"glViewport");
			_glDrawArrays = (PFNGLDRAWARRAYSPROC_)glXGetProcAddress((const GLubyte*)"glDrawArrays");
		}

		if (!s_glBindFramebuffer) libretro_load_blit_gl_funcs();

		// Bind RetroArch's FBO
		uintptr_t ra_fbo = s_hw_render.get_current_framebuffer();
		s_glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, (GLuint)ra_fbo);

		// Create/recreate upload texture after context reset
		if (s_frontend_upload_tex == 0 && _glGenTextures)
			_glGenTextures(1, &s_frontend_upload_tex);

		if (s_frontend_upload_tex && _glBindTexture && _glTexImage2D)
		{
			_glBindTexture(0x0DE1 /*GL_TEXTURE_2D*/, s_frontend_upload_tex);
			// Upload flipped (OpenGL is bottom-up, framebuffer is top-down from glReadPixels)
			_glTexImage2D(0x0DE1, 0, 0x8058 /*GL_RGBA8*/, SCREEN_WIDTH, SCREEN_HEIGHT, 0,
				0x80E1 /*GL_BGRA*/, 0x1401 /*GL_UNSIGNED_BYTE*/, s_framebuffer.data());
			_glTexParameteri(0x0DE1, 0x2801 /*GL_TEXTURE_MIN_FILTER*/, 0x2600 /*GL_NEAREST*/);
			_glTexParameteri(0x0DE1, 0x2800 /*GL_TEXTURE_MAG_FILTER*/, 0x2600 /*GL_NEAREST*/);

			// Draw fullscreen quad using texture
			// In Core Profile we need a shader, but we can use glBlitFramebuffer from a texture-attached FBO instead
			if (s_frontend_read_fbo == 0 && s_glGenFramebuffers)
				s_glGenFramebuffers(1, &s_frontend_read_fbo);

			if (s_frontend_read_fbo && s_glBlitFramebuffer)
			{
				// Attach texture to read FBO
				typedef void (*PFNGLFRAMEBUFFERTEXTURE2DPROC_)(unsigned int, unsigned int, unsigned int, unsigned int, int);
				static PFNGLFRAMEBUFFERTEXTURE2DPROC_ _glFramebufferTexture2D = nullptr;
				if (!_glFramebufferTexture2D)
					_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC_)glXGetProcAddress((const GLubyte*)"glFramebufferTexture2D");

				s_glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, s_frontend_read_fbo);
				if (_glFramebufferTexture2D)
					_glFramebufferTexture2D(GL_READ_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_, 0x0DE1 /*GL_TEXTURE_2D*/, s_frontend_upload_tex, 0);

				// Blit directly (no flip needed - glReadPixels already gives bottom-up which matches RetroArch)
				s_glBlitFramebuffer(
					0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,    // src
					0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,    // dst
					GL_COLOR_BUFFER_BIT_, GL_NEAREST_);

				s_glBindFramebuffer(GL_READ_FRAMEBUFFER_EXT, 0);
			}
		}
	}

	video_cb(RETRO_HW_FRAME_BUFFER_VALID, SCREEN_WIDTH, SCREEN_HEIGHT, 0);

	// Flush audio
	LibretroAudioAPI::FlushAudio(audio_batch_cb);
}

// ============================================================================
// Save states (not supported yet)
// ============================================================================

RETRO_API size_t retro_serialize_size()
{
	return 0; // Save states not supported
}

RETRO_API bool retro_serialize(void* data, size_t size)
{
	return false;
}

RETRO_API bool retro_unserialize(const void* data, size_t size)
{
	return false;
}

// ============================================================================
// Cheats (not supported)
// ============================================================================

RETRO_API void retro_cheat_reset() {}
RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char* code) {}

// ============================================================================
// Memory access
// ============================================================================

RETRO_API unsigned retro_get_region()
{
	return RETRO_REGION_NTSC;
}

RETRO_API void* retro_get_memory_data(unsigned id)
{
	return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	return 0;
}
