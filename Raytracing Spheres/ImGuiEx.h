#pragma once

#include "imgui_internal.h"

namespace ImGuiEx {
	struct ScopedEnablement {
		ScopedEnablement(bool enable) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, !enable);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (enable ? 1.0f : 0.5f));
		}

		~ScopedEnablement() {
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}
	};

	inline void AddUnderline(const ImColor& color) {
		auto itemRectMin = ImGui::GetItemRectMin(), itemRectMax = ImGui::GetItemRectMax();
		itemRectMin.y = itemRectMax.y;
		ImGui::GetWindowDrawList()->AddLine(itemRectMin, itemRectMax, color);
	}

	inline auto Hyperlink(const char* label, const char* link) {
		const auto& style = ImGui::GetStyle();
		ImGui::PushStyleColor(ImGuiCol_Text, style.Colors[ImGuiCol_ButtonHovered]);
		ImGui::Text(label);
		ImGui::PopStyleColor();
		if (ImGui::IsItemHovered()) {
			AddUnderline(style.Colors[ImGuiCol_ButtonHovered]);
			ImGui::SetTooltip(link);
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return true;
		}
		else AddUnderline(style.Colors[ImGuiCol_Button]);
		return false;
	}
}
