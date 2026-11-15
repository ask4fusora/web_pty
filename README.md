`wpty` (`web_pty`) is a cross-platform (currently `x86_64` Linux and `x86_64` Windows) web
pseudo-terminal (PTY) and/or bridge.

It spawns a direct child process inside a native OS pseudo-terminal (`openpty` on Linux, `ConPTY` on
Windows) and exposes that PTY over exactly one transport mode:

1. **Raw `stdio` mode (neither `--port` nor `--stdio-rpc`)**: 100% verbatim `stdin`/`stdout` byte
   passthrough at a fixed `--cols` x `--rows` geometry (`120x36` by default), zero byte
   interception. _Documentation is not complete_.
2. **Proxied web mode (`--stdio-rpc`)**: Streams `stdout` verbatim to a parent application.
   _Documentation is not complete_.
3. **Direct Web Mode (`--port <1024..65535>`)**: Serves an `xterm.js` web terminal (loaded via CDN
   on `GET /`) and an RFC 6455 WebSocket endpoint (`GET /ws`). _Documentation is not complete_.

**Note:** `--port` and `--stdio-rpc` are mutually exclusive: passing both fails fast at startup with
exit code `1`.

## What this means? (See [design principles](#design-principles))

You can simply think of `wpty` and its direct child process as a single process, as if there were no
intermediate process at all, which greatly reduces the pain of debugging and implementing.

## Who might want `wpty` (How `wpty` can be useful)?

1. **Cloud PaaS dashboards, CI/CD runners, and headless process orchestrators (e.g., Render-style
   web shells, container consoles, game server panels)**

   - Any platform or control plane that spawns headless workloads over anonymous pipes (such as PaaS
     web shells like Render or Railway, CI/CD build runners, internal developer portals, or game
     server controllers) normally loses interactive TTY capabilities because child processes detect
     non-TTY `stdin`/`stdout` pipes and disable ANSI color, progress bars, readline editing, and
     `Tab` completion. Wrapping the target command in `wpty --stdio-rpc --cmd "<command>"` gives the
     parent web server a genuine OS terminal stream over ordinary `stdin`/`stdout` pipes and
     forwards browser via `xterm.js`.

2. **Instant browser-based web terminals (`xterm.js`)**

   - Developers and sysadmins who want to temporary expose a live CLI tool, build job, container
     shell, or REPL in a browser without installing Node.js, Go, or Python server dependencies can
     run e.g. `wpty --port 9090 --cmd <command>`.

3. **Rust, C#, Go, Python, etc. host applications (`wpty-lib` / `wpty.h`)**

   - Applications that need cross-platform PTY management without pulling in platform-specific crate
     trees or C++ headers can link against `libwpty.a` (`wpty-lib`) using the 5-function C ABI in
     `wpty.h` or spawn the tiny standalone `wpty` binary over `stdio`.

## Design principles

1. **Unix philosophy: Do one thing and do it well**

   - `wpty` focuses exclusively on 1:1 pseudo-terminal bridging for a single child process and a
     single active connection, leaving authentication, process restarts, and multi-instance
     orchestration (if any is needed) to parent application.

2. **Fail at startup, never at runtime**

   - Argument validation, socket binding, PTY allocation, and process creation all occur before the
     runtime I/O loop begins. If arguments are invalid, `--port` cannot bind, or the PTY cannot be
     created, `wpty` fails immediately with exit code `1` before the child process starts. Once
     running, `wpty` operates on pre-allocated buffers so runtime failures belong strictly to the
     child process soly (hopefully).

3. **100% default byte passthrough**

   - You can expect replacing `<command>` with `wpty --cmd <command>` without any change in behavior
     whatsoever.

   - Once the child process is spawned, `wpty` emits zero diagnostic or framing bytes of its own
     onto `stdout` or `stderr`.

   - Once the child process is spawned, every input bytes and output bytes are forwarded to the
     child process verbatim.

4. **1:1 direct-child lifetime & signal parity**

   - `wpty` imposes zero timeouts, zero sleeps, zero synthetic exit strings, and zero destructor
     kill overrides.

   - `wpty` stays alive strictly while its direct child process is alive.

   - Catchable OS signals (`SIGINT`, `SIGTERM`, `SIGHUP`, `SIGQUIT`, and POSIX job/user signals on
     Linux; `CTRL_C_EVENT`, `CTRL_BREAK_EVENT`, `CTRL_CLOSE_EVENT`, `CTRL_SHUTDOWN_EVENT` on
     Windows) are forwarded directly to the child process.

   - Force-killing `wpty` is equivalent to force-killing the direct child process itself. `wpty`
     delivers the kill strictly to the direct child rather than force-killing the entire process
     tree, and any grandchildren follow standard OS parent-death and terminal-hangup semantics.

   - `wpty` exits with the exact exit code (or signal termination status) of its direct child
     process.

## Repository Layout

```
.
├── README.md           # Project documentation.
├── LICENSE             # Project license.
├── NOTICE              # Apache 2.0 NOTICE file.
├── xmake.lua           # xmake build configuration (wpty binary & wpty-lib static library).
├── wpty.h              # C ABI header for FFI / host embedding (Rust, C#, C, Go, Python, etc.).
├── pty_process.cppm    # export module wpty.pty (POSIX openpty / Windows ConPTY + JobObject).
├── ws_server.cppm      # export module wpty.ws_server (RFC 6455 WebSocket + HTTP + 128 KB scrollback that maybe will be removed because this is not a good long-term implementation).
├── c_api.cpp           # C ABI implementation wrapping wpty.pty and wpty.ws_server.
└── main.cpp            # CLI entry point, signal forwarding, and stdio pump loop.
```

## CLI Usage

```
wpty [--cols <2..1000>] [--rows <2..500>] [--stdio-rpc] --cmd "<command>"
wpty --port <1024..65535> [--bind <ip>] [--cols <2..1000>] [--rows <2..500>] --cmd "<command>"
wpty --help
```

### Flags

| Flag                   | Default         | Description                                                                                                                                                                      |
| ---------------------- | --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--cmd "<command>"`    | None (required) | Command line executed as the direct child process inside the PTY (exits with code `1` if omitted or empty)                                                                       |
| `--port <1024..65535>` | None (disabled) | Optional HTTP + WebSocket port in `1024..65535` for direct `xterm.js` browser attach (excludes reserved ports `0..1023`; mutually exclusive with `--stdio-rpc`)                  |
| `--bind <ip>`          | `127.0.0.1`     | IPv4 bind address when `--port` is enabled                                                                                                                                       |
| `--cols <2..1000>`     | `120`           | Initial terminal width in columns                                                                                                                                                |
| `--rows <2..500>`      | `36`            | Initial terminal height in rows                                                                                                                                                  |
| `--stdio-rpc`          | Off             | Enables in-band 5-byte binary resize frames (`[0x01, cols_hi, cols_lo, rows_hi, rows_lo]`) on `stdin` for proxied browser `xterm.js` sessions (mutually exclusive with `--port`) |
| `--help`, `-h`         | Off             | Prints CLI usage and flag reference to `stdout` and exits with code `0`                                                                                                          |

### Examples

Run a headless service or server in proxied web mode with dynamic `stdin` resize (for parent web
servers, PaaS agents, or game console):

```
wpty --stdio-rpc --cmd "java -Xmx4G -jar paper.jar --nogui"
```

Run an interactive shell in direct web mode with a live `xterm.js` WebSocket endpoint on
`http://0.0.0.0:9090/`:

```
wpty --port 9090 --bind 0.0.0.0 --cols 120 --rows 36 --cmd "pwsh -NoLogo"
```

## Wire Protocol

_Documentation is not complete._

## C ABI (`wpty.h`)

`wpty-lib` exposes a stable C ABI in `wpty.h` so host applications in C, C++, Rust, C#, or Go can
embed `wpty` without spawning a separate binary:

```
#include "wpty.h"

wpty_config_t cfg = {
    .command = "pwsh -NoLogo",
    .bind_host = "127.0.0.1",
    .port = 0,
    .initial_cols = 120,
    .initial_rows = 36,
};

int exit_code = wpty_run_blocking(&cfg);
```

## Building with `xmake`

### Prerequisites

- `xmake` 2.9.5+
- A C++26 compiler (GCC 15+, Clang 19+ with `libc++`, MSVC 19.40+ / Visual Studio 2022 17.10+, etc.)

### Build Commands

```
# Configure and build release.
xmake f -m release
xmake

# Running a build target.
xmake run wpty --help
```

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) and
[NOTICE](NOTICE) files for details.
