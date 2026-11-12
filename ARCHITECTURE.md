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
   - worker pool: J2K decode, KTX encode/write, mesh decode, disk I/O
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
2. The worker reads the texture's KTX2 from the disk cache if it's there.
   Otherwise it calls the request's `J2kSource` for the bytes, decodes them,
   expands to RGBA8, classifies alpha, builds mips, and stores the KTX2.
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
`TerrainTextures` builds each region's 4-layer terrain array once all four
textures are in (region textures from RegionHandshake, the official
defaults otherwise), scaled to a common size of at most 1024.

Each `TextureCache` instance has one results queue, so the texture browser
and the world use separate instances on the same disk cache.

KTX2 entries are RGBA8 with zstd supercompression, plus `viewer.alphaMode` and
`viewer.discardLevel` metadata. The cache root defaults to
`$XDG_CACHE_HOME/viewer` (or `~/.cache/viewer`); override it with `--cache-dir`.

## Building

```
git submodule update --init                 # the message template
cmake --preset release && cmake --build --preset release
ctest --test-dir build/release              # or ./build/release/bin/viewer_tests
./build/release/bin/viewer [--cache-dir PATH] [--import-textures DIR] [--grid LOGIN_URI]
    [--login-file PATH] [--start last|home|Region/x/y/z|secondlife://...] [--test-scene]
    [--pbr-terrain] [--no-vsync] [--screenshot OUT.png [--screenshot-delay S]]
    [--camera "X Y Z DISTANCE YAW PITCH"]
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
2. When its `RegionHandshake` arrives we reply, send throttles, FOV, window
   size and the first `AgentUpdate` (then one per second). Once the main
   region's capabilities have arrived as well, the session is `Connected`.
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
buffer, then a tonemap pass (exposure, ACES fit, sRGB encoding) writes the
swapchain image, and ImGui draws on top.

Four material families, each its own pipeline:

| Material | Shaders | Textures |
|---|---|---|
| `BlinnPhongMaterial` (default objects) | object.vert + blinnphong.frag | diffuse (A: opacity), normal (A: glossiness), specular (RGB color) |
| `PbrMaterial` (PBR objects) | object.vert + pbr.frag | albedo, packed occlusion/roughness/metallic (RGB), emissive with tint, MikkTSpace normal; roughness and metallic factors |
| `TerrainBlinnPhongMaterial` | terrain.vert + terrain_blinnphong.frag | one 4-layer array of diffuse textures |
| `TerrainPbrMaterial` | terrain.vert + terrain_pbr.frag | 4-layer arrays of albedo, ORM, emissive and normal, with per-layer factors |

Missing textures fall back to neutral defaults (white, flat normal, no
emission).

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

Water: each region gets a 256 m square at its RegionHandshake water height,
drawn as blended Blinn-Phong with a procedural ripple normal map that drifts
over time. No reflections, refraction or water outside known regions yet.

Terrain textures come from the grid (see the texture pipeline); the
procedural ones stand in until they're loaded. PBR terrain materials from the
grid aren't supported yet, so the PBR terrain toggle uses procedural ones.

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

`ObjectView` (app) generates geometry on the JobSystem, cached per (shape,
level of detail), so identical prims share a mesh. Only what the camera can see
is drawn: the App puts the view volume in the scene (`render::Frustum`,
six planes from the view-projection matrix; for VR it will have to hold
both eyes), and the views test against it. Prims are tested with a sphere
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
blended alpha) go through a blended pipeline after opaque geometry, sorted
back to front; masked alpha uses an alpha test.

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

Rigged mesh (a header `skin` block) is drawn skinned to the avatar wearing
it; see Avatars. Not drawn yet: sculpted prims, animated objects (rigged mesh
not worn), trees, grass, flexible prims (drawn straight), planar texture
mapping, materials.

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
default. `shapeAvatar` deforms the skeleton and morphs the meshes;
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
lower, eyes, skirt, hair, left arm, left leg, aux 1-3), and the default body
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
