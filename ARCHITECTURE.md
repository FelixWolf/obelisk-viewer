# Architecture

## License and references

The project is licensed under the GNU Lesser General Public License,
version 2.1 (see `LICENSE`), the same license as the official Second Life
viewer.

The official viewer source is used as a reference for behavior (protocol
details, prim geometry, avatars, rendering rules), kept outside this
repository. We read it to understand what it does and then write our own
implementation; we don't copy its code. Where an implementation follows the
official viewer's algorithm closely, a comment names the viewer file it was
checked against (e.g. "matches llvolume.cpp's profile generation").

Viewer artwork we ship (in `assets/`) is Linden Lab's, licensed CC-BY-SA 3.0;
`assets/LICENSE-artwork.txt` carries their notice and lists each file and how
we use it. The build copies `LICENSE` and `LICENSE-artwork.txt` next to the
binary. Add new artwork to that list.

Texture orientation: the official viewer stores decoded images bottom-up
(OpenGL style), so Second Life texture coordinates have v = 0 at the image's
bottom row. We store images top-down; anything sampled with Second Life UVs
(object faces, the terrain blend ramp) must account for the flip.

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
   - world: the only writer of world state (`WorldThread`); publishes
     snapshots for rendering
   - network: UDP circuit plus HTTP (capabilities, event queue, asset fetch)
   - worker pool: J2K decode, KTX encode/write, mesh decode, disk I/O;
     half the hardware threads less two, so the threads above keep cores of
     their own (with one on every hardware thread, decode bursts halved the
     frame rate)
4. **Modules are libraries with one-way dependencies.** `src/<module>` builds
   `viewer::<module>`; only `app/` sees everything.
5. **Textures are cached as KTX2 only.** JPEG 2000 is decoded once and never
   stored.

## Layout

```
src/core     Uuid, JobSystem, MessageQueue, logging                -> viewer::core
src/image    J2kDecoder (OpenJPEG), mips, alpha mode, KTX2 I/O     -> viewer::image
src/assets   TextureCache: J2K -> KTX2 on disk, on worker threads;
             mesh assets (header, zlib + binary LLSD LODs)         -> viewer::assets
src/llsd     LLSD Value, LLSD+XML (tinyxml2) and binary read/write -> viewer::llsd
src/net      HttpClient interface (libcurl backend), request groups,
             UdpSocket (POSIX sockets / Winsock)                   -> viewer::net
src/protocol Message structs generated from the message template,
             packet framing, CircuitManager (UDP circuits)         -> viewer::protocol
src/session  Login, Session (one login: regions, circuits, caps,
             event queues)                                         -> viewer::session
src/render   SDL GPU device, scene pass (HDR + depth), tonemap,
             the four material pipelines, meshes, texture upload   -> viewer::render
src/render/shaders  GLSL, compiled to SPIR-V at build time and embedded
src/volume   Prim geometry from shape parameters (as llvolume.cpp)  -> viewer::volume
src/world    World state: terrain (LayerData decode) and objects
             (full/compressed/terse updates, texture entries)       -> viewer::world
src/avatar   Default avatar: skeleton, .llm body meshes, shape, animation, attachment
             points (from assets/character)                        -> viewer::avatar
src/text     Fonts (stb_truetype; Inter built in), glyph atlas,
             text layout: no GPU, no ImGui                         -> viewer::text
src/ui       ImGui context + SDL3/SDL GPU backends                 -> viewer::ui
src/app      App (owns everything), LoginPanel, TextureBrowser, camera,
             TerrainView, ObjectView, AvatarView, AnimationManager, TextureManager, procedural test scene, main()
tests/       doctest unit tests (fixtures in tests/data)
tools/j2kbench   benchmarks J2kDecoder implementations
tools/httpget    one request through HttpClient; pretty-prints LLSD replies
cmake/           CPM bootstrap + Dependencies.cmake (all versions pinned)
external/master-message-template   git submodule: Linden Lab's message_template.msg
```

## Disk cache

Everything kept between sessions (decoded textures, mesh asset blocks,
animations) is read and written through `DiskCache` (core): entries are byte
blobs keyed by kind, asset id and part (a mesh's header, skin or level of
detail), safe to use from any thread, and consistent when several viewers
share the cache. Nothing else touches cache files, so storage can change
behind it.

The one implementation, `FileCache`, keeps a file per entry:
`<cache>/textures/<xx>/<uuid>.ktx2`, `<cache>/meshes/<xx>/<uuid>.header`,
`.skin` and `.lod<N>`, `<cache>/animations/<xx>/<uuid>.anim`,
`<cache>/objects/<xx>/<region uuid>.objects` (`<xx>`: the
id's first two hex digits). A write goes to a temporary file named uniquely
per instance (a random token) and per write, then is renamed over the
entry, so readers see a whole entry and writers in other processes never mix
bytes. A single-file store (SQLite was measured: reads match files with 64
KB pages and memory mapping, but WAL writes everything twice) can replace it
later.

## Texture pipeline

1. `TextureCache::request()` queues a job on the `JobSystem`.
2. The worker reads the texture's KTX2 from the disk cache if it's big
   enough for the size asked for (`maxDimension`), or whole. Otherwise it
   takes the J2K kept beside it, or calls the request's `J2kSource` for the
   bytes, and decodes them at the coarsest resolution level still that big
   (`readJ2kInfo` reads the size and levels from the codestream header,
   `discardLevelFor` picks the level), expands to RGBA8, classifies alpha,
   builds mips, and stores the KTX2, replacing a smaller one. Most textures
   are shown far smaller than they are, so this decodes a fraction of the
   pixels: in a busy scene, 320 megapixels instead of 2296 (decoding and
   storing went from 343 s of CPU time to 59). A KTX2 below full
   resolution keeps the J2K beside it (`CacheKind::TextureSource`,
   `<uuid>.j2c`), so growing it is a decode, not a download; a whole one
   removes it. Without it (a cache from before, or one trimmed), a result
   too small says `needsSource` and `TextureManager` downloads the J2K
   again. That scene's cache took 1.6 GB (0.7 GB of KTX2, 0.9 GB of J2K)
   instead of 4.2 GB of whole-image KTX2.
3. The result goes into a `MessageQueue`. The main thread drains it and hands
   it to upload jobs.

Textures of the world go through `TextureManager` (app): `get(uuid)` returns
the GPU texture once it's ready and otherwise starts loading it. It asks the
disk cache first; on a miss it downloads the J2K from the region's
`ViewerAsset` capability (`<cap>/?texture_id=<uuid>`, `Accept: image/x-j2c`,
as the official viewer; full downloads, at most 16 at a time, retried on
failure, 404 = missing) and hands the bytes to the cache, which decodes and
stores them. Uploads to the GPU are capped per frame. Images are flipped to
bottom-up rows on load, so Second Life texture coordinates apply unchanged.

Sizes and memory: callers say how many texels across each use needs
(`texelsFor`: the object's size over its distance, times the focal length
in pixels; then `repeatedTexels` divides by the face's repeats, since a
texture tiled four times needs a quarter of the texels and one zoomed in
(a flipbook frame) more, going by the more zoomed in way and clamped to
1/8 to 8 repeats as LLFace::getTextureVirtualSize clamps; the least for
draws that only cast shadows), and a texture is loaded at the most any use asked for this frame,
as a power of two from 32 to 2048 (or whole). It grows at once and shrinks
once a smaller size has done for 5 s. The KTX2 keeps every level, so a new
size is a read of only the levels wanted (`maxDimension`), never a new
decode. Only the copies asked for are uploaded (sRGB color, linear data, or
for `data()` a CPU copy alone). GPU memory has a budget (`--texture-memory`,
2048 MiB by default): over it, the least recently drawn textures not drawn
for 120 frames are evicted; if what's in view still doesn't fit, every
texture is loaded at half the size (up to 1/16, every 2 s), back up once
under half the budget for 10 s. Reads in flight (16) and decoded bytes
waiting for upload (256 MiB) are capped too, so decoding can't run ahead
of the GPU. Every 30 s the log has a `memory:` line: the process's resident
set and what the texture, mesh and prim caches hold.
`TerrainTextures` builds each region's 4-layer terrain array once all four
textures are in (region textures from RegionHandshake, the official
defaults otherwise), scaled to a common size of at most 1024.

Each `TextureCache` instance has one results queue, so the texture browser
and the world use separate instances on the same disk cache.

KTX2 entries are RGBA8 with zstd supercompression, plus `viewer.alpha` and
`viewer.discardLevel` metadata (the J2K level they were decoded at: the
whole image is that many times twice the size).

A texture's alpha is classified once, when it's decoded (`classifyAlpha`):
opaque (all 255), transparent (all 0), a mask, or blended. Mask is the
official viewer's test (LLImageGL::analyzeAlphaData): a histogram of alpha,
plus that of a 2x2 box-filtered copy, with at most a 48th of it mid-range
and not all of it in one half short of that half's extreme, so cut-outs
with soft edges still count. Files written before that key
(`viewer.alphaMode`, which called only exact 0 and 255 a mask) are
classified again when read. Legacy faces use it with their diffuse alpha
mode (`ObjectView::material`): none and emissive are opaque (emissive's
alpha shows unlit), mask cuts out at its cutoff, and blend blends, except
that a wholly opaque texture on an opaque face is drawn solid. With no
mode (default), a mask is cut out if the face is opaque, doesn't glow and
isn't rigged (LLFace::canRenderAsMask), otherwise blended. A wholly
transparent texture (default or blend) isn't drawn at all, only picked if
clicking it does something. The cache root defaults to
`$XDG_CACHE_HOME/viewer` (or `~/.cache/viewer`); override it with `--cache-dir`.

## Building

```
git submodule update --init                 # the message template
cmake --preset release && cmake --build --preset release
ctest --test-dir build/release              # or ./build/release/bin/viewer_tests
./build/release/bin/viewer [--cache-dir PATH] [--import-textures DIR] [--grid LOGIN_URI]
    [--login-file PATH] [--start last|home|Region/x/y/z|secondlife://...] [--test-scene]
    [--pbr-terrain] [--no-vsync] [--screenshot OUT.png [--screenshot-delay S]]
    [--camera "X Y Z DISTANCE YAW PITCH"] [--texture-memory MIB] [--bandwidth KBPS]
./build/release/bin/logintest --login-file ~/.config/viewer/test-login.txt [--seconds N]
./build/release/bin/j2kbench --reduce 0,2 <dir-of-j2k-files>
```

Package sources are cached in `build/_cpm-cache` unless the `CPM_SOURCE_CACHE`
environment variable is set. Debug builds turn on SDL GPU validation; on Vulkan
this only does something if the Khronos validation layer is installed. They
compile with `-Og`: unoptimized, the per-object math alone holds a busy
region under 20 FPS. Release builds keep frame pointers, so
`perf record --call-graph fp` attributes time to callers cheaply.
`--no-vsync` shows how fast frames really are. `FrameProfile` times each
stage of the main thread's frame (session, textures, acquire, loading,
terrain, avatars, objects, ui, render, submit); the FPS overlay shows the
averages, and the screenshot log reports the frame rate, the averages and
the slowest frame's breakdown over the last 240 frames.

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

Each request names a pool (`HttpPool`), as the official viewer's policy
classes do, and each pool has its own connections on each host: `Default`
(8 per host), `Inventory` (4: AISv3 and the inventory fetches) and
`LongPoll` (no limit: the event queues). Requests wait in the client, not in
curl, for their pool to have one free. A region's capabilities all share its
simulator machine's host, together with the other regions running there, so
without pools those regions' event queues could take every connection to it.

Folders are browsed with AISv3's `depth=0` (a folder's own contents):
`depth=1` brings everything in its subfolders too, which for a large
inventory's root was 64 MB and half a minute.

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

That conversation is `LoginFlow` (app), apart from any UI: it starts
logins (typed, a saved account, a credentials file), turns a session's
failure into a prompt (`Code`, `Terms`, `Critical`, `Error`) and retries
with the answer. The ImGui `LoginPanel` only shows it, so our own UI can
replace the panel. Accounts logged in with "Remember me" are saved
(`SavedLogins`): which ones in a plain list (`accounts.txt` in the
platform's config directory: grid and name), their password hash and the
MFA hash a two-factor login returns in the system's credential store
(the keychain library: the Secret Service on Linux, the Keychain on
macOS, the Credential Manager on Windows). The MFA hash goes with each
later login of that account, so the authenticator code is asked for only
when the grid stops taking it (then it's replaced).

For development, `--login-file PATH` (viewer) and `logintest` (headless) read
a test account from a text file: username, then password or `$1$<md5>`, then
optionally a login URI (default: the Aditi beta grid). See
`src/session/include/viewer/session/Credentials.hpp`.

`mac` and `id0` must describe the real machine (`queryMachineInfo`). Falsifying
them violates the Second Life Terms of Service, so there's no way to override
them.

Field names and formats follow pymetaverse's `login.py`.

After login the session is in several simulators at once. Each one is a
`Region` with its own UDP circuit, capabilities, event queue poller and
`HttpRequestGroup`:

1. The main region (from the login reply) gets `UseCircuitCode` and
   `CompleteAgentMovement`, plus a seed capability request.
2. When its `RegionHandshake` arrives we reply. When `AgentMovementComplete`
   says it has placed the agent, it gets throttles, FOV, window size and the
   first `AgentUpdate` (then one per second); sent earlier, the region may
   drop them. Once the main region's capabilities have arrived as well, the
   session is `Connected`.

   Throttles (`AgentThrottle`) work as the official viewer's: a bandwidth
   setting (`--bandwidth`, `Session::setBandwidth`; its
   ThrottleBandwidthKBPS, 3000 by default), of which we ask for up to 1.5
   times (regions rarely fill every category at once), split across
   resend, land, wind, cloud, task, texture and asset as its presets
   (`throttleBitsPerSecond`; at 4500, objects and textures get 1528 kbps
   each). Every 5 s `DynamicThrottle` looks at the main circuit's incoming
   packet loss: over 3% steps the 1.5 down by 0.1 (to 0.2 at least), 0.5%
   or less steps it back up, and the region gets the new throttles. A
   circuit counts as lost the sequence numbers it skipped (gaps under 16)
   that don't turn up within 16 round trips, 5 s at most
   (`CircuitStats::packetsLost`, as llcircuit.cpp). Pymetaverse's fixed
   split we had before gave objects 446 kbps: the region sent objects a
   third as fast, and moving objects' updates queued behind them.
3. The main region's event queue announces neighbours (`EnableSimulator`); each
   gets its own circuit and `UseCircuitCode`. `EstablishAgentCommunication`
   later hands over the neighbour's seed capability. `DisableSimulator` (or a
   circuit going silent) removes a neighbour.
4. Logging out sends `LogoutRequest` and waits up to 5 s for `LogoutReply`.
   The App does this on quit too.

### Teleports and region crossings

The main region changes when the agent moves to another simulator; the
destination becomes the main region at once (checked against
llviewermessage.cpp and llagent.cpp):

- **Teleport.** `Session::teleportTo(Location)` asks the map for the region's
  handle (`MapNameRequest` with the region layer flag; the `MapBlockReply`
  entry whose name matches, ignoring case), then sends
  `TeleportLocationRequest`. `teleportHome()` sends `TeleportLandmarkRequest`
  with a null landmark. The simulator answers with `TeleportStart`,
  `TeleportProgress`, and then either `TeleportFailed` (the reason shows in
  `teleport().failure`) or, through the event queue, `TeleportFinish`. On
  `TeleportFinish` we open a circuit to the destination (or reuse it),
  send `UseCircuitCode` and `CompleteAgentMovement`, and post to its seed
  capability. A teleport within the region comes back as `TeleportLocal`.
- **Crossing.** Walking into a neighbour sends `CrossedRegion` (event queue).
  The neighbour already has a circuit; it gets `CompleteAgentMovement`.

Either way the agent has arrived when the new region sends
`AgentMovementComplete`; it then gets throttles, view size and an
`AgentUpdate`. `arrivals()` counts arrivals and `lastArrival()` says what
kind, so the App can react. A seed capability that differs from the
region's current one replaces its capabilities and event queue; replies to
the old requests are dropped (`Region::httpGeneration`). Until the new main
region's capabilities arrive, `capability()` answers from another region.

While moving between regions, losing the main region (`DisableSimulator`,
silence) doesn't end the session, and no `AgentUpdate`s are sent. Regions the
destination doesn't need are closed by their simulators with
`DisableSimulator`, which can take a minute.

The App keeps world coordinates relative to the agent's region: on a
teleport the origin moves there and the camera goes behind the avatar; on a
crossing the origin moves to the neighbour and the camera shifts by the
same amount, so nothing visibly moves. The camera follows our avatar (found
by agent id among the avatars) as it moves. `AgentController` turns the
arrow keys into `AgentUpdate` control flags and a body rotation; Home
toggles flying. The Teleport window takes a region name, `Region/x/y/z` or
a `secondlife://` or map link (`parseLocation`). For testing,
`--teleport LOCATION` teleports 5 s after logging in and `--walk DEGREES`
walks that way (0 = east) once there.

Objects that cross into another region (our avatar, other avatars, vehicles)
arrive as new objects there; the region they left doesn't always kill its
copy. `World` keeps an index from object id to key, and a full update for an
id it already has elsewhere removes the old copy and everything attached or
linked to it.

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

## Rendering

Each frame the App builds a `render::Scene`: camera, lighting, and lists of
terrain and object draws referring to meshes, textures and materials it
owns. `Renderer::drawScene` renders it into an RGBA16F target with a depth
buffer, then a tonemap pass (exposure, ACES mixed with linear, sRGB encoding) writes the
swapchain image, and ImGui draws on top.

### Sky and lighting

The light comes from the agent's region's environment
(`viewer::environment`, checked against llsettingssky.cpp,
llsettingsdaycycle.cpp and llenvironment.cpp). `EnvironmentManager` (app)
GETs the main region's `ExtEnvironment` capability whenever the region
changes: a day cycle whose five tracks (water, then skies for four altitude
bands from `track_altitudes`) list keyframes at positions 0..1 through the
day, naming sky frames. The position now is (seconds since 1970 +
`day_offset`) mod `day_length`, over `day_length`; the sky blends the
keyframes on either side (values linearly, sun and moon rotations by
slerp, textures switching half way), on the highest track the camera's
altitude reaches. Values under `legacy_haze` take precedence, as in the
official viewer. Without an environment (logged out, or a reply with only a
day asset) a default midday sky is used. Parcel environments aren't
fetched yet.

From a sky, `computeLighting` works out the sun and moon directions (+X
rotated by their rotations), which lights surfaces (the sun if up, else the
moon at its brightness), the light dimmed along its path through the air
(more when low: redder sunsets) and the ambient raised by cloud cover. The
App turns that into linear light the way the official PBR pipeline does
(the settings' sRGB-ish colors linearized, sunlight x1.5 and x pi for our
BRDFs' 1/pi, ambient as gray x1.5) and passes the atmosphere itself to the
shaders (`render::Atmosphere`, in the frame uniforms):

- `sky.frag`, a full-screen triangle drawn first, is the windlight sky
  model (as the official skyF.glsl, with Z up): blue and haze scattering by
  density, the glow around the sun, the darker air below the clouds near
  the horizon. Over it, the cloud layer (as cloudsF.glsl): the sky's
  cloud noise texture mapped across the sky dome (a sphere `dome_radius`
  across whose center is `dome_offset` of that below the eye, so it reaches
  the horizon), coverage from two scales of noise and the cloud density,
  lit by the sun through the air above and shaded on the side away from
  it. Clouds drift by `cloud_scroll_rate` / 100 a second, accumulated by
  `EnvironmentManager` so a change of sky doesn't make them jump. Then the
  sun and moon as soft discs and, at night, hashed stars, all hidden by
  clouds in front of them.
- `atmosphere.glsl`'s `applyAtmosphere` hazes every surface by distance
  (the official calcAtmosphericVars): its color dimmed, scattered light
  added.

The tonemap is the official viewer's default (tonemapUtilF.glsl,
RenderTonemapType 1, RenderTonemapMix 0.7): Stephen Hill's ACES fit between
ACES's input and output matrices, mixed 70/30 with the untouched color, so
dark interiors keep more of their light than ACES alone leaves them.
Exposure is the official RenderExposure, 1 (adjustable in the Environment
window), times a dynamic exposure, as the official viewer's (pipeline.cpp
generateExposure, luminanceF.glsl, exposureF.glsl): each frame the lit
scene's luminance (the middle of the view, nudged towards the ground;
surfaces at half, RenderDiffuseLuminanceScale, the sky in full, told apart
by the depth buffer) goes into a 256² map whose mip chain averages it;
from that average a one-pixel pass picks an exposure between the sky's
bounds (dark views the most, a luminance of 0.175 and up the least) and
eases last frame's towards it (90% in 2 s). The tonemap reads it there, so
it never comes back to the CPU. Skies made for PBR (with
`reflection_probe_ambiance`) range from 1 / (2 sqrt(gamma)) to
2 sqrt(gamma); older skies get neither dynamic exposure nor tonemapping,
as in the official viewer (RenderSkyAutoAdjustLegacy off). The tonemap
dithers its output so the sky's slow gradients don't band. The Environment window shows the day cycle
and can fix the time of day; `--day-time 0..1` does that from the command
line.

The sun and moon are textured quads as the official viewer's heavenly
bodies (llvosky.cpp updateHeavenlyBodyGeometry, sunDiscF.glsl, moonF.glsl),
drawn in the sky pass: each pixel's view ray meets the quad's plane, 0.1 of
the way out times the disc radius (sun 0.5, moon 0.45) times the sky's
scale, enlarged nearer the horizon (30% across, 20% up). The sky's moon
texture (DEFAULT_MOON_ID by default) is drawn times its brightness, fading
in as it rises, and hides the stars behind it; the sun only if the sky
names a texture (none by default: then only its glow, as the official
viewer). The clouds go over both. Moon phases are the texture's own, as in
the official viewer.

### Occlusion culling

What's hidden behind something isn't drawn, and costs next to nothing:
the official viewer does this with occlusion queries, which SDL GPU
hasn't, so after the scene pass `occlusion_reduce.frag` makes the view's
depth small (256 wide, each texel the farthest depth in its block) and it
is downloaded into one of three buffers (`RendererOcclusion.cpp`). Once
that frame's fence says the GPU is done (`Renderer::pollFrames`, which
also reads picks back), it becomes the `OcclusionMap` the next scenes are
culled with, with the view-projection and scene origin it was drawn at;
nothing waits for the GPU. `OcclusionMap::hidden` projects a box's corners
with that view-projection and calls it hidden only if it's wholly inside
that view, in front of its camera, and its nearest point is behind the
farthest depth everywhere it covers (tested at a level of a max pyramid
where it's a few texels across). What it can't be sure of counts as seen,
so the map being a frame or two old shows at most as something newly in
view appearing a frame late.

`Scene::visibility` uses it: in view but hidden counts as out of view, so
objects, rigged attachments, avatars and trees behind walls are skipped
before any work (`ObjectView` asks first, with a generous radius). Hidden
things still go into reflection probes that see them, and into shadow maps
only if they're big (8 m radius): smaller ones behind a wall rarely cast a
shadow into view. `--no-occlusion` turns it off.

### Instanced draws

Object vertex shaders read their draw's index per instance (the draw data
buffer holds one entry per scene draw). Each frame an instance stream
(`Renderer::pushInstances`) gets every pass's draw indices in its draw
order, at its own offset: the view passes', the shadow cascades' and the
probe face's (pushed before each pass begins: copy passes don't nest). Then
neighbours that need the same pipeline, textures and mesh are one instanced
draw. Opaque draws are sorted by pipeline, texture and buffer block, then
by mesh, so prims of the same shape and textures end up side by side;
shadow casters by kind and (for see-through ones) texture, then mesh. That
takes about a third off the draw calls.

### Stereo

A headset (`--vr`; `xr::Headset`, src/xr) goes through SDL's GPU OpenXR
support: the GPU device is made with the headset's properties (SDL makes
the OpenXR instance, on the GPU the headset uses; without a headset the
device is made as usual and the viewer stays on the screen), then the
session, a local and a view space, and one swapchain per eye at the
runtime's recommended size, 8-bit sRGB where offered. Each frame
`beginFrame` waits for the headset (that sets the pace: the window is
acquired without waiting, and presents without vsync) and locates the
eyes and head for when the frame will show, in the viewer's axes and
relative to an origin taken from the head on the first frame and on Home
(and when the runtime recenters). The App places that origin at the
camera (the avatar's eyes: VR is first person), turned to its heading;
the scene's camera is the head, with edges covering both eyes, so culling,
shadows and level of detail serve both. `Renderer::drawEyes` draws each
eye with its off-center view (`Camera::edges`: the tangents of the view's
four edges, which `Camera::projection` turns into an asymmetric frustum)
into an eye image in the window's format, so every pass works unchanged,
then copies it into the headset's image as linear values (OpenXR reads
UNORM images as linear too; an sRGB image encodes them again) and shows
the left eye in the window, cropped to fill it, with the UI over it (the
window never gets a scene of its own). Frames overlap: `xrEndFrame`, where
SteamVR waits for the GPU to finish the eyes, runs on a thread of the
headset's own, so the next frame is waited for, built and recorded
meanwhile; only `acquireImages`, just before the eyes are copied into the
headset's images, waits for it to be done (`xrBeginFrame` must come after
the last frame's end). Before that, the CPU's and the GPU's time added up
within the 11.1 ms; now the larger of the two has to fit.

When a frame is late anyway, SteamVR reprojects the last one to where the
head has turned. So that it has image to show at the edges then, each eye
is drawn wider than the display (overscan: `--vr-overscan PERCENT` per
side, 10 by default; the swapchains bigger to match, at the same pixels to
the degree) and handed back with that wider field of view, which the
runtime fits to the display.

The headset's movements are scaled to the avatar's size: by its standing
eye height (unanimated, rigged mesh joint positions included) over the
wearer's, which is the highest the head has been above the stage space's
floor (heights under 1 m or over 2.3 m, a headset on a desk or a shelf,
not counted), or `--vr-eye-height METRES` (0: not scaled). The head's and
eyes' positions (so their distance apart: a smaller avatar sees the world
bigger) and the controllers' are scaled about the headset's origin.

`Renderer::drawStereo` draws the scene once per eye, each into its own
square image, from the scene's camera moved half the eye separation to
each side, then shows the left eye in the window: what a headset needs,
before there is one. Both go through `Renderer::drawViews`, which does
what the views share once, from the scene's camera (their middle): draw
and light uploads, the shadow cascades, reflection probes, the pick, the
HUD image and the exposure. Each view then gets its own scene pass, glow,
tonemap, outline and labels; the first also makes the occlusion map and
the screenshot. `--stereo-benchmark [SIZE]` (2160 by
default) uses it with a square 90 degree view and logs every five seconds
what a frame costs: frames a second, CPU time by stage, and time spent
waiting for the GPU (the swapchain's acquire, which waits once the GPU is
frames behind). `--without shadows,lights,probes,glow,exposure,clusters,particles,water` leaves those
passes out (`Scene::glow`, `Scene::dynamicExposure`, and the scene's
shadow, light and probe settings), to see what each costs; storage buffers
not made yet are stood in for by an empty one (`Renderer::storageOr`), and
the exposure is 1 until one is measured.

### Picking and touching

The pointer is a ray: from the camera through the mouse now, from a VR
controller later (`PointerController`). Each frame `App` aims it
(`Scene::pick`) and the renderer finds what it hits by drawing one pixel
straight down the ray (`RendererPick.cpp`, `pick.frag`): a perspective view
1 milliradian wide along it, with every pickable draw in it (and, in a
second pixel, a tiny orthographic view at the HUD point: HUDs come first).
Four targets carry the nearest face's pick id and what the official pick
reports: position, normal, binormal, texture coordinates after the face's
transform, and the face's own. They're copied to a transfer buffer and read
once that frame's fence says the GPU is done (`Renderer::takePick`), a
frame or two later, so nothing waits; while one is on its way, new
requests are skipped. Being the GPU's own drawing, it hits what's drawn:
sculpts, mesh, alpha masks (see-through texels don't count), and only
faces' front sides, as they're drawn and as the official pick's one-sided
test (double-sided PBR materials both): a face turned away, culled where
it's drawn, isn't hit (on the HUD, a part turned away in front of another
would otherwise take its clicks).

`ObjectView` gives each face a pick id: the prim's local id, an index for
its region and the face. Objects whose click action is "ignore" get none
(clicks go through them), nor does rigged mesh (the official left click
doesn't pick it). Which faces a ray hits is the official pick's rule
(LLVOVolume::lineSegmentIntersect): a face of tint alpha 0 only if its prim
is clickable itself (a touch script in it, not only in its root, a click
action of its own, or physics; listed as pick-only draws, which nothing
else draws: invisible buttons are common), so a HUD's hidden pages and
overlays are clicked through to what shows behind them; and only where the
texture's alpha is above 32 of 255, whatever the tint (a wholly see-through
texture is never hit, even on a clickable prim), unless the face's legacy alpha mode ignores alpha
(none, emissive, a mask cut at 0).

What a click does is decided as the official viewer does (lltoolpie.cpp):
touchable if a script in the prim or its root listens for touches
(UpdateFlags 0x80) and its click action isn't "disabled"; the click action
(sit, buy, pay, open, play, open media, zoom) is its own or its root's,
none for attachments. Each pick is made twice, as the official left
click's (lltoolpie.cpp handleMouseDown): as seen (above), and transparent,
counting every face of what can be picked whatever its alpha (faces nothing
of shows are listed for it alone, `ObjectDraw::pickTransparentOnly`;
pick.frag's pixels 2 and 3). The hand cursor, tooltip and outline follow
the seen one (the official hover's); a press takes the transparent one if
both are the same prim or either is nothing, if it's worn (HUDs included),
or if clicking it does something, else the seen one if that's worn or
clickable, else the transparent one (`PointerController::pressed`): an
invisible button in front of what shows (a mesh HUD's face whose texture
coordinates sit in a see-through corner) takes the press. Pressing the left
button on something touchable
sends ObjectGrab (the prim's local id, the offset from it to its root in
the root's frame, and the surface: llDetectedTouchUV, ST, Face, Pos,
Normal, Binormal; positions in the agent's region's coordinates, HUD hits
in HUD space); while it's held, where on that prim the pointer is now goes
as ObjectGrabUpdate whenever it changes (face -1 off the prim, as the
official viewer's non-physical grab: sliders and colour wheels read
llDetectedTouchUV in touch()); letting go sends ObjectDeGrab. Click actions other than
touch aren't done yet (they touch instead). Over something clickable the
cursor is a hand (`App::updateCursor`, SDL's system cursors: also the
cursor the RmlUi page under the mouse asks for, `Surface::cursor`, each
page's own; ImGui picks its own over its windows), a tooltip names the action (a label by the point the
pointer's on, over everything: `Label::overAll`; the same size on screen
at any distance, so it shows in first person and in each of a headset's
eyes too; on the HUD for HUD attachments), and the prim gets an
outline: its visible pixels (depth tested against the view) into a mask,
then a 2-pixel gold outline around the mask over the view, after
tonemapping. `--pointer X,Y` (fractions of the window) and
`--click-at SECONDS` stand in for the mouse when testing, and
`--point-at UUID` points from the camera at an object.

To compare an avatar with the official viewer:
- `--dump-avatar NAME|self|UUID` logs everything about the avatar's look
  once, `--dump-at SECONDS` in (default 45):
  - its bakes and visual parameters;
  - the animations it plays;
  - each attachment, prim by prim, with each face's texture (and the bake it
    stands for), tint, legacy and PBR materials and overrides;
  - what was decided for each rigged face when drawn: the "Worn mesh"
    developer window's text.
- `--front-view` puts the camera close in front of our avatar after logging
  in.
- An explicit `--grid` now overrides a login file's grid.

The tooltip uses the script's own words for the action (llSetTouchText,
llSetSitText) and shows the object's name: those come in ObjectProperties,
which regions send only for selected prims, so once the pointer has rested
a quarter second on something clickable `ObjectProperties` selects the prim
(ObjectSelect) and deselects it as soon as they're in (as the official
viewer does for its pie menu and inspector). They're kept a minute, then
asked again; physical objects aren't asked (selecting stops them). HUD
parts get no outline (by choice: HUDs are built as flat layers, and the
tooltip names what a click does). Not yet: dragging physical objects,
avatars.

### VR controls

The radial menu and the keyboard (`VrControls`, `RadialMenu`,
`VrKeyboard`) are flat panels in the scene, pointed at with up to two
pointers: the controllers' aim rays in a headset; the mouse in first
person on the desktop, so they can be tried without one (Tab held frees
the cursor; it's free while the menu is open).
- The menu button (B/Y; the right mouse button) held half a second opens
  the radial menu at that hand, following it and facing the eyes (on the
  desktop, half a metre ahead); a quick press closes it, or, closed, calls
  the main-menu hook (`setMainMenu`, nothing yet). It's chosen from by
  that hand's stick, pushed to a slice and let go, or by the other hand's
  ray and its trigger (a click on the desktop). A cursor shows where it's
  pointed; the middle is a resting place (a submenu's name), not a choice.
- What's in it is data, written in `App::radialMenu()`: `MenuItem::run`
  and `MenuItem::submenu`, with `.when` (enabled only then), `.named` (a
  label worked out when shown) and `.staying` (stays open: toggles). A
  level holds 8 slices; a submenu gives one to "< Back", and a level with
  more items pages, its last slice "More p/N".
- The keyboard (Chat) opens ahead and below the eyes; Send says the line in
  local chat. Both panels move with the avatar.
- Presses not on the controls are the world's: the right hand's trigger
  (or, with the cursor free, the left button) is the primary action: the
  object's sit click action sits, a touchable object is touched, and held
  0.6 s on anything else sits on it there (AgentRequestSit with the offset
  in the object's frame; asked to walk there, we ask AgentSit at once).
  With the center dot the left button stays the mouselook button.
- Panels (`render::Panel`: boxes, text, boxes over the text) and pointer
  rays (`render::PointerLine`) are drawn by the label pass over everything,
  in panel pixels mapped through the panel's frame into each eye's clip
  space (label vertices are clip-space positions).

### The viewer's own interface (RmlUi)

The interface people use (not the ImGui debug windows, shown only with
`--developer`: stats, the FPS overlay, the texture browser, teleport,
environment, the session's details (regions, circuits, traffic) and the
worn mesh inspector, which reports what was decided for each face of the
rigged mesh we wear (alpha mode, its legacy material, mirroring); ImGui
still draws the login window if the interface can't be set up) is made with RmlUi
(`viewer::gui`, src/gui): pages written as documents (RML, like HTML) and
style sheets (RCSS, like CSS) in `assets/ui`, read at run time and copied
next to the program. `--ui-dir assets/ui` reads them from the source
folder instead, and F5 reads them again, so they can be edited while the
viewer runs.
- `gui::Gui` sets RmlUi up: its system interface (time, log, clipboard,
  the keyboard a text field asks for), its renderer, and the built-in font
  (Inter, through FreeType). RmlUi keeps these process-wide, so there's one
  Gui (the app's).
- A `gui::Surface` is a page: an RmlUi context drawn into its own image
  (RGBA8, premultiplied, sRGB-encoded values, as the window's), given the
  pointer's input in its pixels. On the desktop the image goes over the
  window (`Gui::drawOver`); in first person and VR it's a `render::Panel`'s
  image, drawn in the label pass over the scene, and the pointers use it
  through `VrControls::Page` (the controllers' rays, a trigger to click,
  a stick to scroll).
- The renderer (`gui::Renderer`, GuiRenderer.cpp) collects a page's draws
  while RmlUi renders it, copying the geometry, then records them in one
  go: new textures and the vertices up, then one render pass. It does
  geometry, textures, scissor rectangles, transforms and gradients
  (`gui_gradient.frag`, as RmlUi's OpenGL 3 renderer reads their
  parameters); not yet clip masks, layers or filters (RmlUi leaves those
  effects out).
- Pages name files by path from where they are, `res://PATH` from the
  documents folder's top, and the app's images by URI: `asset://UUID`
  is a texture from the grid's asset servers (through `TextureManager`,
  asked for each frame it's drawn, `?size=N` texels across, 256 if not
  said; nothing drawn until it's in). The size such an image has isn't
  known when the page is laid out, so pages give them one. Other schemes
  can be added (`Gui::addImageScheme`). `image://NAME` is an image the app
  decoded and gave (`Gui::setImage`, `gui::decodeImage` on the workers):
  pictures from the web. Pages show one only once it's there (`data-if`
  on its name), so its size is known and it can be fitted (`cover`).
- The login screen (`LoginScreen`, `login.rml`) is a page of its own on a
  panel standing in the login lobby until we're in the world: bodiless,
  we stand in the home scene (on a plain floor among the test objects if
  there's none) and walk about on the ground (`CameraController`'s walking:
  WASD and the right button to look; a headset's left stick walks where
  the head looks, the right turns by steps), pointing at the panel with
  the mouse or the controllers' rays; it stays where it stands
  (`VrControls::openPageAt`, with a fixed anchor). It's a view of
  `LoginFlow`, as the ImGui panel was. The remembered
  accounts, each with its profile picture (`AccountPicture` asks the
  region's AgentProfile capability for ours once a login, and the saved
  accounts list keeps its id; the texture comes from the disk cache before
  the next login), log in with a click; Add account is the form (username,
  password, grid, remember); where to start is chosen for either. The
  grid's questions (an authenticator code, the terms, a message) and why a
  login failed show over them, "Logging in..." while one is on its way.
  Log out is on the main menu's Home tab. Screenshots are read back at the
  frame's end, so they show the pages over the window too.
- The home scene (`world::loadHomeScene`, `assets/home.scene`, or
  `--home-scene PATH`) is the lobby's scene: binary LLSD that
  `default_scene/sceneConvert.py` makes from an OXP export, shaped like a
  simulator's state (terrain heights, its textures and water, linksets of
  prims). It becomes the updates a region would send, applied to a
  `World` of its own (`World::standAlone`, so views following snapshots
  start afresh at either side of it), a made-up region (`kHomeRegion`);
  until we arrive, `buildScene` shows it in the session world's place,
  its region the origin. Its assets come from the file's `asset_cdn` (the
  viewer-asset service needs no login). Faces' legacy materials come in
  the file (given ids from their contents, put in `LegacyMaterialManager`);
  PBR faces have only their overrides, over the default material
  (`kHomeBlankMaterial`, put in `MaterialManager`). Linksets named
  `#HOME:Floor`, `Landing`, `Login`, `News` or `GridStatus` aren't drawn:
  they place the eyes (1.7 m over the floor, at the landing, facing its
  +Y) and the panels (facing their +Y). F5 in the lobby reads it again.
- viewerconfig (`ViewerConfig`; the contract is `docs/viewerconfig-api.md`)
  is the project's own web service, asked instead of Linden Lab's sites
  (`--viewerconfig URL`, or `off`): its config (flags, settings' defaults,
  the newest release, which home scene) at startup, its news and grid
  status when the lobby shows and every so often while it does. Requests
  carry nothing but the User-Agent; flags and settings are picked for our
  version and platform here. Each answer's last good copy is kept in the
  cache folder's `viewerconfig/` with its ETag (sent back for a 304) and
  shown until a newer one comes. A home scene it names is downloaded
  once, checked against its size and SHA-256, and replaces the built-in
  one (not one given with `--home-scene`).
- The lobby's boards (`LobbyBoards`, `news.rml` and `grid_status.rml`)
  show viewerconfig's news and grid status on panels at the scene's
  `News` and `GridStatus` markers. They're `VrControls`' standing pages:
  pointed at, pressed and scrolled (a controller's stick, the mouse's
  wheel) as the login panel is, the nearest page a ray meets taking it. A
  news entry clicked shows whole on its board (our posts' text) with Back;
  links (an entry's, Linden Lab's status page, an incident's) open in the
  system's web browser (`SDL_OpenURL`): the viewer has none, so in a
  headset the page says it opens on the desktop. `--pointer X,Y` (and
  `--click-at`) stand in for the mouse on the lobby's pages too.
- Unstyled, an RmlUi scrollbar takes the whole width of what scrolls;
  `theme.rcss` gives scrollbars their size.
- Data reaches pages through RmlUi's data models (bound C++ values, made
  dirty when they change); a data model goes on an element inside the
  body, not the body itself, or `data-for` isn't expanded.

The main menu (`MainMenu`, `main_menu.rml`) has tabs: Home, Chat (see Local chat), Destinations,
Inventory, Outfits, Parcel, Media, Options and About. Home, Destinations,
Inventory, Parcel and About show something so far. Home: a greeting, SLT and local clocks, the viewer's frame rate, the
region's (SimStats: `Region::stats`) and its time dilation, and the region
we're in. Destinations: the Destination Guide (`Destinations`), in three
views, each kept hidden behind the next so Back finds it where it was
scrolled to: its categories (the guide's v2 API; those of its
`secondlife_dg` channel with places in them, What's Hot Now, the busiest,
first, then by name), a category's places (a page of 100, "Show more" for
the next), and one place alone (its full-sized picture, description and
Teleport). Places are kept to the resident's maturity (the login's
`agent_region_access`) here: the API answers every place whatever it's
asked. It doesn't count people (always 0): the world map does
(`GridPopulation`). A place's region is looked up by name (MapNameRequest,
which ignores case; MapBlockReply has the regions as they're named and
echoes what was asked at 0, 0, so the block of that name in any case not at
0, 0 is the region, and none means there's no such region) and kept for
the session; then the people in it (MapItemRequest for agent locations
with its handle; handle 0 is the asking simulator's own region, not the
grid). An answer doesn't say which region it's for: its spots' global
positions do, each with how many are there; an empty region sends none, so
one with no spots 5 s after asking is empty. A count is kept five minutes
and asked again only when it's shown after that. Regions are
taken as 256 m across (the message template's MapBlockReply has no Size
block), so a var region's people beyond its first 256 m aren't counted. Categories don't say their
maturity; those named "Adult..." are left out unless the resident's is
Adult. Cards have a border always there, clear until pointed at, so
nothing moves. Pictures no longer shown are let go when another list
comes. Inventory: browsed as a file explorer (`InventoryBrowser`): the root
folder (AISv3's "root" alias; the reply's first folder), each folder's
contents asked for the first time it's opened, folders then items by name,
links as what they point at (a folder link opens its folder), each with its
kind's icon (`assets/ui/icons/<kind>.svg`, drawn by RmlUi's SVG plugin with
LunaSVG at the size it's laid out at, so sharp at any scale), crumbs of the
folders opened, and an item alone (its picture for textures, creator,
what we may do with it, when it was made, description). The list is kept
hidden behind an item, as Destinations' views are. Objects can be attached
(RezMultipleAttachmentsFromInv, added at their last point) and taken off
(ObjectDetach), and what's worn is marked. `CurrentOutfit` keeps the
Current Outfit Folder in step, as the official viewer does: a link made
(AISv3, POST to the folder with a transaction id) once an attachment asked
for arrives, and removed (DELETE) once one taken off has gone, so the next
login wears what we chose. One taken off is also dropped from the login
outfit's list (`OutfitAttachments::forget`), or its retry pass would put it
back. Clothing and body parts are worn through the COF itself: Wear links
one instead of the others of its kind (wearable type: the item's flags' low
byte; a body part always replaces), Add layers clothing over them, Take off
removes clothing's links; once the writes are answered, the COF is fetched
again for its version and the region asked to make our look from it
(UpdateAvatarAppearance, `cof_version`; asked again with the version it
says it "expected" when ours is stale). The new look comes as
AvatarAppearance. Landmarks and the rest are still to come.

Outfits: the folders in My Outfits as cards (their thumbnails: a folder's
`thumbnail.asset_id`), and one alone with what's in it (its links with
their targets: AISv3's links call). Wearing one replaces what's worn, as
the official viewer's updateCOF: the COF is slammed (AISv3 PUT of the
folder's links) to the outfit's body parts (and those worn of kinds it
hasn't), clothing (each link's description kept: it orders layers),
attachments and gestures, and a folder link to the outfit (the outfit
worn, marked); then our look is asked for, and the attachments put on (five
a frame) and taken off to match. An outfit is only worn once its contents
are known, and never when nothing in it can be worn (or everything would
come off). A fetch of a folder's children now makes the model's contents
of that folder what came (`Model::prune`), so the COF is right after a
slam. Parcel: the one we're on (`ParcelInfo`): the
region sends its properties on the event queue whenever we come onto one
(ParcelProperties; sequence 0 or up, negative ones answer requests for
others), which the session passes on (`drainRegionEvents`, the events it
doesn't use itself). Its picture (asset://), owner and group (avatar
names; groups' from UUIDGroupNameRequest), area, objects, sale price,
rules (ParcelFlags, four bytes most significant first), description and
music, and its estate (EstateCovenantRequest once a region: its name,
owner, null on mainland, and whether it has a covenant; the covenant's
text, an estate asset, isn't fetched yet). About: the version, and the license of everything in the
viewer, picked from a list: ours, the font's, the artwork's and each
shipped library's, gathered by the build (`viewer_license` in the root
CMakeLists.txt and cmake/Dependencies.cmake, written into one embedded
text by cmake/Licenses.cmake). A library added to the viewer gets a
`viewer_license` line. Escape opens and closes it on the desktop (over the middle of
the window), as does a quick press of the menu button in first person and
VR (on a panel ahead, moving with the avatar). `--main-menu [TAB]` opens it at
the start (on that tab).

### Script dialogs and permissions

What scripts ask of us waits in `ScriptDialogs` (no UI; tested alone),
oldest first, and `ScriptDialogPage` (assets/ui/script_dialog.rml) shows
the oldest, with how many more wait:
- Dialogs (ScriptDialog, llDialog): the message and up to 12 buttons, three
  to a row, the first three along the bottom as llDialog lays them out
  (`dialogRows`). A button sends ScriptDialogReply (its index and label);
  Ignore sends nothing. A dialog whose one button is `!!llTextBox!!` is a
  text box (llTextBox): its text goes as the label, index 0, cut to the
  field's 254 bytes. A newer dialog from the same object replaces the one
  waiting.
- Permission requests (ScriptQuestion): what it asks for in words
  (`scriptPermissions`, llRequestPermissions' bits; taking money is marked
  with a caution), Allow or Deny (ScriptAnswerYes with the bits granted, or
  0). Bits we don't know are never granted, and a request of only those
  isn't shown. More than 5 from one owner's scripts in 10 seconds are
  dropped (our own objects each counted on their own), as the official
  viewer. Experience requests (llRequestExperiencePermissions) are asked as
  any other, not remembered per experience.
- Web pages (LoadURL, llLoadURL): Open in browser (the system's) or Not now;
  only http and https.

Answers go to the region that asked (`Session::replyToScriptDialog`,
`answerScriptPermissions`); logging out drops what's waiting. The page is
over the window's top right on the desktop (third person; clicks and the
wheel on it aren't the world's), and on a second VR controls page (the
notice: ahead and to the right, moving with the avatar) in first person and
in a headset, pointed at as the main menu is (in first person on the
desktop, hold Tab to point; a text box takes the keyboard). In a headset a
text box's "Type a reply" opens the VR keyboard, whose Enter sends it
instead of chatting. Not yet: muting an object or owner (Block), script
teleport requests (llMapDestination), group names for group-owned web
pages.

### Glow

As the official viewer's (LLPipeline::generateGlow, combineGlow, at its
default settings). After the scene pass, glowing faces (the texture
entry's glow) draw again into an R16F mask, depth tested against the view,
adding up how much each pixel glows: a legacy face its glow times its
texture's alpha (emissiveF.glsl), a PBR face its glow times its brightest
emissive channel (pbrglowF.glsl), rigged mesh too. `glow_extract.frag`
then takes the view as the display will show it (the tonemap maths are
shared: `tonemap.glsl`) times that glow (at most 1) into a 512x512 image
(RenderGlowResolutionPow 9), which `glow_blur.frag` blurs with the
official eight-tap kernel across, down, across and down (RenderGlowWidth
1.3, RenderGlowStrength 0.325 a pass). The tonemap pass adds the result
over the view, after sRGB encoding, as glowcombineF.glsl does. Nothing is
drawn when nothing glows. Not yet: glow on HUDs.

### Floating text

Text is our own, not ImGui's, so the same code can serve the UI to come.
`viewer::text` loads fonts with stb_truetype (Inter Regular, as the
official viewer uses, is built in: `assets/fonts`, SIL Open Font License),
rasterizes glyphs the first time they're needed into a coverage atlas
(`GlyphAtlas`, shelf packed, cleared and refilled when full), and lays out
UTF-8 text (`layoutText`: line breaks, word wrap, kerning, alignment, glyphs
at whole pixels so text stays sharp).

Floating text (llSetText, with its color) becomes a `Scene::Label` (lines,
each with its own size and color) per prim
(`FloatingText.cpp`), placed as the official viewer places it
(LLViewerObject::updateText, LLHUDText): 0.6 of the prim's height above it,
brought nearer the camera by the prim's radius, fully shown within 8 m and
faded out over the next 4. The renderer (`RendererText.cpp`, `label.vert`,
`label.frag`) lays labels out at the official size (10 points), centered
with the last baseline 12 pixels above the point, far ones first, with a
1 pixel black drop shadow as strong as the text is light. They're drawn
after tonemapping, over sRGB-encoded values as the official viewer's text
is, depth tested at the label's point so whatever is in front hides them.
HUD attachments' text is drawn into the HUD image, never hidden.

### Name tags

Avatars' name tags are labels too (`NameTags.cpp`), styled as the official
viewer's defaults (LLVOAvatar::idleUpdateNameTag, LLHUDNameTag): a black
bubble at half opacity with rounded corners (quarters of a disc the glyph
atlas draws), holding a status line (Away, Do Not Disturb, Editing
Appearance, from the signaled animations), the group title, the display
name (10 points) and, when the display name isn't made from it, the
username (9 points, a little darker). The bubble sits 25 pixels above a
point over the head (an ellipsoid around the body toward the camera's up,
idleCalcNameTagPosition). Tags fade as the avatar gets small on screen
(full from 5 degrees, gone at 2) and with distance (from 20 m, gone at 25).

Legacy names and group titles come in object updates (NameValue: "FirstName
STRING RW SV ..."); display names from the GetDisplayNames capability
(`AvatarNames`, as LLAvatarNameCache: ids batched into a URL up to 3500
characters long, one request at a time, kept 20 minutes). Until an
avatar's display name arrives, its legacy name stands in.

### Local chat and chat bubbles

`LocalChat` takes in ChatFromSimulator as the official viewer does
(process_chat_from_simulator): only fully audible chat; "/me" lines become
"Name ..." (the complete name: display name and username); whispers and
shouts get "whispers: " and "shouts: "; tabs become four spaces. It keeps a
log (the Chat tab's, and the console's: each line is logged too) and, per
avatar, what its bubble shows
(LLVOAvatar::addChat): the latest 12 lines (1023 characters at most), all
gone 24 seconds after the latest, and whether it's typing (chat types 4
and 5; a real line ends it). `Session::say` sends ChatFromViewer (and
typing starts and stops); `--say TEXT` (`--say-at SECONDS`) tries it.

Chat bubbles are the name tag's chat mode (idleUpdateNameTagText): the
names move into a band across the bubble's top (black on white at the
bubble's opacity, all at 10 points), and below them the lines, left
aligned and wrapped at 190 pixels, ours yellow and others' white, dimming
to gray after 8 and 16 seconds and gone after 24, with "." to "..." while
typing. Chatting tags are seen to 40 m (fading to 45). Not yet: italic
whispers and bold shouts (no such fonts yet), muting.

Chat is read and written on the main menu's Chat tab (`ChatView`), not in
popups: objects can chat a lot, and a window over the view doesn't suit a
headset. Conversations are on the left (Nearby, then instant messages and
group chats, newest first), each with how many lines weren't seen (ours
don't count); the chosen
one's lines on the right (time, name, text; newest at the bottom, followed
unless scrolled back), and a line to type into (Enter or Send). Objects'
chat can be left out; what's said to us alone (llOwnerSay) always shows,
marked. What's typed goes through `parseChatInput`: "/5 text" is said on
channel 5, as the official chat bar; anything else on 0 ("/me" included).
In a headset the field is a button that opens the VR keyboard, whose Enter
says the line in that conversation (the radial menu's Chat: local chat).
Not yet: typing starts and stops sent while typing on the tab, a sound for
new messages.

### Instant messages

`InstantMessages` keeps conversations as the official viewer
(llimprocessing.cpp, llimview.cpp):
- One to one: ImprovedInstantMessage dialog 0; the session is the two
  agents' ids XORed (`directSession`). What we send (`Session::
  sendInstantMessage`: online, from our name, an empty binary bucket) shows
  at once, as the region doesn't send it back. Typing starts and stops (41,
  42) show under the conversation; do-not-disturb answers (20) are marked.
- Group and ad-hoc chats: the first message comes as ChatterBoxInvitation
  on the event queue (the chat's name in the binary bucket), which is
  accepted (ChatSessionRequest "accept invitation"); later messages are
  dialog 17 (IM_SESSION_SEND), taken only for chats we're in. Ours go to
  the session (to and id both its id) and show when it sends them back.
- Objects' messages (llInstantMessage, dialog 19) go to local chat, as said
  to us alone.
- What came while away is read once a login (ReadOfflineMsgs), marked
  saved, with its own time.
Offers and the grid's notices are `Offers`' (below). Not yet: starting a
conversation (no people list), leaving a group chat, muting.

### Offers

`Offers` queues what people and objects offer us, which also comes as
instant messages (live, or ReadOfflineMsgs' records turned back into
messages by `offlineMessage`). The App gives each message to `Offers`
first, then to `InstantMessages`. The notice page shows the oldest offer
when no script dialog is waiting, with one or two answers. Answers become
`OfferAnswer`s, which the App carries out. As the official viewer
(llimprocessing.cpp, llviewermessage.cpp):

| Offer (dialog) | First answer | Other answer |
| --- | --- | --- |
| Teleport (22) | TeleportLureRequest | IM 24 |
| Summons (25) | taken at once, no notice | |
| Teleport asked for (26) | StartLure to them | nothing |
| Friendship (38) | AcceptFriendship (calling card folder) | DeclineFriendship |
| Group invitation (3) | IM 35 to the group (the fee is on the notice) | IM 36 |
| Inventory from someone (4) | kept (accepted on arrival: IM 5) | moved to the Trash |
| Inventory from an object (9) | IM 10 into its type's folder | IM 11 |
| Group notice (32) with an attachment | IM 33 into its type's folder | IM 34 |
| Web page from the grid (28) | opened in the browser | nothing |
| Grid message (1), object alert (31) | OK | |

Friendships offered while we were away go by the AcceptFriendship and
DeclineFriendship capabilities. So do group invitations that came while
away with no transaction (AcceptGroupInvite, DeclineGroupInvite).

A folder for accepted inventory is the system folder of the item's type
(the folder type number equals the asset type). Our inventory model doesn't
know the system folders until the root's contents are fetched, so answers
that need one ask for them (`InventoryBrowser::root`). They wait until the
folders are known, or for 15 seconds, then fall back to the root the login
named.

Differences from the official viewer:
- An object's offer that is declined is declined. The official viewer takes
  it into the Trash instead.
- No muting and no do-not-disturb.
- No checks of the region's rating before teleporting: the region refuses,
  and the teleport fails with its reason.
- Others' answers to our offers (accepted, declined) are only logged.

### HUDs

HUD attachments (attachment points 31 to 38, on the mScreen joint) are
drawn into an image of their own (`RendererHud.cpp`), not onto the view,
so the image can be shown anywhere: over the view, as the official viewer
does, or on a panel in the scene (`render::Hud::placement`): on the left
forearm, or floating in front of the camera, where a VR headset would show
it. XR can take `Renderer::hudImage()` as it is.

HUD space is the official one (llviewerdisplay.cpp get_hud_matrices): an
orthographic camera looking along +X, +Y left, +Z up, the image 1 unit
tall and its aspect wide (zoom divides both). The points sit at their
avatar_lad.xml positions with Y times the aspect (the official mScreen
joint is scaled (1, aspect, 1)), so corner HUDs stay in their corners; the
objects themselves aren't scaled. `ObjectView` marks HUD attachments'
draws `hud`: they're never culled, always at the most detail, with
textures as big as they show on the image, and they give no lights or
reflection probes; the view, shadows and probes leave them out. They're
drawn as the official viewer draws HUDs: unlit (legacy faces fullbright;
PBR base color plus emission, as pbropaqueF.glsl IS_HUD), opaque faces
first, then blended ones from far to near, written in sRGB (they're drawn
after tonemapping there) into a transparent image, which leaves it
premultiplied by coverage. Over the view it's blended in sRGB after the
tonemap; on a panel it's turned back into linear light for the HDR view.
Clicking one is picking (the HUD point is tried first) and touching as
anywhere (Picking and touching), dragging included. Not planned: an
outline on HUD parts (it would get in the way of how HUDs are built), HUD
zoom, and rigged mesh on HUDs (the official viewer doesn't draw it
either).

### Reflection probes

Reflections, and on skies made for PBR the ambient light, come from
reflection probes (`RendererProbes.cpp`, `probes.glsl`), as the official
viewer's (LLReflectionMapManager): cubes captured of everything around a
point, 128² per face, one face a frame. There are three kinds:

- The default probe, 64 m above the camera: the sky and terrain only
  (all terrain, in view or not), recaptured every 2 s
  (`Reflections::updateSeconds`, the official RenderDefaultProbeUpdatePeriod).
  It's what surfaces fall back on where no other probe reaches.
- Manual probes: prims set to be reflection probes (extra parameter 0x90:
  ambiance, near clip, box or sphere), a sphere their X size across or their
  box. They're usually where builders want rooms lit from inside.
- Automatic probes (`AutomaticProbes.cpp`), where the official octree puts
  them: in every 32 m node, and those exist only where the 64 m node around
  them held more than 128 prims. A grid stands in for the octree. A probe
  sits at the middle of its prims' bounds, its radius reaches their
  corners (8 m at least), and it's at least 2 m above the ground and high
  enough that its near clip (half its radius) clears it.

Local probes (manual and automatic, `Scene::probes`) see objects (not
avatars or worn mesh) out to 64 m (RenderReflectionProbeDrawDistance). The
63 nearest the camera get cubes (cube arrays, cube 0 the default probe);
the nearest one without a capture goes next, otherwise the one captured
longest ago, at most every 10 s (`refreshSeconds`). The face to capture is
announced a frame ahead (`Renderer::nextProbeFace`, `Reflections::
probeFace`), so the scene lists the objects in it even when they're out of
view, and only those. Captures are lit with the sky's ambient, not the
probes' (as the official irradiance pass): lit by themselves, each would
come out brighter than the last. When the sixth face is in, the cube is
mipmapped and filtered: radiance, 7 levels (128 down to 2, max_probe_lod
6), each face of level n the capture through a GGX lobe of roughness n / 6
(64 importance samples, each reading the capture's level that matches its
solid angle), and irradiance (16², the cosine-weighted average around each
direction, 128 samples). A cube face's layout is the mirror image of a
camera's, so faces are rendered of the world mirrored in y (`faceView`),
which keeps the scene pipelines' winding; lookups mirror y back.

Shading: the 32 nearest local probes with a capture go to the shaders
(a storage buffer). For each surface, the probes whose sphere or box holds
it are blended as the official viewer does: in a sphere, more weight
nearer the middle (1 / distance, fading out over the outer half; new
captures fade in over a second); in a box, more weight away from its walls,
and none for automatic probes at all; manual probes outweigh automatic
ones as they get closer. Manual probes follow reflected rays to their
volume's edge (parallax); automatic ones don't, as in the official viewer.
The default probe gets a tiny weight, so it shows only where nothing else
reaches. PBR surfaces take image-based light: diffuse from the irradiance
and specular from the radiance at their roughness, scaled and biased by the
split-sum table (`brdf_table.frag`, made once: 128², GGX with Smith
visibility). Ambient light is a probe's irradiance times its ambiance (the
sky's `reflection_probe_ambiance`, or the probe's own if more), mixed with
the sky's ambient while that's under 1; skies made before PBR keep the
sky's ambient ("classic mode"). Legacy faces with glossiness reflect the
probes softly at that glossiness (stronger at grazing angles, only on what
isn't already bright: the official applyGlossEnv), and with environment
intensity sharply, mixed in by it (applyLegacyEnv). Not yet: dynamic
probes (avatars), mirrors (hero probes), screen-space reflections, the
official probes' ray-cast nudge towards empty space.

### Shadows

The sun (or the moon, while it's the light) casts shadows through four
cascaded shadow maps (`ShadowCascades.hpp`): the view out to 128 m cut into
slices that grow with distance (85% logarithmic splits), each covered by a
2048x2048 layer of one depth texture array, rendered from the light with an
orthographic projection around a sphere enclosing the slice. The sphere
keeps its size as the camera turns and its center snaps to whole texels,
so edges don't shimmer. Casters up to 300 m towards the light beyond a
slice still cast into it.

Each cascade draws the terrain and the casters whose bounding spheres reach
it (meshes keep their bounds; skinned meshes get a 4 m sphere around the
root), leaving out ones smaller than four texels there. Opaque casters need
no fragment shader work; alpha-masked and blended ones sample their
texture and cast where at least half opaque (full-bright blended ones,
like glowing signs, cast nothing). Both sides of faces cast, with a depth
bias. Culling keeps out-of-view objects whose shadow can reach the view
(`Scene::visibility`: their sphere swept away from the light against the
view frustum), marked shadow only.

Surfaces pick the cascade by their depth along the view, push the lookup
1.5 texels along their normal against acne, and filter 3x3 comparisons; the
shadow scales the sun's light only, and fades out over the last 10% of the
distance. Measured at Ahern in late afternoon: about 17,000 shadow draws
across the cascades, frames 3.0 ms without shadows, 5.0-7.3 ms with (most
of it recording draws). `--no-shadows` and the Environment window turn
them off.

### Local lights

Any prim can give light: extra parameter 0x20 (linear color, alpha the
intensity; radius, cutoff, falloff), and 0x40 makes it a projector (a
texture, field of view, focus, ambiance). `ObjectView` turns each lit prim
within range into a `render::LocalLight` before culling it (a light out of
view still lights the view), as the official viewer's deferred lights
(pipeline.cpp): reach 1.5 times the radius, half the falloff, color times
intensity. The renderer keeps the lights whose reach touches the
view frustum, nearest first, at most 64 (`Renderer::kMaxLights`), in a
storage buffer every surface shader loops over (`lights.glsl`):
(1 - clamp((d + falloff) / (1 + falloff)))^2 * 2 with d the distance over
the reach, times N.L, as the official legacy lighting. Highlights too, as
the official point and spot light shaders: legacy faces with glossiness get
the legacy highlight (below; a projector's at most 1), PBR surfaces GGX
times 3.25 (the official balance with legacy lights, at most 10). Local
lights cast no shadows.

Projectors: a light with a texture (llprimitive.h isLightSpotlight; the
field of view alone doesn't make one) shines it through a frustum
(`render::projectorFor`, as pipeline.cpp setupSpotLight): the near plane is
the prim's -Z face, the field of view spans the prim's Y size there, the
aspect is X over Y, and the far plane is the reach beyond. As
spotLightF.glsl's legacy branch, the image is sharp at the focus distance
and blurs (higher mip levels) towards the far plane, its edges fading over
a margin that grows with the blur; ambiance adds the most blurred image,
faded to a circle, to what faces away from the projector. Projector images
are copied (blits, scaled) into the layers of one 512² mipmapped array,
eight at most at a time, so every surface shader samples them through one
binding; a level of the array stands for the texture's own level of the
same size, so the blur matches the official viewer's whatever the
texture's size. Until its texture loads a projector shines the official
viewer's default image, a flat 0x7f gray (white would flood its whole cone).

Clusters (`render::buildClusters`, `clusters.glsl`): rather than every
pixel looking at every light and probe, each view is split into 16 tiles
across, as many down as keeps them square, and 24 slices in depth, each a
constant factor deeper than the last (from the near plane to 512 m; anything
further is in the last). Per view and frame, the CPU finds the clusters each
light's reach and each probe's sphere (a box probe's bounding sphere) may
touch (the slices its depth range covers; the tiles under its bounding
box's corners on screen, or every tile if it reaches the near plane) and
lays out one array: four uints per cluster (where its lights start, how
many; where its probes start, how many), then the index lists. A fragment
finds its cluster from its pixel and its depth along the view, and loops
over just those. Probe captures don't use them (clusterDepth.w 0: every
light and probe). `--without clusters` turns them off, to compare; at 3000
by 3000 an eye, facing the busy west side of the test region, they cut the
GPU's time about 8 ms a frame, for about 1 ms of CPU.

### PBR materials

Faces can carry glTF 2.0 metallic-roughness materials, as the official
viewer's (llgltfmaterial.cpp, llgltfmateriallist.cpp):

- Extra parameter 0x80 lists (face, material asset id) pairs
  (`Object::materials`). `MaterialManager` (app) fetches the asset through
  ViewerAsset (`?material_id=`), keeps it in the disk cache
  (`CacheKind::Material`) and parses it (`assets::parseMaterialAsset`):
  LLSD {version 1.0/1.1, type "GLTF 2.0", data: glTF JSON}; the first
  material's textures (texture index -> source image -> URI holding the
  asset id), factors, alpha mode and cutoff, double-sidedness and
  KHR_texture_transforms. `llsd::fromJson` reads the JSON into LLSD values.
- Per-face overrides come separately, after the object update:
  `GenericStreamingMessage` method 0x4175, LLSD notation
  (`llsd::fromNotation`) {id: local id, te: [faces], od: [override per
  face]}. Each override sets only the keys it has (tex, bc, ec, mf, rf, am,
  ac, ds, ti); a texture id of all f's means "none". The world keeps them
  by object, also before the object arrives (`World::materialOverrides`);
  a kill or an empty set clears them. The simulator only streams them to
  viewers that asked for the `ModifyMaterialParams` capability (never used
  otherwise), so the session asks for it. Repeats, offsets and rotations
  set in the build tool on a PBR face live here (ti), not in the texture
  entry.
- The simulator streams overrides after full updates only, so the region's
  object cache keeps each object's override text (file version VOC3) and
  puts it back when the object comes from the cache.
- `ObjectView` draws a face with its material (override applied) through
  the PBR pipelines (blended and double-sided variants), ignoring the
  legacy texture, color and repeats; until the material loads, the legacy
  texture stands in. Normal and metallic-roughness maps are sampled as
  data, not sRGB (`TextureManager::getLinear` keeps a second GPU copy). One
  texture transform (the base color's) applies to every map, converted as
  the official texture_transform (flip v, transform, flip back). Missing
  emissive maps mean the factor alone glows, as glTF says.

### Materials

Four material families, each its own pipeline:

| Material | Shaders | Textures |
|---|---|---|
| `BlinnPhongMaterial` (default objects) | object.vert + blinnphong.frag | diffuse (A: opacity or emissive mask), normal (A: glossiness), specular (RGB color); the normal and specular maps with their own transforms |
| `PbrMaterial` (PBR objects) | object.vert + pbr.frag | albedo, packed occlusion/roughness/metallic (RGB), emissive with tint, MikkTSpace normal; roughness and metallic factors |
| `TerrainBlinnPhongMaterial` | terrain.vert + terrain_blinnphong.frag | one 4-layer array of diffuse textures |
| `TerrainPbrMaterial` | terrain.vert + terrain_pbr.frag | 4-layer arrays of albedo, ORM, emissive and normal, with per-layer factors |

A legacy face's color (the texture entry's tint) is sRGB, like its
texture. The official viewer multiplies the two and makes the product
linear (alphaF.glsl, diffuseF.glsl). `BlinnPhongMaterial::diffuseTint` is
therefore made linear too (the shader takes linear), which gives the same
result. Taking the tint as linear made every tinted face too light: a dark
tint of 0.23 came out about three times as bright.

Missing textures fall back to neutral defaults (white, flat normal, no
emission). A base color texture that is coming but not in yet shows as the
official viewer's default image, flat 0x7f gray (`diffuseLoading`,
`albedoLoading`), not white, which colored lights would wash out. Normal maps tilt a unit normal along a unit tangent made
perpendicular to it (`perturbNormal`): the interpolated ones are scaled by
the object's size, which on big prims multiplied the maps' tilt many times.

Legacy materials (the official viewer's LLMaterial, before PBR) give
`BlinnPhongMaterial` faces their normal and specular maps. A texture entry
face names one by id; `LegacyMaterialManager` (app) asks the region's
`RenderMaterials` capability for them (`assets::legacyMaterialRequest`:
POST {"Zipped": zlib(binary LLSD [16-byte ids])}, answered the same way
with [{ID, Material}], `parseLegacyMaterials`), 50 to a request and one
request a second, the official default throttle (a 503 means asked too
fast: those go back in the queue, at most three times). A material has:
normal and specular maps, each with its own repeats, offset and rotation
(fixed point, x 10000; `DrawData::mapUv`, applied to the mesh coordinates
the vertex shaders pass along, `outMeshUv`); a specular color (sRGB) and
glossiness (exponent / 255, times the normal map's alpha); environment intensity (the sharp reflection, by
the specular map's alpha); and what the diffuse alpha means: none (opaque), blend, mask (with a
cutoff), emissive (opaque; alpha is how much shows unlit, the lit color
mixed towards the texture's, as materialF.glsl) or default (the texture's
own).

Meshes don't own buffers: `MeshPool` packs them into large shared vertex and
index buffers (64 MiB and 32 MiB blocks, one set per vertex layout), and a
`Mesh` is a range of a block, drawn with a first index and vertex offset. A
range a mesh releases is reused only four frames later, when no frame in
flight can still draw it. Thousands of separate buffers made frames slow:
SDL's Vulkan backend searches a list of every buffer a command buffer has
used on each bind. `drawScene` sorts opaque draws by pipeline, texture and
block, and only sets state (pipeline, samplers, buffers) when it changes.
New meshes' contents are staged and go up in one copy pass per frame
(`MeshPool::flush`), before the frame that may draw them.

Object draws push no uniforms: each frame, every draw's transform, normal
matrix, texture transform, material values and first joint matrix go into
one storage buffer (`DrawData`, shared by object and fragment shaders), and
a draw finds its entry through an instance-rate vertex attribute holding
the draw's index, which it's drawn with as its first instance. (SDL warns
that built-in instance ids differ between backends; attribute fetch with a
first instance doesn't.) Terrain, a handful of draws, still uses uniforms.

Texture uploads run on worker threads: SDL GPU lets any thread create
textures and record and submit its own command buffer, and a texture is
usable in frames submitted after its upload was. Decoded textures are
flipped (bottom row first) on the decode worker too.

Skinned meshes (`SkinnedVertex`: up to four joint indices and weights per
vertex) use skinned.vert with the Blinn-Phong or PBR fragment shaders. Each
`ObjectDraw` carries its joint matrices; before the scene pass, all of the
frame's are copied into one storage buffer (SDL's uniform pushes are limited
to 4 KiB, too small for rigged mesh's joint counts), and each draw is told
where its matrices start.

Terrain follows the official viewer's behavior (our own implementation; see
`src/world/TerrainLayers.cpp`):
- Each vertex gets a layer value (0..3): height plus Perlin noise, against the
  region's start heights and ranges, and a blend noise value. The noise is
  bit-identical to the viewer's (checked against its noise.cpp); its tables
  come from a fixed seed through the Microsoft C runtime's rand(), i.e. what
  the Windows viewer shows (the official viewer differs per platform here).
  On a region's far edges the value comes from the neighbour, as the viewer
  does.
- Layer weights come from a 2D blend ramp indexed by the layer value and the
  blend noise: the official viewer's `alpha_gradient_2d` image, shipped in
  `assets/` (CC-BY-SA artwork, see below) and embedded in the binary.
- Classic terrain projects textures straight down, 12 m per repeat. PBR
  terrain is sampled triplanar (sharpness 8), 8 m per repeat, and drops layers
  under 10% weight, so slopes don't stretch. Both use global coordinates, so
  textures line up across region borders.

Water, as the official viewer's (lldrawpoolwater.cpp, waterF.glsl,
underWaterF.glsl, waterFogF.glsl):

- Settings: the day cycle's water track (`environment::WaterSettings`, as
  LLSettingsWater: fog color and density, the underwater fog mod, fresnel
  scale and offset, blur, refraction scales above and below, normal scale,
  the two wave directions, the normal map, by default 822ded49), blended
  between keyframes like the skies, the normal maps cross-fading.
- Where: each known region's 256 m square at its RegionHandshake water
  height, and squares around them out to ten regions from the camera's at
  the camera's region's height (`App::addWater`, `Scene::water`).
- The surface (`RendererWater.cpp`, `water.frag`): after the opaque scene,
  which is copied for it to show through (the main pass splits there; the
  occlusion map is made before, so what's under it isn't hidden), before
  what's blended. Three layers of waves from the normal map, drifting along
  the wave directions; what's behind bent by them; the reflection probes
  along the waves' reflected view, mixed in by the official fresnel term;
  the sun's highlight (GGX, its normal flattened with distance). From
  below, what's above bent by the waves' sum, and the water's fog to it.
- Fog: every surface under the water plane (the camera's region's height)
  gets the water's fog instead of the air's haze (`applyAtmosphere` in
  atmosphere.glsl, as applySkyAndWaterFog: from where the view enters the
  water, the official formula with ks from the light's height), with the
  density raised by the fog mod when the camera is under it; then the sky
  below the horizon is the fog's color.

- Water exclusion surfaces ("Hide water"): faces with the official
  viewer's invisiprim textures (IMG_ALPHA_GRAD e97cf410, IMG_ALPHA_GRAD_2D
  38b86f85) aren't drawn anywhere; on a prim neither worn nor rigged they
  are kept as pick-only draws marked `waterExclusion`. Before each view a
  mask is drawn (as mWaterExclusionMask, its own depth): the water squares
  1, then the exclusion faces' parts above the water 0. Where it's 0 the
  water shows what's behind it as it is, and surfaces under the water get
  no water fog from above (waterHazeF.glsl); from below everything's
  fogged. Every scene shader that applies the atmosphere reads it
  (WATER_MASK_BINDING); with no exclusion surfaces in the scene the mask is
  a white texel and the pass is skipped.

Not yet: planar (mirror) reflections, shoreline fade.

Terrain layers come from the grid (`TerrainTextures`). As the official
viewer (LLTerrainMaterials::getMaterialType), each of a region's four detail
assets is asked for both as a texture and as a PBR material: texture terrain
if any loads as a texture, otherwise PBR terrain once all four materials
have, its four arrays (base color, metallic-roughness, emissive, normal)
from the materials' textures and its per-layer factors from theirs.
Procedural layers stand in until then. Arrays are built from the textures'
whole images (CPU copies) at their largest size, at most 1024, and built
again when a bigger copy arrives, so terrain isn't left at the size that
loaded first. PBR layers get the region's material overrides
(`TerrainOverrides`: asked once of each region's ModifyRegion capability,
four override maps as faces' overrides are) and each layer's texture
transform, as lldrawpoolterrain.cpp: the material's base color transform
(KHR_texture_transform with v flipped around it, the same for every map),
its scale over RenderTerrainPBRScale (8 m), from region-local meters (the
region's southwest corner is the origin); the triplanar projections use it
alike, their tangents turned by its inverse so normal maps follow. The PBR
terrain toggle is a shader test: procedural PBR layers everywhere.

Shaders are GLSL in `src/render/shaders`, compiled to SPIR-V by glslang at
build time (the installed `glslangValidator` if there is one, otherwise
glslang is built from source; `-DVIEWER_BUNDLED_GLSLANG=ON` forces that) and
embedded in the binary. Only SPIR-V is built, so SDL uses Vulkan (Linux,
Windows). Metal and D3D12 need the SPIR-V converted (SPIRV-Cross /
shadercross) first.

`--screenshot` saves the scene (without UI) as a PNG and quits, which is how
rendering changes get checked without someone looking at the window.

## Objects

Every region message for the world goes to `WorldThread`, which owns the
session's `World`. On its own thread it decodes each message
(`world::decodeMessage`, no world state needed) into a `WorldUpdate` (object
updates, kills, terrain patches, region handshake, appearance, animations,
or a removed region) and `World::apply`s it, in arrival order. The render
thread reads snapshots: each frame it takes the latest published copy
(`WorldThread::snapshot`), and whenever it has taken one and the world has
changed, the world thread publishes the next. Copies are cheap because
`World` holds objects, terrain, appearances and animations through shared
pointers and copies them on write: a snapshot copies the containers of
pointers, and a change copies only what a snapshot still shares. Snapshots
the render thread is done with go back to the world thread to be released.

`World` decodes ObjectUpdate, ObjectUpdateCompressed,
ImprovedTerseObjectUpdate and KillObject into `world::Object`s keyed by
(region, local id): transform, shape parameters, texture entry (per face:
texture, color, repeats/offset/rotation, fullbright, shiny, glow) and extra
parameters (sculpt 0x30 and mesh 0x60, both an asset id plus a sculpt type
where 5 means mesh; flexible). Terse updates' texture entries carry a 4-byte
length first, unlike the full updates'.

Objects move between updates. Each update records when it arrived
(`CircuitEvent::received`) and the region's time dilation; the simulator
sends a moving object's velocity, acceleration and angular velocity and
then stays quiet while it moves as predicted. `world::extrapolate` predicts
the pose from those (as llviewerobject.cpp: root objects move, anything
spins). `ObjectMotion` (app) applies it each frame for the views: time since
arrival plus half the circuit's round trip, scaled by time dilation;
prediction stops 2 s after a region goes quiet; avatars stay above ground;
nothing moves into a region we don't know. Unlike the official viewer, when
an update disagrees with the prediction the difference fades out over about
0.1 s rather than jumping (over 2 m it jumps). Only moving objects (and ones
updated in the last second) are tracked; still ones cost a comparison.
Measured on our avatar walking: without prediction it stood still in
75-90% of frames and jumped up to 0.85 m; with prediction and fading every
frame moves, with the largest frame-to-frame change in step 2-4 cm.

### Object cache

Simulators mark most objects cacheable and send them as compressed updates
(ObjectUpdateCompressed). The world thread keeps each region's cacheable
objects in a `RegionObjectCache`: the block as received and its CRC, by
local id (as llvocache.cpp; temporary objects, and anything that then gets
a terse or uncompressed update or is killed, are left out). A cache
belongs to one instance of a region (the handshake's CacheID); after a
restart local ids change and the old cache is dropped. It is stored
through `DiskCache` (`CacheKind::Objects`, by region id) when the region
goes away or the session ends.

When a region's `RegionHandshake` arrives the world thread loads its cache,
then asks (`WorldThread::drainRequests`, through the App) for the handshake
reply: flags 0 if we have objects cached (the simulator then sends
`ObjectUpdateCached` probes, local id and CRC), 0x2 if not (full updates
straight away). The session holds the reply until then (3 s at most).
Probes whose CRC matches are applied from the cache; the rest are asked
for with `RequestMultipleObjects` (cache miss type 0 = not cached, 1 =
changed). Mauve, measured: 642 objects in view 8 s after logging in with
no cache, 1324 once cached (1805 hits, 87 misses).

Simulators only send objects around the camera the viewer reports in
`AgentUpdate` (and within its far distance), so the session passes the real
camera (`Session::setCamera`), 128 m far. Formats follow llviewerobject.cpp,
llvovolume.cpp, llprimitive.cpp and llvolumemessage.cpp. Terse updates for
objects we haven't seen are dropped for now (the official viewer asks for the
full update).

`volume::generateVolume` builds a prim's faces from its shape parameters the
way llvolume.cpp does: profile (square, circle, triangles, half circle, with
cuts and hollows of any shape) swept along a path (line, circle, circle2),
with twist, taper, shear, skew, radius offset and revolutions. Face i is
texture entry i, so faces line up with in-world face numbers.

Faces with planar mapping (texture entry media flags, texgen 2) take their
texture coordinates from their position in meters on the face's plane,
twice a meter (llface.cpp planarProjection), instead of the mesh's own:
geometry is shared at unit size, so the object vertex shader does it with
the draw's scale (`ObjectDraw::planar`), before the face's legacy or glTF
transform. Big floors use it to repeat a texture by size. Their textures
are sized for the repeats that makes (twice the largest side).

Flexible prims (extra parameter 0x10: softness, tension, drag, gravity,
wind, user force) bend as the official viewer's LLVolumeImplFlexible bends
them (`FlexibleView`): the prim's length is a chain of 2^softness sections
from its bottom, simulated in scene coordinates each frame (gravity, user
force, tension toward the section below's direction, momentum slowed by
drag, each bend at most atan(2 x section length)), so moving the prim or
its wearer swings it. It's simulated in no more sections than it's drawn
in: 12 x its length over its distance, at most 8 (FLEXIBLE_OBJECT_MAX_SECTIONS
3). The chain is remapped to the drawn sections (cubic between simulated
ones), turned into path points in the prim's frame, and
`volume::generateVolumeAlong` sweeps the profile along them, in meters (the
draw has no scale). New geometry goes up as new meshes (the pool keeps
released ones until the frames using them are done). Wind blows each
section by the wind over it times the prim's wind sensitivity.

As the official viewer's doIdleUpdate, not every prim is updated every
frame: time is counted in flexi frames (60 a second), and a prim covering a
share s of the view is updated every 0.005 / s + 1 of them (at most 32;
`FlexibleView::updatePeriod`), spread over frames by its key; in between it
keeps its meshes. When it is updated but its path hasn't moved (half a
millimeter) since last meshed, as one at rest, it keeps them too.

Wind: regions send it as LayerData of type '7' ('9' for large regions),
two patches coded as terrain patches are (the x velocities, then the y) on
a 16 by 16 grid over the region (`world::decodeWindLayer`). `World` keeps
each region's latest, apart from its terrain since it changes every few
seconds, and `world::windVelocity` looks it up between the grid's points,
times WIND_SCALE_HACK (2), as LLWind::getVelocity. Flexible prims and
particles use it (the official viewer's trees don't: their trunk bend is
never set).

Particles (llParticleSystem): a full update's PSBlock, or a compressed
update's legacy 86 bytes (flag 0x08; 0x400 alone keeps the one there),
decode into `world::ParticleSystem` (`decodeParticleSystem`, as
LLPartSysData: fixed-point fields, the size-prefixed format's optional glow
and blend factors; nothing for a checksum of 0 or a version it doesn't
know). `ObjectIndex` lists the prims that have one, and `ParticleView`
(owned by `ObjectView`) runs them as LLViewerPartSourceScript and
LLViewerPartSim do: each source, every frame (in view or not, within draw
distance; not on HUDs yet), emits a burst every burst rate seconds (dropped,
exploding, or within its angles or cone, turned by the prim and by its
angular velocity), skipping particles too small to ever show and thinning
those too far to be seen before they die; particles move by velocity and
acceleration (wind, steering to the target, following the source, bouncing
off its height, a straight line to the target, as flagged), their color,
scale and glow going from start to end, until their max age. A source
outlives its own max age until its prim next updates (as the official
viewer makes a new one then). At most 4096 live (RenderMaxPartCount): the
official throttles slow bursts near that. Ribbon particles chain to the
one emitted after them (the newest to the source). The renderer
(`RendererParticles.cpp`) sorts this frame's particles far to near once,
uploads their corners, and the vertex shader turns each quad to face each
eye's camera (or along its velocity); they're drawn after the blended
objects, depth tested, blended with their own blend factors (a pipeline
per pair, made when first used), emissive ones unlit and the rest lit
facing the camera (sun and its shadow, ambient, local lights); their glow
adds to the glow mask. Particles without a texture use the official
viewer's default image (pixiesmall, 168e6813-096e-07ea-97ae-fd416826f627,
from the asset server); a soft white dot stands in while it loads.

Texture animation (llSetTextureAnim; `world::TextureAnimation`, from the
TextureAnim field or a compressed update's 0x40 block) runs as the
official viewer's LLViewerTextureAnim: flipbook frames from a grid (whole
frames, or smooth), scrolling, rotating or scaling, looping, reversed or
ping-ponging, on one face or all. `animateTexture` gives the offset, scale
or rotation for how many frames it has run, and those stand in for the
face's own in its texture transform (`ObjectView`); normal and specular
maps follow the animated texture, and PBR faces get it before their
material's transform. An update carrying an animation restarts it, unless
the running one is smooth (that carries on, as LLVOVolume has it). An
animated face's textures are sized for the repeats it shows.

Which prims get looked at: `ObjectIndex` keeps the world's prims by link
set, kept up to date from each world snapshot's changes (`World::changes`:
WorldThread publishes snapshots one after another, each listing the
objects updated, added or removed since the one before; a gap, or a region
going, means rebuilding). Still link sets go into 32 m cells with a sphere
around their members; moving ones (worn, moving or spinning, animated
objects, or under a parent not known yet) are kept apart. Each frame
`ObjectView` looks at a cell, then a link set, only if its sphere is in
view and not hidden (`Scene::occluded`), in a reflection probe's view, or
could cast a shadow into view (only its big prims if it's hidden: as
`Scene::visibility`). Moving link sets are looked at every frame, worn ones
skipped with their avatar when that's hidden; lights and reflection probe
prims always (they matter unseen); automatic probes get every prim's
sphere from the index. The world also keeps its avatars' and animated
objects' keys, so `AvatarView` doesn't look through every object, and it
poses avatars out of sight only every eighth frame.

Per-frame cost: `ObjectView` keeps a record per prim. It holds what the
prim is (on a HUD, rigged mesh; redone when its content revision changes,
which movement alone doesn't) and its face draws once everything in them
has loaded. Those are reused while nothing they depend on has changed: its
content, its link root's (click actions), its level of detail, the mesh
and material managers' revisions, its material override, and, if it's
worn, avatars' appearances (Bake on Mesh). Each frame a kept prim only
gets its transform and shadow-only flag, and its textures are asked for
again through handles recorded when the draws were built
(`TextureManager::record`, `touch`), scaled to this frame's size; prims
whose draws change every frame (texture animation, flexible) aren't kept.
Rigged mesh is kept the same way (its skin too, checked against its
attachment's root prim as well); each frame it gets its owner's
frame, its joint palette (posed joints times the skin's bind matrices) and
its draw order. A rigged mesh's faces share that palette, and the renderer
uploads it once for them all.
`TextureManager` chooses sizes only for the textures used last frame and
checks its budget and retries now and then; `AvatarView` works out each
avatar's root and attachment point frames once a frame, and rigged mesh
its owner's frame and visibility once per owner.

`ObjectView` (app) generates geometry on the JobSystem, cached per (shape,
level of detail), so identical prims share a mesh. Only what the camera can see
is drawn: the App puts the view volume in the scene (`render::Frustum`,
six planes from the view-projection matrix; in VR the head's, wide enough
for both eyes), and the views test against it. Prims are tested with a sphere
around their geometry (its largest coordinate per axis, measured when the
shape is generated, since tori and twisted prims reach past their box),
mesh objects with their box, avatars and rigged mesh with the skeleton's
extent (plus a metre for hair and the like), terrain regions with their
height range. Out of view also means not loaded: textures (and rigged
mesh levels) are only requested for what is drawn. Measured at Ahern:
10921 faces drawn and 3.40 ms per frame before, 6535 and 2.52 ms after,
the same picture. The level of detail follows
the official viewer's screen-size thresholds. Each frame it draws the prims
within 128 m: linked children relative to their root, each face with its
texture (from `TextureManager`), color, texture transform (as llface.cpp's
xform) and fullbright flag. Transparent faces (color alpha, or textures with
blended alpha) go through a blended pipeline after opaque geometry; masked
alpha uses an alpha test. Blended faces are ordered as the official viewer
orders them (lldrawable.cpp, pipeline.cpp, llvoavatar.cpp): each face far
to near by its own center's depth along the view, pulled forward by a
quarter of its size (meshes keep their bounds for this), ties in scene
order; then rigged mesh, wearer by wearer (far first) and, within one, by
attachment order (attachment point, then when attached; later ones first,
so the first attached is drawn on top), each attachment's faces in order.
Distance can't order layers of clothing on one body, so their attachment
order does. As in the official viewer, depth tests pass on equal depth
(layers made of the same geometry show the later one) and blended rigged
mesh writes depth. A face blended because of its tint (alpha under 1) is
only blended, whatever its alpha mode: no mask cutoff, no emissive mask
(llvovolume.cpp). Opaque geometry is drawn wholly opaque first, then
alpha-masked (the official viewer's passes), each in a fixed order: the
same mesh worn twice (a body's hands and a hands attachment) lies exactly
on itself, so ties go by attachment order, the later attached last, so it
shows. Skinned vertices are projected from the skeleton's frame with one
matrix per draw, not from their world position: region-sized coordinates
round to hundredths of a millimetre, as much as the gap between layers
made to fit each other (fur on a body), which would poke through each
other as the view turns. Skinned normals use the inverse transpose of the
skinning matrix (the official viewer applies the bind shape's to normals):
bind shapes stretch meshes, and stretched normals light seams wrongly.
The legacy highlight (`legacySpecular`, for the sun and local lights) is
the official specular table worked out in the shader: normalized
Blinn-Phong of exponent glossiness² x 368 (RenderSpecularExponent), with
the official Fresnel ((1 - V.H)^5 x 0.4 + 0.5) and geometry terms, over
N.H x N.L, times min(6 N.L, 1). Legacy faces have no specular highlight
unless "shiny": then specular
color, glossiness and environment intensity are 0.25, 0.5 or 0.75 (low,
medium, high, as llvovolume.cpp), so they shine and reflect the probe. A
legacy material's own specular values replace those only when it has a
specular map, as in the official viewer.

Mesh objects (sculpt type 5) take their geometry from `MeshManager` (app):
for each mesh it fetches the header with a Range request for the first 4 KiB
of `<ViewerAsset>/?mesh_id=<uuid>` (`Accept: application/vnd.ll.mesh`), then
the level of detail the object needs (or the nearest the asset has) as
another Range request; `assets::decodeMeshLod` inflates it (zlib) and decodes
the binary LLSD faces (16-bit quantized positions, normals, texture
coordinates, triangle lists) on a worker. While a level loads, the nearest
ready one is drawn. Downloaded parts go into the disk cache as received
(header bytes, the skin block and each level's compressed block), so later
sessions read them from disk.

Linden trees and grass are object types of their own (`TreeView`, app),
built as the official viewer's llvotree.cpp and llvograss.cpp do, from the
species tables of its trees.xml and grass.xml (LGPL-2.1, like this
project); the species is the first byte of the object's data (or the
tree-species byte of compressed updates). A tree is recursive: a tapered,
noise-bumped cylinder per trunk or branch segment, `branches` branches per
segment turned by the twist and bent down by the droop, the trunk going on
turned 70.5 degrees for `trunk_depth` segments, and two crossed two-sided
quads of leaves at the ends; bark is the left half of the species'
texture, leaves the right. The mesh depends only on species and level of
detail (cylinder slices 10, 5, 4, 3 by how many degrees the tree spans), so
trees share them (at most two built per frame); each is placed by its
position (0.1 m into the ground), rotation turned a quarter, and |scale| x
0.05. Grass is 32 two-sided blades scattered with a Gaussian spread over
the object's area (one table for every grass object, seeded the same each
run), each standing on the terrain under it, so each grass object has its
own mesh (rebuilt when it or the terrain changes); drawn within 64 m. Both
are alpha-masked Blinn-Phong draws: culled, sorted and shadowed like any
object.

Rigged mesh (a header `skin` block) is drawn skinned to the avatar wearing
it; see Avatars. Not drawn yet: flexible prims (drawn straight).

Sculpted prims (sculpt types 1-4) take their shape from a sculpt map, a
texture whose pixels' RGB are positions (0..255 to -0.5..0.5), as
llvolume.cpp LLVolume::sculpt (`volume::generateSculpt`): the path and
profile (circles) get as many points as the map's aspect and the level of
detail allow (sides of 6, 8, 16 or 32, at most a point per four pixels),
each mesh point is the pixel there instead of the sweep, the map's edges
join by type (sphere: poles pinched, sides wrapped; torus: both wrapped;
cylinder: sides; plane: none), invert and mirror flip it, and the faces
are built as any prim's, the seams' normals joined the same way. A map
with too little or too much surface area (flat, or noise) makes a sphere
instead. `ObjectView` reads the map through `TextureManager::data` (whole,
on the CPU) and keys the geometry by map and type too; a sculpted prim is
drawn once its map has loaded.

## Avatars

`avatar::BaseAvatar` loads the official viewer's character files, shipped in
`assets/character` (CC-BY-SA, see `assets/LICENSE-artwork.txt`) and copied
next to the binary: the skeleton (`avatar_skeleton.xml`; joint rotations are
XYZ Euler angles), attachment points (`avatar_lad.xml`; HUD points are
skipped, `mRoot` is the avatar root) and the `.llm` body meshes (head, upper
and lower body, eyelashes, hair, skirt, eyes). Weighted meshes are in
skeleton space, made with the pelvis at `pelvisBind`; the eyes are in their
eye joint's space.

Shape (`avatar/Shape.hpp`): `avatar_lad.xml`'s `<param>`s are visual
parameters. Morph parameters add `.llm` morph targets (vertex, normal and
texture coordinate offsets) to their mesh; skeletal ones add to joint scales
and positions; driver parameters set the parameters they drive (a ramp up,
plateau and ramp down over the driver's range). `ParamWeights` turns the
AvatarAppearance message's VisualParam bytes into weights: one byte per
parameter of group 0 or 3, in id order, mapped onto the parameter's range.
Parameters for the other sex (the "male" parameter, 80) stay at their
default. Regions don't always send AvatarAppearance (never our own after
login), so an avatar still without one after 3 s is asked for with a
GenericMessage "avatartexturesrequest" (`Session::requestAvatarAppearance`),
then every 30 s until it comes, as the official viewer does for avatars left
with default parameters. `shapeAvatar` deforms the skeleton and morphs the meshes;
`poseJoints` poses the skeleton (rest pose or animated) and `skinPalette`
gives each mesh the matrices its vertices are skinned with: each vertex weight is an index into the mesh's skin joint list
(llavatarjointmesh.cpp's order: skin joints in skeleton order, each preceded
by its nearest base ancestor) plus a blend towards the next one, and each
joint maps from where the mesh was made (the sum of its base ancestors'
pivots) to where it is now. A child joint's offset is scaled by its parent's
scale, but scale doesn't otherwise carry down (llxform.cpp). Body height and
pelvis-to-foot follow computeBodySize.

`AvatarView` (app) morphs each avatar's body when its appearance arrives or
changes (on the CPU, uploaded once), and skins it on the GPU every frame to
its animated skeleton; until the appearance arrives it uses the default
shape. As in llvoavatar.cpp, the root (pelvis) is
`pelvisToFoot` above the feet, and the feet are half the body height below
the object's position, raised by the hover offset (the message's
AppearanceHover block). Each part is
textured with its server-side bake from the avatar's texture entry (8 head,
9 upper, 10 lower, 11 eyes, 19 skirt, 20 hair), taken from AvatarAppearance
when we have it, otherwise from the object. Bakes are fetched from
`<agent_appearance_service>texture/<avatar>/<bake name>/<texture>` through
`TextureManager` and cached like any other texture; they have a fifth J2K
component, which is ignored. A part isn't drawn while its bake is the
default or invisible texture, or still loading.

Attachments: an object whose parent is an avatar sits at the attachment
point given by its state byte (`attachmentPointFromState`); `ObjectView`
asks `AvatarView` for the point's frame on the posed skeleton.

Animation: the simulator sends AvatarAnimation with the full list of
animations an avatar plays (standing is one too), each with a sequence
number. `AnimationManager` (app) loads them (disk cache, else
`<ViewerAsset>/?animatn_id=`),
parses them (`avatar::parseAnimation`, llkeyframemotion.cpp's format:
quantized rotation and position keys per joint, priorities, loop points,
ease times) and binds joint names (and aliases) to the skeleton.
`avatar::Animator` plays them: looping between the loop points, easing in
and out with a cubic step, animations that don't loop stopping early enough
to ease out by their end. Per joint, the highest priority (then latest)
animation comes first and lower ones only fill what's left of a total weight
of one (llpose.cpp's LLJointStateBlender). Animated rotations replace the
joint's; animated positions replace the shaped ones. Attachments follow the
animated skeleton.

Rigged mesh: `MeshManager` also loads a mesh's skin block (`.skin` in the
disk cache): joint names, inverse bind matrices, the bind shape matrix and
optional joint positions (`alt_inverse_bind_matrix`). Levels of detail carry
per-vertex weights (up to four joint/weight pairs, llvolume.cpp's format) and
upload as skinned meshes. `ObjectView` skins a worn rigged mesh to its
avatar's posed joints: joint * inverse bind * bind shape, with joints the
skeleton doesn't have following the pelvis. Fitted mesh is weighted to the
skeleton's collision volumes, which are joints too: they inherit their bone's
skeletal changes in proportion to their own scale, and morph parameters'
`volume_morph`s resize them. Joint positions a rigged attachment asks for
replace the shape's (the mesh with the highest id wins a conflict, as the
official viewer picks) unless an animation moves that joint. That includes
the pelvis, which otherwise sits at the root. A mesh's pelvis offset raises
the whole avatar (the render position, so attachments too); again the
highest mesh id's wins. Both only come from meshes that carry joint
positions, as in llvoavatar.cpp addAttachmentOverridesForObject. Bake on Mesh: attachment faces showing one of the
`IMG_USE_BAKED_*` placeholders show the wearer's bake instead (head, upper,
lower, eyes, skirt, hair, left arm, left leg, aux 1-3). Any of a worn face's
textures may be one: its diffuse texture, its legacy material's normal and
specular maps, and its PBR material's base color, normal,
metallic-roughness and emissive maps (`ObjectView::faceTexture`; normal and
metallic-roughness bakes are sampled as plain data), so one bake can be the
color and another the glow or roughness. Not on HUDs or animated objects
(as getBakedTextureForMagicId). A diffuse bake is cut out at 0.5 where its
alpha is, like the body, unless the face's legacy material sets its alpha
mode. The default body
parts using those bakes are hidden (head and eyelashes, upper, lower, eyes,
skirt, hair), as llvoavatar.cpp updateMeshVisibility does. A worn rigged
mesh's level of detail comes from its avatar's size (the diagonal of its
posed joints' box, used as a radius, as LLVOVolume::calcLOD does), not the
object's scale.

Animated objects (animesh: the extended mesh parameter's flag on a link
set's root) get a skeleton of their own, as llcontrolavatar.cpp does: the
default shape with no body, its root where the root prim is (turned by the
root mesh's bind shape rotation unless worn), posed by the animations
ObjectAnimation lists for the object. Every rigged prim of the link set is
skinned to it, also when the object is worn. The simulator only sends
ObjectAnimation to viewers that ask for the ObjectAnimation capability, so
the seed request names it although nothing fetches it. Not yet: the
official viewer's size and position limits on where animations may move
them (AnimatedObjectsMaxLegalOffset and MaxLegalSize).

Not yet: IK constraints, the official viewer's
procedural motions (head and eye movement, breathing, hand poses), clothing
morph masks and the physics-driven morphs (breast, belly and butt physics).

### Inventory

`inventory::Model` (src/inventory) holds the agent's inventory as far as
it has been fetched: folders and items by id, links included (`linkedId`:
an item's or a folder's), children by folder. `inventory::Ais` is the
AISv3 client (the region's `InventoryAPIv3` capability, as the official
viewer's AISAPI): GETs of a folder's children to a depth, a folder's links,
an item, by id or by a system folder's alias (`current` for the Current
Outfit Folder). Replies are LLSD whose contents come in `_embedded` maps
keyed by id (`categories`, `items`, `links`, and a link's target as
`item` or `category`); `Model::merge` takes in all of it, however deep, and
each request's callback (on the main thread, from `Ais::update`) gets the
ids it brought. Both are owned by the App and emptied when a session ends.
Writes (links, moves, removal) are to come with outfit editing.

### Wearing the whole outfit

The simulator brings back one attachment a point after login; the rest
of the outfit (a head and hair both on the skull, rigged clothes on a
hand) is worn by the viewer, as the official one does from the Current
Outfit Folder (`OutfitAttachments`). Through AISv3 that's one request,
`category/current/links`, whose links carry their targets (any not
embedded are fetched alone); without it (other grids), the older
capabilities: the folder of type 46 among the root's
(FetchInventoryDescendents2), its links, then their items
(FetchInventory2). The objects not worn (no attachment of ours with that
`AttachItemID`) are attached once our avatar has been in 3 seconds and no
attachment has arrived for 2 (the simulator's own restores done), five a
frame (RezMultipleAttachmentsFromInv: point 0, where each was last, with
ATTACHMENT_ADD so they share points), then once more 30 seconds later for
any still missing. An item attached twice is logged.

### Viewer-made motion (IK)

Our own avatar gets motion on top of its animation that only this viewer
sees directly (nothing goes to the simulator; others would see it only through
something like puppetry): `avatar::applyIk` (`avatar/Ik.hpp`), given goals in
avatar space, after the animation and rigged mesh's joint overrides and
before the joints become matrices (so attachments and rigged mesh follow).

- Desktop (`App::updateIk`): the head looks where the pointer is (what it's
  on, or 10 m along its ray), following with the official look-at's lag
  (0.15 s half-life) and easing out when the pointer leaves the view. As
  llheadrotmotion.cpp LLHeadRotMotion: the head turned upright towards the
  point, at most 72 degrees from the body, the torso taking 35% and the neck
  and head half each of the rest; the eyes turn the rest of the way, at
  most 27 degrees (LLEyeMotion). Composed with what's animated rather than
  replacing it. While pressing on something in the world (touching), the
  left hand reaches for the point pressed (the arm the official viewer
  points with, lleditingmotion.cpp) (easing in and out, 0.08 s
  half-life), following the drag: the pointer's point on what was pressed,
  or off it, along the pointer's ray as far as the press was; out of
  reach, the arm points at it.
- Others see it as the official viewer's look-at and point-at
  (`ViewerEffects`, ViewerEffect messages, `Session::sendAttention`; 57
  bytes each: our id, the target's, a position as three little-endian
  doubles, the target type): the cursor as a free look (type 3, 2 s) at
  ourselves, its direction 2 m long from our head (no further, like the
  official limit); with no cursor in the view, an idle look (type 1, 3 s)
  2 m ahead the way the camera looks, in the avatar's own frame (as
  llagentcamera.cpp updateLookAt); a press as a grab point-at and a select
  look-at at the pressed prim, the point in its own frame, lasting until
  cleared on release. A clear goes out as the official one does: type
  "none", no target or position, lasting no time (as the clear type itself,
  receivers would turn to the global origin). Each is one effect (its own id) updated when its target changes
  or its point moves 5 cm (at most 4 a second), and sent again before it
  runs out.
- Others' look-at and point-at (their ViewerEffects) move their heads and
  arms the same way: each holds until that avatar sends another (none
  clears it), as in the official viewer, whose receivers don't time them
  out. Their targets resolve as calcTargetPosition: at an avatar, its head
  plus the offset (in world axes for free looks and mouselook, turned by
  the avatar otherwise); at a prim, its position plus the offset turned by
  it; with no target, a global position. Eased as ours; a point-at moves
  the left arm.
- VR (`App::updateVrIk`): goals are chains (`IkGoals::chains`: a bone, how
  many joints up to it FABRIK moves, where it goes and its rotation): the
  head (a chain of 4: torso, chest, neck, head) placed so the avatar's
  eyes are on the headset's and turned as it is, each wrist (3: shoulder,
  elbow, wrist) 8 cm behind its controller's grip and turned from the
  grip's axes. The view is anchored on the animated pelvis, at the eyes'
  unanimated height and offset ahead along the headset's heading, so the
  avatar's animation and IK don't move it. The body turns once the head
  is 45 degrees round from it, and faces the head to walk (left stick);
  the right stick turns 30 degrees a flick and recenters when clicked.
- Reaching uses FABRIK (`avatar::fabrik`): the chain's joints moved
  backward from the target and forward from the root, keeping bone lengths,
  until the end is within 0.1 mm; a pole keeps the elbows bending down,
  back and out. The new positions become rotations joint by joint (each
  turned the least from the animated pose), blended with the animation by
  the goal's weight; a goal rotation sets the end joint's.

### First person

First person is the official viewer's mouselook (`App::setFirstPerson`). M
toggles it; zooming all the way in enters it and zooming out leaves it;
`--first-person` starts in it.
- The camera sits between our eyes (`AvatarView::eyePosition`, taken
  before the head shrinks), 5 cm near plane. The mouse is captured and turns
  it with no button held, and the body faces where it looks.
- W/S (and up/down) walk, A/D (and left/right) step sideways, E/Q fly up and
  down. The MOUSELOOK control flag is set throughout, and the left button
  sends ML_LBUTTON_DOWN and ML_LBUTTON_UP (as LLToolCompGun) for scripts
  that take controls, instead of touching.
- The head is hidden by scaling `mHead` and every joint below it to
  nothing, which shrinks the body's head, Bento face bones and rigged mesh
  on them alike. Unrigged attachments on points not visible in first
  person (`avatar_lad.xml` visible_in_first_person: skull, eyes, ears,
  nose, mouth, chin, jaw, tongue) are drawn only into shadow maps (not
  picked). Rigged mesh is drawn wherever it's worn: what's weighted to the
  head shrinks with it, the rest stays (a whole body worn on the skull
  still shows its body).
- The shadow keeps the head: the pose is also skinned unshrunk
  (`AvatarView::shadowJoints`), and the body and rigged mesh are drawn
  twice, shrunk with `ObjectDraw::castsShadow` off, and whole as
  `shadowOnly`.
- Clicks and the look-at go through the middle of the view, marked by a
  small dot (`App::addCenterDot`: a pointer line of no length, all dot,
  white in a dark ring; hidden while the cursor is free). A dot, not a
  crosshair: it's a social place, not a shooter. Others are told as a
  mouselook look-at (type 9) along the camera's direction.
