#include "ui/theme.h"

namespace nonstop {

void applyDarkTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 9.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 5.0f;

    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 6.0f);
    style.ScrollbarSize = 14.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.50f, 0.56f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.08f, 0.08f, 0.10f, 0.96f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.11f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.12f, 0.15f, 0.98f);
    colors[ImGuiCol_Border]                = ImVec4(0.20f, 0.22f, 0.28f, 0.70f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.14f, 0.15f, 0.19f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.24f, 0.26f, 0.33f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.08f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.12f, 0.13f, 0.18f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.06f, 0.06f, 0.08f, 0.75f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.10f, 0.13f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.08f, 0.08f, 0.10f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.25f, 0.27f, 0.35f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.35f, 0.38f, 0.48f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.45f, 0.48f, 0.60f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.65f, 0.35f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.60f, 0.30f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.75f, 0.40f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.20f, 0.16f, 0.28f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.32f, 0.24f, 0.48f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.42f, 0.30f, 0.62f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.22f, 0.18f, 0.32f, 0.80f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.30f, 0.24f, 0.44f, 0.80f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.40f, 0.30f, 0.58f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.22f, 0.24f, 0.30f, 0.60f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.35f, 0.38f, 0.50f, 0.78f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.50f, 0.54f, 0.70f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.22f, 0.28f, 0.25f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.40f, 0.44f, 0.55f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.60f, 0.35f, 0.90f, 0.95f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.14f, 0.14f, 0.18f, 0.86f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.30f, 0.24f, 0.44f, 0.80f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.26f, 0.20f, 0.38f, 1.00f);
    colors[ImGuiCol_TabUnfocused]          = ImVec4(0.10f, 0.10f, 0.13f, 0.97f);
    colors[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.18f, 0.15f, 0.25f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.00f, 0.96f, 0.83f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.30f, 1.00f, 0.90f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.65f, 0.35f, 0.95f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.80f, 0.45f, 1.00f, 1.00f);
}

} // namespace nonstop
