/*
 * PrismatiX native extension ABI (layer 2). Plain C, stable across compilers.
 *
 * A native extension library (.dll, .so, .dylib) exports two functions:
 *
 *     PX_PLUGIN_EXPORT uint32_t px_plugin_abi_version(void);
 *     PX_PLUGIN_EXPORT PxStatus px_plugin_init(const PxHostApi* host);
 *
 * and may export `void px_plugin_shutdown(void)`. The Player loads a library
 * only after checking its SHA-256 and signature against the extension's
 * manifest, then calls px_plugin_abi_version (its major version must equal
 * the Player's and its minor version must not be newer) and px_plugin_init,
 * in which the library registers the services its manifest lists in
 * `native.services`.
 *
 * Scripts reach a service by name with bytes in and bytes out
 * (`@prismatix/runtime/native`); they never see a function pointer, so the
 * same service can be provided differently elsewhere (the Web Player loads
 * no libraries). Every call into the library happens on the game thread;
 * only post_message may be called from any thread.
 *
 * docs/native-extensions.md describes the format, signing and packaging.
 */
#ifndef PRISMATIX_PX_PLUGIN_ABI_H
#define PRISMATIX_PX_PLUGIN_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PX_PLUGIN_ABI_MAJOR 1u
#define PX_PLUGIN_ABI_MINOR 1u
#define PX_PLUGIN_ABI_VERSION ((PX_PLUGIN_ABI_MAJOR << 16) | PX_PLUGIN_ABI_MINOR)

#if defined(_WIN32)
#define PX_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PX_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef int32_t PxStatus;
#define PX_OK 0
/* The service failed; the script's request throws PXJS7902. */
#define PX_ERROR_FAILED 1
#define PX_ERROR_INVALID_ARGUMENT 2
#define PX_ERROR_UNKNOWN_SERVICE 3
/* register_service: the name is not in the manifest's native.services. */
#define PX_ERROR_NOT_DECLARED 4
/* A buffer is over the 16 MiB limit or a queue is full. */
#define PX_ERROR_TOO_LARGE 5
#define PX_ERROR_ABI 6
/* Called at the wrong time (register_service after init, respond twice). */
#define PX_ERROR_STATE 7

#define PX_LOG_INFO 0
#define PX_LOG_WARNING 1
#define PX_LOG_ERROR 2

/* The largest request, response or message. */
#define PX_PLUGIN_MAX_BUFFER (16u * 1024u * 1024u)
/* respond_error keeps at most this many bytes of its message. */
#define PX_PLUGIN_MAX_ERROR_MESSAGE 4096u

/* Host-owned; valid only during the handler call it is passed to. */
typedef struct PxResponse PxResponse;

/*
 * Handles one request. `request` is valid only during the call. Write the
 * answer with host->respond (no call means an empty response). Return PX_OK,
 * or an error status to make the script's call throw; host->respond_error
 * (ABI 1.1) gives that error a message.
 */
typedef PxStatus (*PxServiceHandler)(void* user_data, const uint8_t* request,
                                     size_t request_size, PxResponse* response);

typedef struct PxHostApi {
    /* The Player's PX_PLUGIN_ABI_VERSION. */
    uint32_t abi_version;
    /* sizeof(PxHostApi) in the Player; later minor versions only append fields. */
    uint32_t struct_size;
    /* Opaque; pass it back to every function below. */
    void* host;
    /* UTF-8, valid while the library is loaded. */
    const char* engine_version;
    const char* extension_id;
    /* "win32-x64", "linux-x64", "darwin-x64" or "darwin-arm64". */
    const char* platform;

    /* During px_plugin_init only: registers a declared service. */
    PxStatus (*register_service)(void* host, const char* name,
                                 PxServiceHandler handler, void* user_data);
    /* Copies the response; at most once per handler call. */
    PxStatus (*respond)(PxResponse* response, const uint8_t* data, size_t size);
    /*
     * Queues a message for the extension's script, delivered on the game
     * thread from the next frame (`onMessage`). Thread-safe. At most 256
     * messages wait per service; a full queue returns PX_ERROR_TOO_LARGE.
     */
    PxStatus (*post_message)(void* host, const char* service,
                             const uint8_t* data, size_t size);
    /* Writes to the Player log. */
    void (*log)(void* host, int32_t level, const char* message);

    /* ABI 1.1 (struct_size covers the fields below). */

    /*
     * Fails the request with a UTF-8 message: the script's call throws
     * PXJS7902 with it, whatever status the handler returns. At most one of
     * respond or respond_error per handler call. The message is copied and
     * cut to PX_PLUGIN_MAX_ERROR_MESSAGE bytes.
     */
    PxStatus (*respond_error)(PxResponse* response, const char* message);
    /*
     * The folder holding this library (UTF-8, absolute, no trailing
     * separator), valid while the library is loaded: where it finds files it
     * ships beside itself, such as another library to load at run time.
     */
    const char* plugin_directory;
} PxHostApi;

typedef uint32_t (*PxPluginAbiVersionFn)(void);
typedef PxStatus (*PxPluginInitFn)(const PxHostApi* host);
typedef void (*PxPluginShutdownFn)(void);

#ifdef PX_PLUGIN_BUILD
/* Declarations for the library itself (define PX_PLUGIN_BUILD when building it). */
PX_PLUGIN_EXPORT uint32_t px_plugin_abi_version(void);
PX_PLUGIN_EXPORT PxStatus px_plugin_init(const PxHostApi* host);
PX_PLUGIN_EXPORT void px_plugin_shutdown(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PRISMATIX_PX_PLUGIN_ABI_H */
