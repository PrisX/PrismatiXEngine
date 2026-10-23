#include "Engine/Script/ScriptRawGraphics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <vector>

namespace px::script {
namespace {

using Failure = ScriptRawGraphics::Failure;

Failure TypeFailure(std::string message) {
    return {.range = false, .code = "PXJS7803", .message = std::move(message)};
}

Failure RangeFailure(std::string code, std::string message) {
    return {.range = true, .code = std::move(code), .message = std::move(message)};
}

Failure Unknown(const std::string_view kind, const std::string_view id) {
    return RangeFailure("PXJS7802", "No raw " + std::string(kind) + " '" +
                                        std::string(id) + "'");
}

bool ValidId(const std::string_view id) {
    return !id.empty() && id.size() <= 128;
}

const Variant* Property(const VariantObject& object, const std::string_view key) {
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

std::optional<double> Number(const Variant& value) {
    if (const auto* number = value.TryGet<double>()) return *number;
    if (const auto* integer = value.TryGet<std::int64_t>())
        return static_cast<double>(*integer);
    return std::nullopt;
}

std::optional<graphics::RawBlendMode> ParseBlend(const std::string_view value) {
    if (value == "normal") return graphics::RawBlendMode::Normal;
    if (value == "premultiplied") return graphics::RawBlendMode::Premultiplied;
    if (value == "additive") return graphics::RawBlendMode::Additive;
    if (value == "multiply") return graphics::RawBlendMode::Multiply;
    if (value == "replace") return graphics::RawBlendMode::Replace;
    return std::nullopt;
}

std::optional<bool> ParseFilter(const VariantObject& object) {
    const auto* filter = Property(object, "filter");
    if (!filter || filter->Type() == VariantType::Null) return true;
    const auto* name = filter->TryGet<std::string>();
    if (!name || (*name != "linear" && *name != "nearest")) return std::nullopt;
    return *name == "linear";
}

// {r, g, b, a} with 0-255 channels; null clears to nothing.
std::optional<std::optional<Color>> ParseClear(const Variant& value) {
    if (value.Type() == VariantType::Null) return std::optional<Color>{};
    const auto* object = value.AsObject();
    if (!object) return std::nullopt;
    std::uint8_t channels[4]{0, 0, 0, 255};
    const char* names[4]{"r", "g", "b", "a"};
    for (int index = 0; index < 4; ++index) {
        const auto* channel = Property(*object, names[index]);
        if (!channel) continue;
        const auto number = Number(*channel);
        if (!number || !std::isfinite(*number) || *number < 0.0 || *number > 255.0)
            return std::nullopt;
        channels[index] = static_cast<std::uint8_t>(std::lround(*number));
    }
    return std::optional<Color>{Color{channels[0], channels[1], channels[2], channels[3]}};
}

// A finite number a shader uniform (a float) can hold.
std::optional<float> UniformNumber(const Variant& value) {
    const auto number = Number(value);
    if (!number || !std::isfinite(*number) ||
        std::abs(*number) > std::numeric_limits<float>::max())
        return std::nullopt;
    return static_cast<float>(*number);
}

// The default randomSeed of a mesh's effect, the way the Stage seeds a node's.
float EffectSeed(const std::string_view mesh, const std::string_view effect) {
    std::uint32_t hash = 2166136261u;
    for (const auto text : {mesh, effect}) {
        for (const unsigned char character : text) {
            hash ^= character;
            hash *= 16777619u;
        }
        hash ^= 0xffu;
        hash *= 16777619u;
    }
    return static_cast<float>(hash);
}

// Named uniform values as ctx.stage.nodeEffect takes them: a number, or 1 to
// 4 numbers (vec2 is 2, color is 4 in 0-1).
std::optional<graphics::CustomEffectNamedParameters> ParseEffectParameters(
    const Variant& value) {
    graphics::CustomEffectNamedParameters parameters;
    if (value.Type() == VariantType::Null) return parameters;
    const auto* named = value.AsObject();
    if (!named || named->size() > 8) return std::nullopt;
    for (const auto& [name, item] : *named) {
        std::vector<float> components;
        if (const auto* list = item.AsArray()) {
            if (list->empty() || list->size() > 4) return std::nullopt;
            for (const auto& entry : *list) {
                const auto number = UniformNumber(entry);
                if (!number) return std::nullopt;
                components.push_back(*number);
            }
        } else {
            const auto number = UniformNumber(item);
            if (!number) return std::nullopt;
            components.push_back(*number);
        }
        parameters.emplace(name, std::move(components));
    }
    return parameters;
}

}  // namespace

Variant ScriptRawGraphics::LimitsValue() const {
    const auto count = [](const std::size_t value) {
        return static_cast<std::int64_t>(value);
    };
    return Variant(VariantObject{
        {"maxVerticesPerFrame", count(m_limits.verticesPerFrame)},
        {"maxIndicesPerFrame", count(m_limits.indicesPerFrame)},
        {"maxUploadsPerFrame", count(m_limits.uploadsPerFrame)},
        {"maxUploadBytesPerFrame", count(m_limits.uploadBytesPerFrame)},
        {"maxVerticesPerMesh", count(m_limits.verticesPerMesh)},
        {"maxIndicesPerMesh", count(m_limits.indicesPerMesh)},
        {"maxMeshes", count(m_limits.meshes)},
        {"maxTextures", count(m_limits.textures)},
        {"maxTargets", count(m_limits.targets)},
        {"maxTextureSize", static_cast<std::int64_t>(m_limits.textureSize)},
        {"maxEffectsPerFrame", count(m_limits.effectsPerFrame)},
        {"backend", m_renderer ? m_renderer->BackendName() : std::string("none")},
    });
}

Variant ScriptRawGraphics::StatsValue() const {
    const auto count = [](const std::size_t value) {
        return static_cast<std::int64_t>(value);
    };
    return Variant(VariantObject{
        {"frame", static_cast<std::int64_t>(m_frame)},
        {"vertices", count(m_drawnVertices)},
        {"indices", count(m_drawnIndices)},
        {"uploads", count(m_uploads)},
        {"uploadBytes", count(m_uploadBytes)},
        {"skippedMeshes", count(m_skippedMeshes)},
        {"skippedEffects", count(m_skippedEffects)},
        {"meshes", count(m_meshes.size())},
        {"textures", count(m_textures.size())},
        {"targets", count(m_targets.size())},
    });
}

ScriptRawGraphics::Outcome ScriptRawGraphics::ApplyMeshOptions(
    Mesh& mesh, const std::string_view id, const Variant& options) const {
    if (options.Type() == VariantType::Null) return std::nullopt;
    const auto* object = options.AsObject();
    if (!object) return TypeFailure("raw mesh options must be an object");
    for (const auto& [key, value] : *object) {
        if (key == "phase") {
            const auto* name = value.TryGet<std::string>();
            const auto phase = name ? ParseRenderPhase(*name) : std::nullopt;
            if (!phase) return TypeFailure("phase must be before-stage, after-stage, before-ui or after-ui");
            mesh.phase = *phase;
        } else if (key == "order") {
            const auto order = Number(value);
            if (!order || !std::isfinite(*order)) return TypeFailure("order must be a finite number");
            mesh.order = static_cast<std::int64_t>(*order);
        } else if (key == "visible") {
            const auto* visible = value.TryGet<bool>();
            if (!visible) return TypeFailure("visible must be a boolean");
            mesh.visible = *visible;
        } else if (key == "route") {
            const auto* route = value.TryGet<std::string>();
            if (!route) return TypeFailure("route must be a string");
            mesh.route = *route;
        } else if (key == "blend") {
            const auto* name = value.TryGet<std::string>();
            const auto blend = name ? ParseBlend(*name) : std::nullopt;
            if (!blend) return TypeFailure("blend must be normal, premultiplied, additive, multiply or replace");
            mesh.blend = *blend;
        } else if (key == "texture") {
            if (value.Type() == VariantType::Null) { mesh.texture = {}; continue; }
            const auto* source = value.AsObject();
            const auto* kind = source ? Property(*source, "kind") : nullptr;
            const auto* name = kind ? kind->TryGet<std::string>() : nullptr;
            if (!name) return TypeFailure("texture must be {kind: \"asset\" | \"texture\" | \"target\", ...}");
            const auto* reference = Property(*source, *name == "asset" ? "path" : "id");
            const auto* text = reference ? reference->TryGet<std::string>() : nullptr;
            if (!text || text->empty()) return TypeFailure("texture source needs its path or id");
            if (*name == "asset") mesh.texture = {SourceKind::Asset, *text};
            else if (*name == "texture") mesh.texture = {SourceKind::Texture, *text};
            else if (*name == "target") mesh.texture = {SourceKind::Target, *text};
            else return TypeFailure("texture kind must be asset, texture or target");
        } else if (key == "mask") {
            // A mesh id, a list of them (their coverage united) or null.
            std::vector<std::string> masks;
            const auto add = [&](const Variant& item) -> Outcome {
                const auto* mask = item.TryGet<std::string>();
                if (!mask || !ValidId(*mask)) return TypeFailure("mask must be a mesh id or a list of mesh ids");
                if (*mask == id) return TypeFailure("a mesh cannot mask itself");
                if (std::ranges::find(masks, *mask) == masks.end()) masks.push_back(*mask);
                return std::nullopt;
            };
            if (const auto* list = value.AsArray()) {
                if (list->size() > m_limits.masksPerMesh)
                    return RangeFailure("PXJS7804", "a raw mesh has at most " +
                                                        std::to_string(m_limits.masksPerMesh) + " masks");
                for (const auto& item : *list)
                    if (auto failure = add(item)) return failure;
            } else if (value.Type() != VariantType::Null) {
                if (auto failure = add(value)) return failure;
            }
            mesh.masks = std::move(masks);
        } else if (key == "maskInverted") {
            const auto* inverted = value.TryGet<bool>();
            if (!inverted) return TypeFailure("maskInverted must be a boolean");
            mesh.maskInverted = *inverted;
        } else if (key == "target") {
            if (value.Type() == VariantType::Null) { mesh.target.clear(); continue; }
            const auto* target = value.TryGet<std::string>();
            if (!target || !ValidId(*target)) return TypeFailure("target must be a render target id");
            mesh.target = *target;
        } else if (key == "transform") {
            if (value.Type() == VariantType::Null) { mesh.transform = {}; continue; }
            const auto* transform = value.AsObject();
            if (!transform) return TypeFailure("transform must be {a, b, c, d, tx, ty}");
            graphics::AffineTransform2D parsed;
            float* fields[6]{&parsed.a, &parsed.b, &parsed.c, &parsed.d, &parsed.tx, &parsed.ty};
            const char* names[6]{"a", "b", "c", "d", "tx", "ty"};
            for (int index = 0; index < 6; ++index) {
                const auto* field = Property(*transform, names[index]);
                const auto number = field ? Number(*field) : std::nullopt;
                if (!number || !std::isfinite(*number))
                    return TypeFailure("transform needs finite a, b, c, d, tx and ty");
                *fields[index] = static_cast<float>(*number);
            }
            mesh.transform = parsed;
        } else if (key == "effect") {
            if (auto failure = ParseEffect(value, id, mesh.effect)) return failure;
        } else {
            return TypeFailure("unknown raw mesh option: " + key);
        }
    }
    if (!mesh.target.empty() && mesh.texture.kind == SourceKind::Target &&
        mesh.texture.value == mesh.target)
        return TypeFailure("a mesh cannot sample the render target it draws into");
    if (!mesh.target.empty() && !mesh.masks.empty())
        return TypeFailure("masks apply to meshes drawn to the screen, not into a render target");
    if (!mesh.target.empty() && mesh.effect)
        return TypeFailure("effects apply to meshes drawn to the screen, not into a render target");
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::ParseEffect(
    const Variant& value, const std::string_view meshId,
    std::optional<Effect>& effect) const {
    if (value.Type() == VariantType::Null) {
        effect.reset();
        return std::nullopt;
    }
    const auto* object = value.AsObject();
    if (!object) return TypeFailure("effect must be {id, parameters?, progress?, seed?} or null");
    Effect parsed;
    bool hasSeed = false;
    for (const auto& [key, field] : *object) {
        if (key == "id") {
            const auto* id = field.TryGet<std::string>();
            if (!id || !ValidId(*id)) return TypeFailure("effect id must be a .pxeffect id");
            parsed.id = *id;
        } else if (key == "parameters") {
            auto parameters = ParseEffectParameters(field);
            if (!parameters)
                return TypeFailure("effect parameters are at most 8 named numbers or vectors of 1 to 4 finite numbers");
            parsed.parameters = std::move(*parameters);
        } else if (key == "progress") {
            const auto progress = Number(field);
            if (!progress || !std::isfinite(*progress)) return TypeFailure("effect progress must be a finite number");
            parsed.progress = std::clamp(static_cast<float>(*progress), 0.0f, 1.0f);
        } else if (key == "seed") {
            const auto seed = UniformNumber(field);
            if (!seed) return TypeFailure("effect seed must be a finite number");
            parsed.seed = *seed;
            hasSeed = true;
        } else {
            return TypeFailure("unknown raw mesh effect option: " + key);
        }
    }
    if (parsed.id.empty()) return TypeFailure("effect needs the id of a .pxeffect");
    if (!hasSeed) parsed.seed = EffectSeed(meshId, parsed.id);
    // Effects are loaded with the package, before any extension runs; a
    // renderer without the GPU-effects tier has none, so there only the
    // shape is checked and the mesh draws without its effect.
    if (m_renderer && m_renderer->SupportsStagePostEffects()) {
        if (!m_renderer->HasCustomEffect(parsed.id, "node"))
            return TypeFailure("effect '" + parsed.id + "' is not a packaged .pxeffect with targetLayer node");
        parsed.resolved = parsed.parameters.empty()
                              ? m_renderer->CustomEffectDefaults(parsed.id)
                              : m_renderer->ResolveCustomEffectParameters(parsed.id, parsed.parameters);
        if (!parsed.resolved)
            return TypeFailure("effect parameters do not match the uniforms '" + parsed.id +
                               "' declares (names, number of components and ranges)");
    }
    effect = std::move(parsed);
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::CreateMesh(
    const std::string_view id, const Variant* options) {
    if (!ValidId(id)) return TypeFailure("raw mesh ids are 1 to 128 characters");
    const auto existing = m_meshes.find(id);
    if (existing == m_meshes.end() && m_meshes.size() >= m_limits.meshes)
        return RangeFailure("PXJS7804", "an extension has at most " +
                                            std::to_string(m_limits.meshes) + " raw meshes");
    Mesh mesh;
    mesh.sequence = existing == m_meshes.end() ? ++m_sequence : existing->second.sequence;
    if (options) {
        if (auto failure = ApplyMeshOptions(mesh, id, *options)) return failure;
    }
    m_meshes.insert_or_assign(std::string(id), std::move(mesh));
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::UpdateMesh(
    const std::string_view id, const Variant& options, bool& found) {
    const auto mesh = m_meshes.find(id);
    found = mesh != m_meshes.end();
    if (!found) return std::nullopt;
    Mesh updated = mesh->second;
    if (auto failure = ApplyMeshOptions(updated, id, options)) return failure;
    mesh->second = std::move(updated);
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::CountUpload(const std::size_t bytes) {
    if (m_uploads + 1 > m_limits.uploadsPerFrame)
        return RangeFailure("PXJS7801", "raw upload budget exceeded: at most " +
                                            std::to_string(m_limits.uploadsPerFrame) +
                                            " uploads per frame");
    if (bytes > m_limits.uploadBytesPerFrame - std::min(m_uploadBytes, m_limits.uploadBytesPerFrame))
        return RangeFailure("PXJS7801", "raw upload budget exceeded: at most " +
                                            std::to_string(m_limits.uploadBytesPerFrame) +
                                            " bytes per frame");
    ++m_uploads;
    m_uploadBytes += bytes;
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::UploadVertices(
    const std::string_view id, const float* data, const std::size_t floatCount,
    const std::optional<std::size_t> firstVertex) {
    const Upload upload{.id = id, .vertices = data, .floatCount = floatCount,
                        .firstVertex = firstVertex, .hasVertices = true};
    return UploadBatch({&upload, 1});
}

ScriptRawGraphics::Outcome ScriptRawGraphics::UploadIndices(
    const std::string_view id, const std::uint16_t* data16,
    const std::uint32_t* data32, const std::size_t count) {
    const Upload upload{.id = id, .indices16 = data16, .indices32 = data32,
                        .indexCount = count, .hasIndices = true};
    return UploadBatch({&upload, 1});
}

ScriptRawGraphics::Outcome ScriptRawGraphics::UploadBatch(const std::span<const Upload> uploads) {
    constexpr std::size_t stride = 8;
    // Everything is checked before anything changes, so a failed batch
    // leaves every mesh as it was.
    std::vector<Mesh*> meshes;
    meshes.reserve(uploads.size());
    std::size_t bytes = 0;
    for (const auto& upload : uploads) {
        const auto found = m_meshes.find(upload.id);
        if (found == m_meshes.end()) return Unknown("mesh", upload.id);
        Mesh& mesh = found->second;
        if (std::ranges::find(meshes, &mesh) != meshes.end())
            return TypeFailure("an upload names mesh '" + std::string(upload.id) + "' twice");
        if (!upload.hasVertices && !upload.hasIndices)
            return TypeFailure("an upload of mesh '" + std::string(upload.id) + "' has neither vertices nor indices");
        meshes.push_back(&mesh);
        std::size_t vertexCount = mesh.vertices.size();
        if (upload.hasVertices) {
            if (upload.floatCount % stride != 0)
                return TypeFailure("vertices hold 8 floats each (x, y, u, v, r, g, b, a)");
            const std::size_t count = upload.floatCount / stride;
            if (upload.firstVertex) {
                if (*upload.firstVertex > vertexCount || count > vertexCount - *upload.firstVertex)
                    return RangeFailure("PXJS7802", "firstVertex and the data reach past the mesh's vertices");
            } else {
                if (count > m_limits.verticesPerMesh)
                    return RangeFailure("PXJS7804", "a raw mesh has at most " +
                                                        std::to_string(m_limits.verticesPerMesh) + " vertices");
                vertexCount = count;
            }
            for (std::size_t index = 0; index < upload.floatCount; ++index)
                if (!std::isfinite(upload.vertices[index])) return TypeFailure("vertices must be finite numbers");
            bytes += upload.floatCount * sizeof(float);
        }
        if (upload.hasIndices) {
            if (upload.indexCount % 3 != 0) return TypeFailure("indices describe triangles: a multiple of 3");
            if (upload.indexCount > m_limits.indicesPerMesh)
                return RangeFailure("PXJS7804", "a raw mesh has at most " +
                                                    std::to_string(m_limits.indicesPerMesh) + " indices");
            for (std::size_t index = 0; index < upload.indexCount; ++index) {
                const std::uint32_t value = upload.indices16 ? upload.indices16[index] : upload.indices32[index];
                if (value >= vertexCount)
                    return RangeFailure("PXJS7802", "index " + std::to_string(value) +
                                                        " is past the mesh's " +
                                                        std::to_string(vertexCount) + " vertices");
            }
            bytes += upload.indexCount * (upload.indices16 ? 2U : 4U);
        }
    }
    if (auto failure = CountUpload(bytes)) return failure;
    for (std::size_t part = 0; part < uploads.size(); ++part) {
        const auto& upload = uploads[part];
        Mesh& mesh = *meshes[part];
        if (upload.hasVertices) {
            const std::size_t count = upload.floatCount / stride;
            const std::size_t first = upload.firstVertex.value_or(0);
            if (!upload.firstVertex) {
                mesh.vertices.assign(count, {});
                // Indices left over from more vertices would read past the new ones.
                if (!upload.hasIndices && std::ranges::any_of(mesh.indices, [count](const int index) {
                        return static_cast<std::size_t>(index) >= count;
                    }))
                    mesh.indices.clear();
            }
            for (std::size_t index = 0; index < count; ++index) {
                const float* source = upload.vertices + index * stride;
                mesh.vertices[first + index] = {source[0], source[1], source[2], source[3],
                                                source[4], source[5], source[6], source[7]};
            }
        }
        if (upload.hasIndices) {
            mesh.indices.resize(upload.indexCount);
            for (std::size_t index = 0; index < upload.indexCount; ++index)
                mesh.indices[index] = static_cast<int>(upload.indices16 ? upload.indices16[index]
                                                                        : upload.indices32[index]);
        }
    }
    return std::nullopt;
}

bool ScriptRawGraphics::DestroyMesh(const std::string_view id) {
    const auto mesh = m_meshes.find(id);
    if (mesh == m_meshes.end()) return false;
    m_meshes.erase(mesh);
    return true;
}

std::size_t ScriptRawGraphics::TextureBytes() const {
    std::size_t total = 0;
    for (const auto& [id, texture] : m_textures)
        total += static_cast<std::size_t>(texture.width) * texture.height * 4U;
    for (const auto& [id, target] : m_targets)
        total += static_cast<std::size_t>(target.width) * target.height * 4U;
    return total;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::CreateTexture(
    const std::string_view id, const int width, const int height,
    const Variant* options) {
    if (!ValidId(id)) return TypeFailure("raw texture ids are 1 to 128 characters");
    if (width <= 0 || height <= 0 || width > m_limits.textureSize ||
        height > m_limits.textureSize)
        return RangeFailure("PXJS7804", "raw textures are 1 to " +
                                            std::to_string(m_limits.textureSize) +
                                            " pixels on each side");
    bool linear = true;
    if (options && options->Type() != VariantType::Null) {
        const auto* object = options->AsObject();
        const auto filter = object ? ParseFilter(*object) : std::nullopt;
        if (!filter) return TypeFailure("raw texture options are {filter: \"linear\" | \"nearest\"}");
        linear = *filter;
    }
    const auto existing = m_textures.find(id);
    if (existing == m_textures.end() && m_textures.size() >= m_limits.textures)
        return RangeFailure("PXJS7804", "an extension has at most " +
                                            std::to_string(m_limits.textures) + " raw textures");
    const std::size_t replaced = existing == m_textures.end() ? 0
        : static_cast<std::size_t>(existing->second.width) * existing->second.height * 4U;
    if (TextureBytes() - replaced + static_cast<std::size_t>(width) * height * 4U >
        m_limits.textureBytes)
        return RangeFailure("PXJS7804", "raw textures and targets of an extension hold at most " +
                                            std::to_string(m_limits.textureBytes / (1024U * 1024U)) +
                                            " MiB");
    Texture texture{width, height, linear, {}};
    if (m_renderer) texture.resource = m_renderer->CreateRawTexture(width, height, linear);
    m_textures.insert_or_assign(std::string(id), std::move(texture));
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::UploadTexture(
    const std::string_view id, const std::uint8_t* pixels, const std::size_t bytes,
    const std::optional<Region> region) {
    const auto texture = m_textures.find(id);
    if (texture == m_textures.end()) return Unknown("texture", id);
    const Region area = region.value_or(Region{0, 0, texture->second.width, texture->second.height});
    if (area.x < 0 || area.y < 0 || area.w <= 0 || area.h <= 0 ||
        area.x > texture->second.width - area.w || area.y > texture->second.height - area.h)
        return RangeFailure("PXJS7802", "the region is outside the texture");
    const std::size_t expected = static_cast<std::size_t>(area.w) * area.h * 4U;
    if (bytes != expected)
        return TypeFailure("pixels hold " + std::to_string(expected) +
                           " bytes (RGBA8, region width x height x 4), not " + std::to_string(bytes));
    if (auto failure = CountUpload(bytes)) return failure;
    if (m_renderer && texture->second.resource)
        (void)m_renderer->UpdateRawTexture(
            texture->second.resource,
            Rect{static_cast<float>(area.x), static_cast<float>(area.y),
                 static_cast<float>(area.w), static_cast<float>(area.h)},
            pixels, static_cast<std::size_t>(area.w) * 4U);
    return std::nullopt;
}

bool ScriptRawGraphics::DestroyTexture(const std::string_view id) {
    const auto texture = m_textures.find(id);
    if (texture == m_textures.end()) return false;
    m_textures.erase(texture);
    return true;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::ApplyTargetOptions(
    Target& target, const Variant& options) const {
    if (options.Type() == VariantType::Null) return std::nullopt;
    const auto* object = options.AsObject();
    if (!object) return TypeFailure("raw target options must be an object");
    for (const auto& [key, value] : *object) {
        if (key == "clear") {
            const auto clear = ParseClear(value);
            if (!clear) return TypeFailure("clear must be {r, g, b, a} (0-255) or null");
            target.clear = *clear;
        } else if (key == "order") {
            const auto order = Number(value);
            if (!order || !std::isfinite(*order)) return TypeFailure("order must be a finite number");
            target.order = static_cast<std::int64_t>(*order);
        } else if (key == "filter") {
            const auto filter = ParseFilter(*object);
            if (!filter) return TypeFailure("filter must be linear or nearest");
            target.linear = *filter;
        } else {
            return TypeFailure("unknown raw target option: " + key);
        }
    }
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::CreateTarget(
    const std::string_view id, const int width, const int height,
    const Variant* options) {
    if (!ValidId(id)) return TypeFailure("raw target ids are 1 to 128 characters");
    if (width <= 0 || height <= 0 || width > m_limits.textureSize ||
        height > m_limits.textureSize)
        return RangeFailure("PXJS7804", "raw targets are 1 to " +
                                            std::to_string(m_limits.textureSize) +
                                            " pixels on each side");
    const auto existing = m_targets.find(id);
    if (existing == m_targets.end() && m_targets.size() >= m_limits.targets)
        return RangeFailure("PXJS7804", "an extension has at most " +
                                            std::to_string(m_limits.targets) + " raw targets");
    const std::size_t replaced = existing == m_targets.end() ? 0
        : static_cast<std::size_t>(existing->second.width) * existing->second.height * 4U;
    if (TextureBytes() - replaced + static_cast<std::size_t>(width) * height * 4U >
        m_limits.textureBytes)
        return RangeFailure("PXJS7804", "raw textures and targets of an extension hold at most " +
                                            std::to_string(m_limits.textureBytes / (1024U * 1024U)) +
                                            " MiB");
    Target target;
    target.sequence = existing == m_targets.end() ? ++m_sequence : existing->second.sequence;
    target.width = width;
    target.height = height;
    if (options) {
        if (auto failure = ApplyTargetOptions(target, *options)) return failure;
    }
    if (m_renderer) target.resource = m_renderer->CreateRawTarget(width, height, target.linear);
    m_targets.insert_or_assign(std::string(id), std::move(target));
    return std::nullopt;
}

ScriptRawGraphics::Outcome ScriptRawGraphics::UpdateTarget(
    const std::string_view id, const Variant& options, bool& found) {
    const auto target = m_targets.find(id);
    found = target != m_targets.end();
    if (!found) return std::nullopt;
    const bool linear = target->second.linear;
    if (auto failure = ApplyTargetOptions(target->second, options)) return failure;
    if (target->second.linear != linear) (void)target->second.resource.SetLinearSampling(target->second.linear);
    return std::nullopt;
}

bool ScriptRawGraphics::DestroyTarget(const std::string_view id) {
    const auto target = m_targets.find(id);
    if (target == m_targets.end()) return false;
    m_targets.erase(target);
    return true;
}

void ScriptRawGraphics::Clear() {
    m_meshes.clear();
    m_textures.clear();
    m_targets.clear();
}

void ScriptRawGraphics::BeginFrame() {
    ++m_frame;
    m_uploads = 0;
    m_uploadBytes = 0;
}

graphics::TextureHandle ScriptRawGraphics::Resolve(const Source& source) const {
    switch (source.kind) {
        case SourceKind::None: return {};
        case SourceKind::Asset:
            return m_renderer ? m_renderer->AssetTexture(source.value) : graphics::TextureHandle{};
        case SourceKind::Texture: {
            const auto texture = m_textures.find(source.value);
            return texture == m_textures.end() ? graphics::TextureHandle{} : texture->second.resource.Handle();
        }
        case SourceKind::Target: {
            const auto target = m_targets.find(source.value);
            return target == m_targets.end() ? graphics::TextureHandle{} : target->second.resource.Handle();
        }
    }
    return {};
}

bool ScriptRawGraphics::Draw(const Mesh& mesh, const bool coverage) {
    if (mesh.vertices.empty()) return true;
    const std::size_t indexCount = mesh.indices.empty() ? mesh.vertices.size() : mesh.indices.size();
    if (m_drawnVertices + mesh.vertices.size() > m_limits.verticesPerFrame ||
        m_drawnIndices + indexCount > m_limits.indicesPerFrame) {
        ++m_skippedMeshes;
        return false;
    }
    // A texture source that does not exist (yet) skips the mesh rather than
    // drawing it untextured.
    const auto texture = Resolve(mesh.texture);
    if (mesh.texture.kind != SourceKind::None && !texture) return false;
    m_drawnVertices += mesh.vertices.size();
    m_drawnIndices += indexCount;
    if (!m_renderer) {
        // Nothing applies an effect without a renderer.
        if (mesh.effect && !coverage) ++m_skippedEffects;
        return true;
    }
    if (mesh.indices.empty() && mesh.vertices.size() % 3 != 0) return false;
    // A mask's coverage is its own alpha: its effect does not apply there.
    if (mesh.effect && !coverage) DrawThroughEffect(mesh, texture);
    else DrawGeometry(mesh, texture, coverage);
    return true;
}

void ScriptRawGraphics::DrawGeometry(const Mesh& mesh, const graphics::TextureHandle texture,
                                     const bool coverage) {
    m_renderer->DrawRawGeometry(texture, mesh.vertices.data(),
                                static_cast<int>(mesh.vertices.size()),
                                mesh.indices.empty() ? nullptr : mesh.indices.data(),
                                mesh.indices.empty() ? 0 : static_cast<int>(mesh.indices.size()),
                                coverage ? graphics::RawBlendMode::Normal : mesh.blend,
                                mesh.transform);
}

Rect ScriptRawGraphics::ScreenBounds(const Mesh& mesh) const {
    float left = std::numeric_limits<float>::max(), top = left;
    float right = std::numeric_limits<float>::lowest(), bottom = right;
    for (const auto& vertex : mesh.vertices) {
        const auto& transform = mesh.transform;
        const Vec2 point = m_renderer->TransformPoint(
            {transform.a * vertex.x + transform.c * vertex.y + transform.tx,
             transform.b * vertex.x + transform.d * vertex.y + transform.ty});
        left = std::min(left, point.x);
        top = std::min(top, point.y);
        right = std::max(right, point.x);
        bottom = std::max(bottom, point.y);
    }
    if (right > left && bottom > top) return {left, top, right - left, bottom - top};
    // A degenerate mesh shades against the whole screen, as a Stage node does.
    int width = 0, height = 0;
    m_renderer->GetLogicalSize(width, height);
    return {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
}

void ScriptRawGraphics::DrawThroughEffect(const Mesh& mesh, const graphics::TextureHandle texture) {
    const auto& effect = *mesh.effect;
    // Parameters the renderer could not check when they were set (no effect
    // list then) are resolved now; an effect that is still unknown, or one
    // past the per-frame budget, draws the mesh without it.
    auto parameters = effect.resolved;
    if (!parameters && m_renderer->HasCustomEffect(effect.id, "node"))
        parameters = effect.parameters.empty()
                         ? m_renderer->CustomEffectDefaults(effect.id)
                         : m_renderer->ResolveCustomEffectParameters(effect.id, effect.parameters);
    if (!parameters || m_effectDraws >= m_limits.effectsPerFrame) {
        ++m_skippedEffects;
        DrawGeometry(mesh, texture, false);
        return;
    }
    ++m_effectDraws;
    const Rect viewport = ScreenBounds(mesh);
    if (texture) {
        // A Stage leaf's contract: the shader runs on the mesh's own draw and
        // samples its texture at the mesh's UVs. The mesh's blend composites
        // the result, so a render target (premultiplied, drawn "premultiplied")
        // keeps its alpha once.
        const bool active = m_renderer->BeginNodeEffect(effect.id, effect.progress, effect.seed,
                                                        *parameters, viewport);
        DrawGeometry(mesh, texture, false);
        if (active) m_renderer->EndNodeEffect();
        else ++m_skippedEffects;
        return;
    }
    // Vertex colors only, so there is no texture to sample: like a Stage
    // group, the mesh is drawn into a transparent screen-size layer and the
    // shader samples that layer (screen UVs), which then composites as the
    // premultiplied colors drawing into it left.
    if (!m_renderer->BeginComposition()) {
        ++m_skippedEffects;
        DrawGeometry(mesh, texture, false);
        return;
    }
    DrawGeometry(mesh, texture, false);
    if (!m_renderer->EndComposition(effect.id, effect.progress, effect.seed, *parameters,
                                    viewport, true))
        ++m_skippedEffects;
}

void ScriptRawGraphics::DrawMasked(const Mesh& mesh) {
    std::vector<const Mesh*> masks;
    for (const auto& id : mesh.masks) {
        const auto mask = m_meshes.find(id);
        if (mask == m_meshes.end() || !mask->second.target.empty()) {
            ++m_skippedMeshes;
            return;
        }
        masks.push_back(&mask->second);
    }
    if (!m_renderer) {
        if (Draw(mesh))
            for (const auto* mask : masks) (void)Draw(*mask, true);
        return;
    }
    // Never show a masked mesh unmasked: a failed mask skips it.
    if (!m_renderer->BeginAlphaMask()) {
        ++m_skippedMeshes;
        return;
    }
    bool covered = Draw(mesh) && m_renderer->BeginAlphaMaskCoverage();
    for (const auto* mask : masks) covered = covered && Draw(*mask, true);
    m_renderer->EndAlphaMask(covered, mesh.maskInverted);
}

void ScriptRawGraphics::RenderTargets() {
    std::vector<std::pair<const std::string*, const Target*>> targets;
    for (const auto& [id, target] : m_targets) targets.emplace_back(&id, &target);
    std::ranges::sort(targets, [](const auto& left, const auto& right) {
        return std::tie(left.second->order, left.second->sequence) <
               std::tie(right.second->order, right.second->sequence);
    });
    std::vector<const Mesh*> meshes;
    for (const auto& [targetId, target] : targets) {
        meshes.clear();
        for (const auto& [id, mesh] : m_meshes)
            if (mesh.visible && mesh.target == *targetId) meshes.push_back(&mesh);
        std::ranges::sort(meshes, [](const Mesh* left, const Mesh* right) {
            return std::tie(left->order, left->sequence) < std::tie(right->order, right->sequence);
        });
        if (!m_renderer) {
            for (const auto* mesh : meshes) (void)Draw(*mesh);
            continue;
        }
        if (!target->resource || !m_renderer->BeginRawTarget(target->resource.Handle(), target->clear))
            continue;
        for (const auto* mesh : meshes) (void)Draw(*mesh);
        m_renderer->EndRawTarget();
    }
}

void ScriptRawGraphics::Render(const RenderPhase phase, const std::string_view route,
                               const std::string_view modalRoute) {
    if (m_meshes.empty()) {
        m_lastPhase = phase;
        return;
    }
    if (!m_lastPhase || phase <= *m_lastPhase) {
        m_drawnVertices = 0;
        m_drawnIndices = 0;
        m_skippedMeshes = 0;
        m_effectDraws = 0;
        m_skippedEffects = 0;
        RenderTargets();
    }
    m_lastPhase = phase;
    std::vector<const Mesh*> meshes;
    for (const auto& [id, mesh] : m_meshes)
        if (mesh.visible && mesh.target.empty() && mesh.phase == phase &&
            (mesh.route.empty() || mesh.route == route || mesh.route == modalRoute))
            meshes.push_back(&mesh);
    std::ranges::sort(meshes, [](const Mesh* left, const Mesh* right) {
        return std::tie(left->order, left->sequence) < std::tie(right->order, right->sequence);
    });
    for (const auto* mesh : meshes) {
        if (mesh->masks.empty()) (void)Draw(*mesh);
        else DrawMasked(*mesh);
    }
}

}  // namespace px::script
