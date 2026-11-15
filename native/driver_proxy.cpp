// Source-built Vulkan dispatch adapter for the unchanged MCLA game engine.
// It neither patches libmain.so nor hooks dlopen/dlsym process-wide.
//
// Supported driver modes:
//   - "system": forwards to /system/lib64/libvulkan.so (recommended for Mali/Helio G85)
//   - "custom:NAME": loads a driver package unpacked in drivers/NAME
//
// Ported from andrewnakas/skate3-android and aliss32/mcla-recomp-android.

#include <jni.h>
#include <android/log.h>
#include <adrenotools/driver.h>
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <limits.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#define EXPORT extern "C" __attribute__((visibility("default")))

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks*);

namespace {
constexpr const char* kTag = "MclaDriver";
constexpr uint32_t kMaxPhysicalDevices = 16;
std::mutex g_init_mutex;
std::atomic<bool> g_ready{false};
std::atomic<uint32_t> g_game_instances{0};
bool g_attempted = false;
bool g_custom = false;
std::string g_system_identity;
void* g_loader = nullptr;
PFN_vkGetInstanceProcAddr g_get = nullptr;
PFN_vkDestroyInstance g_destroy = nullptr;
std::string g_arguments;
std::string g_status = "{\"ok\":false,\"error\":\"Driver bridge not initialized\"}";

std::string Quote(const std::string& value) {
  std::string out = "\"";
  for (unsigned char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          constexpr char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += "\"";
  return out;
}

std::string JavaString(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* bytes = env->GetStringUTFChars(value, nullptr);
  if (!bytes) return {};
  std::string result(bytes);
  env->ReleaseStringUTFChars(value, bytes);
  return result;
}

std::string CanonicalDirectory(const std::string& value) {
  char resolved[PATH_MAX];
  struct stat st{};
  if (value.empty() || value.front() != '/' || !realpath(value.c_str(), resolved) ||
      stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return {};
  return std::string(resolved) + "/";
}

bool RegularFile(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string LoaderPath() {
  Dl_info info{};
  if (g_get && dladdr(reinterpret_cast<void*>(g_get), &info) && info.dli_fname)
    return info.dli_fname;
  return {};
}

std::string Failure(const std::string& why) {
  __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", why.c_str());
  return "{\"ok\":false,\"mode\":" + Quote(g_custom ? "custom" : "system") +
         ",\"error\":" + Quote(why) + ",\"loader_path\":" + Quote(LoaderPath()) + "}";
}

template<typename Fn>
Fn Get(VkInstance instance, const char* name) {
  return reinterpret_cast<Fn>(g_get(instance, name));
}

std::string DeviceIdentity(const VkPhysicalDeviceProperties& basic,
                           const VkPhysicalDeviceDriverProperties& driver) {
  return std::to_string(driver.driverID) + "|" + std::to_string(basic.driverVersion) + "|" +
         std::string(driver.driverName) + "|" + std::string(driver.driverInfo) + "|" +
         std::string(basic.deviceName);
}

uint32_t ProbeApiVersion(uint32_t loader_api) {
  return loader_api >= VK_API_VERSION_1_1 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_0;
}

bool ProbeSystemIdentity(std::string& identity) {
  void* handle = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!handle) return false;
  const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(handle, "vkGetInstanceProcAddr"));
  if (!get) { dlclose(handle); return false; }
  const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(VK_NULL_HANDLE, "vkCreateInstance"));
  const auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      get(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
  if (!create) { dlclose(handle); return false; }
  uint32_t api = VK_API_VERSION_1_0;
  if (enumerate_version && enumerate_version(&api) != VK_SUCCESS) api = VK_API_VERSION_1_0;
  api = ProbeApiVersion(api);

  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "MCLA System Driver Probe";
  app.apiVersion = api;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  if (create(&info, nullptr, &instance) != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    dlclose(handle);
    return false;
  }

  bool found = false;
  const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
      get(instance, "vkEnumeratePhysicalDevices"));
  const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      get(instance, "vkGetPhysicalDeviceProperties"));
  const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
      get(instance, "vkGetPhysicalDeviceProperties2"));
  uint32_t count = 0;
  if (enumerate && properties && enumerate(instance, &count, nullptr) == VK_SUCCESS &&
      count > 0 && count <= kMaxPhysicalDevices) {
    std::vector<VkPhysicalDevice> devices(count);
    if (enumerate(instance, &count, devices.data()) == VK_SUCCESS && count > 0) {
      VkPhysicalDeviceProperties basic{};
      properties(devices[0], &basic);
      VkPhysicalDeviceDriverProperties driver{};
      driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
      if (api >= VK_API_VERSION_1_1 && basic.apiVersion >= VK_API_VERSION_1_2 && properties2) {
        VkPhysicalDeviceProperties2 extended{};
        extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        extended.pNext = &driver;
        properties2(devices[0], &extended);
      }
      identity = DeviceIdentity(basic, driver);
      found = true;
    }
  }
  const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
  if (destroy) destroy(instance, nullptr);
  dlclose(handle);
  return found;
}

bool VerifyInstance(VkInstance instance, uint32_t requested_api,
                    std::string& devices_json, std::string& error) {
  const auto enumerate = Get<PFN_vkEnumeratePhysicalDevices>(instance, "vkEnumeratePhysicalDevices");
  const auto properties = Get<PFN_vkGetPhysicalDeviceProperties>(instance, "vkGetPhysicalDeviceProperties");
  const auto properties2 = Get<PFN_vkGetPhysicalDeviceProperties2>(instance, "vkGetPhysicalDeviceProperties2");
  if (!enumerate || !properties) {
    error = "Selected loader is missing physical-device query functions";
    return false;
  }
  if (g_custom && (requested_api < VK_API_VERSION_1_1 || !properties2)) {
    error = "Custom driver validation requires a Vulkan 1.1 instance and the properties2 query";
    return false;
  }
  uint32_t count = 0;
  VkResult result = enumerate(instance, &count, nullptr);
  if (result != VK_SUCCESS || count == 0 || count > kMaxPhysicalDevices) {
    error = "Physical-device enumeration failed or returned an unsupported count";
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  result = enumerate(instance, &count, devices.data());
  if (result != VK_SUCCESS || count == 0 || count > devices.size()) {
    error = "Physical-device list changed or could not be read";
    return false;
  }
  devices_json = "[";
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties basic{};
    properties(devices[i], &basic);
    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    if (requested_api >= VK_API_VERSION_1_1 && basic.apiVersion >= VK_API_VERSION_1_2 && properties2) {
      VkPhysicalDeviceProperties2 extended{};
      extended.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
      extended.pNext = &driver;
      properties2(devices[i], &extended);
    }
    if (g_custom) {
      const std::string identity = DeviceIdentity(basic, driver);
      if (g_system_identity.empty()) {
        error = "A custom driver was selected, but this device's own driver could not be read first";
        return false;
      }
      if (identity == g_system_identity) {
        error = "The custom driver did not take effect - identical to device's own driver";
        return false;
      }
    }
    if (i) devices_json += ",";
    devices_json += "{\"name\":" + Quote(basic.deviceName) +
                    ",\"api_version\":" + std::to_string(basic.apiVersion) +
                    ",\"driver_version\":" + std::to_string(basic.driverVersion) +
                    ",\"vendor_id\":" + std::to_string(basic.vendorID) +
                    ",\"device_id\":" + std::to_string(basic.deviceID) +
                    ",\"driver_id\":" + std::to_string(driver.driverID) +
                    ",\"driver_name\":" + Quote(driver.driverName) +
                    ",\"driver_info\":" + Quote(driver.driverInfo) + "}";
  }
  devices_json += "]";
  return true;
}

bool Probe(std::string& devices, std::string& error) {
  const auto create = Get<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
  const auto version = Get<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  uint32_t api = VK_API_VERSION_1_0;
  if (!create || (version && version(&api) != VK_SUCCESS)) {
    error = "Selected Vulkan loader could not provide instance creation/version";
    return false;
  }
  if (g_custom && api < VK_API_VERSION_1_1) {
    error = "The Vulkan loader of this Android version is older than Vulkan 1.1";
    return false;
  }
  api = ProbeApiVersion(api);
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "MCLA Driver Validation";
  app.apiVersion = api;
  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  VkInstance instance = VK_NULL_HANDLE;
  const VkResult result = create(&info, nullptr, &instance);
  if (result != VK_SUCCESS || instance == VK_NULL_HANDLE) {
    error = "Selected Vulkan driver failed the startup probe (VkResult=" + std::to_string(result) + ")";
    return false;
  }
  const bool valid = VerifyInstance(instance, api, devices, error);
  g_destroy(instance, nullptr);
  return valid;
}
} // namespace

EXPORT jint JNICALL JNI_OnLoad(JavaVM*, void*) { return JNI_VERSION_1_6; }

EXPORT jstring JNICALL Java_com_mcla_recomp_DriverBridge_nativeInit(
    JNIEnv* env, jclass, jstring native_dir_value, jstring driver_dir_value,
    jstring filename_value, jboolean custom_value, jboolean fallback_value) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  const std::string native_dir_arg = JavaString(env, native_dir_value);
  const std::string driver_dir_arg = JavaString(env, driver_dir_value);
  const std::string filename = JavaString(env, filename_value);
  if (env->ExceptionCheck()) return nullptr;
  const bool custom = custom_value == JNI_TRUE;
  const bool fallback = fallback_value == JNI_TRUE;
  const std::string arguments = Quote(native_dir_arg) + Quote(driver_dir_arg) + Quote(filename) +
                                (custom ? "1" : "0") + (fallback ? "1" : "0");
  if (g_attempted) {
    const std::string response = arguments == g_arguments ? g_status :
        "{\"ok\":false,\"error\":\"Restart the application process before changing drivers\"}";
    return env->NewStringUTF(response.c_str());
  }
  g_attempted = true;
  g_arguments = arguments;
  g_custom = custom;
  const std::string native_dir = CanonicalDirectory(native_dir_arg);
  std::string error;
  if (native_dir.empty()) {
    error = "Native library directory is missing or not an absolute directory";
  } else {
    void* selected = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    const auto selected_get = selected ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(selected, "vkGetInstanceProcAddr")) : nullptr;
    if (selected_get != &vkGetInstanceProcAddr) {
      error = "The application Vulkan lookup does not resolve to the driver proxy; restart before loading the game";
    }
    if (selected) dlclose(selected);
  }
  const bool proxy_ok = error.empty();
  if (error.empty() && custom) {
    const std::string driver_dir = CanonicalDirectory(driver_dir_arg);
    char file_path[PATH_MAX];
    if (driver_dir.empty() || driver_dir.rfind("/data/", 0) != 0) {
      error = "Custom Vulkan driver must be in app-private /data storage";
    } else if (filename.empty() || filename.find('/') != std::string::npos ||
               filename == "." || filename == "..") {
      error = "Custom Vulkan driver filename is invalid";
    } else if (!realpath((driver_dir + filename).c_str(), file_path) ||
               std::string(file_path).rfind(driver_dir, 0) != 0 || !RegularFile(file_path)) {
      error = "Custom Vulkan driver file is missing or escapes its private directory";
    } else {
      if (!ProbeSystemIdentity(g_system_identity)) {
        g_system_identity.clear();
        __android_log_print(ANDROID_LOG_WARN, kTag,
                            "could not read this device's own driver identity before loading the custom one");
      }
      g_loader = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
          driver_dir.c_str(), native_dir.c_str(), driver_dir.c_str(), filename.c_str(), nullptr, nullptr);
      if (!g_loader) error = "libadrenotools could not initialize the selected custom driver";
    }
  } else if (error.empty()) {
    g_loader = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
      const char* detail = dlerror();
      error = std::string("System Vulkan loader could not open: ") + (detail ? detail : "unknown error");
    }
  }
  std::string devices;
  if (error.empty()) {
    g_get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
    g_destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(g_loader, "vkDestroyInstance"));
    if (!g_get || !g_destroy || g_get == &vkGetInstanceProcAddr || g_destroy == &vkDestroyInstance) {
      error = "Vulkan loader entrypoints are missing or resolve recursively to the proxy";
    } else if (!Probe(devices, error)) {
      // Keep the exact failure for the status below.
    }
  }
  std::string fallback_from;
  if (!error.empty() && custom && fallback && proxy_ok) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "custom driver failed (%s); trying the system driver",
                        error.c_str());
    fallback_from = error;
    error.clear();
    devices.clear();
    g_custom = false;
    g_get = nullptr;
    g_destroy = nullptr;
    g_loader = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!g_loader) {
      const char* detail = dlerror();
      error = std::string("System Vulkan loader could not open: ") + (detail ? detail : "unknown error");
    } else {
      g_get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_loader, "vkGetInstanceProcAddr"));
      g_destroy = reinterpret_cast<PFN_vkDestroyInstance>(dlsym(g_loader, "vkDestroyInstance"));
      if (!g_get || !g_destroy || g_get == &vkGetInstanceProcAddr || g_destroy == &vkDestroyInstance) {
        error = "Vulkan loader entrypoints are missing or resolve recursively to the proxy";
      } else {
        Probe(devices, error);
      }
    }
    if (!error.empty()) error = fallback_from + "; the system driver failed too: " + error;
  }
  if (!error.empty()) {
    g_status = Failure(error);
  } else {
    g_status = "{\"ok\":true,\"mode\":" + Quote(g_custom ? "custom" : "system") +
        ",\"error\":\"\",\"fallback_from\":" + Quote(fallback_from) +
        ",\"loader_path\":" + Quote(LoaderPath()) +
        ",\"devices\":" + devices + "}";
    g_ready.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kTag, "Driver initialized: %s", g_status.c_str());
  }
  return env->NewStringUTF(g_status.c_str());
}

EXPORT jstring JNICALL Java_com_mcla_recomp_DriverBridge_nativeStatus(JNIEnv* env, jclass) {
  std::lock_guard<std::mutex> lock(g_init_mutex);
  return env->NewStringUTF(g_status.c_str());
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(
    const VkInstanceCreateInfo* info, const VkAllocationCallbacks* allocator, VkInstance* instance) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto create = Get<PFN_vkCreateInstance>(VK_NULL_HANDLE, "vkCreateInstance");
  const VkResult result = create(info, allocator, instance);
  if (result != VK_SUCCESS) return result;
  const uint32_t api = info && info->pApplicationInfo && info->pApplicationInfo->apiVersion ?
      info->pApplicationInfo->apiVersion : VK_API_VERSION_1_0;
  std::string devices, error;
  if (!VerifyInstance(*instance, api, devices, error)) {
    __android_log_print(ANDROID_LOG_ERROR, kTag, "Game Vulkan instance rejected: %s", error.c_str());
    g_destroy(*instance, allocator);
    *instance = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const uint32_t count = g_game_instances.fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= 8) __android_log_print(ANDROID_LOG_INFO, kTag,
      "Game Vulkan instance %u verified: mode=%s loader=%s devices=%s", count,
      g_custom ? "custom" : "system", LoaderPath().c_str(), devices.c_str());
  return VK_SUCCESS;
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* allocator) {
  if (g_ready.load(std::memory_order_acquire)) g_destroy(instance, allocator);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* name) {
  if (!g_ready.load(std::memory_order_acquire) || !name) return nullptr;
  const auto real = g_get(instance, name);
  if (!real) return nullptr;
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkGetInstanceProcAddr);
  if (std::strcmp(name, "vkCreateInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkCreateInstance);
  if (std::strcmp(name, "vkDestroyInstance") == 0)
    return reinterpret_cast<PFN_vkVoidFunction>(&vkDestroyInstance);
  return real;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* name) {
  if (!g_ready.load(std::memory_order_acquire)) return nullptr;
  const auto fn = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_loader, "vkGetDeviceProcAddr"));
  return fn ? fn(device, name) : nullptr;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* version) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceVersion>(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
  if (fn) return fn(version);
  if (version) *version = VK_API_VERSION_1_0;
  return VK_SUCCESS;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(
    const char* layer, uint32_t* count, VkExtensionProperties* properties) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceExtensionProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
  return fn ? fn(layer, count, properties) : VK_ERROR_INITIALIZATION_FAILED;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t* count, VkLayerProperties* properties) {
  if (!g_ready.load(std::memory_order_acquire)) return VK_ERROR_INITIALIZATION_FAILED;
  const auto fn = Get<PFN_vkEnumerateInstanceLayerProperties>(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
  return fn ? fn(count, properties) : VK_ERROR_INITIALIZATION_FAILED;
}
