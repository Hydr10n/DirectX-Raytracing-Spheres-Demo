#pragma once

#include "imgui_internal.h"

namespace ImGuiEx {
	struct ScopedEnablement {
		ScopedEnablement(bool value) {
			ImGui::PushItemFlag(ImGuiItemFlags_Disabled, !value);
			ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (value ? 1.0f : 0.5f));
		}

		~ScopedEnablement() {
			ImGui::PopStyleVar();
			ImGui::PopItemFlag();
		}
	};

	inline void AlignForWidth(float width, float alignment = 0.5f) {
		if (const auto offset = (ImGui::GetContentRegionAvail().x - width) * alignment; offset > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offset);
	}

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

	inline ImGuiInputEvent* FindLatestInputEvent(ImGuiContext* context, ImGuiInputEventType type, int arg = -1) {
		auto& g = *context;
		for (auto i = g.InputEventsQueue.Size - 1; i >= 0; i--) {
			if (const auto e = &g.InputEventsQueue[i];
				e->Type == type && (type != ImGuiInputEventType_Key || e->Key.Key == arg) && (type != ImGuiInputEventType_MouseButton || e->MouseButton.Button == arg)) return e;
		}
		return nullptr;
	}
}
