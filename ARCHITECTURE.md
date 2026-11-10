# Architecture

## Rules

1. **No singletons or mutable globals.** Everything is owned by `App` (or by
   something `App` owns) and passed explicitly to whoever needs it.
2. **App lifetime vs session lifetime.** The window, renderer, UI, job system and
   asset cache live as long as the process. The UDP circuit, capabilities, event
   queue and world state belong to one `Session`. Logging out destroys the
   `Session`; logging in again creates a fresh one, with no relaunch. Nothing
   that lives as long as the App may hold pointers into a `Session`.
3. **Threads share no mutable state.** They communicate through message queues
   (`viewer::MessageQueue`). One thread owns each piece of state:
   - main: SDL events, ImGui, GPU swapchain acquire and submit
   - world: the only writer of world state; publishes snapshots for rendering
   - network: UDP circuit plus HTTP (capabilities, event queue, asset fetch)
   - worker pool: J2K decode, KTX encode/write, mesh decode, disk I/O
4. **Modules are libraries with one-way dependencies.** `src/<module>` builds
   `viewer::<module>`; only `app/` sees everything.
5. **Textures are cached as KTX2 only.** JPEG 2000 is decoded once and never
   stored.

## Layout

```
src/core     Uuid, JobSystem, MessageQueue, logging                -> viewer::core
src/image    J2kDecoder (OpenJPEG), mips, alpha mode, KTX2 I/O     -> viewer::image
src/assets   TextureCache: J2K -> KTX2 on disk, on worker threads   -> viewer::assets
src/llsd     LLSD Value, LLSD+XML read/write (tinyxml2)            -> viewer::llsd
src/net      HttpClient interface (libcurl backend), request groups,
             UdpSocket (POSIX sockets / Winsock)                   -> viewer::net
src/protocol Message structs generated from the message template,
             packet framing, CircuitManager (UDP circuits)         -> viewer::protocol
src/session  Login, Session (one login: regions, circuits, caps,
             event queues)                                         -> viewer::session
src/render   SDL GPU device, frames, texture upload                -> viewer::render
src/ui       ImGui context + SDL3/SDL GPU backends                 -> viewer::ui
src/app      App (owns everything), LoginPanel, TextureBrowser panel, main()
tests/       doctest unit tests (fixtures in tests/data)
tools/j2kbench   benchmarks J2kDecoder implementations
tools/httpget    one request through HttpClient; pretty-prints LLSD replies
cmake/           CPM bootstrap + Dependencies.cmake (all versions pinned)
external/master-message-template   git submodule: Linden Lab's message_template.msg
```

## Texture pipeline

1. `TextureCache::request()` queues a job on the `JobSystem`.
2. The worker reads `<cache>/textures/<xx>/<uuid>.ktx2` if it exists. Otherwise it
   calls the request's `J2kSource` for the bytes, decodes them, expands to RGBA8,
   classifies alpha, builds mips, and writes the KTX2 atomically (temp file + rename).
3. The result goes into a `MessageQueue`. The main thread drains it and uploads
   to the GPU in batches capped per frame.

KTX2 entries are RGBA8 with zstd supercompression, plus `viewer.alphaMode` and
`viewer.discardLevel` metadata. The cache root defaults to
`$XDG_CACHE_HOME/viewer` (or `~/.cache/viewer`); override it with `--cache-dir`.

## Building

```
git submodule update --init                 # the message template
cmake --preset release && cmake --build --preset release
ctest --test-dir build/release              # or ./build/release/bin/viewer_tests
./build/release/bin/viewer [--cache-dir PATH] [--import-textures DIR] [--grid LOGIN_URI]
./build/release/bin/j2kbench --reduce 0,2 <dir-of-j2k-files>
```

Package sources are cached in `build/_cpm-cache` unless the `CPM_SOURCE_CACHE`
environment variable is set. Debug builds turn on SDL GPU validation; on Vulkan
this only does something if the Khronos validation layer is installed.

## Code style

`.clang-format` at the root is authoritative: Allman braces everywhere, `if(x)` /
`for(...)` with no space before the parenthesis. Use postfix `i++` (clang-format
can't enforce that). Run `clang-format -i` on the files you touch.

## Swappable backends

Third-party libraries sit behind interfaces in our own modules, so they can be
swapped later. The backend's .cpp defines the factory function:

| Interface | Factory | Current backend |
|---|---|---|
| `viewer::image::J2kDecoder` | `createJ2kDecoder()` | `src/image/OpenJpegDecoder.cpp` (OpenJPEG) |
| `viewer::net::HttpClient` | `createHttpClient()` | `src/net/CurlHttpClient.cpp` (libcurl) |

## HTTP

`HttpClient` runs its own I/O thread. Callbacks run on that thread, so they
should copy what they need into a `MessageQueue` and return.

Once `cancel(id)` returns, that request's callback won't run and isn't still
running. Destroying the client gives the same guarantee for every request.
Per-login owners (`Session`, and later capabilities and the event queue poller)
send through an `HttpRequestGroup` member, which cancels whatever is still
pending when it is destroyed. That's what makes tearing down a `Session`
mid-request safe.

## Login and Session

`App` owns a `std::unique_ptr<session::Session>`. Pressing "Log in" destroys
any old session and constructs a new one, which immediately POSTs the LLSD+XML
login request (`buildLoginRequest`). `Session::update()`, called every frame
on the main thread, then:

1. parses the login reply (`parseLoginReply`) and stores a `LoginSuccess`;
2. POSTs the list of wanted capability names to the region's seed capability
   (three attempts, backing off 1 s and 2 s) and stores the granted URLs;
3. ends in `LoggedIn`, or in `Failed` with a `LoginFailure{reason, message}`.

The login server's prompts come back as failures with a reason: `tos` and
`critical` (the user must accept, then we retry with `agree_to_tos` /
`read_critical`), and `mfa_challenge` (retry with the user's token). The
retry reuses the last attempt's parameters, so the password isn't needed
again. Only the `$1$` MD5 hash of the password is kept, and the plain text is
wiped from the input buffer as soon as it's hashed.

`mac` and `id0` must describe the real machine (`queryMachineInfo`). Falsifying
them violates the Second Life Terms of Service, so there's no way to override
them.

Field names and formats follow pymetaverse's `login.py`.

After login the session is in several simulators at once. Each one is a
`Region` with its own UDP circuit, capabilities, event queue poller and
`HttpRequestGroup`:

1. The main region (from the login reply) gets `UseCircuitCode` and
   `CompleteAgentMovement`, plus a seed capability request.
2. When its `RegionHandshake` arrives we reply, send throttles, FOV, window
   size and the first `AgentUpdate` (then one per second). Once the main
   region's capabilities have arrived as well, the session is `Connected`.
3. The main region's event queue announces neighbours (`EnableSimulator`); each
   gets its own circuit and `UseCircuitCode`. `EstablishAgentCommunication`
   later hands over the neighbour's seed capability. `DisableSimulator` (or a
   circuit going silent) removes a neighbour.
4. Logging out sends `LogoutRequest` and waits up to 5 s for `LogoutReply`.
   The App does this on quit too.

Teleports and region crossings (`TeleportFinish`, `CrossedRegion`) aren't
handled yet.

## UDP circuits

`protocol::CircuitManager` runs one network thread for all circuits. Each
circuit has its own UDP socket `connect()`ed to its simulator, so the OS
drops datagrams from anyone else. The thread handles the transport layer by
itself: sequence numbers, acks (appended to outgoing packets, or `PacketAck`
after 50 ms), resending reliable packets (3x the round trip, at least 500 ms,
up to 4 times), duplicate suppression, and pings both ways. Every other
message reaches the owner as a `CircuitEvent` holding the still-encoded
message; decode the ones you need with `event.decode<msg::Foo>()`, so
messages nobody reads cost no decode time.

Message code is generated at build time: `viewer_msggen` reads
`external/master-message-template/message_template.msg` and writes one struct
per message (`viewer::protocol::msg::UseCircuitCode`, ...) with
`encode`/`decode`, keeping the template's field names. A block with the same
name as its message gets a trailing underscore (`SimulatorLoad_`). The byte
layout is checked against pymetaverse in `tests/ProtocolTests.cpp`.
