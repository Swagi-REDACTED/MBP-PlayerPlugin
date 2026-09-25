#pragma once

#include <imgui.h>
#include <string>
#include <string_view>
#include <unordered_map>

namespace htmlui
{
    struct PanelStyle
    {
        ImU32 background = IM_COL32(10, 12, 22, 210);
        ImU32 border = IM_COL32(255, 255, 255, 24);
        float radius = 20.0f;
        float borderThickness = 1.0f;
        float shadow = 18.0f;
    };

    // Tiny declarative panel renderer used instead of the legacy D3D11
    // HtmlRenderer. It parses a deliberately small HTML-like <panel> dialect
    // and draws through ImGui, so the actual GPU backend remains D3D12.
    class Renderer
    {
    public:
        void SetHTML(std::string html);
        const PanelStyle& Style(std::string_view id) const;
        void DrawPanel(ImDrawList* drawList, std::string_view id, ImVec2 min, ImVec2 max, float alpha = 1.0f) const;

    private:
        static ImU32 ParseColor(std::string_view text, ImU32 fallback);
        static std::string Attribute(std::string_view tag, std::string_view name);
        static float AttributeFloat(std::string_view tag, std::string_view name, float fallback);

        std::string html_;
        std::unordered_map<std::string, PanelStyle> styles_;
        PanelStyle fallback_{};
    };
}
