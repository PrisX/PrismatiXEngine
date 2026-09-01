#include "Engine/Accessibility/PlatformSemanticAdapter.h"

#if defined(__linux__)

#include "Engine/Support/Logger.h"

#include <SDL3/SDL.h>
#include <dbus/dbus.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <locale.h>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace px::accessibility {
namespace {

constexpr char kRootPath[] = "/org/a11y/atspi/accessible/root";
constexpr char kNullPath[] = "/org/a11y/atspi/null";
constexpr char kAccessible[] = "org.a11y.atspi.Accessible";
constexpr char kApplication[] = "org.a11y.atspi.Application";
constexpr char kComponent[] = "org.a11y.atspi.Component";
constexpr char kAction[] = "org.a11y.atspi.Action";
constexpr char kValue[] = "org.a11y.atspi.Value";
constexpr char kText[] = "org.a11y.atspi.Text";
constexpr char kEditableText[] = "org.a11y.atspi.EditableText";

bool Has(const std::vector<std::string>& values, const std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

std::vector<std::size_t> CharacterByteOffsets(const std::string_view value) {
    std::vector<std::size_t> offsets;
    offsets.reserve(value.size() + 1);
    std::size_t byte = 0;
    while (byte < value.size()) {
        offsets.push_back(byte);
        const auto lead = static_cast<unsigned char>(value[byte]);
        std::size_t length = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2
                                      : (lead & 0xf0) == 0xe0 ? 3
                                      : (lead & 0xf8) == 0xf0 ? 4 : 1;
        if (byte + length > value.size()) length = 1;
        for (std::size_t index = 1; index < length; ++index)
            if ((static_cast<unsigned char>(value[byte + index]) & 0xc0) != 0x80) {
                length = 1;
                break;
            }
        byte += length;
    }
    offsets.push_back(value.size());
    return offsets;
}

std::int32_t CharacterOffsetForByte(const std::string_view value,
                                    const std::size_t byte) {
    const auto offsets = CharacterByteOffsets(value);
    const auto found = std::lower_bound(offsets.begin(), offsets.end(),
                                        std::min(byte, value.size()));
    return static_cast<std::int32_t>(std::distance(offsets.begin(), found));
}

std::int32_t CodepointAt(const std::string_view value,
                         const std::size_t byte) {
    if (byte >= value.size()) return -1;
    const auto lead = static_cast<unsigned char>(value[byte]);
    if (lead < 0x80) return lead;
    const std::size_t length = (lead & 0xe0) == 0xc0 ? 2
                               : (lead & 0xf0) == 0xe0 ? 3
                               : (lead & 0xf8) == 0xf0 ? 4 : 1;
    if (length == 1 || byte + length > value.size()) return -1;
    std::uint32_t result = lead & (length == 2 ? 0x1f : length == 3 ? 0x0f : 0x07);
    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = static_cast<unsigned char>(value[byte + index]);
        if ((continuation & 0xc0) != 0x80) return -1;
        result = (result << 6) | (continuation & 0x3f);
    }
    return static_cast<std::int32_t>(result);
}

std::uint32_t Role(const std::string_view role, const bool root) {
    if (root) return 75;  // ATSPI_ROLE_APPLICATION
    if (role == "button") return 43;
    if (role == "checkbox") return 7;
    if (role == "radio") return 44;
    if (role == "slider") return 51;
    if (role == "combobox") return 11;
    if (role == "textbox") return 79;
    if (role == "text") return 61;
    if (role == "heading") return 83;
    if (role == "image") return 27;
    if (role == "progressbar") return 42;
    if (role == "list") return 31;
    if (role == "listbox") return 98;
    if (role == "log") return 111;
    if (role == "dialog") return 16;
    if (role == "window") return 69;
    if (role == "group") return 99;
    if (role == "presentation") return 116;
    return 67;
}

std::string PathFor(const Uuid& id) {
    std::string suffix = id.ToString();
    std::replace(suffix.begin(), suffix.end(), '-', '_');
    return std::string("/org/a11y/atspi/accessible/node_") + suffix;
}

void AppendString(DBusMessageIter& iterator, const std::string_view value) {
    const std::string copy(value);
    const char* pointer = copy.c_str();
    dbus_message_iter_append_basic(&iterator, DBUS_TYPE_STRING, &pointer);
}

void AppendReference(DBusMessageIter& iterator, const std::string& bus,
                     const std::string& path) {
    DBusMessageIter tuple;
    dbus_message_iter_open_container(&iterator, DBUS_TYPE_STRUCT, nullptr,
                                     &tuple);
    const char* busPointer = bus.c_str();
    const char* pathPointer = path.c_str();
    dbus_message_iter_append_basic(&tuple, DBUS_TYPE_STRING, &busPointer);
    dbus_message_iter_append_basic(&tuple, DBUS_TYPE_OBJECT_PATH,
                                   &pathPointer);
    dbus_message_iter_close_container(&iterator, &tuple);
}

DBusMessage* EmptyReply(DBusMessage* request) {
    return dbus_message_new_method_return(request);
}

DBusMessage* Error(DBusMessage* request, const char* name,
                   const char* message) {
    return dbus_message_new_error(request, name, message);
}

template <typename T>
DBusMessage* BasicReply(DBusMessage* request, const int type, T value) {
    DBusMessage* reply = EmptyReply(request);
    if (!reply) return nullptr;
    if (!dbus_message_append_args(reply, type, &value, DBUS_TYPE_INVALID)) {
        dbus_message_unref(reply);
        return nullptr;
    }
    return reply;
}

DBusMessage* StringReply(DBusMessage* request, const std::string_view value) {
    const std::string copy(value);
    const char* pointer = copy.c_str();
    return BasicReply(request, DBUS_TYPE_STRING, pointer);
}

struct Entry {
    ui::AccessibilitySemantics semantics;
    std::optional<std::string> parent;
    std::vector<std::string> children;
    bool root = false;
};

class LinuxSemanticAdapter final
    : public SemanticAdapter,
      public std::enable_shared_from_this<LinuxSemanticAdapter> {
public:
    static std::shared_ptr<LinuxSemanticAdapter> Create(SDL_Window* window) {
        if (!window) return {};
        if (!dbus_threads_init_default()) return {};
        auto adapter = std::shared_ptr<LinuxSemanticAdapter>(
            new LinuxSemanticAdapter());
        adapter->m_thread = std::thread([adapter] { adapter->BusLoop(); });
        return adapter;
    }

    ~LinuxSemanticAdapter() override {
        m_running = false;
        m_wake.notify_all();
        DBusConnection* connection = nullptr;
        {
            std::scoped_lock lock(m_connectionMutex);
            connection = m_connection;
            if (connection) dbus_connection_ref(connection);
        }
        if (connection) {
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
        }
        if (m_thread.joinable()) m_thread.join();
    }

    void Publish(const SemanticTree& tree) override {
        std::unordered_map<std::string, Entry> candidate;
        Flatten(tree.root, std::nullopt, true, candidate);
        {
            std::scoped_lock lock(m_treeMutex);
            m_entries = std::move(candidate);
        }
        m_wake.notify_all();
    }

private:
    LinuxSemanticAdapter() = default;

    void Flatten(const SemanticNode& node,
                 const std::optional<std::string>& parent, const bool root,
                 std::unordered_map<std::string, Entry>& output) {
        const std::string path = root ? kRootPath : PathFor(node.semantics.id);
        Entry entry{node.semantics, parent, {}, root};
        entry.children.reserve(node.children.size());
        for (const auto& child : node.children)
            entry.children.push_back(PathFor(child.semantics.id));
        output.insert_or_assign(path, std::move(entry));
        for (const auto& child : node.children)
            Flatten(child, path, false, output);
    }

    std::optional<Entry> Find(const char* path) const {
        if (!path) return std::nullopt;
        std::scoped_lock lock(m_treeMutex);
        const auto found = m_entries.find(path);
        return found == m_entries.end()
                   ? std::nullopt
                   : std::optional<Entry>(found->second);
    }

    bool Dispatch(const Uuid& id, std::string action,
                  std::string value = {}) {
        struct Request {
            LinuxSemanticAdapter* owner;
            Uuid id;
            std::string action;
            std::string value;
            bool result = false;
        } request{this, id, std::move(action), std::move(value), false};
        if (!SDL_RunOnMainThread(
                [](void* data) {
                    auto& request = *static_cast<Request*>(data);
                    request.result = request.owner->InvokeAction(
                        request.id, request.action, request.value);
                },
                &request, true))
            return false;
        return request.result;
    }

    static DBusHandlerResult Message(DBusConnection* connection,
                                     DBusMessage* message, void* data) {
        auto* self = static_cast<LinuxSemanticAdapter*>(data);
        DBusMessage* reply = self ? self->Handle(message) : nullptr;
        if (!reply) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        dbus_connection_send(connection, reply, nullptr);
        dbus_message_unref(reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    DBusMessage* Handle(DBusMessage* request) {
        const char* path = dbus_message_get_path(request);
        const auto entry = Find(path);
        if (!entry)
            return Error(request, DBUS_ERROR_UNKNOWN_OBJECT,
                         "Accessible object is no longer available");
        const char* interface = dbus_message_get_interface(request);
        const char* member = dbus_message_get_member(request);
        if (!interface || !member) return nullptr;

        if (std::string_view(interface) == DBUS_INTERFACE_PROPERTIES)
            return HandleProperties(request, *entry);
        if (std::string_view(interface) == DBUS_INTERFACE_INTROSPECTABLE &&
            std::string_view(member) == "Introspect")
            return StringReply(request, Introspection());
        if (std::string_view(interface) == kAccessible)
            return HandleAccessible(request, *entry);
        if (std::string_view(interface) == kApplication && entry->root)
            return HandleApplication(request);
        if (std::string_view(interface) == kComponent)
            return HandleComponent(request, *entry);
        if (std::string_view(interface) == kAction)
            return HandleAction(request, *entry);
        if (std::string_view(interface) == kText && entry->semantics.text)
            return HandleText(request, *entry);
        if (std::string_view(interface) == kEditableText &&
            entry->semantics.text && entry->semantics.text->editable)
            return HandleEditableText(request, *entry);
        return Error(request, DBUS_ERROR_UNKNOWN_METHOD,
                     "Unsupported AT-SPI method");
    }

    DBusMessage* HandleProperties(DBusMessage* request, const Entry& entry) {
        const char* member = dbus_message_get_member(request);
        if (std::string_view(member) == "Set") {
            const char* interface = nullptr;
            const char* property = nullptr;
            DBusMessageIter iterator;
            if (!dbus_message_iter_init(request, &iterator) ||
                dbus_message_iter_get_arg_type(&iterator) != DBUS_TYPE_STRING)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Property interface is required");
            dbus_message_iter_get_basic(&iterator, &interface);
            dbus_message_iter_next(&iterator);
            dbus_message_iter_get_basic(&iterator, &property);
            dbus_message_iter_next(&iterator);
            if (std::string_view(interface) == kApplication && entry.root &&
                std::string_view(property) == "Id") {
                DBusMessageIter variant;
                dbus_message_iter_recurse(&iterator, &variant);
                dbus_message_iter_get_basic(&variant, &m_applicationId);
                return EmptyReply(request);
            }
            if (std::string_view(interface) == kValue &&
                std::string_view(property) == "CurrentValue" &&
                entry.semantics.hasRange && !entry.semantics.readOnly) {
                DBusMessageIter variant;
                double value = 0.0;
                dbus_message_iter_recurse(&iterator, &variant);
                dbus_message_iter_get_basic(&variant, &value);
                return Dispatch(entry.semantics.id, "setValue",
                                std::to_string(value))
                           ? EmptyReply(request)
                           : Error(request, DBUS_ERROR_FAILED,
                                   "Engine rejected the value");
            }
            return Error(request, DBUS_ERROR_PROPERTY_READ_ONLY,
                         "AT-SPI property is read-only");
        }
        if (std::string_view(member) == "GetAll") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            DBusMessageIter dictionary;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                             "{sv}", &dictionary);
            dbus_message_iter_close_container(&iterator, &dictionary);
            return reply;
        }
        if (std::string_view(member) != "Get") return nullptr;
        const char* requestedInterface = nullptr;
        const char* property = nullptr;
        if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_STRING,
                                   &requestedInterface, DBUS_TYPE_STRING,
                                   &property, DBUS_TYPE_INVALID))
            return Error(request, DBUS_ERROR_INVALID_ARGS,
                         "Get requires interface and property");
        return PropertyReply(request, requestedInterface, property, entry);
    }

    DBusMessage* PropertyReply(DBusMessage* request, const std::string_view iface,
                               const std::string_view property,
                               const Entry& entry) {
        DBusMessage* reply = EmptyReply(request);
        if (!reply) return nullptr;
        DBusMessageIter iterator;
        DBusMessageIter variant;
        dbus_message_iter_init_append(reply, &iterator);
        const auto stringVariant = [&](const std::string_view value) {
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_VARIANT, "s",
                                             &variant);
            AppendString(variant, value);
            dbus_message_iter_close_container(&iterator, &variant);
        };
        const auto uintVariant = [&](const std::uint32_t value) {
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_VARIANT, "u",
                                             &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_UINT32, &value);
            dbus_message_iter_close_container(&iterator, &variant);
        };
        const auto intVariant = [&](const std::int32_t value) {
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_VARIANT, "i",
                                             &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &value);
            dbus_message_iter_close_container(&iterator, &variant);
        };
        const auto doubleVariant = [&](const double value) {
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_VARIANT, "d",
                                             &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_DOUBLE, &value);
            dbus_message_iter_close_container(&iterator, &variant);
        };
        bool known = true;
        if (iface == kAccessible && property == "version") uintVariant(4);
        else if (iface == kAccessible && property == "Name")
            stringVariant(entry.root ? "PrismatiX Player" : entry.semantics.label);
        else if (iface == kAccessible && property == "Description")
            stringVariant(entry.semantics.description);
        else if (iface == kAccessible && property == "Locale") {
            const char* locale = setlocale(LC_MESSAGES, nullptr);
            stringVariant(locale ? locale : "C");
        } else if (iface == kAccessible && property == "AccessibleId")
            stringVariant(entry.semantics.id.ToString());
        else if (iface == kAccessible && property == "HelpText")
            stringVariant(entry.semantics.description);
        else if (iface == kAccessible && property == "ChildCount")
            intVariant(static_cast<std::int32_t>(entry.children.size()));
        else if (iface == kAccessible && property == "Parent") {
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_VARIANT,
                                             "(so)", &variant);
            if (entry.parent)
                AppendReference(variant, m_busName, *entry.parent);
            else
                AppendReference(variant, m_registryBus,
                                m_registryPath.empty() ? kNullPath
                                                       : m_registryPath);
            dbus_message_iter_close_container(&iterator, &variant);
        } else if (iface == kApplication && property == "ToolkitName")
            stringVariant("PrismatiX");
        else if (iface == kApplication &&
                 (property == "Version" || property == "ToolkitVersion"))
            stringVariant("0.2.0");
        else if (iface == kApplication && property == "AtspiVersion")
            stringVariant("2.1");
        else if (iface == kApplication && property == "InterfaceVersion")
            uintVariant(1);
        else if (iface == kApplication && property == "Id")
            intVariant(m_applicationId);
        else if (iface == kComponent && property == "version") uintVariant(1);
        else if (iface == kAction && property == "version") uintVariant(1);
        else if (iface == kAction && property == "NActions")
            intVariant(static_cast<std::int32_t>(entry.semantics.actions.size()));
        else if (iface == kValue && property == "version") uintVariant(1);
        else if (iface == kValue && property == "MinimumValue")
            doubleVariant(entry.semantics.minimum);
        else if (iface == kValue && property == "MaximumValue")
            doubleVariant(entry.semantics.maximum);
        else if (iface == kValue && property == "MinimumIncrement")
            doubleVariant(entry.semantics.step);
        else if (iface == kValue && property == "CurrentValue") {
            double value = 0.0;
            (void)std::from_chars(entry.semantics.value.data(),
                                  entry.semantics.value.data() +
                                      entry.semantics.value.size(),
                                  value);
            doubleVariant(value);
        } else if (iface == kValue && property == "Text")
            stringVariant(entry.semantics.value);
        else if (iface == kText && property == "version") uintVariant(4);
        else if (iface == kText && property == "CharacterCount") {
            const auto count = entry.semantics.text
                ? static_cast<std::int32_t>(CharacterByteOffsets(
                      entry.semantics.text->layout.Text()).size() - 1)
                : 0;
            intVariant(count);
        } else if (iface == kText && property == "CaretOffset") {
            const auto caret = entry.semantics.text
                ? CharacterOffsetForByte(entry.semantics.text->layout.Text(),
                                         entry.semantics.text->caretByteOffset)
                : 0;
            intVariant(caret);
        } else if (iface == kEditableText && property == "version")
            uintVariant(1);
        else known = false;
        if (!known) {
            dbus_message_unref(reply);
            return Error(request, DBUS_ERROR_UNKNOWN_PROPERTY,
                         "Unknown AT-SPI property");
        }
        return reply;
    }

    DBusMessage* HandleAccessible(DBusMessage* request, const Entry& entry) {
        const std::string_view member(dbus_message_get_member(request));
        if (member == "GetChildAtIndex") {
            std::int32_t index = -1;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &index, DBUS_TYPE_INVALID) ||
                index < 0 || static_cast<std::size_t>(index) >=
                                 entry.children.size())
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Child index is out of range");
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            dbus_message_iter_init_append(reply, &iterator);
            AppendReference(iterator, m_busName, entry.children[index]);
            return reply;
        }
        if (member == "GetChildren") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            DBusMessageIter array;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                             "(so)", &array);
            for (const auto& child : entry.children)
                AppendReference(array, m_busName, child);
            dbus_message_iter_close_container(&iterator, &array);
            return reply;
        }
        if (member == "GetIndexInParent") {
            std::int32_t index = -1;
            if (entry.parent) {
                const auto parent = Find(entry.parent->c_str());
                if (parent) {
                    const auto found = std::find(parent->children.begin(),
                                                 parent->children.end(),
                                                 dbus_message_get_path(request));
                    if (found != parent->children.end())
                        index = static_cast<std::int32_t>(
                            std::distance(parent->children.begin(), found));
                }
            }
            return BasicReply(request, DBUS_TYPE_INT32, index);
        }
        if (member == "GetRole") {
            const std::uint32_t role = Role(entry.semantics.role, entry.root);
            return BasicReply(request, DBUS_TYPE_UINT32, role);
        }
        if (member == "GetRoleName" || member == "GetLocalizedRoleName")
            return StringReply(request,
                               entry.root ? "application" : entry.semantics.role);
        if (member == "GetRelationSet") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            DBusMessageIter array;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                             "(ua(so))", &array);
            dbus_message_iter_close_container(&iterator, &array);
            return reply;
        }
        if (member == "GetState") return StateReply(request, entry);
        if (member == "GetAttributes") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            DBusMessageIter array;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                             "{ss}", &array);
            dbus_message_iter_close_container(&iterator, &array);
            return reply;
        }
        if (member == "GetApplication") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            dbus_message_iter_init_append(reply, &iterator);
            AppendReference(iterator, m_busName, kRootPath);
            return reply;
        }
        if (member == "GetInterfaces") {
            std::vector<std::string> interfaces{kAccessible, kComponent};
            if (entry.root) interfaces.emplace_back(kApplication);
            if (!entry.semantics.actions.empty()) interfaces.emplace_back(kAction);
            if (entry.semantics.hasRange) interfaces.emplace_back(kValue);
            if (entry.semantics.text) interfaces.emplace_back(kText);
            if (entry.semantics.text && entry.semantics.text->editable)
                interfaces.emplace_back(kEditableText);
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            DBusMessageIter array;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY, "s",
                                             &array);
            for (const auto& name : interfaces) AppendString(array, name);
            dbus_message_iter_close_container(&iterator, &array);
            return reply;
        }
        return nullptr;
    }

    DBusMessage* StateReply(DBusMessage* request, const Entry& entry) {
        std::vector<std::uint32_t> states;
        const bool disabled = Has(entry.semantics.states, "disabled");
        if (!disabled) {
            states.push_back(8);   // enabled
            states.push_back(24);  // sensitive
        }
        if (!entry.semantics.hidden) {
            states.push_back(25);  // showing
            states.push_back(30);  // visible
        }
        if (entry.semantics.focusable) states.push_back(11);
        if (Has(entry.semantics.states, "focused")) states.push_back(12);
        if (Has(entry.semantics.states, "checked")) states.push_back(4);
        if (entry.semantics.role == "checkbox" ||
            entry.semantics.role == "radio") states.push_back(41);
        if (entry.semantics.role == "textbox")
            states.push_back(entry.semantics.readOnly ? 43 : 7);
        DBusMessage* reply = EmptyReply(request);
        DBusMessageIter iterator;
        DBusMessageIter array;
        dbus_message_iter_init_append(reply, &iterator);
        dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY, "u",
                                         &array);
        for (const auto state : states)
            dbus_message_iter_append_basic(&array, DBUS_TYPE_UINT32, &state);
        dbus_message_iter_close_container(&iterator, &array);
        return reply;
    }

    DBusMessage* HandleApplication(DBusMessage* request) {
        const std::string_view member(dbus_message_get_member(request));
        if (member == "GetLocale") {
            const char* locale = setlocale(LC_MESSAGES, nullptr);
            return StringReply(request, locale ? locale : "C");
        }
        if (member == "GetApplicationBusAddress")
            return StringReply(request, "");
        return nullptr;
    }

    DBusMessage* HandleComponent(DBusMessage* request, const Entry& entry) {
        const std::string_view member(dbus_message_get_member(request));
        const auto x = static_cast<std::int32_t>(entry.semantics.bounds.x);
        const auto y = static_cast<std::int32_t>(entry.semantics.bounds.y);
        const auto width = static_cast<std::int32_t>(entry.semantics.bounds.w);
        const auto height = static_cast<std::int32_t>(entry.semantics.bounds.h);
        if (member == "Contains") {
            std::int32_t pointX = 0;
            std::int32_t pointY = 0;
            std::uint32_t coordinates = 0;
            dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32, &pointX,
                                  DBUS_TYPE_INT32, &pointY, DBUS_TYPE_UINT32,
                                  &coordinates, DBUS_TYPE_INVALID);
            (void)coordinates;
            dbus_bool_t contained = pointX >= x && pointY >= y &&
                                    pointX <= x + width &&
                                    pointY <= y + height;
            return BasicReply(request, DBUS_TYPE_BOOLEAN, contained);
        }
        if (member == "GetExtents") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            DBusMessageIter tuple;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_STRUCT,
                                             nullptr, &tuple);
            dbus_message_iter_append_basic(&tuple, DBUS_TYPE_INT32, &x);
            dbus_message_iter_append_basic(&tuple, DBUS_TYPE_INT32, &y);
            dbus_message_iter_append_basic(&tuple, DBUS_TYPE_INT32, &width);
            dbus_message_iter_append_basic(&tuple, DBUS_TYPE_INT32, &height);
            dbus_message_iter_close_container(&iterator, &tuple);
            return reply;
        }
        if (member == "GetPosition") {
            DBusMessage* reply = EmptyReply(request);
            dbus_message_append_args(reply, DBUS_TYPE_INT32, &x,
                                     DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);
            return reply;
        }
        if (member == "GetSize") {
            DBusMessage* reply = EmptyReply(request);
            dbus_message_append_args(reply, DBUS_TYPE_INT32, &width,
                                     DBUS_TYPE_INT32, &height,
                                     DBUS_TYPE_INVALID);
            return reply;
        }
        if (member == "GetLayer") {
            const std::uint32_t layer = entry.root ? 7 : 3;
            return BasicReply(request, DBUS_TYPE_UINT32, layer);
        }
        if (member == "GetMDIZOrder") {
            const std::int16_t order = -1;
            return BasicReply(request, DBUS_TYPE_INT16, order);
        }
        if (member == "GrabFocus") {
            dbus_bool_t focused = entry.semantics.focusable &&
                                  Dispatch(entry.semantics.id, "focus");
            return BasicReply(request, DBUS_TYPE_BOOLEAN, focused);
        }
        if (member == "GetAlpha") {
            const double alpha = entry.semantics.hidden ? 0.0 : 1.0;
            return BasicReply(request, DBUS_TYPE_DOUBLE, alpha);
        }
        return nullptr;
    }

    DBusMessage* HandleText(DBusMessage* request, const Entry& entry) {
        const auto& text = *entry.semantics.text;
        const std::string& content = text.layout.Text();
        const auto offsets = CharacterByteOffsets(content);
        const auto count = static_cast<std::int32_t>(offsets.size() - 1);
        const std::string_view member(dbus_message_get_member(request));
        const auto rangeReply = [&](const std::string_view value,
                                    const std::int32_t start,
                                    const std::int32_t end) {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            dbus_message_iter_init_append(reply, &iterator);
            AppendString(iterator, value);
            dbus_message_iter_append_basic(&iterator, DBUS_TYPE_INT32, &start);
            dbus_message_iter_append_basic(&iterator, DBUS_TYPE_INT32, &end);
            return reply;
        };
        const auto selectionValue = [](const std::size_t start,
                                       const std::size_t end) {
            return std::to_string(start) + ":" + std::to_string(end);
        };
        const auto parseRange = [&](std::int32_t& start, std::int32_t& end) {
            return dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                         &start, DBUS_TYPE_INT32, &end,
                                         DBUS_TYPE_INVALID) &&
                   start >= 0 && end >= start && end <= count;
        };
        const auto boundsForBytes = [&](const std::size_t start,
                                        const std::size_t end) {
            Rect bounds = text.layout.BoundsForRange(start, end - start);
            if (bounds.w <= 0.0f || bounds.h <= 0.0f)
                bounds = {0, 0, entry.semantics.bounds.w,
                          entry.semantics.bounds.h};
            bounds.x += entry.semantics.bounds.x + text.origin.x;
            bounds.y += entry.semantics.bounds.y + text.origin.y;
            return bounds;
        };
        const auto extentsReply = [&](const Rect bounds) {
            const auto x = static_cast<std::int32_t>(bounds.x);
            const auto y = static_cast<std::int32_t>(bounds.y);
            const auto width = static_cast<std::int32_t>(bounds.w);
            const auto height = static_cast<std::int32_t>(bounds.h);
            DBusMessage* reply = EmptyReply(request);
            dbus_message_append_args(reply, DBUS_TYPE_INT32, &x,
                                     DBUS_TYPE_INT32, &y,
                                     DBUS_TYPE_INT32, &width,
                                     DBUS_TYPE_INT32, &height,
                                     DBUS_TYPE_INVALID);
            return reply;
        };

        if (member == "GetText") {
            std::int32_t start = 0, end = 0;
            if (!parseRange(start, end))
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Text range is out of bounds");
            return StringReply(request, content.substr(offsets[start],
                offsets[end] - offsets[start]));
        }
        if (member == "SetCaretOffset") {
            std::int32_t offset = -1;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &offset, DBUS_TYPE_INVALID) ||
                offset < 0 || offset > count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Caret offset is out of bounds");
            const dbus_bool_t result = Dispatch(
                entry.semantics.id, "setCaret", std::to_string(offsets[offset]));
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        if (member == "GetCharacterAtOffset") {
            std::int32_t offset = -1;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &offset, DBUS_TYPE_INVALID) ||
                offset < 0 || offset >= count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Character offset is out of bounds");
            const std::int32_t codepoint = CodepointAt(content, offsets[offset]);
            return BasicReply(request, DBUS_TYPE_INT32, codepoint);
        }
        if (member == "GetNSelections") {
            const std::int32_t selections =
                text.selectionStartByteOffset == text.selectionEndByteOffset ? 0 : 1;
            return BasicReply(request, DBUS_TYPE_INT32, selections);
        }
        if (member == "GetSelection") {
            std::int32_t selection = -1;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &selection, DBUS_TYPE_INVALID) ||
                selection != 0 ||
                text.selectionStartByteOffset == text.selectionEndByteOffset)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Selection index is out of bounds");
            const std::int32_t start = CharacterOffsetForByte(
                content, text.selectionStartByteOffset);
            const std::int32_t end = CharacterOffsetForByte(
                content, text.selectionEndByteOffset);
            DBusMessage* reply = EmptyReply(request);
            dbus_message_append_args(reply, DBUS_TYPE_INT32, &start,
                                     DBUS_TYPE_INT32, &end,
                                     DBUS_TYPE_INVALID);
            return reply;
        }
        if (member == "AddSelection" || member == "SetSelection") {
            std::int32_t selection = 0, start = 0, end = 0;
            const bool set = member == "SetSelection";
            const bool valid = set
                ? dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                        &selection, DBUS_TYPE_INT32, &start,
                                        DBUS_TYPE_INT32, &end, DBUS_TYPE_INVALID)
                : dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                        &start, DBUS_TYPE_INT32, &end,
                                        DBUS_TYPE_INVALID);
            if (!valid || selection != 0 || start < 0 || end < start ||
                end > count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Selection range is out of bounds");
            const dbus_bool_t result = Dispatch(
                entry.semantics.id, "setSelection",
                selectionValue(offsets[start], offsets[end]));
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        if (member == "RemoveSelection") {
            std::int32_t selection = -1;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &selection, DBUS_TYPE_INVALID) ||
                selection != 0)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Selection index is out of bounds");
            const dbus_bool_t result = Dispatch(
                entry.semantics.id, "setCaret",
                std::to_string(text.selectionEndByteOffset));
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        if (member == "GetStringAtOffset" || member == "GetTextAtOffset" ||
            member == "GetTextBeforeOffset" ||
            member == "GetTextAfterOffset") {
            std::int32_t offset = -1;
            std::uint32_t granularity = 0;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &offset, DBUS_TYPE_UINT32, &granularity,
                                       DBUS_TYPE_INVALID) ||
                offset < 0 || offset > count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Text offset is out of bounds");
            std::int32_t start = offset;
            std::int32_t end = std::min(count, offset + 1);
            if (granularity != 0) {
                start = 0;
                end = count;
            }
            if (member == "GetTextBeforeOffset") end = start, start = 0;
            if (member == "GetTextAfterOffset") start = end, end = count;
            return rangeReply(content.substr(offsets[start],
                                             offsets[end] - offsets[start]),
                              start, end);
        }
        if (member == "GetCharacterExtents") {
            std::int32_t offset = -1;
            std::uint32_t coordinates = 0;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &offset, DBUS_TYPE_UINT32, &coordinates,
                                       DBUS_TYPE_INVALID) ||
                offset < 0 || offset >= count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Character offset is out of bounds");
            (void)coordinates;
            return extentsReply(boundsForBytes(offsets[offset],
                                               offsets[offset + 1]));
        }
        if (member == "GetRangeExtents") {
            std::int32_t start = 0, end = 0;
            std::uint32_t coordinates = 0;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &start, DBUS_TYPE_INT32, &end,
                                       DBUS_TYPE_UINT32, &coordinates,
                                       DBUS_TYPE_INVALID) ||
                start < 0 || end < start || end > count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Text range is out of bounds");
            (void)coordinates;
            return extentsReply(boundsForBytes(offsets[start], offsets[end]));
        }
        if (member == "GetOffsetAtPoint") {
            std::int32_t x = 0, y = 0;
            std::uint32_t coordinates = 0;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32, &x,
                                       DBUS_TYPE_INT32, &y,
                                       DBUS_TYPE_UINT32, &coordinates,
                                       DBUS_TYPE_INVALID))
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Point and coordinate type are required");
            (void)coordinates;
            const std::size_t byte = text.layout.ByteOffsetAt({
                static_cast<float>(x) - entry.semantics.bounds.x - text.origin.x,
                static_cast<float>(y) - entry.semantics.bounds.y - text.origin.y});
            const std::int32_t offset = CharacterOffsetForByte(content, byte);
            return BasicReply(request, DBUS_TYPE_INT32, offset);
        }
        if (member == "GetAttributes" || member == "GetAttributeRun") {
            std::int32_t offset = 0;
            dbus_bool_t includeDefaults = false;
            const bool valid = member == "GetAttributeRun"
                ? dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                        &offset, DBUS_TYPE_BOOLEAN,
                                        &includeDefaults, DBUS_TYPE_INVALID)
                : dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                        &offset, DBUS_TYPE_INVALID);
            (void)includeDefaults;
            if (!valid || offset < 0 || offset > count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Attribute offset is out of bounds");
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator, dictionary;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                             "{ss}", &dictionary);
            dbus_message_iter_close_container(&iterator, &dictionary);
            const std::int32_t start = 0, end = count;
            dbus_message_iter_append_basic(&iterator, DBUS_TYPE_INT32, &start);
            dbus_message_iter_append_basic(&iterator, DBUS_TYPE_INT32, &end);
            return reply;
        }
        if (member == "GetDefaultAttributes" ||
            member == "GetDefaultAttributeSet") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator, dictionary;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                             "{ss}", &dictionary);
            dbus_message_iter_close_container(&iterator, &dictionary);
            return reply;
        }
        if (member == "GetAttributeValue") return StringReply(request, "");
        if (member == "ScrollSubstringTo" ||
            member == "ScrollSubstringToPoint") {
            const dbus_bool_t visible = !entry.semantics.hidden;
            return BasicReply(request, DBUS_TYPE_BOOLEAN, visible);
        }
        return nullptr;
    }

    DBusMessage* HandleEditableText(DBusMessage* request, const Entry& entry) {
        const auto& text = *entry.semantics.text;
        const std::string& content = text.layout.Text();
        const auto offsets = CharacterByteOffsets(content);
        const auto count = static_cast<std::int32_t>(offsets.size() - 1);
        const std::string_view member(dbus_message_get_member(request));
        if (member == "SetTextContents") {
            const char* value = nullptr;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_STRING,
                                       &value, DBUS_TYPE_INVALID))
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Text content is required");
            const dbus_bool_t result = Dispatch(entry.semantics.id, "setValue",
                                                value ? value : "");
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        if (member == "InsertText") {
            std::int32_t position = -1, length = -1;
            const char* inserted = nullptr;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &position, DBUS_TYPE_STRING, &inserted,
                                       DBUS_TYPE_INT32, &length,
                                       DBUS_TYPE_INVALID) ||
                position < 0 || position > count || length < 0)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Insert range is out of bounds");
            const std::string_view supplied = inserted ? inserted : "";
            const auto insertedOffsets = CharacterByteOffsets(supplied);
            const auto limit = std::upper_bound(insertedOffsets.begin(),
                                                insertedOffsets.end(),
                                                static_cast<std::size_t>(length));
            const std::size_t bytes = limit == insertedOffsets.begin()
                                          ? 0 : *std::prev(limit);
            std::string candidate = content;
            candidate.insert(offsets[position], supplied.substr(0, bytes));
            const dbus_bool_t result = Dispatch(entry.semantics.id, "setValue",
                                                std::move(candidate));
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        if (member == "DeleteText" || member == "CutText" ||
            member == "CopyText") {
            std::int32_t start = 0, end = 0;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &start, DBUS_TYPE_INT32, &end,
                                       DBUS_TYPE_INVALID) ||
                start < 0 || end < start || end > count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Editable text range is out of bounds");
            if (member == "CopyText" || member == "CutText") {
                if (!Dispatch(entry.semantics.id, "copyText",
                              std::to_string(offsets[start]) + ":" +
                                  std::to_string(offsets[end])))
                    return Error(request, DBUS_ERROR_FAILED,
                                 "Clipboard copy failed");
                if (member == "CopyText") return EmptyReply(request);
            }
            std::string candidate = content;
            candidate.erase(offsets[start], offsets[end] - offsets[start]);
            const dbus_bool_t result = Dispatch(entry.semantics.id, "setValue",
                                                std::move(candidate));
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        if (member == "PasteText") {
            std::int32_t position = -1;
            if (!dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32,
                                       &position, DBUS_TYPE_INVALID) ||
                position < 0 || position > count)
                return Error(request, DBUS_ERROR_INVALID_ARGS,
                             "Paste offset is out of bounds");
            const dbus_bool_t result = Dispatch(
                entry.semantics.id, "pasteText", std::to_string(offsets[position]));
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        return nullptr;
    }

    DBusMessage* HandleAction(DBusMessage* request, const Entry& entry) {
        const std::string_view member(dbus_message_get_member(request));
        std::int32_t index = -1;
        if (member != "GetActions" &&
            !dbus_message_get_args(request, nullptr, DBUS_TYPE_INT32, &index,
                                   DBUS_TYPE_INVALID))
            return Error(request, DBUS_ERROR_INVALID_ARGS,
                         "Action index is required");
        if (member != "GetActions" &&
            (index < 0 || static_cast<std::size_t>(index) >=
                              entry.semantics.actions.size()))
            return Error(request, DBUS_ERROR_INVALID_ARGS,
                         "Action index is out of range");
        if (member == "DoAction") {
            dbus_bool_t result = Dispatch(
                entry.semantics.id, entry.semantics.actions[index]);
            return BasicReply(request, DBUS_TYPE_BOOLEAN, result);
        }
        if (member == "GetName" || member == "GetLocalizedName")
            return StringReply(request, entry.semantics.actions[index]);
        if (member == "GetDescription")
            return StringReply(request, entry.semantics.description);
        if (member == "GetKeyBinding") return StringReply(request, "");
        if (member == "GetActions") {
            DBusMessage* reply = EmptyReply(request);
            DBusMessageIter iterator;
            DBusMessageIter array;
            dbus_message_iter_init_append(reply, &iterator);
            dbus_message_iter_open_container(&iterator, DBUS_TYPE_ARRAY,
                                             "(sss)", &array);
            for (const auto& action : entry.semantics.actions) {
                DBusMessageIter tuple;
                dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT,
                                                 nullptr, &tuple);
                AppendString(tuple, action);
                AppendString(tuple, entry.semantics.description);
                AppendString(tuple, "");
                dbus_message_iter_close_container(&array, &tuple);
            }
            dbus_message_iter_close_container(&iterator, &array);
            return reply;
        }
        return nullptr;
    }

    static std::string_view Introspection() {
        return R"xml(<!DOCTYPE node PUBLIC "-//freedesktop//DTD D-BUS Object Introspection 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd"><node><interface name="org.a11y.atspi.Accessible"/><interface name="org.a11y.atspi.Application"/><interface name="org.a11y.atspi.Component"/><interface name="org.a11y.atspi.Action"/><interface name="org.a11y.atspi.Value"/><interface name="org.a11y.atspi.Text"/><interface name="org.a11y.atspi.EditableText"/><interface name="org.freedesktop.DBus.Properties"/><interface name="org.freedesktop.DBus.Introspectable"/></node>)xml";
    }

    static std::optional<std::string> AccessibilityBusAddress() {
        DBusError error;
        dbus_error_init(&error);
        DBusConnection* session = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
        if (!session) {
            dbus_error_free(&error);
            return std::nullopt;
        }
        dbus_connection_set_exit_on_disconnect(session, false);
        DBusMessage* call = dbus_message_new_method_call(
            "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
        DBusMessage* reply = call ? dbus_connection_send_with_reply_and_block(
                                        session, call, 2000, &error)
                                  : nullptr;
        if (call) dbus_message_unref(call);
        std::optional<std::string> address;
        const char* text = nullptr;
        if (reply && dbus_message_get_args(reply, &error, DBUS_TYPE_STRING,
                                           &text, DBUS_TYPE_INVALID) && text)
            address = text;
        if (reply) dbus_message_unref(reply);
        dbus_connection_close(session);
        dbus_connection_unref(session);
        dbus_error_free(&error);
        return address;
    }

    DBusConnection* Connect() {
        const auto address = AccessibilityBusAddress();
        if (!address) return nullptr;
        DBusError error;
        dbus_error_init(&error);
        DBusConnection* connection =
            dbus_connection_open_private(address->c_str(), &error);
        if (!connection || !dbus_bus_register(connection, &error)) {
            if (connection) {
                dbus_connection_close(connection);
                dbus_connection_unref(connection);
            }
            dbus_error_free(&error);
            return nullptr;
        }
        dbus_connection_set_exit_on_disconnect(connection, false);
        static const DBusObjectPathVTable vtable{
            nullptr, &LinuxSemanticAdapter::Message, nullptr, nullptr,
            nullptr, nullptr};
        if (!dbus_connection_register_fallback(
                connection, "/org/a11y/atspi/accessible", &vtable, this)) {
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
            return nullptr;
        }
        const char* unique = dbus_bus_get_unique_name(connection);
        m_busName = unique ? unique : "";
        RegisterWithRegistry(connection);
        return connection;
    }

    void RegisterWithRegistry(DBusConnection* connection) {
        if (m_busName.empty()) return;
        DBusMessage* call = dbus_message_new_method_call(
            "org.a11y.atspi.Registry", kRootPath, "org.a11y.atspi.Socket",
            "Embed");
        if (!call) return;
        DBusMessageIter iterator;
        dbus_message_iter_init_append(call, &iterator);
        AppendReference(iterator, m_busName, kRootPath);
        DBusError error;
        dbus_error_init(&error);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(
            connection, call, 3000, &error);
        dbus_message_unref(call);
        if (reply) {
            DBusMessageIter outer;
            DBusMessageIter tuple;
            const char* bus = nullptr;
            const char* path = nullptr;
            if (dbus_message_iter_init(reply, &outer) &&
                dbus_message_iter_get_arg_type(&outer) == DBUS_TYPE_STRUCT) {
                dbus_message_iter_recurse(&outer, &tuple);
                dbus_message_iter_get_basic(&tuple, &bus);
                dbus_message_iter_next(&tuple);
                dbus_message_iter_get_basic(&tuple, &path);
                if (bus) m_registryBus = bus;
                if (path) m_registryPath = path;
            }
            dbus_message_unref(reply);
        }
        dbus_error_free(&error);
    }

    void BusLoop() {
        while (m_running) {
            DBusConnection* connection = Connect();
            if (!connection) {
                std::unique_lock lock(m_wakeMutex);
                m_wake.wait_for(lock, std::chrono::seconds(2),
                                [this] { return !m_running.load(); });
                continue;
            }
            {
                std::scoped_lock lock(m_connectionMutex);
                m_connection = connection;
            }
            PX_LOG_INFO("Linux AT-SPI accessibility provider registered");
            while (m_running && dbus_connection_get_is_connected(connection))
                (void)dbus_connection_read_write_dispatch(connection, 250);
            {
                std::scoped_lock lock(m_connectionMutex);
                if (m_connection == connection) m_connection = nullptr;
            }
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
        }
    }

    std::atomic<bool> m_running{true};
    std::thread m_thread;
    mutable std::mutex m_treeMutex;
    std::unordered_map<std::string, Entry> m_entries;
    std::mutex m_connectionMutex;
    DBusConnection* m_connection = nullptr;
    std::mutex m_wakeMutex;
    std::condition_variable m_wake;
    std::string m_busName;
    std::string m_registryBus;
    std::string m_registryPath;
    std::int32_t m_applicationId = 0;
};

}  // namespace

std::shared_ptr<SemanticAdapter> CreatePlatformSemanticAdapter(
    SDL_Window* window) {
    auto adapter = LinuxSemanticAdapter::Create(window);
    if (!adapter) PX_LOG_ERROR("Could not start Linux AT-SPI provider");
    return adapter;
}

std::string_view PlatformAccessibilityBackend() { return "Linux AT-SPI"; }

}  // namespace px::accessibility

#endif
