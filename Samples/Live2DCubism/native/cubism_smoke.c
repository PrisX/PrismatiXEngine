/*
 * cubism_smoke <Core library> <model.moc3>
 *
 * Loads your Cubism Core and a real .moc3 through the same bridge the plugin
 * uses, runs one update and prints what Core returned. Run it once before
 * trusting the plugin with your Core: the bridge calls Core through function
 * pointers it declares itself (cubism_core_api.h), so a prototype that does
 * not match your Live2DCubismCore.h shows up here, not at compile time.
 */
#include "cubism_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t* ReadFile(const char* path, size_t* size) {
    FILE* file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (length <= 0) {
        fclose(file);
        return NULL;
    }
    uint8_t* bytes = (uint8_t*)malloc((size_t)length);
    if (bytes && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    *size = (size_t)length;
    return bytes;
}

static uint32_t U32(const uint8_t* at) {
    uint32_t value;
    memcpy(&value, at, sizeof value);
    return value;
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: cubism_smoke <Core library> <model.moc3>\n");
        return 2;
    }
    char error[512] = {0};
    if (!cb_core_load(argv[1], error, sizeof error)) {
        fprintf(stderr, "core: %s\n", error);
        return 1;
    }
    printf("Core version 0x%08x\n", cb_core_version());
    size_t size = 0;
    uint8_t* moc = ReadFile(argv[2], &size);
    if (!moc) {
        fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    CbModel* model = cb_model_create(moc, size, error, sizeof error);
    free(moc);
    if (!model) {
        fprintf(stderr, "model: %s\n", error);
        return 1;
    }
    CbBuffer description = {0};
    if (!cb_model_describe(model, &description, error, sizeof error)) {
        fprintf(stderr, "describe: %s\n", error);
        return 1;
    }
    const uint32_t jsonLength = U32(description.data);
    printf("describe: %zu bytes, JSON %u bytes\n%.*s\n", description.size, jsonLength, (int)(jsonLength > 600 ? 600 : jsonLength),
           (const char*)description.data + 4);

    /* One update with every parameter at 0 and parts untouched. */
    const char* json = (const char*)description.data + 4;
    uint32_t parameters = 0;
    for (const char* p = json; (p = strstr(p, "\"min\":")); ++p) ++parameters;
    CbBuffer request = {0};
    const float identity[4] = {1.0f, -1.0f, 0.0f, 0.0f};
    cb_buffer_append_u32(&request, CB_UPDATE_FORCE_ALL);
    cb_buffer_append(&request, identity, sizeof identity);
    cb_buffer_append_u32(&request, parameters);
    for (uint32_t i = 0; i < parameters; ++i) {
        const float zero = 0.0f;
        cb_buffer_append(&request, &zero, sizeof zero);
    }
    cb_buffer_append_u32(&request, 0);
    CbBuffer update = {0};
    if (!cb_model_update(model, request.data, request.size, &update, error, sizeof error)) {
        fprintf(stderr, "update: %s\n", error);
        return 1;
    }
    printf("update: %zu bytes for %u drawables\n", update.size, U32(update.data));
    cb_buffer_free(&request);
    cb_buffer_free(&update);
    cb_buffer_free(&description);
    cb_model_destroy(model);
    cb_core_unload();
    printf("ok\n");
    return 0;
}
