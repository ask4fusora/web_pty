#include <signal.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define write_raw _write
#else
#include <unistd.h>
#define write_raw ::write
#endif

import std;
import wpty.pty;
import wpty.ws_server;

namespace {

wpty::PtyProcess* g_active_pty = nullptr;

extern "C" void handle_forwarded_signal(int sig) {
    if (g_active_pty != nullptr) {
        g_active_pty->forward_signal(sig);
    }
}

#ifdef _WIN32
BOOL WINAPI handle_win32_console_ctrl(DWORD ctrl_type) {
    if (ctrl_type == CTRL_C_EVENT ||
        ctrl_type == CTRL_BREAK_EVENT ||
        ctrl_type == CTRL_CLOSE_EVENT ||
        ctrl_type == CTRL_SHUTDOWN_EVENT) {
        if (g_active_pty != nullptr) {
            g_active_pty->forward_signal(SIGINT);
        }
        return TRUE;
    }
    return FALSE;
}
#endif

void install_signal_forwarders() noexcept {
#ifdef _WIN32
    SetConsoleCtrlHandler(handle_win32_console_ctrl, TRUE);
#endif
#ifdef SIGPIPE
    ::signal(SIGPIPE, SIG_IGN);
#endif
    constexpr int kForwardedSignals[] = {
        SIGINT,
        SIGTERM,
#ifdef SIGHUP
        SIGHUP,
#endif
#ifdef SIGQUIT
        SIGQUIT,
#endif
#ifdef SIGUSR1
        SIGUSR1,
#endif
#ifdef SIGUSR2
        SIGUSR2,
#endif
#ifdef SIGALRM
        SIGALRM,
#endif
#ifdef SIGCONT
        SIGCONT,
#endif
#ifdef SIGTSTP
        SIGTSTP,
#endif
#ifdef SIGTTIN
        SIGTTIN,
#endif
#ifdef SIGTTOU
        SIGTTOU,
#endif
#ifdef SIGURG
        SIGURG,
#endif
#ifdef SIGXCPU
        SIGXCPU,
#endif
#ifdef SIGXFSZ
        SIGXFSZ,
#endif
#ifdef SIGVTALRM
        SIGVTALRM,
#endif
#ifdef SIGPROF
        SIGPROF,
#endif
#ifdef SIGWINCH
        SIGWINCH,
#endif
#ifdef SIGPWR
        SIGPWR,
#endif
#ifdef SIGSYS
        SIGSYS,
#endif
    };
    for (int sig : kForwardedSignals) {
        ::signal(sig, handle_forwarded_signal);
    }
}

struct CliArgs {
    const char* bind_host{"127.0.0.1"};
    std::uint16_t port{0};
    std::uint16_t cols{120};
    std::uint16_t rows{36};
    const char* cmd{nullptr};
    bool stdio_rpc{false};
    bool show_help{false};
    const char* error_msg{nullptr};
};

constexpr const char* kHelpText =
    "Usage:\n"
    "  wpty [--cols <2..1000>] [--rows <2..500>] [--stdio-rpc] --cmd \"<command>\"\n"
    "  wpty --port <1024..65535> [--bind <ip>] [--cols <2..1000>] [--rows <2..500>] --cmd \"<command>\"\n"
    "  wpty --help\n"
    "\n"
    "Flags:\n"
    "  --cmd \"<command>\"     Command executed as the direct child process inside the PTY (required)\n"
    "  --port <1024..65535>  Optional HTTP + WebSocket port for direct xterm.js browser attach (mutually exclusive with --stdio-rpc)\n"
    "  --bind <ip>           IPv4 bind address when --port is enabled (default: 127.0.0.1)\n"
    "  --cols <2..1000>      Initial terminal width in columns (default: 120)\n"
    "  --rows <2..500>       Initial terminal height in rows (default: 36)\n"
    "  --stdio-rpc           Enable 5-byte binary resize frames ([0x01, cols_hi, cols_lo, rows_hi, rows_lo]) on stdin (mutually exclusive with --port)\n"
    "  --help, -h            Print this help text and exit\n";

void write_cstr(int fd, const char* s) noexcept {
    if (s == nullptr) return;
    (void)write_raw(fd, s, static_cast<unsigned int>(::strlen(s)));
}

[[nodiscard]] auto to_string_view(const char* s) noexcept -> std::string_view {
    if (s == nullptr) return {};
    return std::string_view(s, ::strlen(s));
}

[[nodiscard]] constexpr auto str_equals(const char* a, const char* b) noexcept -> bool {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

[[nodiscard]] auto parse_u16_range(
    const char* s,
    std::uint16_t min_val,
    std::uint16_t max_val,
    std::uint16_t& out) noexcept -> bool {
    if (s == nullptr || *s == '\0') return false;
    char* end = nullptr;
    long v = ::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < static_cast<long>(min_val) || v > static_cast<long>(max_val)) {
        return false;
    }
    out = static_cast<std::uint16_t>(v);
    return true;
}

void parse_single_flag(int& index, int argc, char* argv[], CliArgs& args) noexcept {
    const char* flag = argv[index];
    if (str_equals(flag, "--help") || str_equals(flag, "-h")) {
        args.show_help = true;
    } else if (str_equals(flag, "--port")) {
        if (index + 1 >= argc || !parse_u16_range(argv[++index], 1024U, 65535U, args.port)) {
            args.error_msg = "wpty: error: --port requires an unprivileged port in 1024..65535\n";
        }
    } else if (str_equals(flag, "--bind")) {
        if (index + 1 >= argc || argv[index + 1][0] == '\0') {
            args.error_msg = "wpty: error: --bind requires a non-empty IPv4 address\n";
        } else {
            args.bind_host = argv[++index];
        }
    } else if (str_equals(flag, "--cols")) {
        if (index + 1 >= argc || !parse_u16_range(argv[++index], 2U, 1000U, args.cols)) {
            args.error_msg = "wpty: error: --cols requires an integer in 2..1000\n";
        }
    } else if (str_equals(flag, "--rows")) {
        if (index + 1 >= argc || !parse_u16_range(argv[++index], 2U, 500U, args.rows)) {
            args.error_msg = "wpty: error: --rows requires an integer in 2..500\n";
        }
    } else if (str_equals(flag, "--cmd")) {
        if (index + 1 >= argc || argv[index + 1][0] == '\0') {
            args.error_msg = "wpty: error: --cmd requires a non-empty command string\n";
        } else {
            args.cmd = argv[++index];
        }
    } else if (str_equals(flag, "--stdio-rpc")) {
        args.stdio_rpc = true;
    } else {
        args.error_msg = "wpty: error: unknown flag (try --help)\n";
    }
}

[[nodiscard]] auto parse_args(int argc, char* argv[]) noexcept -> CliArgs {
    CliArgs args{};
    for (int i = 1; i < argc; ++i) {
        parse_single_flag(i, argc, argv, args);
        if (args.show_help || args.error_msg != nullptr) {
            return args;
        }
    }
    if (!args.show_help) {
        if (args.port != 0U && args.stdio_rpc) {
            args.error_msg = "wpty: error: --port and --stdio-rpc are mutually exclusive\n";
        } else if (args.cmd == nullptr || args.cmd[0] == '\0') {
            args.error_msg = "wpty: error: --cmd \"<command>\" is required (try --help)\n";
        }
    }
    return args;
}

[[nodiscard]] auto create_ws_server(const CliArgs& args, wpty::PtyProcess& pty)
    -> std::unique_ptr<wpty::WsConsoleServer> {
    if (args.port == 0U) return nullptr;
    return std::make_unique<wpty::WsConsoleServer>(
        to_string_view(args.bind_host),
        args.port,
        [&pty](std::span<const std::byte> input) {
            (void)pty.write_all(input);
        },
        [&pty](std::uint16_t cols, std::uint16_t rows) {
            (void)pty.resize(wpty::TermSize{cols, rows});
        });
}

void emit_output_chunk(std::span<const std::byte> chunk, wpty::WsConsoleServer* server) {
    if (server != nullptr) {
        server->broadcast_output(chunk);
        return;
    }
    (void)write_raw(1, chunk.data(), static_cast<unsigned int>(chunk.size()));
}

void drain_final_bytes(wpty::PtyProcess& pty, wpty::WsConsoleServer* server, std::span<std::byte> buffer) {
    for (int iter = 0; iter < 32; ++iter) {
        auto drain = pty.read_some(buffer, 0);
        if (drain.bytes_read == 0UZ) break;
        emit_output_chunk(buffer.first(drain.bytes_read), server);
    }
}

void pump_pty_session(wpty::PtyProcess& pty, wpty::WsConsoleServer* server) {
    std::array<std::byte, 8192> buffer{};
    while (true) {
        auto res = pty.read_some(buffer, 30);
        if (res.bytes_read > 0UZ) {
            emit_output_chunk(std::span<const std::byte>(buffer.data(), res.bytes_read), server);
        }
        if (res.eof || !pty.is_alive()) {
            drain_final_bytes(pty, server, buffer);
            break;
        }
    }
}

void shutdown_server(wpty::WsConsoleServer* server, int exit_code) noexcept {
    if (server != nullptr) {
        server->notify_process_exit(exit_code);
        server->stop();
    }
}

} // namespace

auto main(int argc, char* argv[]) -> int {
    CliArgs args = parse_args(argc, argv);
    if (args.show_help) {
        write_cstr(1, kHelpText);
        return 0;
    }
    if (args.error_msg != nullptr) {
        write_cstr(2, args.error_msg);
        return 1;
    }

    install_signal_forwarders();

    wpty::PtyProcess pty;
    pty.set_stdio_passthrough(args.port == 0U);
    pty.set_stdio_rpc(args.stdio_rpc);

    auto server = create_ws_server(args, pty);
    if (server && !server->start()) {
        return 1;
    }

    if (!pty.spawn(to_string_view(args.cmd), wpty::TermSize{args.cols, args.rows})) {
        if (server) server->stop();
        return 1;
    }

    g_active_pty = &pty;
    pump_pty_session(pty, server.get());
    pty.wait_for_exit();

    int code = pty.exit_code();
    g_active_pty = nullptr;
    shutdown_server(server.get(), code);
    return code;
}
