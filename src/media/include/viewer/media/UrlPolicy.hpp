#pragma once

// What a media tab may go to and load, the media host's own rule (the
// viewer decides what plays; this is under it, whatever the viewer says).
// Pages over http and https only; nothing on this computer or the local
// network, by name or address, so a prim can't reach a router's page or a
// service on localhost through someone's viewer. URLs as Chromium gives
// them: canonical (lowercase scheme and host, IPv4 as four decimals, IPv6
// in brackets, compressed).
//
// (A name that resolves to a private address isn't caught here.)

#include <string_view>

namespace viewer::media
{

// A page a tab may show (its main frame): http, https (data: with
// `allowData`, for tests), about:blank.
bool navigable(std::string_view url, bool allowData);
// Anything a page may load (images, scripts, frames, sockets): those, ws,
// wss and blob: too.
bool loadable(std::string_view url, bool allowData);
// Whether a host (as in a canonical URL) is this computer or the local
// network: localhost and *.localhost, loopback, private, link-local,
// carrier-grade NAT and unspecified addresses.
bool localHost(std::string_view host);

} // namespace viewer::media
