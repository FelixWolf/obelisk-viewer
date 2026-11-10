# One library per module; dependencies between modules are explicit and
# one-directional. app/ is the only place that sees everything.
#
#   core  <-  image  <-  assets        (J2K decode, KTX2, texture cache)
#   core  <-  image  <-  render  <-  ui
#   app sees everything
#
# Planned: llsd, protocol, net, world, session, xr, voice.

add_subdirectory(core)
add_subdirectory(image)
add_subdirectory(assets)
add_subdirectory(render)
add_subdirectory(ui)
add_subdirectory(app)
