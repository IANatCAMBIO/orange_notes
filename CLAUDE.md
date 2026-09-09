# Notes — project guide

Apple Notes–style app in **plain C + GTK3 + SQLite**. Two window types:
a Library (folders/tags sidebar, notes as list or grid) and one editor
window per note (WYSIWYG rich text). No GNOME HeaderBars anywhere —
plain `GtkWindow` titlebars, formatted `"Notes - <thing>"`.

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./notes
make run
make app      # dist/Notes.app (unversioned name; macOS; sips/iconutil;
              # composition.png → .icns; NOT self-contained — needs MacPorts GTK)
make deb      # dist/notes_<version>_<arch>.deb (needs dpkg-deb,
make rpm      # dist/notes-<version>-1.<arch>.rpm  needs rpmbuild —
              # build these ON the target Linux distro; they install to
              # /opt/notes + a /usr/bin wrapper script that execs by
              # absolute path so argv[0]-relative icons/defaults resolve)
```

The semantic version lives in the **`VERSION` file** at the repo root
(one line, e.g. `3.6.2`); the Makefile reads it into its `VERSION`
variable with `cat` (macOS ships GNU make 3.81, which has no
`$(file <…)`) and errors out if the file is missing or empty. Single
source: baked into the binary as `ON_VERSION` (About dialog), into the
.deb/.rpm filenames and into the .app bundle's Info.plist — the .app
bundle NAME is deliberately unversioned so the path in /Applications
never changes. Objects depend on both the Makefile and the VERSION file
so a version bump recompiles.

Dependencies (MacPorts): `gtk3 +quartz`, `sqlite3`, `pkgconf`, and
optionally `gtk-osx-application-gtk3` (native macOS menubar — pkg-config
module is **`gtk-mac-integration-gtk3`**, NOT "gtkmacintegration"; the
Makefile auto-detects it and defines `HAVE_GTKOSX`). librsvg is
OPTIONAL now that all app icons are PNGs (incl. `warning.png` in the
confirm dialogs) — it only renders the bundled `icons/theme/` symbolic
arrows.
After toggling a dependency, run `make clean && make` so every object
sees the new flags.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config init; sets `icons/composition.png` as default window icon |
| `src/app.[ch]` | Shared `OnApp` context: db handle, open-editors map, icon loading, the shared toolbar-button factory (`on_app_tool_item_new`) |
| `src/db.[ch]` | SQLite: folders (nested), notes (content BLOB), tags, note_tags, counts, ordering |
| `src/serialize.[ch]` | BNBF binary format ⇄ GtkTextBuffer; image anchors; shared GtkTextTag set (`on_buffer_ensure_tags`); the cheap image API the media browser needs — `on_note_count_images` (record walk, decodes nothing) and `on_note_image_nth` (decode ONE image by ordinal, capped), both on the same `bnbf_*` reader as everything else, with `png_decode_capped` now the ONE decode path (the full deserializer included) |
| `src/editor_window.[ch]` | WYSIWYG editor: new windows open in the screen's bottom-right corner, 12 px clear of the work area (`editor_place_bottom_right`, quirk #21); inline/paragraph formatting, list continuation, #tag autocomplete popup (never inside code blocks — capture is suppressed there, and `strip_tags_in_code_blocks` removes tag spans carried in by code-block formatting or paste), image paste/context menu (a single click on an embedded image opens the SHARED modal viewer over the text — `ed->overlay` wraps the text scroll, `editor_viewer_ops` addresses images by ORDINAL and looks the anchor up fresh every call so an edit underneath can only close the panel, and its action link is "Open in image viewer" = `image_open_external`, since "Show in source note" would be a no-op here; the click used to toggle that image's INLINE thumbnail/full size, which now lives only on the context menu.  The ordinal is defined by `editor_image_count`/`editor_image_nth`/`editor_image_ord` — THE walk, also behind `editor_reveal_image`), floating code-block "copy" links (plain labels hit-tested from the view's own press/motion handlers — quirk #22), title line (line 0 centered + heading-sized, both derived by `title_line_sync`, gated by `first_line_title`), debounced autosave; `on_editor_window_open_image()` opens a note scrolled to its Nth image (`editor_reveal_image` counts image ANCHORS, the same order `on_note_count_images` counts IMAGE records — that shared ordinal is the media browser's whole addressing scheme; deferred through an idle on a fresh window like the search-term open, immediate on an already-open one) |
| `src/library_window.[ch]` | Sidebar (folders+counts+emoji prefix, tags+counts), notes list/grid (list: Title/Path/Modified/Created, all resizable + sortable, Path and Created hidden by default; Path fed by `on_db_folder_path_map`; list density Compact/Comfortable — Comfortable renders a bold title + small dimmed body-text preview via `notes_title_cell_func` and `NL_PREVIEW`), notes sorted Modified-newest-first by default (in-list drag reorder is off while sorted — list stores refuse row drops), folder context menu has Sort Subfolders Alphabetically (one level, `on_db_folder_reorder`), DnD (notes→folder incl. multi-select; single folder rows re-nest INTO / reorder BEFORE-AFTER / trash / drag-restore via `on_db_folder_move`+`on_db_folder_reorder`; drag icons: folder.png, file.png for one note, documents.png for 2+), sortable headers, context menus, one unified toolbar (folder area \| notes area, whose Quicknote button (archive.png) calls `on_library_quicknote()` — a note in the ROOT folder whatever is selected, editor to the front; THE one implementation, also behind the `notes quicknote` CLI/IPC command \| Search …; the About button that used to sit at the far right, with its expanding spacer and per-style child swap, was removed 2026-08 — About lives in the File menu only), menubar (File/View), native-menubar hook, bottom status bar (left: selection path; selecting notes posts a transient "N files selected" event from both views' selection signals; right: latest event — post from anywhere via `on_app_status()`, printf-style, no-op until the library installs `app->notify_status`) |
| `src/search_query.[ch]` | THE query language, shared by the search window's worker and the CLI's `search` (it replaced `on_note_text_matches`, which knew only one literal needle): `on_query_new` parses the text into terms — bare words ANDed, `"quoted phrases"` matched whole, `-word`/`-"phrase"` excluding — and `on_query_matches` tests one note's title+body against all of them, folding the note ONCE however many terms there are. A '-' with space after it and an empty `""` are ordinary text/nothing; an unclosed quote runs to the end; curly quotes count as quotes (macOS input methods). A query of nothing but exclusions matches every note that avoids them, and one that parses to NO terms reports `on_query_is_empty` so callers can prompt instead of returning zero hits. Regex mode has no term syntax at all — the query is one pattern, compiled here so a bad one is caught before any searching. `on_query_highlight_term` (first positive term, unquoted) is what an opened result seeds the editor's in-note search with |
| `src/search_window.[ch]` | Search over titles + full text on a worker thread (spinner while running); scope = All Notes / live library selection; case + regex options; the query goes through `search_query.[ch]`, and the parsed OnQuery — immutable, so the worker matches with it freely — IS what the job carries |
| `src/image_viewer.[ch]` | THE modal image viewer, shared by the media browser and the editor: a GtkOverlay child (dark event box covering the whole overlay, so it swallows every click meant for the widget behind), the image fitted to the OVERLAY's allocation less `ON_IMAGE_VIEWER_INSET` on each side and the two label rows, a caption + host action link row, and a centred "Previous | Next" row (ONE GtkLabel carrying both links, told apart by href; NO wrap-around — at either end the word is dim plain text via Pango `alpha` and the key is consumed but does nothing, so an arrow can't leak through to the widget behind). Click anywhere / Escape closes; Left/Right and the links go through `img_step`. Hosts are DECOUPLED by `OnImageViewerOps` — `count`/`render`/`caption`/`action`, with images addressed by INDEX and `count()` re-read on every use, so a host whose set is still growing (the media browser mid-scan) needs only `on_image_viewer_nav_sync`. `render()` returns a surface the panel takes over; NULL closes the panel, which is what makes an edit under the editor's panel safe (the ops look the anchor up fresh every call and never hold a pixbuf). `action()` runs AFTER the panel has closed itself, so a host may open windows or tear itself down from it. The panel connects the overlay's `size-allocate` ITSELF (debounced 150 ms, guarded by the last fitted box) so neither host wires a configure-event — safe because GtkOverlay measures only its MAIN child, so the fitted image never feeds back into the allocation. `img_hit` keeps a press on a link label from closing the panel — needed because a DEAD-END "Previous"/"Next" carries no link, hence no input-only window, and does reach the backdrop handler. `on_image_viewer_fit` is the ONE pixbuf→device-scaled-cairo-surface fit (quirk #5), used by both hosts' `render`. `on_image_viewer_free` disconnects from the overlay: a host frees the panel from its window's `destroy`, which runs BEFORE the children are torn down. **The panel NEVER takes the keyboard focus** (`can_focus FALSE`, no `grab_focus`, no `<a>` link labels anywhere in it) — quirk #23 is the whole story, and it is why the clickable words are plain labels hit-tested in `img_press` and why every host must swallow the keys the panel does not want |
| `src/media_window.[ch]` | Media browser (library toolbar's images.png button, View \| Media…, Ctrl/Cmd+M): every image embedded in the notes the pane is listing, as a GtkFlowBox of square-boxed thumbnails captioned with their note. Note set comes from `notes_for_selection()` in library_window.c — THE one selection→note-list mapping, shared with `refresh_notes` — and is SNAPSHOTTED at open (id+title only). Scanning runs in 40 ms idle slices (`media_scan_idle`), one image per step, holding the current note's blob across yields; image counts come from `on_note_count_images` (record walk, no decode) and each thumbnail from `on_note_image_nth` at thumb size. Capped at MEDIA_MAX_IMAGES (500) with the truncation said in the status line — every cell holds its own decoded pixels. A single click opens the SHARED modal viewer (`image_viewer.[ch]`, which owns the panel, the clicks, the keys and the Previous | Next row); this window supplies `media_viewer_ops` — cells addressed by `MediaCell.idx` (grid order, append-only), a caption, and the "Show in source note" action link, whose render op re-reads the note's blob at panel size rather than upscaling the thumbnail. Getting to the note is that LINK under the image's bottom-right corner, NOT a double click — the first press of a double click dismisses the panel its second press was aimed at, and before the modal design the same collision made a double click open the wrong note (the in-grid expand reflowed the grid under the pointer). A GtkLabel carrying links owns an input-only window and takes the press before the enclosing event box, so the link works despite the click-to-close — the panel checks the link's allocation anyway rather than trusting that (`img_hit`). `media_viewer_action` calls `on_editor_window_open_image()`. `media_add_cell` calls `on_image_viewer_nav_sync` when a cell lands directly after the one on show — a greyed-out "Next" going live mid-scan, the only nav change a scan can cause |
| `src/settings_window.[ch]` | List density, sidebar counts, code copy/line-number toggles, first-line-H1, image viewer, native macOS menubar, database location |
| `src/export.[ch]` | HTML + Markdown export (all notes mirroring folder tree, or single note) |
| `src/cli.[ch]` | Headless subcommand interface (runs before GTK in main; tags/folders/notes CRUD, backup, export); folders by path, notes by id. Agent-ready surface: `note cat [--md]` (plain text from the body_text cache / Markdown via `on_export_note_markdown`, images as `![image N]()` placeholders), `note append`/`note set` (plain text in; `set` REPLACES content and clears the tag links), `search TEXT [--regex]` (case-insensitive titles+bodies via `on_db_note_body_map`, prints id/modified/path; TEXT takes the same operators as the search window — ANDed words, "quoted phrases", -exclusions — so quote the whole query for the shell, and an empty one exits 2), `note tags`/`tag`/`untag` + `tag notes` (`note tag` appends the literal `#name` span under the on-tag text tag and rewrites note_tags from the buffer, so GUI saves keep it), `note restore`, `action list/show/done/undone/due/text` (items addressed `NOTEID:ORD` **or** by stable `UID` — `action_token_parse` tells them apart BY SHAPE, a ':' means the positional form, a bare decimal means a uid; `action list --uid` prepends the uid as a further FIRST column, leaving the default output byte-identical because text must stay last, and an unknown flag exits 1 so a caller can probe an older build; done/due/text rewrite the '!' line via the on_editor_action_* helpers — headless OnApp has editors==NULL so they take the offscreen path.  `action show UID` prints ONE item as the same UID-first record `list --uid` emits (`action_print_line` is the one definition of that layout) — the O(1) read-back a mirror needs for a pinned item, instead of listing and diffing the whole table.  `action text UID TEXT` RENAMES an item: `action_text_ord` replaces only the span `on_action_split_due` marks off, so the '!' prefix, the line's own spacing and any trailing `due <date>` survive, and the new run is given the old text's strike state explicitly (a plain insert would inherit it from the preceding character).  The uid survives a rename on BOTH paths — the editor's identity mark sits at the LINE START, outside the replaced span, so `action_marks_prune` leaves it alone and the hint pass re-matches; headless, the ord pass does, since a rename moves no line.  A blank or newline-containing text is REFUSED (either would stop the line being that item and retire its uid), and a new text ending in a parseable `due X` sets the due date, exactly as typing it in the editor would); `note new/append/set` and `action text` all accept `-` = stdin (shipped over the socket by `on_cli_command_reads_stdin`).  **GUI parity (2026-09)**: `folder info/rename/move/emoji/ai-mode/sort/restore`, `note info/pin/unpin`, `note list --recent|--pinned`, `trash list`/`trash empty --yes`, `stats` — all thin wrappers over db functions the GUI already used.  Two things about them: `folder restore` takes an ID, not a path, because `on_db_folder_list` filters trashed rows so a trashed folder's path no longer resolves (`trash list` is where the id comes from); and `trash empty` REFUSES without `--yes`, being the one CLI command with no undo.  `folder sort` and the GUI's Sort Subfolders Alphabetically are now ONE function, `on_db_folder_sort_children`.  **Images out**: `note images ID` lists ord/bytes/WxH (dimensions from each PNG's header via `on_png_probe_size` — nothing is decoded) and `note image ID N FILE` writes one out through `on_note_image_nth_png`, i.e. the stored bytes VERBATIM, never decode+re-encode.  N is **1-based** on both, matching the `![image N]()` placeholders `note cat --md` writes.  **`--json`** on every record-printing command (note list/info/cat/tags/images, folder list/info, tag list/notes, action list/show, search, trash list, stats): one array, or one object for the single-record commands.  The reason is escaping — a title or action text may contain a tab, which silently shifts the plain form's columns.  `cli_json_take` strips the flag from a COPY of argv before dispatch and only for commands `cli_json_capable` names, so a note whose content is literally `--json` still survives `note new`; it ASSIGNS `cli_json` every dispatch, since inside a GUI instance serving remote calls the static outlives the command.  JSON records carry more than the plain ones (path, folder_id, created, pinned; the action uid always) — a JSON note listing must therefore always be given the folder-path map, which is what `cli_paths_for` is for |
| `icons/` | custom PNG toolbar icons + `composition.png` app logo (window icon + About dialog, and the .icns/Linux hicolor packaging icon), loaded by basename; see icons/README.md |
| `tools/import-apple-notes.sh` | Apple Notes migration (AppleScript export → CLI import; keeps modification dates) |

## Data & formats

- DB: `~/.local/share/notes/notes.db` (GLib user-data dir; the filename is
  `ON_DB_FILENAME` in db.h).  The dir went `orange-notes` → `blue_notes`
  pre-release → `records` → `notes` (2026-08), and the file `notes.db` →
  `records.db` → `notes.db` again.  **Do not rename either again without a
  shim**: user data lives there, and a rename with no pickup path looks
  exactly like "my notes vanished".  The 2026-08 rename shipped one
  (`on_db_adopt_legacy_path()`: rename a leftover `records.db` into place,
  either beside the expected path or from the old default dir); it was
  REMOVED days later once the sole user's database had been adopted, the
  same way the pre-1.4 `notes.db` shim went in 2026-07.  Consequence to
  know before opening an old backup: a `records.db` is no longer picked
  up — rename the file to `notes.db` by hand.
- Note content: **BNBF v5** blobs (magic `BNBF`; the pre-rename `ONBF`
  magic was retired 2026-07 after an offline scan found zero such blobs)
  (see header comment in `serialize.h`).
  TEXT records = styled runs (flag bits ↔ named GtkTextTags via one
  shared table); IMAGE = full-resolution PNG + display width; TABLE;
  CHECK. All older versions (1–4) still parse.
- **A blob can be rewritten WITHOUT deserializing it**, and a format
  migration MUST be written that way: a full deserialize decodes every PNG
  in the database (seconds per hundred MB, and the DB is ~600 MB).  Walk
  records like `on_note_extract_text` does, copy image payloads verbatim,
  change only the flag words you mean to, preserve the blob's own version
  field, and return "nothing changed" so untouched notes are never
  rewritten.  Write back through a content-ONLY UPDATE — a formatting
  cleanup must not bump `updated_at`, or the whole library reorders
  itself.  The 2026-08 first-line-H1 strip
  (`on_note_strip_first_line_h1` + `on_app_first_line_h1_strip` +
  `on_db_note_set_content`) was the worked example; all three were removed
  after it ran on the sole user's database and found zero notes to change
  (verified independently: 0 of 1296 notes carried any paragraph style on
  line 0).  `git log` has it if the shape is ever needed again.
- **`PRAGMA user_version` 4 IS CONSUMED** even though nothing claims it any
  more: the removed H1 strip stamped it, so the user's database sits at 4
  while a freshly created one stops at 3.  The next one-time migration must
  therefore be **5 or higher** — numbering it 4 would silently skip the one
  database that matters.
- **Task checkboxes are GtkTextChildAnchors** carrying their state as
  object data (`on_anchor_set/is_checkbox`), rendered as native
  GtkCheckButtons (BNBF v5 CHECK records).  A task line = anchor + space
  + text under the on-list-check paragraph tag.  (The pre-v5 glyph
  format and its load-time migration were removed 2026-07 after a blob
  scan verified zero glyph notes remained.)  Like all anchors,
  copy/paste within a note drops the widget/state.
- **Images are GtkTextChildAnchors**, not pixbufs: the anchor carries the
  original pixbuf + display width as object data
  (`on_anchor_set_image/get_image`). The editor attaches a HiDPI-aware
  GtkImage (pixels scaled × scale-factor, cairo surface with device
  scale) at each anchor; export/search/thumbnails read anchor data
  offscreen and never need widgets. Default thumbnail display fits a
  200×125 box (`ON_IMAGE_THUMB_W/H`, aspect kept, never upscaled);
  right-click menu toggles thumbnail/full.
- ALL UI settings live in the ini (`[notes]` group), loaded into
  memory ONCE by `on_app_config_init()` and written through on change
  (`on_app_config_get/set`); the file is never re-read while running.
  The ini normally sits NEXT TO THE BINARY (portable mode); when no
  binary-adjacent ini exists AND that directory is unwritable (system
  installs: .deb/.rpm in /opt, .app in /Applications) it falls back to
  `~/.config/notes/notes.ini` instead.
  On first launch (no ini) it is seeded from `notes.ini.defaults`
  next to the binary (committed; empty `db_dir` = default DB location).
  The live ini is gitignored — its rewrites drop comments and carry
  per-machine values.  (It was NOT actually ignored until 2026-08: the
  .gitignore still named the pre-2026-07 `blue_notes`/`blue_notes.ini`, so
  the binary and a machine-specific `db_dir` were committed.  A rename must
  update .gitignore too.)  The rename's ini pickup (`config_adopt_legacy()`,
  which republished every `[records]` key under `[notes]`) was REMOVED once
  the sole install had been adopted — a stray `records.ini` is now simply
  ignored, which means the app falls back to `notes.ini.defaults` and loses
  the configured `db_dir`.  That is the failure mode any future rename of
  this file must budget for.
  Keys: `db_dir`, `code_copy_button` (`1|0`),
  `code_line_numbers` (`1|0`), `native_menubar` (`1|0`),
  `db_integrity_check` (`1|0`, default 1 — run `PRAGMA integrity_check` and
  `PRAGMA foreign_key_check` against the database at startup, warning in a
  dialog if either reports problems; see `startup_integrity_check()` in
  main.c.  This was ONCE a file-hash comparison instead — a `db_hash` key
  snapshotted at clean exit, compared against an `app->db_hash_at_open`
  taken before `on_db_open`, offering Open Anyway / Run Integrity Check when
  the file had changed underneath.  All of that is GONE from the code; a
  leftover `db_hash=` line in an existing ini is simply ignored, and nothing
  writes one any more),
  `sidebar_counts` (`1|0`, default 0 — folder/tag counts in the
  sidebar), `first_line_title` (`1|0`, default 1 — treat line 0 as the
  note's title: the editor CENTERS it and renders it heading-sized,
  unconditionally and whatever it holds, so a line becomes the title just
  by landing on line 0 (delete the title line and the body line under it
  takes over).  Both are EDITOR-ONLY derived tags (`on-title-center`,
  `on-title-size`), never serialized — nothing is written into the note,
  which is exactly what lets the setting turn the look off again.  Off = a
  plain left-aligned body line; line 0 still supplies the note title
  either way.  Applies live via `on_editor_title_refresh_all`.  The old
  auto-H1 behaviour (real, SERIALIZED H1 on the first line of a brand-new
  note, `ed->auto_h1`) was removed with this — it could not be undone by
  the setting, and it stacked with the derived scale.  A stored H1 on line 0
  (applied by hand, or by a build predating all this) is harmless: the
  derived size stands down there (see quirk 20), so it looks identical —
  the only tell is that turning the setting OFF leaves that one line big.
  Replaced the old
  `first_line_h1` key 2026-08 — a stale `first_line_h1=` line in an
  existing ini is simply ignored),
  `compact_editor_toolbar` (`1|0`, default 1
  — collapse the editor's H1/H2/¶ buttons into an "Aa" Styles menu
  button and the list buttons into a "≡" Lists one; applies live via
  `on_editor_rebuild_toolbars_all`), `touch_assist` (`1|0`, default 0 =
  DISABLED — GTK's touch aids: the teardrop drag handles under text
  selections/the cursor, the selection magnifier, and the tap
  cut/copy/paste bubble, which some Linux input stacks (VM tablets)
  show for plain mouse input; GTK3 has no API for any of them.  Two
  levers: CSS in `on_app_apply_touch_assist` hides handles + magnifier
  live (`cursor-handle` collapsed, `popover.magnifier` transparent —
  the bubble can't be CSS-hidden: invisible-but-clickable buttons), and
  main() sets GDK_CORE_DEVICE_EVENTS=1 before GTK init, which stops the
  touchscreen classification driving all three — restart to change;
  costs XI2 smooth scrolling; no-op off X11), `image_viewer` (program
  path; unset = system default),
  `media_win_w`/`media_win_h` (last media-window size, same contract as
  the search window's),
  `search_win_w`/`search_win_h` (last search-window size, the default
  for the next one), `editor_win_w`/`editor_win_h` (default editor
  window CLIENT size, 640×509 when unset — read at editor open only,
  deliberately NOT written back on resize, unlike the search window's),
  `statusbar_db_path` (`1|0`, default 1 — prefix the
  folder path in the library/editor status bars with the DB file's path,
  formatted by `on_app_location_text`; applies live from Settings),
  `statusbar_note_id` (`1|0`, default 0 — show "id:N" at the right edge
  of each editor's status bar; the label is no-show-all so its updater
  owns visibility; applies live via `on_editor_status_refresh_all`),
  `show_done_actions` (`1|0`, default 1 — list completed items in the
  library's Action Items view; hidden mode also drops a row the moment
  its checkbox is ticked; applies live via the full notify),
  `list_columns` (list-view column layout,
  `key:vis` pairs in display order, default
  `path:0,title:1,modified:1,created:0`
  — written on every header drag/toggle, applied at window
  construction; right-click a column header for the show/hide menu),
  `list_autofit` (`1|0`, default 1 — same header menu: Path/Modified/Created always show
  their FULL content, Title takes the ellipsis + expand and is the one
  column that truncates.  Implemented by MEASURING content with a
  PangoLayout after every refresh (`list_autofit_apply`) and setting
  FIXED widths — NOT with GTK_TREE_VIEW_COLUMN_AUTOSIZE, which is
  unusable: columns cache resized/requested widths that override it
  (never shrinking back), and ellipsizing renderers report a ~3-char
  minimum so ellipsized columns collapse instead of fitting.  Manual
  resize grips come back when it's off),
  `list_density_comfortable` (`1|0`, default 1 — Comfortable list
  density: tall rows with a bold title and a small dimmed preview of the
  first body-text line after the title, rendered via `notes_title_cell_func`
  using Pango alpha rather than a fixed colour so the preview stays
  readable on the selection highlight; applies live via
  `notify_notes_changed`). The old DB settings table is
  GONE (dropped from the schema and the live DB 2026-07); all
  preferences live in the ini.
- **Custom DB location** (shared-folder support) lives in the CONFIG
  FILE `notes.ini` NEXT TO THE BINARY (`[notes] db_dir=`;
  resolved from argv[0] by `on_app_config_init()`, which must run before
  any config read — main() calls it first thing), never in the DB.
  `on_app_switch_database()` switches live: closes all editors (flushing
  saves), swaps the handle, copies the current file to the target if no
  notes.db exists there (or overwrites it at the user's choice);
  either way the original file is deleted on success, persists, refreshes
  the library. Failure reverts to the old DB. If the configured DB can't be opened at
  startup, main.c ERRORS OUT — deliberately NO fallback to the default
  location: a silent fallback once made a user's notes "disappear" and
  strands writes in the wrong file (the trigger was a relaunch racing
  the dying instance's final flush past the 5 s busy timeout). One
  configured database, or a clear error. When no notes.db EXISTS at
  the expected location (first launch / emptied dir),
  `startup_first_run()` asks — "Open a notes.db File" (persists the
  new db_dir) or "Create a New notes.db" — instead of silently creating
  an empty DB.  **RENAMING OR MOVING THE DB DIRECTORY BY HAND puts you
  here**: `db_dir` still names the old path, nothing is found, and the
  Welcome dialog appears — answering "Create a New notes.db" at that
  point leaves the real database orphaned at its new path.  Fix the
  `db_dir` line in the ini (or use "Open a notes.db File", which
  persists it) rather than creating one.
- **Folder emoji** (`folders.emoji` TEXT, default `''`): each folder
  can have an optional emoji set via its Info dialog (right-click a
  folder → Info, or via New Folder). GTK's built-in emoji chooser opens
  on click; the chosen emoji is stored in the DB and displayed as a
  sidebar prefix with a two-space separator (`🎉  Folder Name`).
  `on_db_folder_get/set_emoji` in db.c read/write the column;
  `add_folder_rows()` in library_window.c builds the display string.
  A migration (`ALTER TABLE folders ADD COLUMN emoji TEXT NOT NULL
  DEFAULT ''`) backfills existing databases transparently.
- **Trash is a soft-delete flag, not a folder**: `notes.trashed` /
  `folders.trashed` columns + the `trash_folder_ids` view (recursive
  closure — only the TOP deleted folder is flagged; its subtree stays
  attached and is implicitly trashed via the view). folder_id/parent_id
  are untouched by deletion — they ARE the restore location; restore
  clears the flag and re-parents to top level only when the original
  location is itself still trashed. Moving notes (`on_db_notes_move`)
  always clears the flag (drag out of Trash = restore-to-folder). All
  normal listings/counts filter through `NOTE_VISIBLE_SQL`; search's
  All-scope uses `on_db_note_list_all(db, TRUE)` to keep deleted notes
  findable; export/CLI pass FALSE. CLI note/folder delete TRASHES by
  default (one bulk trash call for notes); `--permanent` deletes
  outright and `note restore` un-trashes — safe for agent use.
  Sidebar: "Pinned Notes" on top (only while any are pinned; the
  selection-restore fallback reads the FIRST row's kind from the model
  rather than assuming All Notes), then "All Notes" (`SB_KIND_ALL`,
  newest-first), then "Action Items", then the folder tree and Tags,
  and the "Trash" section at the bottom only while non-empty
  (`SB_KIND_TRASH`, trashed folders as `SB_KIND_TRASH_FOLDER` children;
  `on_db_folder_list_trashed` fed those rows through
  `run_folder_query`, which reads its columns POSITIONALLY as
  `FOLDER_COLS` — the query hand-wrote a five-column list instead, so
  every trashed folder showed its parent's id as its name and its real
  name as its emoji, in the sidebar as well as in `trash list`.  Fixed
  2026-09 by using the macro: never hand-write that column list);
  in trash views the Delete paths turn permanent (with confirm) and the
  note menu becomes Open/Restore/Delete Permanently; GUI note/folder
  deletes elsewhere just trash with a status message, no dialog.
- **Action items are '!' lines**: a line whose FIRST character is '!'
  (outside code blocks; anchors occupy the first slot like a character)
  is an action item — text = rest of line trimmed, DONE = the whole rest
  struck through (ON_FMT_STRIKE, which serializes; that is the persisted
  state).  The one definition lives in `on_note_extract_actions`
  (serialize.c, a cheap record walk like body_text extraction).  The
  editor tints action lines blue via the derived, editor-only
  `on-action` tag (priority 0 so #tags keep their orange; re-derived on
  insert/delete/paragraph-format/load/undo like the emoji padding) —
  nothing about "actionness" is stored in BNBF.  The `action_items`
  table (note_id/ord/text/done/due/uid, ON DELETE CASCADE) is a queryable
  mirror like note_tags: editor_save rewrites it only when the
  extracted set differs from `ed->last_actions` (full library notify
  then, light otherwise); CLI saves sync unconditionally; a one-time
  backfill for pre-feature notes is gated by `PRAGMA user_version`
  (`on_app_actions_backfill`, run after every long-lived `on_db_open` —
  GUI start, CLI, db switch).  Library: "Action Items" sidebar
  row directly under All Notes, ABOVE the folder tree (visible while
  items exist; optional count = OPEN items) shows a third notes-pane
  stack child ("actions"): untitled checkbox column + "Action" text
  column (done rows struck).  Toggling
  writes the db row, then `on_editor_action_set_done` strikes/un-strikes
  the '!' line's text — in the live buffer + autosave when the note is
  open, offscreen blob rewrite otherwise (ord = position among the
  note's REAL action lines; bare "!" lines don't count).  DUE DATES live
  in the line text as a trailing "due <date>" — ISO "YYYY-MM-DD" is the
  written form, the parser (`on_action_split_due`, shared by extractor
  and editor like on_list_prefix_chars) also reads "M/D/YY[YY]"; the
  LAST word-boundary "due" that parses wins, so "send due diligence
  report due 12/31/26" keeps its text.  A line that is only "! due X"
  is no item.  Double-clicking the Due Date CELL opens a GtkCalendar
  dialog → `on_editor_action_set_due` rewrites the suffix (appended
  text inherits the item's strike state so done items stay done);
  double-clicking elsewhere opens the owning note.  The view follows
  the notes list's column conventions — the layout machinery is
  view-generic (`view_columns_persist/apply`, config key + count +
  default carried as object data on the VIEW, header buttons carry
  "on-view"); `action_columns` ini key, default `done:1,action:1,due:1`;
  headers sort (Action alpha, Due soonest-first with undated last, Done
  by state).  The Due Date cell is tinted by urgency via a cell data
  func (draw-time, so it rolls over at midnight): overdue red, today
  dark yellow, ahead green — darkened for the striped row backgrounds;
  no-due rows must reset "foreground-set" (shared renderer).
  `due` is in the CREATE TABLE (so new databases have it immediately)
  and a guarded ALTER migration backfills existing databases on open.
- **An action item's STABLE identity is `action_items.uid`** — `ord` is a
  POSITION and shifts whenever a '!' line is added or removed, so it can
  never be a reference an external mirror (the Lists app) holds onto.
  The uid is assigned once, invisible to the user, and NOT stored in the
  note text (that was considered and rejected: a token in the prose
  leaks into `note cat`, both exports, the body_text search cache and the
  list previews, duplicates itself on copy/paste, and is deletable by
  ordinary editing).  Since `on_db_note_set_actions` rebuilds a note's
  rows DELETE-then-INSERT, an AUTOINCREMENT column would be reissued on
  every save; instead the OLD rows are read first and matched against the
  new set in four passes, strongest evidence first — identical text (an
  item that only moved), then the live editor's per-line mark hint (the
  ONLY signal that survives a reword), then the same ord (the headless
  reword), then a fresh id from the `action_uid_seq` one-row high-water
  mark (only ever incremented, so a retired uid is never handed out
  again).  Each pass completes before the next, so a strong match cannot
  lose its row to a positional guess made earlier in the list.  The
  editor keeps one `GtkTextMark` per action line carrying that item's uid
  (`ed->action_marks`, seeded at load, re-placed after any save that
  rewrote the table); marks ride edits to the surrounding text, which is
  what makes a rewording keep its identity.  They are PRUNED in the
  delete-range BEFORE handler (`action_marks_prune`) — GtkTextBuffer
  would otherwise collapse a deleted line's mark onto the FOLLOWING
  line, where it would confidently misidentify a different item; pruning
  even on internal changes matters because an undo replaces the whole
  buffer.  A cut-and-paste reorder therefore loses its mark and is caught
  by the text pass instead: the two signals cover each other's blind
  spot.  Migration: `uid` is in the CREATE TABLE plus a guarded ALTER,
  and `on_app_action_uids_backfill` (user_version 3) fills existing rows.
  That backfill ALSO re-runs whenever any row has uid 0 — an older build
  writing to an already-migrated database inserts without the column, and
  the version stamp alone would leave those rows unidentified forever;
  the probe is an indexed existence check, so the normal case is free.
  NOTE the index on `action_items(uid)` is created AFTER the ALTER
  migrations (with the Trash view), never in the schema string: indexing
  a column that does not exist yet fails the whole batch, which makes
  `on_db_open` return NULL and the app refuse to start on every existing
  database.
  `grid_pref` restores list/grid when leaving the view.
- **CLI backup**: `on_db_backup_to()` (db.c) uses SQLite's online backup
  API on the live DB; exposed as `notes backup FILE.db`. No GUI
  equivalent — the File menu backup/restore items were removed.
- **CLI ↔ GUI coexistence is socket-based, not lock-based**: a running
  GUI serves later CLI invocations over a unix socket (`src/ipc.c`), so
  the two never write the DB concurrently. The old in-DB `in_use`
  instance lock and the read-only mode (`app->read_only`, `PRAGMA
  query_only`, `on_app_db_acquire/release`) were REMOVED with that
  change. SIGTERM
  (pkill) destroys all windows so editor autosaves flush and the loop
  ends cleanly.

## Hard-won GTK3 quirks (do not re-learn these)

1. **Text-window children are BUFFER-anchored.** Children added via
   `gtk_text_view_add_child_in_window(GTK_TEXT_WINDOW_TEXT)` take their
   position in buffer coordinates — they ride scrolling at 1x on their
   own. The view's top margin is re-added ONLY on the initial
   allocation of a freshly added child; positions set later via
   `gtk_text_view_move_child()` land as-is (verified on screen), so add
   at 0,0 and position everything through move_child with plain buffer
   coordinates. Probing tip: use `gdk_window_get_origin` on a realized
   window — offscreen pixel-scans do NOT composite these children.
2. **Never reposition those children on scroll.** A `move_child()`
   issued while scrolled doesn't take effect until the next
   validate/allocate cycle, so scroll-driven "corrections" (especially
   ones computed via `buffer_to_window_coords`, which double-apply the
   scroll) land late and misalign the widget by the scroll delta.
   Reposition only when content or geometry changes: rebuild on buffer
   changes (idle-coalesced) and view `size-allocate`. No off-screen
   hiding is needed — they scroll and clip naturally.
3. The floating copy button must be **pinned to an exact size in CSS for
   all states** (`button, button:hover, button:active { min-width/height;
   padding:0 }`) — theme hover styling otherwise changes its allocation
   and it jumps under the pointer. Position math uses the constant
   `CODE_BTN_SIZE`, never live allocations.
4. Anchor the button's y to `gtk_text_view_get_line_yrange()` (line top =
   where paragraph-background shading starts), not the char rect (which
   sits below pixels-above-lines).
5. **Retina blur**: raw pixbufs render 1 buffer-pixel = 1 logical px.
   Anything that must be sharp goes through cairo surfaces with device
   scale: editor images, grid thumbnails
   (`cairo_surface_set_device_scale`, list-store column type
   `CAIRO_GOBJECT_TYPE_SURFACE`, icon-view pixbuf renderer bound to the
   "surface" attribute), and toolbar icons (`on_app_icon_image_sized`
   rasterizes SVGs at size × monitor scale factor and wraps them via
   `gdk_cairo_surface_create_from_pixbuf`).
6. **Toolbars are ICONS-ONLY, and that is not a setting.** Every toolbar
   sets `GTK_TOOLBAR_ICONS` at build time; buttons come from
   `on_app_tool_item_new` (icon file, or a Pango-markup glyph as the
   icon_widget when there is no file). GtkToolbar natively supports
   TEXT/ICONS/BOTH and the app once exposed all three per family
   (`ON_TOOLBAR_LIBRARY/EDITOR`) as two Settings dropdowns, two ini keys
   (`toolbar_style_library`/`toolbar_style_editor`) and a right-click
   style menu on any toolbar, all backed by a registry of live toolbars in
   `OnApp`. ALL of that was removed 2026-09 — text-mode toolbars were
   never what the app wanted, and the icons carry the design. Stale
   `toolbar_style_*` lines in an existing ini are simply ignored. `git log`
   has the registry if a live restyle is ever needed again.
7. **Editor letter buttons (B/I/U/S) are markup glyphs on purpose** —
   elementary's symbolic SVGs are 16px light-grey and look fuzzy/washed
   next to Pango-rendered glyphs. Icon field NULL → fallback markup is
   the primary look. H1/H2/¶/•/1./{ } are glyphs too.
8. Multi-row drag to a folder: GtkTreeView drags a single
   GTK_TREE_MODEL_ROW; on drop, if the dragged note is in the current
   multi-selection, move the whole selection.
9. Paragraph-style tags must cover the trailing newline (see
   `line_span()`) so typing at line end inherits them; list items carry a
   literal "• "/"N. " prefix plus an indent tag; Enter continues lists,
   Enter on an empty item ends them; numbered blocks renumber.
10. Inline typing follows `ed->inline_flags` (word-processor model),
    enforced in the after-handler of `insert-text` for insertions ≤2
    chars (longer pastes keep their own tags).
11. **Clearing a tree/list store zeroes its view's scrollbar.** Every
    model rebuild (refresh_sidebar, refresh_notes) must capture the
    scrolled window's vadjustment value first and restore it via
    `scroll_keep_queue()` (idle-deferred so the rebuilt view re-validates
    its height before the value is clamped). The sidebar always
    restores; the notes pane only when re-showing the same selection
    (`shown_kind/shown_id`) so navigation still starts at the top.
12. **Emoji padding is macOS-only** (`#ifdef __APPLE__` in
    `tag_emoji_in_range`): Apple Color Emoji draws wider than its Pango
    advance, so emoji get an editor-only letter-spacing tag (self + the
    following char, since Pango splits spacing half-per-side at run
    edges). Linux emoji fonts fit their advance — the pass compiles to a
    no-op there. The only other platform-specific code is the
    HAVE_GTKOSX menubar integration; everything else is portable GTK3.
13. **A custom GTK_TREE_MODEL_ROW drop handler must own the WHOLE dest
    protocol.**  GtkTreeView's default `drag-motion` handler validates
    row drops by requesting the drag DATA on every motion
    (`set_status_pending` + `gtk_drag_get_data`), so
    `"drag-data-received"` fires repeatedly MID-DRAG.  On quartz the
    reply arrives before the release, so a received-handler that treats
    every delivery as a drop runs with stale coordinates (0,0 → the top
    sidebar row) and `gtk_drag_finish()`es the drag while the button is
    still down — drops only land when an X11-style late reply slips
    past the release.  Fix (see the sidebar in library_window.c):
    connect `drag-motion` (compute + validate the target yourself,
    `gtk_tree_view_set_drag_dest_row` + `gdk_drag_status`, return TRUE
    to block the class closure), `drag-leave` (clear the indicator),
    and `drag-drop` (request the data, return TRUE); then
    `drag-data-received` fires exactly once, at drop time, with real
    coordinates.  Costs the built-in drag auto-scroll/auto-expand.
14. **`gtk_tree_view_expand_all` after every model rebuild re-expands
    folders the user collapsed.**  refresh_sidebar snapshots the
    expanded rows before the clear (keyed kind+id — paths shift when
    folders move) and restores that state in its selection-restore walk;
    only the first population expands everything.  Custom drag icons go
    on with `g_signal_connect_after("drag-begin")` — the class handler
    sets its own row-snapshot icon in the class closure, so a normal
    connection gets overridden.

15. **GTK 3.24's GtkTreeView collapses a multi-selection on PRESS.**
    Its multipress gesture does CLEAR_AND_SELECT on any unmodified
    primary press — no drag deferral — so dragging a multi-selection is
    impossible out of the box (GtkIconView is fine: it defers via
    `last_single_clicked`).  You can't just consume the press: the
    multipress AND row-drag gestures both run in the BUBBLE phase, so a
    TRUE from a button-press handler kills drag initiation too.  Fix
    (notes list): on press over an already-selected row with ≥2
    selected, install a veto select-function; a drag-begin lifts the
    veto keeping the selection, a plain button-release lifts it and
    applies the collapse via gtk_tree_view_set_cursor.

16. **GtkTreeView type-ahead search auto-picks a useless column**:
    `gtk_tree_view_set_model` sets the search column to the first model
    column transformable to string — our stores lead with the int64 id,
    so typing in a focused view popped a search box that matched
    nothing.  Every tree view disables it
    (`gtk_tree_view_set_enable_search(view, FALSE)`); to bring it back
    usefully, point `gtk_tree_view_set_search_column` at a text column
    (e.g. NL_TITLE) instead.

17. **Anchored children sit with their BOTTOM on the text baseline**, so
    a widget taller than the font's ascent (the task checkboxes) rides
    visually high next to its line's text.  Widget margins cannot be
    negative and CSS padding on the `check` node can only move the
    indicator UP relative to the baseline, never down — the working
    lever is a negative `rise` on a GtkTextTag covering the anchor
    CHARACTER (editor-only `on-check-drop` tag, −3 px, applied in
    attach_checkbox_widget): GtkTextView honors Pango rise when placing
    child segments.  A theme-padding-stripping CSS pin stays on the
    button itself so the box is the bare indicator on themes that do
    pad it (macOS Adwaita already doesn't).

18. **`notify::cursor-position` fires INSIDE the insert-text class
    handler** — after the character lands in the buffer but BEFORE any
    after-handlers run.  So a cursor-moved handler that adopts the style
    of the char left of the cursor reads the brand-new, still-untagged
    character and wiped `ed->inline_flags` before the insert
    after-handler could apply it (broke arming bold with no selection:
    Ctrl/Cmd+B, then type).  Fix: an insert-text BEFORE-handler sets
    `ed->typing_insert` for ≤2-char (typed) insertions; on_cursor_moved
    skips style adoption while it's up; the after-handler clears it.
    Real navigation (clicks, arrows) still adopts.

19. **A tag can't style an EMPTY line, so the caret there can't be styled
    by tags at all.**  `gtk_text_buffer_apply_tag` over a zero-length span
    is a silent no-op — which is why the old auto-H1 re-applied itself per
    keystroke instead of pre-styling the line.  The caret's height and
    x-position on an unwritten line therefore come from the view's
    DEFAULTS, and the title line needs both (see `title_line_sync`):
    justification via
    `gtk_text_view_set_justification`, font size via a style class the
    function toggles on the view (`textview.on-title-empty
    { font-size: 160% }`, matching ON_TAGNAME_H1's 1.6 scale).  Enlarging
    the whole view is safe ONLY because it is done exclusively while the
    buffer is empty — an empty buffer IS line 0 and nothing else.  The
    class must select the `textview` node, NOT its `text` child:
    `gtk_text_view_set_attributes_from_style` reads the default font off
    the widget's own style context and takes only letter-spacing from the
    text node.  A line that is empty but has a NEWLINE (a blank first
    line) is the in-between case: its newline is the one character it owns,
    so spans there run THROUGH the newline while spans on a line with text
    stop before it — covering the newline would let the next line inherit
    the tag from text typed after it.

20. **GtkTextTag "scale" values MULTIPLY when tags overlap** —
    `_gtk_text_attributes_fill_from_tags` does `dest->font_scale *=
    vals->font_scale` per tag, which is why an H1 line that is also H2
    renders at 2.08x.  So a DERIVED scale tag must never be laid over text
    that might already carry a real one: `title_line_sync` applies
    `on-title-size` only where line 0 has no ON_FMT_PARA_MASK style of its
    own (H1 there is already title-sized; H2/code/list is a style the user
    chose).  Weight and justification don't compound this way — only
    scale does.

21. **What `gtk_window_move()` positions is PLATFORM-DEPENDENT, and window
    gravity is not the way out.**  Measured on GTK 3.24: on quartz the
    coordinate is the CLIENT origin (the frame extends the titlebar's 28 px
    ABOVE it — `gdk_window_get_frame_extents` on a realized-but-unmapped
    window reports `y = -28`), while X11's documented behaviour is the
    frame's top-left.  `GDK_GRAVITY_SOUTH_EAST` looks like the fix —
    "move the bottom-right corner to this point" — and IS honoured on
    quartz, but GTK computes it from the client size it knows before
    mapping, so a request for `corner − 12` landed the frame flush IN the
    corner with the margin silently swallowed.  Reliable recipe (see
    `editor_place_bottom_right`): keep default gravity, move using the
    CLIENT size for the first placement, then correct ONCE in a `map-event`
    handler from the real `gdk_window_get_frame_extents` — shift by the
    leftover delta via `gtk_window_get_position` + `gtk_window_move`, which
    share a coordinate space whatever the convention, and disconnect the
    handler.  On macOS the residual is 0, so nothing visibly moves.
    Positions must be measured against `gdk_monitor_get_workarea`, never
    the monitor rect: the work area already excludes the menu bar, Dock and
    Linux panels.

23. **A GtkTextView that loses the focus renders its text GREY, and on
    quartz that grey rendering OUTLIVES the thing that took the focus.**  The
    modal image viewer (`image_viewer.c`) used to `gtk_widget_grab_focus()`
    its own panel so its keys would reach the host window's handler.  Closing
    it then left the whole editor's text grey — looking exactly like an
    unfocused window — until the user clicked to another window and back.
    Measured with the panel closed: GTK reports the focus restored to the
    view (`state-flags 0x80 -> 0xa0 FOCUSED`), resolves the text colour to
    pure black, and emits a FULL-CLIP draw.  The screen keeps the grey
    anyway.  Nothing at the GTK level clears it — not
    `gtk_widget_queue_draw()` on the view, not
    `gdk_window_invalidate_rect(win, NULL, TRUE)` on the toplevel, not
    `gtk_widget_reset_style()` on either the view or the window; only
    SCROLLING (which re-lays-out lines, and fixes it region by region as you
    go) or a real toplevel activation change.  Grey is provably the BACKDROP
    colour: the same instrumentation shows `state=0xc0 BACKDROP` resolving to
    `0.20,0.20,0.20` while `FOCUSED` gives `0.00,0.00,0.00`.
    **So do not repair it — do not create it.**  The panel is
    `can_focus FALSE`, never grabs the focus, and the host window's
    `key-press-event` handler (which runs before GtkWindow forwards to the
    focus widget) serves its keys off whatever still has focus.  The host
    then MUST swallow every other key while the panel is open, or typing
    would edit the note blind behind it.  The corollary that bit next:
    **nothing inside such a panel may be a GtkLabel `<a>` link**, because a
    link label owns an input-only window and grabs the focus when clicked —
    reintroducing the bug on the first Next.  Hit-test plain labels from the
    panel's own button-press handler instead (same remedy as quirk #22),
    and serve their hover cursor from the panel's `motion-notify-event`
    (`img_motion` -> `img_cursor`): the panel's event box genuinely owns
    its GdkWindow, so unlike quirk #22's text window there is no rival
    owner — but it must be given `GDK_POINTER_MOTION_MASK` explicitly,
    since a GtkEventBox asks only for BUTTON_MOTION.  `img_nav_delta` is
    THE one reading of the nav row, shared by the press and the cursor,
    so the pointer can never promise a step a click will not deliver.

24. **A row of two small GtkLabel links has dead gaps, and a click in one is
    silently lost.**  The viewer's "Previous | Next" started as ONE GtkLabel
    carrying both `<a>` links; a press landing on the label but in the
    spacing, on the separator, or just past a word's last glyph activated
    nothing and did not close the panel either, so stepping through images
    intermittently took two clicks.  Word-exact hit-testing has the same
    hole.  The row is now split by MIDLINE — the whole row is claimed, left
    half steps back, right half forward — so there is no gap to miss and a
    near-miss can never dismiss the picture.

22. **The text window's CURSOR has exactly one owner, so a clickable
    region inside a GtkTextView must be served from the VIEW's own
    handlers, not from a wrapper widget.**  The code blocks' "copy" links
    were once a GtkLabel inside a `gtk_event_box_set_visible_window(FALSE)`
    GtkEventBox whose realize handler set the "pointer" cursor, on the
    theory that the event box owns a GdkWindow the cursor could be scoped
    to.  It does not: `visible_window` FALSE clears the widget's
    has-window flag, so `gtk_widget_get_window()` returns the PARENT —
    the view's bin window (verified: `has_window=0`, and get_window ==
    `gtk_text_view_get_window(GTK_TEXT_WINDOW_TEXT)`).  The realize
    handler was therefore setting the cursor for the WHOLE text area, and
    `on_view_motion_notify` (connected after, so it wins) put the text
    cursor straight back on the next motion — the hand cursor never
    appeared anywhere.  Motion reaches the view even over the event box
    because GtkEventBox's input-only window asks for BUTTON_MOTION_MASK
    only, never POINTER_MOTION_MASK.  So: the links are plain GtkLabels
    with no input window at all, and both the click
    (`on_view_button_press`) and the hover cursor (`on_view_motion_notify`)
    hit-test them via `code_link_at_view_pos`.
    Hit-test against the child's **ALLOCATION**: in-window children are
    POSITIONED in buffer coordinates (quirk #1), but GTK re-allocates them
    as the view scrolls (measured: `move_child(300,200)` with a 12 px top
    margin allocates at y=212, and at y=92 once scrolled 120), so an
    allocation is always in the same window coordinates `event->x/y`
    arrives in — no `window_to_buffer_coords` conversion, and correct at
    any scroll offset.  Consume the press (return TRUE) so the caret does
    not move under the link.

## Performance decisions

- The media browser never deserializes a note: counting a note's images
  is a record walk that skips PNG payloads, and each thumbnail decodes
  exactly one image at thumbnail size (`on_note_image_nth`, which caps
  the decode via the loader's size-prepared, so a 12 MP screenshot is
  never inflated).  The scan yields every 40 ms and keeps the current
  note's blob across yields, so a note with twenty screenshots is read
  from SQLite once.  Measured on the 1296-note/600 MB database: All
  Notes fills 500 thumbnails in ~30 s with the window fully responsive
  throughout.  The 500 cap is a MEMORY bound, not a speed one — every
  cell holds its own decoded pixels.
- Grid thumbnails render ONLY while grid view is visible (`want_thumbs`
  in refresh_notes; on_view_grid refreshes) — the thumb cache keys on
  updated_at, so without the gate the edited note re-rendered on every
  autosave.  And they render ASYNCHRONOUSLY: refresh_notes only sets
  thumbnails found fresh in the cache; every stale/missing one is queued
  as a ThumbJob (row reference + id + updated_at) and rendered by
  `thumb_fill_idle` in 40 ms time slices.  Rendering them inline once
  hung the GUI ~37 s (measured, 1266 notes / 617 MB): deleting a note's
  last #tag pruned the orphaned tag, the sidebar selection on that tag
  row fell back to All Notes, and the grid rendered every thumbnail —
  every PNG in the DB decoded — in one synchronous pass.
- Sidebar counts come from two GROUP BY maps (`on_db_note_count_map` /
  `on_db_tag_count_map`), not per-row COUNTs — per-query latency hurts
  on shared/network DBs.  The list view's Path column likewise reads
  `on_db_folder_path_map` (all folders in one query, paths built in
  memory), never per-note `on_db_folder_path`.
- Editor saves use the LIGHT notify (`app->notify_note_saved` →
  refresh_notes only): editing a note can't change folder counts, so
  the sidebar isn't rebuilt per autosave/close. The full
  `notify_notes_changed` (sidebar + notes) fires only when the save
  changed the note's tag set — tracked LIVE by `ed->tags_modified`
  (set in tag_capture_end / on_tag_row_activated on creation, the
  before-handler on delete-range when the doomed range touches an
  on-tag span, and the insert after-handler when typing inside one) —
  never by scanning the buffer at save time. note_tags is rewritten
  only when that flag is set. Create/move/delete run in the library,
  which refreshes itself directly; db switch uses the full notify.
- `ed->dirty` (set by editor_queue_autosave, cleared by editor_save)
  gates the close-time flush: closing a window whose last autosave
  already ran skips serialization entirely.
- **Images are never re-encoded — on save OR on export**: the PNG bytes
  are cached on the pixbuf as `"on-png"` GBytes (attached from the blob at
  full-res deserialize, or on the first encode of a pasted/inserted image),
  and `on_image_png_bytes()` is THE accessor — cache hit, or encode once and
  cache.  `on_note_serialize` and `export.c`'s `emit_image` both go through
  it, so the bytes written out are the ones the database holds.  Scaled loads
  (`on_note_deserialize_scaled`, thumbnails) skip the cache — their pixbuf no
  longer matches the bytes.  Before this, every autosave of an image-heavy
  note re-compressed every PNG on the main loop; the EXPORTER kept doing it
  until 2026-09 (`gdk_pixbuf_save`/`save_to_buffer` per image, for both the
  HTML data-URIs and the Markdown side-car files), which measured 3.15 s to
  export one note holding 20 screenshots and 0.71 s afterwards — the whole
  difference being PNG compression the database had already done.
- code_buttons_rebuild has a fast path: when block-start offsets match
  the existing buttons' marks, it only repositions (no widget churn per
  keystroke).
- Cross-note search reads the `notes.body_text` cache column (filled by
  every save via `on_note_extract_text`, a record-walk over the BNBF
  blob that skips image payloads entirely) — fetched as ONE query for
  the whole table (`on_db_note_body_map`), not per note: per-query
  latency is what hurts on shared/network DBs. NULL rows (pre-column
  saves) fall back to the extractor and write back. Measured: full cold
  extraction of 1260 notes / 616 MB of blobs = 183 ms; the warm path
  reads ~1 MB of text. The old path deserialized every note into a
  GtkTextBuffer, decoding every PNG, per search.
- Search runs on a worker thread (GtkSpinner in the window), never the
  GTK main loop. The worker opens its OWN SQLite connection (one
  connection must not cross threads); scope is resolved on the main
  thread first (it reads library widgets); results come back via
  g_idle_add. A SearchJob owns everything and frees itself on the main
  thread after checking its atomic `cancelled` flag — set when the
  window closes or a newer search starts, so it never touches a dead
  window. GRegex is immutable ⇒ compile on main (instant bad-pattern
  errors), match on worker.
- refresh_sidebar keeps its `populating` guard up through the
  selection restore, so the restore's select_iter can't fire the
  changed handler and rebuild the notes pane a second time — every
  caller pairs it with an explicit refresh_notes. If the old selection
  no longer exists it falls back to the root and refreshes the notes
  pane itself.
- Note deletes/moves go through the BULK `on_db_notes_delete` /
  `on_db_notes_move` (one transaction + one orphan-tag prune) — the
  old per-note variants fsynced per call, froze the GUI on big drops,
  and were REMOVED (pass `&id, 1` for one note).  The drop handler
  also calls `gtk_drag_finish` BEFORE its refreshes so the DnD
  handshake isn't stalled by the model rebuilds.  Autofit column
  measuring rides refresh_notes' population loop (one PangoLayout, one
  measurement per unique folder path, skipped while the grid is the
  visible view — on_view_list re-measures on switch); it never does a
  second model walk. The `#tag` autocomplete queries the tag
  list ONCE per capture (`ed->tag_choices`) and filters in memory per
  keystroke. `on_app_config_set` skips the ini rewrite when the value
  is unchanged. The startup/exit DB hash streams through `GChecksum`
  (never loads the file whole). Exports uniquify names within the run
  only, so re-exporting to the same directory overwrites (a mirror),
  not duplicates.
- Deliberately NOT done: WAL journal or synchronous=NORMAL pragmas —
  unsafe/risky on network filesystems, which the shared-DB feature
  targets.

## Environment gotchas

- Corporate TLS interception: MacPorts curl fails on github.com
  (self-signed cert in chain) — **use `/usr/bin/curl`** (macOS keychain
  trusts the proxy CA). gitlab.gnome.org and deb.debian.org work either
  way.
- clangd shows "gtk/gtk.h not found" diagnostics on every file — noise
  (no compile_commands.json); trust `make`, which builds `-Wall -Wextra`
  clean.
- The GUI can be launched in background for the user with
  `./notes & disown` after `pkill -f "./notes"`.

## Common task patterns

When making a targeted change, start by reading the files in the "Read" column,
then change the files in the "Change" column.

| Task | Read first | Change |
|---|---|---|
| Add a note field (metadata) | `db.h`, `db.c` | `db.h`, `db.c` (schema + ALTER migration) |
| Add a note field (content/format) | `serialize.h`, `serialize.c` | `serialize.h`, `serialize.c`, `editor_window.c` |
| Add a new sidebar row or section | `library_window.c` (`SB_KIND_*`, `refresh_sidebar`) | `library_window.c`, `app.h` |
| Add a new CLI command | `cli.h` (synopsis), `cli.c` (`cmd_*`, `cli_dispatch_verbs`) | `cli.h`, `cli.c` |
| Make a CLI command print JSON too | `cli.c` (`json_str`/`json_time`/`json_array_*`, `print_note_line`) | `cli.c` (add the branch + the name to `cli_json_capable`) |
| Add a new toolbar button | `app.c` (`on_app_tool_item_new`), target window .c | `app.h` (if new kind), target window .c |
| Add a new ini setting | `app.h` (`OnApp` struct + block comment), `main.c` (bool loading block) | `app.h`, `main.c`, `settings_window.c` |
| Add a new DB column | `db.c` (schema + ALTER migration section around line 223) | `db.h`, `db.c` |
| Modify the BNBF format | `serialize.h` (format spec), `serialize.c` | `serialize.h`, `serialize.c` (bump `BNBF_VERSION`, add new `REC_*`) |
| Change editor window layout | `editor_window.c` (`editor_build_layout`, `editor_build_view`) | `editor_window.c` |
| Change library window layout | `library_window.c` (builder functions: `library_build_*`) | `library_window.c` |
| Change AI summary behaviour | `library_window.c` (`run_ai_summary`, `build_ai_pane`) | `library_window.c` |
| Modify export output | `export.c` | `export.c` |
| Modify search behaviour | `search_window.c` | `search_window.c` |
| Change the search QUERY LANGUAGE (both surfaces) | `search_query.h` (the syntax), `search_query.c` | `search_query.c` |
| Modify the media browser | `media_window.c`, `serialize.h` (image API) | `media_window.c` |
| Change the modal image viewer (BOTH hosts) | `image_viewer.h` (the ops contract), `image_viewer.c` | `image_viewer.c` |
| Add a THIRD image-viewer host | `image_viewer.h`, `media_window.c` (`media_viewer_ops`) | new host .c only |

## Rename cleanup TODO

Two renames — Blue Notes → Records (2026-07-31), Records → Notes
(2026-08) — left a few things intentionally unchanged.  The second rename
covered every identifier and string that literally said "records", plus the
on-disk names (with the two adopt shims documented above), the binary,
the .app bundle, the packages and .gitignore.  What was deliberately NOT
touched, and should be cleaned up eventually:

- **Internal C naming**: `on_` / `On` / `ON_` prefixes throughout all
  source files (originally stood for "Orange Notes" → carried through
  Blue Notes → Records → Notes; safe to rename but a large mechanical
  change, and they never said "records", which is why the 2026-08 sweep
  left them alone).
- **The word "records" as a NOUN in the format docs** — `serialize.[ch]`,
  the BNBF sections here, "PNG image records", "typed records" — refers to
  BNBF records, not the app.  A rename sweep MUST protect these (and the
  verb, as in "the CLI records the request", and the Blue Note Records
  credit below); do it with an explicit protect-list and assert the
  protected strings survive, never a bare find-and-replace.
- **Header guards**: `BLUE_DB_H`, `BLUE_IPC_H`, `BLUE_CLI_H` etc. in
  the `#ifndef` guards — purely cosmetic, zero runtime impact.
- **BNBF format name**: `serialize.h` still has a note that BNBF stood
  for "Blue Notes Binary Format". The magic bytes `BNBF` are stored in
  every note blob and cannot be changed without a migration; the comment
  is just a historical footnote.
- **About dialog authors string**: "And thanks to Blue Note Records…" —
  an acknowledgment of the jazz label, intentionally kept and deliberately
  excluded from both rename sweeps ("Records" there is the label's name).
  The app is called Notes again, so "Note" overlaps once more, though the
  original Blue Notes double-entendre is still gone. Worth rewording.
- **REFACTORING.md historical paths**: references to
  `~/.local/share/blue_notes/pre-heal-backup-20260709.db` and
  `~/.local/share/blue_notes/pre-onbf-migration-20260709.db` — those
  backup files physically exist at those paths; update the doc if/when
  the files are moved or deleted.

## Conventions

- Every function gets a banner comment: purpose, params, return; comment
  non-obvious variables. Column-aligned trailing comments, ~78-col lines.
- `on_` prefix for public symbols; `On` prefix for types.
- UI strings use UTF-8 escapes for …, •, ✕ etc. in source.
- No GtkHeaderBar. Window titles `"Notes - <name>"`.
- Scrollbars: overlay scrolling disabled globally
  (`GTK_OVERLAY_SCROLLING=0` in main) + per-scrolled-window; vertical
  policy AUTOMATIC.
