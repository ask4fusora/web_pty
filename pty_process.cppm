module;

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

export module wpty.pty;

import std;

export namespace wpty {

struct TermSize {
    std::uint16_t cols{120};
    std::uint16_t rows{36};
};

struct ReadOutcome {
    std::size_t bytes_read{0};
    bool eof{false};
};

class PtyProcess {
public:
    PtyProcess() noexcept;
    PtyProcess(const PtyProcess&) = delete;
    auto operator=(const PtyProcess&) -> PtyProcess& = delete;

    PtyProcess(PtyProcess&& other) noexcept;
    auto operator=(PtyProcess&& other) noexcept -> PtyProcess&;
    ~PtyProcess();

    void set_stdio_passthrough(bool enabled) noexcept;
    void set_stdio_rpc(bool enabled) noexcept;
    [[nodiscard]] auto spawn(std::string_view command, TermSize initial_size = {}) -> bool;
    [[nodiscard]] auto read_some(std::span<std::byte> buffer, int timeout_ms = 50) -> ReadOutcome;
    [[nodiscard]] auto write_all(std::span<const std::byte> data) -> bool;
    void forward_signal(int sig) noexcept;
    [[nodiscard]] auto resize(TermSize new_size) -> bool;
    [[nodiscard]] auto is_alive() noexcept -> bool;
    void wait_for_exit() noexcept;
    [[nodiscard]] auto exit_code() const noexcept -> int;

private:
#ifdef _WIN32
    void* hpc_{nullptr};
    void* pipe_in_write_{nullptr};
    void* pipe_out_read_{nullptr};
    void* proc_handle_{nullptr};
    void* job_handle_{nullptr};
#else
    int master_fd_{-1};
    int child_pid_{-1};
    bool stdin_open_{true};
#endif
    bool stdio_passthrough_{true};
    bool stdio_rpc_{false};
    int exit_code_{0};

    void move_from(PtyProcess&& other) noexcept;
    void cleanup() noexcept;
    void reset_for_spawn() noexcept;
    void consume_stdin_chunk(std::span<const std::byte> chunk);

#ifdef _WIN32
    [[nodiscard]] auto open_conpty_pipes(void*& in_read, void*& out_write) noexcept -> bool;
    [[nodiscard]] auto create_pseudo_console(TermSize size, void* in_read, void* out_write) noexcept -> bool;
    [[nodiscard]] auto launch_win32_child(std::string_view command) -> bool;
    void bind_child_to_job_object() noexcept;
    void start_win32_stdin_thread();
    [[nodiscard]] auto drain_conpty_pipe(std::span<std::byte> buffer, std::uint32_t available) noexcept -> ReadOutcome;
    [[nodiscard]] auto handle_empty_conpty_pipe(int timeout_ms) noexcept -> ReadOutcome;
    void close_pseudo_console_if_open() noexcept;
#else
    [[nodiscard]] auto open_posix_pty(TermSize size, int& slave_fd) noexcept -> bool;
    [[nodiscard]] auto fork_posix_child(std::string_view command, int slave_fd) -> bool;
    [[noreturn]] static void run_posix_child(std::string_view command, int master_fd, int slave_fd, int expected_ppid) noexcept;
    static void attach_controlling_tty(int slave_fd) noexcept;
    static void bind_parent_death_sigkill(int expected_ppid) noexcept;
    static void set_default_terminal_env() noexcept;
    static void exec_command_direct_or_shell(std::string_view command) noexcept;
    static void set_fd_nonblocking(int fd) noexcept;
    void handle_stdin_poll_events(short revents);
    [[nodiscard]] auto read_master_fd(std::span<std::byte> buffer, short revents) noexcept -> ReadOutcome;
    static auto decode_wait_status(int status) noexcept -> int;
#endif
};

} // namespace wpty

namespace wpty {

namespace {

constexpr std::size_t kResizeFrameBytes = 5UZ;
constexpr std::uint8_t kResizeOpcode = 0x01U;

[[nodiscard]] auto decode_u16_be(const std::byte* p) noexcept -> std::uint16_t {
    auto hi = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[0]));
    auto lo = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(p[1]));
    return static_cast<std::uint16_t>((hi << 8U) | lo);
}

[[nodiscard]] auto is_valid_term_size(TermSize sz) noexcept -> bool {
    return sz.cols >= 2U && sz.cols <= 1000U && sz.rows >= 2U && sz.rows <= 500U;
}

[[nodiscard]] auto try_parse_resize_frame(std::span<const std::byte> data, TermSize& out_size) noexcept -> bool {
    if (data.size() < kResizeFrameBytes) return false;
    if (std::to_integer<std::uint8_t>(data[0]) != kResizeOpcode) return false;
    TermSize candidate{decode_u16_be(data.data() + 1), decode_u16_be(data.data() + 3)};
    if (!is_valid_term_size(candidate)) return false;
    out_size = candidate;
    return true;
}

#ifdef _WIN32
void disable_handle_inheritance(HANDLE h) noexcept {
    if (h != nullptr && h != INVALID_HANDLE_VALUE) {
        SetHandleInformation(h, HANDLE_FLAG_INHERIT, 0);
    }
}

void sanitize_parent_stdio_inheritance() noexcept {
    disable_handle_inheritance(GetStdHandle(STD_INPUT_HANDLE));
    disable_handle_inheritance(GetStdHandle(STD_OUTPUT_HANDLE));
    disable_handle_inheritance(GetStdHandle(STD_ERROR_HANDLE));
}

[[nodiscard]] auto utf8_to_wide(std::string_view utf8) -> std::wstring {
    if (utf8.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), len);
    wide.push_back(L'\0');
    return wide;
}
#else
[[nodiscard]] auto has_shell_metacharacters(std::string_view cmd) noexcept -> bool {
    for (char c : cmd) {
        switch (c) {
            case ';':
            case '|':
            case '&':
            case '<':
            case '>':
            case '$':
            case '`':
            case '(':
            case ')':
            case '{':
            case '}':
            case '*':
            case '?':
            case '~':
            case '!':
            case '#':
            case '\n':
                return true;
            default:
                break;
        }
    }
    return false;
}

[[nodiscard]] auto split_command_tokens(std::string_view cmd) -> std::vector<std::string> {
    std::vector<std::string> tokens;
    std::string current;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (char c : cmd) {
        if (escaped) {
            current.push_back(c);
            escaped = false;
        } else if (c == '\\' && !in_single) {
            escaped = true;
        } else if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        } else if ((c == ' ' || c == '\t') && !in_single && !in_double) {
            if (!current.empty()) {
                tokens.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) {
        tokens.push_back(std::move(current));
    }
    return tokens;
}
#endif

} // namespace

PtyProcess::PtyProcess() noexcept
#ifdef _WIN32
    : hpc_(nullptr),
      pipe_in_write_(nullptr),
      pipe_out_read_(nullptr),
      proc_handle_(nullptr),
      job_handle_(nullptr),
      stdio_passthrough_(true),
      stdio_rpc_(false),
      exit_code_(0)
#else
    : master_fd_(-1),
      child_pid_(-1),
      stdin_open_(true),
      stdio_passthrough_(true),
      stdio_rpc_(false),
      exit_code_(0)
#endif
{}

PtyProcess::PtyProcess(PtyProcess&& other) noexcept {
    move_from(std::move(other));
}

auto PtyProcess::operator=(PtyProcess&& other) noexcept -> PtyProcess& {
    if (this != &other) {
        cleanup();
        move_from(std::move(other));
    }
    return *this;
}

PtyProcess::~PtyProcess() {
    cleanup();
}

void PtyProcess::set_stdio_passthrough(bool enabled) noexcept {
    stdio_passthrough_ = enabled;
}

void PtyProcess::set_stdio_rpc(bool enabled) noexcept {
    stdio_rpc_ = enabled;
}

auto PtyProcess::exit_code() const noexcept -> int {
    return exit_code_;
}

void PtyProcess::reset_for_spawn() noexcept {
    cleanup();
    exit_code_ = 0;
#ifndef _WIN32
    stdin_open_ = stdio_passthrough_;
#endif
}

void PtyProcess::consume_stdin_chunk(std::span<const std::byte> chunk) {
    std::size_t offset = 0;
    while (stdio_rpc_ && offset < chunk.size()) {
        TermSize target_size{};
        if (!try_parse_resize_frame(chunk.subspan(offset), target_size)) {
            break;
        }
        (void)resize(target_size);
        offset += kResizeFrameBytes;
    }
    if (offset < chunk.size()) {
        (void)write_all(chunk.subspan(offset));
    }
}

#ifdef _WIN32

auto PtyProcess::open_conpty_pipes(void*& in_read, void*& out_write) noexcept -> bool {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_in_r = INVALID_HANDLE_VALUE;
    HANDLE h_in_w = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&h_in_r, &h_in_w, &sa, 0)) {
        return false;
    }
    disable_handle_inheritance(h_in_w);

    HANDLE h_out_r = INVALID_HANDLE_VALUE;
    HANDLE h_out_w = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&h_out_r, &h_out_w, &sa, 0)) {
        CloseHandle(h_in_r);
        CloseHandle(h_in_w);
        return false;
    }
    disable_handle_inheritance(h_out_r);

    in_read = h_in_r;
    out_write = h_out_w;
    pipe_in_write_ = h_in_w;
    pipe_out_read_ = h_out_r;
    return true;
}

auto PtyProcess::create_pseudo_console(TermSize size, void* in_read, void* out_write) noexcept -> bool {
    COORD coord{static_cast<SHORT>(size.cols), static_cast<SHORT>(size.rows)};
    HPCON hpc = nullptr;
    HRESULT hr = CreatePseudoConsole(
        coord,
        static_cast<HANDLE>(in_read),
        static_cast<HANDLE>(out_write),
        0,
        &hpc);
    CloseHandle(static_cast<HANDLE>(in_read));
    CloseHandle(static_cast<HANDLE>(out_write));
    if (FAILED(hr)) {
        return false;
    }
    hpc_ = hpc;
    return true;
}

void PtyProcess::bind_child_to_job_object() noexcept {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) return;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli{};
    jeli.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
        JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK |
        JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    AssignProcessToJobObject(job, static_cast<HANDLE>(proc_handle_));
    job_handle_ = job;
}

auto PtyProcess::launch_win32_child(std::string_view command) -> bool {
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<std::byte> attr_buf(attr_size);
    auto* attr_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
    if (!InitializeProcThreadAttributeList(attr_list, 1, 0, &attr_size) ||
        !UpdateProcThreadAttribute(
            attr_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hpc_, sizeof(HPCON), nullptr, nullptr)) {
        return false;
    }

    SetEnvironmentVariableW(L"TERM", L"xterm-256color");
    SetEnvironmentVariableW(L"COLORTERM", L"truecolor");

    std::wstring wide_cmd = utf8_to_wide(command);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = attr_list;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr,
        wide_cmd.data(),
        nullptr,
        nullptr,
        FALSE,
        EXTENDED_STARTUPINFO_PRESENT,
        nullptr,
        nullptr,
        &si.StartupInfo,
        &pi);
    DeleteProcThreadAttributeList(attr_list);
    if (!ok) {
        return false;
    }

    CloseHandle(pi.hThread);
    proc_handle_ = pi.hProcess;
    bind_child_to_job_object();
    return true;
}

void PtyProcess::start_win32_stdin_thread() {
    if (!stdio_passthrough_) return;
    std::thread([this]() {
        HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
        if (h_in == nullptr || h_in == INVALID_HANDLE_VALUE) return;
        std::array<std::byte, 1024> buf{};
        while (proc_handle_ != nullptr) {
            DWORD nread = 0;
            if (!ReadFile(h_in, buf.data(), static_cast<DWORD>(buf.size()), &nread, nullptr) || nread == 0) {
                break;
            }
            consume_stdin_chunk(std::span<const std::byte>(buf.data(), static_cast<std::size_t>(nread)));
        }
    }).detach();
}

auto PtyProcess::spawn(std::string_view command, TermSize size) -> bool {
    reset_for_spawn();
    sanitize_parent_stdio_inheritance();

    void* in_read = nullptr;
    void* out_write = nullptr;
    if (!open_conpty_pipes(in_read, out_write)) {
        return false;
    }
    if (!create_pseudo_console(size, in_read, out_write)) {
        cleanup();
        return false;
    }
    if (!launch_win32_child(command)) {
        cleanup();
        return false;
    }
    start_win32_stdin_thread();
    return true;
}

void PtyProcess::close_pseudo_console_if_open() noexcept {
    if (hpc_ != nullptr) {
        ClosePseudoConsole(static_cast<HPCON>(hpc_));
        hpc_ = nullptr;
    }
}

auto PtyProcess::drain_conpty_pipe(std::span<std::byte> buffer, std::uint32_t available) noexcept -> ReadOutcome {
    DWORD to_read = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), available));
    DWORD nread = 0;
    if (!ReadFile(static_cast<HANDLE>(pipe_out_read_), buffer.data(), to_read, &nread, nullptr) || nread == 0) {
        return {0, true};
    }
    return {static_cast<std::size_t>(nread), false};
}

auto PtyProcess::handle_empty_conpty_pipe(int timeout_ms) noexcept -> ReadOutcome {
    if (!is_alive()) {
        close_pseudo_console_if_open();
        DWORD avail = 0;
        if (!PeekNamedPipe(static_cast<HANDLE>(pipe_out_read_), nullptr, 0, nullptr, &avail, nullptr) || avail == 0) {
            return {0, true};
        }
        return {0, false};
    }
    if (timeout_ms > 0) {
        Sleep(static_cast<DWORD>(std::min(timeout_ms, 15)));
    }
    return {0, false};
}

auto PtyProcess::read_some(std::span<std::byte> buffer, int timeout_ms) -> ReadOutcome {
    if (pipe_out_read_ == nullptr) return {0, true};
    DWORD avail = 0;
    if (!PeekNamedPipe(static_cast<HANDLE>(pipe_out_read_), nullptr, 0, nullptr, &avail, nullptr)) {
        return {0, true};
    }
    if (avail == 0) {
        return handle_empty_conpty_pipe(timeout_ms);
    }
    return drain_conpty_pipe(buffer, avail);
}

auto PtyProcess::write_all(std::span<const std::byte> data) -> bool {
    if (pipe_in_write_ == nullptr) return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        if (!WriteFile(
                static_cast<HANDLE>(pipe_in_write_),
                data.data() + offset,
                static_cast<DWORD>(data.size() - offset),
                &written,
                nullptr)) {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

void PtyProcess::forward_signal(int /*sig*/) noexcept {
    if (pipe_in_write_ != nullptr) {
        const std::uint8_t etx = 0x03U;
        DWORD written = 0;
        WriteFile(static_cast<HANDLE>(pipe_in_write_), &etx, 1, &written, nullptr);
    }
}

auto PtyProcess::resize(TermSize new_size) -> bool {
    if (hpc_ == nullptr) return false;
    COORD coord{static_cast<SHORT>(new_size.cols), static_cast<SHORT>(new_size.rows)};
    ResizePseudoConsole(static_cast<HPCON>(hpc_), coord);
    return true;
}

auto PtyProcess::is_alive() noexcept -> bool {
    if (proc_handle_ == nullptr) return false;
    auto hproc = static_cast<HANDLE>(proc_handle_);
    if (WaitForSingleObject(hproc, 0) == WAIT_TIMEOUT) return true;
    DWORD code = 0;
    if (GetExitCodeProcess(hproc, &code)) {
        exit_code_ = static_cast<int>(code);
    }
    CloseHandle(hproc);
    proc_handle_ = nullptr;
    close_pseudo_console_if_open();
    return false;
}

void PtyProcess::wait_for_exit() noexcept {
    if (proc_handle_ == nullptr) return;
    auto hproc = static_cast<HANDLE>(proc_handle_);
    WaitForSingleObject(hproc, INFINITE);
    DWORD code = 0;
    if (GetExitCodeProcess(hproc, &code)) {
        exit_code_ = static_cast<int>(code);
    }
    CloseHandle(hproc);
    proc_handle_ = nullptr;
    close_pseudo_console_if_open();
}

void PtyProcess::move_from(PtyProcess&& other) noexcept {
    hpc_ = std::exchange(other.hpc_, nullptr);
    pipe_in_write_ = std::exchange(other.pipe_in_write_, nullptr);
    pipe_out_read_ = std::exchange(other.pipe_out_read_, nullptr);
    proc_handle_ = std::exchange(other.proc_handle_, nullptr);
    job_handle_ = std::exchange(other.job_handle_, nullptr);
    stdio_passthrough_ = std::exchange(other.stdio_passthrough_, true);
    stdio_rpc_ = std::exchange(other.stdio_rpc_, false);
    exit_code_ = std::exchange(other.exit_code_, 0);
}

void PtyProcess::cleanup() noexcept {
    close_pseudo_console_if_open();
    if (pipe_in_write_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(pipe_in_write_));
        pipe_in_write_ = nullptr;
    }
    if (pipe_out_read_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(pipe_out_read_));
        pipe_out_read_ = nullptr;
    }
    if (proc_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(proc_handle_));
        proc_handle_ = nullptr;
    }
    if (job_handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(job_handle_));
        job_handle_ = nullptr;
    }
}

#else

void PtyProcess::set_fd_nonblocking(int fd) noexcept {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void PtyProcess::attach_controlling_tty(int slave_fd) noexcept {
    ::setsid();
    ::ioctl(slave_fd, TIOCSCTTY, 0);
    ::dup2(slave_fd, STDIN_FILENO);
    ::dup2(slave_fd, STDOUT_FILENO);
    ::dup2(slave_fd, STDERR_FILENO);
    if (slave_fd > STDERR_FILENO) {
        ::close(slave_fd);
    }
}

void PtyProcess::bind_parent_death_sigkill(int expected_ppid) noexcept {
#if defined(__linux__)
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != static_cast<pid_t>(expected_ppid)) {
        ::_exit(1);
    }
#else
    (void)expected_ppid;
#endif
}

void PtyProcess::set_default_terminal_env() noexcept {
    ::setenv("TERM", "xterm-256color", 0);
    ::setenv("COLORTERM", "truecolor", 0);
}

void PtyProcess::exec_command_direct_or_shell(std::string_view command) noexcept {
    if (!has_shell_metacharacters(command)) {
        auto tokens = split_command_tokens(command);
        if (!tokens.empty()) {
            std::vector<char*> argv;
            argv.reserve(tokens.size() + 1UZ);
            for (auto& tok : tokens) {
                argv.push_back(tok.data());
            }
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
        }
    }
    std::string cmd_str(command.data(), command.size());
    ::execl("/bin/sh", "sh", "-c", cmd_str.c_str(), static_cast<char*>(nullptr));
}

[[noreturn]] void PtyProcess::run_posix_child(
    std::string_view command,
    int master_fd,
    int slave_fd,
    int expected_ppid) noexcept {
    ::close(master_fd);
    attach_controlling_tty(slave_fd);
    bind_parent_death_sigkill(expected_ppid);
    set_default_terminal_env();
    exec_command_direct_or_shell(command);
    ::_exit(127);
}

auto PtyProcess::open_posix_pty(TermSize size, int& slave_fd) noexcept -> bool {
    int master_fd = -1;
    struct winsize ws{};
    ws.ws_col = size.cols;
    ws.ws_row = size.rows;
    if (::openpty(&master_fd, &slave_fd, nullptr, nullptr, &ws) < 0) {
        return false;
    }
    master_fd_ = master_fd;
    return true;
}

auto PtyProcess::fork_posix_child(std::string_view command, int slave_fd) -> bool {
    int ppid_before_fork = static_cast<int>(::getpid());
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(slave_fd);
        cleanup();
        return false;
    }
    if (pid == 0) {
        run_posix_child(command, master_fd_, slave_fd, ppid_before_fork);
    }
    ::close(slave_fd);
    set_fd_nonblocking(master_fd_);
    child_pid_ = static_cast<int>(pid);
    return true;
}

auto PtyProcess::spawn(std::string_view command, TermSize size) -> bool {
    reset_for_spawn();
    int slave_fd = -1;
    if (!open_posix_pty(size, slave_fd)) {
        return false;
    }
    return fork_posix_child(command, slave_fd);
}

void PtyProcess::handle_stdin_poll_events(short revents) {
    if (!stdin_open_ || (revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        return;
    }
    if ((revents & POLLIN) == 0) {
        stdin_open_ = false;
        return;
    }
    std::array<std::byte, 1024> in_buf{};
    ssize_t nin = ::read(STDIN_FILENO, in_buf.data(), in_buf.size());
    if (nin > 0) {
        consume_stdin_chunk(std::span<const std::byte>(in_buf.data(), static_cast<std::size_t>(nin)));
    } else {
        stdin_open_ = false;
    }
}

auto PtyProcess::read_master_fd(std::span<std::byte> buffer, short revents) noexcept -> ReadOutcome {
    if ((revents & (POLLIN | POLLHUP)) != 0) {
        ssize_t n = ::read(master_fd_, buffer.data(), buffer.size());
        if (n > 0) {
            return {static_cast<std::size_t>(n), false};
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return {0, (revents & POLLHUP) != 0};
        }
        return {0, true};
    }
    if ((revents & (POLLERR | POLLNVAL)) != 0) {
        return {0, true};
    }
    return {0, false};
}

auto PtyProcess::read_some(std::span<std::byte> buffer, int timeout_ms) -> ReadOutcome {
    if (master_fd_ < 0) return {0, true};
    struct pollfd pfds[2]{};
    pfds[0].fd = master_fd_;
    pfds[0].events = POLLIN;
    pfds[1].fd = stdin_open_ ? STDIN_FILENO : -1;
    pfds[1].events = POLLIN;

    nfds_t nfds = stdin_open_ ? 2 : 1;
    if (::poll(pfds, nfds, timeout_ms) <= 0) {
        return {0, false};
    }
    handle_stdin_poll_events(pfds[1].revents);
    return read_master_fd(buffer, pfds[0].revents);
}

auto PtyProcess::write_all(std::span<const std::byte> data) -> bool {
    if (master_fd_ < 0) return false;
    std::size_t offset = 0;
    while (offset < data.size()) {
        ssize_t n = ::write(master_fd_, data.data() + offset, data.size() - offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += static_cast<std::size_t>(n);
    }
    return true;
}

void PtyProcess::forward_signal(int sig) noexcept {
    if (child_pid_ > 0) {
        ::kill(static_cast<pid_t>(child_pid_), sig);
    }
}

auto PtyProcess::resize(TermSize new_size) -> bool {
    if (master_fd_ < 0) return false;
    struct winsize ws{};
    ws.ws_col = new_size.cols;
    ws.ws_row = new_size.rows;
    ::ioctl(master_fd_, TIOCSWINSZ, &ws);
    return true;
}

auto PtyProcess::decode_wait_status(int status) noexcept -> int {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}

auto PtyProcess::is_alive() noexcept -> bool {
    if (child_pid_ <= 0) return false;
    int status = 0;
    pid_t res = ::waitpid(static_cast<pid_t>(child_pid_), &status, WNOHANG);
    if (res == 0) return true;
    if (res > 0) {
        exit_code_ = decode_wait_status(status);
    }
    child_pid_ = -1;
    return false;
}

void PtyProcess::wait_for_exit() noexcept {
    if (child_pid_ <= 0) return;
    int status = 0;
    while (true) {
        pid_t res = ::waitpid(static_cast<pid_t>(child_pid_), &status, 0);
        if (res < 0 && errno == EINTR) continue;
        if (res > 0) {
            exit_code_ = decode_wait_status(status);
        }
        break;
    }
    child_pid_ = -1;
}

void PtyProcess::move_from(PtyProcess&& other) noexcept {
    master_fd_ = std::exchange(other.master_fd_, -1);
    child_pid_ = std::exchange(other.child_pid_, -1);
    stdin_open_ = std::exchange(other.stdin_open_, false);
    stdio_passthrough_ = std::exchange(other.stdio_passthrough_, true);
    stdio_rpc_ = std::exchange(other.stdio_rpc_, false);
    exit_code_ = std::exchange(other.exit_code_, 0);
}

void PtyProcess::cleanup() noexcept {
    if (master_fd_ >= 0) {
        ::close(master_fd_);
        master_fd_ = -1;
    }
    child_pid_ = -1;
}

#endif

} // namespace wpty
