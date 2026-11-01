# UI Design Plan — fixing the "hollow middle"

**Date:** 2026-11-01
**Status:** proposal, nothing implemented yet
**Scope:** `webui/index.html`, `webui/style.css`, `webui/app.js`, plus one small
backend addition if the left rail is built.

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
- `#nav-right` is a dead selector — styled twice, matches nothing.
- The idle sync tooltip can overflow at narrow widths; item 4 removes the
  tooltip entirely.
