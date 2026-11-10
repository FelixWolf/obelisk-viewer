# One library per module; dependencies between modules are explicit and
# one-directional. app/ is the only place that sees everything.
#
#   core  <-  render  <-  ui  <-  app
#   core  <-  image   (JPEG 2000 decode; later KTX2 read/write)
#
# Planned: llsd, protocol, net, world, assets, session, xr, voice.

add_subdirectory(core)
add_subdirectory(image)
add_subdirectory(render)
add_subdirectory(ui)
add_subdirectory(app)
