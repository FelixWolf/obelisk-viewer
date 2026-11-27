# Roadmap

What's still needed for the viewer to be complete. This isn't meant to
become a full-featured viewer like the official one: most editing and
building tools are out of scope. Desktop and VR should work alike (the
same interactions in both). Items marked ★ were asked for directly.

As of November 2026 the viewer has: login, movement and camera,
teleports and region crossings, avatars (appearance, animation, attachments,
rigged mesh), prims, sculpts and mesh, legacy and PBR materials, local
lights and projectors, sun shadows, reflection probes, the windlight sky
with clouds, sun and moon, terrain (texture and PBR), water, trees and
grass, particles, and HUDs (over the screen or on a panel), picking and
touching with a hover outline and touch text, floating text, name tags,
chat bubbles, texture animation, glow, flexible prims, IK for our
avatar's head and hand (desktop) and others' look-at and point-at, and
first person (desktop). It has no chat window, IM, sitting or
sound yet.

## Avatars and animation

- Sitting and standing (sit targets, the sit response, the seated camera)
- Complexity limits and "jelly dolls" for crowded places

## Interaction

- Click actions besides touch and sit: pay, buy, open, play media (touch,
  sit, the hover outline with touch text, clicking HUDs, and the radial
  menu's touch and sit, the click's first, are done)
- Script dialogs: `llDialog`, `llTextBox`, permission requests,
  `llLoadURL`, `llMapDestination`
- ★ Radial menu, the same on desktop and in VR
- Teleport UI: map, search, landmarks, home; maybe a minimap

## Communication

- Local chat window: the log and a line to type in (chat comes in, shows in
  bubbles, and can be sent; there's no window yet)
- Instant messages, group chat, friends list, online and offline notices
- ★ VR keyboard
- Voice (WebRTC): nearby done; group, ad-hoc and private calls later

## Inventory and appearance

- Inventory browser (AISv3)
- Wearing, attaching and detaching, outfits (the Current Outfit Folder)
- Inventory offers and other notifications

## Audio

- Object sounds (triggered and attached), preloading, ambient wind
- Parcel music streams, UI sounds

## UI

- ★ Better UI. Leaning towards our own retained UI system for anything
  users see, with ImGui for debug UI only (not decided yet). Drawing it
  into images, as HUDs are, would let the same UI be a screen overlay on
  desktop and panels in VR.
- ★ Login screen: an island with trees, and Second Life grid stats
- Settings in a config file instead of hard-coded parameters (done:
  settings.llsd beside accounts.llsd, defined in ViewerSettings.cpp, on the
  main menu's Settings tab), and key bindings

## VR

- OpenXR: stereo rendering (done: the head tracked from the
  avatar's eyes), controllers, locomotion (smooth and teleport), comfort
  options
- Built from the items above: radial menu, VR keyboard, IK, hover
  outline, HUD panels

## Platform

- Shaders beyond SPIR-V (Metal, D3D12), if macOS or Windows-native
  rendering matters
- Crash logging

## Suggested order

1. The config file and the UI foundation: every user-facing item depends
   on them.
2. Touch and click, hover outlines, script dialogs and chat: what makes
   the viewer usable day to day.
3. First person and IK, then OpenXR, so desktop and VR grow together.
4. The rendering items (glow, flexiprims, particles, texture animation)
   fit in anywhere: they're mostly self-contained.
