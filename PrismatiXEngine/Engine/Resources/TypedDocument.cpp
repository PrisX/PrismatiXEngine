#include "Engine/Resources/TypedDocument.h"

#include <charconv>
#include <iomanip>
#include <sstream>

namespace px::resource {

namespace {
std::string Trim(std::string_view value) {
    const std::size_t a = value.find_first_not_of(" \t\r");
    if (a == std::string_view::npos) return {};
    const std::size_t b = value.find_last_not_of(" \t\r");
    return std::string(value.substr(a, b - a + 1));
}

diag::Diagnostic ParseError(const std::string& path, int line, std::string message,
                            std::string details = {}) {
    diag::Diagnostic d{ diag::Severity::Error, "PXRES-E1001", "resource-parser",
                        std::move(message), std::move(details) };
    d.source.path = path;
    d.source.line = line;
    return d;
}

std::string Unescape(std::string_view value, bool& ok) {
    ok = false;
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return {};
    std::string out;
    for (std::size_t i = 1; i + 1 < value.size(); ++i) {
        if (value[i] != '\\') {
            out.push_back(value[i]);
            continue;
        }
        if (++i + 1 > value.size()) return {};
        switch (value[i]) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            default: return {};
        }
    }
    ok = true;
    return out;
}

std::vector<std::string> Arguments(std::string_view body) {
    std::vector<std::string> out;
    if (Trim(body).empty()) return out;
    std::size_t start = 0;
    bool quoted = false;
    int depth = 0;
    for (std::size_t i = 0; i <= body.size(); ++i) {
        if (i < body.size() && body[i] == '"' && (i == 0 || body[i - 1] != '\\')) quoted = !quoted;
        if (!quoted && i < body.size()) {
            if (body[i] == '(' || body[i] == '[' || body[i] == '{') ++depth;
            else if (body[i] == ')' || body[i] == ']' || body[i] == '}') --depth;
        }
        if (i == body.size() || (!quoted && depth == 0 && body[i] == ',')) {
            out.push_back(Trim(body.substr(start, i - start)));
            start = i + 1;
        }
    }
    return out;
}

Result<Variant> ParseValue(std::string_view raw, const std::string& path, int line) {
    const std::string value = Trim(raw);
    if (value == "null") return Result<Variant>::Success(Variant{});
    if (value == "true") return Result<Variant>::Success(Variant(true));
    if (value == "false") return Result<Variant>::Success(Variant(false));
    if (!value.empty() && value.front() == '"') {
        bool ok = false;
        std::string text = Unescape(value, ok);
        return ok ? Result<Variant>::Success(Variant(std::move(text)))
                  : Result<Variant>::Failure(ParseError(path, line, "Invalid quoted string."));
    }
    auto typed = [&](std::string_view name) -> std::optional<std::vector<std::string>> {
        if (!value.starts_with(name) || value.size() <= name.size() + 1 ||
            value[name.size()] != '(' || value.back() != ')') return std::nullopt;
        return Arguments(std::string_view(value).substr(name.size() + 1,
                                                         value.size() - name.size() - 2));
    };
    auto numbers = [&](const std::vector<std::string>& args, std::vector<double>& result) {
        for (const std::string& arg : args) {
            double number = 0.0;
            const auto parsed = std::from_chars(arg.data(), arg.data() + arg.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != arg.data() + arg.size()) return false;
            result.push_back(number);
        }
        return true;
    };
    if (auto args = typed("vec2")) {
        std::vector<double> n;
        if (args->size() == 2 && numbers(*args, n))
            return Result<Variant>::Success(Variant(Vec2{ static_cast<float>(n[0]), static_cast<float>(n[1]) }));
    }
    if (auto args = typed("rect")) {
        std::vector<double> n;
        if (args->size() == 4 && numbers(*args, n))
            return Result<Variant>::Success(Variant(Rect{ static_cast<float>(n[0]), static_cast<float>(n[1]), static_cast<float>(n[2]), static_cast<float>(n[3]) }));
    }
    if (auto args = typed("color")) {
        std::vector<double> n;
        if ((args->size() == 3 || args->size() == 4) && numbers(*args, n)) {
            return Result<Variant>::Success(Variant(Color{ static_cast<std::uint8_t>(n[0]), static_cast<std::uint8_t>(n[1]), static_cast<std::uint8_t>(n[2]), static_cast<std::uint8_t>(args->size() == 4 ? n[3] : 255) }));
        }
    }
    if (auto args = typed("uuid")) {
        if (args->size() == 1) {
            bool ok = false;
            const std::string text = Unescape((*args)[0], ok);
            if (ok)
                if (auto id = Uuid::Parse(text)) return Result<Variant>::Success(Variant(*id));
        }
    }
    if (auto args = typed("res")) {
        if (args->size() == 2) {
            bool a = false, b = false;
            const std::string idText = Unescape((*args)[0], a);
            const std::string pathText = Unescape((*args)[1], b);
            if (a && b)
                if (auto id = Uuid::Parse(idText))
                    return Result<Variant>::Success(Variant(ResourceRefValue{ *id, pathText }));
        }
    }
    if (auto args = typed("array")) {
        VariantArray array;
        for (const auto& argument : *args) {
            auto item = ParseValue(argument, path, line); if (!item) return item;
            array.push_back(std::move(item.Value()));
        }
        return Result<Variant>::Success(Variant(std::move(array)));
    }
    if (auto args = typed("object")) {
        if (args->size() % 2 != 0) return Result<Variant>::Failure(ParseError(path,line,"object() requires alternating string keys and values."));
        VariantObject object;
        for (std::size_t i=0;i<args->size();i+=2) {
            bool ok=false; const std::string key=Unescape((*args)[i],ok); if(!ok)return Result<Variant>::Failure(ParseError(path,line,"object() key must be a quoted string."));
            auto item=ParseValue((*args)[i+1],path,line);if(!item)return item;object[key]=std::move(item.Value());
        }
        return Result<Variant>::Success(Variant(std::move(object)));
    }

    if (value.find_first_of(".eE") == std::string::npos) {
        std::int64_t integer = 0;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), integer);
        if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
            return Result<Variant>::Success(Variant(integer));
    }
    double number = 0.0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
    if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size())
        return Result<Variant>::Success(Variant(number));
    return Result<Variant>::Failure(ParseError(path, line, "Unsupported property value.", value));
}

std::string Quote(std::string_view text) {
    std::string out = "\"";
    for (char c : text) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            default: out.push_back(c); break;
        }
    }
    return out + '"';
}

std::string ValueText(const Variant& value) {
    std::ostringstream out;
    out << std::setprecision(9);
    switch (value.Type()) {
        case VariantType::Null: return "null";
        case VariantType::Bool: return *value.TryGet<bool>() ? "true" : "false";
        case VariantType::Integer: return std::to_string(*value.TryGet<std::int64_t>());
        case VariantType::Number: out << *value.TryGet<double>(); return out.str();
        case VariantType::String: return Quote(*value.TryGet<std::string>());
        case VariantType::Vec2: { const Vec2 v = *value.TryGet<Vec2>(); out << "vec2(" << v.x << ", " << v.y << ')'; return out.str(); }
        case VariantType::Rect: { const Rect v = *value.TryGet<Rect>(); out << "rect(" << v.x << ", " << v.y << ", " << v.w << ", " << v.h << ')'; return out.str(); }
        case VariantType::Color: { const Color c = *value.TryGet<Color>(); out << "color(" << static_cast<int>(c.r) << ", " << static_cast<int>(c.g) << ", " << static_cast<int>(c.b) << ", " << static_cast<int>(c.a) << ')'; return out.str(); }
        case VariantType::Uuid: return "uuid(" + Quote(value.TryGet<Uuid>()->ToString()) + ")";
        case VariantType::ResourceRef: { const auto& r = *value.TryGet<ResourceRefValue>(); return "res(" + Quote(r.id.ToString()) + ", " + Quote(r.lastKnownPath) + ")"; }
        case VariantType::Array: {
            std::string text="array(";const auto* array=value.AsArray();for(std::size_t i=0;array&&i<array->size();++i){if(i)text+=", ";text+=ValueText((*array)[i]);}return text+")";
        }
        case VariantType::Object: {
            std::string text="object(";const auto* object=value.AsObject();bool first=true;if(object)for(const auto& [key,item]:*object){if(!first)text+=", ";first=false;text+=Quote(key)+", "+ValueText(item);}return text+")";
        }
    }
    return "null";
}

const char* KindName(DocumentKind kind) {
    switch (kind) {
        case DocumentKind::Project: return "pxproject";
        case DocumentKind::Resource: return "pxresource";
        case DocumentKind::Scene: return "pxscene";
        case DocumentKind::Meta: return "pxmeta";
    }
    return "pxresource";
}
}

Result<TypedDocument> ParseTypedDocument(std::string_view text, const std::string& sourcePath) {
    TypedDocument document;
    std::vector<diag::Diagnostic> diagnostics;
    NodeRecord* activeNode = nullptr;
    bool headerSeen = false;
    std::istringstream input{ std::string(text) };
    std::string raw;
    int line = 0;
    while (std::getline(input, raw)) {
        ++line;
        const std::string current = Trim(raw);
        if (current.empty() || current.starts_with('#')) continue;
        if (!headerSeen) {
            std::istringstream header(current);
            std::string kind, id, type;
            int version = 0;
            header >> kind >> version >> id;
            std::getline(header, type);
            type = Trim(type);
            if (kind == "@pxproject") document.kind = DocumentKind::Project;
            else if (kind == "@pxresource") document.kind = DocumentKind::Resource;
            else if (kind == "@pxscene") document.kind = DocumentKind::Scene;
            else if (kind == "@pxmeta") document.kind = DocumentKind::Meta;
            else {
                diagnostics.push_back(ParseError(sourcePath, line, "Missing typed-document header."));
                continue;
            }
            document.formatVersion = version;
            if(version!=TypedDocument::CurrentVersion)diagnostics.push_back(ParseError(sourcePath,line,"Only strict typed-document version 3 is supported."));
            auto parsedId = Uuid::Parse(id);
            if (!parsedId) diagnostics.push_back(ParseError(sourcePath, line, "Invalid document UUID."));
            else document.id = *parsedId;
            document.type = type;
            headerSeen = true;
            continue;
        }
        if (current.starts_with("[node ") && current.ends_with(']')) {
            const auto parts = Arguments(std::string_view(current).substr(6, current.size() - 7));
            if (parts.size() != 4) {
                diagnostics.push_back(ParseError(sourcePath, line, "A node header requires id,parent,name,type."));
                continue;
            }
            bool a = false, b = false, c = false, d = false;
            const std::string id = Unescape(parts[0], a);
            const std::string parent = Unescape(parts[1], b);
            const std::string name = Unescape(parts[2], c);
            const std::string type = Unescape(parts[3], d);
            auto nodeId = Uuid::Parse(id);
            auto parentId = Uuid::Parse(parent);
            if (!a || !b || !c || !d || !nodeId || (!parent.empty() && !parentId)) {
                diagnostics.push_back(ParseError(sourcePath, line, "Invalid node header."));
                continue;
            }
            document.nodes.push_back(NodeRecord{ *nodeId, parentId.value_or(Uuid{}), name, type });
            activeNode = &document.nodes.back();
            continue;
        }
        const std::size_t equals = current.find('=');
        if (equals == std::string::npos) {
            diagnostics.push_back(ParseError(sourcePath, line, "Expected a property assignment."));
            continue;
        }
        const std::string key = Trim(std::string_view(current).substr(0, equals));
        auto value = ParseValue(std::string_view(current).substr(equals + 1), sourcePath, line);
        if (!value) {
            diagnostics.insert(diagnostics.end(), value.Diagnostics().begin(), value.Diagnostics().end());
            continue;
        }
        auto& properties = activeNode ? activeNode->properties : document.properties;
        if (!properties.emplace(key, value.TakeValue()).second)
            diagnostics.push_back(ParseError(sourcePath, line, "Duplicate property.", key));
    }
    if (!headerSeen) diagnostics.push_back(ParseError(sourcePath, 1, "The document is empty or missing a header."));
    return diagnostics.empty() ? Result<TypedDocument>::Success(std::move(document))
                               : Result<TypedDocument>::Failure(std::move(diagnostics));
}

std::string WriteTypedDocument(const TypedDocument& document) {
    std::ostringstream out;
    out << '@' << KindName(document.kind) << ' ' << document.formatVersion << ' '
        << document.id.ToString();
    if (!document.type.empty()) out << ' ' << document.type;
    out << "\n\n";
    for (const auto& [key, value] : document.properties) out << key << " = " << ValueText(value) << '\n';
    for (const NodeRecord& node : document.nodes) {
        out << "\n[node " << Quote(node.id.ToString()) << ", "
            << Quote(node.parent.Empty() ? std::string{} : node.parent.ToString()) << ", "
            << Quote(node.name) << ", " << Quote(node.type) << "]\n";
        for (const auto& [key, value] : node.properties) out << key << " = " << ValueText(value) << '\n';
    }
    return out.str();
}

}  // namespace px::resource
