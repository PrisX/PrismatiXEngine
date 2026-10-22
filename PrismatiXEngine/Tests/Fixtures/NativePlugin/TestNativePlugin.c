/* A native extension library for JavaScriptHostIntegrationTests: "test.echo"
 * answers with the request reversed, "test.fail" always fails, and init
 * posts one "test.echo" message. */
#define PX_PLUGIN_BUILD 1
#include "Engine/Native/PxPluginAbi.h"

#include <stdlib.h>
#include <string.h>

static const PxHostApi* g_host = NULL;

static PxStatus Echo(void* user_data, const uint8_t* request, size_t size, PxResponse* response) {
    (void)user_data;
    uint8_t* reversed = size > 0 ? (uint8_t*)malloc(size) : NULL;
    if (size > 0 && !reversed) return PX_ERROR_FAILED;
    for (size_t index = 0; index < size; ++index) reversed[index] = request[size - 1 - index];
    const PxStatus status = g_host->respond(response, reversed, size);
    free(reversed);
    return status;
}

static PxStatus Fail(void* user_data, const uint8_t* request, size_t size, PxResponse* response) {
    (void)user_data; (void)request; (void)size; (void)response;
    return PX_ERROR_FAILED;
}

PX_PLUGIN_EXPORT uint32_t px_plugin_abi_version(void) { return PX_PLUGIN_ABI_VERSION; }

PX_PLUGIN_EXPORT PxStatus px_plugin_init(const PxHostApi* host) {
    if (!host || host->abi_version >> 16 != PX_PLUGIN_ABI_MAJOR || host->struct_size < sizeof(PxHostApi))
        return PX_ERROR_ABI;
    g_host = host;
    if (host->register_service(host->host, "test.echo", &Echo, NULL) != PX_OK) return PX_ERROR_FAILED;
    if (host->register_service(host->host, "test.fail", &Fail, NULL) != PX_OK) return PX_ERROR_FAILED;
    /* An undeclared service is refused. */
    if (host->register_service(host->host, "test.undeclared", &Echo, NULL) != PX_ERROR_NOT_DECLARED)
        return PX_ERROR_FAILED;
    static const uint8_t hello[] = {'h', 'i'};
    host->post_message(host->host, "test.echo", hello, sizeof hello);
    host->log(host->host, PX_LOG_INFO, "test plugin ready");
    return PX_OK;
}

PX_PLUGIN_EXPORT void px_plugin_shutdown(void) { g_host = NULL; }
