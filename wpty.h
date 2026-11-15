#ifndef WPTY_H_INCLUDED
#define WPTY_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(WPTY_SHARED_BUILD)
#define WPTY_API __declspec(dllexport)
#elif defined(_WIN32) && defined(WPTY_SHARED_USE)
#define WPTY_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define WPTY_API __attribute__((visibility("default")))
#else
#define WPTY_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wpty_context wpty_context_t;

typedef struct wpty_config {
    const char* command;
    const char* bind_host;
    uint16_t port;          /* 0 = portless stdio pipe mode; >0 = start HTTP/WS server on port */
    uint16_t initial_cols;
    uint16_t initial_rows;
} wpty_config_t;

WPTY_API wpty_context_t* wpty_create(const wpty_config_t* config);
WPTY_API int wpty_start(wpty_context_t* ctx);
WPTY_API int wpty_run_blocking(const wpty_config_t* config);
WPTY_API void wpty_stop(wpty_context_t* ctx);
WPTY_API void wpty_destroy(wpty_context_t* ctx);

#ifdef __cplusplus
}
#endif

#endif /* WPTY_H_INCLUDED */
