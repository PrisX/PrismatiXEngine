/*
 * Wraps Cubism Core for the PrismatiX plugin: loads Core at run time, revives
 * a .moc3 and answers the two calls a renderer needs, "describe" (what never
 * changes) and "update" (what changes every frame). No PrismatiX types here,
 * so the unit tests and cubism_smoke use it directly.
 *
 * Wire format (little-endian, which every platform the Player loads native
 * libraries for is):
 *
 *   describe response   u32 jsonLength | json | zero padding to 4 | blob
 *     blob              u16 index[indexCount] per drawable, in drawable order
 *   update request      u32 flags (CB_UPDATE_FORCE_ALL) | f32 ax, ay, tx, ty
 *                       | u32 parameterCount | f32 value[parameterCount]
 *                       | u32 partCount (0: leave parts alone) | f32 opacity[partCount]
 *     (ax, ay, tx, ty) maps model units to screen pixels:
 *     x' = ax * x + tx, y' = ay * y + ty.
 *   update response     u32 drawableCount | u8 dynamicFlags[n] padded to 4
 *                       | i32 renderOrder[n]
 *                       | f32 vertex[8 * vertexCount] for each drawable whose
 *                         dynamicFlags has PX_CSM_VERTEX_POSITIONS_DID_CHANGE
 *                         or PX_CSM_OPACITY_DID_CHANGE, in drawable order
 *     A vertex is x, y, u, v, r, g, b, a: the layout of the Player's raw meshes,
 *     already transformed, with the texture's v flipped and the drawable's
 *     opacity in a, so the script copies it without touching a float.
 */
#ifndef PX_CUBISM_BRIDGE_H
#define PX_CUBISM_BRIDGE_H

/* update request flags */
#define CB_UPDATE_FORCE_ALL 1u

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CbBuffer {
    uint8_t* data;
    size_t size;
    size_t capacity;
} CbBuffer;

void cb_buffer_free(CbBuffer* buffer);
/* Both return 0 when memory runs out. */
int cb_buffer_append(CbBuffer* buffer, const void* bytes, size_t size);
int cb_buffer_append_u32(CbBuffer* buffer, uint32_t value);

/* Loads Core from `path` (empty or NULL: the platform's default library name). */
int cb_core_load(const char* path, char* error, size_t error_size);
int cb_core_loaded(void);
unsigned int cb_core_version(void);
void cb_core_unload(void);

typedef struct CbModel CbModel;

/* `moc` is copied; the caller keeps ownership of its buffer. */
CbModel* cb_model_create(const uint8_t* moc, size_t size, char* error, size_t error_size);
void cb_model_destroy(CbModel* model);

/* Appends the describe response to `out`. */
int cb_model_describe(CbModel* model, CbBuffer* out, char* error, size_t error_size);
/* Applies the request, updates the model and appends the update response to `out`. */
int cb_model_update(CbModel* model, const uint8_t* request, size_t size, CbBuffer* out,
                    char* error, size_t error_size);

#ifdef __cplusplus
}
#endif

#endif /* PX_CUBISM_BRIDGE_H */
