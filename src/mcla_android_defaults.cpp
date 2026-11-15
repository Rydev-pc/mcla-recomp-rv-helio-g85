// Defaults de Android para Helio G85 / Mali-G52 MC2 (4 GB RAM).
// Estratégia copiada do Skate 3 Mobile: detecção de RAM em tempo de execução
// e redução automática de orçamentos de textura/malha, sem reescrever a
// renderização Vulkan — o System Driver do Mali é mantido e apenas os
// orçamentos e perfis de qualidade são ajustados.

#include "mcla_android_defaults.h"

#if defined(__ANDROID__)
#include <sys/sysinfo.h>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace {

uint64_t AndroidTotalRamMb() {
  struct sysinfo info = {};
  return sysinfo(&info) == 0 ? (uint64_t(info.totalram) * uint64_t(info.mem_unit)) >> 20 : 0;
}

// Dispositivos com menos de 4 GB de RAM são considerados low-end: a memória,
// não a nitidez, é o limitador.
bool AndroidIsLowEnd() {
  return AndroidTotalRamMb() != 0 && AndroidTotalRamMb() < 4000;
}

// Separa núcleos rápidos e lentos (big.LITTLE) lendo a frequência máxima de
// cada núcleo. Retorna string vazia se não houver clusters distintos.
std::string AndroidCoreList(bool fast) {
  std::map<uint64_t, std::vector<int>> by_ceiling;
  for (int cpu = 0; cpu < 64; ++cpu) {
    std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                       "/cpufreq/cpuinfo_max_freq";
    std::ifstream f(path);
    uint64_t khz = 0;
    if (!f || !(f >> khz) || khz == 0) {
      continue;
    }
    by_ceiling[khz].push_back(cpu);
  }
  if (by_ceiling.size() < 2) {
    return {};  // um único cluster: não definir afinidade
  }
  // "Fast" = todos os núcleos exceto o cluster mais lento.
  std::vector<int> cores;
  for (auto it = by_ceiling.begin(); it != by_ceiling.end(); ++it) {
    const bool is_slowest = it == by_ceiling.begin();
    if (fast != is_slowest) {
      cores.insert(cores.end(), it->second.begin(), it->second.end());
    }
  }
  std::string out;
  for (size_t i = 0; i < cores.size(); ++i) {
    if (i != 0) {
      out += "+";
    }
    out += std::to_string(cores[i]);
  }
  return out;
}

std::string AndroidThreadPlacement() {
  const std::string fast = AndroidCoreList(true);
  const std::string slow = AndroidCoreList(false);
  if (fast.empty() || slow.empty()) {
    return {};
  }
  const std::string f = "cpu:" + fast;
  const std::string s = "cpu:" + slow;
  return
      // Threads críticas do frame ficam nos núcleos rápidos.
      "Main XThread=" + f + ";GPU Commands=" + f + ";render_thread=" + f +
      ";Kernel Dispatch=" + f +
      // Áudio também precisa de núcleos rápidos.
      ";Audio Worker=" + f + ";RwAudioCore=" + f +
      // Threads de streaming/background ficam nos núcleos lentos, com nice baixo.
      ";XMA Decoder=" + s + ",nice:5;rwfilesys=" + s + ",nice:5;load_thread=" + s +
      ",nice:5;presence_thread=" + s + ",nice:10";
}

struct AndroidStoreBudgets {
  uint32_t tex_mb;
  uint32_t mesh_mb;
  const char* tier;
};

AndroidStoreBudgets PickAndroidStoreBudgets() {
  const uint64_t total_mb = AndroidTotalRamMb();
  AndroidStoreBudgets b;
  if (total_mb == 0) {
    b = {256, 192, "unknown"};
  } else if (total_mb >= 6000) {
    b = {256, 192, "standard (6 GB+)"};
  } else if (total_mb >= 4000) {
    b = {192, 144, "medium (4-6 GB)"};
  } else {
    // Dispositivos com menos de 4 GB: reduzir orçamentos para caber na RAM.
    b = {128, 96, "small (under 4 GB)"};
  }
  std::fprintf(stderr, "store budgets: %s tier (%llu MB RAM) -> tex %u MB, mesh %u MB\n",
               b.tier, (unsigned long long)total_mb, b.tex_mb, b.mesh_mb);
  std::fflush(stderr);
  return b;
}

}  // namespace

void MclaApplyAndroidDefaults() {
#if defined(__ANDROID__)
  // Defaults base (válidos para todos os dispositivos Android).
  constexpr std::pair<const char*, const char*> kDefaults[] = {
      {"vulkan_pipeline_cache_file", "true"},
      {"vulkan_async_skip_incomplete_frames_max_ms", "40"},
      {"mcla_render_height", "720"},
      {"skate3_native_render_scene_handheld_potato", "false"},
      {"vsync", "true"},
      {"log_flush_interval", "1"},
      {"log_flush_level", "warn"},
      {"vulkan_spirv_optimize", "true"},
      {"texture_cache_memory_limit_soft", "256"},
      {"texture_cache_memory_limit_hard", "384"},
      {"texture_cache_memory_limit_soft_lifetime", "30"},
      {"texture_cache_memory_limit_render_to_texture", "24"},
      {"resolution_scale", "1"},
      {"draw_resolution_scale_x", "1"},
      {"draw_resolution_scale_y", "1"},
      {"gpu_wait_reg_mem_timeout_ms", "20"},
      {"gpu_idle_spin_iterations", "32"},
      {"rtl_critical_section_max_spin", "256"},
      {"audio_device_channels", "2"},
      {"audio_device_sample_frames", "512"},
  };
  for (const auto& [name, value] : kDefaults) {
    if (!rex::cvar::SetFlagByName(name, value)) {
      REXLOG_WARN("Android default could not be applied: {}={}", name, value);
    } else {
      REXLOG_INFO("Android default: {} = {}", name, value);
    }
  }

  // Se for low-end (< 4 GB RAM), aplicar reduções agressivas.
  if (AndroidIsLowEnd()) {
    rex::cvar::SetFlagByName("mcla_vulkan_mali_relax_vertex_store", "true");
    rex::cvar::SetFlagByName("edram_tiles", "2048");
    rex::cvar::SetFlagByName("gpu_memory_pressure_percent", "75");
    rex::cvar::SetFlagByName("render_target_max_height", "720");
    rex::cvar::SetFlagByName("anisotropic_override", "1");
    rex::cvar::SetFlagByName("mcla_render_guest_static_refresh", "2");
    rex::cvar::SetFlagByName("mcla_render_lw_refresh", "2");
    REXLOG_INFO("Android low-end profile applied (< 4 GB RAM)");
  } else {
    rex::cvar::SetFlagByName("edram_tiles", "4096");
    rex::cvar::SetFlagByName("gpu_memory_pressure_percent", "90");
    rex::cvar::SetFlagByName("anisotropic_override", "5");
    REXLOG_INFO("Android standard profile applied (>= 4 GB RAM)");
  }

  // Orçamentos de textura e malha baseados na quantidade de RAM.
  const auto budgets = PickAndroidStoreBudgets();
  rex::cvar::SetFlagByName("texture_cache_memory_limit_soft",
                           std::to_string(budgets.tex_mb));
  rex::cvar::SetFlagByName("texture_cache_memory_limit_hard",
                           std::to_string(budgets.tex_mb + 128));
  rex::cvar::SetFlagByName("mesh_store_mb", std::to_string(budgets.mesh_mb));

  // Afinidade de threads para big.LITTLE.
  const std::string placement = AndroidThreadPlacement();
  if (!placement.empty()) {
    rex::cvar::SetFlagByName("android_thread_placement_map", placement);
    REXLOG_INFO("Android thread placement: {}", placement);
  }

  REXLOG_INFO("MCLA Android defaults applied (Helio G85 / Mali-G52 profile)");
#endif
}
