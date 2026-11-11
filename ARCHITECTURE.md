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
   - world: decodes the regions' messages into `WorldUpdate`s (`WorldThread`);
     the main thread owns `World` and applies them, which is cheap
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

## Texture pipeline

1. `TextureCache::request()` queues a job on the `JobSystem`.
2. The worker reads `<cache>/textures/<xx>/<uuid>.ktx2` if it exists. Otherwise it
   calls the request's `J2kSource` for the bytes, decodes them, expands to RGBA8,
   classifies alpha, builds mips, and writes the KTX2 atomically (temp file + rename).
3. The result goes into a `MessageQueue`. The main thread drains it and uploads
   to the GPU in batches capped per frame.

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
and the world use separate instances on the same directory.

KTX2 entries are RGBA8 with zstd supercompression, plus `viewer.alphaMode` and
`viewer.discardLevel` metadata. The cache root defaults to
`$XDG_CACHE_HOME/viewer` (or `~/.cache/viewer`); override it with `--cache-dir`.

## Building

```
git submodule update --init                 # the message template
cmake --preset release && cmake --build --preset release
ctest --test-dir build/release              # or ./build/release/bin/viewer_tests
./build/release/bin/viewer [--cache-dir PATH] [--import-textures DIR] [--grid LOGIN_URI]
    [--login-file PATH] [--test-scene] [--pbr-terrain]
    [--screenshot OUT.png [--screenshot-delay S]] [--camera "X Y Z DISTANCE YAW PITCH"]
./build/release/bin/logintest --login-file ~/.config/viewer/test-login.txt [--seconds N]
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

Every region message for the world goes through `WorldThread`, which decodes
it on its own thread (`world::decodeMessage`: no world state needed) into a
`WorldUpdate`: object updates, kills, terrain patches, region handshake,
appearance, animations, or a removed region. The main thread drains these
once per frame, in order, and `World::apply`s them.

`World` decodes ObjectUpdate, ObjectUpdateCompressed,
ImprovedTerseObjectUpdate and KillObject into `world::Object`s keyed by
(region, local id): transform, shape parameters, texture entry (per face:
texture, color, repeats/offset/rotation, fullbright, shiny, glow) and extra
parameters (sculpt 0x30 and mesh 0x60, both an asset id plus a sculpt type
where 5 means mesh; flexible). Terse updates' texture entries carry a 4-byte
length first, unlike the full updates'.

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
level of detail), so identical prims share a mesh. The level of detail follows
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
ready one is drawn. Downloaded parts are cached on disk as received (header
bytes and each level's compressed block, `<cache>/meshes/<xx>/<uuid>.header`
and `.lod<N>`, written atomically), so later sessions read them from disk.

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
number. `AnimationManager` (app) loads them (disk cache
`<cache>/animations/<xx>/<uuid>.anim`, else `<ViewerAsset>/?animatn_id=`),
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
replace the shape's (the mesh with the lowest id wins a conflict, as the
official viewer picks) unless an animation moves that joint; the pelvis
offset isn't applied yet. Bake on Mesh: attachment faces showing one of the
`IMG_USE_BAKED_*` placeholders show the wearer's bake instead (head, upper,
lower, eyes, skirt, hair, left arm, left leg, aux 1-3).

Not yet: animated objects (animesh), IK constraints, the official viewer's
procedural motions (head and eye movement, breathing, hand poses), clothing
morph masks and the physics-driven morphs (breast, belly and butt physics).
