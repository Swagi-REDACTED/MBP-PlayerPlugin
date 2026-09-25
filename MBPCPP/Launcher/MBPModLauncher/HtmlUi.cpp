#include "HtmlUi.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace htmlui
{
    namespace
    {
        ImU32 WithAlpha(ImU32 color, float alpha)
        {
            const int original = (color >> IM_COL32_A_SHIFT) & 0xFF;
            const int scaled = std::clamp(static_cast<int>(std::lround(original * std::clamp(alpha, 0.0f, 1.0f))), 0, 255);
            return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(scaled) << IM_COL32_A_SHIFT);
        }
    }

    std::string Renderer::Attribute(std::string_view tag, std::string_view name)
    {
        const std::string needle = std::string(name) + "=\"";
        const size_t begin = tag.find(needle);
        if (begin == std::string_view::npos)
            return {};
        const size_t valueStart = begin + needle.size();
        const size_t end = tag.find('"', valueStart);
        if (end == std::string_view::npos)
            return {};
        return std::string(tag.substr(valueStart, end - valueStart));
    }

    float Renderer::AttributeFloat(std::string_view tag, std::string_view name, float fallback)
    {
        const std::string value = Attribute(tag, name);
        if (value.empty()) return fallback;
        char* end = nullptr;
        const float parsed = std::strtof(value.c_str(), &end);
        return end != value.c_str() ? parsed : fallback;
    }

    ImU32 Renderer::ParseColor(std::string_view text, ImU32 fallback)
    {
        if (text.size() != 7 && text.size() != 9) return fallback;
        if (text.front() != '#') return fallback;

        auto byteAt = [&](size_t offset, unsigned& out) -> bool
        {
            const std::string pair(text.substr(offset, 2));
            char* end = nullptr;
            const unsigned long value = std::strtoul(pair.c_str(), &end, 16);
            if (end != pair.c_str() + 2) return false;
            out = static_cast<unsigned>(value);
            return true;
        };

        unsigned r = 0, g = 0, b = 0, a = 255;
        if (!byteAt(1, r) || !byteAt(3, g) || !byteAt(5, b)) return fallback;
        if (text.size() == 9 && !byteAt(7, a)) return fallback;
        return IM_COL32(r, g, b, a);
    }

    void Renderer::SetHTML(std::string html)
    {
        html_ = std::move(html);
        styles_.clear();

        size_t cursor = 0;
        while ((cursor = html_.find("<panel", cursor)) != std::string::npos)
        {
            const size_t end = html_.find('>', cursor);
            if (end == std::string::npos) break;
            const std::string_view tag(html_.data() + cursor, end - cursor + 1);
            const std::string id = Attribute(tag, "id");
            if (!id.empty())
            {
                PanelStyle style = fallback_;
                style.background = ParseColor(Attribute(tag, "background"), style.background);
                style.border = ParseColor(Attribute(tag, "border"), style.border);
                style.radius = AttributeFloat(tag, "radius", style.radius);
                style.borderThickness = AttributeFloat(tag, "border-width", style.borderThickness);
                style.shadow = AttributeFloat(tag, "shadow", style.shadow);
                styles_.insert_or_assign(id, style);
            }
            cursor = end + 1;
        }
    }

    const PanelStyle& Renderer::Style(std::string_view id) const
    {
        const auto it = styles_.find(std::string(id));
        return it == styles_.end() ? fallback_ : it->second;
    }

    void Renderer::DrawPanel(ImDrawList* drawList, std::string_view id, ImVec2 min, ImVec2 max, float alpha) const
    {
        if (!drawList) return;
        const PanelStyle& style = Style(id);

        if (style.shadow > 0.0f)
        {
            for (int i = 5; i >= 1; --i)
            {
                const float t = static_cast<float>(i) / 5.0f;
                const float expand = style.shadow * t * 0.45f;
                const ImU32 shadow = IM_COL32(0, 0, 0, static_cast<int>(28.0f * (1.0f - t * 0.55f) * alpha));
                drawList->AddRectFilled(
                    ImVec2(min.x - expand, min.y - expand + 4.0f),
                    ImVec2(max.x + expand, max.y + expand + 4.0f),
                    shadow,
                    style.radius + expand);
            }
        }

        drawList->AddRectFilled(min, max, WithAlpha(style.background, alpha), style.radius);
        if (style.borderThickness > 0.0f)
            drawList->AddRect(min, max, WithAlpha(style.border, alpha), style.radius, 0, style.borderThickness);
    }
}
