/*
 * A miniature Player for tests: loads the plugin library like the Player does
 * (px_plugin_abi_version, px_plugin_init with a PxHostApi), then serves
 * service calls over stdin/stdout so a Node test can drive the real library
 * with the real script-side encoder and decoder.
 *
 *   plugin_harness <plugin library> <service,service,...> [<request fifo> <response fifo>]
 *
 * With the two FIFO paths (POSIX) it reads and writes those instead of stdin
 * and stdout, which lets a synchronous caller (the script's native.request is
 * synchronous) drive it with blocking reads and writes.
 *
 * Request frame (stdin):  u32 nameLength | name | u32 requestLength | request
 * Response frame (stdout): u32 pxStatus | u32 responseLength | response
 * End of input shuts the plugin down.
 */
#include "Engine/Native/PxPluginAbi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#define OpenLibrary(path) ((void*)LoadLibraryA(path))
#define FindSymbol(library, name) ((void*)GetProcAddress((HMODULE)(library), name))
#else
#include <dlfcn.h>
#define OpenLibrary(path) dlopen(path, RTLD_NOW | RTLD_LOCAL)
#define FindSymbol(library, name) dlsym(library, name)
#endif

struct PxResponse {
    uint8_t* data;
    size_t size;
    int answered;
};

typedef struct Service {
    char name[128];
    PxServiceHandler handler;
    void* user;
} Service;

static Service g_services[16];
static int g_serviceCount = 0;
static char g_declared[16][128];
static int g_declaredCount = 0;

static PxStatus RegisterService(void* host, const char* name, PxServiceHandler handler, void* user) {
    (void)host;
    int declared = 0;
    for (int i = 0; i < g_declaredCount; ++i) declared |= strcmp(g_declared[i], name) == 0;
    if (!declared) return PX_ERROR_NOT_DECLARED;
    if (g_serviceCount == 16) return PX_ERROR_TOO_LARGE;
    snprintf(g_services[g_serviceCount].name, sizeof g_services[0].name, "%s", name);
    g_services[g_serviceCount].handler = handler;
    g_services[g_serviceCount].user = user;
    ++g_serviceCount;
    return PX_OK;
}

static PxStatus Respond(PxResponse* response, const uint8_t* data, size_t size) {
    if (response->answered) return PX_ERROR_STATE;
    if (size > PX_PLUGIN_MAX_BUFFER) return PX_ERROR_TOO_LARGE;
    response->data = (uint8_t*)malloc(size ? size : 1);
    if (!response->data) return PX_ERROR_FAILED;
    memcpy(response->data, data, size);
    response->size = size;
    response->answered = 1;
    return PX_OK;
}

static PxStatus PostMessage(void* host, const char* service, const uint8_t* data, size_t size) {
    (void)host; (void)service; (void)data; (void)size;
    return PX_OK;
}

static void Log(void* host, int32_t level, const char* message) {
    (void)host;
    fprintf(stderr, "[plugin %d] %s\n", (int)level, message);
}

static int ReadExact(void* buffer, size_t size) { return size == 0 || fread(buffer, 1, size, stdin) == size; }
static void WriteAll(const void* buffer, size_t size) {
    if (size) fwrite(buffer, 1, size, stdout);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: plugin_harness <plugin library> <service,service,...> [<request fifo> <response fifo>]\n");
        return 2;
    }
#if defined(_WIN32)
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    if (argc >= 5) {
        /* The caller opens the request FIFO for writing first, then the response FIFO for reading. */
        if (!freopen(argv[3], "rb", stdin) || !freopen(argv[4], "wb", stdout)) {
            fprintf(stderr, "cannot open the FIFOs\n");
            return 2;
        }
    }
    char* list = strdup(argv[2]);
    for (char* name = strtok(list, ","); name && g_declaredCount < 16; name = strtok(NULL, ","))
        snprintf(g_declared[g_declaredCount++], sizeof g_declared[0], "%s", name);
    free(list);

    void* library = OpenLibrary(argv[1]);
    if (!library) {
        fprintf(stderr, "cannot load %s\n", argv[1]);
        return 3;
    }
    uint32_t (*abiVersion)(void) = NULL;
    PxStatus (*init)(const PxHostApi*) = NULL;
    void (*shutdownPlugin)(void) = NULL;
    void* symbol = FindSymbol(library, "px_plugin_abi_version");
    memcpy(&abiVersion, &symbol, sizeof symbol);
    symbol = FindSymbol(library, "px_plugin_init");
    memcpy(&init, &symbol, sizeof symbol);
    symbol = FindSymbol(library, "px_plugin_shutdown");
    memcpy(&shutdownPlugin, &symbol, sizeof symbol);
    if (!abiVersion || !init || (abiVersion() >> 16) != PX_PLUGIN_ABI_MAJOR || (abiVersion() & 0xFFFFu) > PX_PLUGIN_ABI_MINOR) {
        fprintf(stderr, "not a compatible PrismatiX plugin\n");
        return 4;
    }
    PxHostApi host;
    memset(&host, 0, sizeof host);
    host.abi_version = PX_PLUGIN_ABI_VERSION;
    host.struct_size = sizeof host;
    host.engine_version = "harness";
    host.extension_id = "live2d-cubism";
    host.platform = "harness";
    host.register_service = &RegisterService;
    host.respond = &Respond;
    host.post_message = &PostMessage;
    host.log = &Log;
    if (init(&host) != PX_OK) {
        fprintf(stderr, "px_plugin_init failed\n");
        return 5;
    }

    for (;;) {
        uint32_t nameLength = 0, requestLength = 0;
        if (fread(&nameLength, 1, sizeof nameLength, stdin) != sizeof nameLength) break;
        char name[128] = {0};
        if (nameLength >= sizeof name || !ReadExact(name, nameLength)) return 6;
        if (!ReadExact(&requestLength, sizeof requestLength) || requestLength > PX_PLUGIN_MAX_BUFFER) return 6;
        uint8_t* request = (uint8_t*)malloc(requestLength ? requestLength : 1);
        if (!request || !ReadExact(request, requestLength)) return 6;

        PxStatus status = PX_ERROR_UNKNOWN_SERVICE;
        PxResponse response = {0};
        for (int i = 0; i < g_serviceCount; ++i) {
            if (strcmp(g_services[i].name, name) != 0) continue;
            status = g_services[i].handler(g_services[i].user, request, requestLength, &response);
            break;
        }
        free(request);
        const uint32_t statusWord = (uint32_t)status;
        const uint32_t size = (uint32_t)response.size;
        WriteAll(&statusWord, sizeof statusWord);
        WriteAll(&size, sizeof size);
        WriteAll(response.data, response.size);
        fflush(stdout);
        free(response.data);
    }
    if (shutdownPlugin) shutdownPlugin();
    return 0;
}
