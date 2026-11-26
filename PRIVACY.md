# Privacy Policy: viewerconfig

Last updated: November 24, 2026

This policy covers **viewerconfig**, the web service that Obelisk Viewer uses
for its news, grid status, configuration, lobby scene and updates. The Obelisk
Viewer project runs it at `viewerconfig.coyoteware.com`.

**In short:** you don't need an account, and the viewer doesn't send
viewerconfig your Second Life name, password, or any ID that identifies you or
your computer. Your IP address may be logged when the viewer connects. We don't
use viewerconfig for analytics, advertising or tracking.

## What the viewer sends

| When | What it asks for |
|------|------------------|
| The viewer starts | Configuration: feature flags, default settings, the latest version, and which lobby scene to use |
| The lobby (login screen) is showing | News and grid status, then updates every so often while the lobby stays open |
| News has pictures | Those pictures |
| The viewer starts, then every hour or so | The list of sites whose in-world media may play by itself. Your computer compares media with it; the viewer never sends what media is around you |
| The lobby scene has changed | The new scene file |
| An installed viewer starts, then every 6 hours (sooner after a failure) | Whether there is a newer version |
| There is a newer version | The update's files |

Each request includes:

- **Your IP address.** Any internet connection needs this so that the reply
  can reach you.
- **The viewer's name and version**, and for everything except news pictures,
  your operating system's type. For example: `ObeliskViewer/0.4.0 (linux)`.
- **The address requested** (for updates, this names the installed edition and
  operating system, or the update's file), and standard technical headers such as the kinds
  of reply the viewer accepts.
- **An ETag**, if the viewer has a saved copy of a reply. This is a label for
  that copy's contents, so the service can answer "nothing has changed". It is
  the same for everyone who has that copy, and isn't an ID for you.

The viewer uses no cookies for these requests, and does not include your
Second Life account, login details, chat, inventory, location or friends.

Everyone gets the same answers. The viewer decides on your computer which
flags and settings apply to you.

## What we keep

viewerconfig's database holds no information about viewers or the people using
them. Your connection does pass through other systems that may log it, so
**assume that your IP address is logged**:

- **Cloudflare**, which connections pass through on the way to our server. It is
  a separate company, and handles connection details under
  [its own privacy policy](https://www.cloudflare.com/privacypolicy/).
- **nginx**, the web server on our server.
- **uWSGI**, which runs viewerconfig on our server.

Besides your IP address, these logs may include the time, the address
requested, the result, and the viewer's name, version and operating system's
type.

We also use **fail2ban** on our server to limit abusive connections. It uses IP
addresses to spot abuse and slow down or block where it comes from, and may
keep its own record of them.

We use these logs to run the service, fix problems and prevent abuse. We don't
use them to work out who you are or to build a profile of you.

The company hosting our server necessarily handles its network traffic, and
may process connection details under its own privacy policy.

We don't sell this data, or share it for advertising.

## What stays on your computer

The viewer saves the latest configuration, news, grid status, media list and
their ETags, and the lobby scenes it has downloaded, so that the lobby works offline. These
are in the `viewerconfig` folder inside the viewer's cache:

- **Windows:** `%LOCALAPPDATA%\obelisk\cache\viewerconfig`
- **Linux:** `~/.cache/obelisk/viewerconfig` (or under `$XDG_CACHE_HOME`)
- **macOS:** `~/Library/Caches/obelisk/viewerconfig`

If you have chosen another cache folder, look there instead. You can delete
the `viewerconfig` folder at any time while the viewer is closed.

Installed copies also keep downloaded updates until they're installed, and
some after. The update system, Velopack, may keep an installation ID on your
computer for its own use. The viewer doesn't send it.

## Other sites

News and grid status come from Second Life's sites and other feeds. Our server
fetches them, so showing them doesn't connect you to those sites. If you open
a link, it opens in your web browser, and that site's privacy policy applies.

This policy doesn't cover the rest of what the viewer connects to, such as
Second Life itself (see
[Linden Lab's privacy policy](https://www.lindenlab.com/privacy)), media
streams, or places you download the viewer from.

## Turning it off

In the viewer's settings, under **Network**, set **Viewer news and status
service** to `off`, then restart the viewer. It then uses what it has saved.

Saved news can still list pictures, which the viewer would download from
viewerconfig. To prevent that as well, close the viewer after changing the
setting and delete the `viewerconfig` folder described above.

You can also set this to a different server's address. This policy doesn't
apply to servers we don't run.

To stop only update checks and downloads, turn off **Download updates** there
and restart the viewer. An update already downloaded is still installed.

## Your rights

Depending on where you live, you may have the right to ask for a copy of your
personal data, or for it to be corrected or deleted. Because viewerconfig has no
accounts or IDs, logged IP addresses are usually all we could match to you, and
we may not be able to confirm that one is yours. You are welcome to ask.

## Changes

viewerconfig doesn't collect telemetry. If that or anything else described
here changes, we will update this policy and the date at the top. Earlier
versions stay in this repository's history.

## Contact

For questions or requests, please
[open an issue](https://github.com/FelixWolf/obelisk-viewer/issues). Issues are
public, so please don't put personal information in them.
