#include "Engine/UI/UIStage.h"

#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/VN/Runtime/Dialogue.h"

#include <algorithm>
#include <cmath>

namespace px::ui {

namespace {
constexpr float kChoiceH = 64.0f;
constexpr float kChoiceGap = 14.0f;

float Ease(const std::string& kind, float p) {
    p = std::clamp(p, 0.0f, 1.0f);
    if (kind == "linear") return p;
    if (kind == "inCubic") return p * p * p;
    if (kind == "inOutQuad") return p < 0.5f ? 2 * p * p : 1 - std::pow(-2 * p + 2, 2) / 2;
    if (kind == "outQuad") return 1 - (1 - p) * (1 - p);
    return 1 - std::pow(1 - p, 3);
}

void AnchorOrigin(Anchor a, int cw, int ch, float& ox, float& oy) {
    const int col = static_cast<int>(a) % 3;
    const int row = static_cast<int>(a) / 3;
    ox = col == 0 ? 0.0f : (col == 1 ? cw * 0.5f : static_cast<float>(cw));
    oy = row == 0 ? 0.0f : (row == 1 ? ch * 0.5f : static_cast<float>(ch));
}
}

void UIStage::Load(UIScene scene) {
    m_scene = std::move(scene);
    m_anims.assign(m_scene.nodes.size(), NodeAnim{});
    TriggerEnter();
}

void UIStage::TriggerEnter() {
    for (NodeAnim& a : m_anims) {
        a.elapsed = 0.0f;
        a.active = true;
    }
}

Rect UIStage::ScreenRect(const UINode& node, graphics::Renderer2D& r) const {
    int w = 0, h = 0;
    r.GetLogicalSize(w, h);
    return ScreenRect(node, w, h);
}

Rect UIStage::ScreenRect(const UINode& node, int logicalW, int logicalH) const {
    float ox = 0, oy = 0;
    AnchorOrigin(node.anchor, m_scene.canvasW, m_scene.canvasH, ox, oy);
    const float sx = m_scene.canvasW ? static_cast<float>(logicalW) / m_scene.canvasW : 1.0f;
    const float sy = m_scene.canvasH ? static_cast<float>(logicalH) / m_scene.canvasH : 1.0f;
    return Rect{ (ox + node.rect.x) * sx, (oy + node.rect.y) * sy, node.rect.w * sx,
                 node.rect.h * sy };
}

void UIStage::ApplyAnim(const UINode& node, const NodeAnim& anim, Rect& rect, float& opacity) const {
    if (!anim.active) {
        return;
    }
    for (const AnimClip& clip : node.animations) {
        if (clip.trigger != "scene.enter") {
            continue;
        }
        const float t = anim.elapsed - clip.delay;
        auto sample = [&](const std::string& prop, float fallback) {
            float result = fallback;
            bool found = false;
            const AnimKey* prev = nullptr;
            for (const AnimKey& k : clip.keys) {
                if (k.property != prop) continue;
                found = true;
                if (t <= k.time) {
                    if (!prev) return k.value;
                    const float span = std::max(0.0001f, k.time - prev->time);
                    const float p = Ease(clip.easing, (t - prev->time) / span);
                    return prev->value + (k.value - prev->value) * p;
                }
                prev = &k;
                result = k.value;
            }
            return found ? result : fallback;
        };
        if (t < 0.0f) {
            opacity = sample("opacity", opacity);
            continue;
        }
        opacity = sample("opacity", opacity);
        rect.x += sample("x", 0.0f);
        rect.y += sample("y", 0.0f);
        const float scale = sample("scale", 1.0f);
        if (scale != 1.0f) {
            const float nw = rect.w * scale, nh = rect.h * scale;
            rect.x -= (nw - rect.w) * 0.5f;
            rect.y -= (nh - rect.h) * 0.5f;
            rect.w = nw;
            rect.h = nh;
        }
    }
}

std::vector<Rect> UIStage::GridCells(const UINode& node, const Rect& area,
                                     std::size_t count) const {
    const int cols = node.type == NodeType::GalleryGrid ? 4 : 2;
    const float pad = 12.0f;
    const float cw = (area.w - pad * (cols + 1)) / cols;
    const float ch = node.type == NodeType::GalleryGrid ? cw * 9.0f / 16.0f : 96.0f;
    std::vector<Rect> cells;
    cells.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const int r = static_cast<int>(i) / cols;
        const int c = static_cast<int>(i) % cols;
        cells.push_back(Rect{ area.x + pad + c * (cw + pad), area.y + pad + r * (ch + pad), cw, ch });
    }
    return cells;
}

std::vector<std::size_t> UIStage::DrawOrder() const {
    std::vector<std::size_t> order(m_scene.nodes.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return m_scene.nodes[a].order < m_scene.nodes[b].order;
    });
    return order;
}

std::optional<UIAction> UIStage::Update(const Input& input, float dt) {
    m_mx = input.MouseX();
    m_my = input.MouseY();
    for (NodeAnim& a : m_anims) {
        if (a.active) a.elapsed += dt;
    }
    if (!input.LeftClick()) {
        return std::nullopt;
    }

    const std::vector<std::size_t> order = DrawOrder();
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const UINode& node = m_scene.nodes[*it];
        if (!node.visible || node.locked) continue;
        const Rect rect = ScreenRect(node, 1280, 720);
        if (node.type == NodeType::Button && rect.Contains(m_mx, m_my)) {
            return UIAction{ node.actionType, node.actionTarget, node.actionArg, node.id };
        }
        if (node.type == NodeType::ChoiceList && m_choices) {
            for (std::size_t c = 0; c < m_choices->size(); ++c) {
                const Rect cr{ rect.x, rect.y + c * (kChoiceH + kChoiceGap), rect.w, kChoiceH };
                if (cr.Contains(m_mx, m_my)) {
                    return UIAction{ "choice", "", std::to_string(c), node.id };
                }
            }
        }
        if (node.type == NodeType::SaveGrid || node.type == NodeType::GalleryGrid) {
            auto git = m_grids.find(node.bind);
            if (git != m_grids.end()) {
                const auto& items = git->second;
                const std::vector<Rect> cells = GridCells(node, rect, items.size());
                for (std::size_t c = 0; c < items.size(); ++c) {
                    if (!items[c].locked && cells[c].Contains(m_mx, m_my)) {
                        return UIAction{ items[c].actionType, "", items[c].actionArg, node.id };
                    }
                }
            }
        }
    }
    return std::nullopt;
}

void UIStage::RenderNode(graphics::Renderer2D& r, const UINode& node, std::size_t index) {
    if (!node.visible) {
        return;
    }
    Rect rect = ScreenRect(node, r);
    float opacity = node.opacity;
    if (!m_editMode && index < m_anims.size()) {
        ApplyAnim(node, m_anims[index], rect, opacity);
    }
    const auto alpha = static_cast<std::uint8_t>(std::clamp(opacity, 0.0f, 255.0f));
    const bool hovered = rect.Contains(m_mx, m_my);

    auto withAlpha = [&](Color c) {
        c.a = static_cast<std::uint8_t>(c.a * (alpha / 255.0f));
        return c;
    };
    auto drawBox = [&](Color fill) {
        if (node.radius > 0.5f) {
            r.DrawRoundedRect(rect, node.radius, withAlpha(fill));
        } else {
            r.DrawRect(rect, withAlpha(fill));
        }
        if (node.borderTopHeight > 0.0f) {
            r.DrawRect(Rect{ rect.x, rect.y, rect.w, node.borderTopHeight },
                       withAlpha(node.borderColor));
        }
    };
    auto drawCenteredText = [&](const std::string& text, Color col) {
        if (text.empty()) return;
        const Vec2 sz = r.MeasureText(text, node.font, node.fontSize);
        const float tx = node.align == "left" ? rect.x + 16 : rect.x + (rect.w - sz.x) * 0.5f;
        const float ty = rect.y + (rect.h - sz.y) * 0.5f;
        r.DrawTextOutline(text, tx, ty, node.font, node.fontSize, withAlpha(col),
                          Color{ 0, 0, 0, 255 }, node.outlineSize, alpha, node.textShadow);
    };

    switch (node.type) {
        case NodeType::Image: {
            const std::string& img = (hovered && !node.hoverImage.empty()) ? node.hoverImage
                                                                           : node.image;
            if (!img.empty()) r.DrawImage(img, rect, alpha);
            break;
        }
        case NodeType::Text:
            drawCenteredText(node.text, node.textColor);
            break;
        case NodeType::Button: {
            if (!node.image.empty() || !node.hoverImage.empty()) {
                const std::string& img = (hovered && !node.hoverImage.empty()) ? node.hoverImage
                                                                               : node.image;
                if (!img.empty()) r.DrawImage(img, rect, alpha);
            } else {
                drawBox(hovered ? node.hoverColor : node.bgColor);
            }
            drawCenteredText(node.text, node.textColor);
            break;
        }
        case NodeType::DialogueBox: {
            drawBox(node.bgColor);
            if (m_dialogue && !m_dialogue->fullText.empty()) {
                if (!m_dialogue->speaker.empty()) {
                    r.DrawTextOutline(m_dialogue->speaker, rect.x + 28, rect.y - 44, node.font,
                                      node.fontSize + 2, m_dialogue->textColor, Color{ 0, 0, 0, 255 },
                                      2, alpha, true);
                }
                r.DrawTextOutline(m_dialogue->displayText, rect.x + 30, rect.y + 26, node.font,
                                  node.fontSize, node.textColor, Color{ 0, 0, 0, 255 }, 2, alpha,
                                  false, static_cast<int>(rect.w - 60));
            }
            break;
        }
        case NodeType::ChoiceList: {
            if (m_choices) {
                for (std::size_t c = 0; c < m_choices->size(); ++c) {
                    const Rect cr{ rect.x, rect.y + c * (kChoiceH + kChoiceGap), rect.w, kChoiceH };
                    const bool h = cr.Contains(m_mx, m_my);
                    r.DrawRoundedRect(cr, 10.0f, withAlpha(h ? node.hoverColor : node.bgColor));
                    const Vec2 sz = r.MeasureText((*m_choices)[c], node.font, node.fontSize);
                    r.DrawTextOutline((*m_choices)[c], cr.x + (cr.w - sz.x) * 0.5f,
                                      cr.y + (cr.h - sz.y) * 0.5f, node.font, node.fontSize,
                                      node.textColor, Color{ 0, 0, 0, 255 }, 2, alpha);
                }
            }
            break;
        }
        case NodeType::SaveGrid:
        case NodeType::GalleryGrid: {
            auto git = m_grids.find(node.bind);
            const std::size_t count = git != m_grids.end() ? git->second.size() : 0;
            const std::vector<Rect> cells = GridCells(node, rect, count);
            for (std::size_t c = 0; c < count; ++c) {
                const UIStage::GridItem& item = git->second[c];
                const Rect& cr = cells[c];
                const bool h = cr.Contains(m_mx, m_my) && !item.locked;
                if (!item.locked && !item.image.empty()) {
                    r.DrawImage(item.image, cr, alpha);
                } else {
                    r.DrawRoundedRect(cr, 6.0f,
                                      withAlpha(item.locked ? Color{ 18, 20, 28, 220 }
                                                            : (h ? node.hoverColor : node.bgColor)));
                }
                const std::string label = item.locked ? "??? (鎖定)" : item.label;
                if (!label.empty()) {
                    r.DrawTextOutline(label, cr.x + 8, cr.y + cr.h - 26, node.font, 18,
                                      withAlpha(node.textColor), Color{ 0, 0, 0, 255 }, 2, alpha);
                }
                if (h) {
                    r.DrawRect(Rect{ cr.x, cr.y, cr.w, 3 }, withAlpha(Color{ 120, 200, 255, 255 }));
                }
            }
            break;
        }
        case NodeType::Component:
            break;
        default:
            drawBox(hovered ? node.hoverColor : node.bgColor);
            if (!node.text.empty()) drawCenteredText(node.text, node.textColor);
            break;
    }
}

void UIStage::Render(graphics::Renderer2D& r) {
    int w = 0, h = 0;
    r.GetLogicalSize(w, h);
    if (m_scene.bgColor.a > 0) {
        r.DrawRect(Rect{ 0, 0, static_cast<float>(w), static_cast<float>(h) }, m_scene.bgColor);
    }
    if (!m_scene.bgImage.empty()) {
        r.DrawImageAuto(m_scene.bgImage, graphics::DisplayMode::Fill, m_scene.bgAlpha);
    }
    for (std::size_t idx : DrawOrder()) {
        RenderNode(r, m_scene.nodes[idx], idx);
    }
}

}
