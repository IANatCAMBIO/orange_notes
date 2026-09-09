/* ===========================================================================
 * app.h — shared application context for Notes
 *
 * A single OnApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open editor windows, carries the
 * user's preferences, and loads button icons from the app-local icons/
 * folder (see on_app_icon_image_sized).
 * =========================================================================== */

#ifndef BLUE_APP_H
#define BLUE_APP_H

#include <gtk/gtk.h>
#include "db.h"

/* Semantic version, baked in by the Makefile (-DON_VERSION="x.y.z") so
 * the About dialog and the package filenames come from ONE definition
 * (the VERSION variable at the top of the Makefile).                        */
#ifndef ON_VERSION
#define ON_VERSION "dev"
#endif

/* ---------------------------------------------------------------------------
 * OnApp — global application state.
 *
 * Fields:
 *   gtk_app        — the GtkApplication driving the main loop.
 *   db             — open notes database (owned; closed at shutdown).
 *   editors        — map of open editor windows, keyed by note id
 *                    (gint64* keys, GtkWindow* values).  An entry exists
 *                    exactly while that note's editor window is open.
 *   library_window — the (single) library window, or NULL before startup.
 *   notify_notes_changed — hook installed by the library window: the
 *                    FULL refresh (sidebar counts/tags + notes pane).
 *                    For structural changes — notes created/moved/
 *                    deleted, database switched/restored, tag set
 *                    edited.  May be NULL.
 *   notify_note_saved — lighter hook, also installed by the library
 *                    window: updates the ONE row of the notes pane that
 *                    the saved note owns (title, modified time, preview),
 *                    leaving the rest of the model, the selection and the
 *                    scroll position alone.  Editor saves use this unless
 *                    the note's tag set or action set changed — a save can
 *                    change neither which notes are listed nor the folder
 *                    counts, so neither the sidebar nor a full repopulate
 *                    is warranted.  Takes the note's id so the row can be
 *                    found without rebuilding the model.  May be NULL.
 *   notify_status  — hook installed by the library window: shows an event
 *                    message ("DB saved", …) on the right side of its
 *                    status bar.  Post through on_app_status(), which
 *                    handles the hook being NULL.
 *   icons_dir      — absolute path of the local icons/ folder the button
 *                    icons (elementary SVGs) are loaded from (owned
 *                    string).
 *   code_copy_buttons — whether code blocks show their floating copy
 *                    button (File → Settings…); persisted as the
 *                    "code_copy_button" setting.
 *   sidebar_counts — whether the library sidebar shows note counts next
 *                    to folders and tags; persisted as the
 *                    "sidebar_counts" setting (default off).
 *   first_line_title — whether line 0 of a note is treated as its title:
 *                    the editor centers that line and auto-formats it as
 *                    Heading 1 while a brand-new (or emptied) note's title
 *                    is still unwritten.  Off = a plain left-aligned body
 *                    line; line 0 still SUPPLIES the note title either
 *                    way.  Persisted as "first_line_title" (default on).
 *   compact_editor_toolbar — whether the editor toolbar collapses the
 *                    paragraph-style buttons (H1/H2/¶) into a "Styles"
 *                    menu button and the list buttons into a "Lists"
 *                    one; persisted as the "compact_editor_toolbar"
 *                    setting (default off).
 *   comfortable_list — whether the library list view shows tall rows
 *                    with a bold title and a small grey body-text
 *                    preview (Comfortable density); persisted as
 *                    "list_density_comfortable" (default off = Compact).
 *   db_dir         — custom directory holding the db (owned string), or
 *                    NULL for the default location.  Persisted in the
 *                    config FILE (notes.ini next to the binary), not
 *                    the database — the database's own location cannot
 *                    live inside it.
 *   db_integrity_check — when TRUE, PRAGMA integrity_check and PRAGMA
 *                    foreign_key_check are run against the database at
 *                    startup; a warning dialog is shown if issues are found.
 *   statusbar_db_path — whether the library/editor status bars prefix
 *                    the folder path with the database file's path;
 *                    persisted as the "statusbar_db_path" setting
 *                    (default on).
 *   statusbar_note_id — whether each editor's status bar shows the
 *                    note's database id at the right edge; persisted as
 *                    the "statusbar_note_id" setting (default off).
 *   show_done_actions — whether the library's Action Items view lists
 *                    completed (struck-through) items too; persisted as
 *                    the "show_done_actions" setting (default on).
 *   ai_enabled     — master switch for AI features (Summary toolbar button
 *                    visible only when TRUE); persisted as "ai_enabled".
 *   ai_command     — shell command invoked to run the AI model (owned
 *                    string); set via File → Settings → AI Features.
 *   ai_custom_prompt — prompt text used for folders in Custom AI mode;
 *                    set via File → Settings → AI Features (owned string).
 *   notify_ai_changed — callback that shows or hides the AI toolbar button
 *                    when ai_enabled changes.
 *   db_transient   — TRUE when the database that is currently open was
 *                    chosen interactively at launch (not the configured
 *                    default); used to decide whether to persist the path.
 *   touch_css      — GtkCssProvider that hides the touch drag handles and
 *                    magnifier (applied when touch assistance is disabled);
 *                    NULL when touch assistance is shown.
 * ------------------------------------------------------------------------- */

typedef struct OnApp {
    GtkApplication  *gtk_app;
    OnDatabase      *db;
    GHashTable      *editors;
    GtkWidget       *library_window;
    void           (*notify_notes_changed)(struct OnApp *app);
    void           (*notify_note_saved)(struct OnApp *app, gint64 note_id);
    void           (*notify_status)(struct OnApp *app, const gchar *message);
    gchar           *icons_dir;
    gboolean         code_copy_buttons;
    gboolean         code_line_numbers;
    gboolean         sidebar_counts;
    gboolean         first_line_title;
    gboolean         compact_editor_toolbar;
    gboolean         comfortable_list;
    gchar           *db_dir;
    gboolean         db_integrity_check;
    gboolean         statusbar_db_path;
    gboolean         statusbar_note_id;
    gboolean         show_done_actions;
    gboolean         bold_list_titles;   /* bold titles in comfortable list mode */
    gboolean         ai_enabled;       /* AI features master kill switch       */
    gchar           *ai_command;       /* command path to invoke AI            */
    gchar           *ai_custom_prompt; /* prompt text for Custom folder mode   */
    void           (*notify_ai_changed)(struct OnApp *app); /* show/hide btn   */
    gboolean         db_transient;     /* TRUE when the current DB was opened
                                        * for this session only (not default) */
    GtkCssProvider  *touch_css;        /* screen CSS hiding the touch aids
                                        * (selection handles + magnifier);
                                        * NULL = touch assistance shown       */
} OnApp;

/* ---------------------------------------------------------------------------
 * on_app_status() — post a one-line event message to the library window's
 * status bar (printf-style).  Safe to call from anywhere: a no-op until
 * the library window has installed app->notify_status.
 *   app — the application context.
 *   fmt — printf-style format for the message.
 * ------------------------------------------------------------------------- */
void on_app_status(OnApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* ---------------------------------------------------------------------------
 * on_app_location_text() — format a folder path for a status bar's left
 * label: when the statusbar_db_path setting is on, the database file's
 * path is prepended as one continuous path ("<db path><location>", a
 * "/" inserted for the non-path views); otherwise the location is
 * returned as-is.
 *   app      — the application context.
 *   location — the folder-path part (e.g. "/Work/Projects", "#tag").
 * Returns a newly allocated string; free with g_free().
 * ------------------------------------------------------------------------- */
gchar *on_app_location_text(OnApp *app, const gchar *location);

/* ---------------------------------------------------------------------------
 * on_app_widget_add_css() — attach a one-off CSS snippet to a single
 * widget's style context (application priority).  The provider is owned
 * by the style context after this call.
 *   widget   — the widget to style.
 *   css_text — the CSS.
 * ------------------------------------------------------------------------- */
void on_app_widget_add_css(GtkWidget *widget, const gchar *css_text);

/* ---------------------------------------------------------------------------
 * on_app_notice() — run a modal OK message dialog and destroy it.
 *   parent — transient parent window, or NULL.
 *   type   — GTK_MESSAGE_INFO/WARNING/ERROR.
 *   title  — window title, or NULL for the GTK default.
 *   fmt    — printf-style message.
 * ------------------------------------------------------------------------- */
void on_app_notice(GtkWindow *parent, GtkMessageType type,
                   const gchar *title, const gchar *fmt, ...)
                   G_GNUC_PRINTF(4, 5);

/* ---------------------------------------------------------------------------
 * on_app_pick_path() — run a modal file chooser and return the selection.
 *   parent         — transient parent window, or NULL.
 *   title          — dialog title.
 *   action         — GTK_FILE_CHOOSER_ACTION_OPEN/SELECT_FOLDER/….
 *   accept_label   — accept-button label (e.g. "_Open").
 *   filter_name    — display name of a single file filter, or NULL for
 *                    no filter (filter_pattern is ignored when NULL).
 *   filter_pattern — glob the filter matches (e.g. "*.db").
 * Returns the chosen path (g_free), or NULL if cancelled.
 * ------------------------------------------------------------------------- */
gchar *on_app_pick_path(GtkWindow *parent, const gchar *title,
                        GtkFileChooserAction action,
                        const gchar *accept_label,
                        const gchar *filter_name,
                        const gchar *filter_pattern);

/* ---------------------------------------------------------------------------
 * on_app_init_icons_dir() — locate the icons/ folder next to the
 * executable (falling back to ./icons) and remember it in app->icons_dir.
 *   app   — the application context.
 *   argv0 — argv[0] from main(), used to find the executable's directory.
 * ------------------------------------------------------------------------- */
void on_app_init_icons_dir(OnApp *app, const gchar *argv0);

/* ---------------------------------------------------------------------------
 * on_app_icon_image_sized() — build a GtkImage for icon `name` from
 * "<icons_dir>/<name>.svg" (then .png), rendered at an explicit pixel
 * size.  The bundled icons are elementary SVGs, which need the librsvg
 * gdk-pixbuf loader to decode.
 *   app  — the application context.
 *   name — icon file basename without extension (e.g. "edit-copy").
 *   size — logical pixel size to render at.
 * Returns a new GtkImage, or NULL if no loadable file exists — callers
 * fall back to a text label in that case.
 * ------------------------------------------------------------------------- */
GtkWidget *on_app_icon_image_sized(OnApp *app, const gchar *name,
                                   gint size);

/* ---------------------------------------------------------------------------
 * on_app_icon_surface() — the raw HiDPI-scaled cairo surface behind
 * on_app_icon_image_sized(), for uses that need a surface rather than a
 * widget (e.g. gtk_drag_set_icon_surface).  Same lookup and scaling.
 *   app  — the application context.
 *   name — icon file basename without extension (e.g. "folder").
 *   size — logical pixel size to render at.
 * Returns a new surface (caller cairo_surface_destroy's it), or NULL if
 * no loadable file exists.
 * ------------------------------------------------------------------------- */
cairo_surface_t *on_app_icon_surface(OnApp *app, const gchar *name,
                                     gint size);

/* ---------------------------------------------------------------------------
 * on_app_tool_item_new() — create an icon toolbar button.
 *   app             — the application context.
 *   toggle          — TRUE for a GtkToggleToolButton, FALSE for a plain
 *                     GtkToolButton.
 *   icon_name       — local icon file to use (see on_app_icon_image), or
 *                     NULL for none.
 *   fallback_markup — Pango markup rendered as the "icon" when the icon
 *                     file is missing (e.g. "<b>H1</b>"); NULL to fall
 *                     back to the plain label.
 *   label           — the button's accessible text label; also the icon
 *                     stand-in when both the icon file and
 *                     fallback_markup are absent.
 *   tooltip         — hover help text.
 * Returns the new tool item (not yet shown).
 * ------------------------------------------------------------------------- */
GtkToolItem *on_app_tool_item_new(OnApp *app, gboolean toggle,
                                  const gchar *icon_name,
                                  const gchar *fallback_markup,
                                  const gchar *label,
                                  const gchar *tooltip);

/* ---------------------------------------------------------------------------
 * on_app_config_init() — resolve the application config file once
 * ("notes.ini" in the same directory as the binary, from `argv0`)
 * and load it into memory.  All later reads are served from memory; the
 * file is only written when a setting changes.  Must run before any
 * other config call; safe to call repeatedly.
 * ------------------------------------------------------------------------- */
void on_app_config_init(const gchar *argv0);

/* ---------------------------------------------------------------------------
 * on_app_config_get() — read one setting from the in-memory config.
 * Returns a new string (g_free() it), or NULL when unset/empty.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_get(const gchar *key);

/* ---------------------------------------------------------------------------
 * on_app_config_get_bool() — read a 0/1 setting; `def` when unset.  The
 * app only ever writes "0"/"1", so any other stored value reads as "1".
 * ------------------------------------------------------------------------- */
gboolean on_app_config_get_bool(const gchar *key, gboolean def);

/* ---------------------------------------------------------------------------
 * on_app_apply_touch_assist() — honor the "touch_assist" setting (`1|0`,
 * default 0 = DISABLED): unless enabled, install a screen-wide CSS
 * provider that hides GTK's touch aids — the teardrop drag handles under
 * selections/the cursor ("cursor-handle" nodes, collapsed to nothing)
 * and the selection magnifier (its popover, rendered transparent).  Some
 * Linux input stacks pop these up for plain mouse selections; GTK3 has
 * no API to turn them off, so CSS is the lever.  Removes the provider
 * again when assistance is re-enabled.  Safe to call any time after GTK
 * is initialized; applies live.  The tap cut/copy/paste bubble is the
 * OTHER half of the setting: CSS cannot hide it safely (its buttons
 * would stay clickable while invisible), so main() suppresses it — and
 * the touch classification behind all of these — with
 * GDK_CORE_DEVICE_EVENTS=1 before GTK init (restart to change).
 *   app — the application context (owns the provider).
 * ------------------------------------------------------------------------- */
void on_app_apply_touch_assist(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_config_set() — change one setting: updates the in-memory config
 * AND writes the ini file through.  NULL removes the key.
 * ------------------------------------------------------------------------- */
void on_app_config_set(const gchar *key, const gchar *value);

/* ---------------------------------------------------------------------------
 * on_app_config_get_size() — read a persisted window size from two ini keys.
 * *w and *h are left ALONE unless both keys are present and both parse to a
 * positive number, so the caller just pre-loads its defaults.  The editor and
 * the search window each used to spell this out.
 *   key_w / key_h — the two ini keys (e.g. "search_win_w"/"search_win_h").
 *   w / h         — in/out: the caller's defaults, replaced on success.
 * ------------------------------------------------------------------------- */
void on_app_config_get_size(const gchar *key_w, const gchar *key_h,
                            gint *w, gint *h);

/* ---------------------------------------------------------------------------
 * on_app_read_stream() — read a whole stdio stream into a new string.
 * Used for the CLI's "-" (read stdin) arguments and for collecting a
 * delegated command's captured output.
 *   f      — the stream to drain.
 *   rewind_first — TRUE to rewind() before reading (a temp capture file);
 *                  FALSE to read from the current position (stdin).
 * Returns a newly allocated, NUL-terminated string; g_free() it.
 * ------------------------------------------------------------------------- */
gchar *on_app_read_stream(FILE *f, gboolean rewind_first);

/* ---------------------------------------------------------------------------
 * on_app_config_load_db_dir() — read the custom database directory from
 * the config file. Returns a new string (g_free() it), or NULL when the
 * default location is in use.
 * ------------------------------------------------------------------------- */
gchar *on_app_config_load_db_dir(void);

/* ---------------------------------------------------------------------------
 * on_app_close_all_editors() — destroy every open editor window (each
 * flushes its final autosave on destroy). Used before switching or
 * restoring the database.
 * ------------------------------------------------------------------------- */
void on_app_close_all_editors(OnApp *app);

/* ---------------------------------------------------------------------------
 * on_app_switch_database() — move the app onto a different database
 * location, live: closes all editors, closes the current database, opens
 * notes.db inside `new_dir` (NULL = the default location), and
 * persists the choice in the config file.  If the target has no
 * notes.db yet, the current database file is copied there first, so
 * notes follow the move.  If the target ALREADY has a notes.db, the
 * user is asked first — use the existing database, overwrite it with a
 * copy of the current one, or cancel (which leaves everything untouched).
 * Failures are reported in a dialog and the old database is reopened.
 *   app     — the application context.
 *   new_dir — directory to hold notes.db, or NULL for the default.
 * Returns TRUE if the switch happened.
 * ------------------------------------------------------------------------- */
gboolean on_app_switch_database(OnApp *app, const gchar *new_dir);

/* ---------------------------------------------------------------------------
 * on_app_actions_backfill() — one-time population of the action_items
 * table from pre-existing note content (gated by PRAGMA user_version, so
 * repeat calls cost a single PRAGMA read).  Run after EVERY on_db_open
 * that yields a long-lived handle: GUI startup, the headless CLI path,
 * and database switch/restore (an adopted or restored file may predate
 * the feature).  NULL-safe.
 * ------------------------------------------------------------------------- */
void on_app_actions_backfill(OnDatabase *db);

/* ---------------------------------------------------------------------------
 * on_app_action_uids_backfill() — one-time assignment of stable uids to
 * action_items rows written before the uid column existed (gated by
 * PRAGMA user_version, like on_app_actions_backfill, which it must run
 * AFTER: that one indexes the rows, this one identifies them).  Cheap —
 * it touches the table only, never the note blobs.  NULL-safe.
 * ------------------------------------------------------------------------- */
void on_app_action_uids_backfill(OnDatabase *db);

#endif /* BLUE_APP_H */
