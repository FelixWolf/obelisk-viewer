# One library per module; dependencies between modules are explicit and
# one-directional. app/ is the only place that sees everything.
#
#   core, image, llsd, volume  <-  assets   (texture cache, mesh assets)
#   core  <-  image  <-  render  <-  ui
#   text  <-  render                    (fonts, glyph atlas, text layout)
#   core  <-  net  <-  protocol         (generated messages, UDP circuits)
#   core  <-  llsd, net, protocol  <-  session   (login, regions, caps, event queues)
#   volume                             (prim geometry from shape parameters)
#   core  <-  protocol, volume, assets  <-  world   (terrain, objects)
#   core, llsd  <-  environment         (sky settings, day cycles, lighting)
#   app sees everything
#
# Planned: xr, voice.

add_subdirectory(core)
add_subdirectory(image)
add_subdirectory(llsd)
add_subdirectory(net)
add_subdirectory(protocol)
add_subdirectory(session)
add_subdirectory(inventory)
add_subdirectory(volume)
add_subdirectory(assets)
add_subdirectory(audio)
add_subdirectory(world)
add_subdirectory(environment)
add_subdirectory(avatar)
add_subdirectory(text)
add_subdirectory(render)
add_subdirectory(ui)
add_subdirectory(gui)
add_subdirectory(xr)
add_subdirectory(app)
