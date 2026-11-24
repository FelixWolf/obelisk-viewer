#pragma once

namespace viewer::xr
{

// Whether SteamVR is installed and has a headset to use. Asks SteamVR's
// client library (as OpenVR's VR_IsHmdPresent), which doesn't start SteamVR;
// making an OpenXR instance to ask would.
bool steamVrHeadsetPresent();

} // namespace viewer::xr
