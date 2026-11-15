module;

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

export module wpty.ws_server;

import std;

export namespace wpty {

class WsConsoleServer {
public:
    using InputCallback = std::function<void(std::span<const std::byte>)>;
    using ResizeCallback = std::function<void(std::uint16_t, std::uint16_t)>;

    WsConsoleServer(
        std::string_view bind_host,
        std::uint16_t port,
        InputCallback on_input,
        ResizeCallback on_resize);

    WsConsoleServer(const WsConsoleServer&) = delete;
    auto operator=(const WsConsoleServer&) -> WsConsoleServer& = delete;

    ~WsConsoleServer();

    [[nodiscard]] auto start() -> bool;
    void stop() noexcept;

    void broadcast_output(std::span<const std::byte> data);
    void notify_process_exit(int exit_code);

private:
    struct FrameHeader {
        std::uint8_t opcode{0};
        bool masked{false};
        std::uint64_t payload_len{0};
    };

    std::string bind_host_;
    std::uint16_t port_;
    InputCallback on_input_;
    ResizeCallback on_resize_;

    std::atomic<bool> running_{false};
    std::intptr_t listen_sock_{-1};
    std::thread accept_thread_;

    std::mutex clients_mutex_;
    std::intptr_t active_client_{-1};
    std::vector<std::byte> scrollback_;
    std::atomic<std::size_t> active_workers_{0};

    static constexpr std::size_t kMaxScrollbackBytes = 128UZ * 1024UZ;

    [[nodiscard]] auto open_listen_socket() noexcept -> bool;
    [[nodiscard]] auto bind_and_listen_socket() noexcept -> bool;
    void close_listen_socket() noexcept;
    void disconnect_active_client_locked() noexcept;
    void disconnect_all_clients() noexcept;
    void wait_for_workers() const noexcept;

    void accept_loop();
    [[nodiscard]] auto poll_accept_ready() const noexcept -> bool;
    [[nodiscard]] auto accept_client_socket() const noexcept -> std::intptr_t;
    void spawn_client_worker(std::intptr_t client_sock);

    void handle_client(std::intptr_t client_sock);
    [[nodiscard]] auto upgrade_connection(std::intptr_t client_sock, std::string_view request) -> bool;
    [[nodiscard]] auto register_client_with_scrollback(std::intptr_t client_sock) -> bool;
    [[nodiscard]] auto is_active_client(std::intptr_t client_sock) noexcept -> bool;
    void pump_ws_frames(std::intptr_t client_sock);
    [[nodiscard]] auto read_next_frame_header(std::intptr_t client_sock, FrameHeader& out_hdr) const noexcept -> bool;
    [[nodiscard]] auto read_frame_payload(std::intptr_t client_sock, const FrameHeader& hdr, std::vector<std::uint8_t>& out_payload) const -> bool;
    [[nodiscard]] auto dispatch_frame(std::intptr_t client_sock, std::uint8_t opcode, std::span<const std::uint8_t> payload) -> bool;
    void dispatch_binary_payload(std::span<const std::uint8_t> payload);

    void remove_client(std::intptr_t client_sock) noexcept;
    void append_scrollback(std::span<const std::byte> data);
    void trim_scrollback_to_line_boundary();
};

} // namespace wpty

namespace wpty {

namespace {

void close_socket(std::intptr_t s) noexcept {
    if (s < 0) return;
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(s), SD_BOTH);
    ::closesocket(static_cast<SOCKET>(s));
#else
    ::shutdown(static_cast<int>(s), SHUT_RDWR);
    ::close(static_cast<int>(s));
#endif
}

[[nodiscard]] auto send_raw(std::intptr_t sock, std::span<const std::byte> buf) noexcept -> bool {
    std::size_t total = 0;
    while (total < buf.size()) {
#ifdef _WIN32
        int sent = ::send(
            static_cast<SOCKET>(sock),
            reinterpret_cast<const char*>(buf.data() + total),
            static_cast<int>(buf.size() - total),
            0);
#else
        ssize_t sent = ::send(
            static_cast<int>(sock),
            buf.data() + total,
            buf.size() - total,
            MSG_NOSIGNAL);
#endif
        if (sent <= 0) return false;
        total += static_cast<std::size_t>(sent);
    }
    return true;
}

[[nodiscard]] auto recv_exact(std::intptr_t sock, std::span<std::uint8_t> buf) noexcept -> bool {
    std::size_t total = 0;
    while (total < buf.size()) {
#ifdef _WIN32
        int r = ::recv(
            static_cast<SOCKET>(sock),
            reinterpret_cast<char*>(buf.data() + total),
            static_cast<int>(buf.size() - total),
            0);
#else
        ssize_t r = ::recv(
            static_cast<int>(sock),
            buf.data() + total,
            buf.size() - total,
            0);
#endif
        if (r <= 0) return false;
        total += static_cast<std::size_t>(r);
    }
    return true;
}

[[nodiscard]] constexpr auto rotl32(std::uint32_t v, unsigned int c) noexcept -> std::uint32_t {
    return (v << c) | (v >> (32U - c));
}

[[nodiscard]] auto read_be32(const std::uint8_t* p) noexcept -> std::uint32_t {
    return (static_cast<std::uint32_t>(p[0]) << 24U) |
           (static_cast<std::uint32_t>(p[1]) << 16U) |
           (static_cast<std::uint32_t>(p[2]) << 8U) |
           static_cast<std::uint32_t>(p[3]);
}

void write_be32(std::uint8_t* out, std::uint32_t v) noexcept {
    out[0] = static_cast<std::uint8_t>((v >> 24U) & 0xFFU);
    out[1] = static_cast<std::uint8_t>((v >> 16U) & 0xFFU);
    out[2] = static_cast<std::uint8_t>((v >> 8U) & 0xFFU);
    out[3] = static_cast<std::uint8_t>(v & 0xFFU);
}

[[nodiscard]] constexpr auto sha1_f(unsigned int round, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept
    -> std::uint32_t {
    if (round < 20U) return (b & c) | ((~b) & d);
    if (round < 40U) return b ^ c ^ d;
    if (round < 60U) return (b & c) | (b & d) | (c & d);
    return b ^ c ^ d;
}

[[nodiscard]] constexpr auto sha1_k(unsigned int round) noexcept -> std::uint32_t {
    if (round < 20U) return 0x5A827999U;
    if (round < 40U) return 0x6ED9EBA1U;
    if (round < 60U) return 0x8F1BBCDCU;
    return 0xCA62C1D6U;
}

void sha1_transform_block(const std::uint8_t* block, std::array<std::uint32_t, 5>& state) noexcept {
    std::array<std::uint32_t, 80> w{};
    for (std::size_t i = 0; i < 16; ++i) {
        w[i] = read_be32(block + i * 4);
    }
    for (std::size_t i = 16; i < 80; ++i) {
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    auto [a, b, c, d, e] = state;
    for (unsigned int i = 0; i < 80U; ++i) {
        std::uint32_t temp = rotl32(a, 5) + sha1_f(i, b, c, d) + e + sha1_k(i) + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

[[nodiscard]] auto pad_sha1_message(std::string_view input) -> std::vector<std::uint8_t> {
    std::vector<std::uint8_t> msg(input.begin(), input.end());
    std::uint64_t bit_len = static_cast<std::uint64_t>(msg.size()) * 8ULL;
    msg.push_back(0x80U);
    while ((msg.size() % 64UZ) != 56UZ) {
        msg.push_back(0x00U);
    }
    for (int i = 7; i >= 0; --i) {
        msg.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xFFU));
    }
    return msg;
}

[[nodiscard]] auto sha1_digest(std::string_view input) -> std::array<std::uint8_t, 20> {
    std::array<std::uint32_t, 5> state{
        0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    auto padded = pad_sha1_message(input);
    for (std::size_t offset = 0; offset < padded.size(); offset += 64UZ) {
        sha1_transform_block(padded.data() + offset, state);
    }
    std::array<std::uint8_t, 20> out{};
    for (std::size_t i = 0; i < 5UZ; ++i) {
        write_be32(out.data() + i * 4UZ, state[i]);
    }
    return out;
}

[[nodiscard]] auto base64_encode(std::span<const std::uint8_t> data) -> std::string {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2UZ) / 3UZ) * 4UZ);
    for (std::size_t i = 0; i < data.size(); i += 3UZ) {
        std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16U;
        if (i + 1UZ < data.size()) n |= static_cast<std::uint32_t>(data[i + 1UZ]) << 8U;
        if (i + 2UZ < data.size()) n |= static_cast<std::uint32_t>(data[i + 2UZ]);

        out.push_back(kAlphabet[(n >> 18U) & 0x3FU]);
        out.push_back(kAlphabet[(n >> 12U) & 0x3FU]);
        out.push_back((i + 1UZ < data.size()) ? kAlphabet[(n >> 6U) & 0x3FU] : '=');
        out.push_back((i + 2UZ < data.size()) ? kAlphabet[n & 0x3FU] : '=');
    }
    return out;
}

[[nodiscard]] auto compute_ws_accept_key(std::string_view client_key) -> std::string {
    std::string combined(client_key);
    combined.append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    auto digest = sha1_digest(combined);
    return base64_encode(digest);
}

[[nodiscard]] auto trim_ascii(std::string_view sv) noexcept -> std::string_view {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t')) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r')) {
        sv.remove_suffix(1);
    }
    return sv;
}

[[nodiscard]] auto ascii_iequals(std::string_view a, std::string_view b) noexcept -> bool {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        char cb = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (ca != cb) return false;
    }
    return true;
}

[[nodiscard]] auto extract_http_header(std::string_view req, std::string_view header_name) noexcept
    -> std::string_view {
    std::size_t pos = 0;
    while (pos < req.size()) {
        std::size_t line_end = req.find("\r\n", pos);
        if (line_end == std::string_view::npos) break;
        std::string_view line = req.substr(pos, line_end - pos);
        std::size_t colon = line.find(':');
        if (colon != std::string_view::npos && ascii_iequals(line.substr(0, colon), header_name)) {
            return trim_ascii(line.substr(colon + 1));
        }
        pos = line_end + 2UZ;
    }
    return {};
}

[[nodiscard]] auto build_ws_frame(std::uint8_t opcode, std::span<const std::byte> payload)
    -> std::vector<std::byte> {
    std::vector<std::byte> frame;
    frame.reserve(10UZ + payload.size());
    frame.push_back(static_cast<std::byte>(0x80U | (opcode & 0x0FU)));

    std::size_t len = payload.size();
    if (len <= 125UZ) {
        frame.push_back(static_cast<std::byte>(len));
    } else if (len <= 65535UZ) {
        frame.push_back(static_cast<std::byte>(126U));
        frame.push_back(static_cast<std::byte>((len >> 8U) & 0xFFU));
        frame.push_back(static_cast<std::byte>(len & 0xFFU));
    } else {
        frame.push_back(static_cast<std::byte>(127U));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<std::byte>((len >> (i * 8)) & 0xFFU));
        }
    }
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

[[nodiscard]] auto try_parse_binary_resize(
    std::span<const std::uint8_t> payload,
    std::uint16_t& cols,
    std::uint16_t& rows) noexcept -> bool {
    if (payload.size() != 5UZ || payload[0] != 0x01U) return false;
    cols = static_cast<std::uint16_t>((static_cast<std::uint16_t>(payload[1]) << 8U) | payload[2]);
    rows = static_cast<std::uint16_t>((static_cast<std::uint16_t>(payload[3]) << 8U) | payload[4]);
    return cols >= 2U && cols <= 1000U && rows >= 2U && rows <= 500U;
}

constexpr std::string_view kEmbeddedHtml = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>wpty Console</title>
<link rel="stylesheet" href="https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/css/xterm.min.css"/>
<style>
  html, body { margin: 0; padding: 0; width: 100%; height: 100%; background: #0d1117; overflow: hidden; }
  #terminal { width: 100vw; height: 100vh; }
</style>
</head>
<body>
<div id="terminal"></div>
<script src="https://cdn.jsdelivr.net/npm/@xterm/xterm@5.5.0/lib/xterm.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/@xterm/addon-fit@0.10.0/lib/addon-fit.min.js"></script>
<script>
  const term = new Terminal({ cursorBlink: true, fontFamily: 'monospace', fontSize: 14, convertEol: false, theme: { background: '#0d1117' } });
  const fitAddon = new FitAddon.FitAddon();
  term.loadAddon(fitAddon);
  term.open(document.getElementById('terminal'));
  fitAddon.fit();
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(`${proto}//${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  const sendResize = () => {
    if (ws.readyState === WebSocket.OPEN) {
      const b = new Uint8Array(5);
      b[0] = 1; b[1] = (term.cols >> 8) & 0xff; b[2] = term.cols & 0xff;
      b[3] = (term.rows >> 8) & 0xff; b[4] = term.rows & 0xff;
      ws.send(b);
    }
  };
  ws.onopen = () => { fitAddon.fit(); sendResize(); };
  ws.onmessage = (ev) => {
    if (ev.data instanceof ArrayBuffer) term.write(new Uint8Array(ev.data));
    else if (typeof ev.data === 'string' && !ev.data.startsWith('EXIT:')) term.write(ev.data);
  };
  term.onData((d) => { if (ws.readyState === WebSocket.OPEN) ws.send(d); });
  window.addEventListener('resize', () => { fitAddon.fit(); sendResize(); });
</script>
</body>
</html>)HTML";

[[nodiscard]] auto send_html_index(std::intptr_t client_sock) -> bool {
    std::string resp =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(kEmbeddedHtml.size()) + "\r\n"
        "Connection: close\r\n\r\n";
    resp.append(kEmbeddedHtml);
    return send_raw(client_sock, std::as_bytes(std::span(resp)));
}

[[nodiscard]] auto send_ws_handshake(std::intptr_t client_sock, std::string_view ws_key) -> bool {
    std::string accept_key = compute_ws_accept_key(ws_key);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept_key + "\r\n\r\n";
    return send_raw(client_sock, std::as_bytes(std::span(resp)));
}

[[nodiscard]] auto read_http_request(std::intptr_t client_sock) -> std::string {
    std::array<char, 4096> buf{};
#ifdef _WIN32
    int n = ::recv(static_cast<SOCKET>(client_sock), buf.data(), static_cast<int>(buf.size() - 1UZ), 0);
#else
    ssize_t n = ::recv(static_cast<int>(client_sock), buf.data(), buf.size() - 1UZ, 0);
#endif
    if (n <= 0) return {};
    return std::string(buf.data(), static_cast<std::size_t>(n));
}

void unmask_payload(std::span<std::uint8_t> payload, std::span<const std::uint8_t, 4> mask_key) noexcept {
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] ^= mask_key[i & 3UZ];
    }
}

[[nodiscard]] auto build_ipv4_sockaddr(const std::string& host, std::uint16_t port) noexcept -> sockaddr_in {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }
    return addr;
}

} // namespace

WsConsoleServer::WsConsoleServer(
    std::string_view bind_host,
    std::uint16_t port,
    InputCallback on_input,
    ResizeCallback on_resize)
    : bind_host_(bind_host),
      port_(port),
      on_input_(std::move(on_input)),
      on_resize_(std::move(on_resize)) {
    scrollback_.reserve(kMaxScrollbackBytes * 2UZ);
}

WsConsoleServer::~WsConsoleServer() {
    stop();
}

auto WsConsoleServer::open_listen_socket() noexcept -> bool {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET raw_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (raw_sock == INVALID_SOCKET) return false;
    listen_sock_ = static_cast<std::intptr_t>(raw_sock);
    int opt = 1;
    ::setsockopt(raw_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    int raw_sock = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (raw_sock < 0) return false;
    listen_sock_ = static_cast<std::intptr_t>(raw_sock);
    int opt = 1;
    ::setsockopt(raw_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    return true;
}

auto WsConsoleServer::bind_and_listen_socket() noexcept -> bool {
    sockaddr_in addr = build_ipv4_sockaddr(bind_host_, port_);
#ifdef _WIN32
    auto sock = static_cast<SOCKET>(listen_sock_);
#else
    auto sock = static_cast<int>(listen_sock_);
#endif
    if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_listen_socket();
        return false;
    }
    if (::listen(sock, 16) != 0) {
        close_listen_socket();
        return false;
    }
    return true;
}

auto WsConsoleServer::start() -> bool {
    if (running_.load()) return true;
    if (!open_listen_socket()) return false;
    if (!bind_and_listen_socket()) return false;
    running_.store(true);
    accept_thread_ = std::thread(&WsConsoleServer::accept_loop, this);
    return true;
}

void WsConsoleServer::close_listen_socket() noexcept {
    if (listen_sock_ >= 0) {
        close_socket(listen_sock_);
        listen_sock_ = -1;
    }
}

void WsConsoleServer::disconnect_active_client_locked() noexcept {
    if (active_client_ >= 0) {
        auto close_frame = build_ws_frame(0x08U, {});
        (void)send_raw(active_client_, close_frame);
        close_socket(active_client_);
        active_client_ = -1;
    }
}

void WsConsoleServer::disconnect_all_clients() noexcept {
    std::scoped_lock lock(clients_mutex_);
    disconnect_active_client_locked();
}

void WsConsoleServer::wait_for_workers() const noexcept {
    while (active_workers_.load() > 0UZ) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void WsConsoleServer::stop() noexcept {
    if (!running_.exchange(false)) return;
    close_listen_socket();
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    disconnect_all_clients();
    wait_for_workers();
}

void WsConsoleServer::trim_scrollback_to_line_boundary() {
    if (scrollback_.size() <= kMaxScrollbackBytes) return;
    std::size_t excess = scrollback_.size() - kMaxScrollbackBytes;
    while (excess < scrollback_.size() &&
           std::to_integer<char>(scrollback_[excess]) != '\n') {
        ++excess;
    }
    if (excess < scrollback_.size()) ++excess;
    scrollback_.erase(
        scrollback_.begin(),
        scrollback_.begin() + static_cast<std::ptrdiff_t>(excess));
}

void WsConsoleServer::append_scrollback(std::span<const std::byte> data) {
    if (data.empty()) return;
    if (data.size() >= kMaxScrollbackBytes) {
        auto tail = data.subspan(data.size() - kMaxScrollbackBytes);
        scrollback_.assign(tail.begin(), tail.end());
        return;
    }
    scrollback_.insert(scrollback_.end(), data.begin(), data.end());
    trim_scrollback_to_line_boundary();
}

void WsConsoleServer::broadcast_output(std::span<const std::byte> data) {
    if (data.empty()) return;
    auto frame = build_ws_frame(0x02U, data);
    std::scoped_lock lock(clients_mutex_);
    append_scrollback(data);
    if (active_client_ >= 0 && !send_raw(active_client_, frame)) {
        close_socket(active_client_);
        active_client_ = -1;
    }
}

void WsConsoleServer::notify_process_exit(int exit_code) {
    std::string msg = "EXIT:" + std::to_string(exit_code);
    auto frame = build_ws_frame(0x01U, std::as_bytes(std::span(msg)));
    std::scoped_lock lock(clients_mutex_);
    if (active_client_ >= 0) {
        (void)send_raw(active_client_, frame);
    }
}

auto WsConsoleServer::poll_accept_ready() const noexcept -> bool {
#ifdef _WIN32
    fd_set readfds{};
    FD_ZERO(&readfds);
    FD_SET(static_cast<SOCKET>(listen_sock_), &readfds);
    timeval tv{0, 50000};
    return ::select(0, &readfds, nullptr, nullptr, &tv) > 0;
#else
    struct pollfd pfd{};
    pfd.fd = static_cast<int>(listen_sock_);
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN) != 0;
#endif
}

auto WsConsoleServer::accept_client_socket() const noexcept -> std::intptr_t {
    sockaddr_in client_addr{};
#ifdef _WIN32
    int addr_len = sizeof(client_addr);
    SOCKET s = ::accept(static_cast<SOCKET>(listen_sock_), reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
    if (s == INVALID_SOCKET) return -1;
    return static_cast<std::intptr_t>(s);
#else
    socklen_t addr_len = sizeof(client_addr);
    int s = ::accept(static_cast<int>(listen_sock_), reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
    return static_cast<std::intptr_t>(s);
#endif
}

void WsConsoleServer::spawn_client_worker(std::intptr_t client_sock) {
    active_workers_.fetch_add(1UZ);
    std::thread([this, client_sock]() {
        handle_client(client_sock);
        active_workers_.fetch_sub(1UZ);
    }).detach();
}

void WsConsoleServer::accept_loop() {
    while (running_.load()) {
        if (!poll_accept_ready()) continue;
        std::intptr_t client_sock = accept_client_socket();
        if (client_sock < 0) continue;
        spawn_client_worker(client_sock);
    }
}

auto WsConsoleServer::upgrade_connection(std::intptr_t client_sock, std::string_view request) -> bool {
    std::string_view ws_key = extract_http_header(request, "Sec-WebSocket-Key");
    if (ws_key.empty()) {
        (void)send_html_index(client_sock);
        return false;
    }
    return send_ws_handshake(client_sock, ws_key);
}

auto WsConsoleServer::register_client_with_scrollback(std::intptr_t client_sock) -> bool {
    std::scoped_lock lock(clients_mutex_);
    disconnect_active_client_locked();
    if (!scrollback_.empty()) {
        auto history_frame = build_ws_frame(0x02U, scrollback_);
        if (!send_raw(client_sock, history_frame)) {
            return false;
        }
    }
    active_client_ = client_sock;
    return true;
}

auto WsConsoleServer::is_active_client(std::intptr_t client_sock) noexcept -> bool {
    std::scoped_lock lock(clients_mutex_);
    return active_client_ == client_sock;
}

auto WsConsoleServer::read_next_frame_header(std::intptr_t client_sock, FrameHeader& out_hdr) const noexcept -> bool {
    std::array<std::uint8_t, 2> hdr{};
    if (!recv_exact(client_sock, hdr)) return false;

    out_hdr.opcode = hdr[0] & 0x0FU;
    out_hdr.masked = (hdr[1] & 0x80U) != 0U;
    out_hdr.payload_len = hdr[1] & 0x7FU;

    if (out_hdr.payload_len == 126ULL) {
        std::array<std::uint8_t, 2> ext{};
        if (!recv_exact(client_sock, ext)) return false;
        out_hdr.payload_len = (static_cast<std::uint64_t>(ext[0]) << 8U) | ext[1];
    } else if (out_hdr.payload_len == 127ULL) {
        std::array<std::uint8_t, 8> ext{};
        if (!recv_exact(client_sock, ext)) return false;
        out_hdr.payload_len = 0ULL;
        for (auto b : ext) out_hdr.payload_len = (out_hdr.payload_len << 8U) | b;
    }
    return out_hdr.payload_len <= (1024ULL * 1024ULL);
}

auto WsConsoleServer::read_frame_payload(
    std::intptr_t client_sock,
    const FrameHeader& hdr,
    std::vector<std::uint8_t>& out_payload) const -> bool {
    std::array<std::uint8_t, 4> mask_key{};
    if (hdr.masked && !recv_exact(client_sock, mask_key)) {
        return false;
    }
    out_payload.resize(static_cast<std::size_t>(hdr.payload_len));
    if (hdr.payload_len > 0ULL && !recv_exact(client_sock, out_payload)) {
        return false;
    }
    if (hdr.masked) {
        unmask_payload(out_payload, mask_key);
    }
    return true;
}

void WsConsoleServer::dispatch_binary_payload(std::span<const std::uint8_t> payload) {
    std::uint16_t cols = 0;
    std::uint16_t rows = 0;
    if (try_parse_binary_resize(payload, cols, rows)) {
        if (on_resize_) on_resize_(cols, rows);
    }
}

auto WsConsoleServer::dispatch_frame(
    std::intptr_t client_sock,
    std::uint8_t opcode,
    std::span<const std::uint8_t> payload) -> bool {
    if (!is_active_client(client_sock)) return false;
    if (opcode == 0x08U) return false;
    if (opcode == 0x09U) {
        auto pong = build_ws_frame(0x0AU, std::as_bytes(payload));
        return send_raw(client_sock, pong);
    }
    if (opcode == 0x01U) {
        if (on_input_ && !payload.empty()) {
            on_input_(std::as_bytes(payload));
        }
        return true;
    }
    if (opcode == 0x02U) {
        dispatch_binary_payload(payload);
    }
    return true;
}

void WsConsoleServer::pump_ws_frames(std::intptr_t client_sock) {
    std::vector<std::uint8_t> payload;
    while (running_.load()) {
        FrameHeader hdr{};
        if (!read_next_frame_header(client_sock, hdr)) break;
        if (!read_frame_payload(client_sock, hdr, payload)) break;
        if (!dispatch_frame(client_sock, hdr.opcode, payload)) break;
    }
}

void WsConsoleServer::remove_client(std::intptr_t client_sock) noexcept {
    std::scoped_lock lock(clients_mutex_);
    if (active_client_ == client_sock) {
        active_client_ = -1;
    }
}

void WsConsoleServer::handle_client(std::intptr_t client_sock) {
    std::string req = read_http_request(client_sock);
    if (req.empty() || !upgrade_connection(client_sock, req)) {
        close_socket(client_sock);
        return;
    }
    if (!register_client_with_scrollback(client_sock)) {
        close_socket(client_sock);
        return;
    }
    pump_ws_frames(client_sock);
    remove_client(client_sock);
    close_socket(client_sock);
}

} // namespace wpty
