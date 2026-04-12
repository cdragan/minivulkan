// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

#include "sculptor_asset_browser.h"
#include "sculptor_asset_list.h"

#include "../core/d_printf.h"
#include "../core/gui_imgui.h"
#include "../core/load_png.h"
#include "../core/minivulkan.h"

#include "assets.png.h"

const char* Sculptor::AssetBrowser::get_editor_name() const
{
    return "Asset Browser";
}

bool Sculptor::AssetBrowser::render_tab_button(AssetTab tab)
{
    constexpr float padding  = 4.0f;
    constexpr float rounding = 4.0f;

    const uint32_t tab_idx  = static_cast<uint32_t>(tab);
    const float    tab_f    = static_cast<float>(tab_idx);
    const ImVec2   total_size{icon_h + padding * 2, icon_h + padding * 2};
    const ImVec2   uv0{tab_f * tab_uv_width, 0};
    const ImVec2   uv1{(tab_f + 1.0f) * tab_uv_width, 1};

    const bool is_active = (active_tab == tab);

    // Invisible button for hit detection
    ImGui::PushID(static_cast<int>(tab_idx));
    const bool clicked    = ImGui::InvisibleButton("tab", total_size);
    const bool is_hovered = ImGui::IsItemHovered();
    ImGui::PopID();

    const ImVec2 btn_min = ImGui::GetItemRectMin();
    const ImVec2 btn_max = ImGui::GetItemRectMax();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    // Draw tab background: rounded on left, flat on right (connects to list)
    if (is_active) {
        const ImU32 tab_bg = ImGui::GetColorU32(ImGuiCol_Tab, 1.8f);
        draw_list->AddRectFilled(btn_min, btn_max, tab_bg,
                                 rounding, ImDrawFlags_RoundCornersLeft);
    }
    else if (is_hovered) {
        const ImU32 hover_bg = ImGui::GetColorU32(ImGuiCol_TabHovered, 0.5f);
        draw_list->AddRectFilled(btn_min, btn_max, hover_bg,
                                 rounding, ImDrawFlags_RoundCornersLeft);
    }

    // Draw the icon image on top
    const ImVec2 icon_min{btn_min.x + padding, btn_min.y + padding};
    const ImVec2 icon_max{btn_max.x - padding, btn_max.y - padding};
    draw_list->AddImage(make_texture_id(icon_texture), icon_min, icon_max, uv0, uv1);

    static const char* const tab_tooltips[] = {
        "Geometries",
        "Textures",
        "Materials",
    };

    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tab_tooltips[tab_idx]);

    return clicked;
}

void Sculptor::AssetBrowser::register_tab(AssetTab         tab,
                                          AssetSlotHeader* slots,
                                          uint32_t         slot_stride,
                                          AssetType        asset_type,
                                          AssetListState*  state,
                                          const char*      add_label,
                                          const char*      default_name)
{
    TabData& td    = tabs[static_cast<int>(tab)];
    td.slots        = slots;
    td.slot_stride  = slot_stride;
    td.asset_type   = asset_type;
    td.state        = state;
    td.add_label    = add_label;
    td.default_name = default_name;
}

int32_t Sculptor::AssetBrowser::get_selected_slot() const
{
    return tabs[static_cast<int>(active_tab)].selected;
}

bool Sculptor::AssetBrowser::allocate_resources()
{
    if ( ! icon_sampler) {
        static const VkSamplerCreateInfo sampler_info = {
            VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            nullptr,
            0,                                          // flags
            VK_FILTER_NEAREST,                          // magFilter
            VK_FILTER_NEAREST,                          // minFilter
            VK_SAMPLER_MIPMAP_MODE_NEAREST,             // mipmapMode
            VK_SAMPLER_ADDRESS_MODE_REPEAT,             // addressModeU
            VK_SAMPLER_ADDRESS_MODE_REPEAT,             // addressModeV
            VK_SAMPLER_ADDRESS_MODE_REPEAT,             // addressModeW
            0,                                          // mipLodBias
            VK_FALSE,                                   // anisotropyEnable
            0,                                          // maxAnisotropy
            VK_FALSE,                                   // compareEnable
            VK_COMPARE_OP_NEVER,                        // compareOp
            0,                                          // minLod
            0,                                          // maxLod
            VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,    // borderColor
            VK_FALSE                                    // unnormalizedCoordinates
        };

        const VkResult res = CHK(vkCreateSampler(vk_dev, &sampler_info, nullptr, &icon_sampler));
        if (res != VK_SUCCESS)
            return false;
    }

    if ( ! icon_texture) {
        if ( ! load_png(assets, sizeof(assets), &icon_image))
            return false;

        icon_texture = ImGui_ImplVulkan_AddTexture(icon_sampler,
                                                    icon_image.get_view(),
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        icon_h       = static_cast<float>(icon_image.get_height());
        tab_uv_width = 1.0f / static_cast<float>(AssetTab::num_tabs);
    }

    return true;
}

void Sculptor::AssetBrowser::free_resources()
{
}

bool Sculptor::AssetBrowser::create_gui_frame(uint32_t image_idx, bool* need_realloc, const UserInput& input)
{
    ImGui::Begin("Asset Browser###AssetBrowser");

    const float tab_btn_size  = icon_h + 8.0f; // icon + 2*padding
    const float tab_col_width = tab_btn_size + 2.0f;

    // Left column: vertical icon tabs, no border, flush against list
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0, 2});
    ImGui::BeginChild("asset_tabs", ImVec2{tab_col_width, 0}, false);
    {
        for (uint32_t i = 0; i < static_cast<uint32_t>(AssetTab::num_tabs); i++) {
            const AssetTab tab = static_cast<AssetTab>(i);
            if (render_tab_button(tab))
                active_tab = tab;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine(0, 0);

    // Right area: asset list for the active tab
    ImGui::BeginChild("asset_list", ImVec2{0, 0}, true);
    {
        TabData& td = tabs[static_cast<int>(active_tab)];
        if (td.slots)
            render_asset_list(td.slots, td.slot_stride, td.asset_type,
                              td.state, &td.selected, td.add_label, td.default_name);
        else
            ImGui::TextDisabled("(not yet available)");
    }
    ImGui::EndChild();

    ImGui::End();

    return true;
}

bool Sculptor::AssetBrowser::draw_frame(VkCommandBuffer cmdbuf, uint32_t image_idx)
{
    if ( ! icon_image.send_to_gpu(cmdbuf))
        return false;

    return true;
}
