#include "Engine/VN/PDS/Parser.h"

#include <cctype>
#include <sstream>

namespace px::vn {

namespace {

std::string Trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

void ParseCommandBody(const std::string& body, Command& cmd) {
    std::size_t i = 0;
    const std::size_t n = body.size();
    bool first = true;

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(body[i]))) ++i;
        if (i >= n) break;

        std::size_t start = i;
        while (i < n && !std::isspace(static_cast<unsigned char>(body[i])) && body[i] != '=') ++i;
        std::string key = body.substr(start, i - start);

        if (first) {
            cmd.type = key;
            first = false;
            continue;
        }

        std::string value;
        if (i < n && body[i] == '=') {
            ++i;
            if (i < n && body[i] == '"') {
                ++i;
                std::size_t vs = i;
                while (i < n && body[i] != '"') ++i;
                value = body.substr(vs, i - vs);
                if (i < n) ++i;
            } else {
                std::size_t vs = i;
                while (i < n && !std::isspace(static_cast<unsigned char>(body[i]))) ++i;
                value = body.substr(vs, i - vs);
            }
        }
        cmd.args.push_back(Arg{ std::move(key), std::move(value) });
    }
}

void ParseMetaFields(const std::string& body, std::unordered_map<std::string, std::string>& out) {
    Command tmp;
    ParseCommandBody("_ " + body, tmp);
    for (Arg& a : tmp.args) {
        out[a.key] = a.value;
    }
}

}

ParsedScript ParsePDS(const std::string& source) {
    ParsedScript script;
    std::istringstream stream(source);
    std::string raw;
    int lineNo = 0;
    bool pendingMeta = false;
    NodeMeta meta;

    auto attachMeta = [&](int commandIndex) {
        if (pendingMeta) {
            meta.commandIndex = commandIndex;
            script.nodeMeta.push_back(meta);
            pendingMeta = false;
            meta = NodeMeta{};
        }
    };

    while (std::getline(stream, raw)) {
        ++lineNo;
        if (!raw.empty() && raw.back() == '\r') {
            raw.pop_back();
        }
        std::string line = Trim(raw);
        if (line.empty()) {
            continue;
        }

        if (line.rfind("//@node", 0) == 0) {
            pendingMeta = true;
            ParseMetaFields(Trim(line.substr(7)), meta.fields);
            continue;
        }
        if (line.rfind("//@link", 0) == 0) {
            LinkMeta link;
            ParseMetaFields(Trim(line.substr(7)), link.fields);
            script.linkMeta.push_back(std::move(link));
            continue;
        }
        if (line.rfind("//", 0) == 0 || line[0] == '#') {
            continue;
        }

        Command cmd;
        cmd.line = lineNo;

        if (line[0] == '*') {
            cmd.type = "label";
            cmd.args.push_back(Arg{ "name", Trim(line.substr(1)) });
        } else if (line.front() == '[') {
            const std::size_t close = line.rfind(']');
            const std::string body =
                (close != std::string::npos) ? line.substr(1, close - 1) : line.substr(1);
            ParseCommandBody(body, cmd);
            if (close != std::string::npos && close + 1 < line.size()) {
                std::string trailing = Trim(line.substr(close + 1));
                if (!trailing.empty()) {
                    cmd.args.push_back(Arg{ "value", trailing });
                }
            }
        } else {
            cmd.type = "say";
            cmd.args.push_back(Arg{ "value", line });
        }

        attachMeta(static_cast<int>(script.commands.size()));
        script.commands.push_back(std::move(cmd));
    }

    return script;
}

std::string WritePDS(const ParsedScript& script) {
    std::unordered_map<int, const NodeMeta*> metaByCmd;
    for (const NodeMeta& m : script.nodeMeta) {
        metaByCmd[m.commandIndex] = &m;
    }

    std::ostringstream out;
    for (const LinkMeta& link : script.linkMeta) {
        out << "//@link";
        for (const auto& [k, v] : link.fields) {
            out << ' ' << k << '=' << v;
        }
        out << '\n';
    }

    for (std::size_t idx = 0; idx < script.commands.size(); ++idx) {
        const Command& cmd = script.commands[idx];

        if (auto it = metaByCmd.find(static_cast<int>(idx)); it != metaByCmd.end()) {
            out << "//@node";
            for (const auto& [k, v] : it->second->fields) {
                out << ' ' << k << '=' << v;
            }
            out << '\n';
        }

        if (cmd.type == "label") {
            out << '*' << cmd.Get("name") << '\n';
            continue;
        }
        if (cmd.type == "say" && cmd.args.size() == 1 && cmd.args[0].key == "value") {
            out << cmd.args[0].value << '\n';
            continue;
        }

        out << '[' << cmd.type;
        for (const Arg& a : cmd.args) {
            out << ' ' << a.key << "=\"" << a.value << '"';
        }
        out << "]\n";
    }
    return out.str();
}

}
