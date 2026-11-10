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
src/core     logging, MessageQueue (header-only)                -> viewer::core
src/image    image types; J2kDecoder interface (OpenJPEG)      -> viewer::image
src/render   SDL GPU device and per-frame command buffer   -> viewer::render
src/ui       ImGui context + SDL3/SDL GPU backends         -> viewer::ui
src/app      App (owns everything), main()
tools/j2kbench   benchmarks J2kDecoder implementations
cmake/           CPM bootstrap + Dependencies.cmake (all versions pinned)
```

## Building

```
cmake --preset release && cmake --build --preset release
./build/release/bin/viewer
./build/release/bin/j2kbench --reduce 0,2 <dir-of-j2k-files>
```

Package sources are cached in `build/_cpm-cache` unless the `CPM_SOURCE_CACHE`
environment variable is set.

## Swappable backends

Third-party codecs sit behind interfaces in our own modules, so they can be
swapped later. For JPEG 2000 that's `viewer::image::J2kDecoder`, with
`createJ2kDecoder()` defined by the backend's .cpp (currently
`src/image/OpenJpegDecoder.cpp`).
