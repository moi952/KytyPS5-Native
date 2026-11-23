#include "graphics/presentation/gameMenu.h"

#include "common/emulatorConfig.h"
#include "graphics/native_gpu/nativeShadow.h"
#include "graphics/presentation/exitVideo.h"
#include "graphics/presentation/performanceOverlay.h"
#include "libs/audio.h"
#include "imgui.h"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {

enum class Page : uint8_t { Closed, Main, Settings, Info, ConfirmExit };

enum MainItem : int { ItemResume, ItemMonitoring, ItemSettings, ItemInfo, ItemExit, MainItemCount };
enum SettingsItem : int { SettingEarly, SettingBack, SettingsItemCount };
enum ConfirmItem : int { ConfirmExitGame, ConfirmCancel, ConfirmItemCount };

struct MenuState {
	Page page     = Page::Closed;
	int  selected = 0;
};

std::mutex            g_mutex;
MenuState             g_state;
std::atomic<bool>     g_open {false};
std::atomic<uint64_t> g_revision {0};

// Channels of the host's playback device, read when the menu opens (SteamOS sets the profile, 2/6/8).
std::atomic<int> g_audio_channels {0};

std::string AudioLabel() {
	const int ch = g_audio_channels.load(std::memory_order_relaxed);
	switch (ch) {
		case 0: return "not detected";
		case 1: return "Mono (1 ch)";
		case 2: return "Stereo (2 ch)";
		case 4: return "Quad (4 ch)";
		case 6: return "5.1 (6 ch)";
		case 8: return "7.1 (8 ch)";
		default: return std::to_string(ch) + " ch";
	}
}

std::atomic<bool> g_early_present {false};

const char* PresentModeLabel(Config::PresentMode mode) {
	switch (mode) {
		case Config::PresentMode::Fifo: return "FIFO";
		case Config::PresentMode::Mailbox: return "Mailbox";
		case Config::PresentMode::Immediate: return "Immediate";
	}
	return "?";
}

// What the Info page shows, read when it opens.
std::mutex                                       g_info_mutex;
std::vector<std::pair<std::string, std::string>> g_info;

void RefreshInfo() {
	std::vector<std::pair<std::string, std::string>> rows;
	g_audio_channels.store(Audio::DetectedOutputChannels(true), std::memory_order_relaxed);
	rows.emplace_back("Audio output", AudioLabel());
	rows.emplace_back("Refresh rate", std::to_string(Config::GetVblankFrequency()) + " Hz");
	rows.emplace_back("Present mode", PresentModeLabel(Config::GetPresentMode()));
	const char* copy = std::getenv("KYTY_NATIVE_GPU_DISPLAY_COPY");
	rows.emplace_back("Display copy", copy != nullptr ? copy : "2 (default)");
	rows.emplace_back("Rendering", NativeGpu::PrimaryRendering() ? "native PS5 GPU path" : "Vulkan path");
	std::scoped_lock lock(g_info_mutex);
	g_info = std::move(rows);
}

// Window thread only: Share and Options held, for the combination.
bool g_back_down  = false;
bool g_start_down = false;

MenuState GetState() {
	std::scoped_lock lock(g_mutex);
	return g_state;
}

void SetState(MenuState state) {
	if (!g_open.load(std::memory_order_acquire) && state.page != Page::Closed) {
		g_audio_channels.store(Audio::DetectedOutputChannels(true), std::memory_order_relaxed);
	}
	{
		std::scoped_lock lock(g_mutex);
		g_state = state;
	}
	g_open.store(state.page != Page::Closed, std::memory_order_release);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

int ItemCount(Page page) {
	switch (page) {
		case Page::Settings: return SettingsItemCount;
		case Page::Info: return 1;
		case Page::ConfirmExit: return ConfirmItemCount;
		default: return MainItemCount;
	}
}

void ToggleEarly() {
	g_early_present.store(!g_early_present.load(std::memory_order_relaxed), std::memory_order_relaxed);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

const char* MonitoringLabel(int mode) {
	switch (mode) {
		case 1: return "FPS";
		case 2: return "Compact";
		case 3: return "Detailed";
		case 4: return "Machine";
		default: return "Off";
	}
}

void StepMonitoring(int step) {
	SetPerformanceOverlayMode((PerformanceOverlayMode() + step + 5) % 5);
	g_revision.fetch_add(1, std::memory_order_acq_rel);
}

void LeaveGame() {
	SDL_Event quit {};
	quit.type = SDL_EVENT_QUIT;
	SDL_PushEvent(&quit);
}

void Activate(const MenuState& state) {
	if (state.page == Page::Main) {
		switch (state.selected) {
			case ItemResume: SetState({}); break;
			case ItemMonitoring: StepMonitoring(1); break;
			case ItemSettings: SetState({Page::Settings, SettingEarly}); break;
			case ItemInfo:
				RefreshInfo();
				SetState({Page::Info, 0});
				break;
			case ItemExit: SetState({Page::ConfirmExit, ConfirmExitGame}); break;
			default: break;
		}
		return;
	}
	if (state.page == Page::Settings) {
		if (state.selected == SettingEarly) {
			ToggleEarly();
		} else {
			SetState({Page::Main, ItemSettings});
		}
		return;
	}
	if (state.page == Page::Info) {
		SetState({Page::Main, ItemInfo});
		return;
	}
	if (state.selected == ConfirmExitGame) {
		SetState({});
		MarkUserExit();
		LeaveGame();
	} else {
		SetState({Page::Main, ItemExit});
	}
}

void Back(const MenuState& state) {
	switch (state.page) {
		case Page::Settings: SetState({Page::Main, ItemSettings}); break;
		case Page::Info: SetState({Page::Main, ItemInfo}); break;
		case Page::ConfirmExit: SetState({Page::Main, ItemExit}); break;
		default: SetState({}); break;
	}
}

// Navigation shared by the gamepad and the keyboard. True when the menu took the input.
bool Navigate(int up_down, int left_right, bool accept, bool back) {
	MenuState state = GetState();
	if (state.page == Page::Closed) {
		return false;
	}
	if (up_down != 0) {
		const int count = ItemCount(state.page);
		state.selected  = (state.selected + up_down + count) % count;
		SetState(state);
	} else if (left_right != 0 && state.page == Page::Main && state.selected == ItemMonitoring) {
		StepMonitoring(left_right);
	} else if (left_right != 0 && state.page == Page::Settings && state.selected == SettingEarly) {
		ToggleEarly();
	} else if (accept) {
		Activate(state);
	} else if (back) {
		Back(state);
	}
	return true;
}

} // namespace

bool ProcessGameMenuInput(const SDL_Event& event) {
	if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
		const bool down   = event.gbutton.down;
		const auto button = static_cast<SDL_GamepadButton>(event.gbutton.button);
		if (button == SDL_GAMEPAD_BUTTON_BACK) {
			g_back_down = down;
		} else if (button == SDL_GAMEPAD_BUTTON_START) {
			g_start_down = down;
		}
		if (!down) {
			return false; // releases go on to the game: nothing stays held there
		}
		const bool combination = (button == SDL_GAMEPAD_BUTTON_BACK && g_start_down) ||
		                         (button == SDL_GAMEPAD_BUTTON_START && g_back_down);
		if (button == SDL_GAMEPAD_BUTTON_GUIDE || combination) {
			SetState(GameMenuOpen() ? MenuState {} : MenuState {Page::Main, ItemResume});
			return true;
		}
		switch (button) {
			case SDL_GAMEPAD_BUTTON_DPAD_UP: return Navigate(-1, 0, false, false);
			case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Navigate(1, 0, false, false);
			case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Navigate(0, -1, false, false);
			case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Navigate(0, 1, false, false);
			case SDL_GAMEPAD_BUTTON_SOUTH: return Navigate(0, 0, true, false);
			case SDL_GAMEPAD_BUTTON_EAST: return Navigate(0, 0, false, true);
			default: return GameMenuOpen();
		}
	}
	if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
		return GameMenuOpen();
	}
	if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_F10) {
		if (!event.key.repeat) {
			SetState(GameMenuOpen() ? MenuState {} : MenuState {Page::Main, ItemResume});
		}
		return true;
	}
	if (event.type == SDL_EVENT_KEY_DOWN && GameMenuOpen()) {
		switch (event.key.key) {
			case SDLK_UP: return Navigate(-1, 0, false, false);
			case SDLK_DOWN: return Navigate(1, 0, false, false);
			case SDLK_LEFT: return Navigate(0, -1, false, false);
			case SDLK_RIGHT: return Navigate(0, 1, false, false);
			case SDLK_RETURN:
			case SDLK_KP_ENTER: return Navigate(0, 0, true, false);
			case SDLK_ESCAPE:
			case SDLK_BACKSPACE: return Navigate(0, 0, false, true);
			default: return true;
		}
	}
	return false;
}

bool EarlyPresentEnabled() noexcept {
	return g_early_present.load(std::memory_order_relaxed);
}

bool GameMenuOpen() noexcept {
	return g_open.load(std::memory_order_acquire);
}

uint64_t GameMenuRevision() noexcept {
	return g_revision.load(std::memory_order_acquire);
}

void DrawGameMenu(vk::Extent2D extent) {
	const MenuState state = GetState();
	if (state.page == Page::Closed) {
		return;
	}
	const ImVec2 display(static_cast<float>(extent.width), static_cast<float>(extent.height));
	const float  scale = std::max(std::min(display.x / 1280.0f, display.y / 720.0f), 0.5f);
	ImGui::GetBackgroundDrawList()->AddRectFilled({0.0f, 0.0f}, display, IM_COL32(0, 0, 0, 170));
	constexpr const char* hint = "Cross / Enter: select    Circle / Esc: back    PS / F10: close";
	// As wide as the controls line under the items needs, so it is not cut at the edge.
	ImGui::PushFont(nullptr, 17.0f * scale);
	const float hint_width = ImGui::CalcTextSize(hint).x + 2.0f * 24.0f * scale;
	ImGui::PopFont();
	ImGui::SetNextWindowPos({display.x * 0.5f, display.y * 0.5f}, ImGuiCond_Always, {0.5f, 0.5f});
	ImGui::SetNextWindowSize(
	    {std::max(std::min(std::max(460.0f * scale, hint_width), display.x - 32.0f), 1.0f), 0.0f},
	    ImGuiCond_Always);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {24.0f * scale, 22.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {12.0f * scale, 12.0f * scale});
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * scale);
	ImGui::PushFont(nullptr, 22.0f * scale);
	constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
	                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
	ImGui::Begin("##KytyGameMenu", nullptr, flags);
	ImGui::TextUnformatted("KytyPS5-Native");
	ImGui::SameLine();
	ImGui::TextDisabled("%s", NativeGpu::PrimaryRendering() ? "native PS5 GPU path" : "Vulkan path");
	ImGui::Separator();
	const float row_height = 44.0f * scale;
	auto        row        = [&](int index, const char* label, const char* value) {
        const bool selected = state.selected == index;
        const ImVec2 start  = ImGui::GetCursorScreenPos();
        const float  width  = ImGui::GetContentRegionAvail().x;
        if (selected) {
            ImGui::GetWindowDrawList()->AddRectFilled(start, {start.x + width, start.y + row_height},
                                                      IM_COL32(255, 255, 255, 40), 6.0f * scale);
        }
        const float text_y = start.y + (row_height - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::GetWindowDrawList()->AddText({start.x + 12.0f * scale, text_y}, IM_COL32(240, 240, 245, 255), label);
        if (value != nullptr) {
            const float value_width = ImGui::CalcTextSize(value).x;
            ImGui::GetWindowDrawList()->AddText({start.x + width - value_width - 12.0f * scale, text_y},
                                                IM_COL32(170, 200, 255, 255), value);
        }
        ImGui::Dummy({width, row_height});
	};
	if (state.page == Page::Main) {
		row(ItemResume, "Resume", nullptr);
		row(ItemMonitoring, "Monitoring", MonitoringLabel(PerformanceOverlayMode()));
		row(ItemSettings, "Settings", nullptr);
		row(ItemInfo, "Info", nullptr);
		row(ItemExit, "Exit game", nullptr);
	} else if (state.page == Page::Settings) {
		ImGui::TextUnformatted("Settings");
		row(SettingEarly, "Early present", g_early_present.load(std::memory_order_relaxed) ? "On" : "Off");
		ImGui::TextDisabled("Shows a ready frame at once, not at the next tick.");
		row(SettingBack, "Back", nullptr);
	} else if (state.page == Page::Info) {
		ImGui::TextUnformatted("Info");
		std::vector<std::pair<std::string, std::string>> rows;
		{
			std::scoped_lock lock(g_info_mutex);
			rows = g_info;
		}
		for (const auto& [label, value]: rows) {
			row(-1, label.c_str(), value.c_str());
		}
		row(0, "Back", nullptr);
	} else {
		ImGui::TextUnformatted("Exit the game?");
		ImGui::TextDisabled("Progress the game has not saved is lost.");
		row(ConfirmExitGame, "Exit game", nullptr);
		row(ConfirmCancel, "Cancel", nullptr);
	}
	ImGui::Separator();
	ImGui::PushFont(nullptr, 17.0f * scale);
	ImGui::TextDisabled("%s", hint);
	ImGui::PopFont();
	ImGui::End();
	ImGui::PopFont();
	ImGui::PopStyleVar(3);
}

} // namespace Libs::Graphics
