/*
	Copyright 2021 flyinghead

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
#include "imgui.h"
#include "gui_util.h"
#include "cheats.h"
#include "IconsFontAwesome6.h"
#ifdef __ANDROID__
#include "oslib/storage.h"
#endif

static void addCheat()
{
	static char cheatName[64];
	static char cheatCode[128];
    centerNextWindow();
    ImGui::SetNextWindowSize(min(ImGui::GetIO().DisplaySize, ScaledVec2(600.f, 400.f)));
    ImguiStyleVar _(ImGuiStyleVar_WindowBorderSize, 1);

    if (ImGui::BeginPopupModal("addCheat", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
    		| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize))
    {
    	{
			ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 8));
			ImGui::AlignTextToFramePadding();
			ImGui::Indent(uiScaled(10));
			ImGui::Text("ADD CHEAT");

			ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("取消").x - ImGui::GetStyle().FramePadding.x * 4.f
				- ImGui::CalcTextSize("好").x - ImGui::GetStyle().ItemSpacing.x);
			if (ImGui::Button("取消"))
				ImGui::CloseCurrentPopup();
			ImGui::SameLine();
			if (ImGui::Button("好"))
			{
				try {
					cheatManager.addGameSharkCheat(cheatName, cheatCode);
					ImGui::CloseCurrentPopup();
					cheatName[0] = 0;
					cheatCode[0] = 0;
				} catch (const FlycastException& e) {
					gui_error(e.what());
				}
			}

			ImGui::Unindent(uiScaled(10));
		}

		ImGui::BeginChild(ImGui::GetID("input"), ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_NavFlattened);
		{
			ImGui::InputText("名称", cheatName, sizeof(cheatName), 0, nullptr, nullptr);
			ImGui::InputTextMultiline("代码", cheatCode, sizeof(cheatCode), ImVec2(0, ImGui::GetTextLineHeight() * 8), 0, nullptr, nullptr);
		}
		ImGui::EndChild();
		ImGui::EndPopup();
    }
}

static void cheatFileSelected(bool cancelled, std::string path)
{
	if (!cancelled)
		gui_runOnUiThread([path]() {
			cheatManager.loadCheatFile(path);
		});
}

void gui_cheats()
{
	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar
    		| ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);

	const char *title = "选择一个金手指文件";
    {
		ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 8));
		ImGui::AlignTextToFramePadding();
		ImGui::Indent(uiScaled(10));
		ImGui::Text(ICON_FA_MASK "  金手指");

		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("添加").x  - ImGui::CalcTextSize("关闭").x - ImGui::GetStyle().FramePadding.x * 6.f
			- ImGui::CalcTextSize("加载").x - ImGui::GetStyle().ItemSpacing.x * 2);
		if (ImGui::Button("添加"))
			ImGui::OpenPopup("addCheat");
		addCheat();
		ImGui::SameLine();
#ifdef __ANDROID__
		if (ImGui::Button("加载"))
		{
			if (!hostfs::addStorage(false, true, title, cheatFileSelected))
				ImGui::OpenPopup(title);
		}
#else
		if (ImGui::Button("加载"))
			ImGui::OpenPopup(title);
#endif

		ImGui::SameLine();
		if (ImGui::Button("关闭"))
			gui_setState(GuiState::Commands);

		ImGui::Unindent(uiScaled(10));
    }
	select_file_popup(title, [](bool cancelled, std::string selection)
		{
			cheatFileSelected(cancelled, selection);
			return true;
		}, true, "cht");

	ImGui::BeginChild(ImGui::GetID("cheats"), ImVec2(0, 0), ImGuiChildFlags_Border, ImGuiWindowFlags_DragScrolling | ImGuiWindowFlags_NavFlattened);
    {
		if (cheatManager.cheatCount() == 0)
			ImGui::Text("(没有加载金手指)");
		else
			for (size_t i = 0; i < cheatManager.cheatCount(); i++)
			{
				ImguiID _(("cheat" + std::to_string(i)).c_str());
				bool v = cheatManager.cheatEnabled(i);
				if (ImGui::Checkbox(cheatManager.cheatDescription(i).c_str(), &v))
					cheatManager.enableCheat(i, v);
			}
    }
    scrollWhenDraggingOnVoid();
    windowDragScroll();

	ImGui::EndChild();
	ImGui::End();
}
