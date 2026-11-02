# UI Design Plan

**Date:** 2026-11-01 (platform options added 2026-11-02)
**Status:** proposal, nothing implemented yet
**Scope:** `webui/index.html`, `webui/style.css`, `webui/app.js`, plus one small
backend addition if the left rail is built. The platform options at the end of
this document also touch `config.json`, `nginx/nas.conf` and `setup.sh`.

**Contents**

1. [Desktop: the hollow middle](#desktop-the-hollow-middle) — diagnosis and five-step plan
2. [Platform options](#platform-options) — mobile web vs. a phone app

## Origin

The UI reads clean but empty — specifically the Files tab, where a narrow
column of rows floats in a large field of flat near-black. The brief was
explicitly *not* to crowd the middle, where files live. So every change below
works on the frame, the header band, or the margins — not on the file rows.

[awwwards](https://www.awwwards.com/) was the reference point. Worth recording
what that turned up, because it was thin: the listing pages serve base64
placeholder thumbnails (real screenshots are on a separate CDN) and carry no
jury commentary, so the only readable signal was the filter taxonomy —
technology filters dominated by GSAP, Lottie, Three.js, WebGL and Locomotive
Scroll, and a typeface list that is almost entirely neo-grotesks (Aeonik,
Apercu, Akzidenz, Basel Grotesk).

That is itself the useful conclusion: most award-site technique is wrong here.
Scroll-jacking, marquees, cursor followers and WebGL heroes are for sites you
visit once. This is a tool someone opens forty times a day. One thing does
transfer — the big-element-against-small-text type contrast — and it is listed
below. The rest of this plan comes from measuring our own CSS.

# Desktop: the hollow middle

## Diagnosis

Measured against the current stylesheet, not estimated.

### 1. The content column has no edges

`#top-nav` is `background: transparent; border-bottom: none`, so the nav is not
a surface — it is two pills floating in space. `main#file-list` is
`max-width: 960px` with no background and no border. On a 1920px viewport that
leaves **~480px of bare `--bg` (`#070809`) on each side**, with nothing to tell
the eye it is a margin rather than an absence.

### 2. All chrome is pressed into the four corners

Sync + sign-out FABs bottom-left, folder + upload FABs bottom-right, nav pills
top-left and top-right. This layout *maximises* the empty middle: a donut of
nothing sits between the perimeter furniture and the centre column.

### 3. The list stops a third of the way down

`.file-row` is `padding: 8px 10px` at `font-size: 13px` — roughly 31px tall. A
10-item folder is ~340px of list under an 80px nav, leaving about **630px of
flat background below the last row** on a 1080p screen. `.empty-state` is the
extreme case: `padding: 80px 0` and a single 13px sentence in a 960px void.

### 4. There is no texture anywhere

All 1536 lines of `style.css` contain no `linear-gradient`, no
`radial-gradient`, no `background-image` and no `backdrop-filter`. Every
surface is a flat fill. Hover is `--surface` (`#0d0f13`) against `--bg`
(`#070809`) — a six-value difference, effectively invisible, so rows do not
feel touchable.

### 5. The two main views do not share a measure

| Element | `max-width` |
| --- | --- |
| `#nav-inner` | 1100px |
| `.dash-screen` (Overview) | 1100px |
| `main#file-list` (Files) | 960px |

Switching Files → Overview silently changes the content width, and on Files the
nav pills overhang the list by 70px a side. Nothing lines up with anything,
which is a large part of why nothing feels anchored.

## Plan

Ordered by impact per unit of work. Items 1–3 are one sitting with no backend
work; the recommendation is to ship those first and re-judge from there.

### 1. Make the list a panel that reaches the bottom

Give `#file-list` a `--surface` background, a `--border`, a radius,
`height: calc(100vh - var(--nav-h) - 32px)`, and its own internal scroll with
`.list-header` made sticky.

Roughly 15 lines of CSS. The emptiness below the last row does not disappear —
it becomes emptiness *inside a defined box*, which reads as room rather than
void. This is what Finder and Dropbox do. It also resolves the open 768px page
scroll issue on this tab, since the panel is viewport-bounded by construction.

### 2. Align every measure to 1100px

Nav, file list and dashboard on the same measure. The pills then sit directly
above the panel's top corners and the layout locks into a grid.

### 3. Add a folder header above the list

Currently there is `#path-bar` and nothing else. Add:

- the current folder name at 28–32px — the one borrowed move, big element
  against small text, to give the page an anchor;
- a thin metadata line beneath it: *4 folders · 18 files · 2.3 GB · modified 2
  days ago*.

Every value there is computable from the `/api/ls` response already in hand.
Fills the top band with real information and shortens the dead zone below.

### 4. Left context rail, ~260px

The structural fix for the horizontal void, and it works by adding things that
currently have nowhere to live:

- **Storage meter.** Needs a new endpoint — about 20 lines of `statvfs()` in
  `FilesystemController`.
- **Sync card:** last run, files copied, next scheduled run. Sync status is
  presently a corner FAB whose entire UI is a CSS tooltip, and
  `/api/sync/runs` — already built and tested — has no interface at all. This
  gives it one.
- **Pinned folders** for quick jumps.
- Today's event count, linking through to Overview.

Retires two corner FABs, spends the dead space on information rather than
decoration, and closes two open threads.

### 5. Texture — last

A faint radial gradient from the top centre, plus a very low-opacity SVG grain
overlay on `body`. Flat near-black is what makes a screen read as *unrendered*
rather than *dark*.

Deliberately sequenced last: done before the structural work, it is just a
finish applied to the wrong shape.

## Explicitly out of scope

The file rows themselves. No thumbnails, no cards, no preview tiles by
default — the middle stays for files. The one candidate for later is an
**opt-in grid view toggle** for image-heavy folders, which is acceptable
because it is the user choosing density rather than the UI imposing it.

## Related open items

Tracked elsewhere, but these intersect:

- Dark-theme `--muted` (`#4a4e62`) is 2.44:1 against `--bg` — fails WCAG AA.
  `#767c96` (4.86:1) is the suggested replacement.
- The bare empty state ("This folder is empty") has no inline action; item 1
  and a richer empty state should land together.
- `#nav-right` is a dead selector — styled twice, matches nothing. Not
  merely dead code: see mobile bug 7 below, where it leaves the phone nav
  overflowing.
- The idle sync tooltip can overflow at narrow widths; item 4 removes the
  tooltip entirely.

---

# Platform options

Separate question from the hollow middle, and a bigger one: the web UI is
reported to look *bad* on a phone. Two routes out, which are not mutually
exclusive — option 1 is a prerequisite for the cheap version of option 2.

## Option 1 — make the web UI work properly on a phone

### What is actually broken

Measured, not guessed. There are four media queries (`600px`, `768px`, `520px`,
`600px` again) but they only reflow the nav, the stat cards and the dashboard
grids. Nothing touches the file list, and nothing accounts for touch input.

**1. The filename column collapses to zero width.** This is the headline bug.
`.file-row` is `grid-template-columns: 24px 1fr 90px 130px 80px` with
`gap: 8px` — so 324px of fixed columns plus 32px of gaps is **356px of
non-negotiable width**. At the 768px breakpoint `main#file-list` drops to
`padding: 16px`, and `.file-row` adds `padding: 8px 10px`:

| Viewport | Row inner width | Fixed + gaps | Left for the name |
| --- | --- | --- | --- |
| 390px (iPhone 14) | 338px | 356px | **−18px** |
| 360px (common Android) | 308px | 356px | **−48px** |

The `1fr` name column is squeezed to nothing and the row overflows the screen.
On a phone the file list is a column of sizes and dates with the filenames
ellipsed away — which is almost exactly "looks bad".

**2. Every hover affordance is unreachable.** There is not a single
`@media (hover: hover)` or `pointer: coarse` query in the stylesheet, so on
touch:

- `.file-actions` sits permanently at `opacity: 0.28` (it is revealed by
  `.file-row:hover`);
- `.drag-handle` is `color: var(--border)` — effectively invisible;
- every tooltip is a `::before`/`::after` on `:hover`, so all of them are dead.
  That includes the sync FAB, whose *entire* status readout is a `data-tip`
  tooltip. On a phone there is no way to see sync state at all.

**3. Tap targets are far too small.** `.file-actions button` is
`padding: 3px 5px` at `font-size: 14px` — roughly 20×20px against a 44×44pt
(Apple) / 48×48dp (Android) minimum. Delete sits 4px from rename, which is a
mis-tap waiting to happen on a destructive action.

**4. iOS zooms the login screen and never zooms back.** `.login-box input` is
`font-size: 14px`. Safari auto-zooms any focused input below 16px. The very
first thing the app does on an iPhone is jump to a zoomed viewport.

**5. The safe area is declared but never used.** `index.html` sets
`viewport-fit=cover`, which deliberately extends the page under the iOS home
indicator — and there are **zero** `env(safe-area-inset-*)` references in the
stylesheet. `#fab-upload` at `bottom: 28px` and `#upload-bar` at `bottom: 24px`
therefore sit under the gesture bar.

**6. Drag-to-move does not exist on touch.** The move gesture is HTML5
`dragstart`/`dragover`/`drop`, and `app.js` registers no `touchstart`,
`touchmove` or `pointerdown` handlers anywhere. Moving a file is desktop-only.
`#drop-overlay` ("Drop files to upload") is also meaningless on a phone.

**7. `#nav-right` is a dead selector, and that breaks the phone nav.** The rule
at `style.css:1432` is `@media (max-width: 520px) { #nav-right { display: none } }`
— but the element is `#nav-right-group`. The clear intent was to hide the theme
toggle and the Rules/Logs pill on small screens; it does nothing. Meanwhile the
same query resets `#nav-inner` to `flex-direction: row`, so a 390px phone tries
to fit three left tabs (~210px) plus a 34px toggle plus two right tabs
(~130px) on one line. It overflows. This is the one item already on the
open-issues list that turns out to be a live layout bug rather than dead code.

### The fix

- **A dedicated phone layout for the file row.** Below ~640px, drop the grid
  entirely and go to a two-line list item: filename on line one at 15px, then
  `size · modified` as a single muted 12px line beneath. Minimum 56px tall.
  This is the standard mobile file-list pattern and it removes the fixed-column
  arithmetic that causes bug 1.
- **Replace per-row action buttons with one overflow affordance.** A single
  44×44px `⋯` button per row opening a bottom sheet (Download / Rename / Move /
  Delete). Fixes bugs 2 and 3 together, and makes Move reachable on touch
  without implementing a touch drag — a "Move to…" folder picker is both easier
  to build and better on a phone than dragging.
- **Wrap hover-only styling in `@media (hover: hover)`** and give touch devices
  full-opacity actions.
- **Bump all inputs to 16px** on small screens.
- **Use `env(safe-area-inset-bottom)`** in the FAB, upload bar and bottom-sheet
  offsets, since `viewport-fit=cover` is already set.
- **Fix the nav:** correct `#nav-right` → `#nav-right-group`, and on phones
  move the five tabs into a bottom tab bar — which is the native-feeling
  pattern anyway and frees the top for the folder header from the desktop plan.
- **Verify on real viewport sizes** (390×844, 360×800, 414×896) rather than by
  eye.

### Verdict

Do this regardless of what happens with option 2. It is a day of CSS plus a
bottom sheet, it needs no new tooling, and every version of option 2 that is
worth doing sits on top of it.

## Option 2 — build a phone application (Android)

**Resolved 2026-11-02: the phone is an Android.** That settles the open
question this section used to hedge on, and it removes the single largest cost
in the whole option — see *Distribution* below. The analysis has been rewritten
accordingly; the iOS notes are kept only where they explain why a decision went
the way it did.

### Prerequisite: uploads are capped at 1 MB today

Found while costing this out, and it blocks every version of option 2 as well
as the existing web UI.

`nginx/nas.conf` is configured correctly for large uploads:

```nginx
client_max_body_size 0;
proxy_request_buffering off;
```

Drogon then undoes it. Its own config template (embedded in `drogon_ctl`) says:

> `client_max_body_size`: Set the maximum body size of HTTP requests received
> by drogon. The default value is `"1M"`. One can set it to `"1024"`, `"1k"`,
> `"10M"`, `"1G"`, etc. Setting it to `""` means no limit.

`config.json`'s `app` block never set the key, so nginx streamed the body
through unbuffered and Drogon rejected anything over **1 MB** with a 413. A
photo from any modern Android phone is 3–8 MB; a video is hundreds. Uploads
were effectively broken for real files.

**Fixed** by adding `"client_max_body_size": "8G"` to `config.json`. A finite
cap rather than `""`: with `proxy_request_buffering off`, Drogon spills bodies
over `max_memory_body_size` (default 64K) to `upload_path`, which sits on the
same disk as the NAS — so an unlimited cap lets one request fill the disk.
8G is far above any phone file while still bounding that.

Deploy with `sudo bash setup.sh --update`; the config merge adds new keys to an
existing deployment without touching existing values.

Not verified against a running server: `nas-backend` was `inactive` at the time
of writing, so the 1 MB figure comes from Drogon's documented default plus the
missing key, not an observed 413.

### What a native app can do that a browser cannot

Short list, and worth being strict about it:

- **Background upload / automatic camera-roll backup.** This is the only
  genuinely unachievable-on-web feature, and it is the real reason to build a
  phone app for a NAS. A browser cannot reliably upload photos while
  backgrounded.
- **Share-sheet target** ("Share → HomeNAS").
- **Native downloads** into the OS file system, and offline cached files.

And the things that *sound* like native wins but are not:

- **Push notifications for alerts** — web push works on Android Chrome and, as
  of iOS 16.4, on installed PWAs.
- **Biometric unlock** — WebAuthn covers this in the browser.
- **Home-screen icon, no browser chrome, splash screen** — all PWA features.

So the decision reduces to a single question: *do you want automatic photo
backup?* If no, native buys essentially nothing. If yes, it is the one feature
that justifies the whole exercise.

### Distribution — no longer a cost

This was going to be the deciding factor and Android removes it. Build an APK,
install it, done: no developer account, no fee, no review, no expiry.

For contrast, and to record why the recommendation would otherwise have been
different: on iOS a free developer account re-signs builds that expire **every
7 days**, a paid account is **$99/year**, and App Store review for an app that
only talks to the author's own house is an argument not worth having. That
treadmill alone would have been enough to stop at a PWA. It does not apply.

### The middle path, and the actual recommendation

**Make the web UI an installable PWA.** This is the step that does not seem to
be on the radar and it captures most of the perceived value of an app:

- `manifest.webmanifest` with icons, `display: standalone`, theme colour —
  gives a home-screen icon that opens with no browser chrome. Visually it *is*
  an app.
- A service worker caching `index.html`, `style.css`, `app.js` for an instant
  offline-capable shell.
- **On Android this is much stronger than on iOS.** Chrome generates a real
  WebAPK, so it gets an app-drawer entry and appears in the app switcher — and
  it can register as a **Web Share Target**, which makes "Share → HomeNAS"
  work from Gallery or any other app. Web push works properly too. Between
  them those cover a large share of what is actually wanted from a NAS app.

Cost: roughly 150 lines, no build toolchain, no second codebase, no store.

**One real blocker to note:** service workers require a trusted secure context.
The current setup generates a **self-signed** cert for `nas.local`, which the
browser treats as untrusted — so the service worker will not register. The
clean fix is `tailscale cert`, which issues a genuine Let's Encrypt
certificate for the MagicDNS `*.ts.net` name. That is worth doing on its own
merits and is a prerequisite for the PWA, so it should be folded into
`setup.sh`.

### Android specifics that will bite

- **Since Android 7, apps ignore user-installed CAs.** A native client's HTTP
  stack will not trust the self-signed `nas.local` cert unless the app ships a
  `network_security_config.xml` pinning the CA. `tailscale cert` fixes this
  *and* the PWA's secure-context requirement — one prerequisite serving both
  paths, which is a further argument for doing it first.
- **`nas.local` mDNS resolution is unreliable on Android.** Use the Tailscale
  MagicDNS `*.ts.net` name, not `.local`. A background worker should not depend
  on mDNS at all.
- **Android 13+ needs `READ_MEDIA_IMAGES` / `READ_MEDIA_VIDEO`**, not
  `READ_EXTERNAL_STORAGE`; Android 14 added partial-access grants, which a
  backup app does not want. Sideloading avoids the Play Store declaration that
  full media access would otherwise require.
- **Doze and battery optimisation** defer periodic work. Fine for photo
  backup, which is not time-critical, but `WorkManager` is the only sane way to
  schedule it.

### Two gaps in the upload API a backup client must work around

Beyond the 1 MB cap above, `/api/upload` takes standard multipart with a
`?path=` destination and ignores the field name, so an OkHttp `MultipartBody`
posts to it directly. But:

- **`f.saveAs()` overwrites silently** and there is no existence check, so the
  client has to track its own uploaded-set — `/api/ls` is the only way to ask
  what is already there.
- **There is no resumable or chunked upload.** A failed transfer restarts from
  zero, which matters on a phone uploading a multi-gigabyte video over a
  flaky link.

### If a native app is still wanted

Realistic stacks, ranked by effort-to-value here:

| Approach | Why | Cost |
| --- | --- | --- |
| **Capacitor + a Kotlin plugin** ← recommended | Wraps the *existing* `webui/` unchanged and adds **only** the backup worker as native code. One UI codebase. It can also pin your own CA, so it sidesteps the cert problem the PWA cannot. | Lowest, if the PWA exists first |
| **Kotlin + Compose** | `WorkManager` + `MediaStore` + OkHttp multipart is exactly the right API set and is first-class. Best-feeling UI, but duplicates the whole file browser. | Moderate |
| **Flutter** | `background_downloader` is solid and it is good at list UIs, but cross-platform is now a benefit you are not buying, and Dart is a new language. | High, for less reason |
| **React Native / Expo** | Closest to existing `app.js` knowledge, but background upload support is the weakest of these — which is the one thing the app exists to do. | High |

Android-only is now the target, so Flutter and React Native both lose their
main justification: there is no second platform to amortise them over.

The API does not block any of them: `/api/*` is already clean JSON with JWT
auth, so a second client is a presentation-layer job. One gap to close first —
`jwt_expiry_seconds` defaults to **86400**, and there is no refresh-token flow
(`AuthController.cpp:73`), so any phone client would demand a fresh login
every 24 hours. A mobile app wants a long-lived refresh token.

### Verdict

Android makes a native app *viable*, not yet *necessary*. The distinction
matters: the only thing it buys is automatic camera-roll backup, so build it
when that is the thing being missed, not before. The ordering that gets the
most value per hour:

0. **Deploy the upload-cap fix.** Done in `config.json`; needs
   `sudo bash setup.sh --update`. Nothing else works without it.
1. **Fix the responsive web UI** (option 1) — mandatory, cheap, fixes the
   actual complaint.
2. **`tailscale cert` + manifest + service worker.** On Android this yields an
   app-drawer entry, a chrome-less window, offline shell, push, and
   "Share → HomeNAS" for ~150 lines. For many uses that is the whole app.
3. **Only if automatic photo backup turns out to be the gap:** wrap the same
   web UI in Capacitor and add one Kotlin plugin — `MediaStore` query +
   `WorkManager` + OkHttp multipart to `/api/upload`.

Step 3 may never be needed, and nothing in steps 0–2 is wasted if it is.
