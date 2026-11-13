#include "pl/runtime/GameHooks.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "pl/Logger.hpp"
#include "pl/memory/Hook.hpp"
#include "pl/memory/Signature.hpp"
#include "pl/memory/Vtable.hpp"
#include "pl/runtime/GameHookRules.h"

namespace pl::runtime {
namespace {

std::atomic_bool g_isPauseMenuOpen{false};
std::atomic_bool g_isHudScreenOpen{false};
std::atomic_bool g_isShowingMenu{false};
std::atomic_bool g_forceGlobalModMenu{false};

#if defined(__arm__) && !defined(__aarch64__)

// ARMv7 Bedrock 1.26.62.x has no usable AArch64 instruction signatures in
// the launcher's bundled rules. Read the relevant functions from the exact
// ClientInstance RTTI vtable instead. These slots were checked against the
// patched 32-bit libminecraftpe.so used by the target launcher:
//   150 = IClientInstance::isShowingMenu()
//   151 = IClientInstance::isShowingPauseScreen()
//   158 = IClientInstance::isInWorldAndNotShowingAnyMenuScreens()
//
// The final method is the authoritative gameplay/HUD visibility predicate:
// it is true only while a world is active and no menu screen is on top.

std::mutex g_armV7HookMutex;
bool g_armV7HooksReady = false;

using ClientInstanceBoolFn = bool (*)(void *);

ClientInstanceBoolFn orig_isShowingMenu = nullptr;
ClientInstanceBoolFn orig_isShowingPauseScreen = nullptr;
ClientInstanceBoolFn orig_isInWorldAndNotShowingAnyMenuScreens = nullptr;

pl::memory::HookHandle g_isShowingMenuHook;
pl::memory::HookHandle g_isShowingPauseScreenHook;
pl::memory::HookHandle g_isInWorldNoMenusHook;

bool hook_isShowingMenu(void *_this) {
  const bool result = orig_isShowingMenu ? orig_isShowingMenu(_this) : false;
  g_isShowingMenu.store(result, std::memory_order_relaxed);
  return result;
}

bool hook_isShowingPauseScreen(void *_this) {
  const bool result = orig_isShowingPauseScreen
                          ? orig_isShowingPauseScreen(_this)
                          : false;
  g_isPauseMenuOpen.store(result, std::memory_order_relaxed);
  return result;
}

bool hook_isInWorldAndNotShowingAnyMenuScreens(void *_this) {
  const bool result = orig_isInWorldAndNotShowingAnyMenuScreens
                          ? orig_isInWorldAndNotShowingAnyMenuScreens(_this)
                          : false;
  g_isHudScreenOpen.store(result, std::memory_order_relaxed);
  return result;
}

void InitArmV7GameHooks() {
  std::lock_guard<std::mutex> lock(g_armV7HookMutex);
  if (g_armV7HooksReady) {
    return;
  }

  constexpr const char *kMinecraftLibrary = "libminecraftpe.so";
  const uintptr_t isShowingMenu = pl::memory::resolveVtableFunction(
      "14ClientInstance", 150, kMinecraftLibrary);
  const uintptr_t isShowingPauseScreen = pl::memory::resolveVtableFunction(
      "14ClientInstance", 151, kMinecraftLibrary);
  const uintptr_t isInWorldNoMenus = pl::memory::resolveVtableFunction(
      "14ClientInstance", 158, kMinecraftLibrary);

  if (!isShowingMenu || !isShowingPauseScreen || !isInWorldNoMenus) {
    preloaderLogger.warn(
        "ARMv7 ClientInstance state hooks could not resolve; HUD overlays "
        "will remain hidden (isShowingMenu={}, isShowingPauseScreen={}, "
        "isInWorldAndNotShowingAnyMenuScreens={})",
        isShowingMenu != 0, isShowingPauseScreen != 0, isInWorldNoMenus != 0);
    return;
  }

  // Keep temporary RAII handles until every hook has installed. If any hook
  // fails, their destructors roll back the hooks already installed.
  pl::memory::HookHandle showingMenuHook(
      reinterpret_cast<pl::memory::FuncPtr>(isShowingMenu),
      reinterpret_cast<pl::memory::FuncPtr>(&hook_isShowingMenu),
      reinterpret_cast<pl::memory::FuncPtr *>(&orig_isShowingMenu));
  pl::memory::HookHandle showingPauseScreenHook(
      reinterpret_cast<pl::memory::FuncPtr>(isShowingPauseScreen),
      reinterpret_cast<pl::memory::FuncPtr>(&hook_isShowingPauseScreen),
      reinterpret_cast<pl::memory::FuncPtr *>(&orig_isShowingPauseScreen));
  pl::memory::HookHandle inWorldNoMenusHook(
      reinterpret_cast<pl::memory::FuncPtr>(isInWorldNoMenus),
      reinterpret_cast<pl::memory::FuncPtr>(
          &hook_isInWorldAndNotShowingAnyMenuScreens),
      reinterpret_cast<pl::memory::FuncPtr *>(
          &orig_isInWorldAndNotShowingAnyMenuScreens));

  if (!showingMenuHook.installed() || !showingPauseScreenHook.installed() ||
      !inWorldNoMenusHook.installed()) {
    preloaderLogger.warn(
        "ARMv7 ClientInstance state-hook installation was incomplete; "
        "rolling back");
    return;
  }

  g_isShowingMenuHook = std::move(showingMenuHook);
  g_isShowingPauseScreenHook = std::move(showingPauseScreenHook);
  g_isInWorldNoMenusHook = std::move(inWorldNoMenusHook);

  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  g_isHudScreenOpen.store(false, std::memory_order_relaxed);
  g_isShowingMenu.store(false, std::memory_order_relaxed);
  g_armV7HooksReady = true;

  preloaderLogger.info(
      "Installed ARMv7 ClientInstance state hooks (menu, pause, "
      "in-world-without-menu)");
}

#else

std::once_flag g_gameHooksOnce;

void (*orig_PauseMenuDtor)(void *) = nullptr;
void hook_PauseMenuDtor(void *_this) {
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  if (orig_PauseMenuDtor) {
    orig_PauseMenuDtor(_this);
  }
}

void (*orig_PauseMenuOpen)(void *) = nullptr;
void hook_PauseMenuOpen(void *_this) {
  g_isPauseMenuOpen.store(true, std::memory_order_relaxed);
  g_isShowingMenu.store(true, std::memory_order_relaxed);
  if (orig_PauseMenuOpen) {
    orig_PauseMenuOpen(_this);
  }
}

void (*orig_HudScreenDtor)(void *) = nullptr;
void hook_HudScreenDtor(void *_this) {
  g_isHudScreenOpen.store(false, std::memory_order_relaxed);
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  g_isShowingMenu.store(false, std::memory_order_relaxed);
  if (orig_HudScreenDtor) {
    orig_HudScreenDtor(_this);
  }
}

void (*orig_HudScreenOpen)(void *) = nullptr;
void hook_HudScreenOpen(void *_this) {
  g_isHudScreenOpen.store(true, std::memory_order_relaxed);
  g_isPauseMenuOpen.store(false, std::memory_order_relaxed);
  g_isShowingMenu.store(false, std::memory_order_relaxed);
  if (orig_HudScreenOpen) {
    orig_HudScreenOpen(_this);
  }
}

bool (*orig_isShowingMenu)(void *) = nullptr;
bool hook_isShowingMenu(void *_this) {
  bool res = false;
  if (orig_isShowingMenu) {
    res = orig_isShowingMenu(_this);
  }
  g_isShowingMenu.store(res, std::memory_order_relaxed);
  return res;
}

uintptr_t ResolveResult(
    const std::unordered_map<std::string, uintptr_t> &results,
    const std::string &signature) {
  auto it = results.find(signature);
  return it == results.end() ? 0 : it->second;
}

bool InstallHook(uintptr_t target, pl::memory::FuncPtr detour,
                 pl::memory::FuncPtr *original,
                 const char *name) {
  if (!target) {
    preloaderLogger.warn("Preloader hook target is missing: {}", name);
    return false;
  }

  if (pl::memory::hook(reinterpret_cast<pl::memory::FuncPtr>(target), detour,
                       original) != 0) {
    preloaderLogger.warn("Failed to install Preloader hook: {}", name);
    return false;
  }
  return true;
}

#endif

} // namespace

void ConfigureGameHooks(std::string rulesPath, std::string minecraftVersion) {
  ConfigureGameHookRules(std::move(rulesPath), std::move(minecraftVersion));
  g_forceGlobalModMenu.store(false, std::memory_order_relaxed);
}

void InitGameHooks() {
#if defined(__arm__) && !defined(__aarch64__)
  // Unlike the signature path, this can be called early and safely retried
  // after libminecraftpe.so has been loaded.
  InitArmV7GameHooks();
#else
  std::call_once(g_gameHooksOnce, [] {
    g_forceGlobalModMenu.store(false, std::memory_order_relaxed);
    auto signatures = LoadConfiguredGameHookSignatures();
    if (!signatures) {
      return;
    }

    std::vector<std::string> requestedSignatures{
        signatures->pauseMenuDtor, signatures->pauseMenuOpen,
        signatures->hudScreenDtor, signatures->hudScreenOpen};
    if (!signatures->isShowingMenu.empty()) {
      requestedSignatures.push_back(signatures->isShowingMenu);
    }
    auto results = pl::memory::resolveSignatures(
        requestedSignatures, "libminecraftpe.so");

    uintptr_t pauseDtor = ResolveResult(results, signatures->pauseMenuDtor);
    uintptr_t pauseOpen = ResolveResult(results, signatures->pauseMenuOpen);
    uintptr_t hudDtor = ResolveResult(results, signatures->hudScreenDtor);
    uintptr_t hudOpen = ResolveResult(results, signatures->hudScreenOpen);
    uintptr_t isShowingMenuAddr = 0;
    if (signatures->isShowingMenuVtableIndex) {
      isShowingMenuAddr = pl::memory::resolveVtableFunction(
          "14ClientInstance", *signatures->isShowingMenuVtableIndex,
          "libminecraftpe.so");
    } else if (!signatures->isShowingMenu.empty()) {
      isShowingMenuAddr = ResolveResult(results, signatures->isShowingMenu);
    }

    if (!pauseDtor || !pauseOpen || !hudDtor || !hudOpen ||
        !isShowingMenuAddr) {
      preloaderLogger.warn(
          "Preloader runtime data is incomplete; game-only overlays remain hidden");
      return;
    }

    bool hooksReady = true;
    hooksReady &= InstallHook(pauseDtor,
                              (pl::memory::FuncPtr)hook_PauseMenuDtor,
                              (pl::memory::FuncPtr *)&orig_PauseMenuDtor,
                              "PauseMenuDtor");
    hooksReady &= InstallHook(pauseOpen,
                              (pl::memory::FuncPtr)hook_PauseMenuOpen,
                              (pl::memory::FuncPtr *)&orig_PauseMenuOpen,
                              "PauseMenuOpen");
    hooksReady &= InstallHook(hudDtor,
                              (pl::memory::FuncPtr)hook_HudScreenDtor,
                              (pl::memory::FuncPtr *)&orig_HudScreenDtor,
                              "HudScreenDtor");
    hooksReady &= InstallHook(hudOpen,
                              (pl::memory::FuncPtr)hook_HudScreenOpen,
                              (pl::memory::FuncPtr *)&orig_HudScreenOpen,
                              "HudScreenOpen");
    hooksReady &= InstallHook(isShowingMenuAddr,
                              (pl::memory::FuncPtr)hook_isShowingMenu,
                              (pl::memory::FuncPtr *)&orig_isShowingMenu,
                              "isShowingMenu");

    if (!hooksReady) {
      preloaderLogger.warn(
          "Preloader runtime hooks are incomplete; game-only overlays remain hidden");
      return;
    }
  });
#endif
}

bool IsPauseMenuOpen() {
  return g_isPauseMenuOpen.load(std::memory_order_relaxed);
}

bool IsHudScreenOpen() {
  return g_isHudScreenOpen.load(std::memory_order_relaxed);
}

bool IsShowingMenu() {
  return g_isShowingMenu.load(std::memory_order_relaxed);
}

bool ShouldForceGlobalModMenu() {
  return g_forceGlobalModMenu.load(std::memory_order_relaxed);
}

} // namespace pl::runtime
