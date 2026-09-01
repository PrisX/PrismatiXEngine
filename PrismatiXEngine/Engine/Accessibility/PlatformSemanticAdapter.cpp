#include "Engine/Accessibility/PlatformSemanticAdapter.h"

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <oleauto.h>
#include <UIAutomationClient.h>
#include <UIAutomationCore.h>
#include <UIAutomationCoreApi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace px::accessibility {
namespace {

constexpr wchar_t kAdapterProperty[] = L"PrismatiX.SemanticAdapter";
constexpr UINT kAccessibilityActionMessage = WM_APP + 0x2A1;

bool Has(const std::vector<std::string>& values, const std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::wstring Wide(const std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                              static_cast<int>(text.size()), output.data(), count);
    return output;
}

std::string Utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int length = static_cast<int>(wcslen(text));
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
                                          length, nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string output(static_cast<std::size_t>(count), '\0');
    (void)WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
                              output.data(), count, nullptr, nullptr);
    return output;
}

CONTROLTYPEID ControlType(const std::string_view role) {
    if (role == "button") return UIA_ButtonControlTypeId;
    if (role == "checkbox") return UIA_CheckBoxControlTypeId;
    if (role == "radio") return UIA_RadioButtonControlTypeId;
    if (role == "slider") return UIA_SliderControlTypeId;
    if (role == "combobox") return UIA_ComboBoxControlTypeId;
    if (role == "textbox") return UIA_EditControlTypeId;
    if (role == "text" || role == "heading") return UIA_TextControlTypeId;
    if (role == "image") return UIA_ImageControlTypeId;
    if (role == "progressbar") return UIA_ProgressBarControlTypeId;
    if (role == "list" || role == "listbox" || role == "log")
        return UIA_ListControlTypeId;
    if (role == "window" || role == "dialog") return UIA_PaneControlTypeId;
    return UIA_GroupControlTypeId;
}

struct Entry {
    ui::AccessibilitySemantics semantics;
    std::optional<Uuid> parent;
    std::vector<Uuid> children;
};

class WindowsSemanticAdapter;
class TextRangeProvider;

class Provider final : public IRawElementProviderSimple,
                       public IRawElementProviderFragment,
                       public IRawElementProviderFragmentRoot,
                       public IInvokeProvider,
                       public IToggleProvider,
                       public IRangeValueProvider,
                       public IValueProvider,
                       public ITextProvider {
public:
    Provider(std::shared_ptr<WindowsSemanticAdapter> owner, Uuid id)
        : m_owner(std::move(owner)), m_id(id) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --m_refs;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* value) override;
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** value) override;
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* value) override;
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(
        IRawElementProviderSimple** value) override;
    HRESULT STDMETHODCALLTYPE Navigate(
        NavigateDirection direction, IRawElementProviderFragment** value) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** value) override;
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* value) override;
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** value) override;
    HRESULT STDMETHODCALLTYPE SetFocus() override;
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(
        IRawElementProviderFragmentRoot** value) override;
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(
        double x, double y, IRawElementProviderFragment** value) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** value) override;
    HRESULT STDMETHODCALLTYPE Invoke() override;
    HRESULT STDMETHODCALLTYPE Toggle() override;
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* value) override;
    HRESULT STDMETHODCALLTYPE SetValue(double value) override;
    HRESULT STDMETHODCALLTYPE get_Value(double* value) override;
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* value) override;
    HRESULT STDMETHODCALLTYPE get_Maximum(double* value) override;
    HRESULT STDMETHODCALLTYPE get_Minimum(double* value) override;
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* value) override;
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* value) override;
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR value) override;
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* value) override;
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** value) override;
    HRESULT STDMETHODCALLTYPE GetVisibleRanges(SAFEARRAY** value) override;
    HRESULT STDMETHODCALLTYPE RangeFromChild(
        IRawElementProviderSimple* child, ITextRangeProvider** value) override;
    HRESULT STDMETHODCALLTYPE RangeFromPoint(
        UiaPoint point, ITextRangeProvider** value) override;
    HRESULT STDMETHODCALLTYPE get_DocumentRange(
        ITextRangeProvider** value) override;
    HRESULT STDMETHODCALLTYPE get_SupportedTextSelection(
        SupportedTextSelection* value) override;

private:
    ~Provider() = default;
    std::shared_ptr<WindowsSemanticAdapter> m_owner;
    Uuid m_id;
    std::atomic<ULONG> m_refs{1};
};

class TextRangeProvider final : public ITextRangeProvider {
public:
    TextRangeProvider(std::shared_ptr<WindowsSemanticAdapter> owner, Uuid id,
                      std::size_t start, std::size_t end)
        : m_owner(std::move(owner)), m_id(id), m_start(start), m_end(end) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG refs = --m_refs;
        if (!refs) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE Clone(ITextRangeProvider** value) override;
    HRESULT STDMETHODCALLTYPE Compare(ITextRangeProvider* range,
                                      BOOL* value) override;
    HRESULT STDMETHODCALLTYPE CompareEndpoints(
        TextPatternRangeEndpoint endpoint, ITextRangeProvider* targetRange,
        TextPatternRangeEndpoint targetEndpoint, int* value) override;
    HRESULT STDMETHODCALLTYPE ExpandToEnclosingUnit(TextUnit unit) override;
    HRESULT STDMETHODCALLTYPE FindAttribute(TEXTATTRIBUTEID attributeId,
                                            VARIANT value, BOOL backward,
                                            ITextRangeProvider** result) override;
    HRESULT STDMETHODCALLTYPE FindText(BSTR value, BOOL backward,
                                       BOOL ignoreCase,
                                       ITextRangeProvider** result) override;
    HRESULT STDMETHODCALLTYPE GetAttributeValue(TEXTATTRIBUTEID attributeId,
                                                VARIANT* value) override;
    HRESULT STDMETHODCALLTYPE GetBoundingRectangles(SAFEARRAY** value) override;
    HRESULT STDMETHODCALLTYPE GetEnclosingElement(
        IRawElementProviderSimple** value) override;
    HRESULT STDMETHODCALLTYPE GetText(int maxLength, BSTR* value) override;
    HRESULT STDMETHODCALLTYPE Move(TextUnit unit, int count,
                                   int* moved) override;
    HRESULT STDMETHODCALLTYPE MoveEndpointByUnit(
        TextPatternRangeEndpoint endpoint, TextUnit unit, int count,
        int* moved) override;
    HRESULT STDMETHODCALLTYPE MoveEndpointByRange(
        TextPatternRangeEndpoint endpoint, ITextRangeProvider* targetRange,
        TextPatternRangeEndpoint targetEndpoint) override;
    HRESULT STDMETHODCALLTYPE Select() override;
    HRESULT STDMETHODCALLTYPE AddToSelection() override;
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override;
    HRESULT STDMETHODCALLTYPE ScrollIntoView(BOOL alignToTop) override;
    HRESULT STDMETHODCALLTYPE GetChildren(SAFEARRAY** value) override;

    [[nodiscard]] const Uuid& Id() const { return m_id; }
    [[nodiscard]] std::size_t Start() const { return m_start; }
    [[nodiscard]] std::size_t End() const { return m_end; }

private:
    ~TextRangeProvider() = default;
    [[nodiscard]] std::vector<std::size_t> Boundaries(const Entry& entry) const;
    void Normalize(const Entry& entry);
    std::shared_ptr<WindowsSemanticAdapter> m_owner;
    Uuid m_id;
    std::size_t m_start = 0;
    std::size_t m_end = 0;
    std::atomic<ULONG> m_refs{1};
};

class WindowsSemanticAdapter final
    : public SemanticAdapter,
      public std::enable_shared_from_this<WindowsSemanticAdapter> {
public:
    static std::shared_ptr<WindowsSemanticAdapter> Create(SDL_Window* window) {
        if (!window) return {};
        auto* hwnd = static_cast<HWND>(SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER,
            nullptr));
        if (!hwnd) return {};
        auto adapter = std::shared_ptr<WindowsSemanticAdapter>(
            new WindowsSemanticAdapter(hwnd));
        if (!adapter->Attach()) return {};
        return adapter;
    }

    ~WindowsSemanticAdapter() override {
        if (m_hwnd && IsWindow(m_hwnd)) {
            if (reinterpret_cast<WNDPROC>(GetWindowLongPtrW(m_hwnd, GWLP_WNDPROC)) ==
                &WindowProc)
                (void)SetWindowLongPtrW(m_hwnd, GWLP_WNDPROC,
                                        reinterpret_cast<LONG_PTR>(m_previous));
            RemovePropW(m_hwnd, kAdapterProperty);
        }
        if (m_comInitialized) CoUninitialize();
    }

    void Publish(const SemanticTree& tree) override {
        Uuid root;
        std::optional<Uuid> focus;
        {
            std::scoped_lock lock(m_mutex);
            m_entries.clear();
            Flatten(tree.root, std::nullopt);
            m_root = tree.root.semantics.id;
            m_revision = tree.revision;
            root = m_root;
            for (const auto& [id, entry] : m_entries)
                if (Has(entry.semantics.states, "focused")) {
                    focus = id;
                    break;
                }
        }
        if (!root.Empty() && UiaClientsAreListening()) {
            auto* provider = new Provider(shared_from_this(), root);
            (void)UiaRaiseAutomationEvent(provider, UIA_LayoutInvalidatedEventId);
            provider->Release();
            if (focus && focus != m_lastFocus) {
                auto* focused = new Provider(shared_from_this(), *focus);
                (void)UiaRaiseAutomationEvent(
                    focused, UIA_AutomationFocusChangedEventId);
                focused->Release();
            }
        }
        m_lastFocus = focus;
    }

    [[nodiscard]] std::optional<Entry> Find(const Uuid& id) const {
        std::scoped_lock lock(m_mutex);
        const auto found = m_entries.find(id);
        return found == m_entries.end() ? std::nullopt
                                        : std::optional<Entry>(found->second);
    }
    [[nodiscard]] Uuid Root() const {
        std::scoped_lock lock(m_mutex);
        return m_root;
    }
    [[nodiscard]] std::optional<Uuid> Focused() const {
        std::scoped_lock lock(m_mutex);
        for (const auto& [id, entry] : m_entries)
            if (Has(entry.semantics.states, "focused")) return id;
        return std::nullopt;
    }
    [[nodiscard]] std::optional<Uuid> At(const double screenX,
                                         const double screenY) const {
        POINT origin{0, 0};
        (void)ClientToScreen(m_hwnd, &origin);
        const double x = screenX - origin.x;
        const double y = screenY - origin.y;
        std::scoped_lock lock(m_mutex);
        std::optional<Uuid> best;
        double bestArea = std::numeric_limits<double>::max();
        for (const auto& [id, entry] : m_entries) {
            const Rect& bounds = entry.semantics.bounds;
            if (entry.semantics.hidden || x < bounds.x || y < bounds.y ||
                x > bounds.x + bounds.w || y > bounds.y + bounds.h)
                continue;
            const double area = std::max(0.0, static_cast<double>(bounds.w)) *
                                std::max(0.0, static_cast<double>(bounds.h));
            if (area <= bestArea) {
                best = id;
                bestArea = area;
            }
        }
        return best;
    }
    [[nodiscard]] HWND Hwnd() const { return m_hwnd; }
    bool Dispatch(const Uuid& id, const std::string_view action,
                  const std::string_view value = {}) {
        if (GetCurrentThreadId() == m_thread)
            return InvokeAction(id, action, value);
        ActionRequest request{id, std::string(action), std::string(value), false};
        (void)SendMessageW(m_hwnd, kAccessibilityActionMessage, 0,
                           reinterpret_cast<LPARAM>(&request));
        return request.result;
    }

private:
    struct ActionRequest {
        Uuid id;
        std::string action;
        std::string value;
        bool result = false;
    };

    explicit WindowsSemanticAdapter(HWND hwnd)
        : m_hwnd(hwnd), m_thread(GetCurrentThreadId()) {
        const HRESULT initialized =
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        m_comInitialized = SUCCEEDED(initialized);
    }
    bool Attach() {
        if (!SetPropW(m_hwnd, kAdapterProperty, this)) return false;
        SetLastError(0);
        m_previous = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            m_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WindowProc)));
        if (!m_previous && GetLastError() != 0) {
            RemovePropW(m_hwnd, kAdapterProperty);
            return false;
        }
        return true;
    }
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                                       LPARAM lParam) {
        auto* self = static_cast<WindowsSemanticAdapter*>(
            GetPropW(hwnd, kAdapterProperty));
        if (self && message == WM_GETOBJECT &&
            static_cast<LONG>(lParam) == UiaRootObjectId) {
            const Uuid root = self->Root();
            if (!root.Empty()) {
                auto owner = self->shared_from_this();
                auto* provider = new Provider(std::move(owner), root);
                const LRESULT result = UiaReturnRawElementProvider(
                    hwnd, wParam, lParam, provider);
                provider->Release();
                return result;
            }
        }
        if (self && message == kAccessibilityActionMessage) {
            auto* request = reinterpret_cast<ActionRequest*>(lParam);
            if (request)
                request->result = self->InvokeAction(
                    request->id, request->action, request->value);
            return request && request->result ? 1 : 0;
        }
        return self && self->m_previous
                   ? CallWindowProcW(self->m_previous, hwnd, message, wParam,
                                     lParam)
                   : DefWindowProcW(hwnd, message, wParam, lParam);
    }
    void Flatten(const SemanticNode& node, const std::optional<Uuid> parent) {
        Entry entry{node.semantics, parent, {}};
        entry.children.reserve(node.children.size());
        for (const auto& child : node.children)
            entry.children.push_back(child.semantics.id);
        m_entries.insert_or_assign(node.semantics.id, std::move(entry));
        for (const auto& child : node.children) Flatten(child, node.semantics.id);
    }

    HWND m_hwnd = nullptr;
    WNDPROC m_previous = nullptr;
    DWORD m_thread = 0;
    bool m_comInitialized = false;
    mutable std::mutex m_mutex;
    std::unordered_map<Uuid, Entry, UuidHash> m_entries;
    Uuid m_root;
    std::uint64_t m_revision = 0;
    std::optional<Uuid> m_lastFocus;
};

Provider* MakeProvider(const std::shared_ptr<WindowsSemanticAdapter>& owner,
                       const Uuid& id) {
    return id.Empty() ? nullptr : new Provider(owner, id);
}

SAFEARRAY* RangeArray(std::vector<ITextRangeProvider*> ranges) {
    SAFEARRAY* result = SafeArrayCreateVector(
        VT_UNKNOWN, 0, static_cast<ULONG>(ranges.size()));
    if (!result) return nullptr;
    for (LONG index = 0; index < static_cast<LONG>(ranges.size()); ++index) {
        IUnknown* range = ranges[static_cast<std::size_t>(index)];
        if (FAILED(SafeArrayPutElement(result, &index, range))) {
            SafeArrayDestroy(result);
            for (auto* item : ranges) item->Release();
            return nullptr;
        }
    }
    for (auto* item : ranges) item->Release();
    return result;
}

std::vector<std::size_t> TextRangeProvider::Boundaries(
    const Entry& entry) const {
    std::vector<std::size_t> result{0};
    if (!entry.semantics.text) return result;
    const auto& layout = entry.semantics.text->layout;
    for (const auto& cluster : layout.Clusters()) {
        result.push_back(cluster.byteStart);
        result.push_back(cluster.byteStart + cluster.byteLength);
    }
    if (layout.Clusters().empty()) {
        const std::string& value = layout.Text();
        for (std::size_t byte = 0; byte < value.size();) {
            const auto lead = static_cast<unsigned char>(value[byte]);
            std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2
                                          : (lead & 0xf0) == 0xe0 ? 3
                                          : (lead & 0xf8) == 0xf0 ? 4 : 1;
            if (byte + length > value.size()) length = 1;
            byte += length;
            result.push_back(byte);
        }
    }
    result.push_back(layout.Text().size());
    std::ranges::sort(result);
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void TextRangeProvider::Normalize(const Entry& entry) {
    const std::size_t size = entry.semantics.text
                                 ? entry.semantics.text->layout.Text().size()
                                 : 0;
    m_start = std::min(m_start, size);
    m_end = std::clamp(m_end, m_start, size);
}

HRESULT TextRangeProvider::QueryInterface(REFIID iid, void** output) {
    if (!output) return E_INVALIDARG;
    *output = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ITextRangeProvider))
        *output = static_cast<ITextRangeProvider*>(this);
    if (!*output) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

HRESULT TextRangeProvider::Clone(ITextRangeProvider** value) {
    if (!value) return E_INVALIDARG;
    *value = new TextRangeProvider(m_owner, m_id, m_start, m_end);
    return S_OK;
}

HRESULT TextRangeProvider::Compare(ITextRangeProvider* range, BOOL* value) {
    if (!value) return E_INVALIDARG;
    const auto* other = dynamic_cast<TextRangeProvider*>(range);
    *value = other && other->m_owner.get() == m_owner.get() &&
                     other->m_id == m_id && other->m_start == m_start &&
                     other->m_end == m_end;
    return S_OK;
}

HRESULT TextRangeProvider::CompareEndpoints(
    const TextPatternRangeEndpoint endpoint, ITextRangeProvider* targetRange,
    const TextPatternRangeEndpoint targetEndpoint, int* value) {
    if (!value) return E_INVALIDARG;
    const auto* other = dynamic_cast<TextRangeProvider*>(targetRange);
    if (!other || other->m_owner.get() != m_owner.get() || other->m_id != m_id)
        return E_INVALIDARG;
    const std::size_t left = endpoint == TextPatternRangeEndpoint_Start
                                 ? m_start : m_end;
    const std::size_t right = targetEndpoint == TextPatternRangeEndpoint_Start
                                  ? other->m_start : other->m_end;
    *value = left < right ? -1 : left > right ? 1 : 0;
    return S_OK;
}

HRESULT TextRangeProvider::ExpandToEnclosingUnit(const TextUnit unit) {
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_ELEMENTNOTAVAILABLE;
    Normalize(*entry);
    if (unit == TextUnit_Character) {
        const auto boundaries = Boundaries(*entry);
        auto end = std::upper_bound(boundaries.begin(), boundaries.end(), m_start);
        if (end == boundaries.end()) {
            m_start = m_end = boundaries.back();
        } else {
            m_end = *end;
            m_start = *std::prev(end);
        }
    } else if (unit == TextUnit_Line &&
               !entry->semantics.text->layout.Clusters().empty()) {
        const auto& clusters = entry->semantics.text->layout.Clusters();
        const auto found = std::find_if(
            clusters.begin(), clusters.end(), [this](const auto& cluster) {
                return m_start >= cluster.byteStart &&
                       m_start <= cluster.byteStart + cluster.byteLength;
            });
        if (found != clusters.end()) {
            m_start = found->byteStart;
            m_end = found->byteStart + found->byteLength;
            for (const auto& cluster : clusters)
                if (cluster.line == found->line) {
                    m_start = std::min(m_start, cluster.byteStart);
                    m_end = std::max(m_end,
                                     cluster.byteStart + cluster.byteLength);
                }
        }
    } else {
        m_start = 0;
        m_end = entry->semantics.text->layout.Text().size();
    }
    return S_OK;
}

HRESULT TextRangeProvider::FindAttribute(TEXTATTRIBUTEID, VARIANT, BOOL,
                                         ITextRangeProvider** result) {
    if (!result) return E_INVALIDARG;
    *result = nullptr;
    return S_OK;
}

HRESULT TextRangeProvider::FindText(BSTR value, const BOOL backward,
                                    const BOOL ignoreCase,
                                    ITextRangeProvider** result) {
    if (!result || !value) return E_INVALIDARG;
    *result = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_ELEMENTNOTAVAILABLE;
    Normalize(*entry);
    std::string haystack = entry->semantics.text->layout.Text().substr(
        m_start, m_end - m_start);
    std::string needle = Utf8(value);
    if (ignoreCase) {
        const auto lower = [](std::string& text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
        };
        lower(haystack);
        lower(needle);
    }
    const std::size_t found = backward ? haystack.rfind(needle)
                                       : haystack.find(needle);
    if (found != std::string::npos)
        *result = new TextRangeProvider(m_owner, m_id, m_start + found,
                                        m_start + found + needle.size());
    return S_OK;
}

HRESULT TextRangeProvider::GetAttributeValue(TEXTATTRIBUTEID, VARIANT* value) {
    if (!value) return E_INVALIDARG;
    VariantInit(value);
    return S_OK;
}

HRESULT TextRangeProvider::GetBoundingRectangles(SAFEARRAY** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_ELEMENTNOTAVAILABLE;
    Normalize(*entry);
    Rect bounds = entry->semantics.text->layout.BoundsForRange(
        m_start, m_end - m_start);
    if (m_start == m_end) {
        const Vec2 caret = entry->semantics.text->layout.CaretPosition(m_start);
        bounds = {caret.x, caret.y, 1.0f,
                  std::max(1.0f, entry->semantics.bounds.h)};
    }
    POINT origin{0, 0};
    (void)ClientToScreen(m_owner->Hwnd(), &origin);
    const double rectangle[4]{
        origin.x + entry->semantics.bounds.x +
            entry->semantics.text->origin.x + bounds.x,
        origin.y + entry->semantics.bounds.y +
            entry->semantics.text->origin.y + bounds.y,
        bounds.w, bounds.h};
    *value = SafeArrayCreateVector(VT_R8, 0, 4);
    if (!*value) return E_OUTOFMEMORY;
    for (LONG index = 0; index < 4; ++index)
        if (FAILED(SafeArrayPutElement(*value, &index,
                                      const_cast<double*>(&rectangle[index])))) {
            SafeArrayDestroy(*value);
            *value = nullptr;
            return E_OUTOFMEMORY;
        }
    return S_OK;
}

HRESULT TextRangeProvider::GetEnclosingElement(
    IRawElementProviderSimple** value) {
    if (!value) return E_INVALIDARG;
    *value = MakeProvider(m_owner, m_id);
    return *value ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT TextRangeProvider::GetText(const int maxLength, BSTR* value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_ELEMENTNOTAVAILABLE;
    Normalize(*entry);
    std::wstring text = Wide(entry->semantics.text->layout.Text().substr(
        m_start, m_end - m_start));
    if (maxLength >= 0 && static_cast<std::size_t>(maxLength) < text.size())
        text.resize(static_cast<std::size_t>(maxLength));
    *value = SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
    return *value || text.empty() ? S_OK : E_OUTOFMEMORY;
}

HRESULT TextRangeProvider::Move(const TextUnit unit, const int count,
                                int* moved) {
    if (!moved) return E_INVALIDARG;
    *moved = 0;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_ELEMENTNOTAVAILABLE;
    Normalize(*entry);
    if (count == 0) return S_OK;
    if (unit != TextUnit_Character) {
        const std::size_t destination = count < 0 ? 0
            : entry->semantics.text->layout.Text().size();
        if (m_start != destination || m_end != destination) {
            m_start = m_end = destination;
            *moved = count < 0 ? -1 : 1;
        }
        return S_OK;
    }
    const auto boundaries = Boundaries(*entry);
    const auto start = std::lower_bound(boundaries.begin(), boundaries.end(), m_start);
    const auto end = std::lower_bound(boundaries.begin(), boundaries.end(), m_end);
    const auto startIndex = static_cast<std::ptrdiff_t>(start - boundaries.begin());
    const auto endIndex = static_cast<std::ptrdiff_t>(end - boundaries.begin());
    const auto destination = std::clamp<std::ptrdiff_t>(
        startIndex + count, 0,
        static_cast<std::ptrdiff_t>(boundaries.size() - 1));
    const auto span = std::max<std::ptrdiff_t>(0, endIndex - startIndex);
    const auto destinationEnd = std::min<std::ptrdiff_t>(
        destination + span,
        static_cast<std::ptrdiff_t>(boundaries.size() - 1));
    m_start = boundaries[static_cast<std::size_t>(destination)];
    m_end = boundaries[static_cast<std::size_t>(destinationEnd)];
    *moved = static_cast<int>(destination - startIndex);
    return S_OK;
}

HRESULT TextRangeProvider::MoveEndpointByUnit(
    const TextPatternRangeEndpoint endpoint, const TextUnit unit,
    const int count, int* moved) {
    if (!moved) return E_INVALIDARG;
    *moved = 0;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_ELEMENTNOTAVAILABLE;
    Normalize(*entry);
    const auto boundaries = Boundaries(*entry);
    std::size_t& target = endpoint == TextPatternRangeEndpoint_Start
                              ? m_start : m_end;
    const auto current = std::lower_bound(boundaries.begin(), boundaries.end(), target);
    const auto index = static_cast<std::ptrdiff_t>(current - boundaries.begin());
    const auto destination = unit == TextUnit_Character
        ? std::clamp<std::ptrdiff_t>(index + count, 0,
              static_cast<std::ptrdiff_t>(boundaries.size() - 1))
        : (count < 0 ? 0
                     : static_cast<std::ptrdiff_t>(boundaries.size() - 1));
    target = boundaries[static_cast<std::size_t>(destination)];
    if (m_start > m_end) {
        if (endpoint == TextPatternRangeEndpoint_Start) m_end = m_start;
        else m_start = m_end;
    }
    *moved = unit == TextUnit_Character
                 ? static_cast<int>(destination - index)
                 : (destination == index ? 0 : count < 0 ? -1 : 1);
    return S_OK;
}

HRESULT TextRangeProvider::MoveEndpointByRange(
    const TextPatternRangeEndpoint endpoint, ITextRangeProvider* targetRange,
    const TextPatternRangeEndpoint targetEndpoint) {
    const auto* other = dynamic_cast<TextRangeProvider*>(targetRange);
    if (!other || other->m_owner.get() != m_owner.get() || other->m_id != m_id)
        return E_INVALIDARG;
    std::size_t& target = endpoint == TextPatternRangeEndpoint_Start
                              ? m_start : m_end;
    target = targetEndpoint == TextPatternRangeEndpoint_Start
                 ? other->m_start : other->m_end;
    if (m_start > m_end) {
        if (endpoint == TextPatternRangeEndpoint_Start) m_end = m_start;
        else m_start = m_end;
    }
    return S_OK;
}

HRESULT TextRangeProvider::Select() {
    return m_owner->Dispatch(m_id, "setSelection",
                             std::to_string(m_start) + ":" +
                                 std::to_string(m_end))
               ? S_OK : UIA_E_NOTSUPPORTED;
}

HRESULT TextRangeProvider::AddToSelection() { return Select(); }

HRESULT TextRangeProvider::RemoveFromSelection() {
    return m_owner->Dispatch(m_id, "setCaret", std::to_string(m_end))
               ? S_OK : UIA_E_NOTSUPPORTED;
}

HRESULT TextRangeProvider::ScrollIntoView(BOOL) {
    const auto entry = m_owner->Find(m_id);
    return entry && !entry->semantics.hidden ? S_OK : UIA_E_NOTSUPPORTED;
}

HRESULT TextRangeProvider::GetChildren(SAFEARRAY** value) {
    if (!value) return E_INVALIDARG;
    *value = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
    return *value ? S_OK : E_OUTOFMEMORY;
}

HRESULT Provider::QueryInterface(REFIID iid, void** output) {
    if (!output) return E_INVALIDARG;
    *output = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IRawElementProviderSimple))
        *output = static_cast<IRawElementProviderSimple*>(this);
    else if (iid == __uuidof(IRawElementProviderFragment))
        *output = static_cast<IRawElementProviderFragment*>(this);
    else if (iid == __uuidof(IRawElementProviderFragmentRoot) &&
             m_id == m_owner->Root())
        *output = static_cast<IRawElementProviderFragmentRoot*>(this);
    else if (iid == __uuidof(IInvokeProvider))
        *output = static_cast<IInvokeProvider*>(this);
    else if (iid == __uuidof(IToggleProvider))
        *output = static_cast<IToggleProvider*>(this);
    else if (iid == __uuidof(IRangeValueProvider))
        *output = static_cast<IRangeValueProvider*>(this);
    else if (iid == __uuidof(IValueProvider))
        *output = static_cast<IValueProvider*>(this);
    else if (iid == __uuidof(ITextProvider))
        *output = static_cast<ITextProvider*>(this);
    if (!*output) return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

HRESULT Provider::get_ProviderOptions(ProviderOptions* value) {
    if (!value) return E_INVALIDARG;
    *value = ProviderOptions_ServerSideProvider;
    return S_OK;
}

HRESULT Provider::GetPatternProvider(const PATTERNID id, IUnknown** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    if (id == UIA_InvokePatternId && Has(entry->semantics.actions, "activate"))
        *value = static_cast<IInvokeProvider*>(this);
    else if (id == UIA_TogglePatternId &&
             (entry->semantics.role == "checkbox" ||
              entry->semantics.role == "radio"))
        *value = static_cast<IToggleProvider*>(this);
    else if (id == UIA_RangeValuePatternId && entry->semantics.hasRange)
        *value = static_cast<IRangeValueProvider*>(this);
    else if (id == UIA_ValuePatternId && entry->semantics.role == "textbox")
        *value = static_cast<IValueProvider*>(this);
    else if (id == UIA_TextPatternId && entry->semantics.text)
        *value = static_cast<ITextProvider*>(this);
    if (*value) AddRef();
    return S_OK;
}

HRESULT Provider::GetPropertyValue(const PROPERTYID id, VARIANT* value) {
    if (!value) return E_INVALIDARG;
    VariantInit(value);
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    const auto setText = [value](const std::string& text) {
        value->vt = VT_BSTR;
        const std::wstring wide = Wide(text);
        value->bstrVal = SysAllocStringLen(wide.data(),
                                          static_cast<UINT>(wide.size()));
    };
    const auto setBool = [value](const bool state) {
        value->vt = VT_BOOL;
        value->boolVal = state ? VARIANT_TRUE : VARIANT_FALSE;
    };
    if (id == UIA_NamePropertyId) setText(entry->semantics.label);
    else if (id == UIA_HelpTextPropertyId) setText(entry->semantics.description);
    else if (id == UIA_AutomationIdPropertyId)
        setText(entry->semantics.id.ToString());
    else if (id == UIA_ControlTypePropertyId) {
        value->vt = VT_I4;
        value->lVal = ControlType(entry->semantics.role);
    } else if (id == UIA_IsEnabledPropertyId)
        setBool(!Has(entry->semantics.states, "disabled"));
    else if (id == UIA_IsKeyboardFocusablePropertyId)
        setBool(entry->semantics.focusable);
    else if (id == UIA_HasKeyboardFocusPropertyId)
        setBool(Has(entry->semantics.states, "focused"));
    else if (id == UIA_IsOffscreenPropertyId)
        setBool(entry->semantics.hidden);
    return S_OK;
}

HRESULT Provider::get_HostRawElementProvider(IRawElementProviderSimple** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    return m_id == m_owner->Root()
               ? UiaHostProviderFromHwnd(m_owner->Hwnd(), value)
               : S_OK;
}

HRESULT Provider::Navigate(const NavigateDirection direction,
                           IRawElementProviderFragment** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    std::optional<Uuid> target;
    if (direction == NavigateDirection_Parent) target = entry->parent;
    else if (direction == NavigateDirection_FirstChild && !entry->children.empty())
        target = entry->children.front();
    else if (direction == NavigateDirection_LastChild && !entry->children.empty())
        target = entry->children.back();
    else if ((direction == NavigateDirection_NextSibling ||
              direction == NavigateDirection_PreviousSibling) && entry->parent) {
        const auto parent = m_owner->Find(*entry->parent);
        if (parent) {
            const auto found = std::find(parent->children.begin(),
                                         parent->children.end(), m_id);
            if (found != parent->children.end()) {
                if (direction == NavigateDirection_NextSibling &&
                    std::next(found) != parent->children.end())
                    target = *std::next(found);
                if (direction == NavigateDirection_PreviousSibling &&
                    found != parent->children.begin())
                    target = *std::prev(found);
            }
        }
    }
    if (target) *value = MakeProvider(m_owner, *target);
    return S_OK;
}

HRESULT Provider::GetRuntimeId(SAFEARRAY** value) {
    if (!value) return E_INVALIDARG;
    *value = SafeArrayCreateVector(VT_I4, 0, 3);
    if (!*value) return E_OUTOFMEMORY;
    const std::string id = m_id.ToString();
    LONG values[3]{UiaAppendRuntimeId,
                   static_cast<LONG>(std::hash<std::string>{}(id)),
                   static_cast<LONG>(std::hash<std::string>{}("px:" + id))};
    for (LONG index = 0; index < 3; ++index)
        (void)SafeArrayPutElement(*value, &index, &values[index]);
    return S_OK;
}

HRESULT Provider::get_BoundingRectangle(UiaRect* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    POINT origin{0, 0};
    (void)ClientToScreen(m_owner->Hwnd(), &origin);
    value->left = origin.x + entry->semantics.bounds.x;
    value->top = origin.y + entry->semantics.bounds.y;
    value->width = entry->semantics.hidden ? 0.0 : entry->semantics.bounds.w;
    value->height = entry->semantics.hidden ? 0.0 : entry->semantics.bounds.h;
    return S_OK;
}

HRESULT Provider::GetEmbeddedFragmentRoots(SAFEARRAY** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    return S_OK;
}

HRESULT Provider::SetFocus() {
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    return entry->semantics.focusable && m_owner->Dispatch(m_id, "focus")
               ? S_OK
               : UIA_E_NOTSUPPORTED;
}

HRESULT Provider::get_FragmentRoot(IRawElementProviderFragmentRoot** value) {
    if (!value) return E_INVALIDARG;
    *value = MakeProvider(m_owner, m_owner->Root());
    return *value ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
}

HRESULT Provider::ElementProviderFromPoint(
    const double x, const double y, IRawElementProviderFragment** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    if (const auto id = m_owner->At(x, y)) *value = MakeProvider(m_owner, *id);
    return S_OK;
}

HRESULT Provider::GetFocus(IRawElementProviderFragment** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    if (const auto id = m_owner->Focused()) *value = MakeProvider(m_owner, *id);
    return S_OK;
}

HRESULT Provider::Invoke() {
    return m_owner->Dispatch(m_id, "activate") ? S_OK : UIA_E_NOTSUPPORTED;
}

HRESULT Provider::Toggle() { return Invoke(); }

HRESULT Provider::get_ToggleState(ToggleState* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    *value = Has(entry->semantics.states, "checked") ? ToggleState_On
                                                      : ToggleState_Off;
    return S_OK;
}

HRESULT Provider::SetValue(const double value) {
    return m_owner->Dispatch(m_id, "setValue", std::to_string(value))
               ? S_OK
               : E_INVALIDARG;
}

HRESULT Provider::get_Value(double* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    try { *value = std::stod(entry->semantics.value); }
    catch (...) { return UIA_E_NOTSUPPORTED; }
    return S_OK;
}

HRESULT Provider::get_IsReadOnly(BOOL* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    *value = entry->semantics.readOnly ? TRUE : FALSE;
    return S_OK;
}

HRESULT Provider::get_Maximum(double* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.hasRange) return UIA_E_NOTSUPPORTED;
    *value = entry->semantics.maximum;
    return S_OK;
}

HRESULT Provider::get_Minimum(double* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.hasRange) return UIA_E_NOTSUPPORTED;
    *value = entry->semantics.minimum;
    return S_OK;
}

HRESULT Provider::get_LargeChange(double* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.hasRange) return UIA_E_NOTSUPPORTED;
    *value = std::max(entry->semantics.step,
                      (entry->semantics.maximum - entry->semantics.minimum) / 10.0);
    return S_OK;
}

HRESULT Provider::get_SmallChange(double* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.hasRange) return UIA_E_NOTSUPPORTED;
    *value = entry->semantics.step;
    return S_OK;
}

HRESULT Provider::SetValue(const LPCWSTR value) {
    const auto entry = m_owner->Find(m_id);
    if (!entry || entry->semantics.readOnly) return UIA_E_NOTSUPPORTED;
    return m_owner->Dispatch(m_id, "setValue", Utf8(value)) ? S_OK
                                                              : E_INVALIDARG;
}

HRESULT Provider::get_Value(BSTR* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry) return UIA_E_ELEMENTNOTAVAILABLE;
    const std::wstring wide = Wide(entry->semantics.value);
    *value = SysAllocStringLen(wide.data(), static_cast<UINT>(wide.size()));
    return *value || wide.empty() ? S_OK : E_OUTOFMEMORY;
}

HRESULT Provider::GetSelection(SAFEARRAY** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_NOTSUPPORTED;
    const auto& text = *entry->semantics.text;
    *value = RangeArray({new TextRangeProvider(
        m_owner, m_id, text.selectionStartByteOffset,
        text.selectionEndByteOffset)});
    return *value ? S_OK : E_OUTOFMEMORY;
}

HRESULT Provider::GetVisibleRanges(SAFEARRAY** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_NOTSUPPORTED;
    *value = RangeArray({new TextRangeProvider(
        m_owner, m_id, 0, entry->semantics.text->layout.Text().size())});
    return *value ? S_OK : E_OUTOFMEMORY;
}

HRESULT Provider::RangeFromChild(IRawElementProviderSimple*,
                                 ITextRangeProvider** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    return UIA_E_NOTSUPPORTED;
}

HRESULT Provider::RangeFromPoint(const UiaPoint point,
                                 ITextRangeProvider** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_NOTSUPPORTED;
    POINT origin{0, 0};
    (void)ClientToScreen(m_owner->Hwnd(), &origin);
    const auto& text = *entry->semantics.text;
    const std::size_t offset = text.layout.ByteOffsetAt({
        static_cast<float>(point.x - origin.x - entry->semantics.bounds.x -
                           text.origin.x),
        static_cast<float>(point.y - origin.y - entry->semantics.bounds.y -
                           text.origin.y)});
    *value = new TextRangeProvider(m_owner, m_id, offset, offset);
    return S_OK;
}

HRESULT Provider::get_DocumentRange(ITextRangeProvider** value) {
    if (!value) return E_INVALIDARG;
    *value = nullptr;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_NOTSUPPORTED;
    *value = new TextRangeProvider(
        m_owner, m_id, 0, entry->semantics.text->layout.Text().size());
    return S_OK;
}

HRESULT Provider::get_SupportedTextSelection(
    SupportedTextSelection* value) {
    if (!value) return E_INVALIDARG;
    const auto entry = m_owner->Find(m_id);
    if (!entry || !entry->semantics.text) return UIA_E_NOTSUPPORTED;
    *value = SupportedTextSelection_Single;
    return S_OK;
}

}  // namespace

std::shared_ptr<SemanticAdapter> CreatePlatformSemanticAdapter(
    SDL_Window* window) {
    auto adapter = WindowsSemanticAdapter::Create(window);
    if (!adapter) PX_LOG_ERROR("Could not attach Windows UI Automation provider");
    return adapter;
}

std::string_view PlatformAccessibilityBackend() { return "Windows UI Automation"; }

}  // namespace px::accessibility

#elif !defined(__APPLE__) && !defined(__linux__)

namespace px::accessibility {
std::shared_ptr<SemanticAdapter> CreatePlatformSemanticAdapter(SDL_Window*) {
    return {};
}
std::string_view PlatformAccessibilityBackend() { return "unavailable"; }
}  // namespace px::accessibility

#endif
