#include "Engine/UI/Widgets.h"
#include "Engine/Animation/MotionRenderer.h"
#include "Engine/Text/Typography.h"

#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Platform/Input.h"
#include "Engine/UI/Theme.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_scancode.h>

namespace px::ui {
namespace {
const StyleBox& StateBox(const Control& control, const ControlStyle& style) {
    if (!control.Enabled()) return style.disabled;
    if (control.Pressed()) return style.pressed;
    if (control.Focused()) return style.focused;
    if (control.Hovered()) return style.hover;
    return style.normal;
}

// Resolves inline style runs against the control's font and size. An empty
// base font stays empty (plus a "|b"/"|i" suffix) for the UI context to fill
// with the theme font.
std::vector<text::TextFontRun> FontRuns(const std::vector<text::TextRun>& runs,
                                        const std::string& baseFont, const int baseSize) {
    std::vector<text::TextFontRun> resolved;
    resolved.reserve(runs.size());
    for (const auto& run : runs) {
        const int size = run.style.size > 0 ? run.style.size : baseSize;
        resolved.push_back({run.byteStart, run.byteLength,
                            text::StyledFontPath(run.style.font.empty() ? baseFont : run.style.font,
                                                 run.style.bold, run.style.italic),
                            std::max(1, static_cast<int>(std::lround(
                                static_cast<float>(size) * run.style.scale)))});
    }
    return resolved;
}

// Inline runs over `length` bytes of text, or one run in the control's own
// style when the text has none (a ruby line). The last run is extended over
// text the runs were not parsed for.
std::vector<text::TextRun> RunsOver(const std::vector<text::TextRun>& runs,
                                    const std::size_t length) {
    if (runs.empty()) return {text::TextRun{0, length, {}}};
    auto result = runs;
    auto& last = result.back();
    if (last.byteStart + last.byteLength < length) last.byteLength = length - last.byteStart;
    return result;
}

std::size_t RunAt(const std::vector<text::TextRun>& runs, const std::size_t byte) {
    const auto found = std::upper_bound(runs.begin(), runs.end(), byte,
        [](const std::size_t value, const text::TextRun& run) { return value < run.byteStart; });
    return found == runs.begin() ? 0 : static_cast<std::size_t>(found - runs.begin() - 1);
}

void DrawBox(graphics::Renderer2D& renderer, Rect area, const StyleBox& box) {
    renderer.DrawRoundedRect(area, box.cornerRadius, box.background);
    if (box.borderWidth <= 0.0f) return;
    if (box.cornerRadius > 0.5f) renderer.DrawRoundedBorder(area, box.borderWidth, box.cornerRadius, box.border);
    else renderer.DrawBorder(area,box.borderWidth,box.cornerRadius,box.border);
}

graphics::HorizontalAlignment Horizontal(HorizontalTextAlignment value){switch(value){case HorizontalTextAlignment::Center:return graphics::HorizontalAlignment::Center;case HorizontalTextAlignment::Right:return graphics::HorizontalAlignment::Right;default:return graphics::HorizontalAlignment::Left;}}
graphics::VerticalAlignment Vertical(VerticalTextAlignment value){switch(value){case VerticalTextAlignment::Center:return graphics::VerticalAlignment::Center;case VerticalTextAlignment::Bottom:return graphics::VerticalAlignment::Bottom;default:return graphics::VerticalAlignment::Top;}}
std::size_t PreviousGrapheme(std::string_view value,std::size_t cursor){const auto boundaries=text::GraphemeBoundaries(value);const auto found=std::lower_bound(boundaries.begin(),boundaries.end(),cursor);return found==boundaries.begin()?0:*(found-1);}
std::size_t NextGrapheme(std::string_view value,std::size_t cursor){const auto boundaries=text::GraphemeBoundaries(value);const auto found=std::upper_bound(boundaries.begin(),boundaries.end(),cursor);return found==boundaries.end()?value.size():*found;}
std::size_t SnapGrapheme(std::string_view value, std::size_t cursor) {
    cursor = std::min(cursor, value.size());
    const auto boundaries = text::GraphemeBoundaries(value);
    const auto found = std::lower_bound(boundaries.begin(), boundaries.end(), cursor);
    if (found == boundaries.end()) return value.size();
    if (*found == cursor || found == boundaries.begin()) return *found;
    const auto previous = *std::prev(found);
    return cursor - previous <= *found - cursor ? previous : *found;
}
bool ParseByteRange(std::string_view value, std::size_t& start,
                    std::size_t& end) {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) return false;
    const auto parse = [](std::string_view part, std::size_t& output) {
        if (part.empty()) return false;
        const auto result = std::from_chars(part.data(), part.data() + part.size(),
                                            output);
        return result.ec == std::errc{} && result.ptr == part.data() + part.size();
    };
    return parse(value.substr(0, separator), start) &&
           parse(value.substr(separator + 1), end);
}
bool ParseByteOffset(std::string_view value, std::size_t& output) {
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(),
                                        output);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}
void DrawSelection(graphics::Renderer2D& renderer,
                   const text::TextLayout& layout, const Vec2 origin,
                   const std::size_t start, const std::size_t end) {
    if (start >= end) return;
    for (const auto& cluster : layout.Clusters()) {
        const std::size_t clusterEnd = cluster.byteStart + cluster.byteLength;
        if (clusterEnd <= start || cluster.byteStart >= end) continue;
        renderer.DrawRect({origin.x + cluster.bounds.x,
                           origin.y + cluster.bounds.y,
                           cluster.bounds.w, cluster.bounds.h},
                          {70, 120, 210, 125});
    }
}
graphics::ContentScaleMode ToScaleMode(TextureScaleMode value){switch(value){case TextureScaleMode::Fit:return graphics::ContentScaleMode::Fit;case TextureScaleMode::Fill:return graphics::ContentScaleMode::Fill;case TextureScaleMode::Original:return graphics::ContentScaleMode::Original;default:return graphics::ContentScaleMode::Stretch;}}
}

Vec2 Panel::MeasureOverride(Vec2 available) {
    return Control::MeasureOverride(available);
}

void Panel::ArrangeOverride(Rect finalRect) { Control::ArrangeOverride(finalRect); }

void Panel::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    // An authored box decoration replaces the theme's panel box.
    if (GetBoxDecoration()) return;
    DrawBox(renderer, LayoutRect(), StateBox(*this, theme.Resolve(*this)));
}

Vec2 Label::MeasureOverride(Vec2 available) {
    const int size = m_fontSize > 0 ? m_fontSize : 24;
    const int wrap = m_wrap && available.x > 0.0f
                         ? std::max(1, static_cast<int>(std::floor(available.x)))
                         : 0;
    if (const auto layout = MeasureTextLayout(m_text, m_fontSize, wrap))
        return layout->Size();
    const float estimatedWidth = static_cast<float>(m_text.size()) * static_cast<float>(size) * 0.55f;
    if (!m_wrap || available.x <= 0.0f) return {estimatedWidth, static_cast<float>(size) * 1.35f};
    const float lines = std::max(1.0f, std::ceil(estimatedWidth / available.x));
    return {std::min(estimatedWidth, available.x), lines * static_cast<float>(size) * 1.35f};
}

int Label::FontSizeToFit(std::string_view content, int requested,
                         float width, float height) const {
    if (width <= 0.0f || height <= 0.0f || content.empty()) return requested;
    const auto parsed = text::ParseRubyMarkup(content);
    const std::string& plain = parsed.plain;
    const int wrap = std::max(1, static_cast<int>(std::floor(width)));
    const auto fits = [&](const int size) {
        const auto layout = parsed.runs.empty() && parsed.ruby.empty()
            ? MeasureTextLayout(plain, size, wrap)
            : MeasureTextRuns(plain, FontRuns(RunsOver(parsed.runs, plain.size()), m_fontPath, size),
                              wrap);
        return !layout || layout->Size().y <= height - 8.0f;
    };
    if (fits(requested)) return requested;
    // Wrapped height grows with the size, so the largest fitting size is
    // found by bisection instead of shaping every size down to the minimum.
    int low = 12;
    int high = requested - 1;
    if (!fits(low)) return low;
    while (low < high) {
        const int middle = low + (high - low + 1) / 2;
        if (fits(middle)) low = middle;
        else high = middle - 1;
    }
    return low;
}

std::string Label::ReflowedText(graphics::Renderer2D& renderer, const std::string& font,
                                const int size, const float width) const {
    if (m_text.empty() || width <= 0.0f) return m_text;
    const bool prefix = m_layoutReference.size() > m_text.size() &&
                        m_layoutReference.compare(0, m_text.size(), m_text) == 0;
    const std::string& source = prefix ? m_layoutReference : m_text;
    std::string key = source;
    key += '\x01'; key += font; key += '\x01'; key += std::to_string(size);
    key += '\x01'; key += std::to_string(static_cast<int>(width));
    if (key != m_reflowKey) {
        m_reflowKey = std::move(key);
        m_reflowBreaks = text::WrapBreaks(source, width, [&](const std::string_view line) {
            return renderer.MeasureText(std::string(line), font, size).x;
        });
    }
    if (m_reflowBreaks.empty()) return m_text;
    std::string shown;
    shown.reserve(m_text.size() + m_reflowBreaks.size());
    std::size_t from = 0;
    for (const std::size_t at : m_reflowBreaks) {
        if (at >= m_text.size()) break;
        shown.append(m_text, from, at - from);
        // A break replaces the space it falls on (Latin text).
        while (!shown.empty() && shown.back() == ' ') shown.pop_back();
        shown += '\n';
        from = at;
    }
    shown.append(m_text, from, std::string::npos);
    return shown;
}

void Label::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style = theme.Resolve(*this);
    const int size = m_fontSize > 0 ? m_fontSize : style.fontSize;
    const std::string& font = m_fontPath.empty() ? style.font : m_fontPath;
    const Color color = m_hasColor ? m_color : (Enabled() ? style.text : style.textDisabled);
    if (m_letterSpacing == 0.0f || m_wrap || m_text.find_first_of("\r\n") != std::string::npos) {
        // Reflowed text carries its own line breaks; SDL_ttf must not wrap it
        // again with its slightly different measurements.
        const bool reflow = m_wrap;
        const std::string shown = reflow
            ? ReflowedText(renderer, font, size, LayoutRect().w - 2.0f * static_cast<float>(m_outlineSize))
            : m_text;
        renderer.DrawTextInRect(shown,LayoutRect(),font,size,color,
                                Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),m_wrap,
                                255,m_outlineSize,m_outlineColor,reflow);
        return;
    }
    // Tracked text: lay each UTF-8 character out on its own so the gap between
    // characters can grow. Kerning pairs are not applied across the gap.
    std::vector<std::pair<std::string, float>> characters;
    float width = 0.0f;
    for (std::size_t index = 0; index < m_text.size();) {
        const auto lead = static_cast<unsigned char>(m_text[index]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        std::string character = m_text.substr(index, length);
        index += length;
        // A control character takes no cell and no gap.
        if (text::MakeDrawable(character).empty()) continue;
        const float advance = renderer.MeasureText(character, font, size).x;
        width += advance + (characters.empty() ? 0.0f : m_letterSpacing);
        characters.emplace_back(std::move(character), advance);
    }
    const Rect area = LayoutRect();
    float x = area.x;
    if (m_horizontalAlignment == HorizontalTextAlignment::Center) x += (area.w - width) / 2.0f;
    else if (m_horizontalAlignment == HorizontalTextAlignment::Right) x += area.w - width;
    for (const auto& [character, advance] : characters) {
        renderer.DrawTextInRect(character, {x, area.y, advance + 1.0f, area.h}, font, size, color,
                                graphics::HorizontalAlignment::Left, Vertical(m_verticalAlignment),
                                false, 255, m_outlineSize, m_outlineColor);
        x += advance + m_letterSpacing;
    }
}

Button::Button(std::string text, std::string name) : Control(std::move(name)), m_text(std::move(text)) {
    SetFocusMode(FocusMode::All);
}

Button::~Button() {
    if (m_capturingBinding && m_bindingInput)
        m_bindingInput->CancelKeyCapture();
    if (m_capturingWheelBinding && m_bindingInput)
        m_bindingInput->CancelWheelCapture();
}

void Button::SetBindingAction(std::string action) {
    if (m_capturingBinding && m_bindingInput)
        m_bindingInput->CancelKeyCapture();
    if (m_capturingWheelBinding && m_bindingInput)
        m_bindingInput->CancelWheelCapture();
    m_capturingBinding = false;
    m_capturingWheelBinding = false;
    m_replacingBindingKey.reset();
    m_pendingSharedBinding.reset();
    m_selectedBindingIndex = 0;
    m_bindingFeedbackSeconds = 0.0f;
    m_bindingAction = std::move(action);
    RefreshBindingText();
}

void Button::SetBindingDevice(const BindingDevice device) {
    if (m_capturingBinding && m_bindingInput)
        m_bindingInput->CancelKeyCapture();
    if (m_capturingWheelBinding && m_bindingInput)
        m_bindingInput->CancelWheelCapture();
    m_capturingBinding = false;
    m_capturingWheelBinding = false;
    m_replacingBindingKey.reset();
    m_pendingSharedBinding.reset();
    m_selectedBindingIndex = 0;
    m_bindingDevice = device;
    m_bindingFeedbackSeconds = 0.0f;
    RefreshBindingText();
}

bool Button::SetWheelThreshold(const float threshold) {
    if (!std::isfinite(threshold) || threshold < 0.0f || threshold > 100.0f)
        return false;
    m_wheelThreshold = threshold;
    return true;
}

void Button::SetInputBindingService(Input* input,
                                    std::function<void()> onChanged) {
    if (m_capturingBinding && m_bindingInput != input)
        m_bindingInput->CancelKeyCapture();
    if (m_capturingWheelBinding && m_bindingInput != input)
        m_bindingInput->CancelWheelCapture();
    if (m_bindingInput != input) {
        m_capturingBinding = false;
        m_capturingWheelBinding = false;
        m_replacingBindingKey.reset();
        m_pendingSharedBinding.reset();
    }
    m_bindingInput = input;
    m_bindingChanged = std::move(onChanged);
    RefreshBindingText();
}

void Button::RefreshBindingText() {
    if (m_bindingAction.empty() || !m_bindingInput || m_capturingBinding ||
        m_capturingWheelBinding ||
        m_bindingFeedbackSeconds > 0.0f)
        return;
    if (!m_bindingInput->HasAction(m_bindingAction)) {
        if (m_text != "Unknown action") SetText("Unknown action");
        return;
    }
    if (m_bindingDevice == BindingDevice::Wheel) {
        const auto binding = m_bindingInput->GetWheelBinding(m_bindingAction);
        const std::string& caption = !binding ||
            binding->direction == WheelDirection::None ? m_unboundText :
            binding->direction == WheelDirection::Up ? m_wheelUpText :
            m_wheelDownText;
        if (m_text != caption) SetText(caption);
        return;
    }
    const auto keys = m_bindingInput->KeyBindings(m_bindingAction);
    if (keys.empty()) {
        m_selectedBindingIndex = 0;
        if (m_text != m_unboundText) SetText(m_unboundText);
        return;
    }
    m_selectedBindingIndex = std::min(m_selectedBindingIndex,
                                      keys.size() - 1);
    std::string caption;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (!caption.empty()) caption += " / ";
        if (m_selectableBindingKeys && index == m_selectedBindingIndex)
            caption += '[';
        caption += Input::KeyName(keys[index]);
        if (m_selectableBindingKeys && index == m_selectedBindingIndex)
            caption += ']';
    }
    if (caption != m_text) SetText(std::move(caption));
}

void Button::Activate() {
    if (!Enabled()) return;
    EmitSignal("activated");
    if (!m_bindingAction.empty()) {
        if (m_bindingDevice == BindingDevice::Wheel) {
            if (!m_bindingInput || !m_bindingInput->HasAction(m_bindingAction)) {
                SetText("Unknown action");
                m_bindingFeedbackSeconds = 1.5f;
            } else if (m_capturingWheelBinding) {
                m_bindingInput->CancelWheelCapture();
                m_capturingWheelBinding = false;
                RefreshBindingText();
            } else if (m_bindingInput->BeginWheelCapture()) {
                m_capturingWheelBinding = true;
                SetText(m_bindingPrompt);
            }
            return;
        }
        if (!m_bindingInput || !m_bindingInput->HasAction(m_bindingAction)) {
            SetText("Unknown action");
            m_bindingFeedbackSeconds = 1.5f;
        } else if (m_pendingSharedBinding) {
            const auto pending = *m_pendingSharedBinding;
            m_pendingSharedBinding.reset();
            m_bindingFeedbackSeconds = 0.0f;
            ApplyBindingKey(pending.key, pending.replacingKey, pending.add,
                            true);
        } else if (m_capturingBinding) {
            m_bindingInput->CancelKeyCapture();
            m_capturingBinding = false;
            m_replacingBindingKey.reset();
            m_bindingFeedbackSeconds = 0.0f;
            RefreshBindingText();
        } else if (m_bindingInput->BeginKeyCapture()) {
            m_capturingBinding = true;
            m_replacingBindingKey.reset();
            if (m_selectableBindingKeys && !m_addCapturedBinding) {
                const auto keys = m_bindingInput->KeyBindings(m_bindingAction);
                if (m_selectedBindingIndex < keys.size())
                    m_replacingBindingKey = keys[m_selectedBindingIndex];
            }
            m_bindingFeedbackSeconds = 0.0f;
            SetText(m_bindingPrompt);
        }
        return;
    }
    if (m_activated) m_activated();
}

void Button::HandleEvent(UIEvent& event) {
    if (m_capturingWheelBinding && event.type == UIEventType::KeyDown &&
        ((event.key == SDL_SCANCODE_ESCAPE && m_escapeCancelsBinding) ||
         (event.key == SDL_SCANCODE_DELETE && m_deleteClearsBinding))) {
        m_bindingInput->CancelWheelCapture();
        m_capturingWheelBinding = false;
        if (event.key == SDL_SCANCODE_DELETE &&
            m_bindingInput->SetWheelBinding(m_bindingAction,
                                             WheelDirection::None)) {
            if (m_bindingChanged) m_bindingChanged();
        }
        RefreshBindingText();
        event.handled = true;
        return;
    }
    if (m_bindingDevice == BindingDevice::Keyboard &&
        m_selectableBindingKeys && !m_bindingAction.empty() &&
        m_bindingInput && !m_capturingBinding &&
        (event.type == UIEventType::KeyDown ||
         event.type == UIEventType::Scroll)) {
        const auto keys = m_bindingInput->KeyBindings(m_bindingAction);
        if (event.type == UIEventType::KeyDown &&
            event.key == SDL_SCANCODE_DELETE) {
            m_pendingSharedBinding.reset();
            m_bindingFeedbackSeconds = 0.0f;
            if (m_selectedBindingIndex < keys.size() &&
                m_bindingInput->RemoveKeyBinding(
                    m_bindingAction, keys[m_selectedBindingIndex])) {
                if (m_bindingChanged) m_bindingChanged();
                RefreshBindingText();
            }
            event.handled = true;
            return;
        }
        const bool previous =
            (event.type == UIEventType::KeyDown &&
             event.key == SDL_SCANCODE_PAGEUP) ||
            (event.type == UIEventType::Scroll && event.wheel > 0.0f);
        const bool next =
            (event.type == UIEventType::KeyDown &&
             event.key == SDL_SCANCODE_PAGEDOWN) ||
            (event.type == UIEventType::Scroll && event.wheel < 0.0f);
        if ((previous || next) && !keys.empty()) {
            m_pendingSharedBinding.reset();
            m_bindingFeedbackSeconds = 0.0f;
            m_selectedBindingIndex = previous
                ? (m_selectedBindingIndex + keys.size() - 1) % keys.size()
                : (m_selectedBindingIndex + 1) % keys.size();
            RefreshBindingText();
            event.handled = true;
            return;
        }
    }
    if (event.type == UIEventType::Click || event.type == UIEventType::Activate) {
        Activate(); event.handled = true;
    }
}

void Button::Update(float deltaSeconds) {
    Control::Update(deltaSeconds);
    if (m_bindingAction.empty() || !m_bindingInput) return;
    if (m_bindingDevice == BindingDevice::Wheel) {
        if (m_capturingWheelBinding) {
            if (const auto direction = m_bindingInput->TakeCapturedWheel()) {
                m_capturingWheelBinding = false;
                if (m_bindingInput->SetWheelBinding(m_bindingAction,
                                                    *direction, m_wheelThreshold)) {
                    if (m_bindingChanged) m_bindingChanged();
                    RefreshBindingText();
                } else {
                    SetText("Invalid wheel binding");
                    m_bindingFeedbackSeconds = 1.5f;
                }
            } else if (!m_bindingInput->WheelCaptureActive()) {
                m_capturingWheelBinding = false;
                RefreshBindingText();
            }
            return;
        }
        if (m_bindingFeedbackSeconds > 0.0f)
            m_bindingFeedbackSeconds = std::max(0.0f,
                m_bindingFeedbackSeconds - std::max(0.0f, deltaSeconds));
        RefreshBindingText();
        return;
    }
    if (m_capturingBinding) {
        if (const auto key = m_bindingInput->TakeCapturedKey()) {
            m_capturingBinding = false;
            const auto replacingKey = m_replacingBindingKey;
            m_replacingBindingKey.reset();
            if (*key == SDL_SCANCODE_ESCAPE && m_escapeCancelsBinding) {
                RefreshBindingText();
            } else if (*key == SDL_SCANCODE_DELETE && m_deleteClearsBinding) {
                const bool removed = replacingKey
                    ? m_bindingInput->RemoveKeyBinding(m_bindingAction,
                                                       *replacingKey)
                    : m_bindingInput->ClearKeyboardBindings(m_bindingAction);
                if (removed) {
                    if (m_bindingChanged) m_bindingChanged();
                    RefreshBindingText();
                }
            } else {
                ApplyBindingKey(*key, replacingKey, m_addCapturedBinding,
                                m_allowSharedBinding);
            }
        } else if (!m_bindingInput->KeyCaptureActive()) {
            // A host can cancel a pending capture when changing input modes.
            m_capturingBinding = false;
            m_replacingBindingKey.reset();
            RefreshBindingText();
        }
        return;
    }
    if (m_bindingFeedbackSeconds > 0.0f) {
        m_bindingFeedbackSeconds = std::max(0.0f,
            m_bindingFeedbackSeconds - std::max(0.0f, deltaSeconds));
        if (m_bindingFeedbackSeconds == 0.0f)
            m_pendingSharedBinding.reset();
    }
    RefreshBindingText();
}

void Button::ApplyBindingKey(const int key,
                             const std::optional<int> replacingKey,
                             const bool add, const bool allowConflict) {
    const auto result = add
        ? m_bindingInput->AddKeyBinding(m_bindingAction, key, allowConflict)
        : replacingKey
        ? m_bindingInput->ReplaceKeyBinding(m_bindingAction, *replacingKey,
                                            key, allowConflict)
        : m_bindingInput->RebindKey(m_bindingAction, key, allowConflict);
    if (result.status == KeyRebindStatus::Applied) {
        if (m_selectableBindingKeys && add) {
            const auto keys = m_bindingInput->KeyBindings(m_bindingAction);
            const auto selected = std::ranges::find(keys, key);
            if (selected != keys.end())
                m_selectedBindingIndex = static_cast<std::size_t>(
                    std::distance(keys.begin(), selected));
        }
        if (m_bindingChanged) m_bindingChanged();
        RefreshBindingText();
        return;
    }
    if (result.status == KeyRebindStatus::Conflict &&
        !result.conflicts.empty() && m_confirmSharedBinding &&
        !allowConflict) {
        m_pendingSharedBinding = PendingSharedBinding{key, replacingKey, add};
        SetText(m_bindingConfirmText);
        m_bindingFeedbackSeconds = 4.0f;
        return;
    }
    SetText(result.status == KeyRebindStatus::Conflict
                ? m_bindingConflictText
                : result.status == KeyRebindStatus::LimitReached
                    ? m_bindingLimitText : "Invalid key");
    m_bindingFeedbackSeconds = 1.5f;
}

Vec2 Button::MeasureOverride(Vec2) {
    if (const auto layout = MeasureTextLayout(m_text, m_fontSize))
        return {std::max(88.0f, layout->Size().x + 28.0f),
                std::max(48.0f, layout->Size().y + 16.0f)};
    const float size = m_fontSize > 0 ? static_cast<float>(m_fontSize) : 24.0f;
    return {std::max(88.0f, static_cast<float>(m_text.size()) * size * 0.58f + 28.0f), 48.0f};
}

void Button::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    // An authored box decoration (drawn by Control::Draw) replaces the theme box.
    if (!GetBoxDecoration()) {
        const auto& style = theme.Resolve(*this);
        DrawBox(renderer, LayoutRect(), StateBox(*this, style));
    }
    DrawTextContent(renderer, theme);
}

void Button::DrawTextContent(graphics::Renderer2D& renderer,
                             const Theme& theme) const {
    if (m_text.empty()) return;
    const auto& style = theme.Resolve(*this);
    const int fontSize = m_fontSize > 0 ? m_fontSize : style.fontSize;
    // Narrow buttons (toolbar captions) keep less padding so text is not clipped.
    const float inset=std::min(12.0f,LayoutRect().w*0.08f);
    Rect textArea=LayoutRect();textArea.x+=inset;textArea.w=std::max(0.0f,textArea.w-inset*2);
    Color color=Enabled()?(m_hasTextColor?m_textColor:style.text):style.textDisabled;
    if(Hovered()&&Enabled()&&GetBoxDecoration()&&GetBoxDecoration()->hoverText)color=*GetBoxDecoration()->hoverText;
    renderer.DrawTextInRect(m_text,textArea,m_fontPath.empty()?style.font:m_fontPath,fontSize,color,
                            Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),false);
}

Vec2 IconButton::MeasureOverride(Vec2) { return {44.0f, 44.0f}; }
void IconButton::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style=theme.Resolve(*this);
    Color color=Enabled()?style.text:style.textDisabled;
    if(Hovered()&&Enabled())color=style.focused.border;
    renderer.DrawTextInRect(Text(),LayoutRect(),style.font,style.fontSize,color,
                            graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,false);
}

void TextureButton::DrawSelf(graphics::Renderer2D& renderer,
                             const Theme& theme) {
    const std::string* path = &m_normalPath;
    if (!Enabled() && !m_disabledPath.empty()) path = &m_disabledPath;
    else if (Pressed() && !m_pressedPath.empty()) path = &m_pressedPath;
    else if ((Hovered() || FocusVisible()) && !m_hoverPath.empty()) path = &m_hoverPath;
    if (!path->empty()) {
        renderer.DrawImageInRect(*path, LayoutRect(), ToScaleMode(m_scaleMode),
                                 graphics::HorizontalAlignment::Center,
                                 graphics::VerticalAlignment::Center,
                                 Enabled() ? 255 : 128);
        DrawTextContent(renderer, theme);
        return;
    }
    Button::DrawSelf(renderer, theme);
}

void TextureButton::CollectImageDependencies(std::vector<std::string>& paths) const {
    for (const auto* path : {&m_normalPath, &m_hoverPath, &m_pressedPath,
                             &m_disabledPath})
        if (!path->empty()) paths.push_back(*path);
}

void TextureRect::SetOpacity(float value) { m_opacity = std::clamp(value, 0.0f, 1.0f); }
Status TextureRect::SetPath(std::string value) {
    std::optional<sdk::LayeredPortrait> portrait;
    if (value.ends_with(".pxportrait") && m_loader) {
        const auto text = m_loader(value);
        if (!text) {
            diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                .code = "PXUIPORTRAIT1001", .category = "UI.Portrait",
                .message = "Portrait descriptor is missing: " + value};
            diagnostic.source.path = value;
            return Status::Fail(std::move(diagnostic));
        }
        portrait = sdk::ParseLayeredPortrait(*text);
        if (!portrait) {
            diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                .code = "PXUIPORTRAIT1002", .category = "UI.Portrait",
                .message = "Portrait descriptor is invalid: " + value};
            diagnostic.source.path = value;
            return Status::Fail(std::move(diagnostic));
        }
    }
    m_path = std::move(value);
    m_portrait = std::move(portrait);
    if (m_portrait) m_displayPortraitPath.clear();
    if (m_path.empty()) {
        m_displayPath.clear();
        m_displayPortrait.reset();
        m_displayPortraitPath.clear();
    }
    return Status::Ok();
}
Status TextureRect::SetTexture(ResourceRefValue value) {
    if (const auto status = SetPath(value.lastKnownPath); !status) return status;
    m_texture = std::move(value);
    return Status::Ok();
}
Status TextureRect::SetSourceLoader(SourceLoader loader) {
    if (!loader && m_path.ends_with(".pxportrait")) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
            .code = "PXUIPORTRAIT1003", .category = "UI.Portrait",
            .message = "TextureRect requires a portrait source loader"};
        diagnostic.source.path = m_path;
        return Status::Fail(std::move(diagnostic));
    }
    auto previous = std::move(m_loader);
    m_loader = std::move(loader);
    const auto status = SetPath(m_path);
    if (!status) m_loader = std::move(previous);
    return status;
}
void TextureRect::CollectImageDependencies(std::vector<std::string>& paths) const {
    if (m_portrait) {
        for (const auto& layer : m_portrait->layers) paths.push_back(layer.path);
    } else if (!m_path.empty() && !m_path.ends_with(".pxportrait")) {
        paths.push_back(m_path);
    }
}
void TextureRect::DrawSelf(graphics::Renderer2D& renderer, const Theme&) {
    if (m_portrait) {
        bool ready = true;
        for (const auto& layer : m_portrait->layers) {
            renderer.PreloadImage(layer.path);
            ready = renderer.ImageReady(layer.path) && ready;
        }
        if (ready && m_displayPortraitPath != m_path) {
            m_displayPortrait = *m_portrait;
            m_displayPortraitPath = m_path;
            m_displayPath.clear();
        }
    } else if (!m_path.empty() && !m_path.ends_with(".pxportrait")) {
        renderer.PreloadImage(m_path);
        if (renderer.ImageReady(m_path)) {
            m_displayPath = m_path;
            m_displayPortrait.reset();
            m_displayPortraitPath.clear();
        }
    }
    if (m_displayPortrait) {
        const auto& portrait = *m_displayPortrait;
        const Rect bounds = LayoutRect();
        if (bounds.w <= 0 || bounds.h <= 0) return;
        float scaleX = bounds.w / portrait.width;
        float scaleY = bounds.h / portrait.height;
        if (m_scaleMode == TextureScaleMode::Fit) scaleX = scaleY = std::min(scaleX, scaleY);
        else if (m_scaleMode == TextureScaleMode::Fill) scaleX = scaleY = std::max(scaleX, scaleY);
        else if (m_scaleMode == TextureScaleMode::Original) scaleX = scaleY = 1.0f;
        const float width = portrait.width * scaleX;
        const float height = portrait.height * scaleY;
        float x = bounds.x, y = bounds.y;
        if (m_horizontalAlignment == HorizontalTextAlignment::Center) x += (bounds.w - width) * 0.5f;
        else if (m_horizontalAlignment == HorizontalTextAlignment::Right) x += bounds.w - width;
        if (m_verticalAlignment == VerticalTextAlignment::Center) y += (bounds.h - height) * 0.5f;
        else if (m_verticalAlignment == VerticalTextAlignment::Bottom) y += bounds.h - height;
        renderer.PushClip(bounds);
        for (const auto& layer : portrait.layers) {
            renderer.DrawImage(layer.path,
                {x + layer.x * scaleX, y + layer.y * scaleY,
                 layer.width * scaleX, layer.height * scaleY},
                static_cast<std::uint8_t>(m_opacity * layer.opacity));
        }
        renderer.PopClip();
    } else if (!m_displayPath.empty() && renderer.ImageReady(m_displayPath)) {
        if (m_region.w > 0 && m_region.h > 0) {
            // An atlas cell is placed like a whole image of the cell's size.
            const Rect bounds = LayoutRect();
            float scaleX = bounds.w / m_region.w;
            float scaleY = bounds.h / m_region.h;
            if (m_scaleMode == TextureScaleMode::Fit) scaleX = scaleY = std::min(scaleX, scaleY);
            else if (m_scaleMode == TextureScaleMode::Fill) scaleX = scaleY = std::max(scaleX, scaleY);
            else if (m_scaleMode == TextureScaleMode::Original) scaleX = scaleY = 1.0f;
            const float width = m_region.w * scaleX;
            const float height = m_region.h * scaleY;
            float x = bounds.x, y = bounds.y;
            if (m_scaleMode != TextureScaleMode::Stretch) {
                if (m_horizontalAlignment == HorizontalTextAlignment::Center) x += (bounds.w - width) * 0.5f;
                else if (m_horizontalAlignment == HorizontalTextAlignment::Right) x += bounds.w - width;
                if (m_verticalAlignment == VerticalTextAlignment::Center) y += (bounds.h - height) * 0.5f;
                else if (m_verticalAlignment == VerticalTextAlignment::Bottom) y += bounds.h - height;
            }
            renderer.PushClip(bounds);
            renderer.DrawImageRegion(m_displayPath, m_region, {x, y, width, height},
                                     static_cast<std::uint8_t>(m_opacity * 255.0f));
            renderer.PopClip();
            return;
        }
        renderer.DrawImageInRect(m_displayPath,LayoutRect(),ToScaleMode(m_scaleMode),
            Horizontal(m_horizontalAlignment),Vertical(m_verticalAlignment),static_cast<std::uint8_t>(m_opacity*255.0f));
    }
}

Status MotionRect::SetPath(std::string value) {
    return LoadPath(std::move(value), false);
}

Status MotionRect::LoadPath(std::string value, const bool forceReload) {
    if (!forceReload && value == m_path && m_clip) return Status::Ok();
    if (value.empty()) {
        m_path.clear();
        m_sampler.reset();
        m_clip.reset();
        m_position = 0;
        m_playbackTime = 0;
        m_startMarkersPending = true;
        return Status::Ok();
    }
    if (!m_loader) {
        m_path = std::move(value);
        return Status::Ok();
    }
    const auto text = m_loader(value);
    if (!text) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
            .code = "PXUIMOTION1001", .category = "UI.Motion",
            .message = "Motion descriptor is missing: " + value};
        diagnostic.source.path = value;
        return Status::Fail(std::move(diagnostic));
    }
    auto parsed = sdk::ParseMotion(*text, value);
    if (!parsed.Valid()) {
        std::vector<diag::Diagnostic> diagnostics;
        for (const auto& issue : parsed.diagnostics) {
            diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
                .code = issue.code, .category = "UI.Motion", .message = issue.message};
            diagnostic.source.path = issue.path;
            diagnostics.push_back(std::move(diagnostic));
        }
        return Status::Fail(std::move(diagnostics));
    }
    auto clip = std::make_unique<sdk::MotionClip>(std::move(*parsed.clip));
    auto sampler = std::make_unique<sdk::MotionSampler>(*clip);
    sampler->Seek(m_position);
    m_path = std::move(value);
    m_sampler = std::move(sampler);
    m_clip = std::move(clip);
    m_position = m_sampler->Position();
    m_playbackTime = m_position;
    m_startMarkersPending = m_position == 0;
    return Status::Ok();
}

Status MotionRect::SetSourceLoader(SourceLoader loader) {
    if (!loader && !m_path.empty()) {
        diag::Diagnostic diagnostic{.severity = diag::Severity::Error,
            .code = "PXUIMOTION1002", .category = "UI.Motion",
            .message = "MotionRect requires a motion source loader"};
        diagnostic.source.path = m_path;
        return Status::Fail(std::move(diagnostic));
    }
    auto previous = std::move(m_loader);
    m_loader = std::move(loader);
    const auto status = LoadPath(m_path, true);
    if (!status) m_loader = std::move(previous);
    return status;
}

Status MotionRect::SetSpeed(const double value) {
    if (!std::isfinite(value) || value <= 0 || value > 16) {
        return Status::Fail({.severity = diag::Severity::Error,
            .code = "PXUIMOTION1003", .category = "UI.Motion",
            .message = "Motion playback speed must be within (0, 16]"});
    }
    m_speed = value;
    return Status::Ok();
}

void MotionRect::Seek(const double seconds) {
    const double finite = std::isfinite(seconds) ? std::max(0.0, seconds) : 0.0;
    m_playbackTime = m_clip && m_clip->loop
        ? std::fmod(finite, m_clip->duration) : finite;
    m_position = m_playbackTime;
    m_startMarkersPending = m_playbackTime == 0;
    if (m_sampler) {
        m_sampler->Seek(m_position);
        m_position = m_sampler->Position();
    }
}

void MotionRect::EmitMarkers(const double from, const double to,
                             const bool includeStart) {
    if (!m_clip || !m_markerSink || m_clip->markers.empty() || to < from) return;
    const double duration = m_clip->duration;
    const auto lastCycle = m_clip->loop
        ? static_cast<std::uint64_t>(std::floor(to / duration)) : 0;
    const auto startCycle = m_clip->loop
        ? static_cast<std::uint64_t>(std::floor(from / duration)) : 0;
    // A resumed window can skip many loop iterations. Bound audible events
    // instead of unleashing minutes of stale effects in one frame.
    const auto firstCycle = lastCycle > startCycle + 64 ? lastCycle - 64 : startCycle;
    std::size_t emitted = 0;
    for (auto cycle = firstCycle; cycle <= lastCycle; ++cycle) {
        const double base = static_cast<double>(cycle) * duration;
        for (const auto& marker : m_clip->markers) {
            const double at = base + marker.time;
            if ((at > from || (includeStart && at == from)) && at <= to) {
                m_markerSink(marker);
                if (++emitted >= 256) return;
            }
        }
    }
}

void MotionRect::Update(const float deltaSeconds) {
    Control::Update(deltaSeconds);
    if (!m_playing || !m_clip || !std::isfinite(deltaSeconds) || deltaSeconds <= 0) return;
    const double from = m_playbackTime;
    const double next = from + static_cast<double>(deltaSeconds) * m_speed;
    if (!std::isfinite(next)) return;
    if (m_clip->loop && next / m_clip->duration > 1'000'000) {
        Seek(next);
        m_startMarkersPending = false;
        return;
    }
    const bool complete = !m_clip->loop && next >= m_clip->duration;
    const double endpoint = complete ? m_clip->duration : next;
    EmitMarkers(from, endpoint, m_startMarkersPending);
    Seek(endpoint);
    m_startMarkersPending = false;
    if (complete) {
        m_playing = false;
        EmitSignal("playbackFinished");
    }
}

void MotionRect::DrawSelf(graphics::Renderer2D& renderer, const Theme&) {
    if (m_clip && m_sampler)
        animation::DrawMotionPose(renderer, *m_clip, *m_sampler, LayoutRect());
}

void MotionRect::CollectImageDependencies(std::vector<std::string>& paths) const {
    if (!m_clip) return;
    for (const auto& layer : m_clip->layers)
        if (!layer.image.empty()) paths.push_back(layer.image);
}

void SpriteText::CollectImageDependencies(std::vector<std::string>& paths) const {
    for (const auto& glyph : m_glyphs) paths.push_back(glyph.path);
}

void SpriteText::DrawSelf(graphics::Renderer2D& renderer, const Theme&) {
    if (m_text.empty() || m_glyphs.empty()) return;
    const auto boundaries = text::GraphemeBoundaries(m_text);
    const Rect bounds = LayoutRect();
    float cursor = 0;
    for (std::size_t index = 1; index < boundaries.size() && index <= 64; ++index) {
        const std::string_view character(m_text.data() + boundaries[index - 1],
                                         boundaries[index] - boundaries[index - 1]);
        const auto found = std::ranges::find_if(m_glyphs, [&](const SpriteGlyph& glyph) {
            return glyph.character == character;
        });
        if (found == m_glyphs.end()) {
            cursor += m_advance;
            continue;
        }
        renderer.PreloadImage(found->path);
        if (renderer.ImageReady(found->path)) {
            renderer.DrawImageInRect(found->path,
                {bounds.x + cursor + found->x, bounds.y + found->y,
                 found->width, found->height},
                graphics::ContentScaleMode::Stretch);
        }
        cursor += m_advance > 0 ? m_advance : found->width;
    }
}

void ProgressBar::SetRange(double minimum, double maximum) {
    m_minimum = minimum; m_maximum = std::max(minimum, maximum); SetValue(m_value);
}
void ProgressBar::SetValue(double value) { m_value = std::clamp(value, m_minimum, m_maximum); }
Vec2 ProgressBar::MeasureOverride(Vec2) { return {160.0f, 18.0f}; }
void ProgressBar::DrawSelf(graphics::Renderer2D& renderer, const Theme& theme) {
    const auto& style = theme.Resolve(*this);
    renderer.DrawRoundedRect(LayoutRect(), LayoutRect().h * 0.5f, style.normal.background);
    const double range = m_maximum - m_minimum;
    const float ratio = range > 0.0 ? static_cast<float>((m_value - m_minimum) / range) : 0.0f;
    Rect fill = LayoutRect(); fill.w *= ratio;
    renderer.DrawRoundedRect(fill, LayoutRect().h * 0.5f, style.focused.border);
}

void ColorRect::DrawSelf(graphics::Renderer2D& renderer,const Theme&){if(GetBoxDecoration())return;renderer.DrawRect(LayoutRect(),m_color);}

void CheckBox::HandleEvent(UIEvent& event){
    if(event.type==UIEventType::Click||event.type==UIEventType::Activate){m_checked=!m_checked;SetVisualChecked(m_checked);EmitSignal("toggled",{{"value",m_checked}});if(m_toggled)m_toggled(m_checked);Activate();event.handled=true;return;}
    Button::HandleEvent(event);
}
void CheckBox::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Button::DrawSelf(renderer,theme);const Rect r=LayoutRect();const float size=std::min(22.0f,r.h-10);renderer.DrawRoundedRect({r.x+8,r.y+(r.h-size)*.5f,size,size},3,{20,23,30,255});if(m_checked)renderer.DrawRoundedRect({r.x+12,r.y+(r.h-size)*.5f+4,size-8,size-8},2,theme.Resolve(*this).focused.border);}

void Slider::SetRange(double minimum,double maximum,double step){m_min=minimum;m_max=std::max(minimum,maximum);m_step=std::max(0.0,step);SetValue(m_value);}
void Slider::SetValueSilently(double value){value=std::clamp(value,m_min,m_max);if(m_step>0)value=m_min+std::round((value-m_min)/m_step)*m_step;m_value=value;}
void Slider::SetValue(double value){const double previous=m_value;SetValueSilently(value);if(m_value==previous)return;EmitSignal("valueChanged",{{"value",m_value}});if(m_changed)m_changed(m_value);}
void Slider::SetFromPosition(float x){const Rect r=LayoutRect();const float knob=std::min(std::max(1.0f,m_knobSize),std::max(r.w,r.h));const float travel=r.w-knob;const double ratio=travel>0?std::clamp((x-r.x-knob*.5f)/travel,0.0f,1.0f):0;SetValue(m_min+(m_max-m_min)*ratio);}
void Slider::HandleEvent(UIEvent& event){if(event.type==UIEventType::PointerDown||(event.type==UIEventType::PointerMove&&Pressed())){SetFromPosition(event.position.x);event.handled=true;}else if(event.type==UIEventType::KeyDown){if(event.key==SDL_SCANCODE_LEFT)SetValue(m_value-(m_step>0?m_step:(m_max-m_min)/100));else if(event.key==SDL_SCANCODE_RIGHT)SetValue(m_value+(m_step>0?m_step:(m_max-m_min)/100));else return;event.handled=true;}}
bool Slider::PerformAccessibilityAction(const std::string_view action,
                                        const std::string_view value) {
    if (!Enabled() || !IsVisibleInTree()) return false;
    const double step = m_step > 0.0 ? m_step : (m_max - m_min) / 100.0;
    if (action == "increment") SetValue(m_value + step);
    else if (action == "decrement") SetValue(m_value - step);
    else if (action == "setValue") {
        try {
            std::size_t consumed = 0;
            const double parsed = std::stod(std::string(value), &consumed);
            if (consumed != value.size() || !std::isfinite(parsed)) return false;
            SetValue(parsed);
        } catch (...) { return false; }
    } else return Control::PerformAccessibilityAction(action, value);
    return true;
}
Vec2 Slider::MeasureOverride(Vec2){return{180,26};}
void Slider::CollectImageDependencies(std::vector<std::string>& paths) const {
    for (const auto* path : {&m_railPath, &m_railHoverPath,
                             &m_railPressedPath, &m_knobPath, &m_knobHoverPath,
                             &m_knobPressedPath})
        if (!path->empty()) paths.push_back(*path);
}

void Slider::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){
    const auto& style=theme.Resolve(*this);const Rect r=LayoutRect();
    const float ratio=m_max>m_min?static_cast<float>((m_value-m_min)/(m_max-m_min)):0;
    const float railHeight=std::min(r.h,m_railHeight);
    const Rect rail{r.x,r.y+(r.h-railHeight)*.5f,r.w,railHeight};
    const auto drawRail = [&](const std::string& path) {
        renderer.DrawImageInRect(path,rail,graphics::ContentScaleMode::Stretch,
            graphics::HorizontalAlignment::Center,
            graphics::VerticalAlignment::Center,255);
    };
    if(m_fillRailToValue) {
        if(!m_railPath.empty()) drawRail(m_railPath);
        const std::string* fillPath=&m_railPressedPath;
        if(!Pressed()&&(Hovered()||FocusVisible())&&!m_railHoverPath.empty())
            fillPath=&m_railHoverPath;
        if(fillPath->empty()) fillPath=&m_railPath;
        if(!fillPath->empty()&&ratio>0) {
            renderer.PushClip({rail.x,rail.y,rail.w*ratio,rail.h});
            drawRail(*fillPath);
            renderer.PopClip();
        }
    } else if(!m_railPath.empty()||!m_railHoverPath.empty()||!m_railPressedPath.empty()) {
        const std::string* railPath=&m_railPath;
        if(Pressed()&&!m_railPressedPath.empty())railPath=&m_railPressedPath;
        else if((Hovered()||FocusVisible())&&!m_railHoverPath.empty())railPath=&m_railHoverPath;
        if(!railPath->empty()) drawRail(*railPath);
    } else {
        renderer.DrawRoundedRect(rail,railHeight*.5f,style.normal.background);
        Rect fill=rail;fill.w*=ratio;
        renderer.DrawRoundedRect(fill,railHeight*.5f,style.focused.border);
    }
    const float knobSize=std::min(std::max(1.0f,m_knobSize),std::max(r.w,r.h));
    const Rect knob{r.x+(r.w-knobSize)*ratio,r.y+(r.h-knobSize)*.5f,
                    knobSize,knobSize};
    const std::string* knobPath=&m_knobPath;
    if(Pressed()&&!m_knobPressedPath.empty())knobPath=&m_knobPressedPath;
    else if((Hovered()||FocusVisible())&&!m_knobHoverPath.empty())knobPath=&m_knobHoverPath;
    if(!knobPath->empty())
        renderer.DrawImageInRect(*knobPath,knob,graphics::ContentScaleMode::Stretch,
            graphics::HorizontalAlignment::Center,
            graphics::VerticalAlignment::Center,255);
    else renderer.DrawRoundedRect(knob,knobSize*.5f,style.text);
}

LineEdit::LineEdit(std::string text,std::string name)
    : Control(std::move(name)), m_text(std::move(text)),
      m_cursor(m_text.size()), m_selectionAnchor(m_cursor) {
    SetFocusMode(FocusMode::All);
}
void LineEdit::SetSelectionBytes(const std::size_t start,
                                 const std::size_t end) {
    m_selectionAnchor = SnapGrapheme(m_text, start);
    m_cursor = SnapGrapheme(m_text, end);
}
void LineEdit::MoveCaret(const std::size_t value, const bool extend) {
    const std::size_t next = SnapGrapheme(m_text, value);
    if (!extend) m_selectionAnchor = next;
    m_cursor = next;
}
bool LineEdit::DeleteSelection() {
    const std::size_t start = SelectionStartByteOffset();
    const std::size_t end = SelectionEndByteOffset();
    if (start == end) return false;
    m_text.erase(start, end - start);
    m_cursor = m_selectionAnchor = start;
    return true;
}
void LineEdit::SetCaretFromPoint(const Vec2 point, const bool extend) {
    const auto layout = MeasureTextLayout(m_text, 0);
    const std::size_t target = layout
        ? layout->ByteOffsetAt({point.x - LayoutRect().x - 10.0f,
                                point.y - LayoutRect().y - 8.0f})
        : m_text.size();
    MoveCaret(target, extend);
}
void LineEdit::HandleEvent(UIEvent& event) {
    if (event.type == UIEventType::PointerDown) {
        SetCaretFromPoint(event.position, event.shift);
        m_pointerSelecting = true;
        event.handled = true;
        return;
    }
    if (event.type == UIEventType::PointerMove && m_pointerSelecting) {
        SetCaretFromPoint(event.position, true);
        event.handled = true;
        return;
    }
    if (event.type == UIEventType::PointerUp ||
        event.type == UIEventType::FocusLost)
        m_pointerSelecting = false;
    if (event.type == UIEventType::TextInput) {
        (void)DeleteSelection();
        m_text.insert(m_cursor, event.text);
        m_cursor += event.text.size();
        m_selectionAnchor = m_cursor;
        InvalidateLayout();
        EmitSignal("textChanged", {{"text", m_text}});
        event.handled = true;
        return;
    }
    if (event.type == UIEventType::Activate) {
        EmitSignal("submitted", {{"text", m_text}});
        if (m_submitted) m_submitted(m_text);
        event.handled = true;
        return;
    }
    if (event.type != UIEventType::KeyDown) return;
    bool changed = false;
    if (event.control && event.key == SDL_SCANCODE_A) {
        SetSelectionBytes(0, m_text.size());
    } else if (event.key == SDL_SCANCODE_BACKSPACE) {
        changed = DeleteSelection();
        if (!changed && m_cursor > 0) {
            const auto previous = PreviousGrapheme(m_text, m_cursor);
            m_text.erase(previous, m_cursor - previous);
            m_cursor = m_selectionAnchor = previous;
            changed = true;
        }
    } else if (event.key == SDL_SCANCODE_DELETE) {
        changed = DeleteSelection();
        if (!changed && m_cursor < m_text.size()) {
            const auto next = NextGrapheme(m_text, m_cursor);
            m_text.erase(m_cursor, next - m_cursor);
            m_selectionAnchor = m_cursor;
            changed = true;
        }
    } else if (event.key == SDL_SCANCODE_LEFT) {
        if (!event.shift && SelectionStartByteOffset() != SelectionEndByteOffset())
            MoveCaret(SelectionStartByteOffset(), false);
        else
            MoveCaret(PreviousGrapheme(m_text, m_cursor), event.shift);
    } else if (event.key == SDL_SCANCODE_RIGHT) {
        if (!event.shift && SelectionStartByteOffset() != SelectionEndByteOffset())
            MoveCaret(SelectionEndByteOffset(), false);
        else
            MoveCaret(NextGrapheme(m_text, m_cursor), event.shift);
    } else if (event.key == SDL_SCANCODE_HOME) {
        MoveCaret(0, event.shift);
    } else if (event.key == SDL_SCANCODE_END) {
        MoveCaret(m_text.size(), event.shift);
    } else {
        return;
    }
    if (changed) {
        InvalidateLayout();
        EmitSignal("textChanged", {{"text", m_text}});
    }
    event.handled = true;
}
AccessibilitySemantics LineEdit::DescribeAccessibility() const {
    auto semantics = Control::DescribeAccessibility();
    text::TextLayout layout = MeasureTextLayout(m_text, 0)
                                  .value_or(text::TextLayout(m_text, "und", {}, {}));
    semantics.text = AccessibilityTextSemantics{
        .layout = std::move(layout),
        .origin = {10.0f, 8.0f},
        .caretByteOffset = m_cursor,
        .selectionStartByteOffset = SelectionStartByteOffset(),
        .selectionEndByteOffset = SelectionEndByteOffset(),
        .editable = true,
        .multiline = false,
    };
    semantics.actions.emplace_back("setCaret");
    semantics.actions.emplace_back("setSelection");
    return semantics;
}
bool LineEdit::PerformAccessibilityAction(const std::string_view action,
                                          const std::string_view value) {
    if (!Enabled() || !IsVisibleInTree()) return false;
    if (action == "setValue") {
        SetText(std::string(value));
        EmitSignal("textChanged", {{"text", m_text}});
        return true;
    }
    if (action == "setCaret") {
        std::size_t offset = 0;
        if (!ParseByteOffset(value, offset) || offset > m_text.size()) return false;
        MoveCaret(offset, false);
        return true;
    }
    if (action == "setSelection") {
        std::size_t start = 0, end = 0;
        if (!ParseByteRange(value, start, end) || start > m_text.size() ||
            end > m_text.size()) return false;
        SetSelectionBytes(start, end);
        return true;
    }
    if (action == "copyText") {
        std::size_t start = 0, end = 0;
        if (!ParseByteRange(value, start, end) || start > end ||
            end > m_text.size()) return false;
        return SDL_SetClipboardText(
            m_text.substr(start, end - start).c_str());
    }
    if (action == "pasteText") {
        std::size_t offset = 0;
        if (!ParseByteOffset(value, offset) || offset > m_text.size()) return false;
        char* clipboard = SDL_GetClipboardText();
        if (!clipboard) return false;
        const std::string inserted(clipboard);
        SDL_free(clipboard);
        m_text.insert(offset, inserted);
        m_cursor = m_selectionAnchor = offset + inserted.size();
        InvalidateLayout();
        EmitSignal("textChanged", {{"text", m_text}});
        return true;
    }
    if (action == "submit") {
        UIEvent event{.type = UIEventType::Activate};
        HandleEvent(event);
        return event.handled;
    }
    return Control::PerformAccessibilityAction(action, value);
}
Vec2 LineEdit::MeasureOverride(Vec2){return{220,44};}
void LineEdit::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme) {
    const auto& style=theme.Resolve(*this);
    DrawBox(renderer,LayoutRect(),StateBox(*this,style));
    const bool placeholder = m_text.empty();
    const std::string& shown=placeholder?m_placeholder:m_text;
    const Color color=placeholder?style.textDisabled:style.text;
    const Vec2 origin{LayoutRect().x+10,LayoutRect().y+8};
    const auto shownLayout=renderer.LayoutText(shown,style.font,style.fontSize);
    if (!placeholder)
        DrawSelection(renderer, shownLayout, origin, SelectionStartByteOffset(),
                      SelectionEndByteOffset());
    renderer.DrawTextLayout(shownLayout, origin, style.font, style.fontSize, color);
    if(Focused()){
        const auto layout=placeholder
                              ? renderer.LayoutText("",style.font,style.fontSize)
                              : shownLayout;
        const Vec2 caret=layout.CaretPosition(m_cursor);
        renderer.DrawRect({origin.x+caret.x,origin.y+caret.y,1,
                           static_cast<float>(style.fontSize+4)},
                          style.focused.border);
    }
}

RichTextLabel::RichTextLabel(std::string markup,std::string name):Label({},std::move(name)){SetWrap(true);SetMarkup(std::move(markup));}
void RichTextLabel::SetMarkup(std::string markup){auto parsed=text::ParseRubyMarkup(markup);if(m_markup==markup&&Text()==parsed.plain)return;m_markup=std::move(markup);m_ruby.clear();for(const auto& ruby:parsed.ruby)m_ruby.push_back({ruby.prefix,ruby.base,ruby.reading});if(m_runs.size()!=parsed.runs.size()||!std::equal(m_runs.begin(),m_runs.end(),parsed.runs.begin(),[](const text::TextRun& a,const text::TextRun& b){return a.byteStart==b.byteStart&&a.byteLength==b.byteLength&&a.style==b.style;}))InvalidateLayout();m_runs=std::move(parsed.runs);SetText(std::move(parsed.plain));}
AccessibilitySemantics RichTextLabel::DescribeAccessibility() const {
    auto semantics = Label::DescribeAccessibility();
    const int wrap = m_vertical
                         ? std::max(1, static_cast<int>(LayoutRect().h))
                         : (Wrap() ? std::max(1, static_cast<int>(LayoutRect().w))
                                   : 0);
    const auto orientation = m_vertical ? text::TextOrientation::Vertical
                                        : text::TextOrientation::Horizontal;
    text::TextLayout layout = MeasureTextLayout(
        Text(), FontSize(), wrap, orientation, m_verticalRows)
                                  .value_or(text::TextLayout(
                                      Text(), "und", {}, {}, orientation));
    semantics.text = AccessibilityTextSemantics{.layout = std::move(layout)};
    if (m_vertical)
        semantics.text->origin.x =
            (LayoutRect().w - semantics.text->layout.Size().x) * 0.5f;
    return semantics;
}
Vec2 RichTextLabel::MeasureOverride(const Vec2 available){if(UsesRunLayout()){const int wrap=Wrap()&&available.x>0.0f?std::max(1,static_cast<int>(std::floor(available.x))):0;if(const auto layout=MeasureTextRuns(Text(),FontRuns(RunsOver(m_runs,Text().size()),FontPath(),FontSize()),wrap))return layout->Size();}if(!m_vertical)return Label::MeasureOverride(available);const int height=available.y>0.0f?std::max(1,static_cast<int>(std::floor(available.y))):0;if(const auto layout=MeasureTextLayout(Text(),FontSize(),height,text::TextOrientation::Vertical,m_verticalRows))return layout->Size();return Label::MeasureOverride(available);}
void RichTextLabel::SetLayoutReferenceMarkup(const std::string_view markup) {
    auto parsed = text::ParseRubyMarkup(markup);
    m_referenceRuns = std::move(parsed.runs);
    SetLayoutReference(std::move(parsed.plain));
}

void RichTextLabel::DrawSelf(graphics::Renderer2D& renderer,
                             const Theme& theme) {
    const auto& style = theme.Resolve(*this);
    const int baseSize = FontSize() > 0 ? FontSize() : style.fontSize;
    const std::string& font = FontPath().empty() ? style.font : FontPath();
    if (UsesRunLayout()) {
        DrawStyled(renderer, theme, baseSize);
        return;
    }
    if (!m_vertical) {
        Label::DrawSelf(renderer, theme);
        return;
    }
    const auto layout = renderer.LayoutText(
        Text(), font, baseSize,
        std::max(1, static_cast<int>(std::floor(LayoutRect().h))),
        text::TextOrientation::Vertical, m_verticalRows);
    const Vec2 origin{LayoutRect().x +
                          (LayoutRect().w - layout.Size().x) * 0.5f,
                      LayoutRect().y};
    renderer.PushClip(LayoutRect());
    renderer.DrawTextLayout(layout, origin, font, baseSize, style.text);
    for (const auto& ruby : m_ruby) {
        const Rect base = layout.BoundsForRange(ruby.prefix.size(),
                                                ruby.base.size());
        const int rubySize = std::max(8, baseSize / 2);
        const auto rubyLayout = renderer.LayoutText(
            ruby.reading, font, rubySize, 0,
            text::TextOrientation::Vertical);
        renderer.DrawTextLayout(rubyLayout,
                                {origin.x + base.x + base.w,
                                 origin.y + base.y},
                                font, rubySize, style.text);
    }
    renderer.PopClip();
}

// Inline styles and ruby: each run is drawn in its own font, size and colour
// on a shared baseline, then ruby readings above their bases at half the
// base's run size. While a typewriter shows a prefix of the line, the whole
// line is laid out and only the visible part drawn, so nothing moves to
// another line as it appears.
void RichTextLabel::DrawStyled(graphics::Renderer2D& renderer, const Theme& theme,
                               const int baseSize) {
    const auto& style = theme.Resolve(*this);
    const std::string& baseFont = FontPath().empty() ? style.font : FontPath();
    const Color baseColor = HasColor() ? GetColor() : (Enabled() ? style.text : style.textDisabled);
    const std::string& visible = Text();
    const std::string& reference = LayoutReference();
    const bool prefix = reference.size() > visible.size() &&
                        reference.compare(0, visible.size(), visible) == 0;
    const std::string& laid = prefix ? reference : visible;
    const auto styleRuns = RunsOver(prefix && !m_referenceRuns.empty() ? m_referenceRuns : m_runs,
                                    laid.size());
    const auto runs = FontRuns(styleRuns, baseFont, baseSize);
    const Rect area = LayoutRect();
    const int wrap = Wrap() ? std::max(1, static_cast<int>(std::floor(area.w))) : 0;
    const auto layout = renderer.LayoutTextRuns(laid, runs, wrap);
    Vec2 origin{area.x, area.y};
    if (VerticalAlignment() == VerticalTextAlignment::Center)
        origin.y += (area.h - layout.Size().y) * 0.5f;
    else if (VerticalAlignment() == VerticalTextAlignment::Bottom)
        origin.y += area.h - layout.Size().y;
    // Lines align on their own width, as SDL_ttf aligns wrapped lines.
    std::vector<float> lineShift;
    for (const auto& cluster : layout.Clusters()) {
        const auto line = static_cast<std::size_t>(cluster.line);
        if (lineShift.size() <= line) lineShift.resize(line + 1, 0.0f);
        lineShift[line] = std::max(lineShift[line], cluster.bounds.x + cluster.bounds.w);
    }
    for (float& shift : lineShift) {
        shift = HorizontalAlignment() == HorizontalTextAlignment::Center ? (area.w - shift) * 0.5f
              : HorizontalAlignment() == HorizontalTextAlignment::Right ? area.w - shift : 0.0f;
    }
    const auto shiftOf = [&](const int line) {
        return static_cast<std::size_t>(line) < lineShift.size()
            ? lineShift[static_cast<std::size_t>(line)] : 0.0f;
    };
    const float rubyExtent = static_cast<float>(std::max(8, baseSize / 2));
    const float edge = static_cast<float>(OutlineSize());
    renderer.PushClip({area.x - edge, area.y - rubyExtent - edge,
                       area.w + edge * 2.0f, area.h + rubyExtent + edge * 2.0f});
    const auto& clusters = layout.Clusters();
    for (std::size_t first = 0; first < clusters.size();) {
        if (clusters[first].byteStart >= visible.size()) {
            ++first;
            continue;
        }
        const std::size_t run = RunAt(styleRuns, clusters[first].byteStart);
        std::size_t last = first;
        while (last + 1 < clusters.size() && clusters[last + 1].line == clusters[first].line &&
               clusters[last + 1].byteStart < visible.size() &&
               RunAt(styleRuns, clusters[last + 1].byteStart) == run)
            ++last;
        const auto& head = clusters[first];
        const std::size_t end = clusters[last].byteStart + clusters[last].byteLength;
        std::string piece = laid.substr(head.byteStart, end - head.byteStart);
        std::erase(piece, '\n');
        if (!piece.empty() && run < runs.size()) {
            const float width = clusters[last].bounds.x + clusters[last].bounds.w - head.bounds.x;
            renderer.DrawTextInRect(piece,
                {origin.x + shiftOf(head.line) + head.bounds.x, origin.y + head.bounds.y,
                 width + static_cast<float>(runs[run].size), head.bounds.h},
                runs[run].fontPath, runs[run].size, styleRuns[run].style.color.value_or(baseColor),
                graphics::HorizontalAlignment::Left, graphics::VerticalAlignment::Top, false,
                255, OutlineSize(), OutlineColor());
        }
        first = last + 1;
    }
    for (const auto& ruby : m_ruby) {
        // A typewriter reveal shows a prefix of the text; a reading is drawn
        // only once its whole base is visible.
        if (ruby.prefix.size() + ruby.base.size() > visible.size()) continue;
        const Rect base = layout.BoundsForRange(ruby.prefix.size(), ruby.base.size());
        if (base.w <= 0.0f) continue;
        const std::size_t run = RunAt(styleRuns, ruby.prefix.size());
        if (run >= runs.size()) continue;
        const int rubySize = std::max(8, runs[run].size / 2);
        const Vec2 reading = renderer.MeasureText(ruby.reading, runs[run].fontPath, rubySize);
        int line = 0;
        for (const auto& cluster : clusters)
            if (cluster.byteStart == ruby.prefix.size()) { line = cluster.line; break; }
        renderer.DrawTextInRect(ruby.reading,
            {origin.x + shiftOf(line) + base.x + base.w * 0.5f - reading.x * 0.5f,
             origin.y + base.y - static_cast<float>(rubySize), reading.x + 2.0f,
             static_cast<float>(rubySize) * 1.5f},
            runs[run].fontPath, rubySize, styleRuns[run].style.color.value_or(baseColor),
            graphics::HorizontalAlignment::Left, graphics::VerticalAlignment::Top, false,
            255, OutlineSize(), OutlineColor());
    }
    renderer.PopClip();
}

void NinePatchRect::CollectImageDependencies(std::vector<std::string>& paths) const {
    if (!m_path.empty()) paths.push_back(m_path);
}
void NinePatchRect::DrawSelf(graphics::Renderer2D& renderer,const Theme&){if(!m_path.empty())renderer.DrawNinePatch(m_path,LayoutRect(),m_margins,m_drawCenter);}

TextEdit::TextEdit(std::string text,std::string name)
    :Control(std::move(name)),m_text(std::move(text)),m_cursor(m_text.size()),
     m_selectionAnchor(m_cursor){SetFocusMode(FocusMode::All);}
void TextEdit::Changed(){InvalidateLayout();EmitSignal("textChanged",{{"text",m_text}});}
void TextEdit::SetSelectionBytes(const std::size_t start,
                                 const std::size_t end) {
    m_selectionAnchor = SnapGrapheme(m_text, start);
    m_cursor = SnapGrapheme(m_text, end);
}
void TextEdit::MoveCaret(const std::size_t value, const bool extend) {
    const std::size_t next = SnapGrapheme(m_text, value);
    if (!extend) m_selectionAnchor = next;
    m_cursor = next;
}
bool TextEdit::DeleteSelection() {
    const std::size_t start = SelectionStartByteOffset();
    const std::size_t end = SelectionEndByteOffset();
    if (start == end) return false;
    m_text.erase(start, end - start);
    m_cursor = m_selectionAnchor = start;
    return true;
}
void TextEdit::SetCaretFromPoint(const Vec2 point, const bool extend) {
    const int wrap = std::max(1, static_cast<int>(LayoutRect().w - 20.0f));
    const auto layout = MeasureTextLayout(m_text, 0, wrap);
    const std::size_t target = layout
        ? layout->ByteOffsetAt({point.x - LayoutRect().x - 10.0f,
                                point.y - LayoutRect().y - 8.0f})
        : m_text.size();
    MoveCaret(target, extend);
}
void TextEdit::HandleEvent(UIEvent& event) {
    if (event.type == UIEventType::PointerDown) {
        SetCaretFromPoint(event.position, event.shift);
        m_pointerSelecting = true;
        event.handled = true;
        return;
    }
    if (event.type == UIEventType::PointerMove && m_pointerSelecting) {
        SetCaretFromPoint(event.position, true);
        event.handled = true;
        return;
    }
    if (event.type == UIEventType::PointerUp ||
        event.type == UIEventType::FocusLost)
        m_pointerSelecting = false;
    if (event.type == UIEventType::TextInput) {
        if (m_readOnly) return;
        (void)DeleteSelection();
        m_text.insert(m_cursor,event.text);
        m_cursor+=event.text.size();
        m_selectionAnchor=m_cursor;
        Changed();event.handled=true;return;
    }
    if(event.type==UIEventType::Activate){
        if (m_readOnly) return;
        (void)DeleteSelection();
        m_text.insert(m_cursor,"\n");++m_cursor;m_selectionAnchor=m_cursor;
        Changed();event.handled=true;return;
    }
    if(event.type!=UIEventType::KeyDown)return;
    bool changed=false;
    if(event.control&&event.key==SDL_SCANCODE_A){SetSelectionBytes(0,m_text.size());}
    else if(event.key==SDL_SCANCODE_BACKSPACE&&!m_readOnly){
        changed=DeleteSelection();
        if(!changed&&m_cursor>0){const auto previous=PreviousGrapheme(m_text,m_cursor);m_text.erase(previous,m_cursor-previous);m_cursor=m_selectionAnchor=previous;changed=true;}
    }
    else if(event.key==SDL_SCANCODE_DELETE&&!m_readOnly){
        changed=DeleteSelection();
        if(!changed&&m_cursor<m_text.size()){const auto next=NextGrapheme(m_text,m_cursor);m_text.erase(m_cursor,next-m_cursor);m_selectionAnchor=m_cursor;changed=true;}
    }
    else if(event.key==SDL_SCANCODE_LEFT){
        if(!event.shift&&SelectionStartByteOffset()!=SelectionEndByteOffset())MoveCaret(SelectionStartByteOffset(),false);
        else MoveCaret(PreviousGrapheme(m_text,m_cursor),event.shift);
    }
    else if(event.key==SDL_SCANCODE_RIGHT){
        if(!event.shift&&SelectionStartByteOffset()!=SelectionEndByteOffset())MoveCaret(SelectionEndByteOffset(),false);
        else MoveCaret(NextGrapheme(m_text,m_cursor),event.shift);
    }
    else if(event.key==SDL_SCANCODE_UP||event.key==SDL_SCANCODE_DOWN){
        const int wrap=std::max(1,static_cast<int>(LayoutRect().w-20.0f));
        if(const auto layout=MeasureTextLayout(m_text,0,wrap)){
            Vec2 position=layout->CaretPosition(m_cursor);
            const float step=std::max(1.0f,layout->Size().y/
                static_cast<float>(std::max(1,layout->Clusters().empty()?1:
                    1+std::ranges::max_element(layout->Clusters(),{},&text::TextClusterLayout::line)->line)));
            position.y+=event.key==SDL_SCANCODE_UP?-step:step;
            MoveCaret(layout->ByteOffsetAt(position),event.shift);
        }
    }
    else if(event.key==SDL_SCANCODE_HOME)MoveCaret(0,event.shift);
    else if(event.key==SDL_SCANCODE_END)MoveCaret(m_text.size(),event.shift);
    else return;
    if(changed)Changed();event.handled=true;
}
AccessibilitySemantics TextEdit::DescribeAccessibility() const {
    auto semantics=Control::DescribeAccessibility();
    const int wrap=std::max(1,static_cast<int>(LayoutRect().w-20.0f));
    text::TextLayout layout=MeasureTextLayout(m_text,0,wrap)
        .value_or(text::TextLayout(m_text,"und",{},{}));
    semantics.readOnly=m_readOnly;
    semantics.text=AccessibilityTextSemantics{
        .layout=std::move(layout),
        .origin={10.0f,8.0f},
        .caretByteOffset=m_cursor,
        .selectionStartByteOffset=SelectionStartByteOffset(),
        .selectionEndByteOffset=SelectionEndByteOffset(),
        .editable=!m_readOnly,
        .multiline=true,
    };
    semantics.actions.emplace_back("setCaret");
    semantics.actions.emplace_back("setSelection");
    return semantics;
}
bool TextEdit::PerformAccessibilityAction(const std::string_view action,
                                          const std::string_view value) {
    if(!Enabled()||!IsVisibleInTree())return false;
    if(action=="setValue"){
        if(m_readOnly)return false;
        SetText(std::string(value));Changed();return true;
    }
    if(action=="setCaret"){
        std::size_t offset=0;
        if(!ParseByteOffset(value,offset)||offset>m_text.size())return false;
        MoveCaret(offset,false);return true;
    }
    if(action=="setSelection"){
        std::size_t start=0,end=0;
        if(!ParseByteRange(value,start,end)||start>m_text.size()||end>m_text.size())return false;
        SetSelectionBytes(start,end);return true;
    }
    if(action=="copyText"){
        std::size_t start=0,end=0;
        if(!ParseByteRange(value,start,end)||start>end||end>m_text.size())return false;
        return SDL_SetClipboardText(m_text.substr(start,end-start).c_str());
    }
    if(action=="pasteText"){
        if(m_readOnly)return false;
        std::size_t offset=0;
        if(!ParseByteOffset(value,offset)||offset>m_text.size())return false;
        char* clipboard=SDL_GetClipboardText();
        if(!clipboard)return false;
        const std::string inserted(clipboard);SDL_free(clipboard);
        m_text.insert(offset,inserted);m_cursor=m_selectionAnchor=offset+inserted.size();
        Changed();return true;
    }
    return Control::PerformAccessibilityAction(action,value);
}
Vec2 TextEdit::MeasureOverride(Vec2 available){return{std::min(360.0f,std::max(180.0f,available.x)),std::min(180.0f,std::max(88.0f,available.y))};}
void TextEdit::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){
    const auto& style=theme.Resolve(*this);DrawBox(renderer,LayoutRect(),StateBox(*this,style));
    Rect content=LayoutRect();content.x+=10;content.y+=8;content.w=std::max(0.0f,content.w-20);content.h=std::max(0.0f,content.h-16);
    const bool placeholder=m_text.empty();const std::string& shown=placeholder?m_placeholder:m_text;
    const auto layout=renderer.LayoutText(shown,style.font,style.fontSize,
        std::max(1,static_cast<int>(content.w)));
    renderer.PushClip(content);
    if(!placeholder)DrawSelection(renderer,layout,{content.x,content.y},SelectionStartByteOffset(),SelectionEndByteOffset());
    renderer.DrawTextLayout(layout,{content.x,content.y},style.font,style.fontSize,
        placeholder?style.textDisabled:style.text);
    if(Focused()){
        const Vec2 caret=placeholder?Vec2{}:layout.CaretPosition(m_cursor);
        renderer.DrawRect({content.x+caret.x,content.y+caret.y,1,
                           static_cast<float>(style.fontSize+4)},style.focused.border);
    }
    renderer.PopClip();
}

OptionButton::OptionButton(std::string name):Button({},std::move(name)){}
void OptionButton::SetOptions(std::vector<std::string> options){m_options=std::move(options);if(m_options.empty())m_selected=-1;else m_selected=std::clamp(m_selected<0?0:m_selected,0,static_cast<int>(m_options.size()-1));SetText(m_selected>=0?m_options[static_cast<std::size_t>(m_selected)]:std::string{});}
void OptionButton::SetSelected(const int value){if(m_options.empty()){m_selected=-1;SetText({});return;}m_selected=std::clamp(value,0,static_cast<int>(m_options.size()-1));SetText(m_options[static_cast<std::size_t>(m_selected)]);}
void OptionButton::SelectFromUser(const int value){const int before=m_selected;SetSelected(value);if(m_selected!=before)EmitSignal("itemSelected",{{"index",static_cast<std::int64_t>(m_selected)},{"text",Text()}});}
void OptionButton::HandleEvent(UIEvent& event){if(event.type==UIEventType::Click||event.type==UIEventType::Activate){if(!m_options.empty())SelectFromUser((m_selected+1)%static_cast<int>(m_options.size()));Activate();event.handled=true;return;}Button::HandleEvent(event);}
void OptionButton::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Button::DrawSelf(renderer,theme);const auto& style=theme.Resolve(*this);Rect arrow=LayoutRect();arrow.x=arrow.x+arrow.w-30;arrow.w=22;renderer.DrawTextInRect("▼",arrow,style.font,std::max(8,style.fontSize*2/3),style.text,graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,false);}

SpinBox::SpinBox(std::string name):Control(std::move(name)){SetFocusMode(FocusMode::All);}
void SpinBox::SetRange(const double minimum,const double maximum,const double step){m_minimum=minimum;m_maximum=std::max(minimum,maximum);m_step=std::max(0.000001,step);SetValue(m_value);}
void SpinBox::SetValue(double value){value=std::clamp(value,m_minimum,m_maximum);value=m_minimum+std::round((value-m_minimum)/m_step)*m_step;m_value=std::clamp(value,m_minimum,m_maximum);}
void SpinBox::ChangeFromUser(const double value){const double before=m_value;SetValue(value);if(m_value!=before)EmitSignal("valueChanged",{{"value",m_value}});}
void SpinBox::HandleEvent(UIEvent& event){if(event.type==UIEventType::Click){const Rect area=LayoutRect();ChangeFromUser(m_value+(event.position.y<area.y+area.h*.5f?m_step:-m_step));event.handled=true;}
    else if(event.type==UIEventType::KeyDown){if(event.key==SDL_SCANCODE_UP)ChangeFromUser(m_value+m_step);else if(event.key==SDL_SCANCODE_DOWN)ChangeFromUser(m_value-m_step);else return;event.handled=true;}}
Vec2 SpinBox::MeasureOverride(Vec2){return{140,44};}
void SpinBox::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(*this);DrawBox(renderer,LayoutRect(),StateBox(*this,style));std::ostringstream text;text<<std::setprecision(8)<<m_value;Rect valueArea=LayoutRect();valueArea.x+=10;valueArea.w=std::max(0.0f,valueArea.w-42);renderer.DrawTextInRect(text.str(),valueArea,style.font,style.fontSize,style.text,graphics::HorizontalAlignment::Left,graphics::VerticalAlignment::Center,false);Rect arrows=LayoutRect();arrows.x=arrows.x+arrows.w-30;arrows.w=26;renderer.DrawTextInRect("▲\n▼",arrows,style.font,std::max(10,style.fontSize/2),style.text,graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,true);}

void RadioButton::ClearGroup(){if(m_group.empty())return;scene::Node* root=this;while(root->Parent())root=root->Parent();std::vector<scene::Node*> stack{root};while(!stack.empty()){scene::Node* node=stack.back();stack.pop_back();if(auto* radio=dynamic_cast<RadioButton*>(node);radio&&radio!=this&&radio->Group()==m_group&&radio->Checked()){radio->SetChecked(false);radio->EmitSignal("toggled",{{"value",false}});}for(const auto& child:node->Children())stack.push_back(child.get());}}
void RadioButton::HandleEvent(UIEvent& event){if(event.type==UIEventType::Click||event.type==UIEventType::Activate){if(!Checked()){ClearGroup();SetChecked(true);EmitSignal("toggled",{{"value",true}});}Activate();event.handled=true;return;}CheckBox::HandleEvent(event);}
void RadioButton::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Button::DrawSelf(renderer,theme);const Rect area=LayoutRect();const float size=std::min(22.0f,area.h-10);renderer.DrawRoundedRect({area.x+8,area.y+(area.h-size)*.5f,size,size},size*.5f,{20,23,30,255});if(Checked())renderer.DrawRoundedRect({area.x+13,area.y+(area.h-size)*.5f+5,size-10,size-10},(size-10)*.5f,theme.Resolve(*this).focused.border);}

Vec2 Separator::MeasureOverride(Vec2){return m_orientation==SeparatorOrientation::Horizontal?Vec2{80,8}:Vec2{8,80};}
void Separator::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){Rect line=LayoutRect();if(m_orientation==SeparatorOrientation::Horizontal){line.y+=line.h*.5f;line.h=1;}else{line.x+=line.w*.5f;line.w=1;}renderer.DrawRect(line,theme.Resolve(*this).normal.border);}

void ScrollBar::SetFromPosition(const Vec2 position){const Rect area=LayoutRect();const double ratio=m_vertical?(area.h>0?std::clamp((position.y-area.y)/area.h,0.0f,1.0f):0.0):(area.w>0?std::clamp((position.x-area.x)/area.w,0.0f,1.0f):0.0);SetValue(Minimum()+(Maximum()-Minimum())*ratio);}
void ScrollBar::HandleEvent(UIEvent& event){if(event.type==UIEventType::PointerDown||(event.type==UIEventType::PointerMove&&Pressed())){SetFromPosition(event.position);event.handled=true;return;}if(event.type==UIEventType::Scroll){SetValue(Value()-event.wheel*(Step()>0?Step():(Maximum()-Minimum())*.05));event.handled=true;return;}Slider::HandleEvent(event);}
Vec2 ScrollBar::MeasureOverride(Vec2){return m_vertical?Vec2{18,140}:Vec2{140,18};}
void ScrollBar::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){const auto& style=theme.Resolve(*this);const Rect area=LayoutRect();renderer.DrawRoundedRect(area,std::min(area.w,area.h)*.5f,style.normal.background);const double range=Maximum()-Minimum();const float position=range>0?static_cast<float>((Value()-Minimum())/range):0;const float page=static_cast<float>(std::clamp(m_page,0.05,1.0));Rect thumb=area;if(m_vertical){thumb.h=std::max(area.w,area.h*page);thumb.y=area.y+(area.h-thumb.h)*position;}else{thumb.w=std::max(area.h,area.w*page);thumb.x=area.x+(area.w-thumb.w)*position;}renderer.DrawRoundedRect(thumb,std::min(thumb.w,thumb.h)*.5f,style.focused.border);}

void VideoRect::SetPath(std::string value){if(m_path==value)return;const bool resume=m_playing||m_autoplay;if(m_playback.close)m_playback.close();m_playing=false;m_path=std::move(value);if(resume)SetPlaying(true);}
void VideoRect::CollectImageDependencies(std::vector<std::string>& paths) const {
    if (!m_poster.empty()) paths.push_back(m_poster);
}
void VideoRect::SetAutoplay(const bool value){m_autoplay=value;if(value&&!m_playing)SetPlaying(true);}
void VideoRect::SetPlayback(Playback playback){if(m_playback.close)m_playback.close();m_playback=std::move(playback);const bool start=m_playing||m_autoplay;m_playing=false;if(start)SetPlaying(true);}
void VideoRect::SetPlaying(const bool value){if(!value){if(!m_playing)return;if(m_playback.close)m_playback.close();m_playing=false;EmitSignal("playbackStopped");return;}if(m_playing&&(!m_playback.playing||m_playback.playing()))return;const bool opened=!m_path.empty()&&m_playback.open&&m_playback.open(m_path);m_playing=opened;if(opened)EmitSignal("playbackStarted");}
void VideoRect::Update(const float deltaSeconds){Control::Update(deltaSeconds);if(!m_playing)return;if(m_playback.update)m_playback.update(deltaSeconds);if(m_playback.playing&&m_playback.playing())return;if(m_loop&&m_playback.open&&m_playback.open(m_path))return;m_playing=false;EmitSignal("playbackStopped");}
Vec2 VideoRect::MeasureOverride(Vec2){return{320,180};}
void VideoRect::DrawSelf(graphics::Renderer2D& renderer,const Theme& theme){if(m_playing&&m_playback.texture){const graphics::TextureHandle texture=m_playback.texture();const Vec2 size=m_playback.size?m_playback.size():Vec2{};if(texture&&size.x>0&&size.y>0){const Rect area=LayoutRect();const float scale=std::min(area.w/size.x,area.h/size.y);const Rect target{area.x+(area.w-size.x*scale)*.5f,area.y+(area.h-size.y*scale)*.5f,size.x*scale,size.y*scale};renderer.DrawTexture(texture,target);return;}}if(!m_poster.empty()){renderer.DrawImageInRect(m_poster,LayoutRect(),graphics::ContentScaleMode::Fit);return;}renderer.DrawRect(LayoutRect(),{8,10,14,255});renderer.DrawTextInRect(m_path.empty()?"Video":"Video · "+m_path,LayoutRect(),theme.Resolve(*this).font,16,{150,158,174,255},graphics::HorizontalAlignment::Center,graphics::VerticalAlignment::Center,true);}

}  // namespace px::ui
