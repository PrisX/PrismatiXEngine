#include "cubism_bridge.h"

#include "cubism_core_api.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define CB_DEFAULT_CORE "Live2DCubismCore.dll"
static void* LibraryOpen(const char* path) { return (void*)LoadLibraryA(path); }
static void* LibrarySymbol(void* library, const char* name) { return (void*)GetProcAddress((HMODULE)library, name); }
static void LibraryClose(void* library) { FreeLibrary((HMODULE)library); }
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#define CB_DEFAULT_CORE "libLive2DCubismCore.dylib"
#else
#define CB_DEFAULT_CORE "libLive2DCubismCore.so"
#endif
static void* LibraryOpen(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
static void* LibrarySymbol(void* library, const char* name) { return dlsym(library, name); }
static void LibraryClose(void* library) { dlclose(library); }
#endif

/* Limits that keep a damaged or hostile model from exhausting memory. */
#define CB_MAX_DRAWABLES 20000
#define CB_MAX_PARAMETERS 4096
#define CB_MAX_VERTICES_PER_DRAWABLE 1000000
#define CB_MAX_MOC_BYTES (256u * 1024u * 1024u)
#define CB_VERTEX_FLOATS 8
#define CB_VERTICES_CHANGED (PX_CSM_VERTEX_POSITIONS_DID_CHANGE | PX_CSM_OPACITY_DID_CHANGE)

static void SetError(char* error, size_t error_size, const char* format, ...) {
    if (!error || error_size == 0) return;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(error, error_size, format, arguments);
    va_end(arguments);
}

/* ---- growable buffer -------------------------------------------------- */

void cb_buffer_free(CbBuffer* buffer) {
    if (!buffer) return;
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = buffer->capacity = 0;
}

static int Reserve(CbBuffer* buffer, size_t extra) {
    if (extra > (size_t)-1 - buffer->size) return 0;
    const size_t needed = buffer->size + extra;
    if (needed <= buffer->capacity) return 1;
    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed) capacity *= 2;
    uint8_t* grown = (uint8_t*)realloc(buffer->data, capacity);
    if (!grown) return 0;
    buffer->data = grown;
    buffer->capacity = capacity;
    return 1;
}

static int PutBytes(CbBuffer* buffer, const void* bytes, size_t size) {
    if (size == 0) return 1;
    if (!Reserve(buffer, size)) return 0;
    memcpy(buffer->data + buffer->size, bytes, size);
    buffer->size += size;
    return 1;
}

static int PutU32(CbBuffer* buffer, uint32_t value) { return PutBytes(buffer, &value, sizeof value); }

int cb_buffer_append(CbBuffer* buffer, const void* bytes, size_t size) { return PutBytes(buffer, bytes, size); }

int cb_buffer_append_u32(CbBuffer* buffer, uint32_t value) { return PutU32(buffer, value); }

static int PutPadding(CbBuffer* buffer, size_t multiple) {
    static const uint8_t zeros[8] = {0};
    const size_t rest = buffer->size % multiple;
    return rest == 0 ? 1 : PutBytes(buffer, zeros, multiple - rest);
}

static int PutText(CbBuffer* buffer, const char* text) { return PutBytes(buffer, text, strlen(text)); }

static int PutJsonString(CbBuffer* buffer, const char* text) {
    if (!PutText(buffer, "\"")) return 0;
    for (const unsigned char* c = (const unsigned char*)(text ? text : ""); *c; ++c) {
        char escaped[8];
        if (*c == '"' || *c == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)*c;
            escaped[2] = 0;
        } else if (*c < 0x20) {
            snprintf(escaped, sizeof escaped, "\\u%04x", *c);
        } else {
            escaped[0] = (char)*c;
            escaped[1] = 0;
        }
        if (!PutText(buffer, escaped)) return 0;
    }
    return PutText(buffer, "\"");
}

static int PutJsonNumber(CbBuffer* buffer, double value) {
    char text[40];
    if (!isfinite(value)) value = 0;
    snprintf(text, sizeof text, "%.9g", value);
    for (char* c = text; *c; ++c)
        if (*c == ',') *c = '.'; /* a host that called setlocale() */
    return PutText(buffer, text);
}

static int PutJsonInt(CbBuffer* buffer, long long value) {
    char text[32];
    snprintf(text, sizeof text, "%lld", value);
    return PutText(buffer, text);
}

/* ---- Core ------------------------------------------------------------- */

static struct {
    void* library;
    CubismCoreApi api;
    int loaded;
} g_core;

static int HostIsLittleEndian(void) {
    const uint32_t one = 1;
    return *(const uint8_t*)&one == 1;
}

int cb_core_loaded(void) { return g_core.loaded; }

unsigned int cb_core_version(void) { return g_core.loaded ? g_core.api.csmGetVersion() : 0; }

void cb_core_unload(void) {
    if (g_core.library) LibraryClose(g_core.library);
    memset(&g_core, 0, sizeof g_core);
}

int cb_core_load(const char* path, char* error, size_t error_size) {
    if (g_core.loaded) return 1;
    if (!HostIsLittleEndian()) {
        SetError(error, error_size, "this bridge's wire format is little-endian");
        return 0;
    }
    const char* name = path && *path ? path : CB_DEFAULT_CORE;
    void* library = LibraryOpen(name);
    if (!library) {
        SetError(error, error_size, "cannot load the Cubism Core library '%s'", name);
        return 0;
    }
    CubismCoreApi api;
    memset(&api, 0, sizeof api);
#define PX_CSM_RESOLVE(ret, symbol, args) \
    do { \
        void* address_ = LibrarySymbol(library, #symbol); \
        if (!address_) { \
            SetError(error, error_size, "'%s' does not export %s", name, #symbol); \
            LibraryClose(library); \
            return 0; \
        } \
        memcpy(&api.symbol, &address_, sizeof address_); \
    } while (0);
    PX_CSM_FUNCTIONS(PX_CSM_RESOLVE)
#undef PX_CSM_RESOLVE
    g_core.library = library;
    g_core.api = api;
    g_core.loaded = 1;
    return 1;
}

/* ---- model ------------------------------------------------------------ */

struct CbModel {
    void* moc_memory;
    csmMoc* moc;
    void* model_memory;
    csmModel* model;
    int parameter_count;
    int part_count;
    int drawable_count;
    int first_update;
    /* Arrays Core owns; they stay valid for the model's lifetime. */
    float* parameter_values;
    const float* parameter_minimum;
    const float* parameter_maximum;
    const float* parameter_default;
    float* part_opacities;
    const csmFlags* constant_flags;
    const csmFlags* dynamic_flags;
    const int* texture_indices;
    const int* render_orders;
    const float* opacities;
    const int* mask_counts;
    const int** masks;
    const int* vertex_counts;
    const csmVector2** positions;
    const csmVector2** uvs;
    const int* index_counts;
    const unsigned short** indices;
    const char** parameter_ids;
    const char** part_ids;
    const char** drawable_ids;
};

static void* AlignedAllocate(size_t alignment, size_t size) {
    void* memory = NULL;
#if defined(_WIN32)
    memory = _aligned_malloc(size, alignment);
#else
    if (posix_memalign(&memory, alignment, size) != 0) memory = NULL;
#endif
    if (memory) memset(memory, 0, size);
    return memory;
}

static void AlignedFree(void* memory) {
    if (!memory) return;
#if defined(_WIN32)
    _aligned_free(memory);
#else
    free(memory);
#endif
}

void cb_model_destroy(CbModel* model) {
    if (!model) return;
    AlignedFree(model->model_memory);
    AlignedFree(model->moc_memory);
    free(model);
}

CbModel* cb_model_create(const uint8_t* moc, size_t size, char* error, size_t error_size) {
    if (!g_core.loaded) {
        SetError(error, error_size, "Cubism Core is not loaded");
        return NULL;
    }
    if (!moc || size == 0 || size > CB_MAX_MOC_BYTES) {
        SetError(error, error_size, "the .moc3 is empty or larger than %u bytes", CB_MAX_MOC_BYTES);
        return NULL;
    }
    const CubismCoreApi* api = &g_core.api;
    CbModel* model = (CbModel*)calloc(1, sizeof *model);
    if (!model) {
        SetError(error, error_size, "out of memory");
        return NULL;
    }
    model->moc_memory = AlignedAllocate(PX_CSM_ALIGN_MOC, size);
    if (!model->moc_memory) {
        SetError(error, error_size, "out of memory");
        cb_model_destroy(model);
        return NULL;
    }
    memcpy(model->moc_memory, moc, size);
    const unsigned int version = api->csmGetMocVersion(model->moc_memory, (unsigned int)size);
    if (version == 0) {
        SetError(error, error_size, "this is not a valid .moc3 for this Core");
        cb_model_destroy(model);
        return NULL;
    }
    if (version > api->csmGetLatestMocVersion()) {
        SetError(error, error_size, "the .moc3 (version %u) needs a newer Core than 0x%08x", version,
                 api->csmGetVersion());
        cb_model_destroy(model);
        return NULL;
    }
    model->moc = api->csmReviveMocInPlace(model->moc_memory, (unsigned int)size);
    if (!model->moc) {
        SetError(error, error_size, "Core refused to revive the .moc3");
        cb_model_destroy(model);
        return NULL;
    }
    const unsigned int model_size = api->csmGetSizeofModel(model->moc);
    model->model_memory = model_size ? AlignedAllocate(PX_CSM_ALIGN_MODEL, model_size) : NULL;
    model->model = model->model_memory
                       ? api->csmInitializeModelInPlace(model->moc, model->model_memory, model_size)
                       : NULL;
    if (!model->model) {
        SetError(error, error_size, "Core could not initialize the model");
        cb_model_destroy(model);
        return NULL;
    }
    csmModel* m = model->model;
    model->parameter_count = api->csmGetParameterCount(m);
    model->part_count = api->csmGetPartCount(m);
    model->drawable_count = api->csmGetDrawableCount(m);
    if (model->parameter_count < 0 || model->parameter_count > CB_MAX_PARAMETERS || model->part_count < 0 ||
        model->part_count > CB_MAX_DRAWABLES || model->drawable_count < 0 ||
        model->drawable_count > CB_MAX_DRAWABLES) {
        SetError(error, error_size, "implausible counts from Core (%d parameters, %d parts, %d drawables)",
                 model->parameter_count, model->part_count, model->drawable_count);
        cb_model_destroy(model);
        return NULL;
    }
    model->parameter_values = api->csmGetParameterValues(m);
    model->parameter_minimum = api->csmGetParameterMinimumValues(m);
    model->parameter_maximum = api->csmGetParameterMaximumValues(m);
    model->parameter_default = api->csmGetParameterDefaultValues(m);
    model->parameter_ids = api->csmGetParameterIds(m);
    model->part_opacities = api->csmGetPartOpacities(m);
    model->part_ids = api->csmGetPartIds(m);
    model->constant_flags = api->csmGetDrawableConstantFlags(m);
    model->dynamic_flags = api->csmGetDrawableDynamicFlags(m);
    model->texture_indices = api->csmGetDrawableTextureIndices(m);
    model->render_orders = api->csmGetDrawableRenderOrders(m);
    model->opacities = api->csmGetDrawableOpacities(m);
    model->mask_counts = api->csmGetDrawableMaskCounts(m);
    model->masks = api->csmGetDrawableMasks(m);
    model->vertex_counts = api->csmGetDrawableVertexCounts(m);
    model->positions = api->csmGetDrawableVertexPositions(m);
    model->uvs = api->csmGetDrawableVertexUvs(m);
    model->index_counts = api->csmGetDrawableIndexCounts(m);
    model->indices = api->csmGetDrawableIndices(m);
    model->drawable_ids = api->csmGetDrawableIds(m);
    if ((model->parameter_count && (!model->parameter_values || !model->parameter_minimum || !model->parameter_maximum ||
                                    !model->parameter_default || !model->parameter_ids)) ||
        (model->part_count && (!model->part_opacities || !model->part_ids)) ||
        (model->drawable_count &&
         (!model->constant_flags || !model->dynamic_flags || !model->texture_indices || !model->render_orders ||
          !model->opacities || !model->mask_counts || !model->masks || !model->vertex_counts || !model->positions ||
          !model->uvs || !model->index_counts || !model->indices || !model->drawable_ids))) {
        SetError(error, error_size, "Core returned a null array for this model");
        cb_model_destroy(model);
        return NULL;
    }
    for (int i = 0; i < model->drawable_count; ++i) {
        if (model->vertex_counts[i] < 0 || model->vertex_counts[i] > CB_MAX_VERTICES_PER_DRAWABLE ||
            model->index_counts[i] < 0 || model->index_counts[i] > 3 * CB_MAX_VERTICES_PER_DRAWABLE ||
            model->mask_counts[i] < 0 || model->mask_counts[i] > model->drawable_count) {
            SetError(error, error_size, "drawable %d has implausible sizes", i);
            cb_model_destroy(model);
            return NULL;
        }
    }
    model->first_update = 1;
    return model;
}

int cb_model_describe(CbModel* model, CbBuffer* out, char* error, size_t error_size) {
    if (!model || !out) return 0;
    const CubismCoreApi* api = &g_core.api;
    csmVector2 size = {0, 0}, origin = {0, 0};
    float pixels_per_unit = 1;
    api->csmReadCanvasInfo(model->model, &size, &origin, &pixels_per_unit);

    CbBuffer json = {0};
    int ok = PutText(&json, "{\"coreVersion\":") && PutJsonInt(&json, api->csmGetVersion()) &&
             PutText(&json, ",\"canvas\":{\"w\":") && PutJsonNumber(&json, size.X) && PutText(&json, ",\"h\":") &&
             PutJsonNumber(&json, size.Y) && PutText(&json, ",\"ox\":") && PutJsonNumber(&json, origin.X) &&
             PutText(&json, ",\"oy\":") && PutJsonNumber(&json, origin.Y) && PutText(&json, ",\"ppu\":") &&
             PutJsonNumber(&json, pixels_per_unit) && PutText(&json, "},\"parameters\":[");
    for (int i = 0; ok && i < model->parameter_count; ++i) {
        ok = (i == 0 || PutText(&json, ",")) && PutText(&json, "{\"id\":") && PutJsonString(&json, model->parameter_ids[i]) &&
             PutText(&json, ",\"min\":") && PutJsonNumber(&json, model->parameter_minimum[i]) && PutText(&json, ",\"max\":") &&
             PutJsonNumber(&json, model->parameter_maximum[i]) && PutText(&json, ",\"def\":") &&
             PutJsonNumber(&json, model->parameter_default[i]) && PutText(&json, "}");
    }
    ok = ok && PutText(&json, "],\"parts\":[");
    for (int i = 0; ok && i < model->part_count; ++i)
        ok = (i == 0 || PutText(&json, ",")) && PutJsonString(&json, model->part_ids[i]);
    ok = ok && PutText(&json, "],\"drawables\":[");
    for (int i = 0; ok && i < model->drawable_count; ++i) {
        ok = (i == 0 || PutText(&json, ",")) && PutText(&json, "{\"id\":") && PutJsonString(&json, model->drawable_ids[i]) &&
             PutText(&json, ",\"texture\":") && PutJsonInt(&json, model->texture_indices[i]) && PutText(&json, ",\"flags\":") &&
             PutJsonInt(&json, model->constant_flags[i]) && PutText(&json, ",\"vc\":") &&
             PutJsonInt(&json, model->vertex_counts[i]) && PutText(&json, ",\"ic\":") &&
             PutJsonInt(&json, model->index_counts[i]) && PutText(&json, ",\"masks\":[");
        for (int k = 0; ok && k < model->mask_counts[i]; ++k)
            ok = (k == 0 || PutText(&json, ",")) && PutJsonInt(&json, model->masks[i][k]);
        ok = ok && PutText(&json, "]}");
    }
    ok = ok && PutText(&json, "]}") && json.size <= UINT32_MAX;
    if (ok) ok = PutU32(out, (uint32_t)json.size) && PutBytes(out, json.data, json.size) && PutPadding(out, 4);
    cb_buffer_free(&json);

    for (int i = 0; ok && i < model->drawable_count; ++i)
        ok = PutBytes(out, model->indices[i], (size_t)model->index_counts[i] * sizeof(unsigned short));
    if (!ok) SetError(error, error_size, "out of memory");
    return ok;
}

static float Clamp(float value, float minimum, float maximum) {
    if (!(value == value)) return minimum; /* NaN */
    return value < minimum ? minimum : value > maximum ? maximum : value;
}

int cb_model_update(CbModel* model, const uint8_t* request, size_t size, CbBuffer* out, char* error,
                    size_t error_size) {
    if (!model || !out) return 0;
    const CubismCoreApi* api = &g_core.api;
    uint32_t flags_in = 0, parameter_count = 0, part_count = 0;
    float transform[4];
    size_t cursor = 0;
    if (!request || size < sizeof flags_in + sizeof transform + sizeof parameter_count) {
        SetError(error, error_size, "update request is too short");
        return 0;
    }
    memcpy(&flags_in, request + cursor, sizeof flags_in);
    cursor += sizeof flags_in;
    memcpy(transform, request + cursor, sizeof transform);
    cursor += sizeof transform;
    memcpy(&parameter_count, request + cursor, sizeof parameter_count);
    cursor += sizeof parameter_count;
    if ((int64_t)parameter_count != model->parameter_count ||
        size - cursor < (size_t)parameter_count * sizeof(float) + sizeof(uint32_t)) {
        SetError(error, error_size, "update request has %u parameters, the model has %d", parameter_count,
                 model->parameter_count);
        return 0;
    }
    for (uint32_t i = 0; i < parameter_count; ++i) {
        float value;
        memcpy(&value, request + cursor + (size_t)i * sizeof value, sizeof value);
        model->parameter_values[i] = Clamp(value, model->parameter_minimum[i], model->parameter_maximum[i]);
    }
    cursor += (size_t)parameter_count * sizeof(float);
    memcpy(&part_count, request + cursor, sizeof part_count);
    cursor += sizeof part_count;
    if (part_count != 0) {
        if ((int64_t)part_count != model->part_count || size - cursor < (size_t)part_count * sizeof(float)) {
            SetError(error, error_size, "update request has %u part opacities, the model has %d parts", part_count,
                     model->part_count);
            return 0;
        }
        for (uint32_t i = 0; i < part_count; ++i) {
            float value;
            memcpy(&value, request + cursor + (size_t)i * sizeof value, sizeof value);
            model->part_opacities[i] = Clamp(value, 0.0f, 1.0f);
        }
    }

    api->csmUpdateModel(model->model);

    const uint32_t count = (uint32_t)model->drawable_count;
    /* Everything is new on the first update and when the script says the transform changed. */
    const uint8_t force = (model->first_update || (flags_in & CB_UPDATE_FORCE_ALL))
                              ? (PX_CSM_VISIBILITY_DID_CHANGE | PX_CSM_OPACITY_DID_CHANGE | PX_CSM_DRAW_ORDER_DID_CHANGE |
                                 PX_CSM_RENDER_ORDER_DID_CHANGE | PX_CSM_VERTEX_POSITIONS_DID_CHANGE)
                              : 0;
    int ok = PutU32(out, count);
    for (uint32_t i = 0; ok && i < count; ++i) {
        const uint8_t flags = (uint8_t)(model->dynamic_flags[i] | force);
        ok = PutBytes(out, &flags, 1);
    }
    ok = ok && PutPadding(out, 4) && PutBytes(out, model->render_orders, (size_t)count * sizeof(int));
    /* Vertices in the Player's raw mesh layout: x, y, u, v, r, g, b, a. */
    for (uint32_t i = 0; ok && i < count; ++i) {
        const uint8_t flags = (uint8_t)(model->dynamic_flags[i] | force);
        if ((flags & CB_VERTICES_CHANGED) == 0) continue;
        const int vertices = model->vertex_counts[i];
        ok = Reserve(out, (size_t)vertices * CB_VERTEX_FLOATS * sizeof(float));
        if (!ok) break;
        float* vertex = (float*)(out->data + out->size);
        const float alpha = model->opacities[i];
        for (int v = 0; v < vertices; ++v, vertex += CB_VERTEX_FLOATS) {
            const csmVector2 position = model->positions[i][v];
            const csmVector2 uv = model->uvs[i][v];
            vertex[0] = transform[0] * position.X + transform[2];
            vertex[1] = transform[1] * position.Y + transform[3];
            vertex[2] = uv.X;
            vertex[3] = 1.0f - uv.Y; /* Cubism's UV origin is bottom-left, the Player's top-left */
            vertex[4] = vertex[5] = vertex[6] = 1.0f;
            vertex[7] = alpha;
        }
        out->size += (size_t)vertices * CB_VERTEX_FLOATS * sizeof(float);
    }
    api->csmResetDrawableDynamicFlags(model->model);
    model->first_update = 0;
    if (!ok) SetError(error, error_size, "out of memory");
    return ok;
}
