#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace px::sdk {

enum class MotionScaleMode { Fit, Fill, Stretch };
enum class MotionProperty { X, Y, ScaleX, ScaleY, Rotation, Opacity, Visible, Frame };
enum class MotionCurve { Step, Linear, EaseIn, EaseOut, EaseInOut, CubicBezier };
enum class MotionAudioBus { SE, Voice };
enum class MotionBlendMode { Normal, Additive, Multiply };

struct MotionKey {
    double time = 0;
    double value = 0;
    bool visible = true;
    std::size_t frame = 0;
    MotionCurve curve = MotionCurve::Linear;
    // Timing cubic: x is normalized time, y is normalized value. Overshoot
    // is permitted so authored motion can express elastic source curves.
    double bezierX1 = 0, bezierY1 = 0, bezierX2 = 1, bezierY2 = 1;
};

struct MotionAtlasFrame {
    int x = 0, y = 0, width = 0, height = 0;
};

struct MotionLayer {
    std::string id;
    std::string image;
    std::vector<MotionAtlasFrame> frames;
    std::optional<std::size_t> parent;
    int z = 0;
    MotionBlendMode blendMode = MotionBlendMode::Normal;
    double width = 0, height = 0;
    double anchorX = 0, anchorY = 0;
    double x = 0, y = 0;
    double scaleX = 1, scaleY = 1;
    double rotation = 0, opacity = 1;
    bool visible = true;
};

struct MotionTrack {
    std::size_t layer = 0;
    MotionProperty property = MotionProperty::X;
    std::vector<MotionKey> keys;
};

struct MotionAudioMarker {
    std::string id;
    double time = 0;
    std::string audio;
    MotionAudioBus bus = MotionAudioBus::SE;
};

struct MotionClip {
    std::string id;
    int schemaRevision = 1;
    double canvasWidth = 0, canvasHeight = 0;
    double duration = 0;
    bool loop = false;
    MotionScaleMode scaleMode = MotionScaleMode::Fit;
    std::vector<MotionLayer> layers;
    std::vector<MotionTrack> tracks;
    std::vector<MotionAudioMarker> markers;
    // Precomputed once while parsing, not re-sorted or graph-walked each frame.
    std::vector<std::size_t> hierarchyOrder;
    std::vector<std::size_t> drawOrder;
};

struct MotionDiagnostic {
    std::string code;
    std::string message;
    std::string path;
};

struct MotionParseResult {
    std::optional<MotionClip> clip;
    std::vector<MotionDiagnostic> diagnostics;
    [[nodiscard]] bool Valid() const { return clip.has_value() && diagnostics.empty(); }
};

[[nodiscard]] MotionParseResult ParseMotion(std::string_view json,
                                           std::string sourcePath = {});
[[nodiscard]] std::vector<std::string> MotionDependencies(const MotionClip& clip);
[[nodiscard]] std::vector<std::string> MotionAudioDependencies(const MotionClip& clip);

struct MotionAffine {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

struct MotionDrawLayer {
    std::size_t layer = 0;
    MotionAffine transform;
    std::optional<MotionAtlasFrame> sourcePixels;
    double opacity = 1;
    bool visible = true;
};

// Owns only sampling scratch. The immutable clip must outlive the sampler.
// Seek is deterministic, allocation-free after construction, and emits no
// markers or sounds (those belong to the playback integration layer).
class MotionSampler {
public:
    explicit MotionSampler(const MotionClip& clip);
    void Seek(double timeSeconds);
    [[nodiscard]] double Position() const { return m_position; }
    [[nodiscard]] const std::vector<MotionDrawLayer>& Pose() const { return m_pose; }

private:
    struct State {
        double x = 0, y = 0, scaleX = 1, scaleY = 1;
        double rotation = 0, opacity = 1;
        bool visible = true;
        std::size_t frame = 0;
    };
    const MotionClip& m_clip;
    double m_position = 0;
    std::vector<State> m_states;
    std::vector<MotionDrawLayer> m_world;
    std::vector<MotionDrawLayer> m_pose;
};

}  // namespace px::sdk
