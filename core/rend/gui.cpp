/*
	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "gui.h"
#include "osd.h"
#include "cfg/cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/maple/maple_devs.h"
#include "imgui.h"
#include "network/net_handshake.h"
#include "network/ggpo.h"
#include "wsi/context.h"
#include "input/gamepad_device.h"
#include "gui_util.h"
#include "game_scanner.h"
#include "version.h"
#include "oslib/oslib.h"
#include "audio/audiostream.h"
#include "imgread/common.h"
#include "log/LogManager.h"
#include "emulator.h"
#include "rend/mainui.h"
#include "lua/lua.h"
#include "gui_chat.h"
#include "imgui_driver.h"
#if FC_PROFILER
#include "implot.h"
#endif
#include "boxart/boxart.h"
#include "profiler/fc_profiler.h"
#include "hw/naomi/card_reader.h"
#include "oslib/resources.h"
#if defined(USE_SDL)
#include "sdl/sdl.h"
#endif

#ifdef __ANDROID__
#include "gui_android.h"
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <mutex>
#include <algorithm>

static bool game_started;

int insetLeft, insetRight, insetTop, insetBottom;
std::unique_ptr<ImGuiDriver> imguiDriver;

static bool inited = false;
GuiState gui_state = GuiState::Main;
static bool commandLineStart;
static u32 mouseButtons;
static int mouseX, mouseY;
static float mouseWheel;
static std::string error_msg;
static bool error_msg_shown;
static std::string osd_message;
static double osd_message_end;
static std::mutex osd_message_mutex;
static void (*showOnScreenKeyboard)(bool show);
static bool keysUpNextFrame[512];
static bool uiUserScaleUpdated;

static void reset_vmus();
void error_popup();

static GameScanner scanner;
static BackgroundGameLoader gameLoader;
static Boxart boxart;
static Chat chat;
static std::recursive_mutex guiMutex;
using LockGuard = std::lock_guard<std::recursive_mutex>;

static void emuEventCallback(Event event, void *)
{
	switch (event)
	{
	case Event::Resume:
		game_started = true;
		break;
	case Event::Start:
		GamepadDevice::load_system_mappings();
		break;
	case Event::Terminate:
		GamepadDevice::load_system_mappings();
		game_started = false;
		break;
	default:
		break;
	}
}

void gui_init()
{
	if (inited)
		return;
	inited = true;

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
#if FC_PROFILER
	ImPlot::CreateContext();
#endif
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

	io.IniFilename = NULL;

	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls

    EventManager::listen(Event::Resume, emuEventCallback);
    EventManager::listen(Event::Start, emuEventCallback);
	EventManager::listen(Event::Terminate, emuEventCallback);
    ggpo::receiveChatMessages([](int playerNum, const std::string& msg) { chat.receive(playerNum, msg); });
}

static ImGuiKey keycodeToImGuiKey(u8 keycode)
{
	switch (keycode)
	{
		case 0x2B: return ImGuiKey_Tab;
		case 0x50: return ImGuiKey_LeftArrow;
		case 0x4F: return ImGuiKey_RightArrow;
		case 0x52: return ImGuiKey_UpArrow;
		case 0x51: return ImGuiKey_DownArrow;
		case 0x4B: return ImGuiKey_PageUp;
		case 0x4E: return ImGuiKey_PageDown;
		case 0x4A: return ImGuiKey_Home;
		case 0x4D: return ImGuiKey_End;
		case 0x49: return ImGuiKey_Insert;
		case 0x4C: return ImGuiKey_Delete;
		case 0x2A: return ImGuiKey_Backspace;
		case 0x2C: return ImGuiKey_Space;
		case 0x28: return ImGuiKey_Enter;
		case 0x29: return ImGuiKey_Escape;
		case 0x04: return ImGuiKey_A;
		case 0x06: return ImGuiKey_C;
		case 0x19: return ImGuiKey_V;
		case 0x1B: return ImGuiKey_X;
		case 0x1C: return ImGuiKey_Y;
		case 0x1D: return ImGuiKey_Z;
		case 0xE0:
		case 0xE4:
			return ImGuiMod_Ctrl;
		case 0xE1:
		case 0xE5:
			return ImGuiMod_Shift;
		case 0xE3:
		case 0xE7:
			return ImGuiMod_Super;
		default: return ImGuiKey_None;
	}
}

void gui_initFonts()
{
	static float uiScale;

	verify(inited);

#if !defined(TARGET_UWP) && !defined(__SWITCH__)
	settings.display.uiScale = std::max(1.f, settings.display.dpi / 100.f * 0.75f);
   	// Limit scaling on small low-res screens
    if (settings.display.width <= 640 || settings.display.height <= 480)
    	settings.display.uiScale = std::min(1.4f, settings.display.uiScale);
#endif
    settings.display.uiScale *= config::UIScaling / 100.f;
	if (settings.display.uiScale == uiScale && ImGui::GetIO().Fonts->IsBuilt())
		return;
	uiScale = settings.display.uiScale;

    // Setup Dear ImGui style
	ImGui::GetStyle() = ImGuiStyle{};
    ImGui::StyleColorsDark();
    ImGui::GetStyle().TabRounding = 0;
    ImGui::GetStyle().ItemSpacing = ImVec2(8, 8);		// from 8,4
    ImGui::GetStyle().ItemInnerSpacing = ImVec2(4, 6);	// from 4,4
#if defined(__ANDROID__) || defined(TARGET_IPHONE) || defined(__SWITCH__)
    ImGui::GetStyle().TouchExtraPadding = ImVec2(1, 1);	// from 0,0
#endif
	if (settings.display.uiScale > 1)
		ImGui::GetStyle().ScaleAllSizes(settings.display.uiScale);

    static const ImWchar ranges[] =
    {
    	0x0020, 0xFFFF, // All chars
        0,
    };

	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();
	const float fontSize = 17.f * settings.display.uiScale;
	size_t dataSize;
	std::unique_ptr<u8[]> data = resource::load("fonts/Roboto-Medium.ttf", dataSize);
	verify(data != nullptr);
	io.Fonts->AddFontFromMemoryTTF(data.release(), dataSize, fontSize, nullptr, ranges);
    ImFontConfig font_cfg;
    font_cfg.MergeMode = true;
#ifdef _WIN32
    u32 cp = GetACP();
    std::string fontDir = std::string(nowide::getenv("SYSTEMROOT")) + "\\Fonts\\";
    switch (cp)
    {
    case 932:	// Japanese
		{
			font_cfg.FontNo = 2;	// UIGothic
			ImFont* font = io.Fonts->AddFontFromFileTTF((fontDir + "msgothic.ttc").c_str(), fontSize, &font_cfg, io.Fonts->GetGlyphRangesJapanese());
			font_cfg.FontNo = 2;	// Meiryo UI
			if (font == nullptr)
				io.Fonts->AddFontFromFileTTF((fontDir + "Meiryo.ttc").c_str(), fontSize, &font_cfg, io.Fonts->GetGlyphRangesJapanese());
		}
		break;
    case 949:	// Korean
		{
			ImFont* font = io.Fonts->AddFontFromFileTTF((fontDir + "Malgun.ttf").c_str(), fontSize, &font_cfg, io.Fonts->GetGlyphRangesKorean());
			if (font == nullptr)
			{
				font_cfg.FontNo = 2;	// Dotum
				io.Fonts->AddFontFromFileTTF((fontDir + "Gulim.ttc").c_str(), fontSize, &font_cfg, io.Fonts->GetGlyphRangesKorean());
			}
		}
    	break;
    case 950:	// Traditional Chinese
		{
			font_cfg.FontNo = 1; // Microsoft JhengHei UI Regular
			ImFont* font = io.Fonts->AddFontFromFileTTF((fontDir + "Msjh.ttc").c_str(), fontSize, &font_cfg, GetGlyphRangesChineseTraditionalOfficial());
			font_cfg.FontNo = 0;
			if (font == nullptr)
				io.Fonts->AddFontFromFileTTF((fontDir + "MSJH.ttf").c_str(), fontSize, &font_cfg, GetGlyphRangesChineseTraditionalOfficial());
		}
    	break;
    case 936:	// Simplified Chinese
		io.Fonts->AddFontFromFileTTF((fontDir + "Simsun.ttc").c_str(), fontSize, &font_cfg, GetGlyphRangesChineseSimplifiedOfficial());
    	break;
    default:
    	break;
    }
#elif defined(__APPLE__) && !defined(TARGET_IPHONE)
    std::string fontDir = std::string("/System/Library/Fonts/");

    extern std::string os_Locale();
    std::string locale = os_Locale();

    if (locale.find("ja") == 0)             // Japanese
    {
        io.Fonts->AddFontFromFileTTF((fontDir + "ヒラギノ角ゴシック W4.ttc").c_str(), fontSize, &font_cfg, io.Fonts->GetGlyphRangesJapanese());
    }
    else if (locale.find("ko") == 0)       // Korean
    {
        io.Fonts->AddFontFromFileTTF((fontDir + "AppleSDGothicNeo.ttc").c_str(), fontSize, &font_cfg, io.Fonts->GetGlyphRangesKorean());
    }
    else if (locale.find("zh-Hant") == 0)  // Traditional Chinese
    {
        io.Fonts->AddFontFromFileTTF((fontDir + "PingFang.ttc").c_str(), fontSize, &font_cfg, GetGlyphRangesChineseTraditionalOfficial());
    }
    else if (locale.find("zh-Hans") == 0)  // Simplified Chinese
    {
        io.Fonts->AddFontFromFileTTF((fontDir + "PingFang.ttc").c_str(), fontSize, &font_cfg, GetGlyphRangesChineseSimplifiedOfficial());
    }
#elif defined(__ANDROID__)
    if (getenv("FLYCAST_LOCALE") != nullptr)
    {
    	const ImWchar *glyphRanges = nullptr;
    	std::string locale = getenv("FLYCAST_LOCALE");
        if (locale.find("ja") == 0)				// Japanese
        	glyphRanges = io.Fonts->GetGlyphRangesJapanese();
        else if (locale.find("ko") == 0)		// Korean
        	glyphRanges = io.Fonts->GetGlyphRangesKorean();
        else if (locale.find("zh_TW") == 0
        		|| locale.find("zh_HK") == 0)	// Traditional Chinese
        	glyphRanges = GetGlyphRangesChineseTraditionalOfficial();
        else if (locale.find("zh_CN") == 0)		// Simplified Chinese
        	glyphRanges = GetGlyphRangesChineseSimplifiedOfficial();

        if (glyphRanges != nullptr)
        	io.Fonts->AddFontFromFileTTF("/system/fonts/NotoSansCJK-Regular.ttc", fontSize, &font_cfg, glyphRanges);
    }

    // TODO Linux, iOS, ...
#endif
	NOTICE_LOG(RENDERER, "Screen DPI is %.0f, size %d x %d. Scaling by %.2f", settings.display.dpi, settings.display.width, settings.display.height, settings.display.uiScale);
}

void gui_keyboard_input(u16 wc)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureKeyboard)
		io.AddInputCharacter(wc);
}

void gui_keyboard_inputUTF8(const std::string& s)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureKeyboard)
		io.AddInputCharactersUTF8(s.c_str());
}

void gui_keyboard_key(u8 keyCode, bool pressed)
{
	if (!inited)
		return;
	ImGuiKey key = keycodeToImGuiKey(keyCode);
	if (key == ImGuiKey_None)
		return;
	if (!pressed && ImGui::IsKeyDown(key))
	{
		keysUpNextFrame[keyCode] = true;
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.AddKeyEvent(key, pressed);
}

bool gui_keyboard_captured()
{
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureKeyboard;
}

bool gui_mouse_captured()
{
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureMouse;
}

void gui_set_mouse_position(int x, int y)
{
	mouseX = std::round(x * settings.display.pointScale);
	mouseY = std::round(y * settings.display.pointScale);
}

void gui_set_mouse_button(int button, bool pressed)
{
	if (pressed)
		mouseButtons |= 1 << button;
	else
		mouseButtons &= ~(1 << button);
}

void gui_set_mouse_wheel(float delta)
{
	mouseWheel += delta;
}

static void gui_newFrame()
{
	imguiDriver->newFrame();
	ImGui::GetIO().DisplaySize.x = settings.display.width;
	ImGui::GetIO().DisplaySize.y = settings.display.height;

	ImGuiIO& io = ImGui::GetIO();

	if (mouseX < 0 || mouseX >= settings.display.width || mouseY < 0 || mouseY >= settings.display.height)
		io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	else
		io.AddMousePosEvent(mouseX, mouseY);
	static bool delayTouch;
#if defined(__ANDROID__) || defined(TARGET_IPHONE) || defined(__SWITCH__)
	// Delay touch by one frame to allow widgets to be hovered before click
	// This is required for widgets using ImGuiButtonFlags_AllowItemOverlap such as TabItem's
	if (!delayTouch && (mouseButtons & (1 << 0)) != 0 && !io.MouseDown[ImGuiMouseButton_Left])
		delayTouch = true;
	else
		delayTouch = false;
#endif
	if (io.WantCaptureMouse)
	{
		io.AddMouseWheelEvent(0, -mouseWheel / 16);
		mouseWheel = 0;
	}
	if (!delayTouch)
		io.AddMouseButtonEvent(ImGuiMouseButton_Left, (mouseButtons & (1 << 0)) != 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Right, (mouseButtons & (1 << 1)) != 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Middle, (mouseButtons & (1 << 2)) != 0);
	io.AddMouseButtonEvent(3, (mouseButtons & (1 << 3)) != 0);

	// shows a popup navigation window even in game because of the OSD
	//io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, ((kcode[0] & DC_BTN_X) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, ((kcode[0] & DC_BTN_B) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceUp, ((kcode[0] & DC_BTN_Y) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, ((kcode[0] & DC_BTN_A) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, ((kcode[0] & DC_DPAD_LEFT) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadRight, ((kcode[0] & DC_DPAD_RIGHT) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadUp, ((kcode[0] & DC_DPAD_UP) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, ((kcode[0] & DC_DPAD_DOWN) == 0));
	
	float analog;
	analog = joyx[0] < 0 ? -(float)joyx[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, analog > 0.1f, analog);
	analog = joyx[0] > 0 ? (float)joyx[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, analog > 0.1f, analog);
	analog = joyy[0] < 0 ? -(float)joyy[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, analog > 0.1f, analog);
	analog = joyy[0] > 0 ? (float)joyy[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, analog > 0.1f, analog);

	ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);

	if (showOnScreenKeyboard != nullptr)
		showOnScreenKeyboard(io.WantTextInput);
#ifdef USE_SDL
	else
	{
		if (io.WantTextInput && !SDL_IsTextInputActive())
		{
			SDL_StartTextInput();
		}
		else if (!io.WantTextInput && SDL_IsTextInputActive())
		{
			SDL_StopTextInput();
		}
	}
#endif
}

static void delayedKeysUp()
{
	ImGuiIO& io = ImGui::GetIO();
	for (u32 i = 0; i < std::size(keysUpNextFrame); i++)
		if (keysUpNextFrame[i])
			io.AddKeyEvent(keycodeToImGuiKey(i), false);
	memset(keysUpNextFrame, 0, sizeof(keysUpNextFrame));
}

static void gui_endFrame(bool gui_open)
{
    ImGui::Render();
    imguiDriver->renderDrawData(ImGui::GetDrawData(), gui_open);
    delayedKeysUp();
}

void gui_setOnScreenKeyboardCallback(void (*callback)(bool show))
{
	showOnScreenKeyboard = callback;
}

void gui_set_insets(int left, int right, int top, int bottom)
{
	insetLeft = left;
	insetRight = right;
	insetTop = top;
	insetBottom = bottom;
}

#if 0
#include "oslib/timeseries.h"
#include <vector>
TimeSeries renderTimes;
TimeSeries vblankTimes;

void gui_plot_render_time(int width, int height)
{
	std::vector<float> v = renderTimes.data();
	ImGui::PlotLines("Render Times", v.data(), v.size(), 0, "", 0.0, 1.0 / 30.0, ImVec2(300, 50));
	ImGui::Text("StdDev: %.1f%%", renderTimes.stddev() * 100.f / 0.01666666667f);
	v = vblankTimes.data();
	ImGui::PlotLines("VBlank", v.data(), v.size(), 0, "", 0.0, 1.0 / 30.0, ImVec2(300, 50));
	ImGui::Text("StdDev: %.1f%%", vblankTimes.stddev() * 100.f / 0.01666666667f);
}
#endif

void gui_open_settings()
{
	const LockGuard lock(guiMutex);
	if (gui_state == GuiState::Closed && !settings.naomi.slave)
	{
		if (!ggpo::active())
		{
			HideOSD();
			try {
				emu.stop();
				gui_setState(GuiState::Commands);
			} catch (const FlycastException& e) {
				gui_stop_game(e.what());
			}
		}
		else
		{
			chat.toggle();
		}
	}
	else if (gui_state == GuiState::VJoyEdit)
	{
		gui_setState(GuiState::VJoyEditCommands);
	}
	else if (gui_state == GuiState::Loading)
	{
		gameLoader.cancel();
	}
	else if (gui_state == GuiState::Commands)
	{
		gui_setState(GuiState::Closed);
		GamepadDevice::load_system_mappings();
		emu.start();
	}
}

void gui_start_game(const std::string& path)
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Main && gui_state != GuiState::Closed && gui_state != GuiState::Commands)
		return;
	emu.unloadGame();
	reset_vmus();
    chat.reset();

	scanner.stop();
	gui_setState(GuiState::Loading);
	gameLoader.load(path);
}

void gui_stop_game(const std::string& message)
{
	const LockGuard lock(guiMutex);
	if (!commandLineStart)
	{
		// Exit to main menu
		emu.unloadGame();
		gui_setState(GuiState::Main);
		reset_vmus();
		if (!message.empty())
			gui_error("Flycast已停止\n\n" + message);
	}
	else
	{
		if (!message.empty())
			ERROR_LOG(COMMON, "Flycast已停止: %s", message.c_str());
		// Exit emulator
		dc_exit();
	}
}

static bool savestateAllowed()
{
	return !settings.content.path.empty() && !settings.network.online && !settings.naomi.multiboard;
}

static void gui_display_commands()
{
   	imguiDriver->displayVmus();

    centerNextWindow();
    ImGui::SetNextWindowSize(ScaledVec2(330, 0));

    ImGui::Begin("##commands", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

    {
    	if (card_reader::barcodeAvailable())
    	{
			char cardBuf[64] {};
			strncpy(cardBuf, card_reader::barcodeGetCard().c_str(), sizeof(cardBuf) - 1);
			if (ImGui::InputText("Card", cardBuf, sizeof(cardBuf), ImGuiInputTextFlags_None, nullptr, nullptr))
				card_reader::barcodeSetCard(cardBuf);
    	}

    	DisabledScope scope(!savestateAllowed());

		// Load State
		if (ImGui::Button("加载存档", ScaledVec2(110, 50)) && savestateAllowed())
		{
			gui_setState(GuiState::Closed);
			dc_loadstate(config::SavestateSlot);
		}
		ImGui::SameLine();

		// Slot #
		std::string slot = "存档位 " + std::to_string((int)config::SavestateSlot + 1);
		if (ImGui::Button(slot.c_str(), ImVec2(80 * settings.display.uiScale - ImGui::GetStyle().FramePadding.x, 50 * settings.display.uiScale)))
			ImGui::OpenPopup("slot_select_popup");
		if (ImGui::BeginPopup("slot_select_popup"))
		{
			for (int i = 0; i < 10; i++)
				if (ImGui::Selectable(std::to_string(i + 1).c_str(), config::SavestateSlot == i, 0,
						ImVec2(ImGui::CalcTextSize("存档位 8").x, 0))) {
					config::SavestateSlot = i;
					SaveSettings();
				}
			ImGui::EndPopup();
		}
		ImGui::SameLine();

		// Save State
		if (ImGui::Button("保存存档", ScaledVec2(110, 50)) && savestateAllowed())
		{
			gui_setState(GuiState::Closed);
			dc_savestate(config::SavestateSlot);
		}
    }

	ImGui::Columns(2, "buttons", false);

	// Settings
	if (ImGui::Button("设置", ScaledVec2(150, 50)))
	{
		gui_setState(GuiState::Settings);
	}
	ImGui::NextColumn();
	if (ImGui::Button("回到游戏", ScaledVec2(150, 50)))
	{
		GamepadDevice::load_system_mappings();
		gui_setState(GuiState::Closed);
	}

	ImGui::NextColumn();

	// Insert/Eject Disk
	const char *disk_label = libGDR_GetDiscType() == Open ? "更换光盘" : "更换光盘";
	if (ImGui::Button(disk_label, ScaledVec2(150, 50)))
	{
		if (libGDR_GetDiscType() == Open)
		{
			gui_setState(GuiState::SelectDisk);
		}
		else
		{
			DiscOpenLid();
			gui_setState(GuiState::Closed);
		}
	}
	ImGui::NextColumn();

	// Cheats
	{
		DisabledScope scope(settings.network.online);

		if (ImGui::Button("金手指", ScaledVec2(150, 50)) && !settings.network.online)
			gui_setState(GuiState::Cheats);
	}
	ImGui::Columns(1, nullptr, false);

	// Exit
	if (ImGui::Button(commandLineStart ? "退出" : "关闭游戏", ScaledVec2(300, 50)
			+ ImVec2(ImGui::GetStyle().ColumnsMinSpacing + ImGui::GetStyle().FramePadding.x * 2 - 1, 0)))
	{
		gui_stop_game();
	}

	ImGui::End();
}

inline static void header(const char *title)
{
	ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)); // Left
	ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
	ImGui::BeginDisabled();
	ImGui::ButtonEx(title, ImVec2(-1, 0));
	ImGui::EndDisabled();
	ImGui::PopStyleVar();
	ImGui::PopStyleVar();
}

const char *maple_device_types[] =
{
	"无",
	"Sega 控制器",
	"光枪",
	"键盘",
	"鼠标",
	"双摇杆",
	"街机/ASCII摇杆",
	"Maracas 控制器",
	"Fishing 控制器",
	"Pop'n Music 控制器",
	"Racing 控制器",
	"Densha de Go! 控制器",
//	"Dreameye",
};

const char *maple_expansion_device_types[] = 
{ 
	"无",
	"Sega VMU",
	"Vibration Pack",
	"Microphone",
};

static const char *maple_device_name(MapleDeviceType type)
{
	switch (type)
	{
	case MDT_SegaController:
		return maple_device_types[1];
	case MDT_LightGun:
		return maple_device_types[2];
	case MDT_Keyboard:
		return maple_device_types[3];
	case MDT_Mouse:
		return maple_device_types[4];
	case MDT_TwinStick:
		return maple_device_types[5];
	case MDT_AsciiStick:
		return maple_device_types[6];
	case MDT_MaracasController:
		return maple_device_types[7];
	case MDT_FishingController:
		return maple_device_types[8];
	case MDT_PopnMusicController:
		return maple_device_types[9];
	case MDT_RacingController:
		return maple_device_types[10];
	case MDT_DenshaDeGoController:
		return maple_device_types[11];
	case MDT_Dreameye:
//		return maple_device_types[12];
	case MDT_None:
	default:
		return maple_device_types[0];
	}
}

static MapleDeviceType maple_device_type_from_index(int idx)
{
	switch (idx)
	{
	case 1:
		return MDT_SegaController;
	case 2:
		return MDT_LightGun;
	case 3:
		return MDT_Keyboard;
	case 4:
		return MDT_Mouse;
	case 5:
		return MDT_TwinStick;
	case 6:
		return MDT_AsciiStick;
	case 7:
		return MDT_MaracasController;
	case 8:
		return MDT_FishingController;
	case 9:
		return MDT_PopnMusicController;
	case 10:
		return MDT_RacingController;
	case 11:
		return MDT_DenshaDeGoController;
	case 12:
		return MDT_Dreameye;
	case 0:
	default:
		return MDT_None;
	}
}

static const char *maple_expansion_device_name(MapleDeviceType type)
{
	switch (type)
	{
	case MDT_SegaVMU:
		return maple_expansion_device_types[1];
	case MDT_PurupuruPack:
		return maple_expansion_device_types[2];
	case MDT_Microphone:
		return maple_expansion_device_types[3];
	case MDT_None:
	default:
		return maple_expansion_device_types[0];
	}
}

const char *maple_ports[] = { "None", "A", "B", "C", "D", "All" };

struct Mapping {
	DreamcastKey key;
	const char *name;
};

const Mapping dcButtons[] = {
	{ EMU_BTN_NONE, "十字键" },
	{ DC_DPAD_UP, "上" },
	{ DC_DPAD_DOWN, "下" },
	{ DC_DPAD_LEFT, "左" },
	{ DC_DPAD_RIGHT, "右" },

	{ DC_AXIS_UP, "Thumbstick Up" },
	{ DC_AXIS_DOWN, "Thumbstick Down" },
	{ DC_AXIS_LEFT, "Thumbstick Left" },
	{ DC_AXIS_RIGHT, "Thumbstick Right" },

	{ DC_AXIS2_UP, "R.Thumbstick Up" },
	{ DC_AXIS2_DOWN, "R.Thumbstick Down" },
	{ DC_AXIS2_LEFT, "R.Thumbstick Left" },
	{ DC_AXIS2_RIGHT, "R.Thumbstick Right" },

	{ DC_AXIS3_UP,    "Axis 3 Up"    },
	{ DC_AXIS3_DOWN,  "Axis 3 Down"  },
	{ DC_AXIS3_LEFT,  "Axis 3 Left"  },
	{ DC_AXIS3_RIGHT, "Axis 3 Right" },

	{ DC_DPAD2_UP,    "DPad2 Up"    },
	{ DC_DPAD2_DOWN,  "DPad2 Down"  },
	{ DC_DPAD2_LEFT,  "DPad2 Left"  },
	{ DC_DPAD2_RIGHT, "DPad2 Right" },

	{ EMU_BTN_NONE, "按键" },
	{ DC_BTN_A, "A" },
	{ DC_BTN_B, "B" },
	{ DC_BTN_X, "X" },
	{ DC_BTN_Y, "Y" },
	{ DC_BTN_C, "C" },
	{ DC_BTN_D, "D" },
	{ DC_BTN_Z, "Z" },

	{ EMU_BTN_NONE, "肩键"      },
	{ DC_AXIS_LT,   "Left Trigger"  },
	{ DC_AXIS_RT,   "Right Trigger" },
	{ DC_AXIS_LT2,   "Left Trigger 2" },
	{ DC_AXIS_RT2,   "Right Trigger 2" },

	{ EMU_BTN_NONE, "系统按键" },
	{ DC_BTN_START, "开始" },
	{ DC_BTN_RELOAD, "重载" },

	{ EMU_BTN_NONE, "模拟器" },
	{ EMU_BTN_MENU, "菜单" },
	{ EMU_BTN_ESCAPE, "退出" },
	{ EMU_BTN_FFORWARD, "快进" },
	{ EMU_BTN_LOADSTATE, "加载存档" },
	{ EMU_BTN_SAVESTATE, "保存存档" },
	{ EMU_BTN_BYPASS_KB, "绕过模拟键盘" },

	{ EMU_BTN_NONE, nullptr }
};

const Mapping arcadeButtons[] = {
	{ EMU_BTN_NONE, "十字键" },
	{ DC_DPAD_UP, "上" },
	{ DC_DPAD_DOWN, "下" },
	{ DC_DPAD_LEFT, "左" },
	{ DC_DPAD_RIGHT, "右" },

	{ DC_AXIS_UP, "Thumbstick Up" },
	{ DC_AXIS_DOWN, "Thumbstick Down" },
	{ DC_AXIS_LEFT, "Thumbstick Left" },
	{ DC_AXIS_RIGHT, "Thumbstick Right" },

	{ DC_AXIS2_UP, "R.Thumbstick Up" },
	{ DC_AXIS2_DOWN, "R.Thumbstick Down" },
	{ DC_AXIS2_LEFT, "R.Thumbstick Left" },
	{ DC_AXIS2_RIGHT, "R.Thumbstick Right" },

	{ EMU_BTN_NONE, "按键" },
	{ DC_BTN_A, "按键 1" },
	{ DC_BTN_B, "按键 2" },
	{ DC_BTN_C, "按键 3" },
	{ DC_BTN_X, "按键 4" },
	{ DC_BTN_Y, "按键 5" },
	{ DC_BTN_Z, "按键 6" },
	{ DC_DPAD2_LEFT, "按键 7" },
	{ DC_DPAD2_RIGHT, "按键 8" },
//	{ DC_DPAD2_RIGHT, "Button 9" }, // TODO

	{ EMU_BTN_NONE, "肩键" },
	{ DC_AXIS_LT, "Left Trigger" },
	{ DC_AXIS_RT, "Right Trigger" },
	{ DC_AXIS_LT2,   "Left Trigger 2" },
	{ DC_AXIS_RT2,   "Right Trigger 2" },

	{ EMU_BTN_NONE, "系统按键" },
	{ DC_BTN_START, "开始" },
	{ DC_BTN_RELOAD, "重载" },
	{ DC_BTN_D, "投币" },
	{ DC_DPAD2_UP, "服务" },
	{ DC_DPAD2_DOWN, "测试" },
	{ DC_BTN_INSERT_CARD, "插卡" },

	{ EMU_BTN_NONE, "模拟器" },
	{ EMU_BTN_MENU, "菜单" },
	{ EMU_BTN_ESCAPE, "退出" },
	{ EMU_BTN_FFORWARD, "快进" },
	{ EMU_BTN_LOADSTATE, "加载存档" },
	{ EMU_BTN_SAVESTATE, "保存存档" },
	{ EMU_BTN_BYPASS_KB, "绕过模拟键盘" },

	{ EMU_BTN_NONE, nullptr }
};

static MapleDeviceType maple_expansion_device_type_from_index(int idx)
{
	switch (idx)
	{
	case 1:
		return MDT_SegaVMU;
	case 2:
		return MDT_PurupuruPack;
	case 3:
		return MDT_Microphone;
	case 0:
	default:
		return MDT_None;
	}
}

static std::shared_ptr<GamepadDevice> mapped_device;
static u32 mapped_code;
static bool analogAxis;
static bool positiveDirection;
static double map_start_time;
static bool arcade_button_mode;
static u32 gamepad_port;

static void unmapControl(const std::shared_ptr<InputMapping>& mapping, u32 gamepad_port, DreamcastKey key)
{
	mapping->clear_button(gamepad_port, key);
	mapping->clear_axis(gamepad_port, key);
}

static DreamcastKey getOppositeDirectionKey(DreamcastKey key)
{
	switch (key)
	{
	case DC_DPAD_UP:
		return DC_DPAD_DOWN;
	case DC_DPAD_DOWN:
		return DC_DPAD_UP;
	case DC_DPAD_LEFT:
		return DC_DPAD_RIGHT;
	case DC_DPAD_RIGHT:
		return DC_DPAD_LEFT;
	case DC_DPAD2_UP:
		return DC_DPAD2_DOWN;
	case DC_DPAD2_DOWN:
		return DC_DPAD2_UP;
	case DC_DPAD2_LEFT:
		return DC_DPAD2_RIGHT;
	case DC_DPAD2_RIGHT:
		return DC_DPAD2_LEFT;
	case DC_AXIS_UP:
		return DC_AXIS_DOWN;
	case DC_AXIS_DOWN:
		return DC_AXIS_UP;
	case DC_AXIS_LEFT:
		return DC_AXIS_RIGHT;
	case DC_AXIS_RIGHT:
		return DC_AXIS_LEFT;
	case DC_AXIS2_UP:
		return DC_AXIS2_DOWN;
	case DC_AXIS2_DOWN:
		return DC_AXIS2_UP;
	case DC_AXIS2_LEFT:
		return DC_AXIS2_RIGHT;
	case DC_AXIS2_RIGHT:
		return DC_AXIS2_LEFT;
	case DC_AXIS3_UP:
		return DC_AXIS3_DOWN;
	case DC_AXIS3_DOWN:
		return DC_AXIS3_UP;
	case DC_AXIS3_LEFT:
		return DC_AXIS3_RIGHT;
	case DC_AXIS3_RIGHT:
		return DC_AXIS3_LEFT;
	default:
		return EMU_BTN_NONE;
	}
}
static void detect_input_popup(const Mapping *mapping)
{
	ImVec2 padding = ScaledVec2(20, 20);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, padding);
	if (ImGui::BeginPopupModal("Map Control", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
	{
		ImGui::Text("等待连接控制器 '%s'...", mapping->name);
		double now = os_GetSeconds();
		ImGui::Text("超时 %d s", (int)(5 - (now - map_start_time)));
		if (mapped_code != (u32)-1)
		{
			std::shared_ptr<InputMapping> input_mapping = mapped_device->get_input_mapping();
			if (input_mapping != NULL)
			{
				unmapControl(input_mapping, gamepad_port, mapping->key);
				if (analogAxis)
				{
					input_mapping->set_axis(gamepad_port, mapping->key, mapped_code, positiveDirection);
					DreamcastKey opposite = getOppositeDirectionKey(mapping->key);
					// Map the axis opposite direction to the corresponding opposite dc button or axis,
					// but only if the opposite direction axis isn't used and the dc button or axis isn't mapped.
					if (opposite != EMU_BTN_NONE
							&& input_mapping->get_axis_id(gamepad_port, mapped_code, !positiveDirection) == EMU_BTN_NONE
							&& input_mapping->get_axis_code(gamepad_port, opposite).first == (u32)-1
							&& input_mapping->get_button_code(gamepad_port, opposite) == (u32)-1)
						input_mapping->set_axis(gamepad_port, opposite, mapped_code, !positiveDirection);
				}
				else
					input_mapping->set_button(gamepad_port, mapping->key, mapped_code);
			}
			mapped_device = NULL;
			ImGui::CloseCurrentPopup();
		}
		else if (now - map_start_time >= 5)
		{
			mapped_device = NULL;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar(2);
}

static void displayLabelOrCode(const char *label, u32 code, const char *suffix = "")
{
	if (label != nullptr)
		ImGui::Text("%s%s", label, suffix);
	else
		ImGui::Text("[%d]%s", code, suffix);
}

static void displayMappedControl(const std::shared_ptr<GamepadDevice>& gamepad, DreamcastKey key)
{
	std::shared_ptr<InputMapping> input_mapping = gamepad->get_input_mapping();
	u32 code = input_mapping->get_button_code(gamepad_port, key);
	if (code != (u32)-1)
	{
		displayLabelOrCode(gamepad->get_button_name(code), code);
		return;
	}
	std::pair<u32, bool> pair = input_mapping->get_axis_code(gamepad_port, key);
	code = pair.first;
	if (code != (u32)-1)
	{
		displayLabelOrCode(gamepad->get_axis_name(code), code, pair.second ? "+" : "-");
		return;
	}
}

static void controller_mapping_popup(const std::shared_ptr<GamepadDevice>& gamepad)
{
	fullScreenWindow(true);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	if (ImGui::BeginPopupModal("按键映射", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float winWidth = ImGui::GetIO().DisplaySize.x - insetLeft - insetRight - (style.WindowBorderSize + style.WindowPadding.x) * 2;
		const float col_width = (winWidth - style.GrabMinSize - style.ItemSpacing.x
				- (ImGui::CalcTextSize("映射").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)
				- (ImGui::CalcTextSize("解除映射").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)) / 2;
		const float scaling = settings.display.uiScale;

		static int map_system;
		static int item_current_map_idx = 0;
		static int last_item_current_map_idx = 2;

		std::shared_ptr<InputMapping> input_mapping = gamepad->get_input_mapping();
		if (input_mapping == NULL || ImGui::Button("完成", ScaledVec2(100, 30)))
		{
			ImGui::CloseCurrentPopup();
			gamepad->save_mapping(map_system);
			last_item_current_map_idx = 2;
			ImGui::EndPopup();
			ImGui::PopStyleVar();
			return;
		}
		ImGui::SetItemDefaultFocus();

		float portWidth = 0;
		if (gamepad->maple_port() == MAPLE_PORTS)
		{
			ImGui::SameLine();
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (30 * scaling - ImGui::GetFontSize()) / 2));
			portWidth = ImGui::CalcTextSize("AA").x + ImGui::GetStyle().ItemSpacing.x * 2.0f + ImGui::GetFontSize();
			ImGui::SetNextItemWidth(portWidth);
			if (ImGui::BeginCombo("端口", maple_ports[gamepad_port + 1]))
			{
				for (u32 j = 0; j < MAPLE_PORTS; j++)
				{
					bool is_selected = gamepad_port == j;
					if (ImGui::Selectable(maple_ports[j + 1], &is_selected))
						gamepad_port = j;
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			portWidth += ImGui::CalcTextSize("Port").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x;
			ImGui::PopStyleVar();
		}
		float comboWidth = ImGui::CalcTextSize("Dreamcast 控制器").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 4;
		float gameConfigWidth = 0;
		if (!settings.content.gameId.empty())
			gameConfigWidth = ImGui::CalcTextSize(gamepad->isPerGameMapping() ? "删除游戏配置" : "生成游戏配置").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(0, ImGui::GetContentRegionAvail().x - comboWidth - gameConfigWidth - ImGui::GetStyle().ItemSpacing.x - 100 * scaling * 2 - portWidth);

		ImGui::AlignTextToFramePadding();

		if (!settings.content.gameId.empty())
		{
			if (gamepad->isPerGameMapping())
			{
				if (ImGui::Button("删除游戏配置", ScaledVec2(0, 30)))
				{
					gamepad->setPerGameMapping(false);
					if (!gamepad->find_mapping(map_system))
						gamepad->resetMappingToDefault(arcade_button_mode, true);
				}
			}
			else
			{
				if (ImGui::Button("生成游戏配置", ScaledVec2(0, 30)))
					gamepad->setPerGameMapping(true);
			}
			ImGui::SameLine();
		}
		if (ImGui::Button("重置中……", ScaledVec2(100, 30)))
			ImGui::OpenPopup("确认重置");

		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));
		if (ImGui::BeginPopupModal("确认重置", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::Text("你确定要将映射重置为默认设置吗？");
			static bool hitbox;
			if (arcade_button_mode)
			{
				ImGui::Text("控制器类型:");
				if (ImGui::RadioButton("游戏手柄", !hitbox))
					hitbox = false;
				ImGui::SameLine();
				if (ImGui::RadioButton("街机 / Hit Box", hitbox))
					hitbox = true;
			}
			ImGui::NewLine();
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(20 * scaling, ImGui::GetStyle().ItemSpacing.y));
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
			if (ImGui::Button("是"))
			{
				gamepad->resetMappingToDefault(arcade_button_mode, !hitbox);
				gamepad->save_mapping(map_system);
				ImGui::CloseCurrentPopup();
			}
			ImGui::SameLine();
			if (ImGui::Button("否"))
				ImGui::CloseCurrentPopup();
			ImGui::PopStyleVar(2);

			ImGui::EndPopup();
		}
		ImGui::PopStyleVar(1);

		ImGui::SameLine();

		const char* items[] = { "Dreamcast 控制器", "街机控制器" };

		if (last_item_current_map_idx == 2 && game_started)
			// Select the right mappings for the current game
			item_current_map_idx = settings.platform.isArcade() ? 1 : 0;

		// Here our selection data is an index.

		ImGui::SetNextItemWidth(comboWidth);
		// Make the combo height the same as the Done and Reset buttons
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (30 * scaling - ImGui::GetFontSize()) / 2));
		ImGui::Combo("##arcadeMode", &item_current_map_idx, items, IM_ARRAYSIZE(items));
		ImGui::PopStyleVar();
		if (last_item_current_map_idx != 2 && item_current_map_idx != last_item_current_map_idx)
		{
			gamepad->save_mapping(map_system);
		}
		const Mapping *systemMapping = dcButtons;
		if (item_current_map_idx == 0)
		{
			arcade_button_mode = false;
			map_system = DC_PLATFORM_DREAMCAST;
			systemMapping = dcButtons;
		}
		else if (item_current_map_idx == 1)
		{
			arcade_button_mode = true;
			map_system = DC_PLATFORM_NAOMI;
			systemMapping = arcadeButtons;
		}

		if (item_current_map_idx != last_item_current_map_idx)
		{
			if (!gamepad->find_mapping(map_system))
				if (map_system == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
					gamepad->resetMappingToDefault(arcade_button_mode, true);
			input_mapping = gamepad->get_input_mapping();

			last_item_current_map_idx = item_current_map_idx;
		}

		char key_id[32];

		ImGui::BeginChild(ImGui::GetID("buttons"), ImVec2(0, 0), ImGuiChildFlags_FrameStyle, ImGuiWindowFlags_DragScrolling | ImGuiWindowFlags_NavFlattened);

		for (; systemMapping->name != nullptr; systemMapping++)
		{
			if (systemMapping->key == EMU_BTN_NONE)
			{
				ImGui::Columns(1, nullptr, false);
				header(systemMapping->name);
				ImGui::Columns(3, "bindings", false);
				ImGui::SetColumnWidth(0, col_width);
				ImGui::SetColumnWidth(1, col_width);
				continue;
			}
			sprintf(key_id, "key_id%d", systemMapping->key);
			ImGui::PushID(key_id);

			const char *game_btn_name = nullptr;
			if (arcade_button_mode)
			{
				game_btn_name = GetCurrentGameButtonName(systemMapping->key);
				if (game_btn_name == nullptr)
					game_btn_name = GetCurrentGameAxisName(systemMapping->key);
			}
			if (game_btn_name != nullptr && game_btn_name[0] != '\0')
				ImGui::Text("%s - %s", systemMapping->name, game_btn_name);
			else
				ImGui::Text("%s", systemMapping->name);

			ImGui::NextColumn();
			displayMappedControl(gamepad, systemMapping->key);

			ImGui::NextColumn();
			if (ImGui::Button("Map"))
			{
				map_start_time = os_GetSeconds();
				ImGui::OpenPopup("映射控制器");
				mapped_device = gamepad;
				mapped_code = -1;
				gamepad->detectButtonOrAxisInput([](u32 code, bool analog, bool positive)
						{
							mapped_code = code;
							analogAxis = analog;
							positiveDirection = positive;
						});
			}
			detect_input_popup(systemMapping);
			ImGui::SameLine();
			if (ImGui::Button("取消映射"))
			{
				input_mapping = gamepad->get_input_mapping();
				unmapControl(input_mapping, gamepad_port, systemMapping->key);
			}
			ImGui::NextColumn();
			ImGui::PopID();
		}
		ImGui::Columns(1, nullptr, false);
	    scrollWhenDraggingOnVoid();
	    windowDragScroll();

		ImGui::EndChild();
		error_popup();
		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

static void gamepadSettingsPopup(const std::shared_ptr<GamepadDevice>& gamepad)
{
	centerNextWindow();
	ImGui::SetNextWindowSize(min(ImGui::GetIO().DisplaySize, ScaledVec2(450.f, 300.f)));

	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	if (ImGui::BeginPopupModal("游戏手柄设置", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
	{
		if (ImGui::Button("完成", ScaledVec2(100, 30)))
		{
			gamepad->save_mapping();
			// Update both console and arcade profile/mapping
			int rumblePower = gamepad->get_rumble_power();
			float deadzone = gamepad->get_dead_zone();
			float saturation = gamepad->get_saturation();
			int otherPlatform = settings.platform.isConsole() ? DC_PLATFORM_NAOMI : DC_PLATFORM_DREAMCAST;
			if (!gamepad->find_mapping(otherPlatform))
				if (otherPlatform == DC_PLATFORM_DREAMCAST || !gamepad->find_mapping(DC_PLATFORM_DREAMCAST))
					gamepad->resetMappingToDefault(otherPlatform != DC_PLATFORM_DREAMCAST, true);
			std::shared_ptr<InputMapping> mapping = gamepad->get_input_mapping();
			if (mapping != nullptr)
			{
				if (gamepad->is_rumble_enabled() && rumblePower != mapping->rumblePower) {
					mapping->rumblePower = rumblePower;
					mapping->set_dirty();
				}
				if (gamepad->has_analog_stick())
				{
					if (deadzone != mapping->dead_zone) {
						mapping->dead_zone = deadzone;
						mapping->set_dirty();
					}
					if (saturation != mapping->saturation) {
						mapping->saturation = saturation;
						mapping->set_dirty();
					}
				}
				if (mapping->is_dirty())
					gamepad->save_mapping(otherPlatform);
			}
			gamepad->find_mapping();

			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			ImGui::PopStyleVar();
			return;
		}
		ImGui::NewLine();
		if (gamepad->is_virtual_gamepad())
		{
			header("Haptic");
			OptionSlider("Power", config::VirtualGamepadVibration, 0, 60, "Haptic feedback power");
		}
		else if (gamepad->is_rumble_enabled())
		{
			header("Rumble");
			int power = gamepad->get_rumble_power();
			ImGui::SetNextItemWidth(300 * settings.display.uiScale);
			if (ImGui::SliderInt("Power", &power, 0, 100, "%d%%"))
				gamepad->set_rumble_power(power);
			ImGui::SameLine();
			ShowHelpMarker("Rumble power");
		}
		if (gamepad->has_analog_stick())
		{
			header("Thumbsticks");
			int deadzone = std::round(gamepad->get_dead_zone() * 100.f);
			ImGui::SetNextItemWidth(300 * settings.display.uiScale);
			if (ImGui::SliderInt("Dead zone", &deadzone, 0, 100, "%d%%"))
				gamepad->set_dead_zone(deadzone / 100.f);
			ImGui::SameLine();
			ShowHelpMarker("Minimum deflection to register as input");
			int saturation = std::round(gamepad->get_saturation() * 100.f);
			ImGui::SetNextItemWidth(300 * settings.display.uiScale);
			if (ImGui::SliderInt("Saturation", &saturation, 50, 200, "%d%%"))
				gamepad->set_saturation(saturation / 100.f);
			ImGui::SameLine();
			ShowHelpMarker("在拇指摇杆完全偏转（偏移到最大角度）时发送给游戏的值 "
					"在拇指摇杆完全偏转之前，大于100%的值会达到饱和");
		}

		ImGui::EndPopup();
	}
	ImGui::PopStyleVar();
}

void error_popup()
{
	if (!error_msg_shown && !error_msg.empty())
	{
		ImVec2 padding = ScaledVec2(20, 20);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, padding);
		ImGui::OpenPopup("错误");
		if (ImGui::BeginPopupModal("错误", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 400.f * settings.display.uiScale);
			ImGui::TextWrapped("%s", error_msg.c_str());
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
			float currentwidth = ImGui::GetContentRegionAvail().x;
			ImGui::SetCursorPosX((currentwidth - 80.f * settings.display.uiScale) / 2.f + ImGui::GetStyle().WindowPadding.x);
			if (ImGui::Button("好", ScaledVec2(80.f, 0)))
			{
				error_msg.clear();
				ImGui::CloseCurrentPopup();
			}
			ImGui::SetItemDefaultFocus();
			ImGui::PopStyleVar();
			ImGui::PopTextWrapPos();
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar();
		ImGui::PopStyleVar();
		error_msg_shown = true;
	}
}

static void contentpath_warning_popup()
{
    static bool show_contentpath_selection;

    if (scanner.content_path_looks_incorrect)
    {
        ImGui::OpenPopup("内容位置不正确？");
        if (ImGui::BeginPopupModal("内容位置不正确？", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + 400.f * settings.display.uiScale);
            ImGui::TextWrapped("  扫描了 %d 个文件夹，但未找到任何游戏！  ", scanner.empty_folders_scanned);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
            float currentwidth = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX((currentwidth - 100.f * settings.display.uiScale) / 2.f + ImGui::GetStyle().WindowPadding.x - 55.f * settings.display.uiScale);
            if (ImGui::Button("重选", ScaledVec2(100.f, 0)))
            {
            	scanner.content_path_looks_incorrect = false;
                ImGui::CloseCurrentPopup();
                show_contentpath_selection = true;
            }

            ImGui::SameLine();
            ImGui::SetCursorPosX((currentwidth - 100.f * settings.display.uiScale) / 2.f + ImGui::GetStyle().WindowPadding.x + 55.f * settings.display.uiScale);
            if (ImGui::Button("取消", ScaledVec2(100.f, 0)))
            {
            	scanner.content_path_looks_incorrect = false;
                ImGui::CloseCurrentPopup();
                scanner.stop();
                config::ContentPath.get().clear();
            }
            ImGui::SetItemDefaultFocus();
            ImGui::PopStyleVar();
            ImGui::EndPopup();
        }
    }
    if (show_contentpath_selection)
    {
        scanner.stop();
        ImGui::OpenPopup("选择目录");
        select_file_popup("选择目录", [](bool cancelled, std::string selection)
        {
            show_contentpath_selection = false;
            if (!cancelled)
            {
            	config::ContentPath.get().clear();
                config::ContentPath.get().push_back(selection);
            }
            scanner.refresh();
            return true;
        });
    }
}

static inline void gui_debug_tab()
{
	if (ImGui::BeginTabItem("调试"))
	{
		ImVec2 normal_padding = ImGui::GetStyle().FramePadding;
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
	    header("记录中");
	    {
	    	LogManager *logManager = LogManager::GetInstance();
	    	for (LogTypes::LOG_TYPE type = LogTypes::AICA; type < LogTypes::NUMBER_OF_LOGS; type = (LogTypes::LOG_TYPE)(type + 1))
	    	{
				bool enabled = logManager->IsEnabled(type, logManager->GetLogLevel());
				std::string name = std::string(logManager->GetShortName(type)) + " - " + logManager->GetFullName(type);
				if (ImGui::Checkbox(name.c_str(), &enabled) && logManager->GetLogLevel() > LogTypes::LWARNING) {
					logManager->SetEnable(type, enabled);
					cfgSaveBool("log", logManager->GetShortName(type), enabled);
				}
	    	}
	    	ImGui::Spacing();

	    	static const char *levels[] = { "Notice", "Error", "Warning", "Info", "Debug" };
	    	if (ImGui::BeginCombo("Log Verbosity", levels[logManager->GetLogLevel() - 1], ImGuiComboFlags_None))
	    	{
	    		for (std::size_t i = 0; i < std::size(levels); i++)
	    		{
	    			bool is_selected = logManager->GetLogLevel() - 1 == (int)i;
	    			if (ImGui::Selectable(levels[i], &is_selected)) {
	    				logManager->SetLogLevel((LogTypes::LOG_LEVELS)(i + 1));
						cfgSaveInt("log", "Verbosity", i + 1);
	    			}
	                if (is_selected)
	                    ImGui::SetItemDefaultFocus();
	    		}
	    		ImGui::EndCombo();
	    	}
	    }
#if FC_PROFILER
    	ImGui::Spacing();
	    header("Profiling");
	    {

			OptionCheckbox("启用", config::ProfilerEnabled, "启用性能分析器");
			if (!config::ProfilerEnabled)
			{
				ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
			}
			OptionCheckbox("显示", config::ProfilerDrawToGUI, "将性能分析器的输出以叠加层的形式绘制出来");
			OptionCheckbox("输出到终端", config::ProfilerOutputTTY, "将性能分析器的输出写入终端");
			// TODO frame warning time
			if (!config::ProfilerEnabled)
			{
		        ImGui::PopItemFlag();
		        ImGui::PopStyleVar();
			}
	    }
#endif
		ImGui::PopStyleVar();
		ImGui::EndTabItem();
	}
}

static void addContentPath(const std::string& path)
{
	auto& contentPath = config::ContentPath.get();
	if (std::count(contentPath.begin(), contentPath.end(), path) == 0)
	{
		scanner.stop();
		contentPath.push_back(path);
		scanner.refresh();
	}
}

static float calcComboWidth(const char *biggestLabel) {
	return ImGui::CalcTextSize(biggestLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
}

static void gui_display_settings()
{
	static bool maple_devices_changed;

	fullScreenWindow(false);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);

    ImGui::Begin("设置", NULL, ImGuiWindowFlags_DragScrolling | ImGuiWindowFlags_NoResize
    		| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
	ImVec2 normal_padding = ImGui::GetStyle().FramePadding;

    if (ImGui::Button("完成", ScaledVec2(100, 30)))
    {
    	if (uiUserScaleUpdated)
    	{
    		uiUserScaleUpdated = false;
    		mainui_reinit();
    	}
    	if (game_started)
    		gui_setState(GuiState::Commands);
    	else
    		gui_setState(GuiState::Main);
    	if (maple_devices_changed)
    	{
    		maple_devices_changed = false;
    		if (game_started && settings.platform.isConsole())
    		{
    			maple_ReconnectDevices();
    			reset_vmus();
    		}
    	}
       	SaveSettings();
    }
	if (game_started)
	{
	    ImGui::SameLine();
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16 * settings.display.uiScale, normal_padding.y));
		if (config::Settings::instance().hasPerGameConfig())
		{
			if (ImGui::Button("删除游戏配置", ScaledVec2(0, 30)))
			{
				config::Settings::instance().setPerGameConfig(false);
				config::Settings::instance().load(false);
				loadGameSpecificSettings();
			}
		}
		else
		{
			if (ImGui::Button("生成游戏配置", ScaledVec2(0, 30)))
				config::Settings::instance().setPerGameConfig(true);
		}
	    ImGui::PopStyleVar();
	}

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16, 6));

    if (ImGui::BeginTabBar("settings", ImGuiTabBarFlags_NoTooltip))
    {
		if (ImGui::BeginTabItem("通用"))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
			{
				DisabledScope scope(settings.platform.isArcade());

				const char *languages[] = { "日语", "英语", "德语", "法语", "西班牙语", "意大利语", "默认" };
				OptionComboBox("语言", config::Language, languages, std::size(languages),
					"在Dreamcast BIOS中配置语言");

				const char *broadcast[] = { "NTSC", "PAL", "PAL/M", "PAL/N", "默认" };
				OptionComboBox("制式", config::Broadcast, broadcast, std::size(broadcast),
						"非VGA模式的电视广播标准");
			}

			const char *consoleRegion[] = { "日本", "美国", "欧洲", "默认" };
			const char *arcadeRegion[] = { "日本", "美国", "Export", "Korea" };
			const char **region = settings.platform.isArcade() ? arcadeRegion : consoleRegion;
			OptionComboBox("区域", config::Region, region, std::size(consoleRegion),
						"BIOS区域");

			const char *cable[] = { "VGA", "RGB分量", "TV复合信号" };
			{
				DisabledScope scope(config::Cable.isReadOnly() || settings.platform.isArcade());

				const char *value = config::Cable == 0 ? cable[0]
						: config::Cable > 0 && config::Cable <= (int)std::size(cable) ? cable[config::Cable - 1]
						: "?";
				if (ImGui::BeginCombo("线缆类型", value, ImGuiComboFlags_None))
				{
					for (int i = 0; i < IM_ARRAYSIZE(cable); i++)
					{
						bool is_selected = i == 0 ? config::Cable <= 1 : config::Cable - 1 == i;
						if (ImGui::Selectable(cable[i], &is_selected))
							config::Cable = i == 0 ? 0 : i + 1;
						if (is_selected)
							ImGui::SetItemDefaultFocus();
					}
					ImGui::EndCombo();
				}
	            ImGui::SameLine();
	            ShowHelpMarker("视频连接类型");
			}

#if !defined(TARGET_IPHONE)
            ImVec2 size;
            size.x = 0.0f;
            size.y = (ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2.f)
            				* (config::ContentPath.get().size() + 1) ;//+ ImGui::GetStyle().FramePadding.y * 2.f;

            if (BeginListBox("游戏路径", size, ImGuiWindowFlags_NavFlattened))
            {
            	int to_delete = -1;
                for (u32 i = 0; i < config::ContentPath.get().size(); i++)
                {
                	ImGui::PushID(config::ContentPath.get()[i].c_str());
                    ImGui::AlignTextToFramePadding();
                	ImGui::Text("%s", config::ContentPath.get()[i].c_str());
                	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("X").x - ImGui::GetStyle().FramePadding.x);
                	if (ImGui::Button("X"))
                		to_delete = i;
                	ImGui::PopID();
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(24, 3));
#ifdef __ANDROID__
                if (ImGui::Button("添加"))
                {
                	hostfs::addStorage(true, false, [](bool cancelled, std::string selection) {
            			if (!cancelled)
            				addContentPath(selection);
                	});
                }
#else
                if (ImGui::Button("添加"))
                	ImGui::OpenPopup("选择目录");
                select_file_popup("选择目录", [](bool cancelled, std::string selection) {
					if (!cancelled)
        				addContentPath(selection);
					return true;
                });
#endif
                ImGui::SameLine();
    			if (ImGui::Button("重新扫描"))
    				scanner.refresh();
                ImGui::PopStyleVar();
                scrollWhenDraggingOnVoid();

        		ImGui::EndListBox();
            	if (to_delete >= 0)
            	{
            		scanner.stop();
            		config::ContentPath.get().erase(config::ContentPath.get().begin() + to_delete);
        			scanner.refresh();
            	}
            }
            ImGui::SameLine();
            ShowHelpMarker("游戏存储目录");

            size.y = ImGui::GetTextLineHeightWithSpacing() * 1.25f + ImGui::GetStyle().FramePadding.y * 2.0f;

#if defined(__linux__) && !defined(__ANDROID__)
            if (BeginListBox("数据目录", size, ImGuiWindowFlags_NavFlattened))
            {
            	ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", get_writable_data_path("").c_str());
                ImGui::EndListBox();
            }
            ImGui::SameLine();
            ShowHelpMarker("包含BIOS文件以及保存的VMU和状态的目录");
#else
            if (BeginListBox("主页目录", size, ImGuiWindowFlags_NavFlattened))
            {
            	ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", get_writable_config_path("").c_str());
#ifdef __ANDROID__
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("更改").x - ImGui::GetStyle().FramePadding.x);
                if (ImGui::Button("更改"))
                	gui_setState(GuiState::Onboarding);
#endif
#ifdef TARGET_MAC
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("在Finder中显示").x - ImGui::GetStyle().FramePadding.x);
                if (ImGui::Button("在Finder中显示"))
                {
                    char temp[512];
                    sprintf(temp, "打开 \"%s\"", get_writable_config_path("").c_str());
                    system(temp);
                }
#endif
                ImGui::EndListBox();
            }
            ImGui::SameLine();
            ShowHelpMarker("Flycast保存配置文件和VMU的目录。BIOS文件应存放在名为 \"data\"的子文件夹中");
#endif // !linux
#endif // !TARGET_IPHONE

			OptionCheckbox("盒装封面游戏列表", config::BoxartDisplayMode,
					"在游戏列表中显示游戏封面");
			OptionCheckbox("下载盒装封面", config::FetchBoxart,
					"从TheGamesDB.net获取封面图片");
			if (OptionSlider("界面比例", config::UIScaling, 50, 200, "调整用户界面元素和字体的大小", "%d%%"))
				uiUserScaleUpdated = true;
			if (uiUserScaleUpdated)
			{
				ImGui::SameLine();
				if (ImGui::Button("Apply")) {
					mainui_reinit();
					uiUserScaleUpdated = false;
				}
			}

			if (OptionCheckbox("隐藏旧版Naomi Roms", config::HideLegacyNaomiRoms,
					"从内容浏览器中隐藏 .bin、.dat 和 .lst 文件"))
				scanner.refresh();
	    	ImGui::Text("自动存档状态：");
			OptionCheckbox("加载", config::AutoLoadState,
					"启动时加载游戏上次保存的状态");
			ImGui::SameLine();
			OptionCheckbox("保存", config::AutoSaveState,
					"停止时保存游戏状态");
			OptionCheckbox("Naomi自由游玩", config::ForceFreePlay, "配置Naomi游戏为自由游玩模式。");

			ImGui::PopStyleVar();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("控制器"))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
			header("物理设备");
		    {
				if (ImGui::BeginTable("physicalDevices", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings))
				{
					ImGui::TableSetupColumn("系统", ImGuiTableColumnFlags_WidthFixed);
					ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("端口", ImGuiTableColumnFlags_WidthFixed);
					ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);

					const float portComboWidth = calcComboWidth("无");
					const ImVec4 gray{ 0.5f, 0.5f, 0.5f, 1.f };

					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextColored(gray, "系统");

					ImGui::TableSetColumnIndex(1);
					ImGui::TextColored(gray, "名称");

					ImGui::TableSetColumnIndex(2);
					ImGui::TextColored(gray, "端口");

					for (int i = 0; i < GamepadDevice::GetGamepadCount(); i++)
					{
						std::shared_ptr<GamepadDevice> gamepad = GamepadDevice::GetGamepad(i);
						if (!gamepad)
							continue;
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::Text("%s", gamepad->api_name().c_str());

						ImGui::TableSetColumnIndex(1);
						ImGui::Text("%s", gamepad->name().c_str());

						ImGui::TableSetColumnIndex(2);
						char port_name[32];
						sprintf(port_name, "##mapleport%d", i);
						ImGui::PushID(port_name);
						ImGui::SetNextItemWidth(portComboWidth);
						if (ImGui::BeginCombo(port_name, maple_ports[gamepad->maple_port() + 1]))
						{
							for (int j = -1; j < (int)std::size(maple_ports) - 1; j++)
							{
								bool is_selected = gamepad->maple_port() == j;
								if (ImGui::Selectable(maple_ports[j + 1], &is_selected))
									gamepad->set_maple_port(j);
								if (is_selected)
									ImGui::SetItemDefaultFocus();
							}

							ImGui::EndCombo();
						}

						ImGui::TableSetColumnIndex(3);
						ImGui::SameLine(0, 8 * settings.display.uiScale);
						if (gamepad->remappable() && ImGui::Button("映射"))
						{
							gamepad_port = 0;
							ImGui::OpenPopup("按键映射");
						}

						controller_mapping_popup(gamepad);

#ifdef __ANDROID__
						if (gamepad->is_virtual_gamepad())
						{
							if (ImGui::Button("编辑布局"))
							{
								vjoy_start_editing();
								gui_setState(GuiState::VJoyEdit);
							}
						}
#endif
						if (gamepad->is_rumble_enabled() || gamepad->has_analog_stick()
#ifdef __ANDROID__
							|| gamepad->is_virtual_gamepad()
#endif
							)
						{
							ImGui::SameLine(0, 16 * settings.display.uiScale);
							if (ImGui::Button("设置"))
								ImGui::OpenPopup("游戏手柄设置");
							gamepadSettingsPopup(gamepad);
						}
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
		    }

	    	ImGui::Spacing();
	    	OptionSlider("鼠标灵敏度", config::MouseSensitivity, 1, 500);
#if defined(_WIN32) && !defined(TARGET_UWP)
	    	OptionCheckbox("“使用原始输入", config::UseRawInput, "支持多个指向设备（鼠标、光枪）和键盘");
#endif

			ImGui::Spacing();
			header("Dreamcast设备");
		    {
				bool is_there_any_xhair = false;
				if (ImGui::BeginTable("dreamcastDevices", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings,
						ImVec2(0, 0), 8 * settings.display.uiScale))
				{
					const float mainComboWidth = calcComboWidth(maple_device_types[11]); 			// densha de go! controller
					const float expComboWidth = calcComboWidth(maple_expansion_device_types[2]);	// vibration pack

					for (int bus = 0; bus < MAPLE_PORTS; bus++)
					{
						ImGui::TableNextRow();
						ImGui::TableSetColumnIndex(0);
						ImGui::Text("端口 %c", bus + 'A');

						ImGui::TableSetColumnIndex(1);
						char device_name[32];
						sprintf(device_name, "##device%d", bus);
						float w = ImGui::CalcItemWidth() / 3;
						ImGui::PushItemWidth(w);
						ImGui::SetNextItemWidth(mainComboWidth);
						if (ImGui::BeginCombo(device_name, maple_device_name(config::MapleMainDevices[bus]), ImGuiComboFlags_None))
						{
							for (int i = 0; i < IM_ARRAYSIZE(maple_device_types); i++)
							{
								bool is_selected = config::MapleMainDevices[bus] == maple_device_type_from_index(i);
								if (ImGui::Selectable(maple_device_types[i], &is_selected))
								{
									config::MapleMainDevices[bus] = maple_device_type_from_index(i);
									maple_devices_changed = true;
								}
								if (is_selected)
									ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
						int port_count = 0;
						switch (config::MapleMainDevices[bus]) {
							case MDT_SegaController:
								port_count = 2;
								break;
							case MDT_LightGun:
							case MDT_TwinStick:
							case MDT_AsciiStick:
							case MDT_RacingController:
								port_count = 1;
								break;
							default: break;
						}
						for (int port = 0; port < port_count; port++)
						{
							ImGui::TableSetColumnIndex(2 + port);
							sprintf(device_name, "##device%d.%d", bus, port + 1);
							ImGui::PushID(device_name);
							ImGui::SetNextItemWidth(expComboWidth);
							if (ImGui::BeginCombo(device_name, maple_expansion_device_name(config::MapleExpansionDevices[bus][port]), ImGuiComboFlags_None))
							{
								for (int i = 0; i < IM_ARRAYSIZE(maple_expansion_device_types); i++)
								{
									bool is_selected = config::MapleExpansionDevices[bus][port] == maple_expansion_device_type_from_index(i);
									if (ImGui::Selectable(maple_expansion_device_types[i], &is_selected))
									{
										config::MapleExpansionDevices[bus][port] = maple_expansion_device_type_from_index(i);
										maple_devices_changed = true;
									}
									if (is_selected)
										ImGui::SetItemDefaultFocus();
								}
								ImGui::EndCombo();
							}
							ImGui::PopID();
						}
						if (config::MapleMainDevices[bus] == MDT_LightGun)
						{
							ImGui::TableSetColumnIndex(3);
							sprintf(device_name, "##device%d.xhair", bus);
							ImGui::PushID(device_name);
							u32 color = config::CrosshairColor[bus];
							float xhairColor[4] {
								(color & 0xff) / 255.f,
								((color >> 8) & 0xff) / 255.f,
								((color >> 16) & 0xff) / 255.f,
								((color >> 24) & 0xff) / 255.f
							};
							bool colorChanged = ImGui::ColorEdit4("Crosshair color", xhairColor, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf
									| ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoLabel);
							ImGui::SameLine();
							bool enabled = color != 0;
							if (ImGui::Checkbox("Crosshair", &enabled) || colorChanged)
							{
								if (enabled)
								{
									config::CrosshairColor[bus] = (u8)(std::round(xhairColor[0] * 255.f))
											| ((u8)(std::round(xhairColor[1] * 255.f)) << 8)
											| ((u8)(std::round(xhairColor[2] * 255.f)) << 16)
											| ((u8)(std::round(xhairColor[3] * 255.f)) << 24);
									if (config::CrosshairColor[bus] == 0)
										config::CrosshairColor[bus] = 0xC0FFFFFF;
								}
								else
								{
									config::CrosshairColor[bus] = 0;
								}
							}
							is_there_any_xhair |= enabled;
							ImGui::PopID();
						}
						ImGui::PopItemWidth();
					}
					ImGui::EndTable();
				}
				{
					DisabledScope scope(!is_there_any_xhair);
					OptionSlider("准星大小", config::CrosshairSize, 10, 100);
				}
				OptionCheckbox("每款游戏专用的VMU A1", config::PerGameVmu, "当启用时，每款游戏在控制器A的端口1上都有自己的VMU。");
		    }

			ImGui::PopStyleVar();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("视频"))
		{
			int renderApi;
			bool perPixel;
			switch (config::RendererType)
			{
			default:
			case RenderType::OpenGL:
				renderApi = 0;
				perPixel = false;
				break;
			case RenderType::OpenGL_OIT:
				renderApi = 0;
				perPixel = true;
				break;
			case RenderType::Vulkan:
				renderApi = 1;
				perPixel = false;
				break;
			case RenderType::Vulkan_OIT:
				renderApi = 1;
				perPixel = true;
				break;
			case RenderType::DirectX9:
				renderApi = 2;
				perPixel = false;
				break;
			case RenderType::DirectX11:
				renderApi = 3;
				perPixel = false;
				break;
			case RenderType::DirectX11_OIT:
				renderApi = 3;
				perPixel = true;
				break;
			}

			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
			const bool has_per_pixel = GraphicsContext::Instance()->hasPerPixel();
		    header("透明排序");
		    {
		    	int renderer = perPixel ? 2 : config::PerStripSorting ? 1 : 0;
		    	ImGui::Columns(has_per_pixel ? 3 : 2, "renderers", false);
		    	ImGui::RadioButton("逐三角形", &renderer, 0);
	            ImGui::SameLine();
	            ShowHelpMarker("按三角形对透明多边形进行排序。速度快但可能会产生图形错误");
            	ImGui::NextColumn();
		    	ImGui::RadioButton("逐条带", &renderer, 1);
	            ImGui::SameLine();
	            ShowHelpMarker("按条带对透明多边形进行排序。速度更快但可能会产生图形错误");
	            if (has_per_pixel)
	            {
	            	ImGui::NextColumn();
	            	ImGui::RadioButton("逐像素", &renderer, 2);
	            	ImGui::SameLine();
	            	ShowHelpMarker("按像素对透明多边形进行排序。速度较慢但准确");
	            }
		    	ImGui::Columns(1, NULL, false);
		    	switch (renderer)
		    	{
		    	case 0:
		    		perPixel = false;
		    		config::PerStripSorting.set(false);
		    		break;
		    	case 1:
		    		perPixel = false;
		    		config::PerStripSorting.set(true);
		    		break;
		    	case 2:
		    		perPixel = true;
		    		break;
		    	}
		    }
	    	ImGui::Spacing();
            ImGuiStyle& style = ImGui::GetStyle();
            float innerSpacing = style.ItemInnerSpacing.x;

		    header("渲染设置");
		    {
		    	ImGui::Text("自动跳帧：");
		    	ImGui::Columns(3, "autoskip", false);
		    	OptionRadioButton("禁用", config::AutoSkipFrame, 0, "不跳帧");
            	ImGui::NextColumn();
		    	OptionRadioButton("普通", config::AutoSkipFrame, 1, "当GPU和CPU都运行缓慢时跳过一帧");
            	ImGui::NextColumn();
		    	OptionRadioButton("最大", config::AutoSkipFrame, 2, "当GPU运行缓慢时跳过一帧");
		    	ImGui::Columns(1, nullptr, false);

		    	OptionCheckbox("阴影", config::ModifierVolumes,
		    			"启用修改器体积，通常用于阴影");
		    	OptionCheckbox("雾化", config::Fog, "启用雾化效果");
		    	OptionCheckbox("宽屏", config::Widescreen,
		    			"在正常的4:3画面比例之外绘制几何图形。可能会在暴露的区域产生图形错误");
				{
					DisabledScope scope(!config::Widescreen);

					ImGui::Indent();
					OptionCheckbox("超宽屏", config::SuperWidescreen,
							"当屏幕或窗口的宽高比大于16:9时，使用其全部宽度\n保持画面填充并去除黑边。");
					ImGui::Unindent();
		    	}
		    	OptionCheckbox("宽屏游戏金手指", config::WidescreenGameHacks,
		    			"修改游戏，使其以16:9的变形宽银幕格式显示，并使用水平屏幕拉伸。仅部分游戏支持此功能。");

				const std::array<int, 5> aniso{ 1, 2, 4, 8, 16 };
	            const std::array<std::string, 5> anisoText{ "禁用", "2x", "4x", "8x", "16x" };
	            u32 afSelected = 0;
	            for (u32 i = 0; i < aniso.size(); i++)
	            {
	            	if (aniso[i] == config::AnisotropicFiltering)
	            		afSelected = i;
	            }

                ImGui::PushItemWidth(ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f);
                if (ImGui::BeginCombo("##各向异性过滤", anisoText[afSelected].c_str(), ImGuiComboFlags_NoArrowButton))
                {
                	for (u32 i = 0; i < aniso.size(); i++)
                    {
                        bool is_selected = aniso[i] == config::AnisotropicFiltering;
                        if (ImGui::Selectable(anisoText[i].c_str(), is_selected))
                        	config::AnisotropicFiltering = aniso[i];
                        if (is_selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
                ImGui::SameLine(0, innerSpacing);

                if (ImGui::ArrowButton("##降低各向异性过滤", ImGuiDir_Left))
                {
                    if (afSelected > 0)
                    	config::AnisotropicFiltering = aniso[afSelected - 1];
                }
                ImGui::SameLine(0, innerSpacing);
                if (ImGui::ArrowButton("##增加各向异性过滤", ImGuiDir_Right))
                {
                    if (afSelected < aniso.size() - 1)
                    	config::AnisotropicFiltering = aniso[afSelected + 1];
                }
                ImGui::SameLine(0, style.ItemInnerSpacing.x);

                ImGui::Text("各向异性过滤");
                ImGui::SameLine();
                ShowHelpMarker("更高的各向异性过滤值可以使从斜角观看的纹理更加清晰，但这对图形处理器（GPU）的要求也更高。这个选项只对使用多级渐远纹理（mipmapped textures）的情况产生明显的影响。");

		    	ImGui::Text("纹理过滤");
		    	ImGui::Columns(3, "textureFiltering", false);
		    	OptionRadioButton("默认", config::TextureFiltering, 0, "使用游戏默认的纹理过滤");
            	ImGui::NextColumn();
		    	OptionRadioButton("强制使用邻近取样", config::TextureFiltering, 1, "对所有纹理强制使用最近邻滤波。这样会让纹理外观更清晰，但可能会导致各种渲染问题。这个选项通常不会影响性能。");
            	ImGui::NextColumn();
		    	OptionRadioButton("强制使用线性过滤", config::TextureFiltering, 2, "对所有纹理强制使用线性过滤。这样做可以使纹理外观更平滑，但也可能导致各种渲染问题。这个选项通常不会影响性能。");
		    	ImGui::Columns(1, nullptr, false);

#ifndef TARGET_IPHONE
		    	OptionCheckbox("垂直同步", config::VSync, "将帧率与屏幕刷新率同步。");
		    	if (isVulkan(config::RendererType))
		    	{
			    	ImGui::Indent();
					{
						DisabledScope scope(!config::VSync);

						OptionCheckbox("重复帧", config::DupeFrames, "在高刷新率显示器（120 Hz及以上）上重复帧");
			    	}
			    	ImGui::Unindent();
		    	}
#endif
		    	OptionCheckbox("显示帧率计数器（FPS）", config::ShowFPS, "在屏幕上显示每秒帧数计数器");
		    	OptionCheckbox("在游戏中显示VMU", config::FloatVMUs, "在游戏时显示VMU液晶显示屏");
		    	OptionCheckbox("屏幕旋转90度", config::Rotate90, "将屏幕逆时针旋转90度");
		    	OptionCheckbox("延迟帧交换", config::DelayFrameSwapping,
		    			"有助于避免屏幕闪烁或视频卡顿。不推荐在慢速平台上使用。");
		    	OptionCheckbox("修复上采样边缘溢出问题", config::FixUpscaleBleedingEdge,
		    			"在放大时有助于解决纹理渗色问题。如果在2D游戏（如MVC2、CVS、KOF等）中放大时出现像素扭曲的情况，禁用这个功能可能会有所帮助");
		    	OptionCheckbox("原生深度插值", config::NativeDepthInterpolation,
		    			"有助于解决AMD显卡上的纹理损坏和深度问题。在某些情况下，也能帮助Intel显卡");
		    	OptionCheckbox("全帧缓冲模拟", config::EmulateFramebuffer,
		    			"完全准确的VRAM帧缓冲仿真。有助于那些直接访问帧缓冲以实现特殊效果的游戏。 "
		    			"速度非常慢，并且与放大和宽屏不兼容。");
		    	constexpr int apiCount = 0
					#ifdef USE_VULKAN
		    			+ 1
					#endif
					#ifdef USE_DX9
						+ 1
					#endif
					#ifdef USE_OPENGL
						+ 1
					#endif
					#ifdef USE_DX11
						+ 1
					#endif
						;

		    	if (apiCount > 1)
		    	{
		    		ImGui::Text("图形API:");
					ImGui::Columns(apiCount, "renderApi", false);
#ifdef USE_OPENGL
					ImGui::RadioButton("OpenGL", &renderApi, 0);
					ImGui::NextColumn();
#endif
#ifdef USE_VULKAN
#ifdef __APPLE__
					ImGui::RadioButton("Vulkan (Metal)", &renderApi, 1);
					ImGui::SameLine(0, style.ItemInnerSpacing.x);
					ShowHelpMarker("MoltenVK：一个运行在苹果Metal图形框架上的Vulkan实现");
#else
					ImGui::RadioButton("Vulkan", &renderApi, 1);
#endif // __APPLE__
					ImGui::NextColumn();
#endif
#ifdef USE_DX9
					ImGui::RadioButton("DirectX 9", &renderApi, 2);
					ImGui::NextColumn();
#endif
#ifdef USE_DX11
					ImGui::RadioButton("DirectX 11", &renderApi, 3);
					ImGui::NextColumn();
#endif
					ImGui::Columns(1, nullptr, false);
		    	}

	            const std::array<float, 13> scalings{ 0.5f, 1.f, 1.5f, 2.f, 2.5f, 3.f, 4.f, 4.5f, 5.f, 6.f, 7.f, 8.f, 9.f };
	            const std::array<std::string, 13> scalingsText{ "一半", "原生", "x1.5", "x2", "x2.5", "x3", "x4", "x4.5", "x5", "x6", "x7", "x8", "x9" };
	            std::array<int, scalings.size()> vres;
	            std::array<std::string, scalings.size()> resLabels;
	            u32 selected = 0;
	            for (u32 i = 0; i < scalings.size(); i++)
	            {
	            	vres[i] = scalings[i] * 480;
	            	if (vres[i] == config::RenderResolution)
	            		selected = i;
	            	if (!config::Widescreen)
	            		resLabels[i] = std::to_string((int)(scalings[i] * 640)) + "x" + std::to_string((int)(scalings[i] * 480));
	            	else
	            		resLabels[i] = std::to_string((int)(scalings[i] * 480 * 16 / 9)) + "x" + std::to_string((int)(scalings[i] * 480));
	            	resLabels[i] += " (" + scalingsText[i] + ")";
	            }

                ImGui::PushItemWidth(ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f);
                if (ImGui::BeginCombo("##Resolution", resLabels[selected].c_str(), ImGuiComboFlags_NoArrowButton))
                {
                	for (u32 i = 0; i < scalings.size(); i++)
                    {
                        bool is_selected = vres[i] == config::RenderResolution;
                        if (ImGui::Selectable(resLabels[i].c_str(), is_selected))
                        	config::RenderResolution = vres[i];
                        if (is_selected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopItemWidth();
                ImGui::SameLine(0, innerSpacing);

                if (ImGui::ArrowButton("##Decrease Res", ImGuiDir_Left))
                {
                    if (selected > 0)
                    	config::RenderResolution = vres[selected - 1];
                }
                ImGui::SameLine(0, innerSpacing);
                if (ImGui::ArrowButton("##Increase Res", ImGuiDir_Right))
                {
                    if (selected < vres.size() - 1)
                    	config::RenderResolution = vres[selected + 1];
                }
                ImGui::SameLine(0, style.ItemInnerSpacing.x);

                ImGui::Text("内部分辨率");
                ImGui::SameLine();
                ShowHelpMarker("内部渲染分辨率。分辨率越高效果越好，但对图形处理器（GPU）的要求也越高。可以使用高于你的显示器分辨率的值（但不超过显示器分辨率的两倍）来进行超采样，这可以在不降低清晰度的前提下提供高质量的抗锯齿效果");

		    	OptionSlider("水平拉伸", config::ScreenStretching, 100, 250,
		    			"水平拉伸屏幕", "%d%%");
		    	OptionArrowButtons("跳帧", config::SkipFrame, 0, 6,
		    			"在两帧实际渲染的帧之间要跳过的帧数");
		    }
			if (perPixel)
			{
				ImGui::Spacing();
				header("每像素设置");

				const std::array<int64_t, 4> bufSizes{ 512_MB, 1_GB, 2_GB, 4_GB };
				const std::array<std::string, 4> bufSizesText{ "512 MB", "1 GB", "2 GB", "4 GB" };
                ImGui::PushItemWidth(ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f);
				u32 selected = 0;
				for (; selected < bufSizes.size(); selected++)
					if (bufSizes[selected] == config::PixelBufferSize)
						break;
				if (selected == bufSizes.size())
					selected = 0;
				if (ImGui::BeginCombo("##PixelBuffer", bufSizesText[selected].c_str(), ImGuiComboFlags_NoArrowButton))
				{
					for (u32 i = 0; i < bufSizes.size(); i++)
					{
						bool is_selected = i == selected;
						if (ImGui::Selectable(bufSizesText[i].c_str(), is_selected))
							config::PixelBufferSize = bufSizes[i];
						if (is_selected) {
							ImGui::SetItemDefaultFocus();
							selected = i;
						}
					}
					ImGui::EndCombo();
				}
                ImGui::PopItemWidth();
				ImGui::SameLine(0, innerSpacing);

				if (ImGui::ArrowButton("##Decrease BufSize", ImGuiDir_Left))
				{
					if (selected > 0)
						config::PixelBufferSize = bufSizes[selected - 1];
				}
				ImGui::SameLine(0, innerSpacing);
				if (ImGui::ArrowButton("##Increase BufSize", ImGuiDir_Right))
				{
					if (selected < bufSizes.size() - 1)
						config::PixelBufferSize = bufSizes[selected + 1];
				}
				ImGui::SameLine(0, style.ItemInnerSpacing.x);

                ImGui::Text("像素缓冲区大小");
                ImGui::SameLine();
                ShowHelpMarker("像素缓冲区的大小。在进行大比例放大时，可能需要增加其大小。");

                OptionSlider("最大层数", config::PerPixelLayers, 8, 128,
                		"最大透明层数。对于某些复杂的场景，可能需要增加这个数值。减少它可能会提高性能。");
			}
	    	ImGui::Spacing();
		    header("渲染到纹理");
		    {
		    	OptionCheckbox("复制到显存", config::RenderToTextureBuffer,
		    			"将渲染好的纹理复制回显存，速度较慢但准确。");
		    }
	    	ImGui::Spacing();
		    header("纹理放大");
		    {
#ifdef _OPENMP
		    	OptionArrowButtons("纹理放大", config::TextureUpscale, 1, 8,
		    			"使用xBRZ算法放大纹理。仅适用于快速平台和某些2D游戏。", "x%d");
		    	OptionSlider("纹理最大尺寸", config::MaxFilteredTextureSize, 8, 1024,
		    			"纹理的尺寸大于这个维度平方的，将不会被放大。");
		    	OptionArrowButtons("最大线程数", config::MaxThreads, 1, 8,
		    			"用于纹理放大的最大线程数。建议值：物理核心数减一。");
#endif
		    	OptionCheckbox("加载自定义纹理", config::CustomTextures,
		    			"从data/textures/<游戏ID>加载自定义/高分辨率纹理");
		    }
#ifdef VIDEO_ROUTING
#ifdef __APPLE__
			header("视频路由（Syphon）");
#elif defined(_WIN32)
			((renderApi == 0) || (renderApi == 3)) ? header("“视频路由（Syphon）") : header("Video路由（仅适用于OpenGL或DirectX 11）");
#endif
			{
#ifdef _WIN32
				DisabledScope scope(!((renderApi == 0) || (renderApi == 3)));
#endif
				OptionCheckbox("将视频内容发送到另一个程序", config::VideoRouting,
					"例如，直接将 GPU 纹理路由到 OBS Studio，而不是使用对 CPU 要求较高的显示/窗口捕获");

				{
					DisabledScope scope(!config::VideoRouting);
					OptionCheckbox("发送前缩小规模", config::VideoRoutingScale, "共享较小的纹理时可能提升性能，效果因情况而异");
					{
						DisabledScope scope(!config::VideoRoutingScale);
						static int vres = config::VideoRoutingVRes;
						if (ImGui::InputInt("输出垂直分辨率", &vres))
						{
							config::VideoRoutingVRes = vres;
						}
					}
					ImGui::Text("输出纹理大小: %d x %d", config::VideoRoutingScale ? config::VideoRoutingVRes * settings.display.width / settings.display.height : settings.display.width, config::VideoRoutingScale ? config::VideoRoutingVRes : settings.display.height);
				}
			}
#endif
			ImGui::PopStyleVar();
			ImGui::EndTabItem();

		    switch (renderApi)
		    {
		    case 0:
		    	config::RendererType = perPixel ? RenderType::OpenGL_OIT : RenderType::OpenGL;
		    	break;
		    case 1:
		    	config::RendererType = perPixel ? RenderType::Vulkan_OIT : RenderType::Vulkan;
		    	break;
		    case 2:
		    	config::RendererType = RenderType::DirectX9;
		    	break;
		    case 3:
		    	config::RendererType = perPixel ? RenderType::DirectX11_OIT : RenderType::DirectX11;
		    	break;
		    }
		}
		if (ImGui::BeginTabItem("音频"))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
			OptionCheckbox("启用DSP", config::DSPEnabled,
					"启用Dreamcast数字声音处理器。仅建议在快速平台上使用。");
            OptionCheckbox("启用VMU音效", config::VmuSound, "启用时播放VMU蜂鸣声。");

			if (OptionSlider("音量大小", config::AudioVolume, 0, 100, "调整模拟器的音频大小", "%d%%"))
			{
				config::AudioVolume.calcDbPower();
			};
#ifdef __ANDROID__
			if (config::AudioBackend.get() == "auto" || config::AudioBackend.get() == "android")
				OptionCheckbox("自动低延迟", config::AutoLatency,
						"自动设置音频延迟。推荐操作。");
#endif
            if (!config::AutoLatency
            		|| (config::AudioBackend.get() != "auto" && config::AudioBackend.get() != "android"))
            {
				int latency = (int)roundf(config::AudioBufferSize * 1000.f / 44100.f);
				ImGui::SliderInt("Latency", &latency, 12, 512, "%d ms");
				config::AudioBufferSize = (int)roundf(latency * 44100.f / 1000.f);
				ImGui::SameLine();
				ShowHelpMarker("设置最大音频延迟。并非所有音频驱动程序都支持此功能。");
            }

			AudioBackend *backend = nullptr;
			std::string backend_name = config::AudioBackend;
			if (backend_name != "自动")
			{
				backend = AudioBackend::getBackend(config::AudioBackend);
				if (backend != nullptr)
					backend_name = backend->slug;
			}

			AudioBackend *current_backend = backend;
			if (ImGui::BeginCombo("音频驱动", backend_name.c_str(), ImGuiComboFlags_None))
			{
				bool is_selected = (config::AudioBackend.get() == "auto");
				if (ImGui::Selectable("自动 - 自动选择驱动程序", &is_selected))
					config::AudioBackend.set("auto");

				for (u32 i = 0; i < AudioBackend::getCount(); i++)
				{
					backend = AudioBackend::getBackend(i);
					is_selected = (config::AudioBackend.get() == backend->slug);

					if (is_selected)
						current_backend = backend;

					if (ImGui::Selectable((backend->slug + " - " + backend->name).c_str(), &is_selected))
						config::AudioBackend.set(backend->slug);
					if (is_selected)
						ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			ShowHelpMarker("要使用的音频驱动程序");

			if (current_backend != nullptr)
			{
				// get backend specific options
				int option_count;
				const AudioBackend::Option *options = current_backend->getOptions(&option_count);

				for (int o = 0; o < option_count; o++)
				{
					std::string value = cfgLoadStr(current_backend->slug, options->name, "");

					if (options->type == AudioBackend::Option::integer)
					{
						int val = stoi(value);
						if (ImGui::SliderInt(options->caption.c_str(), &val, options->minValue, options->maxValue))
						{
							std::string s = std::to_string(val);
							cfgSaveStr(current_backend->slug, options->name, s);
						}
					}
					else if (options->type == AudioBackend::Option::checkbox)
					{
						bool check = value == "1";
						if (ImGui::Checkbox(options->caption.c_str(), &check))
							cfgSaveStr(current_backend->slug, options->name,
									check ? "1" : "0");
					}
					else if (options->type == AudioBackend::Option::list)
					{
						if (ImGui::BeginCombo(options->caption.c_str(), value.c_str(), ImGuiComboFlags_None))
						{
							bool is_selected = false;
							for (const auto& cur : options->values)
							{
								is_selected = value == cur;
								if (ImGui::Selectable(cur.c_str(), &is_selected))
									cfgSaveStr(current_backend->slug, options->name, cur);

								if (is_selected)
									ImGui::SetItemDefaultFocus();
							}
							ImGui::EndCombo();
						}
					}
					else {
						WARN_LOG(RENDERER, "未知选项");
					}

					options++;
				}
			}

			ImGui::PopStyleVar();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("网络"))
		{
			ImGuiStyle& style = ImGui::GetStyle();
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);

			header("网络类型");
			{
				DisabledScope scope(game_started);

				int netType = 0;
				if (config::GGPOEnable)
					netType = 1;
				else if (config::NetworkEnable)
					netType = 2;
				else if (config::BattleCableEnable)
					netType = 3;
				ImGui::Columns(4, "networkType", false);
				ImGui::RadioButton("禁用", &netType, 0);
				ImGui::NextColumn();
				ImGui::RadioButton("GGPO", &netType, 1);
				ImGui::SameLine(0, style.ItemInnerSpacing.x);
				ShowHelpMarker("使用GGPO启用联网");
				ImGui::NextColumn();
				ImGui::RadioButton("Naomi", &netType, 2);
				ImGui::SameLine(0, style.ItemInnerSpacing.x);
				ShowHelpMarker("为支持的Naomi和Atomiswave游戏启用联网功能");
				ImGui::NextColumn();
				ImGui::RadioButton("Battle Cable", &netType, 3);
				ImGui::SameLine(0, style.ItemInnerSpacing.x);
				ShowHelpMarker("为支持的游戏模拟泰森（战斗）空调制解调器电缆");
				ImGui::Columns(1, nullptr, false);

				config::GGPOEnable = false;
				config::NetworkEnable = false;
				config::BattleCableEnable = false;
				switch (netType) {
				case 1:
					config::GGPOEnable = true;
					break;
				case 2:
					config::NetworkEnable = true;
					break;
				case 3:
					config::BattleCableEnable = true;
					break;
				}
			}
			if (config::GGPOEnable || config::NetworkEnable || config::BattleCableEnable) {
				ImGui::Spacing();
				header("配置");
			}
			{
				if (config::GGPOEnable)
				{
					config::NetworkEnable = false;
					OptionCheckbox("以玩家1的身份进行游戏", config::ActAsServer,
							"取消选择将以玩家2的身份进行游戏");
					char server_name[256];
					strcpy(server_name, config::NetworkServer.get().c_str());
					ImGui::InputText("对等", server_name, sizeof(server_name), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
					ImGui::SameLine();
					ShowHelpMarker("您的对等IP地址和可选端口");
					config::NetworkServer.set(server_name);
					OptionSlider("帧延迟", config::GGPODelay, 0, 20,
						"设置帧延迟，对于ping值大于100毫秒的会话来说，这是建议的做法");

					ImGui::Text("左拇指摇杆:");
					OptionRadioButton<int>("禁用", config::GGPOAnalogAxes, 0, "不使用左拇指摇杆");
					ImGui::SameLine();
					OptionRadioButton<int>("水平轴", config::GGPOAnalogAxes, 1, "仅使用左拇指摇杆的水平轴");
					ImGui::SameLine();
					OptionRadioButton<int>("水平+垂直", config::GGPOAnalogAxes, 2, "使用左拇指摇杆的水平轴和垂直轴");

					OptionCheckbox("启用聊天", config::GGPOChat, "当收到聊天消息时打开聊天窗口");
					if (config::GGPOChat)
					{
						OptionCheckbox("启用聊天窗口超时", config::GGPOChatTimeoutToggle, "20秒后自动关闭聊天窗口");
						if (config::GGPOChatTimeoutToggle)
						{
							char chatTimeout[256];
							sprintf(chatTimeout, "%d", (int)config::GGPOChatTimeout);
							ImGui::InputText("聊天窗口超时 (秒)", chatTimeout, sizeof(chatTimeout), ImGuiInputTextFlags_CharsDecimal, nullptr, nullptr);
							ImGui::SameLine();
							ShowHelpMarker("设置接收到新消息后聊天窗口保持打开的时间");
							config::GGPOChatTimeout.set(atoi(chatTimeout));
						}
					}
					OptionCheckbox("网络统计", config::NetworkStats,
							"在屏幕上显示网络统计信息");
				}
				else if (config::NetworkEnable)
				{
					OptionCheckbox("创建本地服务器", config::ActAsServer,
							"创建一个用于Naomi网络游戏的本地服务器");
					if (!config::ActAsServer)
					{
						char server_name[256];
						strcpy(server_name, config::NetworkServer.get().c_str());
						ImGui::InputText("服务", server_name, sizeof(server_name), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
						ImGui::SameLine();
						ShowHelpMarker("要连接的服务器。留空则会在默认端口上自动查找服务器");
						config::NetworkServer.set(server_name);
					}
					char localPort[256];
					sprintf(localPort, "%d", (int)config::LocalPort);
					ImGui::InputText("本地端口", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal, nullptr, nullptr);
					ImGui::SameLine();
					ShowHelpMarker("使用的本地UDP端口");
					config::LocalPort.set(atoi(localPort));
				}
				else if (config::BattleCableEnable)
				{
					char server_name[256];
					strcpy(server_name, config::NetworkServer.get().c_str());
					ImGui::InputText("对等", server_name, sizeof(server_name), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
					ImGui::SameLine();
					ShowHelpMarker("要连接的对等方。留空以在默认端口上自动查找玩家");
					config::NetworkServer.set(server_name);
					char localPort[256];
					sprintf(localPort, "%d", (int)config::LocalPort);
					ImGui::InputText("本地端口", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal, nullptr, nullptr);
					ImGui::SameLine();
					ShowHelpMarker("使用的本地UDP端口");
					config::LocalPort.set(atoi(localPort));
				}
			}
			ImGui::Spacing();
			header("网络设置");
			{
				OptionCheckbox("启用UPnP", config::EnableUPnP, "为网络对战自动配置您的网络路由器");
				OptionCheckbox("广播数字输出", config::NetworkOutput, "在TCP端口8000上广播数字输出和力反馈状态 "
						"兼容MAME的 \"-output network\" 选项。仅适用于街机游戏。");
				{
					DisabledScope scope(game_started);

					OptionCheckbox("宽带适配器模拟", config::EmulateBBA,
							"模拟以太网宽带适配器（BBA）而不是调制解调器");
				}
			}
#ifdef NAOMI_MULTIBOARD
			ImGui::Spacing();
			header("多屏幕");
			{
				//OptionRadioButton<int>("Disabled", config::MultiboardSlaves, 0, "Multiboard disabled (when optional)");
				OptionRadioButton<int>("1 (Twin)", config::MultiboardSlaves, 1, "一种屏幕配置（F355 双屏）");
				ImGui::SameLine();
				OptionRadioButton<int>("3 (Deluxe)", config::MultiboardSlaves, 2, "三种屏幕配置");
			}
#endif
			ImGui::PopStyleVar();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("高级"))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
		    header("CPU模式");
		    {
				ImGui::Columns(2, "cpu_modes", false);
				OptionRadioButton("动态重编译器", config::DynarecEnabled, true,
					"使用动态重编译器。在大多数情况下推荐使用。");
				ImGui::NextColumn();
				OptionRadioButton("解释器", config::DynarecEnabled, false,
					"使用解释器。虽然速度很慢，但在动态重编译器出现问题时可能有所帮助。");
				ImGui::Columns(1, NULL, false);

				OptionSlider("SH4时钟", config::Sh4Clock, 100, 300,
						"对主要的SH4 CPU进行超频或降频。默认值是200 MHz。使用其他值可能会导致崩溃、冻结或触发意外的核反应。",
						"%d MHz");
		    }
	    	ImGui::Spacing();
		    header("其他");
		    {
		    	OptionCheckbox("HLE BIOS", config::UseReios, "强制使用高级BIOS模拟");
	            OptionCheckbox("多线程模拟", config::ThreadedRendering,
	            		"在不同的线程上运行模拟的CPU和GPU");
#ifndef __ANDROID
	            OptionCheckbox("串行控制台", config::SerialConsole,
	            		"将Dreamcast串行控制台的内容输出到标准输出");
#endif
				{
					DisabledScope scope(game_started);
					OptionCheckbox("Dreamcast 32MB内存", config::RamMod32MB,
						"为Dreamcast启用32MB内存升级模块。可能会影响兼容性");
				}
	            OptionCheckbox("导出纹理", config::DumpTextures,
	            		"将所有纹理导出到data/texdump/<游戏ID>目录下");

	            bool logToFile = cfgLoadBool("log", "LogToFile", false);
	            bool newLogToFile = logToFile;
				ImGui::Checkbox("记录日志到文件", &newLogToFile);
				if (logToFile != newLogToFile)
				{
					cfgSaveBool("log", "LogToFile", newLogToFile);
					LogManager::Shutdown();
					LogManager::Init();
				}
	            ImGui::SameLine();
	            ShowHelpMarker("将调试信息记录到flycast.log文件中");
#ifdef SENTRY_UPLOAD
	            OptionCheckbox("自动报告崩溃", config::UploadCrashLogs,
	            		"自动上传崩溃报告至sentry.io以协助故障排查。不包含个人信息。");
#endif
		    }
			ImGui::PopStyleVar();
			ImGui::EndTabItem();

			#ifdef USE_LUA
			header("Lua 脚本编写");
			{
				char LuaFileName[256];

				strcpy(LuaFileName, config::LuaFileName.get().c_str());
				ImGui::InputText("Lua 文件名", LuaFileName, sizeof(LuaFileName), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
				ImGui::SameLine();
				ShowHelpMarker("指定要使用的Lua文件名。该文件应位于Flycast配置目录中。当留空时，默认使用flycast.lua");
				config::LuaFileName = LuaFileName;

			}
			#endif
		}

#if !defined(NDEBUG) || defined(DEBUGFAST) || FC_PROFILER
		gui_debug_tab();
#endif

		if (ImGui::BeginTabItem("关于"))
		{
			ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, normal_padding);
		    header("Flycast");
		    {
				ImGui::Text("版本号: %s", GIT_VERSION);
				ImGui::Text("Git Hash: %s", GIT_HASH);
				ImGui::Text("构建日期: %s", BUILD_DATE);
		    }
	    	ImGui::Spacing();
		    header("平台信息");
		    {
		    	ImGui::Text("CPU: %s",
#if HOST_CPU == CPU_X86
					"x86"
#elif HOST_CPU == CPU_ARM
					"ARM"
#elif HOST_CPU == CPU_MIPS
					"MIPS"
#elif HOST_CPU == CPU_X64
					"x86/64"
#elif HOST_CPU == CPU_GENERIC
					"Generic"
#elif HOST_CPU == CPU_ARM64
					"ARM64"
#else
					"未知"
#endif
						);
		    	ImGui::Text("操作系统: %s",
#ifdef __ANDROID__
					"Android"
#elif defined(__unix__)
					"Linux"
#elif defined(__APPLE__)
#ifdef TARGET_IPHONE
		    		"iOS"
#else
					"macOS"
#endif
#elif defined(TARGET_UWP)
					"Windows Universal Platform"
#elif defined(_WIN32)
					"Windows"
#elif defined(__SWITCH__)
					"Switch"
#else
					"未知"
#endif
						);
#ifdef TARGET_IPHONE
				const char *getIosJitStatus();
				ImGui::Text("JIT状态: %s", getIosJitStatus());
#endif
		    }
	    	ImGui::Spacing();
	    	if (isOpenGL(config::RendererType))
				header("OpenGL");
	    	else if (isVulkan(config::RendererType))
				header("Vulkan");
	    	else if (isDirectX(config::RendererType))
				header("DirectX");
			ImGui::Text("驱动名称: %s", GraphicsContext::Instance()->getDriverName().c_str());
			ImGui::Text("版本: %s", GraphicsContext::Instance()->getDriverVersion().c_str());
			ImGui::Text("汉化 By 邻家小熊");	
			ImGui::Text("汉化仅供娱乐，请勿用于任何商业用途");
			ImGui::PopStyleVar();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();

    scrollWhenDraggingOnVoid();
    windowDragScroll();
    ImGui::End();
    ImGui::PopStyleVar();
}

void gui_display_notification(const char *msg, int duration)
{
	std::lock_guard<std::mutex> lock(osd_message_mutex);
	osd_message = msg;
	osd_message_end = os_GetSeconds() + (double)duration / 1000.0;
}

static std::string get_notification()
{
	std::lock_guard<std::mutex> lock(osd_message_mutex);
	if (!osd_message.empty() && os_GetSeconds() >= osd_message_end)
		osd_message.clear();
	return osd_message;
}

inline static void gui_display_demo()
{
	ImGui::ShowDemoWindow();
}

static void gameTooltip(const std::string& tip)
{
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(tip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static bool getGameImage(const GameBoxart& art, ImTextureID& textureId, bool allowLoad)
{
	textureId = ImTextureID{};
	if (art.boxartPath.empty())
		return false;

	// Get the boxart texture. Load it if needed.
	textureId = imguiDriver->getTexture(art.boxartPath);
	if (textureId == ImTextureID() && allowLoad)
	{
		int width, height;
		u8 *imgData = loadImage(art.boxartPath, width, height);
		if (imgData != nullptr)
		{
			try {
				textureId = imguiDriver->updateTextureAndAspectRatio(art.boxartPath, imgData, width, height);
			} catch (...) {
				// vulkan can throw during resizing
			}
			free(imgData);
		}
		return true;
	}
	return false;
}

static bool gameImageButton(ImTextureID textureId, const std::string& tooltip, ImVec2 size)
{
	float ar = imguiDriver->getAspectRatio(textureId);
	ImVec2 uv0 { 0.f, 0.f };
	ImVec2 uv1 { 1.f, 1.f };
	if (ar > 1)
	{
		uv0.y = -(ar - 1) / 2;
		uv1.y = 1 + (ar - 1) / 2;
	}
	else if (ar != 0)
	{
		ar = 1 / ar;
		uv0.x = -(ar - 1) / 2;
		uv1.x = 1 + (ar - 1) / 2;
	}
	bool pressed = ImGui::ImageButton("", textureId, size - ImGui::GetStyle().FramePadding * 2, uv0, uv1);
	gameTooltip(tooltip);

    return pressed;
}

static void gui_display_content()
{
	fullScreenWindow(false);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::Begin("##main", NULL, ImGuiWindowFlags_NoDecoration);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(20, 8));
    ImGui::AlignTextToFramePadding();
    ImGui::Indent(10 * settings.display.uiScale);
    ImGui::Text("游戏");
    ImGui::Unindent(10 * settings.display.uiScale);

    static ImGuiTextFilter filter;
#if !defined(__ANDROID__) && !defined(TARGET_IPHONE) && !defined(TARGET_UWP) && !defined(__SWITCH__)
	ImGui::SameLine(0, 32 * settings.display.uiScale);
	filter.Draw("Filter");
#endif
    if (gui_state != GuiState::SelectDisk)
    {
#ifdef TARGET_UWP
    	void gui_load_game();
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Settings").x
				- ImGui::GetStyle().FramePadding.x * 4.0f  - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize("Load...").x);
		if (ImGui::Button("Load..."))
			gui_load_game();
		ImGui::SameLine();
#elif defined(__SWITCH__)
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Settings").x
				- ImGui::GetStyle().FramePadding.x * 4.0f  - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize("Exit").x);
		if (ImGui::Button("Exit"))
			dc_exit();
		ImGui::SameLine();
#else
		ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Settings").x - ImGui::GetStyle().FramePadding.x * 2.0f);
#endif
		if (ImGui::Button("设置"))
			gui_setState(GuiState::Settings);
    }
    ImGui::PopStyleVar();

    scanner.fetch_game_list();

	// Only if Filter and Settings aren't focused... ImGui::SetNextWindowFocus();
	ImGui::BeginChild(ImGui::GetID("library"), ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_DragScrolling | ImGuiWindowFlags_NavFlattened);
    {
		const int itemsPerLine = std::max<int>(ImGui::GetContentRegionMax().x / (150 * settings.display.uiScale + ImGui::GetStyle().ItemSpacing.x), 1);
		const float responsiveBoxSize = ImGui::GetContentRegionMax().x / itemsPerLine - ImGui::GetStyle().FramePadding.x * 2;
		const ImVec2 responsiveBoxVec2 = ImVec2(responsiveBoxSize, responsiveBoxSize);
		
		if (config::BoxartDisplayMode)
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
		else
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(8, 20));

		int counter = 0;
		int loadedImages = 0;
		if (gui_state != GuiState::SelectDisk && filter.PassFilter("Dreamcast BIOS"))
		{
			ImGui::PushID("bios");
			bool pressed;
			if (config::BoxartDisplayMode)
			{
				ImTextureID textureId{};
				GameMedia game;
				GameBoxart art = boxart.getBoxart(game);
				if (getGameImage(art, textureId, loadedImages < 10))
					loadedImages++;
				if (textureId != ImTextureID())
					pressed = gameImageButton(textureId, "Dreamcast BIOS", responsiveBoxVec2);
				else
					pressed = ImGui::Button("Dreamcast BIOS", responsiveBoxVec2);
			}
			else
			{
				pressed = ImGui::Selectable("Dreamcast BIOS");
			}
			if (pressed)
				gui_start_game("");
			ImGui::PopID();
			counter++;
		}
		{
			scanner.get_mutex().lock();
			for (const auto& game : scanner.get_game_list())
			{
				if (gui_state == GuiState::SelectDisk)
				{
					std::string extension = get_file_extension(game.path);
					if (extension != "gdi" && extension != "chd"
							&& extension != "cdi" && extension != "cue")
						// Only dreamcast disks
						continue;
				}
				std::string gameName = game.name;
				GameBoxart art;
				if (config::BoxartDisplayMode)
				{
					art = boxart.getBoxart(game);
					gameName = art.name;
				}
				if (filter.PassFilter(gameName.c_str()))
				{
					ImGui::PushID(game.path.c_str());
					bool pressed;
					if (config::BoxartDisplayMode)
					{
						if (counter % itemsPerLine != 0)
							ImGui::SameLine();
						counter++;
						ImTextureID textureId{};
						// Get the boxart texture. Load it if needed (max 10 per frame).
						if (getGameImage(art, textureId, loadedImages < 10))
							loadedImages++;
						if (textureId != ImTextureID())
							pressed = gameImageButton(textureId, game.name, responsiveBoxVec2);
						else
						{
							pressed = ImGui::Button(gameName.c_str(), responsiveBoxVec2);
							gameTooltip(game.name);
						}
					}
					else
					{
						pressed = ImGui::Selectable(gameName.c_str());
					}
					if (pressed)
					{
						if (gui_state == GuiState::SelectDisk)
						{
							settings.content.path = game.path;
							try {
								DiscSwap(game.path);
								gui_setState(GuiState::Closed);
							} catch (const FlycastException& e) {
								gui_error(e.what());
							}
						}
						else
						{
							std::string gamePath(game.path);
							scanner.get_mutex().unlock();
							gui_start_game(gamePath);
							scanner.get_mutex().lock();
							ImGui::PopID();
							break;
						}
					}
					ImGui::PopID();
				}
			}
			scanner.get_mutex().unlock();
		}
        ImGui::PopStyleVar();
    }
    scrollWhenDraggingOnVoid();
    windowDragScroll();
	ImGui::EndChild();
	ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();

    contentpath_warning_popup();
}

static bool systemdir_selected_callback(bool cancelled, std::string selection)
{
	if (cancelled)
	{
		gui_setState(GuiState::Main);
		return true;
	}
	selection += "/";

	std::string data_path = selection + "data/";
	if (!file_exists(data_path))
	{
		if (!make_directory(data_path))
		{
			WARN_LOG(BOOT, "Cannot create 'data' directory: %s", data_path.c_str());
			gui_error("Invalid selection:\nFlycast cannot write to this directory.");
			return false;
		}
	}
	else
	{
		// Test
		std::string testPath = data_path + "writetest.txt";
		FILE *file = fopen(testPath.c_str(), "w");
		if (file == nullptr)
		{
			WARN_LOG(BOOT, "Cannot write in the 'data' directory");
			gui_error("Invalid selection:\nFlycast cannot write to this directory.");
			return false;
		}
		fclose(file);
		unlink(testPath.c_str());
	}
	set_user_config_dir(selection);
	add_system_data_dir(selection);
	set_user_data_dir(data_path);

	if (cfgOpen())
	{
		config::Settings::instance().load(false);
		// Make sure the renderer type doesn't change mid-flight
		config::RendererType = RenderType::OpenGL;
		gui_setState(GuiState::Main);
		if (config::ContentPath.get().empty())
		{
			scanner.stop();
			config::ContentPath.get().push_back(selection);
		}
		SaveSettings();
	}
	return true;
}

static void gui_display_onboarding()
{
	ImGui::OpenPopup("Select System Directory");
	select_file_popup("Select System Directory", &systemdir_selected_callback);
}

static std::future<bool> networkStatus;

static void gui_network_start()
{
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(330, 180));

	ImGui::Begin("##network", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
	ImGui::AlignTextToFramePadding();
	ImGui::SetCursorPosX(20.f * settings.display.uiScale);

	if (networkStatus.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
	{
		ImGui::Text("Starting...");
		try {
			if (networkStatus.get())
				gui_setState(GuiState::Closed);
			else
				gui_stop_game();
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
	}
	else
	{
		ImGui::Text("Starting Network...");
		if (NetworkHandshake::instance->canStartNow())
			ImGui::Text("Press Start to start the game now.");
	}
	ImGui::Text("%s", get_notification().c_str());

	float currentwidth = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX((currentwidth - 100.f * settings.display.uiScale) / 2.f + ImGui::GetStyle().WindowPadding.x);
	ImGui::SetCursorPosY(126.f * settings.display.uiScale);
	if (ImGui::Button("Cancel", ScaledVec2(100.f, 0)) && NetworkHandshake::instance != nullptr)
	{
		NetworkHandshake::instance->stop();
		try {
			networkStatus.get();
		}
		catch (const FlycastException& e) {
		}
		gui_stop_game();
	}
	ImGui::PopStyleVar();

	ImGui::End();

	if ((kcode[0] & DC_BTN_START) == 0 && NetworkHandshake::instance != nullptr)
		NetworkHandshake::instance->startNow();
}

static void gui_display_loadscreen()
{
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(330, 180));

    ImGui::Begin("##loading", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorPosX(20.f * settings.display.uiScale);
	try {
		const char *label = gameLoader.getProgress().label;
		if (label == nullptr)
		{
			if (gameLoader.ready())
				label = "Starting...";
			else
				label = "Loading...";
		}

		if (gameLoader.ready())
		{
			if (NetworkHandshake::instance != nullptr)
			{
				networkStatus = NetworkHandshake::instance->start();
				gui_setState(GuiState::NetworkStart);
			}
			else
			{
				gui_setState(GuiState::Closed);
				ImGui::Text("%s", label);
			}
		}
		else
		{
			ImGui::Text("%s", label);
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
			ImGui::ProgressBar(gameLoader.getProgress().progress, ImVec2(-1, 20.f * settings.display.uiScale), "");
			ImGui::PopStyleColor();

			float currentwidth = ImGui::GetContentRegionAvail().x;
			ImGui::SetCursorPosX((currentwidth - 100.f * settings.display.uiScale) / 2.f + ImGui::GetStyle().WindowPadding.x);
			ImGui::SetCursorPosY(126.f * settings.display.uiScale);
			if (ImGui::Button("Cancel", ScaledVec2(100.f, 0)))
				gameLoader.cancel();
		}
	} catch (const FlycastException& ex) {
		ERROR_LOG(BOOT, "%s", ex.what());
#ifdef TEST_AUTOMATION
		die("Game load failed");
#endif
		gui_stop_game(ex.what());
	}
	ImGui::PopStyleVar();

    ImGui::End();
}

void gui_display_ui()
{
	FC_PROFILE_SCOPE;
	const LockGuard lock(guiMutex);

	if (gui_state == GuiState::Closed || gui_state == GuiState::VJoyEdit)
		return;
	if (gui_state == GuiState::Main)
	{
		if (!settings.content.path.empty() || settings.naomi.slave)
		{
#ifndef __ANDROID__
			commandLineStart = true;
#endif
			gui_start_game(settings.content.path);
			return;
		}
	}

	gui_newFrame();
	ImGui::NewFrame();
	error_msg_shown = false;
	bool gui_open = gui_is_open();

	switch (gui_state)
	{
	case GuiState::Settings:
		gui_display_settings();
		break;
	case GuiState::Commands:
		gui_display_commands();
		break;
	case GuiState::Main:
		//gui_display_demo();
		gui_display_content();
		break;
	case GuiState::Closed:
		break;
	case GuiState::Onboarding:
		gui_display_onboarding();
		break;
	case GuiState::VJoyEdit:
		break;
	case GuiState::VJoyEditCommands:
#ifdef __ANDROID__
		gui_display_vjoy_commands();
#endif
		break;
	case GuiState::SelectDisk:
		gui_display_content();
		break;
	case GuiState::Loading:
		gui_display_loadscreen();
		break;
	case GuiState::NetworkStart:
		gui_network_start();
		break;
	case GuiState::Cheats:
		gui_cheats();
		break;
	default:
		die("Unknown UI state");
		break;
	}
	error_popup();
	gui_endFrame(gui_open);

	if (gui_state == GuiState::Closed)
		emu.start();
}

static float LastFPSTime;
static int lastFrameCount = 0;
static float fps = -1;

static std::string getFPSNotification()
{
	if (config::ShowFPS)
	{
		double now = os_GetSeconds();
		if (now - LastFPSTime >= 1.0) {
			fps = (MainFrameCount - lastFrameCount) / (now - LastFPSTime);
			LastFPSTime = now;
			lastFrameCount = MainFrameCount;
		}
		if (fps >= 0.f && fps < 9999.f) {
			char text[32];
			snprintf(text, sizeof(text), "F:%.1f%s", fps, settings.input.fastForwardMode ? " >>" : "");

			return std::string(text);
		}
	}
	return std::string(settings.input.fastForwardMode ? ">>" : "");
}

void gui_display_osd()
{
	if (gui_state == GuiState::VJoyEdit)
		return;
	std::string message = get_notification();
	if (message.empty())
		message = getFPSNotification();

//	if (!message.empty() || config::FloatVMUs || crosshairsNeeded() || (ggpo::active() && config::NetworkStats))
	{
		gui_newFrame();
		ImGui::NewFrame();

		if (!message.empty())
		{
			ImGui::SetNextWindowBgAlpha(0);
			ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetIO().DisplaySize.y), ImGuiCond_Always, ImVec2(0.f, 1.f));	// Lower left corner
			ImGui::SetNextWindowSize(ImVec2(ImGui::GetIO().DisplaySize.x, 0));

			ImGui::Begin("##osd", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav
					| ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground);
			ImGui::SetWindowFontScale(1.5);
			ImGui::TextColored(ImVec4(1, 1, 0, 0.7f), "%s", message.c_str());
			ImGui::End();
		}
		imguiDriver->displayCrosshairs();
		if (config::FloatVMUs)
			imguiDriver->displayVmus();
//		gui_plot_render_time(settings.display.width, settings.display.height);
		if (ggpo::active())
		{
			if (config::NetworkStats)
				ggpo::displayStats();
			chat.display();
		}
		lua::overlay();

		gui_endFrame(gui_is_open());
	}
}

void gui_display_profiler()
{
#if FC_PROFILER
	gui_newFrame();
	ImGui::NewFrame();

	ImGui::Begin("Profiler", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);

	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));

	std::unique_lock<std::recursive_mutex> lock(fc_profiler::ProfileThread::s_allThreadsLock);
	
	for(const fc_profiler::ProfileThread* profileThread : fc_profiler::ProfileThread::s_allThreads)
	{
		char text[256];
		std::snprintf(text, 256, "%.3f : Thread %s", (float)profileThread->cachedTime, profileThread->threadName.c_str());
		ImGui::TreeNode(text);

		ImGui::Indent();
		fc_profiler::drawGUI(profileThread->cachedResultTree);
		ImGui::Unindent();
	}

	ImGui::PopStyleColor();
	
	for (const fc_profiler::ProfileThread* profileThread : fc_profiler::ProfileThread::s_allThreads)
	{
		fc_profiler::drawGraph(*profileThread);
	}

	ImGui::End();

	gui_endFrame(true);
#endif
}

void gui_open_onboarding()
{
	gui_setState(GuiState::Onboarding);
}

void gui_cancel_load()
{
	gameLoader.cancel();
}

void gui_term()
{
	if (inited)
	{
		inited = false;
		scanner.stop();
		ImGui::DestroyContext();
	    EventManager::unlisten(Event::Resume, emuEventCallback);
	    EventManager::unlisten(Event::Start, emuEventCallback);
	    EventManager::unlisten(Event::Terminate, emuEventCallback);
		gui_save();
	}
}

void fatal_error(const char* text, ...)
{
    va_list args;

    char temp[2048];
    va_start(args, text);
    vsnprintf(temp, sizeof(temp), text, args);
    va_end(args);
    ERROR_LOG(COMMON, "%s", temp);

    gui_display_notification(temp, 2000);
}

extern bool subfolders_read;

void gui_refresh_files()
{
	scanner.refresh();
	subfolders_read = false;
}

static void reset_vmus()
{
	for (u32 i = 0; i < std::size(vmu_lcd_status); i++)
		vmu_lcd_status[i] = false;
}

void gui_error(const std::string& what)
{
	error_msg = what;
}

void gui_save()
{
	boxart.saveDatabase();
}

void gui_loadState()
{
	const LockGuard lock(guiMutex);
	if (gui_state == GuiState::Closed && savestateAllowed())
	{
		try {
			emu.stop();
			dc_loadstate(config::SavestateSlot);
			emu.start();
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
	}
}

void gui_saveState()
{
	const LockGuard lock(guiMutex);
	if (gui_state == GuiState::Closed && savestateAllowed())
	{
		try {
			emu.stop();
			dc_savestate(config::SavestateSlot);
			emu.start();
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
	}
}

void gui_setState(GuiState newState)
{
	gui_state = newState;
	if (newState == GuiState::Closed)
	{
		// If the game isn't rendering any frame, these flags won't be updated and keyboard/mouse input will be ignored.
		// So we force them false here. They will be set in the next ImGUI::NewFrame() anyway
		ImGuiIO& io = ImGui::GetIO();
		io.WantCaptureKeyboard = false;
		io.WantCaptureMouse = false;
	}
}

#ifdef TARGET_UWP
// Ugly but a good workaround for MS stupidity
// UWP doesn't allow the UI thread to wait on a thread/task. When an std::future is ready, it is possible
// that the task has not yet completed. Calling std::future::get() at this point will throw an exception
// AND destroy the std::future at the same time, rendering it invalid and discarding the future result.
bool __cdecl Concurrency::details::_Task_impl_base::_IsNonBlockingThread() {
	return false;
}
#endif
