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
#include "rend/osd.h"
#include "cfg/cfg.h"
#include "hw/maple/maple_if.h"
#include "hw/maple/maple_devs.h"
#include "imgui.h"
#include "imgui_stdlib.h"
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
#include "mainui.h"
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
#include "achievements/achievements.h"
#include "gui_achievements.h"
#include "IconsFontAwesome6.h"
#include "oslib/storage.h"
#include <stb_image_write.h>
#include "hw/pvr/Renderer_if.h"
#include "hw/mem/addrspace.h"
#if defined(USE_SDL)
#include "sdl/sdl.h"
#include "sdl/dreamlink.h"
#endif

#include "vgamepad.h"
#ifdef __ANDROID__
#if HOST_CPU == CPU_ARM64 && USE_VULKAN
#include "rend/vulkan/adreno.h"
#endif
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
static u64 osd_message_end;
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

ImFont *largeFont;
static Toast toast;
static ThreadRunner uiThreadRunner;

static void emuEventCallback(Event event, void *)
{
	switch (event)
	{
	case Event::Resume:
		game_started = true;
		vgamepad::startGame();
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

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
#if FC_PROFILER
	ImPlot::CreateContext();
#endif
	ImGuiIO& io = ImGui::GetIO();
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
	uiThreadRunner.init();

#if !defined(TARGET_UWP) && !defined(__SWITCH__)
	settings.display.uiScale = std::max(1.f, settings.display.dpi / 100.f * 0.75f);
   	// Limit scaling on small low-res screens
    if (settings.display.width <= 640 || settings.display.height <= 480)
    	settings.display.uiScale = std::min(1.2f, settings.display.uiScale);
#endif
    settings.display.uiScale *= config::UIScaling / 100.f;
	if (settings.display.uiScale == uiScale && ImGui::GetIO().Fonts->IsBuilt())
		return;
	uiScale = settings.display.uiScale;

    // Setup Dear ImGui style
	ImGui::GetStyle() = ImGuiStyle{};
    ImGui::StyleColorsDark();
    ImGui::GetStyle().TabRounding = 5.0f;
    ImGui::GetStyle().FrameRounding = 3.0f;
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
	largeFont = nullptr;
	const float fontSize = uiScaled(17.f);
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
	// Font Awesome symbols (added to default font)
	data = resource::load("fonts/" FONT_ICON_FILE_NAME_FAS, dataSize);
	verify(data != nullptr);
    font_cfg.FontNo = 0;
	static ImWchar faRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
	io.Fonts->AddFontFromMemoryTTF(data.release(), dataSize, fontSize, &font_cfg, faRanges);
    // Large font without Asian glyphs
	data = resource::load("fonts/Roboto-Regular.ttf", dataSize);
	verify(data != nullptr);
	const float largeFontSize = uiScaled(21.f);
	largeFont = io.Fonts->AddFontFromMemoryTTF(data.release(), dataSize, largeFontSize, nullptr, ranges);

    NOTICE_LOG(RENDERER, "Screen DPI is %.0f, size %d x %d. Scaling by %.2f", settings.display.dpi, settings.display.width, settings.display.height, settings.display.uiScale);
	vgamepad::applyUiScale();
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
			if (achievements::canPause())
			{
				vgamepad::hide();
				try {
					emu.stop();
					gui_setState(GuiState::Commands);
				} catch (const FlycastException& e) {
					gui_stop_game(e.what());
				}
			}
		}
		else
		{
			chat.toggle();
		}
	}
	else if (gui_state == GuiState::VJoyEdit)
	{
		vgamepad::pauseEditing();
		// iOS: force a touch up event to make up for the one eaten by the tap gesture recognizer
		mouseButtons &= ~1;
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
			gui_error("Flycast停止工作。\n\n" + message);
	}
	else
	{
		if (!message.empty())
			ERROR_LOG(COMMON, "Flycast停止工作: %s", message.c_str());
		// Exit emulator
		dc_exit();
	}
}

static bool savestateAllowed()
{
	return !settings.content.path.empty() && !settings.network.online && !settings.naomi.multiboard;
}

static void appendVectorData(void *context, void *data, int size)
{
	std::vector<u8>& v = *(std::vector<u8> *)context;
	const u8 *bytes = (const u8 *)data;
	v.insert(v.end(), bytes, bytes + size);
}

static void getScreenshot(std::vector<u8>& data, int width = 0)
{
	data.clear();
	std::vector<u8> rawData;
	int height = 0;
	if (renderer == nullptr || !renderer->GetLastFrame(rawData, width, height))
		return;
	stbi_flip_vertically_on_write(0);
	stbi_write_png_to_func(appendVectorData, &data, width, height, 3, &rawData[0], 0);
}

static void savestate()
{
	// TODO save state async: png compression, savestate file compression/write
	std::vector<u8> pngData;
	getScreenshot(pngData, 640);
	dc_savestate(config::SavestateSlot, pngData.empty() ? nullptr : &pngData[0], pngData.size());
	ImguiStateTexture savestatePic;
	savestatePic.invalidate();
}

static void gui_display_commands()
{
	fullScreenWindow(false);
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _{ImGuiStyleVar_WindowBorderSize, 0};

	ImGui::Begin("##commands", NULL, ImGuiWindowFlags_NoDecoration);
	{
		ImguiStyleVar _{ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)};	// left aligned

		float columnWidth = std::min(200.f,
				(ImGui::GetContentRegionAvail().x - uiScaled(100 + 150) - ImGui::GetStyle().FramePadding.x * 2)
				/ 2 / uiScaled(1));
		float buttonWidth = 150.f;	// not scaled
		bool lowWidth = ImGui::GetContentRegionAvail().x < uiScaled(100 + buttonWidth * 3)
				+ ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x * 2;
		if (lowWidth)
			buttonWidth = std::min(150.f,
					(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x * 2)
					/ 3 / uiScaled(1));
		bool lowHeight = ImGui::GetContentRegionAvail().y < uiScaled(100 + 50 * 2 + buttonWidth * 3 / 4) + ImGui::GetTextLineHeightWithSpacing() * 2
				+ ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetStyle().WindowPadding.y;

		GameMedia game;
		game.path = settings.content.path;
		game.fileName = settings.content.fileName;
		GameBoxart art = boxart.getBoxart(game);
		ImguiFileTexture tex(art.boxartPath);
		// TODO use placeholder image if not available
		tex.draw(ScaledVec2(100, 100));

		ImGui::SameLine();
		if (!lowHeight)
		{
			ImGui::BeginChild("game_info", ScaledVec2(0, 100.f), ImGuiChildFlags_Border, ImGuiWindowFlags_None);
			ImGui::PushFont(largeFont);
			ImGui::Text("%s", art.name.c_str());
			ImGui::PopFont();
			{
				ImguiStyleColor _(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.f));
				ImGui::TextWrapped("%s", art.fileName.c_str());
			}
			ImGui::EndChild();
		}

		if (lowWidth) {
			ImGui::Columns(3, "buttons", false);
		}
		else
		{
			ImGui::Columns(4, "buttons", false);
			ImGui::SetColumnWidth(0, uiScaled(100.f)  + ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetColumnWidth(1, uiScaled(columnWidth));
			ImGui::SetColumnWidth(2, uiScaled(columnWidth));
			const ImVec2 vmuPos = ImGui::GetStyle().WindowPadding + ScaledVec2(0.f, 100.f)
					+ ImVec2(insetLeft, ImGui::GetStyle().ItemSpacing.y);
			ImguiVmuTexture::displayVmus(vmuPos);
			ImGui::NextColumn();
		}
		ImguiStyleVar _1{ImGuiStyleVar_FramePadding, ScaledVec2(12.f, 3.f)};

		// Resume
		if (ImGui::Button(ICON_FA_PLAY "  返回游戏", ScaledVec2(buttonWidth, 50)))
		{
			GamepadDevice::load_system_mappings();
			gui_setState(GuiState::Closed);
		}
		// Cheats
		{
			DisabledScope _{settings.network.online || settings.raHardcoreMode};

			if (ImGui::Button(ICON_FA_MASK "  金手指", ScaledVec2(buttonWidth, 50)) && !settings.network.online)
				gui_setState(GuiState::Cheats);
		}
		// Achievements
		{
			DisabledScope _{!achievements::isActive()};

			if (ImGui::Button(ICON_FA_TROPHY "  成就", ScaledVec2(buttonWidth, 50)) && achievements::isActive())
				gui_setState(GuiState::Achievements);
		}
		// Barcode
		if (card_reader::barcodeAvailable())
		{
			ImGui::Text("条形码卡");
			char cardBuf[64] {};
			strncpy(cardBuf, card_reader::barcodeGetCard().c_str(), sizeof(cardBuf) - 1);
			ImGui::SetNextItemWidth(uiScaled(buttonWidth));
			if (ImGui::InputText("##barcode", cardBuf, sizeof(cardBuf), ImGuiInputTextFlags_None, nullptr, nullptr))
				card_reader::barcodeSetCard(cardBuf);
		}

		ImGui::NextColumn();

		// Insert/Eject Disk
		const char *disk_label = gdr::isOpen() ? ICON_FA_COMPACT_DISC "  加载光盘" : ICON_FA_COMPACT_DISC "  弹出光盘";
		if (ImGui::Button(disk_label, ScaledVec2(buttonWidth, 50)))
		{
			if (gdr::isOpen()) {
				gui_setState(GuiState::SelectDisk);
			}
			else {
				emu.openGdrom();
				gui_setState(GuiState::Closed);
			}
		}
		// Settings
		if (ImGui::Button(ICON_FA_GEAR "  设置", ScaledVec2(buttonWidth, 50)))
			gui_setState(GuiState::Settings);

		// Exit
		if (ImGui::Button(commandLineStart ? ICON_FA_POWER_OFF "  退出" : ICON_FA_POWER_OFF "  关闭游戏", ScaledVec2(buttonWidth, 50)))
			gui_stop_game();

		ImGui::NextColumn();
		{
			DisabledScope _{!savestateAllowed()};
			ImguiStateTexture savestatePic;
			time_t savestateDate = dc_getStateCreationDate(config::SavestateSlot);

			// Load State
			{
				DisabledScope _{settings.raHardcoreMode || savestateDate == 0};
				if (ImGui::Button(ICON_FA_CLOCK_ROTATE_LEFT "  加载状态", ScaledVec2(buttonWidth, 50)) && savestateAllowed())
				{
					gui_setState(GuiState::Closed);
					dc_loadstate(config::SavestateSlot);
				}
			}

			// Save State
			if (ImGui::Button(ICON_FA_DOWNLOAD "  保存状态", ScaledVec2(buttonWidth, 50)) && savestateAllowed())
			{
				gui_setState(GuiState::Closed);
				savestate();
			}

			// Slot #
			if (ImGui::ArrowButton("##prev-slot", ImGuiDir_Left))
			{
				if (config::SavestateSlot == 0)
					config::SavestateSlot = 9;
				else
					config::SavestateSlot--;
				SaveSettings();
			}
			std::string slot = "卡槽 " + std::to_string((int)config::SavestateSlot + 1);
			float spacingW = (uiScaled(buttonWidth) - ImGui::GetFrameHeight() * 2 - ImGui::CalcTextSize(slot.c_str()).x) / 2;
			ImGui::SameLine(0, spacingW);
			ImGui::Text("%s", slot.c_str());
			ImGui::SameLine(0, spacingW);
			if (ImGui::ArrowButton("##next-slot", ImGuiDir_Right))
			{
				if (config::SavestateSlot == 9)
					config::SavestateSlot = 0;
				else
					config::SavestateSlot++;
				SaveSettings();
			}
			{
				ImVec4 gray(0.75f, 0.75f, 0.75f, 1.f);
				if (savestateDate == 0)
					ImGui::TextColored(gray, "空");
				else
					ImGui::TextColored(gray, "%s", timeToISO8601(savestateDate).c_str());
			}
			savestatePic.draw(ScaledVec2(buttonWidth, 0.f));
		}

		ImGui::Columns(1, nullptr, false);
	}
	ImGui::End();
}

inline static void header(const char *title)
{
	ImguiStyleVar _(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)); // Left
	ImguiStyleVar _1(ImGuiStyleVar_DisabledAlpha, 1.0f);
	ImGui::BeginDisabled();
	ImGui::ButtonEx(title, ImVec2(-1, 0));
	ImGui::EndDisabled();
}

const char *maple_device_types[] =
{
	"无",
	"世嘉手柄",
	"光枪",
	"键盘",
	"鼠标",
	"双摇杆",
	"街机摇杆（ASCII版）",
	"沙锤控制器",
	"钓鱼控制器",
	"P社音乐控制器",
	"赛车控制器",
	"电车GO!专用控制器",
	"全功能控制器",
//	"Dreameye",
};

const char *maple_expansion_device_types[] =
{
	"无",
	"世嘉可视化记忆卡",
	"震动包",
	"麦克风",
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
	case MDT_SegaControllerXL:
		return maple_device_types[12];
	case MDT_Dreameye:
//		return maple_device_types[13];
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
		return MDT_SegaControllerXL;
	case 13:
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

const char *maple_ports[] = { "无", "A", "B", "C", "D", "全部" };

struct Mapping {
	DreamcastKey key;
	const char *name;
};

const Mapping dcButtons[] = {
	{ EMU_BTN_NONE, "Directions" },
	{ DC_DPAD_UP, "Up" },
	{ DC_DPAD_DOWN, "Down" },
	{ DC_DPAD_LEFT, "Left" },
	{ DC_DPAD_RIGHT, "Right" },

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

	{ EMU_BTN_NONE, "Buttons" },
	{ DC_BTN_A, "A" },
	{ DC_BTN_B, "B" },
	{ DC_BTN_X, "X" },
	{ DC_BTN_Y, "Y" },
	{ DC_BTN_C, "C" },
	{ DC_BTN_D, "D" },
	{ DC_BTN_Z, "Z" },

	{ EMU_BTN_NONE, "Triggers"      },
	{ DC_AXIS_LT,   "Left Trigger"  },
	{ DC_AXIS_RT,   "Right Trigger" },
	{ DC_AXIS_LT2,   "Left Trigger 2" },
	{ DC_AXIS_RT2,   "Right Trigger 2" },

	{ EMU_BTN_NONE, "System Buttons" },
	{ DC_BTN_START, "Start" },
	{ DC_BTN_RELOAD, "Reload" },

	{ EMU_BTN_NONE, "Emulator" },
	{ EMU_BTN_MENU, "Menu" },
	{ EMU_BTN_ESCAPE, "Exit" },
	{ EMU_BTN_FFORWARD, "Fast-forward" },
	{ EMU_BTN_LOADSTATE, "Load State" },
	{ EMU_BTN_SAVESTATE, "Save State" },
	{ EMU_BTN_BYPASS_KB, "Bypass Emulated Keyboard" },
	{ EMU_BTN_SCREENSHOT, "Save Screenshot" },

	{ EMU_BTN_NONE, nullptr }
};

const Mapping arcadeButtons[] = {
	{ EMU_BTN_NONE, "Directions" },
	{ DC_DPAD_UP, "Up" },
	{ DC_DPAD_DOWN, "Down" },
	{ DC_DPAD_LEFT, "Left" },
	{ DC_DPAD_RIGHT, "Right" },

	{ DC_AXIS_UP, "Thumbstick Up" },
	{ DC_AXIS_DOWN, "Thumbstick Down" },
	{ DC_AXIS_LEFT, "Thumbstick Left" },
	{ DC_AXIS_RIGHT, "Thumbstick Right" },

	{ DC_AXIS2_UP, "R.Thumbstick Up" },
	{ DC_AXIS2_DOWN, "R.Thumbstick Down" },
	{ DC_AXIS2_LEFT, "R.Thumbstick Left" },
	{ DC_AXIS2_RIGHT, "R.Thumbstick Right" },

	{ EMU_BTN_NONE, "Buttons" },
	{ DC_BTN_A, "Button 1" },
	{ DC_BTN_B, "Button 2" },
	{ DC_BTN_C, "Button 3" },
	{ DC_BTN_X, "Button 4" },
	{ DC_BTN_Y, "Button 5" },
	{ DC_BTN_Z, "Button 6" },
	{ DC_DPAD2_LEFT, "Button 7" },
	{ DC_DPAD2_RIGHT, "Button 8" },
//	{ DC_DPAD2_RIGHT, "Button 9" }, // TODO

	{ EMU_BTN_NONE, "Triggers" },
	{ DC_AXIS_LT, "Left Trigger" },
	{ DC_AXIS_RT, "Right Trigger" },
	{ DC_AXIS_LT2,   "Left Trigger 2" },
	{ DC_AXIS_RT2,   "Right Trigger 2" },

	{ EMU_BTN_NONE, "System Buttons" },
	{ DC_BTN_START, "Start" },
	{ DC_BTN_RELOAD, "Reload" },
	{ DC_BTN_D, "Coin" },
	{ DC_DPAD2_UP, "Service" },
	{ DC_DPAD2_DOWN, "Test" },
	{ DC_BTN_INSERT_CARD, "Insert Card" },

	{ EMU_BTN_NONE, "Emulator" },
	{ EMU_BTN_MENU, "Menu" },
	{ EMU_BTN_ESCAPE, "Exit" },
	{ EMU_BTN_FFORWARD, "Fast-forward" },
	{ EMU_BTN_LOADSTATE, "Load State" },
	{ EMU_BTN_SAVESTATE, "Save State" },
	{ EMU_BTN_BYPASS_KB, "Bypass Emulated Keyboard" },
	{ EMU_BTN_SCREENSHOT, "Save Screenshot" },

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
static u64 map_start_time;
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
	ImguiStyleVar _(ImGuiStyleVar_WindowPadding, padding);
	ImguiStyleVar _1(ImGuiStyleVar_ItemSpacing, padding);
	if (ImGui::BeginPopupModal("控制器映射", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
	{
		ImGui::Text("正在等待控制器 '%s'……", mapping->name);
		u64 now = getTimeMs();
		ImGui::Text("超时 %d s", (int)(5 - (now - map_start_time) / 1000));
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
		else if (now - map_start_time >= 5000)
		{
			mapped_device = NULL;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
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
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	if (ImGui::BeginPopupModal("控制器映射", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove))
	{
		const ImGuiStyle& style = ImGui::GetStyle();
		const float winWidth = ImGui::GetIO().DisplaySize.x - insetLeft - insetRight - (style.WindowBorderSize + style.WindowPadding.x) * 2;
		const float col_width = (winWidth - style.GrabMinSize - style.ItemSpacing.x
				- (ImGui::CalcTextSize("映射").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)
				- (ImGui::CalcTextSize("解除").x + style.FramePadding.x * 2.0f + style.ItemSpacing.x)) / 2;

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
			return;
		}
		ImGui::SetItemDefaultFocus();

		float portWidth = 0;
		if (gamepad->maple_port() == MAPLE_PORTS)
		{
			ImGui::SameLine();
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (uiScaled(30) - ImGui::GetFontSize()) / 2));
			portWidth = ImGui::CalcTextSize("AA").x + ImGui::GetStyle().ItemSpacing.x * 2.0f + ImGui::GetFontSize();
			ImGui::SetNextItemWidth(portWidth);
			if (ImGui::BeginCombo("Port", maple_ports[gamepad_port + 1]))
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
		}
		float comboWidth = ImGui::CalcTextSize("Dreamcast Controls").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetFontSize() + ImGui::GetStyle().FramePadding.x * 4;
		float gameConfigWidth = 0;
		if (!settings.content.gameId.empty())
			gameConfigWidth = ImGui::CalcTextSize(gamepad->isPerGameMapping() ? "Delete Game Config" : "Make Game Config").x + ImGui::GetStyle().ItemSpacing.x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(0, ImGui::GetContentRegionAvail().x - comboWidth - gameConfigWidth - ImGui::GetStyle().ItemSpacing.x - uiScaled(100) * 2 - portWidth);

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
				if (ImGui::Button("制作游戏配置", ScaledVec2(0, 30)))
					gamepad->setPerGameMapping(true);
			}
			ImGui::SameLine();
		}
		if (ImGui::Button("重置……", ScaledVec2(100, 30)))
			ImGui::OpenPopup("确认重置");

		{
			ImguiStyleVar _(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));
			if (ImGui::BeginPopupModal("Confirm Reset", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
			{
				ImGui::Text("您确定要将映射重置为默认值吗？");
				static bool hitbox;
				if (arcade_button_mode)
				{
					ImGui::Text("控制器类型：");
					if (ImGui::RadioButton("手柄", !hitbox))
						hitbox = false;
					ImGui::SameLine();
					if (ImGui::RadioButton("街机/全按键控制器", hitbox))
						hitbox = true;
				}
				ImGui::NewLine();
				{
	 				ImguiStyleVar _(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
					ImguiStyleVar _1(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
					if (ImGui::Button("是"))
					{
						gamepad->resetMappingToDefault(arcade_button_mode, !hitbox);
						gamepad->save_mapping(map_system);
						ImGui::CloseCurrentPopup();
					}
					ImGui::SameLine();
					if (ImGui::Button("否"))
						ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}

		ImGui::SameLine();

		const char* items[] = { "DC控制器", "街机控制器" };

		if (last_item_current_map_idx == 2 && game_started)
			// Select the right mappings for the current game
			item_current_map_idx = settings.platform.isArcade() ? 1 : 0;

		// Here our selection data is an index.

		ImGui::SetNextItemWidth(comboWidth);
		// Make the combo height the same as the Done and Reset buttons
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (uiScaled(30) - ImGui::GetFontSize()) / 2));
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
			snprintf(key_id, sizeof(key_id), "key_id%d", systemMapping->key);
			ImguiID _(key_id);

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
				map_start_time = getTimeMs();
				ImGui::OpenPopup("控制器映射");
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
			if (ImGui::Button("解除映射"))
			{
				input_mapping = gamepad->get_input_mapping();
				unmapControl(input_mapping, gamepad_port, systemMapping->key);
			}
			ImGui::NextColumn();
		}
		ImGui::Columns(1, nullptr, false);
	    scrollWhenDraggingOnVoid();
	    windowDragScroll();

		ImGui::EndChild();
		error_popup();
		ImGui::EndPopup();
	}
}

static void gamepadPngFileSelected(bool cancelled, std::string path)
{
	if (!cancelled)
		gui_runOnUiThread([path]() {
			vgamepad::loadImage(path);
		});
}

static void gamepadSettingsPopup(const std::shared_ptr<GamepadDevice>& gamepad)
{
	centerNextWindow();
	ImGui::SetNextWindowSize(min(ImGui::GetIO().DisplaySize, ScaledVec2(450.f, 300.f)));

	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	if (ImGui::BeginPopupModal("游戏手柄设置", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_DragScrolling))
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
			return;
		}
		ImGui::NewLine();
		if (gamepad->is_virtual_gamepad())
		{
			if (gamepad->is_rumble_enabled()) {
				header("Haptic");
				OptionSlider("Power", config::VirtualGamepadVibration, 0, 100, "Haptic feedback power", "%d%%");
			}
			header("View");
			OptionSlider("Transparency", config::VirtualGamepadTransparency, 0, 100, "Virtual gamepad buttons transparency", "%d%%");

#if defined(__ANDROID__) || defined(TARGET_IPHONE)
			vgamepad::ImguiVGamepadTexture tex;
			ImGui::Image(tex.getId(), ScaledVec2(300.f, 150.f), ImVec2(0, 1), ImVec2(1, 0));
#endif
			const char *gamepadPngTitle = "选择 PNG 文件";
			if (ImGui::Button("正在选择图片……", ScaledVec2(150, 30)))
#ifdef __ANDROID__
			{
				if (!hostfs::addStorage(false, false, gamepadPngTitle, gamepadPngFileSelected, "image/png"))
					ImGui::OpenPopup(gamepadPngTitle);
			}
#else
			{
				ImGui::OpenPopup(gamepadPngTitle);
			}
#endif
			ImGui::SameLine();
			if (ImGui::Button("使用默认", ScaledVec2(150, 30)))
				vgamepad::loadImage("");

			select_file_popup(gamepadPngTitle, [](bool cancelled, std::string selection)
				{
					gamepadPngFileSelected(cancelled, selection);
					return true;
				}, true, "png");
		}
		else if (gamepad->is_rumble_enabled())
		{
			header("振动");
			int power = gamepad->get_rumble_power();
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("Power", &power, 0, 100, "%d%%"))
				gamepad->set_rumble_power(power);
			ImGui::SameLine();
			ShowHelpMarker("振动强度");
		}
		if (gamepad->has_analog_stick())
		{
			header("摇杆");
			int deadzone = std::round(gamepad->get_dead_zone() * 100.f);
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("死区", &deadzone, 0, 100, "%d%%"))
				gamepad->set_dead_zone(deadzone / 100.f);
			ImGui::SameLine();
			ShowHelpMarker("注册为输入的最小偏转");
			int saturation = std::round(gamepad->get_saturation() * 100.f);
			ImGui::SetNextItemWidth(uiScaled(300));
			if (ImGui::SliderInt("饱和", &saturation, 50, 200, "%d%%"))
				gamepad->set_saturation(saturation / 100.f);
			ImGui::SameLine();
			ShowHelpMarker("以 100% 摇杆偏转发送到游戏的值。 "
					"大于 100% 的值将在摇杆完全偏转之前饱和。");
		}
	    scrollWhenDraggingOnVoid();
	    windowDragScroll();
		ImGui::EndPopup();
	}
}

void error_popup()
{
	if (!error_msg_shown && !error_msg.empty())
	{
		ImVec2 padding = ScaledVec2(20, 20);
		ImguiStyleVar _(ImGuiStyleVar_WindowPadding, padding);
		ImguiStyleVar _1(ImGuiStyleVar_ItemSpacing, padding);
		ImGui::OpenPopup("错误");
		if (ImGui::BeginPopupModal("错误", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
			ImGui::TextWrapped("%s", error_msg.c_str());
			{
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(80.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
				if (ImGui::Button("好", ScaledVec2(80.f, 0)))
				{
					error_msg.clear();
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::SetItemDefaultFocus();
			ImGui::PopTextWrapPos();
			ImGui::EndPopup();
		}
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
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
            ImGui::TextWrapped("  扫描了 %d 个文件夹，但找不到游戏！  ", scanner.empty_folders_scanned);
			{
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x - uiScaled(55.f));
				if (ImGui::Button("重新选择", ScaledVec2(100.f, 0)))
				{
					scanner.content_path_looks_incorrect = false;
					ImGui::CloseCurrentPopup();
					show_contentpath_selection = true;
				}

				ImGui::SameLine();
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x + uiScaled(55.f));
				if (ImGui::Button("取消", ScaledVec2(100.f, 0)))
				{
					scanner.content_path_looks_incorrect = false;
					ImGui::CloseCurrentPopup();
					scanner.stop();
					config::ContentPath.get().clear();
				}
			}
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
    }
    if (show_contentpath_selection)
    {
        scanner.stop();
        const char *title = "选择游戏文件夹";
        ImGui::OpenPopup(title);
        select_file_popup(title, [](bool cancelled, std::string selection)
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

#if !defined(NDEBUG) || defined(DEBUGFAST) || FC_PROFILER

static void gui_debug_tab()
{
	header("Logging");
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
		ImGui::InputText("Log Server", &config::LogServer.get(), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
        ImGui::SameLine();
        ShowHelpMarker("Log to this hostname[:port] with UDP. Default port is 31667.");
	}
#if FC_PROFILER
	ImGui::Spacing();
	header("Profiling");
	{

		OptionCheckbox("启用", config::ProfilerEnabled, "Enable the profiler.");
		if (!config::ProfilerEnabled)
		{
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.5f);
		}
		OptionCheckbox("显示", config::ProfilerDrawToGUI, "在叠加中绘制分析器输出。");
		OptionCheckbox("输出到终端", config::ProfilerOutputTTY, "将分析器输出写入终端");
		// TODO frame warning time
		if (!config::ProfilerEnabled)
		{
			ImGui::PopItemFlag();
			ImGui::PopStyleVar();
		}
	}
#endif
}
#endif

static void addContentPathCallback(const std::string& path)
{
	auto& contentPath = config::ContentPath.get();
	if (std::count(contentPath.begin(), contentPath.end(), path) == 0)
	{
		scanner.stop();
		contentPath.push_back(path);
		if (gui_state == GuiState::Main)
			// when adding content path from empty game list
			SaveSettings();
		scanner.refresh();
	}
}

static void addContentPath(bool start)
{
    const char *title = "选择游戏文件夹";
    select_file_popup(title, [](bool cancelled, std::string selection) {
		if (!cancelled)
			addContentPathCallback(selection);
		return true;
    });
#ifdef __ANDROID__
    if (start)
    {
    	bool supported = hostfs::addStorage(true, false, title, [](bool cancelled, std::string selection) {
    		if (!cancelled)
    			addContentPathCallback(selection);
    	});
    	if (!supported)
    		ImGui::OpenPopup(title);
    }
#else
    if (start)
    	ImGui::OpenPopup(title);
#endif
}

static float calcComboWidth(const char *biggestLabel) {
	return ImGui::CalcTextSize(biggestLabel).x + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
}

static void gui_settings_general()
{
	{
		DisabledScope scope(settings.platform.isArcade());

		const char *languages[] = { "日语", "英语", "德语", "法语", "西班牙语", "意大利语", "默认" };
		OptionComboBox("语言", config::Language, languages, std::size(languages),
			"BIOS 中配置的语言");

		const char *broadcast[] = { "NTSC", "PAL", "PAL/M", "PAL/N", "Default" };
		OptionComboBox("电视制式", config::Broadcast, broadcast, std::size(broadcast),
				"非VGA模式下的电视制式");
	}

	const char *consoleRegion[] = { "日本", "美国", "欧洲", "默认" };
	const char *arcadeRegion[] = { "日本", "美国", "欧洲", "韩国" };
	const char **region = settings.platform.isArcade() ? arcadeRegion : consoleRegion;
	OptionComboBox("区域", config::Region, region, std::size(consoleRegion),
				"BIOS 区域");

	const char *cable[] = { "VGA端子", "色差分量线", "AV端子" };
	{
		DisabledScope scope(config::Cable.isReadOnly() || settings.platform.isArcade());

		const char *value = config::Cable == 0 ? cable[0]
				: config::Cable > 0 && config::Cable <= (int)std::size(cable) ? cable[config::Cable - 1]
				: "?";
		if (ImGui::BeginCombo("接口", value, ImGuiComboFlags_None))
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
        ShowHelpMarker("Video connection type");
	}

#if !defined(TARGET_IPHONE)
    ImVec2 size;
    size.x = 0.0f;
    size.y = (ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 2.f)
    				* (config::ContentPath.get().size() + 1);

    if (BeginListBox("游戏位置", size, ImGuiWindowFlags_NavFlattened))
    {
    	int to_delete = -1;
        for (u32 i = 0; i < config::ContentPath.get().size(); i++)
        {
        	ImguiID _(config::ContentPath.get()[i].c_str());
            ImGui::AlignTextToFramePadding();
            float maxW = ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(ICON_FA_TRASH_CAN).x - ImGui::GetStyle().FramePadding.x * 2
            		 - ImGui::GetStyle().ItemSpacing.x;
            std::string s = middleEllipsis(config::ContentPath.get()[i], maxW);
        	ImGui::Text("%s", s.c_str());
        	ImGui::SameLine(0, maxW - ImGui::CalcTextSize(s.c_str()).x + ImGui::GetStyle().ItemSpacing.x);
        	if (ImGui::Button(ICON_FA_TRASH_CAN))
        		to_delete = i;
        }

        ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(24, 3));
        const bool addContent = ImGui::Button("添加");
        addContentPath(addContent);
        ImGui::SameLine();

        if (ImGui::Button("重新扫描"))
			scanner.refresh();
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
    ShowHelpMarker("存储游戏的文件夹");

    size.y = ImGui::GetTextLineHeightWithSpacing() * 1.25f + ImGui::GetStyle().FramePadding.y * 2.0f;

#if defined(__linux__) && !defined(__ANDROID__)
    if (BeginListBox("数据文件夹", size, ImGuiWindowFlags_NavFlattened))
    {
    	ImGui::AlignTextToFramePadding();
    	float w = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x;
    	std::string s = middleEllipsis(get_writable_data_path(""), w);
        ImGui::Text("%s", s.c_str());
        ImGui::EndListBox();
    }
    ImGui::SameLine();
    ShowHelpMarker("BIOS文件及VMU记忆卡存档/状态保存目录");
#else
#if defined(__ANDROID__) || defined(TARGET_MAC)
    size.y += ImGui::GetTextLineHeightWithSpacing() * 1.25f;
#endif
    if (BeginListBox("主文件夹", size, ImGuiWindowFlags_NavFlattened))
    {
    	ImGui::AlignTextToFramePadding();
    	float w = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x;
    	std::string s = middleEllipsis(get_writable_config_path(""), w);
        ImGui::Text("%s", s.c_str());
        ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(24, 3));
#ifdef __ANDROID__
        {
        	DisabledScope _(!config::UseSafFilePicker);
			if (ImGui::Button("导入"))
				hostfs::importHomeDirectory();
			ImGui::SameLine();
			if (ImGui::Button("导出"))
				hostfs::exportHomeDirectory();
        }
#endif
#ifdef TARGET_MAC
        if (ImGui::Button("在 Finder 中显示"))
        {
            char temp[512];
            snprintf(temp, sizeof(temp), "open \"%s\"", get_writable_config_path("").c_str());
            system(temp);
        }
#endif
        ImGui::EndListBox();
    }
    ImGui::SameLine();
    ShowHelpMarker("Flycast 保存配置文件和 VMU 的文件夹。BIOS 文件应位于名为\"data\"的子文件夹中");
#endif // !linux
#else // TARGET_IPHONE
    {
    	ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(24, 3));
		if (ImGui::Button("重新扫描"))
			scanner.refresh();
    }
#endif

	OptionCheckbox("封面游戏列表", config::BoxartDisplayMode,
			"在游戏列表中显示游戏封面。");
	OptionCheckbox("获取封面", config::FetchBoxart,
			"从 TheGamesDB.net 获取封面图像。");
	if (OptionSlider("UI Scaling", config::UIScaling, 50, 200, "调整 UI 元素和字体的大小。", "%d%%"))
		uiUserScaleUpdated = true;
	if (uiUserScaleUpdated)
	{
		ImGui::SameLine();
		if (ImGui::Button("Apply")) {
			mainui_reinit();
			uiUserScaleUpdated = false;
		}
	}

	if (OptionCheckbox("隐藏传统Naomi游戏", config::HideLegacyNaomiRoms,
			"从内容浏览器中隐藏.bin、.dat和.lst文件"))
		scanner.refresh();
#ifdef __ANDROID__
	OptionCheckbox("使用 SAF 文件选取器", config::UseSafFilePicker,
			"使用 Android 存储访问框架文件选择器选择文件夹和文件。在 Android 10 及更高版本上被忽略。");
#endif

	ImGui::Text("自动状态：");
	OptionCheckbox("加载", config::AutoLoadState,
			"开始时加载游戏的最后保存状态");
	ImGui::SameLine();
	OptionCheckbox("保存", config::AutoSaveState,
			"退出时保存游戏状态");
	OptionCheckbox("Naomi免费游戏", config::ForceFreePlay, "在免费游戏模式下配置 Naomi 游戏。");
#if USE_DISCORD
	OptionCheckbox("Discord Presence", config::DiscordPresence, "Show which game you are playing on Discord");
#endif
#ifdef USE_RACHIEVEMENTS
	OptionCheckbox("启用成就", config::EnableAchievements, "使用 RetroAchievements.org 跟踪您的游戏成就");
	{
		DisabledScope _(!config::EnableAchievements);
		ImGui::Indent();
		OptionCheckbox("硬核模式", config::AchievementsHardcoreMode,
				"启用 RetroAchievements 硬核模式。在此模式下不允许使用作弊和加载状态。");
		ImGui::InputText("用户名", &config::AchievementsUserName.get(),
				achievements::isLoggedOn() ? ImGuiInputTextFlags_ReadOnly : ImGuiInputTextFlags_None, nullptr, nullptr);
		if (config::EnableAchievements)
		{
			static std::future<void> futureLogin;
			achievements::init();
			if (achievements::isLoggedOn())
			{
				ImGui::Text("身份验证成功");
				if (futureLogin.valid())
					futureLogin.get();
				if (ImGui::Button("退出登录", ScaledVec2(100, 0)))
					achievements::logout();
			}
			else
			{
				static char password[256];
				ImGui::InputText("密码", password, sizeof(password), ImGuiInputTextFlags_Password, nullptr, nullptr);
				if (futureLogin.valid())
				{
					if (futureLogin.wait_for(std::chrono::seconds::zero()) == std::future_status::timeout) {
						ImGui::Text("验证中……");
					}
					else
					{
						try {
							futureLogin.get();
						} catch (const FlycastException& e) {
							gui_error(e.what());
						}
					}
				}
				{
					DisabledScope _(config::AchievementsUserName.get().empty() || password[0] == '\0');
					if (ImGui::Button("登录", ScaledVec2(100, 0)) && !futureLogin.valid())
					{
						futureLogin = achievements::login(config::AchievementsUserName.get().c_str(), password);
						memset(password, 0, sizeof(password));
					}
				}
			}
		}
		ImGui::Unindent();
	}
#endif
}

static void gui_settings_controls(bool& maple_devices_changed)
{
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
				snprintf(port_name, sizeof(port_name), "##mapleport%d", i);
				ImguiID _(port_name);
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
				ImGui::SameLine(0, uiScaled(8));
				if (gamepad->remappable() && ImGui::Button("映射"))
				{
					gamepad_port = 0;
					ImGui::OpenPopup("控制器映射");
				}

				controller_mapping_popup(gamepad);

#if defined(__ANDROID__) || defined(TARGET_IPHONE)
				if (gamepad->is_virtual_gamepad())
				{
					if (ImGui::Button("编辑布局"))
					{
						vgamepad::startEditing();
						gui_setState(GuiState::VJoyEdit);
					}
				}
#endif
				if (gamepad->is_rumble_enabled() || gamepad->has_analog_stick()
					|| gamepad->is_virtual_gamepad())
				{
					ImGui::SameLine(0, uiScaled(16));
					if (ImGui::Button("设置"))
						ImGui::OpenPopup("游戏手柄设置");
					gamepadSettingsPopup(gamepad);
				}
			}
			ImGui::EndTable();
		}
    }

	ImGui::Spacing();
	OptionSlider("鼠标灵敏度", config::MouseSensitivity, 1, 500);
#if defined(_WIN32) && !defined(TARGET_UWP)
	OptionCheckbox("使用原始输入", config::UseRawInput, "支持多种指点设备（鼠标、光枪）和键盘");
#endif
#ifdef USE_DREAMCASTCONTROLLER
	OptionCheckbox("使用物理 VMU 内存", config::UsePhysicalVmuMemory,
		"通过 DreamPicoPort/DreamConn 启用对物理 VMU 内存的直接读/写访问。");
#endif

	ImGui::Spacing();
	header("DC设备");
    {
		bool is_there_any_xhair = false;
		if (ImGui::BeginTable("dreamcastDevices", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings,
				ImVec2(0, 0), uiScaled(8)))
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
				snprintf(device_name, sizeof(device_name), "##device%d", bus);
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
					case MDT_SegaControllerXL:
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
					snprintf(device_name, sizeof(device_name), "##device%d.%d", bus, port + 1);
					ImguiID _(device_name);
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
				}
				if (config::MapleMainDevices[bus] == MDT_LightGun)
				{
					ImGui::TableSetColumnIndex(3);
					snprintf(device_name, sizeof(device_name), "##device%d.xhair", bus);
					ImguiID _(device_name);
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
				}
				ImGui::PopItemWidth();
			}
			ImGui::EndTable();
		}
		{
			DisabledScope scope(!is_there_any_xhair);
			OptionSlider("十字准线大小", config::CrosshairSize, 10, 100);
		}
		OptionCheckbox("Per Game VMU A1", config::PerGameVmu, "启用后，每个游戏在控制器 A 的端口 1 上都有自己的 VMU。");
    }
}

static void gui_settings_video()
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

    float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
	if (apiCount > 1)
	{
		header("图形 API");
		{
			ImGui::Columns(apiCount, "renderApi", false);
#ifdef USE_OPENGL
			ImGui::RadioButton("OpenGL", &renderApi, 0);
			ImGui::NextColumn();
#endif
#ifdef USE_VULKAN
#ifdef __APPLE__
			ImGui::RadioButton("Vulkan (Metal)", &renderApi, 1);
			ImGui::SameLine(0, innerSpacing);
			ShowHelpMarker("MoltenVK：在 Apple 的 Metal 图形框架上运行的 Vulkan 实现");
#else
			ImGui::RadioButton("Vulkan", &renderApi, 1);
#endif // __APPLE__
			ImGui::NextColumn();
#endif
#ifdef USE_DX9
			{
				DisabledScope _(settings.platform.isNaomi2());
				ImGui::RadioButton("DirectX 9", &renderApi, 2);
				ImGui::NextColumn();
			}
#endif
#ifdef USE_DX11
			ImGui::RadioButton("DirectX 11", &renderApi, 3);
			ImGui::NextColumn();
#endif
			ImGui::Columns(1, nullptr, false);
    	}
    }
    header("Transparent Sorting");
    {
		const bool has_per_pixel = GraphicsContext::Instance()->hasPerPixel();
    	int renderer = perPixel ? 2 : config::PerStripSorting ? 1 : 0;
    	ImGui::Columns(has_per_pixel ? 3 : 2, "renderers", false);
    	ImGui::RadioButton("Per Triangle", &renderer, 0);
        ImGui::SameLine();
        ShowHelpMarker("对每个三角形的透明多边形进行排序。速度快，但可能会产生图形故障");
    	ImGui::NextColumn();
    	ImGui::RadioButton("Per Strip", &renderer, 1);
        ImGui::SameLine();
        ShowHelpMarker("对每个条带的透明多边形进行排序。速度更快，但可能会产生图形故障");
        if (has_per_pixel)
        {
        	ImGui::NextColumn();
        	ImGui::RadioButton("Per Pixel", &renderer, 2);
        	ImGui::SameLine();
        	ShowHelpMarker("对每个像素的透明多边形进行排序。速度较慢但准确");
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

    header("Rendering Options");
    {
        const std::array<float, 13> scalings{ 0.5f, 1.f, 1.5f, 2.f, 2.5f, 3.f, 4.f, 4.5f, 5.f, 6.f, 7.f, 8.f, 9.f };
        const std::array<std::string, 13> scalingsText{ "Half", "Native", "x1.5", "x2", "x2.5", "x3", "x4", "x4.5", "x5", "x6", "x7", "x8", "x9" };
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
        if (ImGui::BeginCombo("##分辨率", resLabels[selected].c_str(), ImGuiComboFlags_NoArrowButton))
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
        ImGui::SameLine(0, innerSpacing);

        ImGui::Text("内部分辨率");
        ImGui::SameLine();
        ShowHelpMarker("内部渲染分辨率。数值越高效果越好，但会增加GPU负载。可设置高于显示器物理分辨率的数值（最高不超过物理分辨率的两倍）以启用超采样技术，该技术能在保持画面锐度的同时提供高质量的抗锯齿效果。");

#ifndef TARGET_IPHONE
    	OptionCheckbox("垂直同步", config::VSync, "将帧速率与屏幕刷新率同步。推荐");
    	if (isVulkan(config::RendererType))
    	{
	    	ImGui::Indent();
			{
				DisabledScope scope(!config::VSync);

				OptionCheckbox("重复帧", config::DupeFrames, "高刷新率显示器（120 Hz 及更高）上的重复帧");
	    	}
	    	ImGui::Unindent();
    	}
#endif
    	OptionCheckbox("在游戏中显示 VMU", config::FloatVMUs, "在游戏中显示 VMU LCD 屏幕");
    	OptionCheckbox("全帧缓冲区模拟", config::EmulateFramebuffer,
    			"完全精确的 VRAM 帧缓冲区模拟。帮助直接访问帧缓冲区以获得特殊效果的游戏。"
    			"非常慢，与升级和宽屏不兼容。");
    	OptionCheckbox("加载自定义纹理", config::CustomTextures,
    			"从 data/textures/<game id 加载自定义/高分辨率纹理>");
    }
	ImGui::Spacing();
    header("宽屏");
    {
    	OptionCheckbox("宽屏", config::Widescreen,
    			"绘制超出正常 4：3 纵横比的几何体。可能会在显示区域产生图形故障。nAspect Fit 并显示完整的 16：9 内容。");
		{
			DisabledScope scope(!config::Widescreen);

			ImGui::Indent();
			OptionCheckbox("超宽屏", config::SuperWidescreen,
					"当屏幕或窗口的纵横比大于 16：9 时，使用屏幕或窗口的整个宽度。\n纵横比填充并删除黑条。");
			ImGui::Unindent();
    	}
    	OptionCheckbox("宽屏金手指", config::WidescreenGameHacks,
    			"修改游戏，使其以 16：9 变形格式显示，并使用水平屏幕拉伸。仅支持部分游戏。");
    	OptionSlider("水平拉伸", config::ScreenStretching, 100, 250,
    			"水平拉伸屏幕", "%d%%");
    	OptionCheckbox("将屏幕旋转 90°", config::Rotate90, "逆时针旋转屏幕 90°");
    }
	if (perPixel)
	{
		ImGui::Spacing();
		header("像素设置");

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
		ImGui::SameLine(0, innerSpacing);

        ImGui::Text("像素缓冲区大小");
        ImGui::SameLine();
        ShowHelpMarker("像素缓冲区的大小。放大时可能需要增加很大的倍数。");

        OptionSlider("最大层数", config::PerPixelLayers, 8, 128,
        		"透明层的最大数量。对于一些复杂的场景，可能需要增加。减少它可能会提高性能。");
	}
	ImGui::Spacing();
    header("性能");
    {
    	ImGui::Text("自动跳帧：");
    	ImGui::Columns(3, "autoskip", false);
    	OptionRadioButton("禁用", config::AutoSkipFrame, 0, "无跳帧");
    	ImGui::NextColumn();
    	OptionRadioButton("普通", config::AutoSkipFrame, 1, "当 GPU 和 CPU 都运行缓慢时跳过一帧");
    	ImGui::NextColumn();
    	OptionRadioButton("最大", config::AutoSkipFrame, 2, "GPU 运行缓慢时跳过一帧");
    	ImGui::Columns(1, nullptr, false);

    	OptionArrowButtons("跳帧", config::SkipFrame, 0, 6,
    			"在两个实际渲染的帧之间跳过的帧数");
    	OptionCheckbox("阴影", config::ModifierVolumes,
    			"启用修改器体积，通常用于阴影");
    	OptionCheckbox("雾化", config::Fog, "启用雾化效果");
    }
    ImGui::Spacing();
	header("高级");
    {
    	OptionCheckbox("延迟帧交换", config::DelayFrameSwapping,
    			"有助于避免屏幕闪烁或视频出现故障。不建议在慢速平台上使用");
    	OptionCheckbox("修复高档前沿", config::FixUpscaleBleedingEdge,
    			"有助于在放大时解决纹理渗色的情况。如果在 2D 游戏（MVC2、CVS、KOF 等）中放大时像素变形，禁用它会有所帮助。");
    	OptionCheckbox("原生深度插值", config::NativeDepthInterpolation,
    			"帮助解决 AMD GPU 上的纹理损坏和深度问题。在某些情况下还可以帮助英特尔 GPU。");
    	OptionCheckbox("将渲染纹理复制到VRAM", config::RenderToTextureBuffer,
    			"将渲染到的纹理复制回VRAM。速度较慢但准确");
		const std::array<int, 5> aniso{ 1, 2, 4, 8, 16 };
        const std::array<std::string, 5> anisoText{ "禁用", "2x", "4x", "8x", "16x" };
        u32 afSelected = 0;
        for (u32 i = 0; i < aniso.size(); i++)
        {
        	if (aniso[i] == config::AnisotropicFiltering)
        		afSelected = i;
        }

        ImGui::PushItemWidth(ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f);
        if (ImGui::BeginCombo("##Anisotropic Filtering", anisoText[afSelected].c_str(), ImGuiComboFlags_NoArrowButton))
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

        if (ImGui::ArrowButton("##Decrease Anisotropic Filtering", ImGuiDir_Left))
        {
            if (afSelected > 0)
            	config::AnisotropicFiltering = aniso[afSelected - 1];
        }
        ImGui::SameLine(0, innerSpacing);
        if (ImGui::ArrowButton("##Increase Anisotropic Filtering", ImGuiDir_Right))
        {
            if (afSelected < aniso.size() - 1)
            	config::AnisotropicFiltering = aniso[afSelected + 1];
        }
        ImGui::SameLine(0, innerSpacing);

        ImGui::Text("Anisotropic Filtering");
        ImGui::SameLine();
        ShowHelpMarker("较高的值使以倾斜角度查看的纹理看起来更清晰，但对 GPU 的要求更高。此选项仅对 mipmapped 纹理有明显影响。");

    	ImGui::Text("纹理过滤：");
    	ImGui::Columns(3, "textureFiltering", false);
    	OptionRadioButton("默认", config::TextureFiltering, 0, "使用游戏的默认纹理过滤");
    	ImGui::NextColumn();
    	OptionRadioButton("强制最近邻", config::TextureFiltering, 1, "对所有纹理强制最近邻过滤。外观更清晰，但可能会导致各种渲染问题。此选项通常不会影响性能。");
    	ImGui::NextColumn();
    	OptionRadioButton("线性", config::TextureFiltering, 2, "对所有纹理强制线性过滤。外观更平滑，但可能会导致各种渲染问题。此选项通常不会影响性能。");
    	ImGui::Columns(1, nullptr, false);

    	OptionCheckbox("显示 FPS 计数器", config::ShowFPS, "在屏幕上显示帧/秒计数器");
    }
	ImGui::Spacing();
    header("纹理升级");
    {
#ifdef _OPENMP
    	OptionArrowButtons("纹理升级", config::TextureUpscale, 1, 8,
    			"使用 xBRZ 算法升级纹理。仅适用于快速平台和某些 2D 游戏", "x%d");
    	OptionSlider("纹理最大尺寸", config::MaxFilteredTextureSize, 8, 1024,
    			"大于此维度平方的纹理将不会被放大");
    	OptionArrowButtons("最大线程数", config::MaxThreads, 1, 8,
    			"用于纹理放大的最大线程数。推荐：物理内核数减去 1");
#endif
    }
#ifdef VIDEO_ROUTING
#ifdef __APPLE__
	header("视频路由（虹吸）");
#elif defined(_WIN32)
	((renderApi == 0) || (renderApi == 3)) ? header("视频路由（虹吸）") : header("视频路由（仅适用于 OpenGL 或 DirectX 11）");
#endif
	{
#ifdef _WIN32
		DisabledScope scope(!((renderApi == 0) || (renderApi == 3)));
#endif
		OptionCheckbox("将视频内容发送到其他节目", config::VideoRouting,
			"例如，将 GPU 纹理直接路由到 OBS Studio，而不是使用 CPU 密集型显示/窗口捕获");

		{
			DisabledScope scope(!config::VideoRouting);
			OptionCheckbox("发送前缩减规模", config::VideoRoutingScale, "共享较小纹理时可以提高性能，YMMV");
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

static void gui_settings_audio()
{
	OptionCheckbox("启用 DSP", config::DSPEnabled,
			"启用 Dreamcast 数字声音处理器。仅推荐在快速平台上");
    OptionCheckbox("启用VMU声音", config::VmuSound, "启用后播放 VMU 会发出声音。");

	if (OptionSlider("音量级别", config::AudioVolume, 0, 100, "调整模拟器的音频电平", "%d%%"))
	{
		config::AudioVolume.calcDbPower();
	};
#ifdef __ANDROID__
	if (config::AudioBackend.get() == "auto" || config::AudioBackend.get() == "android")
		OptionCheckbox("自动延迟", config::AutoLatency,
				"自动设置音频延迟。推荐");
#endif
    if (!config::AutoLatency
    		|| (config::AudioBackend.get() != "auto" && config::AudioBackend.get() != "android"))
    {
		int latency = (int)roundf(config::AudioBufferSize * 1000.f / 44100.f);
		ImGui::SliderInt("延迟", &latency, 12, 512, "%d ms");
		config::AudioBufferSize = (int)roundf(latency * 44100.f / 1000.f);
		ImGui::SameLine();
		ShowHelpMarker("设置最大音频延迟。并非所有音频驱动程序都支持。");
    }

	AudioBackend *backend = nullptr;
	std::string backend_name = config::AudioBackend;
	if (backend_name != "auto")
	{
		backend = AudioBackend::getBackend(config::AudioBackend);
		if (backend != nullptr)
			backend_name = backend->slug;
	}

	AudioBackend *current_backend = backend;
	if (ImGui::BeginCombo("音频驱动程序", backend_name.c_str(), ImGuiComboFlags_None))
	{
		bool is_selected = (config::AudioBackend.get() == "auto");
		if (ImGui::Selectable("auto - 自动驱动程序选择", &is_selected))
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
}

static void gui_settings_network()
{
	ImGuiStyle& style = ImGui::GetStyle();
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
		ImGui::RadioButton("禁用##network", &netType, 0);
		ImGui::NextColumn();
		ImGui::RadioButton("GGPO", &netType, 1);
		ImGui::SameLine(0, style.ItemInnerSpacing.x);
		ShowHelpMarker("使用 GGPO 启用网络");
		ImGui::NextColumn();
		ImGui::RadioButton("Naomi", &netType, 2);
		ImGui::SameLine(0, style.ItemInnerSpacing.x);
		ShowHelpMarker("为支持的 Naomi 和 Atomiswave 游戏启用网络");
		ImGui::NextColumn();
		ImGui::RadioButton("战斗电缆", &netType, 3);
		ImGui::SameLine(0, style.ItemInnerSpacing.x);
		ShowHelpMarker("模拟 Taisen （Battle） null 调制解调器电缆以用于支持它的游戏");
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
		header("Configuration");
	}
	{
		if (config::GGPOEnable)
		{
			config::NetworkEnable = false;
			OptionCheckbox("扮演玩家 1", config::ActAsServer,
					"取消选择以玩家 2 身份运行");
			ImGui::InputText("对等", &config::NetworkServer.get(), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
			ImGui::SameLine();
			ShowHelpMarker("您的对等 IP 地址和可选端口");
			OptionSlider("帧延迟", config::GGPODelay, 0, 20,
				"设置帧延迟，建议用于 ping 为 >100 毫秒的会话");

			ImGui::Text("左摇杆：");
			OptionRadioButton<int>("禁用##analogaxis", config::GGPOAnalogAxes, 0, "Left thumbstick not used");
			ImGui::SameLine();
			OptionRadioButton<int>("水平", config::GGPOAnalogAxes, 1, "仅使用左摇杆水平轴");
			ImGui::SameLine();
			OptionRadioButton<int>("全部", config::GGPOAnalogAxes, 2, "使用左摇杆水平和垂直轴");

			OptionCheckbox("启用聊天", config::GGPOChat, "收到聊天消息时打开聊天窗口");
			if (config::GGPOChat)
			{
				OptionCheckbox("启用聊天窗口超时", config::GGPOChatTimeoutToggle, "20 秒后自动关闭聊天窗口");
				if (config::GGPOChatTimeoutToggle)
				{
					char chatTimeout[256];
					snprintf(chatTimeout, sizeof(chatTimeout), "%d", (int)config::GGPOChatTimeout);
					ImGui::InputText("聊天窗口超时（秒）", chatTimeout, sizeof(chatTimeout), ImGuiInputTextFlags_CharsDecimal, nullptr, nullptr);
					ImGui::SameLine();
					ShowHelpMarker("设置聊天窗口在收到新消息后保持打开状态的持续时间。");
					config::GGPOChatTimeout.set(atoi(chatTimeout));
				}
			}
			OptionCheckbox("网络统计", config::NetworkStats,
					"在屏幕上显示网络统计信息");
		}
		else if (config::NetworkEnable)
		{
			OptionCheckbox("充当服务器", config::ActAsServer,
					"为 Naomi 网络游戏创建本地服务器");
			if (!config::ActAsServer)
			{
				ImGui::InputText("服务", &config::NetworkServer.get(), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
				ImGui::SameLine();
				ShowHelpMarker("要连接的服务器。留空可在默认端口上自动查找服务器");
			}
			char localPort[256];
			snprintf(localPort, sizeof(localPort), "%d", (int)config::LocalPort);
			ImGui::InputText("本地端口", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal, nullptr, nullptr);
			ImGui::SameLine();
			ShowHelpMarker("要使用的本地 UDP 端口");
			config::LocalPort.set(atoi(localPort));
		}
		else if (config::BattleCableEnable)
		{
			ImGui::InputText("Peer", &config::NetworkServer.get(), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
			ImGui::SameLine();
			ShowHelpMarker("要连接到的对等方。留空可在默认端口上自动查找播放器");
			char localPort[256];
			snprintf(localPort, sizeof(localPort), "%d", (int)config::LocalPort);
			ImGui::InputText("本地端口", localPort, sizeof(localPort), ImGuiInputTextFlags_CharsDecimal, nullptr, nullptr);
			ImGui::SameLine();
			ShowHelpMarker("要使用的本地 UDP 端口");
			config::LocalPort.set(atoi(localPort));
		}
	}
	ImGui::Spacing();
	header("网络选项");
	{
		OptionCheckbox("启用 UPnP", config::EnableUPnP, "自动配置网络路由器以进行网络播放");
		OptionCheckbox("广播数字输出", config::NetworkOutput, "TCP 端口 8000 上的广播数字输出和力反馈状态。 "
				"与 \"-output network\" MAME 选项兼容。仅限街机游戏。");
		{
			DisabledScope scope(game_started);

			OptionCheckbox("宽带适配器模拟", config::EmulateBBA,
					"模拟以太网宽带适配器 （BBA） 而不是调制解调器");
		}
		OptionCheckbox("使用 DCNet", config::UseDCNet, "使用 DCNet 云服务进行 Dreamcast Internet 访问。");
		ImGui::InputText("ISP 用户名", &config::ISPUsername.get(), ImGuiInputTextFlags_CharsNoBlank | ImGuiInputTextFlags_CallbackCharFilter,
				[](ImGuiInputTextCallbackData *data) { return static_cast<int>(data->EventChar <= ' ' || data->EventChar > '~'); }, nullptr);
		ImGui::SameLine();
		ShowHelpMarker("存储在控制台闪存 RAM 中的 ISP 用户名。一些网络游戏用作玩家名称。留空以保留当前闪存 RAM 值。");
	}
#ifdef NAOMI_MULTIBOARD
	ImGui::Spacing();
	header("多板屏");
	{
		//OptionRadioButton<int>("Disabled##multiboard", config::MultiboardSlaves, 0, "禁用多板（可选时）");
		OptionRadioButton<int>("1 (Twin)", config::MultiboardSlaves, 1, "单屏配置（F355 Twin）");
		ImGui::SameLine();
		OptionRadioButton<int>("3 (Deluxe)", config::MultiboardSlaves, 2, "三屏配置");
	}
#endif
}

static void gui_settings_advanced()
{
#if FEAT_SHREC != DYNAREC_NONE
    header("CPU 模式");
    {
		ImGui::Columns(2, "cpu_modes", false);
		OptionRadioButton("动态重新编译器", config::DynarecEnabled, true,
			"使用动态重新编译器。在大多数情况下推荐");
		ImGui::NextColumn();
		OptionRadioButton("解释器", config::DynarecEnabled, false,
			"使用解释器。非常慢，但在出现动力问题时可能会有所帮助");
		ImGui::Columns(1, NULL, false);

		OptionSlider("SH4 Clock", config::Sh4Clock, 100, 300,
				"对主 SH4 CPU 进行超频/降频。默认值为 200 MHz。其他值可能会崩溃、冻结或引发意外的核反应。",
				"%d MHz");
    }
#ifdef GDB_SERVER
	ImGui::Spacing();
	header("虚拟内存地址");
	{
		void *ram_base, *ram, *vram, *aram;
		addrspace::getAddress(&ram_base, &ram, &vram, &aram);

		ImGui::Text("基准地址: %p", ram_base);

		if (ram == nullptr) {
			const ImVec4 gray(0.75f, 0.75f, 0.75f, 1.f);
			ImGui::TextColored(gray, "在模拟开始之前，RAM 地址不可用");
		} else {
			ImGui::Columns(3, "virtualMemoryAddress", false);
			ImGui::Text("RAM: %p", ram);
			ImGui::NextColumn();
			ImGui::Text("VRAM64: %p", vram);
			ImGui::NextColumn();
			ImGui::Text("ARAM: %p", aram);
			ImGui::Columns(1, nullptr, false);
		}

	}
	ImGui::Spacing();
	header("调试");
	{
		OptionCheckbox("启用 GDB", config::GDB, "GDB 调试支持，禁用 Dynarec 并在连接调试器时显着降低性能。");
		OptionCheckbox("等待连接", config::GDBWaitForConnection, "连接调试器后开始模拟。");
#ifndef __ANDROID
		OptionCheckbox("串行控制台", config::SerialConsole, "将 Dreamcast 串行控制台转储到 stdout");
		OptionCheckbox("串行 PTY", config::SerialPTY, "需要选项“串行控制台”才能工作");
#endif

		static int gdbport = config::GDBPort;
		if (ImGui::InputInt("GDB 端口", &gdbport))
		{
			config::GDBPort = gdbport;
		}
		const ImGuiStyle& style = ImGui::GetStyle();
		ImGui::SameLine(0, style.ItemInnerSpacing.x);
		ShowHelpMarker("默认端口为 3263");
	}
#endif
	ImGui::Spacing();
#endif
    header("其他");
    {
    	OptionCheckbox("HLE BIOS", config::UseReios, "强制高级 BIOS 模拟");
        OptionCheckbox("多线程模拟", config::ThreadedRendering,
        		"在不同线程上运行模拟的 CPU 和 GPU");
#if !defined(__ANDROID) && !defined(GDB_SERVER)
        OptionCheckbox("串行控制台", config::SerialConsole,
        		"将 Dreamcast 串行控制台转储到 stdout");
#endif
		{
			DisabledScope scope(game_started);
			OptionCheckbox("Dreamcast 32MB 内存模组", config::RamMod32MB,
				"为 Dreamcast 启用 32MB RAM Mod。可能会影响兼容性");
		}
        OptionCheckbox("转储纹理", config::DumpTextures,
        		"将所有纹理转储到 data/texdump/<game id 中>");
        bool logToFile = cfgLoadBool("log", "LogToFile", false);
		if (ImGui::Checkbox("Log to File", &logToFile))
			cfgSaveBool("log", "LogToFile", logToFile);
        ImGui::SameLine();
        ShowHelpMarker("Log debug information to flycast.log");
#ifdef SENTRY_UPLOAD
        OptionCheckbox("自动报告崩溃", config::UploadCrashLogs,
        		"自动将崩溃报告上传到 sentry.io，以帮助进行故障排除。不包括任何个人信息。");
#endif
    }

#ifdef USE_LUA
	header("Lua Scripting");
	{
		ImGui::InputText("Lua Filename", &config::LuaFileName.get(), ImGuiInputTextFlags_CharsNoBlank, nullptr, nullptr);
		ImGui::SameLine();
		ShowHelpMarker("Specify lua filename to use. Should be located in Flycast config folder. Defaults to flycast.lua when empty.");
	}
#endif
}

#if defined(__ANDROID__) && HOST_CPU == CPU_ARM64 && USE_VULKAN
static bool driverDirty;

static void customDriverCallback(bool cancelled, std::string selection)
{
	if (!cancelled) {
		try {
			uploadCustomGpuDriver(selection);
			config::CustomGpuDriver = true;
			driverDirty = true;
		} catch (const FlycastException& e) {
			gui_error(e.what());
			config::CustomGpuDriver = false;
		}
	}
}
#endif

static void gui_settings_about()
{
    header("Flycast");
    {
		ImGui::Text("版本（邻家小熊汉化）: %s", GIT_VERSION);
		ImGui::Text("Git Hash: %s", GIT_HASH);
		ImGui::Text("构建日期: %s", BUILD_DATE);
    }
	ImGui::Spacing();
    header("平台");
    {
    	ImGui::Text("CPU: %s",
#if HOST_CPU == CPU_X86
			"x86"
#elif HOST_CPU == CPU_ARM
			"ARM"
#elif HOST_CPU == CPU_X64
			"x86/64"
#elif HOST_CPU == CPU_ARM64
			"ARM64"
#else
			"Unknown"
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
			"Unknown"
#endif
				);
#ifdef TARGET_IPHONE
		const char *getIosJitStatus();
		ImGui::Text("JIT Status: %s", getIosJitStatus());
#endif
    }
	ImGui::Spacing();
	if (isOpenGL(config::RendererType))
		header("OpenGL");
	else if (isVulkan(config::RendererType))
		header("Vulkan");
	else if (isDirectX(config::RendererType))
		header("DirectX");
	ImGui::Text("Driver Name: %s", GraphicsContext::Instance()->getDriverName().c_str());
	ImGui::Text("Version: %s", GraphicsContext::Instance()->getDriverVersion().c_str());

#if defined(__ANDROID__) && HOST_CPU == CPU_ARM64 && USE_VULKAN
	if (isVulkan(config::RendererType))
	{
		const char *fileSelectTitle = "选择自定义 GPU 驱动程序";
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
			if (config::CustomGpuDriver)
			{
				std::string name, description, vendor, version;
				if (getCustomGpuDriverInfo(name, description, vendor, version))
				{
					ImGui::Text("自定义驱动程序：");
					ImGui::Indent();
					ImGui::Text("%s - %s", name.c_str(), description.c_str());
					ImGui::Text("%s - %s", vendor.c_str(), version.c_str());
					ImGui::Unindent();
				}

				if (ImGui::Button("使用默认驱动程序")) {
					config::CustomGpuDriver = false;
					ImGui::OpenPopup("重置Vulkan");
				}
			}
			else if (ImGui::Button("上传自定义驱动程序")) {
				if (!hostfs::addStorage(false, false, fileSelectTitle, customDriverCallback))
					ImGui::OpenPopup(fileSelectTitle);
			}

			if (driverDirty) {
				ImGui::OpenPopup("重置Vulkan");
				driverDirty = false;
			}

			ImguiStyleVar _1(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));
			if (ImGui::BeginPopupModal("Reset Vulkan", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
			{
				ImGui::Text("您想重置 Vulkan 以使用新驱动程序吗？");
				ImGui::NewLine();
				ImguiStyleVar _(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
				ImguiStyleVar _1(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
				if (ImGui::Button("是"))
				{
					mainui_reinit();
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button("否"))
					ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
		}
		select_file_popup(fileSelectTitle, [](bool cancelled, std::string selection) {
				customDriverCallback(cancelled, selection);
				return true;
			}, true, "zip");
	}
#endif
}

static void gui_display_settings()
{
	static bool maple_devices_changed;

	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);

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
		ImguiStyleVar _(ImGuiStyleVar_FramePadding, ImVec2(uiScaled(16), normal_padding.y));
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
			if (ImGui::Button("制作游戏配置", ScaledVec2(0, 30)))
				config::Settings::instance().setPerGameConfig(true);
		}
	}

	if (ImGui::GetContentRegionAvail().x >= uiScaled(650.f))
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(16, 6));
	else
		// low width
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(4, 6));

    if (ImGui::BeginTabBar("settings", ImGuiTabBarFlags_NoTooltip))
    {
		if (ImGui::BeginTabItem(ICON_FA_TOOLBOX " 通用"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_settings_general();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(ICON_FA_GAMEPAD " 控制"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_settings_controls(maple_devices_changed);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(ICON_FA_DISPLAY " 视频"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_settings_video();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(ICON_FA_MUSIC " 音频"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_settings_audio();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(ICON_FA_WIFI " 网络"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_settings_network();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem(ICON_FA_MICROCHIP " 高级"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_settings_advanced();
			ImGui::EndTabItem();
		}
#if !defined(NDEBUG) || defined(DEBUGFAST) || FC_PROFILER
		if (ImGui::BeginTabItem(ICON_FA_BUG " 调试"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_debug_tab();
			ImGui::EndTabItem();
		}
#endif
		if (ImGui::BeginTabItem(ICON_FA_CIRCLE_INFO " 关于"))
		{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, normal_padding);
			gui_settings_about();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();

    scrollWhenDraggingOnVoid();
    windowDragScroll();
    ImGui::End();
}

void os_notify(const char *msg, int durationMs, const char *details)
{
	if (gui_state != GuiState::Closed)
	{
		std::lock_guard<std::mutex> _{osd_message_mutex};
		osd_message = msg;
		osd_message_end = getTimeMs() + durationMs;
	}
	else {
		toast.show(msg, details != nullptr ? details : "", durationMs);
	}
}

static std::string get_notification()
{
	std::lock_guard<std::mutex> lock(osd_message_mutex);
	if (!osd_message.empty() && getTimeMs() >= osd_message_end)
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

static bool gameImageButton(ImguiTexture& texture, const std::string& tooltip, ImVec2 size, const std::string& gameName)
{
	bool pressed = texture.button("##imagebutton", size, gameName);
	gameTooltip(tooltip);

    return pressed;
}

#ifdef TARGET_UWP
void gui_load_game()
{
	using namespace Windows::Storage;
	using namespace Concurrency;

	auto picker = ref new Pickers::FileOpenPicker();
	picker->ViewMode = Pickers::PickerViewMode::List;

	picker->FileTypeFilter->Append(".chd");
	picker->FileTypeFilter->Append(".gdi");
	picker->FileTypeFilter->Append(".cue");
	picker->FileTypeFilter->Append(".cdi");
	picker->FileTypeFilter->Append(".zip");
	picker->FileTypeFilter->Append(".7z");
	picker->FileTypeFilter->Append(".elf");
	if (!config::HideLegacyNaomiRoms)
	{
		picker->FileTypeFilter->Append(".bin");
		picker->FileTypeFilter->Append(".lst");
		picker->FileTypeFilter->Append(".dat");
	}
	picker->SuggestedStartLocation = Pickers::PickerLocationId::DocumentsLibrary;

	create_task(picker->PickSingleFileAsync()).then([](StorageFile ^file) {
		if (file)
		{
			NOTICE_LOG(COMMON, "Picked file: %S", file->Path->Data());
			nowide::stackstring path;
			if (path.convert(file->Path->Data()))
				gui_start_game(path.get());
		}
	});
}
#endif

static void gui_display_content()
{
	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::Begin("##main", NULL, ImGuiWindowFlags_NoDecoration);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(20, 8));
    ImGui::AlignTextToFramePadding();
    ImGui::Indent(uiScaled(10));
    ImGui::Text("游戏");
    ImGui::Unindent(uiScaled(10));

    static ImGuiTextFilter filter;
    const float settingsBtnW = iconButtonWidth(ICON_FA_GEAR, "设置");
#if !defined(__ANDROID__) && !defined(TARGET_IPHONE) && !defined(TARGET_UWP) && !defined(__SWITCH__)
	ImGui::SameLine(0, uiScaled(32));
	filter.Draw("Filter", ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x - uiScaled(32)
			- settingsBtnW - ImGui::GetStyle().ItemSpacing.x);
#endif
    if (gui_state != GuiState::SelectDisk)
    {
#ifdef TARGET_UWP
		ImGui::SameLine(ImGui::GetContentRegionMax().x - settingsBtnW
				- ImGui::GetStyle().FramePadding.x * 2.0f  - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize("加载中……").x);
		if (ImGui::Button("加载中……"))
			gui_load_game();
		ImGui::SameLine();
#elif defined(__SWITCH__)
		ImGui::SameLine(ImGui::GetContentRegionMax().x - settingsBtnW
				- ImGui::GetStyle().ItemSpacing.x - iconButtonWidth(ICON_FA_POWER_OFF, "Exit"));
		if (iconButton(ICON_FA_POWER_OFF, "Exit"))
			dc_exit();
		ImGui::SameLine();
#else
		ImGui::SameLine(ImGui::GetContentRegionMax().x - settingsBtnW);
#endif
		if (iconButton(ICON_FA_GEAR, "设置"))
			gui_setState(GuiState::Settings);
    }
    else
    {
		ImGui::SameLine(ImGui::GetContentRegionMax().x
				- ImGui::GetStyle().FramePadding.x * 2.0f - ImGui::CalcTextSize("取消").x);
		if (ImGui::Button("取消"))
			gui_setState(GuiState::Commands);
    }
    ImGui::PopStyleVar();

    scanner.fetch_game_list();

	// Only if Filter and Settings aren't focused... ImGui::SetNextWindowFocus();
	ImGui::BeginChild(ImGui::GetID("library"), ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_DragScrolling | ImGuiWindowFlags_NavFlattened);
    {
		const float totalWidth = ImGui::GetContentRegionMax().x - (!ImGui::GetCurrentWindow()->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0);
		const int itemsPerLine = std::max<int>(totalWidth / (uiScaled(150) + ImGui::GetStyle().ItemSpacing.x), 1);
		const float responsiveBoxSize = totalWidth / itemsPerLine - ImGui::GetStyle().FramePadding.x * 2;
		const ImVec2 responsiveBoxVec2 = ImVec2(responsiveBoxSize, responsiveBoxSize);

		if (config::BoxartDisplayMode)
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
		else
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ScaledVec2(8, 20));

		int counter = 0;
		bool gameListEmpty = false;
		{
			scanner.get_mutex().lock();
			gameListEmpty = scanner.get_game_list().empty();
			for (const auto& game : scanner.get_game_list())
			{
				if (gui_state == GuiState::SelectDisk)
				{
					std::string extension = get_file_extension(game.path);
					if (!game.device && extension != "gdi" && extension != "chd"
							&& extension != "cdi" && extension != "cue")
						// Only dreamcast disks
						continue;
					if (game.path.empty())
						// Dreamcast BIOS isn't a disk
						continue;
				}
				std::string gameName = game.name;
				GameBoxart art;
				if (config::BoxartDisplayMode && !game.device)
				{
					art = boxart.getBoxartAndLoad(game);
					gameName = art.name;
				}
				if (filter.PassFilter(gameName.c_str()))
				{
					ImguiID _(game.path.empty() ? "bios" : game.path);
					bool pressed = false;
					if (config::BoxartDisplayMode)
					{
						if (counter % itemsPerLine != 0)
							ImGui::SameLine();
						counter++;
						// Put the image inside a child window so we can detect when it's fully clipped and doesn't need to be loaded
						if (ImGui::BeginChild("img", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NavFlattened))
						{
							ImguiFileTexture tex(art.boxartPath);
							pressed = gameImageButton(tex, game.name, responsiveBoxVec2, gameName);
						}
						ImGui::EndChild();
					}
					else
					{
						pressed = ImGui::Selectable(gameName.c_str());
					}
					if (pressed)
					{
						if (!config::BoxartDisplayMode)
							art = boxart.getBoxart(game);
						settings.content.title = art.name;
						if (settings.content.title.empty() || settings.content.title == game.fileName)
							settings.content.title = get_file_basename(game.fileName);
						if (gui_state == GuiState::SelectDisk)
						{
							try {
								emu.insertGdrom(game.path);
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
							break;
						}
					}
				}
			}
			scanner.get_mutex().unlock();
		}
		bool addContent = false;
#if !defined(TARGET_IPHONE)
		if (gameListEmpty && gui_state != GuiState::SelectDisk)
		{
			const char *label = "Yóu xì liè biǎo wéi kōng";
			// center horizontally
			const float w = largeFont->CalcTextSizeA(largeFont->FontSize, FLT_MAX, -1.f, label).x + ImGui::GetStyle().FramePadding.x * 2;
			ImGui::SameLine((ImGui::GetContentRegionMax().x - w) / 2);
			if (ImGui::BeginChild("empty", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NavFlattened))
			{
				ImGui::PushFont(largeFont);
				ImGui::NewLine();
				ImGui::Text("%s", label);
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 8));
				addContent = ImGui::Button("Tiān jiā yóu xì wén jiàn jiā");
				ImGui::PopFont();
			}
			ImGui::EndChild();
		}
#endif
        ImGui::PopStyleVar();
        addContentPath(addContent);
    }
    scrollWhenDraggingOnVoid();
    windowDragScroll();
	ImGui::EndChild();
	ImGui::End();

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
			WARN_LOG(BOOT, "无法创建 'data' 目录: %s", data_path.c_str());
			gui_error("选择无效：\nFlycast 无法写入此文件夹。");
			return false;
		}
	}
	// We might be able to create a directory but not a file. Because ... android
	// So let's test to be sure.
	std::string testPath = data_path + "writetest.txt";
	FILE *file = fopen(testPath.c_str(), "w");
	if (file == nullptr)
	{
		WARN_LOG(BOOT, "无法写入“data”目录");
		gui_error("选择无效：\nFlycast 无法写入此文件夹。");
		return false;
	}
	fclose(file);
	unlink(testPath.c_str());

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
	const char *title = "选择 Flycast 主文件夹";
	ImGui::OpenPopup(title);
	select_file_popup(title, &systemdir_selected_callback);
}

static void drawBoxartBackground()
{
	GameMedia game;
	game.path = settings.content.path;
	game.fileName = settings.content.fileName;
	GameBoxart art = boxart.getBoxart(game);
	ImguiFileTexture tex(art.boxartPath);
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	tex.draw(dl, ImVec2(0, 0), ImVec2(settings.display.width, settings.display.height), 1.f);
}

static std::future<bool> networkStatus;

static void gui_network_start()
{
	drawBoxartBackground();
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(330, 0));
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _1(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

	ImGui::Begin("##network", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);

	ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
	ImGui::AlignTextToFramePadding();
	ImGui::SetCursorPosX(uiScaled(20.f));

	if (networkStatus.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
	{
		ImGui::Text("加载中……");
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
		ImGui::Text("启动网络...");
		if (NetworkHandshake::instance->canStartNow())
			ImGui::Text("按开始键开始游戏。");
	}
	ImGui::Text("%s", get_notification().c_str());

	float currentwidth = ImGui::GetContentRegionAvail().x;
	ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
	if (ImGui::Button("取消", ScaledVec2(100.f, 0)) && NetworkHandshake::instance != nullptr)
	{
		NetworkHandshake::instance->stop();
		try {
			networkStatus.get();
		}
		catch (const FlycastException& e) {
		}
		gui_stop_game();
	}
	ImGui::End();

	if ((kcode[0] & DC_BTN_START) == 0 && NetworkHandshake::instance != nullptr)
		NetworkHandshake::instance->startNow();
}

static void gui_display_loadscreen()
{
	drawBoxartBackground();
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(330, 0));
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

    if (ImGui::Begin("##loading", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
    {
		ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
		ImGui::AlignTextToFramePadding();
		ImGui::SetCursorPosX(uiScaled(20.f));
		try {
			const char *label = gameLoader.getProgress().label;
			if (label == nullptr)
			{
				if (gameLoader.ready())
					label = "正在开始……";
				else
					label = "正在加载……";
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
				{
					ImguiStyleColor _(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
					ImGui::ProgressBar(gameLoader.getProgress().progress, ImVec2(-1, uiScaled(20.f)), "");
				}

				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
				if (ImGui::Button("取消", ScaledVec2(100.f, 0)))
					gameLoader.cancel();
			}
		} catch (const FlycastException& ex) {
			ERROR_LOG(BOOT, "%s", ex.what());
#ifdef TEST_AUTOMATION
			die("游戏加载失败");
#endif
			gui_stop_game(ex.what());
		}
    }
    ImGui::End();
}

void gui_display_ui()
{
	FC_PROFILE_SCOPE;
	const LockGuard lock(guiMutex);

	if (gui_state == GuiState::Closed)
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
		vgamepad::draw();
		break;
	case GuiState::VJoyEditCommands:
		vgamepad::displayCommands();
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
	case GuiState::Achievements:
#ifdef USE_RACHIEVEMENTS
		achievements::achievementList();
		break;
#endif
	default:
		die("Unknown UI state");
		break;
	}
	error_popup();
    ImGui::Render();
	gui_endFrame(gui_open);
	uiThreadRunner.execTasks();
	ImguiFileTexture::resetLoadCount();

	if (gui_state == GuiState::Closed)
		emu.start();
}

static u64 LastFPSTime;
static int lastFrameCount = 0;
static float fps = -1;

static std::string getFPSNotification()
{
	if (config::ShowFPS)
	{
		u64 now = getTimeMs();
		if (now - LastFPSTime >= 1000) {
			fps = ((float)MainFrameCount - lastFrameCount) * 1000.f / (now - LastFPSTime);
			LastFPSTime = now;
			lastFrameCount = MainFrameCount;
		}
		if (fps >= 0.f && fps < 9999.f) {
			char text[32];
			snprintf(text, sizeof(text), "F:%4.1f%s", fps, settings.input.fastForwardMode ? " >>" : "");

			return std::string(text);
		}
	}
	return std::string(settings.input.fastForwardMode ? ">>" : "");
}

void gui_draw_osd()
{
	gui_newFrame();
	ImGui::NewFrame();

#ifdef USE_RACHIEVEMENTS
	if (!achievements::notifier.draw())
#endif
		if (!toast.draw())
		{
			std::string message = getFPSNotification();
			if (!message.empty())
			{
				const float maxW = uiScaled(640.f);
				ImDrawList *dl = ImGui::GetForegroundDrawList();
				const ScaledVec2 padding(5.f, 5.f);
				const ImVec2 size = largeFont->CalcTextSizeA(largeFont->FontSize, FLT_MAX, maxW, &message.front(), &message.back() + 1)
						+ padding * 2.f;
				ImVec2 pos(insetLeft, ImGui::GetIO().DisplaySize.y - size.y);
				constexpr float alpha = 0.7f;
				const ImU32 bg_col = alphaOverride(0x00202020, alpha / 2.f);
				dl->AddRectFilled(pos, pos + size, bg_col, 0.f);
				pos += padding;
				const ImU32 col = alphaOverride(0x0000FFFF, alpha);
				dl->AddText(largeFont, largeFont->FontSize, pos, col, &message.front(), &message.back() + 1, maxW);
			}
		}

	if (ggpo::active())
	{
		if (config::NetworkStats)
			ggpo::displayStats();
		chat.display();
	}
	if (!settings.raHardcoreMode)
		lua::overlay();
	vgamepad::draw();
    ImGui::Render();
	uiThreadRunner.execTasks();
}

void gui_display_osd()
{
	gui_draw_osd();
	gui_endFrame(gui_is_open());
}

void gui_display_profiler()
{
#if FC_PROFILER
	gui_newFrame();
	ImGui::NewFrame();

	ImGui::Begin("Profiler", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);

	{
		ImguiStyleColor _(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));

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
	}

	for (const fc_profiler::ProfileThread* profileThread : fc_profiler::ProfileThread::s_allThreads)
	{
		fc_profiler::drawGraph(*profileThread);
	}

	ImGui::End();
    ImGui::Render();
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
	    boxart.term();
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

    os_notify("Fatal Error", 20000, temp);
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

void gui_saveState(bool stopRestart)
{
	const LockGuard lock(guiMutex);
	if ((gui_state == GuiState::Closed || !stopRestart) && savestateAllowed())
	{
		try {
			if (stopRestart)
				emu.stop();
			savestate();
			if (stopRestart)
				emu.start();
		} catch (const FlycastException& e) {
			if (stopRestart)
				gui_stop_game(e.what());
			else
				WARN_LOG(COMMON, "gui_saveState: %s", e.what());
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

std::string gui_getCurGameBoxartUrl()
{
	GameMedia game;
	game.fileName = settings.content.fileName;
	game.path = settings.content.path;
	GameBoxart art = boxart.getBoxart(game);
	return art.boxartUrl;
}

void gui_runOnUiThread(std::function<void()> function) {
	uiThreadRunner.runOnThread(function);
}

void gui_takeScreenshot()
{
	if (!game_started)
		return;
	gui_runOnUiThread([]() {
		std::string date = timeToISO8601(time(nullptr));
		std::replace(date.begin(), date.end(), '/', '-');
		std::replace(date.begin(), date.end(), ':', '-');
		std::string name = "Flycast-" + date + ".png";

		std::vector<u8> data;
		getScreenshot(data);
		if (data.empty()) {
			os_notify("没有可用的屏幕截图", 2000);
		}
		else
		{
			try {
				hostfs::saveScreenshot(name, data);
				os_notify("截图已保存", 2000, name.c_str());
			} catch (const FlycastException& e) {
				os_notify("保存屏幕截图时出错", 5000, e.what());
			}
		}
	});
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
