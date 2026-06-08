#include "Engine/Project/Database.h"

#include <nlohmann/json.hpp>

namespace px::project {

using Json = nlohmann::json;

namespace {
Color ColorFromJson(const Json& j, Color fallback) {
    if (!j.is_array() || j.size() < 3) return fallback;
    Color c;
    c.r = j[0].get<int>();
    c.g = j[1].get<int>();
    c.b = j[2].get<int>();
    c.a = j.size() >= 4 ? j[3].get<int>() : 255;
    return c;
}
Json ColorToJson(Color c) {
    return Json::array({ c.r, c.g, c.b, c.a });
}
}

bool Database::Load(const std::string& jsonText) {
    Json j = Json::parse(jsonText, nullptr, false);
    if (j.is_discarded()) {
        return false;
    }
    characters.clear();
    variables.clear();
    gallery.clear();
    chapters.clear();
    inputMap.clear();

    for (const Json& c : j.value("characters", Json::array())) {
        Character ch;
        ch.id = c.value("id", std::string{});
        ch.name = c.value("name", std::string{});
        ch.color = ColorFromJson(c.value("color", Json{}), ch.color);
        ch.voiceDir = c.value("voiceDir", std::string{});
        for (const Json& e : c.value("expressions", Json::array())) {
            ch.expressions.push_back(e.get<std::string>());
        }
        characters.push_back(std::move(ch));
    }
    for (const Json& v : j.value("variables", Json::array())) {
        variables.push_back(Variable{ v.value("name", std::string{}), v.value("default", 0),
                                      v.value("persistent", false) });
    }
    for (const Json& g : j.value("gallery", Json::array())) {
        gallery.push_back(GalleryItem{ g.value("id", std::string{}), g.value("title", std::string{}),
                                       g.value("thumbnail", std::string{}),
                                       g.value("image", std::string{}) });
    }
    for (const Json& c : j.value("chapters", Json::array())) {
        chapters.push_back(Chapter{ c.value("id", std::string{}), c.value("title", std::string{}),
                                    c.value("script", std::string{}),
                                    c.value("alwaysUnlocked", false) });
    }
    for (const Json& b : j.value("inputMap", Json::array())) {
        inputMap.push_back(InputBinding{ b.value("key", std::string{}),
                                         b.value("action", std::string{}),
                                         b.value("target", std::string{}) });
    }
    return true;
}

std::string Database::Serialize() const {
    Json j;
    Json chs = Json::array();
    for (const Character& c : characters) {
        chs.push_back({ { "id", c.id },
                        { "name", c.name },
                        { "color", ColorToJson(c.color) },
                        { "voiceDir", c.voiceDir },
                        { "expressions", c.expressions } });
    }
    j["characters"] = chs;
    Json vars = Json::array();
    for (const Variable& v : variables) {
        vars.push_back(
            { { "name", v.name }, { "default", v.defaultValue }, { "persistent", v.persistent } });
    }
    j["variables"] = vars;
    Json gal = Json::array();
    for (const GalleryItem& g : gallery) {
        gal.push_back({ { "id", g.id },
                        { "title", g.title },
                        { "thumbnail", g.thumbnail },
                        { "image", g.image } });
    }
    j["gallery"] = gal;
    Json chaps = Json::array();
    for (const Chapter& c : chapters) {
        chaps.push_back({ { "id", c.id },
                          { "title", c.title },
                          { "script", c.script },
                          { "alwaysUnlocked", c.alwaysUnlocked } });
    }
    j["chapters"] = chaps;
    Json bindings = Json::array();
    for (const InputBinding& b : inputMap) {
        bindings.push_back({ { "key", b.key }, { "action", b.action }, { "target", b.target } });
    }
    j["inputMap"] = bindings;
    return j.dump(2);
}

void Database::SeedDefault() {
    characters = { { "girl", "ユズ", Color{ 255, 220, 140, 255 }, "Data/Audio/Voice/girl/", { "d" } } };
    variables = { { "route_a", 0, false }, { "affection", 0, true } };
    gallery = { { "cg_title", "Title CG", "Data/Image/Background/title_bg.jpg",
                  "Data/Image/Background/title_bg.jpg" },
                { "cg_sky", "Sky", "Data/Image/Background/sky.png",
                  "Data/Image/Background/sky.png" } };
    chapters = { { "chapter1", "第一章", "test_scene.pds", true } };
    inputMap = { { "Escape", "screen.open", "Data/UI/settings.pxui" } };
}

const Character* Database::FindCharacter(const std::string& id) const {
    for (const Character& c : characters) {
        if (c.id == id) return &c;
    }
    return nullptr;
}

}
