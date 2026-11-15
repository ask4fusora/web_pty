#include "wpty.h"
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define write_raw _write
#else
#include <unistd.h>
#define write_raw ::write
#endif

import std;
import wpty.pty;
import wpty.ws_server;

struct wpty_context {
    char command[1024]{};
    char bind_host[128]{};
    std::uint16_t port{0};
    wpty::TermSize initial_size{120, 36};

    std::unique_ptr<wpty::PtyProcess> pty;
    std::unique_ptr<wpty::WsConsoleServer> server;
    std::atomic<bool> running{false};
    std::thread pump_thread;
};

namespace {

[[nodiscard]] auto to_string_view(const char* s) noexcept -> std::string_view {
    if (s == nullptr) return {};
    return std::string_view(s, ::strlen(s));
}

void copy_string_bounded(char* dst, std::size_t cap, const char* src) noexcept {
    if (src == nullptr || cap == 0UZ) return;
    std::strncpy(dst, src, cap - 1UZ);
    dst[cap - 1UZ] = '\0';
}

void write_to_host_stdout(std::span<const std::byte> chunk) noexcept {
    if (!chunk.empty()) {
        (void)write_raw(1, chunk.data(), static_cast<unsigned int>(chunk.size()));
    }
}

void forward_chunk(wpty_context_t* ctx, std::span<const std::byte> chunk) {
    if (ctx->server) {
        ctx->server->broadcast_output(chunk);
    }
    write_to_host_stdout(chunk);
}

void drain_remaining_pty_bytes(wpty_context_t* ctx, std::span<std::byte> buffer) {
    for (int iter = 0; iter < 32; ++iter) {
        auto drain = ctx->pty->read_some(buffer, 0);
        if (drain.bytes_read == 0UZ) break;
        forward_chunk(ctx, buffer.first(drain.bytes_read));
    }
}

[[nodiscard]] auto create_bound_server(wpty_context_t* ctx) -> bool {
    if (ctx->port == 0U) return true;
    ctx->server = std::make_unique<wpty::WsConsoleServer>(
        to_string_view(ctx->bind_host),
        ctx->port,
        [ctx](std::span<const std::byte> input) {
            if (ctx->pty) (void)ctx->pty->write_all(input);
        },
        [ctx](std::uint16_t cols, std::uint16_t rows) {
            if (ctx->pty) (void)ctx->pty->resize(wpty::TermSize{cols, rows});
        });
    return ctx->server->start();
}

void pump_pty_loop(wpty_context_t* ctx) {
    std::array<std::byte, 8192> buffer{};
    while (ctx->running.load()) {
        auto res = ctx->pty->read_some(buffer, 30);
        if (res.bytes_read > 0UZ) {
            forward_chunk(ctx, std::span<const std::byte>(buffer.data(), res.bytes_read));
        }
        if (res.eof || !ctx->pty->is_alive()) {
            drain_remaining_pty_bytes(ctx, buffer);
            break;
        }
    }
}

[[nodiscard]] auto run_session(wpty_context_t* ctx) -> int {
    ctx->pty = std::make_unique<wpty::PtyProcess>();
    if (!create_bound_server(ctx)) {
        ctx->running.store(false);
        return 1;
    }
    if (!ctx->pty->spawn(to_string_view(ctx->command), ctx->initial_size)) {
        if (ctx->server) ctx->server->stop();
        ctx->running.store(false);
        return 1;
    }

    pump_pty_loop(ctx);
    ctx->pty->wait_for_exit();

    int code = ctx->pty->exit_code();
    ctx->running.store(false);
    if (ctx->server) {
        ctx->server->notify_process_exit(code);
        ctx->server->stop();
    }
    return code;
}

} // namespace

extern "C" {

WPTY_API wpty_context_t* wpty_create(const wpty_config_t* config) {
    if (config == nullptr || config->command == nullptr) return nullptr;
    auto* ctx = new wpty_context();
    copy_string_bounded(ctx->command, sizeof(ctx->command), config->command);
    const char* host = (config->bind_host != nullptr && config->bind_host[0] != '\0')
                           ? config->bind_host
                           : "127.0.0.1";
    copy_string_bounded(ctx->bind_host, sizeof(ctx->bind_host), host);
    ctx->port = config->port;
    ctx->initial_size.cols = (config->initial_cols > 0U) ? config->initial_cols : 120U;
    ctx->initial_size.rows = (config->initial_rows > 0U) ? config->initial_rows : 36U;
    return ctx;
}

WPTY_API int wpty_start(wpty_context_t* ctx) {
    if (ctx == nullptr || ctx->running.exchange(true)) return -1;
    if (ctx->pump_thread.joinable()) {
        ctx->pump_thread.join();
    }
    ctx->pump_thread = std::thread([ctx]() { (void)run_session(ctx); });
    return 0;
}

WPTY_API int wpty_run_blocking(const wpty_config_t* config) {
    wpty_context_t* ctx = wpty_create(config);
    if (ctx == nullptr) return -1;
    ctx->running.store(true);
    int code = run_session(ctx);
    wpty_destroy(ctx);
    return code;
}

WPTY_API void wpty_stop(wpty_context_t* ctx) {
    if (ctx == nullptr) return;
    ctx->running.store(false);
    if (ctx->pump_thread.joinable()) {
        ctx->pump_thread.join();
    }
    if (ctx->server) {
        ctx->server->stop();
    }
}

WPTY_API void wpty_destroy(wpty_context_t* ctx) {
    if (ctx == nullptr) return;
    wpty_stop(ctx);
    delete ctx;
}

} // extern "C"
