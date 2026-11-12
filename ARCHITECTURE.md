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

Sizes and memory: callers say how many texels across each use needs
(`texelsFor`: the object's size over its distance, times the focal length
in pixels, times the face's repeats; the least for draws that only cast
shadows), and a texture is loaded at the most any use asked for this frame,
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
    [--camera "X Y Z DISTANCE YAW PITCH"] [--texture-memory MIB]
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
line. Not drawn yet: sun and moon textures, moon phases, water.

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
the reach, times N.L, as the official legacy lighting; PBR surfaces take it
on their diffuse color only for now. Local lights cast no shadows.

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
texture's size. Until its texture loads a projector shines white.

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
| `BlinnPhongMaterial` (default objects) | object.vert + blinnphong.frag | diffuse (A: opacity), normal (A: glossiness), specular (RGB color) |
| `PbrMaterial` (PBR objects) | object.vert + pbr.frag | albedo, packed occlusion/roughness/metallic (RGB), emissive with tint, MikkTSpace normal; roughness and metallic factors |
| `TerrainBlinnPhongMaterial` | terrain.vert + terrain_blinnphong.frag | one 4-layer array of diffuse textures |
| `TerrainPbrMaterial` | terrain.vert + terrain_pbr.frag | 4-layer arrays of albedo, ORM, emissive and normal, with per-layer factors |

Missing textures fall back to neutral defaults (white, flat normal, no
emission). Normal maps tilt a unit normal along a unit tangent made
perpendicular to it (`perturbNormal`): the interpolated ones are scaled by
the object's size, which on big prims multiplied the maps' tilt many times.

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

Faces with planar mapping (texture entry media flags, texgen 2) take their
texture coordinates from their position in meters on the face's plane,
twice a meter (llface.cpp planarProjection), instead of the mesh's own:
geometry is shared at unit size, so the object vertex shader does it with
the draw's scale (`ObjectDraw::planar`), before the face's legacy or glTF
transform. Big floors use it to repeat a texture by size. Their textures
are sized for the repeats that makes (twice the largest side).

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
mesh writes depth. Skinned normals use the inverse transpose of the
skinning matrix (the official viewer applies the bind shape's to normals):
bind shapes stretch meshes, and stretched normals light seams wrongly.
Legacy faces have no specular highlight unless "shiny" (which in the
official viewer reflects the environment; a soft sheen stands in until we
have environment reflections).

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
it; see Avatars. Not drawn yet: sculpted prims, flexible prims (drawn straight), planar
texture mapping.

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
