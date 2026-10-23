/*
 * A stand-in for Cubism Core used by the tests: the same exported csm*
 * functions over a tiny fixed model, so the bridge, the plugin and the
 * script's wire format run without Live2D's proprietary library. It is not a
 * moc3 reader. A "moc" here is the 8 bytes "MOCK" + u32 version.
 *
 * The model (canvas 200x400 px, origin 100,200, 100 px per unit):
 *   parameters ParamAngleX (-30..30, 0) and ParamOpacity (0..1, 1)
 *   part       PartBody
 *   drawables  0 Body   texture 0, normal
 *              1 Glow   texture 0, additive, masked by drawable 3, hidden at ParamOpacity 0
 *              2 Cheek  texture 1, multiplicative
 *              3 Mask   texture 0
 *   ParamAngleX shifts drawables 0 and 1 by 0.01 unit per degree and, past 15,
 *   draws Cheek below Body (render orders swap).
 */
#include "../cubism_core_api.h"

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#define MOCK_EXPORT __declspec(dllexport)
#else
#define MOCK_EXPORT __attribute__((visibility("default")))
#endif

enum { kParameters = 2, kParts = 1, kDrawables = 4, kVertices = 4, kIndices = 6, kMaxMasks = 1 };

typedef struct MockModel {
    float parameterValues[kParameters];
    float partOpacities[kParts];
    csmFlags dynamicFlags[kDrawables];
    int renderOrders[kDrawables];
    float opacities[kDrawables];
    csmVector2 positions[kDrawables][kVertices];
    const csmVector2* positionPointers[kDrawables];
    csmVector2 uvs[kDrawables][kVertices];
    const csmVector2* uvPointers[kDrawables];
    unsigned short indices[kDrawables][kIndices];
    const unsigned short* indexPointers[kDrawables];
    int maskArray[kMaxMasks];
    const int* maskPointers[kDrawables];
    float previousAngle;
    float previousOpacity;
    int previousVisible[kDrawables];
    int previousOrder[kDrawables];
    int updated;
} MockModel;

static const char* const kParameterIds[kParameters] = {"ParamAngleX", "ParamOpacity"};
static const float kMinimum[kParameters] = {-30.0f, 0.0f};
static const float kMaximum[kParameters] = {30.0f, 1.0f};
static const float kDefault[kParameters] = {0.0f, 1.0f};
static const char* const kPartIds[kParts] = {"PartBody"};
static const char* const kDrawableIds[kDrawables] = {"Body", "Glow", "Cheek", "Mask"};
static const csmFlags kConstantFlags[kDrawables] = {0, PX_CSM_BLEND_ADDITIVE, PX_CSM_BLEND_MULTIPLICATIVE, 0};
static const int kTextures[kDrawables] = {0, 0, 1, 0};
static const int kMaskCounts[kDrawables] = {0, 1, 0, 0};
static const int kVertexCounts[kDrawables] = {kVertices, kVertices, kVertices, kVertices};
static const int kIndexCounts[kDrawables] = {kIndices, kIndices, kIndices, kIndices};
/* Left, bottom, right, top in units. */
static const float kBounds[kDrawables][4] = {{-1, -2, 1, 2}, {-0.5f, -1, 0.5f, 1}, {-0.4f, -0.4f, 0.4f, 0.4f}, {-0.5f, -1, 0.5f, 1}};

static MockModel* Mock(csmModel* model) { return (MockModel*)model; }
static const MockModel* MockConst(const csmModel* model) { return (const MockModel*)model; }

MOCK_EXPORT unsigned int csmGetVersion(void) { return 0x05000000u; }
MOCK_EXPORT unsigned int csmGetLatestMocVersion(void) { return 1u; }

MOCK_EXPORT unsigned int csmGetMocVersion(const void* address, const unsigned int size) {
    if (!address || size < 8 || memcmp(address, "MOCK", 4) != 0) return 0u;
    uint32_t version;
    memcpy(&version, (const uint8_t*)address + 4, sizeof version);
    return version;
}

MOCK_EXPORT csmMoc* csmReviveMocInPlace(void* address, const unsigned int size) {
    /* Core requires this alignment; the mock enforces it so a bridge that forgets fails here. */
    if (!address || ((uintptr_t)address % PX_CSM_ALIGN_MOC) != 0 || size < 8) return NULL;
    return (csmMoc*)address;
}

MOCK_EXPORT unsigned int csmGetSizeofModel(const csmMoc* moc) {
    (void)moc;
    return (unsigned int)sizeof(MockModel);
}

MOCK_EXPORT csmModel* csmInitializeModelInPlace(const csmMoc* moc, void* address, const unsigned int size) {
    (void)moc;
    if (!address || ((uintptr_t)address % PX_CSM_ALIGN_MODEL) != 0 || size < sizeof(MockModel)) return NULL;
    MockModel* model = (MockModel*)address;
    memset(model, 0, sizeof *model);
    for (int p = 0; p < kParameters; ++p) model->parameterValues[p] = kDefault[p];
    model->partOpacities[0] = 1.0f;
    model->previousAngle = 1e9f; /* never equal: the first update changes everything */
    model->previousOpacity = -1.0f;
    for (int d = 0; d < kDrawables; ++d) {
        model->positionPointers[d] = model->positions[d];
        model->uvPointers[d] = model->uvs[d];
        model->indexPointers[d] = model->indices[d];
        model->maskPointers[d] = kMaskCounts[d] ? model->maskArray : NULL;
        model->opacities[d] = 1.0f;
        model->renderOrders[d] = d;
        model->previousVisible[d] = -1;
        model->previousOrder[d] = -1;
        /* Cubism UVs have a bottom-left origin. */
        const csmVector2 uv[kVertices] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        memcpy(model->uvs[d], uv, sizeof uv);
        const unsigned short quad[kIndices] = {0, 1, 2, 0, 2, 3};
        memcpy(model->indices[d], quad, sizeof quad);
    }
    model->maskArray[0] = 3;
    return (csmModel*)model;
}

MOCK_EXPORT void csmUpdateModel(csmModel* handle) {
    MockModel* model = Mock(handle);
    const float angle = model->parameterValues[0];
    const float glow = model->parameterValues[1];
    const int swapped = angle > 15.0f;
    for (int d = 0; d < kDrawables; ++d) {
        const float shift = d < 2 ? angle * 0.01f : 0.0f;
        const float* b = kBounds[d];
        const csmVector2 corners[kVertices] = {{b[0] + shift, b[1]}, {b[2] + shift, b[1]}, {b[2] + shift, b[3]}, {b[0] + shift, b[3]}};
        memcpy(model->positions[d], corners, sizeof corners);
        model->opacities[d] = d == 1 ? glow : 1.0f;
        model->renderOrders[d] = d == 0 ? (swapped ? 2 : 0) : d == 2 ? (swapped ? 0 : 2) : d;
        const int visible = d == 1 ? glow > 0.0f : 1;
        csmFlags flags = (csmFlags)(visible ? PX_CSM_IS_VISIBLE : 0);
        if (model->previousVisible[d] != visible) flags |= PX_CSM_VISIBILITY_DID_CHANGE;
        if (d == 1 && model->previousOpacity != glow) flags |= PX_CSM_OPACITY_DID_CHANGE;
        if (model->previousOrder[d] != model->renderOrders[d]) flags |= PX_CSM_RENDER_ORDER_DID_CHANGE;
        if (d < 2 && model->previousAngle != angle) flags |= PX_CSM_VERTEX_POSITIONS_DID_CHANGE;
        if (!model->updated) flags |= PX_CSM_VERTEX_POSITIONS_DID_CHANGE;
        model->dynamicFlags[d] = flags;
        model->previousVisible[d] = visible;
        model->previousOrder[d] = model->renderOrders[d];
    }
    model->previousAngle = angle;
    model->previousOpacity = glow;
    model->updated = 1;
}

MOCK_EXPORT void csmReadCanvasInfo(const csmModel* model, csmVector2* size, csmVector2* origin, float* pixelsPerUnit) {
    (void)model;
    size->X = 200.0f;
    size->Y = 400.0f;
    origin->X = 100.0f;
    origin->Y = 200.0f;
    *pixelsPerUnit = 100.0f;
}

MOCK_EXPORT int csmGetParameterCount(const csmModel* model) { return (void)model, kParameters; }
MOCK_EXPORT const char** csmGetParameterIds(const csmModel* model) { return (void)model, (const char**)kParameterIds; }
MOCK_EXPORT const float* csmGetParameterMinimumValues(const csmModel* model) { return (void)model, kMinimum; }
MOCK_EXPORT const float* csmGetParameterMaximumValues(const csmModel* model) { return (void)model, kMaximum; }
MOCK_EXPORT const float* csmGetParameterDefaultValues(const csmModel* model) { return (void)model, kDefault; }
MOCK_EXPORT float* csmGetParameterValues(csmModel* model) { return Mock(model)->parameterValues; }
MOCK_EXPORT int csmGetPartCount(const csmModel* model) { return (void)model, kParts; }
MOCK_EXPORT const char** csmGetPartIds(const csmModel* model) { return (void)model, (const char**)kPartIds; }
MOCK_EXPORT float* csmGetPartOpacities(csmModel* model) { return Mock(model)->partOpacities; }
MOCK_EXPORT int csmGetDrawableCount(const csmModel* model) { return (void)model, kDrawables; }
MOCK_EXPORT const char** csmGetDrawableIds(const csmModel* model) { return (void)model, (const char**)kDrawableIds; }
MOCK_EXPORT const csmFlags* csmGetDrawableConstantFlags(const csmModel* model) { return (void)model, kConstantFlags; }
MOCK_EXPORT const csmFlags* csmGetDrawableDynamicFlags(const csmModel* model) { return MockConst(model)->dynamicFlags; }
MOCK_EXPORT const int* csmGetDrawableTextureIndices(const csmModel* model) { return (void)model, kTextures; }
MOCK_EXPORT const int* csmGetDrawableRenderOrders(const csmModel* model) { return MockConst(model)->renderOrders; }
MOCK_EXPORT const float* csmGetDrawableOpacities(const csmModel* model) { return MockConst(model)->opacities; }
MOCK_EXPORT const int* csmGetDrawableMaskCounts(const csmModel* model) { return (void)model, kMaskCounts; }
MOCK_EXPORT const int** csmGetDrawableMasks(const csmModel* model) { return (const int**)MockConst(model)->maskPointers; }
MOCK_EXPORT const int* csmGetDrawableVertexCounts(const csmModel* model) { return (void)model, kVertexCounts; }
MOCK_EXPORT const csmVector2** csmGetDrawableVertexPositions(const csmModel* model) {
    return (const csmVector2**)MockConst(model)->positionPointers;
}
MOCK_EXPORT const csmVector2** csmGetDrawableVertexUvs(const csmModel* model) {
    return (const csmVector2**)MockConst(model)->uvPointers;
}
MOCK_EXPORT const int* csmGetDrawableIndexCounts(const csmModel* model) { return (void)model, kIndexCounts; }
MOCK_EXPORT const unsigned short** csmGetDrawableIndices(const csmModel* model) {
    return (const unsigned short**)MockConst(model)->indexPointers;
}

MOCK_EXPORT void csmResetDrawableDynamicFlags(csmModel* handle) {
    MockModel* model = Mock(handle);
    for (int d = 0; d < kDrawables; ++d) model->dynamicFlags[d] &= PX_CSM_IS_VISIBLE;
}
