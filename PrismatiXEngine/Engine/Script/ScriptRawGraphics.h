#pragma once

#include "Engine/Core/Variant.h"
#include "Engine/Graphics/Renderer2D.h"
#include "Engine/Graphics/Texture.h"
#include "Engine/Script/ScriptRenderTree.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace px::script {

// Every limit is per extension; uploads are counted per Update frame, drawn
// vertices and indices per rendered frame.
struct ScriptRawLimits {
    std::size_t verticesPerFrame = 65536;
    std::size_t indicesPerFrame = 196608;
    std::size_t uploadsPerFrame = 64;
    std::size_t uploadBytesPerFrame = 16U * 1024U * 1024U;
    std::size_t verticesPerMesh = 65536;
    std::size_t indicesPerMesh = 196608;
    std::size_t meshes = 512;
    std::size_t textures = 64;
    std::size_t targets = 8;
    int textureSize = 4096;
    // Writable textures and render targets together, RGBA8.
    std::size_t textureBytes = 64U * 1024U * 1024U;
};

// One extension's layer-1 drawing resources (@prismatix/runtime/raw): meshes,
// writable textures and render targets. They are presentation state only:
// nothing here is saved, journaled or rolled back, and a load keeps them.
class ScriptRawGraphics {
public:
    using Limits = ScriptRawLimits;

    struct Failure {
        // TypeError (bad arguments) or RangeError (limits, unknown ids).
        bool range = false;
        std::string code;
        std::string message;
    };
    using Outcome = std::optional<Failure>;

    struct Region {
        int x = 0;
        int y = 0;
        int w = 0;
        int h = 0;
    };

    explicit ScriptRawGraphics(graphics::Renderer2D* renderer,
                               Limits limits = {})
        : m_renderer(renderer), m_limits(limits) {}

    [[nodiscard]] const Limits& GetLimits() const { return m_limits; }
    [[nodiscard]] Variant LimitsValue() const;
    [[nodiscard]] Variant StatsValue() const;

    Outcome CreateMesh(std::string_view id, const Variant* options);
    // nullopt result + false: no such mesh.
    Outcome UpdateMesh(std::string_view id, const Variant& options,
                       bool& found);
    Outcome UploadVertices(std::string_view id, const float* data,
                           std::size_t floatCount,
                           std::optional<std::size_t> firstVertex);
    Outcome UploadIndices(std::string_view id, const std::uint16_t* data16,
                          const std::uint32_t* data32, std::size_t count);
    bool DestroyMesh(std::string_view id);

    Outcome CreateTexture(std::string_view id, int width, int height,
                          const Variant* options);
    Outcome UploadTexture(std::string_view id, const std::uint8_t* pixels,
                          std::size_t bytes, std::optional<Region> region);
    bool DestroyTexture(std::string_view id);

    Outcome CreateTarget(std::string_view id, int width, int height,
                         const Variant* options);
    Outcome UpdateTarget(std::string_view id, const Variant& options,
                         bool& found);
    bool DestroyTarget(std::string_view id);

    void Clear();
    // Starts an Update frame: upload counters restart.
    void BeginFrame();
    // Draws the screen meshes of `phase`. The first phase of a rendered frame
    // (a phase not after the previous one) first renders every target.
    void Render(RenderPhase phase, std::string_view route,
                std::string_view modalRoute);

    [[nodiscard]] std::size_t MeshCount() const { return m_meshes.size(); }
    [[nodiscard]] std::size_t TextureCount() const { return m_textures.size(); }
    [[nodiscard]] std::size_t TargetCount() const { return m_targets.size(); }

private:
    enum class SourceKind : std::uint8_t { None, Asset, Texture, Target };
    struct Source {
        SourceKind kind = SourceKind::None;
        std::string value;
    };
    struct Mesh {
        std::uint64_t sequence = 0;
        RenderPhase phase = RenderPhase::AfterStage;
        std::int64_t order = 0;
        bool visible = true;
        std::string route;
        Source texture;
        graphics::RawBlendMode blend = graphics::RawBlendMode::Normal;
        std::string mask;
        std::string target;
        graphics::AffineTransform2D transform;
        std::vector<graphics::RawVertex> vertices;
        std::vector<int> indices;
    };
    struct Texture {
        int width = 0;
        int height = 0;
        bool linear = true;
        graphics::TextureResource resource;
    };
    struct Target {
        std::uint64_t sequence = 0;
        int width = 0;
        int height = 0;
        bool linear = true;
        std::int64_t order = 0;
        std::optional<Color> clear = Color{0, 0, 0, 0};
        graphics::TextureResource resource;
    };

    Outcome ApplyMeshOptions(Mesh& mesh, std::string_view id,
                             const Variant& options) const;
    Outcome ApplyTargetOptions(Target& target, const Variant& options) const;
    Outcome CountUpload(std::size_t bytes);
    [[nodiscard]] std::size_t TextureBytes() const;
    [[nodiscard]] graphics::TextureHandle Resolve(const Source& source) const;
    bool Draw(const Mesh& mesh);
    void RenderTargets();

    graphics::Renderer2D* m_renderer = nullptr;
    Limits m_limits;
    std::map<std::string, Mesh, std::less<>> m_meshes;
    std::map<std::string, Texture, std::less<>> m_textures;
    std::map<std::string, Target, std::less<>> m_targets;
    std::uint64_t m_sequence = 0;
    std::uint64_t m_frame = 0;
    std::size_t m_uploads = 0;
    std::size_t m_uploadBytes = 0;
    std::size_t m_drawnVertices = 0;
    std::size_t m_drawnIndices = 0;
    std::size_t m_skippedMeshes = 0;
    std::optional<RenderPhase> m_lastPhase;
};

}  // namespace px::script
