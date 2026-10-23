/*
 * PrismatiX native extension (layer 2) that exposes Cubism Core as four
 * services. The script owns everything that decides how a model looks (motions,
 * expressions, draw batching, the meshes); this library only runs Core, which
 * a script cannot.
 *
 *   live2d.core.load      request: UTF-8 path of the Core library (may be empty)
 *   live2d.model.create   request: the .moc3 bytes
 *   live2d.model.update   request: u32 handle + the update request
 *   live2d.model.release  request: u32 handle
 *
 * Every response starts with u32 status: 0 and the payload, or 1 and a UTF-8
 * message. See cubism_bridge.h for the payloads.
 */
#define PX_PLUGIN_BUILD 1
#include "Engine/Native/PxPluginAbi.h"

#include "cubism_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_MODELS 64u

typedef struct Slot {
    CbModel* model;
    uint32_t generation;
} Slot;

static const PxHostApi* g_host = NULL;
static Slot g_slots[MAX_MODELS];
static uint32_t g_generation = 0;

static PxStatus Respond(PxResponse* response, CbBuffer* body) {
    const PxStatus status = g_host->respond(response, body->data, body->size);
    cb_buffer_free(body);
    return status;
}

/* A status-1 response: the script gets the message instead of a bare failure. */
static PxStatus Refuse(PxResponse* response, const char* message) {
    const uint32_t failed = 1;
    uint8_t* bytes = NULL;
    const size_t length = strlen(message);
    bytes = (uint8_t*)malloc(sizeof failed + length);
    if (!bytes) return PX_ERROR_FAILED;
    memcpy(bytes, &failed, sizeof failed);
    memcpy(bytes + sizeof failed, message, length);
    const PxStatus status = g_host->respond(response, bytes, sizeof failed + length);
    free(bytes);
    g_host->log(g_host->host, PX_LOG_WARNING, message);
    return status;
}

static int BeginOk(CbBuffer* body) {
    memset(body, 0, sizeof *body);
    return cb_buffer_append_u32(body, 0);
}

static uint32_t Register(CbModel* model) {
    for (uint32_t index = 0; index < MAX_MODELS; ++index) {
        if (g_slots[index].model) continue;
        g_slots[index].model = model;
        g_generation = (g_generation + 1) & 0xFFFFFFu;
        if (g_generation == 0) g_generation = 1; /* handle 0 means "none" */
        g_slots[index].generation = g_generation;
        return (g_generation << 8) | index;
    }
    return 0;
}

static CbModel* Lookup(uint32_t handle) {
    const uint32_t index = handle & 0xFFu;
    if (index >= MAX_MODELS || !g_slots[index].model || g_slots[index].generation != (handle >> 8)) return NULL;
    return g_slots[index].model;
}

static void Forget(uint32_t handle) {
    const uint32_t index = handle & 0xFFu;
    if (!Lookup(handle)) return;
    cb_model_destroy(g_slots[index].model);
    g_slots[index].model = NULL;
}

static PxStatus CoreLoad(void* user, const uint8_t* request, size_t size, PxResponse* response) {
    (void)user;
    char path[1024];
    char error[512] = {0};
    if (size >= sizeof path) return Refuse(response, "the Core path is too long");
    memcpy(path, request, size);
    path[size] = 0;
    if (!cb_core_load(path, error, sizeof error)) return Refuse(response, error);
    CbBuffer body = {0};
    char json[64];
    snprintf(json, sizeof json, "{\"coreVersion\":%u}", cb_core_version());
    if (!BeginOk(&body) || !cb_buffer_append(&body, json, strlen(json))) {
        cb_buffer_free(&body);
        return PX_ERROR_FAILED;
    }
    return Respond(response, &body);
}

static PxStatus ModelCreate(void* user, const uint8_t* request, size_t size, PxResponse* response) {
    (void)user;
    char error[512] = {0};
    CbModel* model = cb_model_create(request, size, error, sizeof error);
    if (!model) return Refuse(response, error);
    const uint32_t handle = Register(model);
    if (handle == 0) {
        cb_model_destroy(model);
        return Refuse(response, "too many Live2D models are loaded at once");
    }
    CbBuffer body = {0};
    if (!BeginOk(&body)) {
        Forget(handle);
        return PX_ERROR_FAILED;
    }
    if (!cb_buffer_append_u32(&body, handle) || !cb_model_describe(model, &body, error, sizeof error)) {
        cb_buffer_free(&body);
        Forget(handle);
        return Refuse(response, error[0] ? error : "out of memory");
    }
    return Respond(response, &body);
}

static PxStatus ModelUpdate(void* user, const uint8_t* request, size_t size, PxResponse* response) {
    (void)user;
    char error[512] = {0};
    uint32_t handle = 0;
    if (size < sizeof handle) return Refuse(response, "update request is too short");
    memcpy(&handle, request, sizeof handle);
    CbModel* model = Lookup(handle);
    if (!model) return Refuse(response, "unknown Live2D model handle");
    CbBuffer body = {0};
    if (!BeginOk(&body)) return PX_ERROR_FAILED;
    if (!cb_model_update(model, request + sizeof handle, size - sizeof handle, &body, error, sizeof error)) {
        cb_buffer_free(&body);
        return Refuse(response, error[0] ? error : "update failed");
    }
    return Respond(response, &body);
}

static PxStatus ModelRelease(void* user, const uint8_t* request, size_t size, PxResponse* response) {
    (void)user;
    uint32_t handle = 0;
    if (size < sizeof handle) return Refuse(response, "release request is too short");
    memcpy(&handle, request, sizeof handle);
    Forget(handle);
    CbBuffer body = {0};
    if (!BeginOk(&body)) return PX_ERROR_FAILED;
    return Respond(response, &body);
}

PX_PLUGIN_EXPORT uint32_t px_plugin_abi_version(void) { return PX_PLUGIN_ABI_VERSION; }

PX_PLUGIN_EXPORT PxStatus px_plugin_init(const PxHostApi* host) {
    if (!host || host->abi_version >> 16 != PX_PLUGIN_ABI_MAJOR || host->struct_size < sizeof(PxHostApi))
        return PX_ERROR_ABI;
    g_host = host;
    if (host->register_service(host->host, "live2d.core.load", &CoreLoad, NULL) != PX_OK ||
        host->register_service(host->host, "live2d.model.create", &ModelCreate, NULL) != PX_OK ||
        host->register_service(host->host, "live2d.model.update", &ModelUpdate, NULL) != PX_OK ||
        host->register_service(host->host, "live2d.model.release", &ModelRelease, NULL) != PX_OK)
        return PX_ERROR_FAILED;
    return PX_OK;
}

PX_PLUGIN_EXPORT void px_plugin_shutdown(void) {
    for (uint32_t index = 0; index < MAX_MODELS; ++index) {
        cb_model_destroy(g_slots[index].model);
        g_slots[index].model = NULL;
    }
    cb_core_unload();
    g_host = NULL;
}
