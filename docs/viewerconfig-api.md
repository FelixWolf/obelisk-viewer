# viewerconfig API (v1)

The contract between the viewer (this repo) and **viewerconfig**, the
project's own web service (Django, `~/source/websites/viewerconfig`). The
viewer asks viewerconfig, never Linden Lab's sites directly, for:

1. **Config:** feature flags, overrides of the viewer's settings, the latest
   viewer version, and which home scene to show.
2. **News:** entries merged from several feeds plus posts written in the
   admin, with pinning.
3. **Grid status:** Linden Lab's status page, digested, plus announcements
   written in the admin.
4. **Home scene:** the lobby's scene file (`default_scene/sceneConvert.py`'s
   output; see `src/world/include/viewer/world/HomeScene.hpp`).
5. **Media whitelist:** the sites whose media on prims may play by itself.

The server fetches from Linden Lab on a schedule and caches what it gets. Any
number of viewers then cost Linden Lab nothing more. Telemetry comes later,
in its own version of this document.

Change this file first when either side needs something different; both
sides follow it.

## General

- **Base URL:** `https://<host>/api/v1/`. The viewer has the host built in
  and can override it in its settings, for a test server.
- **Format:** JSON, UTF-8. Times are ISO 8601 in UTC (`2026-11-20T15:04:05Z`).
  A field the server has no value for is `null`, never missing. The viewer
  ignores fields it doesn't know, so the server can add fields without a new
  version.
- **Versioning:** every response has `"schema": 1`. A change that would break
  older viewers goes under `/api/v2/`, and v1 stays up while released viewers
  use it.
- **Caching:** every response has an `ETag` and a `Cache-Control: max-age`.
  The viewer sends `If-None-Match` and expects `304 Not Modified` when nothing
  changed. Responses are the same for every viewer (nothing per user), so a
  CDN or nginx can cache them in front of Django.
- **Offline:** the viewer keeps the last good copy of each response on disk
  and uses it when the server can't be reached. The home scene also has a
  copy built into the viewer.
- **Privacy:**
  - Requests carry no identifiers, cookies or account details.
  - The only thing that varies is the `User-Agent`: `ObeliskViewer/<version>
    (<os>)`, for example `ObeliskViewer/0.4.0 (linux)`. The name is
    `VIEWER_PACKAGE_ID` (`cmake/Branding.cmake`), so it follows a rename.
  - Flags and overrides are filtered by the viewer, not the server (see
    Config), so nothing about the viewer has to be sent to get them.
  - The server should not log client IP addresses, or should truncate them.
- **Images:** the server downloads feed images, resizes them (at most 640
  px wide), re-encodes them as JPEG or PNG and serves them under `/media/`
  at names made from their content's hash, with
  `Cache-Control: max-age=31536000, immutable`. The viewer decodes PNG and
  JPEG only.
- **Text:** plain text only, no HTML (the viewer's interface can't show
  arbitrary HTML). Paragraphs are separated by a blank line (`\n\n`). The
  server strips HTML from feed content and decodes its entities.
- **Links:** `url` fields are opened in the user's web browser, never inside
  the viewer.

## Config: `GET /api/v1/config`

Fetched once at startup, before the login screen shows anything that
depends on it. `max-age` is 300.

```json
{
  "schema": 1,
  "latest_version": {
    "version": "0.4.0",
    "url": "https://example.org/download",
    "notes": "Plain text: what's new.",
    "required": false
  },
  "flags": [
    {
      "name": "pbr_terrain",
      "enabled": true,
      "min_version": null,
      "max_version": "0.3.9",
      "platforms": ["linux", "windows"]
    }
  ],
  "settings": [
    {
      "key": "ShadowDistance",
      "value": 96,
      "min_version": "0.4.0",
      "max_version": null,
      "platforms": null
    }
  ],
  "home_scene": {
    "url": "https://<host>/api/v1/home-scene/3f2a...e9.scene",
    "sha256": "3f2a...e9",
    "size": 287329
  },
  "refresh": {
    "news": 900,
    "grid_status": 120
  }
}
```

- **`latest_version`:** the newest release.
  - The viewer compares it with its own version and offers the download
    when it's newer.
  - `required: true` means older viewers must not log in (for a release
    that fixes something serious). Use it rarely.
  - `null` means nothing to say.
- **`flags`:** names the viewer knows, each on or off.
  - An entry applies only when every condition holds. `min_version` and
    `max_version` are inclusive. `platforms` lists `linux`, `windows` or
    `macos`, and `null` means all of them.
  - The first entry for a name that applies wins.
  - A name with no entry that applies keeps the viewer's built-in default.
  - Unknown names are ignored.
- **`settings`:** values that replace the viewer's defaults for its
  settings.
  - Conditions work the same way as for flags.
  - A setting the user has changed themselves keeps the user's value.
  - `value` is a JSON boolean, number or string matching the setting's type.
    A value of the wrong type is ignored.
  - Unknown keys are ignored.
- **`home_scene`:** the scene the lobby shows.
  - The viewer downloads it only when `sha256` differs from its cached copy,
    and checks the download against `sha256` and `size`.
  - `null` means the scene built into the viewer.
- **`refresh`:** how often, in seconds, the viewer asks for news and grid
  status while the lobby is showing.
  - Its own minimums are 300 seconds for news and 60 for grid status.

Versions compare as dot-separated numbers: `0.10.0` is newer than `0.9.2`.

## Home scene: `GET /api/v1/home-scene/<sha256>.scene`

The file itself: binary LLSD, `Content-Type: application/octet-stream`. Its
name holds its hash, so it never changes:
`Cache-Control: max-age=31536000, immutable`.

## Updates: `GET /api/v1/updates/<package id>/...`

Velopack's feed and packages, for packaged builds (`docs/packaging.md`).

- `releases.<channel>.json`: the feed, `max-age` 300. `channel` is Velopack's
  (`win`, `linux`). An unknown package or channel has no assets (`200`).
- `<file name>.nupkg`: a package, `application/octet-stream`.

```json
{
  "schema": 1,
  "Assets": [
    {
      "PackageId": "ObeliskViewer",
      "Version": "0.4.0",
      "Type": "Full",
      "FileName": "ObeliskViewer-0.4.0-full.nupkg",
      "SHA1": "EAA9...",
      "SHA256": "F52E...",
      "Size": 4492162
    }
  ]
}
```

- **Names:** fields are Velopack's. `Type` is `Full` or `Delta`.
- **Offered:** only packages of releases published in the admin. Unpublishing
  a release stops its downloads (`404`).
- **Immutable:** a package's contents never change under its name.
- **Integrity:** the viewer checks `Size` and `SHA256`, and takes updates only
  over `https` (or from this computer, to test). Packages aren't signed, so
  trust rests on this service.
- **Privacy:** Velopack's own HTTP source sends an installation id in the feed's
  query, so the viewer doesn't use it: requests are as in General.
- **When:** at startup and every 6 hours while running (15 minutes after a
  failure), unless `network.auto_update` is off or the service is.

## News: `GET /api/v1/news`

Fetched when the lobby shows, then every `refresh.news` seconds while it
does. `max-age` is 300.

```json
{
  "schema": 1,
  "entries": [
    {
      "id": "post-12",
      "source": {"id": "viewer", "name": "TestViewer"},
      "title": "Welcome to the new lobby",
      "summary": "Plain text, at most about 300 characters.",
      "body": "Plain text, paragraphs separated by blank lines.",
      "url": null,
      "published": "2026-11-20T15:04:05Z",
      "image": {"url": "https://<host>/media/news/ab12...png", "width": 640, "height": 360},
      "pinned": true
    },
    {
      "id": "feed-3-9f1c...",
      "source": {"id": "sl-blog", "name": "Second Life Blog"},
      "title": "New Second Life Viewer Update",
      "summary": "The latest Second Life Viewer (26.5) is here...",
      "body": null,
      "url": "https://community.secondlife.com/news/...",
      "published": "2026-11-18T17:00:00Z",
      "image": null,
      "pinned": false
    }
  ]
}
```

- **Order:** the server sends entries in the order to show them.
  - Pinned entries come first, in the order set in the admin.
  - The rest follow, newest first.
  - At most 30 entries.
- **`id`:** stable for an entry's lifetime, so the viewer can remember
  which entries it has shown. Feed entries take theirs from the feed's
  `guid` (or link).
- **`source`:** the feed's, or `viewer` for posts written in the admin.
- **`body`:** the whole text of a post written in the admin. It's `null` for
  feed entries, which show their summary and link to `url`.
- **`url`:** what clicking the entry opens. `null` means nothing does; a
  post with a `body` shows the post itself.

**Admin (server side):**
- **Feeds:** a list of RSS or Atom URLs, each with a name, an id (slug), on
  or off, and how often to fetch it. The first one is the Second Life blog,
  `https://community.secondlife.com/rss/1-blog-rss.xml/`.
- **Posts:** written in the admin, each with a title, summary, body, optional
  image, optional link, publish time and optional expiry time.
  - A post shows once its publish time has passed, and stops after its
    expiry.
- **Pinning:** any entry, from a feed or a post, can be pinned, with a
  position among the pinned ones and an optional time when the pin ends.
- **Hiding:** any feed entry can be hidden, for one that shouldn't show.

## Grid status: `GET /api/v1/grid-status`

Fetched when the lobby shows, then every `refresh.grid_status` seconds
while it does. `max-age` is 60.

```json
{
  "schema": 1,
  "fetched": "2026-11-20T15:03:00Z",
  "stale": false,
  "page_url": "https://status.secondlifegrid.net/",
  "overall": {"indicator": "minor", "description": "Partially Degraded Service"},
  "announcements": [
    {
      "id": 4,
      "severity": "warning",
      "title": "Logins failing for some",
      "body": "Linden Lab knows; their office opens at 9 am SLT.",
      "posted": "2026-11-20T14:30:00Z"
    }
  ],
  "components": [
    {"name": "Logins", "group": "Second Life Regions", "status": "operational"},
    {"name": "Group Text Chat", "group": "Inworld Services", "status": "degraded_performance"}
  ],
  "incidents": [
    {
      "name": "Unscheduled Group Chat Maintenance",
      "status": "investigating",
      "impact": "minor",
      "update": "Plain text of the latest update.",
      "updated": "2026-11-20T13:10:00Z",
      "url": "https://status.secondlifegrid.net/incidents/..."
    }
  ],
  "maintenance": [
    {
      "name": "Scheduled Region Restarts",
      "status": "scheduled",
      "starts": "2026-11-21T08:00:00Z",
      "ends": "2026-11-21T12:00:00Z",
      "update": "Plain text.",
      "url": "https://status.secondlifegrid.net/incidents/..."
    }
  ]
}
```

- **Source:** the server fetches Linden Lab's
  `https://status.secondlifegrid.net/api/v2/summary.json` (a Statuspage
  site) about once a minute.
- **`fetched` and `stale`:** when the server last got an answer from Linden
  Lab. `stale` is true when that was more than 10 minutes ago; the viewer
  then says the status may be out of date.
- **`overall`:** Linden Lab's own summary.
  - `indicator` is one of `none` (all good), `minor`, `major`, `critical` or
    `maintenance`.
  - `description` is their text.
- **`announcements`:** written in the admin, each with a severity (`info`,
  `warning` or `critical`), a title, a body, and the times it starts and ends
  showing.
  - Only those showing now are listed, most severe first, then newest.
  - The viewer shows them above Linden Lab's status, worded as coming from
    the viewer's team rather than from Linden Lab.
- **`components`:** every component Linden Lab lists, with its group's name.
  - Groups themselves aren't listed as components.
  - `status` is one of `operational`, `degraded_performance`,
    `partial_outage`, `major_outage` or `under_maintenance`.
  - The viewer may show only those that aren't `operational`.
- **`incidents`:** unresolved incidents.
- **`maintenance`:** scheduled maintenance that hasn't ended yet.
- **`update` (incidents and maintenance):** the newest update's text.

## Media whitelist: `GET /api/v1/media-whitelist`

The sites whose media on prims may start by itself, for people whose media
setting is "Whitelist" (the default) and where the prim asks to auto-play.
Media elsewhere isn't blocked: it waits for a click. Fetched at startup,
then every `refresh` seconds, logged in or not. `max-age` is 300.

```json
{
  "schema": 1,
  "sites": [
    {"host": "youtube.com", "subdomains": true, "path": "/"},
    {"host": "example.org", "subdomains": false, "path": "/media/"}
  ],
  "refresh": 3600
}
```

- **`host`:** lowercase ASCII (punycode), at least two labels, no port or
  wildcard.
- **`subdomains`:** hosts below it too (`www.youtube.com`, never
  `notyoutube.com`).
- **`path`:** starts with `/`; that path and what's below it, by whole parts
  (`/media/` lists `/media` and `/media/a`, not `/mediaplayer`). `/` is the
  whole site. No escapes, `.` or `..`.
- **Matching:** only `https` on its usual port. The viewer does it, so
  viewerconfig never learns what media anyone sees. Anything it can't read
  plainly counts as not listed, and entries breaking these rules are ignored.
- **Navigation:** every page a tab goes to by itself is checked, redirects
  included. Going to one that isn't listed waits for a click.
- **`refresh`:** the viewer's own minimum is 300 seconds.
- **No list yet** (never reached): nothing is listed.
- **Not a promise of privacy:** a listed site still sees the IP addresses of
  those whose viewers play it. List no link shorteners or open redirectors.
- **Home scene:** the sites of media in the lobby's scene have to be listed
  for it to play by itself.

**Admin (server side):** the sites, each on or off with a note, and the
refresh. YouTube and Second Life are there from the start.

## Errors

- Anything other than `200` or `304` counts as a failure: the viewer keeps
  what it has and tries again at the next refresh.
- The server answers `503` while it has never managed to fetch a source
  (for example, just after deploying, before the first status fetch).
