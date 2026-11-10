# One library per module; dependencies between modules are explicit and
# one-directional. app/ is the only place that sees everything.
#
#   core  <-  image  <-  assets        (J2K decode, KTX2, texture cache)
#   core  <-  image  <-  render  <-  ui
#   core  <-  net  <-  protocol         (generated messages, UDP circuits)
#   core  <-  llsd, net, protocol  <-  session   (login, regions, caps, event queues)
#   app sees everything
#
# Planned: world, xr, voice.

add_subdirectory(core)
add_subdirectory(image)
add_subdirectory(assets)
add_subdirectory(llsd)
add_subdirectory(net)
add_subdirectory(protocol)
add_subdirectory(session)
add_subdirectory(render)
add_subdirectory(ui)
add_subdirectory(app)
