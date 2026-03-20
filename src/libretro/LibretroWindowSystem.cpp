// WindowSystem implementation for libretro - replaces wxWidgets-based wxWindowSystem.cpp
#include "interface/WindowSystem.h"
#include "config/ActiveSettings.h"
#include "config/CemuConfig.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/CafeSystem.h"

static WindowSystem::WindowInfo g_window_info{};

void WindowSystem::Create()
{
	// In libretro, the frontend manages the window.
	// This is a no-op since we don't create our own window.
}

void WindowSystem::ShowErrorDialog(std::string_view message, std::string_view title, std::optional<WindowSystem::ErrorCategory>)
{
	// Log errors instead of showing dialogs
	cemuLog_log(LogType::Force, "Error: {} - {}", title, message);
}

WindowSystem::WindowInfo& WindowSystem::GetWindowInfo()
{
	return g_window_info;
}

void WindowSystem::UpdateWindowTitles(bool isIdle, bool isLoading, double fps)
{
	// No-op in libretro - frontend manages window titles
}

void WindowSystem::GetWindowSize(int& w, int& h)
{
	w = g_window_info.width;
	h = g_window_info.height;
}

void WindowSystem::GetPadWindowSize(int& w, int& h)
{
	w = g_window_info.pad_width;
	h = g_window_info.pad_height;
}

void WindowSystem::GetWindowPhysSize(int& w, int& h)
{
	w = g_window_info.phys_width;
	h = g_window_info.phys_height;
}

void WindowSystem::GetPadWindowPhysSize(int& w, int& h)
{
	w = g_window_info.phys_pad_width;
	h = g_window_info.phys_pad_height;
}

double WindowSystem::GetWindowDPIScale()
{
	return g_window_info.dpi_scale;
}

double WindowSystem::GetPadDPIScale()
{
	return g_window_info.pad_dpi_scale;
}

bool WindowSystem::IsPadWindowOpen()
{
	return false; // No separate pad window in libretro
}

bool WindowSystem::IsKeyDown(uint32 key)
{
	return g_window_info.get_keystate(key);
}

bool WindowSystem::IsKeyDown(PlatformKeyCodes key)
{
	return false;
}

std::string WindowSystem::GetKeyCodeName(uint32 key)
{
	return "";
}

bool WindowSystem::InputConfigWindowHasFocus()
{
	return false;
}

void WindowSystem::NotifyGameLoaded()
{
	// No-op in libretro
}

void WindowSystem::NotifyGameExited()
{
	// No-op in libretro
}

void WindowSystem::RefreshGameList()
{
	// No-op in libretro
}

bool WindowSystem::IsFullScreen()
{
	return true; // libretro is always "fullscreen" from the core's perspective
}

void WindowSystem::CaptureInput(const ControllerState& currentState, const ControllerState& lastState)
{
	// No-op - libretro handles input capture
}
