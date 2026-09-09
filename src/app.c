/* ===========================================================================
 * app.c — shared application helpers (implementation)
 *
 * Icon loading and the shared toolbar-button factory.  Icons live in a
 * plain folder of SVG/PNG files next to the executable ("icons/"), named
 * by the usual freedesktop action names (edit-delete.svg, list-add.svg,
 * …) so users can swap any of them by replacing the file.
 * =========================================================================== */

#include "app.h"
#include "serialize.h"               /* on_note_extract_actions (backfill)  */

#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

void
on_app_status(OnApp *app, const gchar *fmt, ...)
{
    if (app->notify_status == NULL)
        return;
    va_list args;                    /* printf-style arguments              */
    va_start(args, fmt);
    gchar *message = g_strdup_vprintf(fmt, args);
    va_end(args);
    app->notify_status(app, message);
    g_free(message);
}

gchar *
on_app_location_text(OnApp *app, const gchar *location)
{
    if (!app->statusbar_db_path || app->db == NULL ||
        app->db->path == NULL)
        return g_strdup(location);
    /* One continuous path: folder locations already start with "/"; the
     * non-path views ("#tag", "Pinned Notes", …) get one inserted.        */
    return g_strdup_printf("%s%s%s", app->db->path,
                           location != NULL && location[0] == '/' ? ""
                                                                  : "/",
                           location);
}

void
on_app_notice(GtkWindow *parent, GtkMessageType type,
              const gchar *title, const gchar *fmt, ...)
{
    va_list args;                    /* printf-style arguments              */
    va_start(args, fmt);
    gchar *message = g_strdup_vprintf(fmt, args);
    va_end(args);

    GtkWidget *dialog = gtk_message_dialog_new(
        parent, GTK_DIALOG_MODAL, type, GTK_BUTTONS_OK, "%s", message);
    if (title != NULL)
        gtk_window_set_title(GTK_WINDOW(dialog), title);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    g_free(message);
}

gchar *
on_app_pick_path(GtkWindow *parent, const gchar *title,
                 GtkFileChooserAction action, const gchar *accept_label,
                 const gchar *filter_name, const gchar *filter_pattern)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        title, parent, action,
        "_Cancel",    GTK_RESPONSE_CANCEL,
        accept_label, GTK_RESPONSE_ACCEPT,
        NULL);
    if (filter_name != NULL) {
        GtkFileFilter *filter = gtk_file_filter_new();
        gtk_file_filter_set_name(filter, filter_name);
        gtk_file_filter_add_pattern(filter, filter_pattern);
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    }
    gchar *path = NULL;              /* the selection, NULL if cancelled    */
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return path;
}

void
on_app_widget_add_css(GtkWidget *widget, const gchar *css_text)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, css_text, -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget),
                                   GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
}

/* ---------------------------------------------------------------------------
 * exe_dir_from_argv0() — the directory containing the executable; when
 * launched via a bare name from PATH there is no directory part, so fall
 * back to the current working directory.  Returns a new string.
 * ------------------------------------------------------------------------- */
static gchar *
exe_dir_from_argv0(const gchar *argv0)
{
    if (argv0 != NULL && strchr(argv0, G_DIR_SEPARATOR) != NULL) {
        gchar *abs = g_canonicalize_filename(argv0, NULL);
        gchar *dir = g_path_get_dirname(abs);
        g_free(abs);
        return dir;
    }
    return g_get_current_dir();
}

void
on_app_init_icons_dir(OnApp *app, const gchar *argv0)
{
    gchar *exe_dir = exe_dir_from_argv0(argv0);
    app->icons_dir = g_build_filename(exe_dir, "icons", NULL);
    g_free(exe_dir);
}

cairo_surface_t *
on_app_icon_surface(OnApp *app, const gchar *name, gint size)
{
    static const gchar *EXTS[] = { "svg", "png" };

    /* Rasterize at the display's scale factor so icons stay sharp on
     * HiDPI/Retina screens: `size` is the LOGICAL size, the backing
     * pixels are size × sf, and the cairo surface's device scale maps
     * between the two.                                                     */
    gint sf = 1;                     /* display scale factor                */
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if (monitor == NULL)
            monitor = gdk_display_get_monitor(display, 0);
        if (monitor != NULL)
            sf = gdk_monitor_get_scale_factor(monitor);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(EXTS); i++) {
        gchar *path = g_strdup_printf("%s%c%s.%s",
                                      app->icons_dir, G_DIR_SEPARATOR,
                                      name, EXTS[i]);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            /* Verify the file actually decodes (SVGs need the librsvg
             * pixbuf loader) — a broken-image icon is worse than the
             * text fallback the caller provides.                           */
            GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_size(
                path, size * sf, size * sf, NULL);
            if (pix != NULL) {
                cairo_surface_t *surface =
                    gdk_cairo_surface_create_from_pixbuf(pix, sf, NULL);
                g_object_unref(pix);
                g_free(path);
                return surface;
            }
        }
        g_free(path);
    }
    return NULL;
}

GtkWidget *
on_app_icon_image_sized(OnApp *app, const gchar *name, gint size)
{
    cairo_surface_t *surface = on_app_icon_surface(app, name, size);
    if (surface == NULL)
        return NULL;
    GtkWidget *image = gtk_image_new_from_surface(surface);
    cairo_surface_destroy(surface);
    return image;
}

/* on_app_icon_image() — toolbar-default (24 px) icon variant.               */
static GtkWidget *
on_app_icon_image(OnApp *app, const gchar *name)
{
    return on_app_icon_image_sized(app, name, 24);
}

GtkToolItem *
on_app_tool_item_new(OnApp *app, gboolean toggle, const gchar *icon_name,
                     const gchar *fallback_markup, const gchar *label,
                     const gchar *tooltip)
{
    GtkToolItem *item = toggle
        ? GTK_TOOL_ITEM(gtk_toggle_tool_button_new())
        : GTK_TOOL_ITEM(gtk_tool_button_new(NULL, NULL));
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), label);

    /* Icon: the local PNG if present, else the fallback markup rendered
     * as a label standing in for the icon.                                 */
    GtkWidget *icon = (icon_name != NULL)
                      ? on_app_icon_image(app, icon_name) : NULL;
    if (icon == NULL) {
        icon = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon),
                             fallback_markup != NULL ? fallback_markup
                                                     : label);
    }
    gtk_widget_show(icon);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item), icon);

    gtk_tool_item_set_tooltip_text(item, tooltip);
    return item;
}

/* ---------------------------------------------------------------------------
 * The application config: "notes.ini" in the same directory as
 * the binary.  on_app_config_init() resolves the path and loads the
 * whole file into memory ONCE; every read is served from memory and the
 * file is only touched again to write a modification through.  All keys
 * live under one [notes] group.
 * ------------------------------------------------------------------------- */
static gchar    *config_ini_path = NULL;   /* resolved ini path             */
static GKeyFile *config_kf       = NULL;   /* in-memory settings            */

#define CONFIG_GROUP "notes"

void
on_app_config_init(const gchar *argv0)
{
    if (config_kf != NULL)
        return;                      /* already resolved and loaded         */
    gchar *exe_dir = exe_dir_from_argv0(argv0);
    config_ini_path = g_build_filename(exe_dir, "notes.ini", NULL);

    /* Portable mode (the usual case: binary run from its build/unpack
     * directory) keeps the ini next to the binary.  System installs
     * (.deb/.rpm/.app in /Applications: read-only binary dir) would fail
     * every write-through, so when no binary-adjacent ini exists AND the
     * directory is unwritable, the ini lives in the user config dir
     * instead (~/.config/notes/notes.ini).                             */
    if (!g_file_test(config_ini_path, G_FILE_TEST_EXISTS) &&
        g_access(exe_dir, W_OK) != 0) {
        g_free(config_ini_path);
        gchar *cfg_dir = g_build_filename(g_get_user_config_dir(),
                                          "notes", NULL);
        g_mkdir_with_parents(cfg_dir, 0755);
        config_ini_path = g_build_filename(cfg_dir, "notes.ini",
                                           NULL);
        g_free(cfg_dir);
    }

    /* First launch (no ini yet): seed it from the defaults file shipped
     * next to the binary, so a fresh install starts with sane settings.    */
    if (!g_file_test(config_ini_path, G_FILE_TEST_EXISTS)) {
        gchar *defaults_path = g_build_filename(
            exe_dir, "notes.ini.defaults", NULL);
        gchar *contents = NULL;      /* defaults file body                  */
        gsize  len = 0;
        if (g_file_get_contents(defaults_path, &contents, &len, NULL)) {
            GError *err = NULL;
            if (!g_file_set_contents(config_ini_path, contents, len,
                                     &err)) {
                g_warning("config: cannot seed %s: %s", config_ini_path,
                          err->message);
                g_clear_error(&err);
            }
            g_free(contents);
        }
        g_free(defaults_path);
    }
    g_free(exe_dir);

    config_kf = g_key_file_new();
    g_key_file_load_from_file(config_kf, config_ini_path,
                              G_KEY_FILE_NONE, NULL);  /* absent file OK    */
}

/* config_write() — flush the in-memory settings to the ini file.            */
static void
config_write(void)
{
    GError *err = NULL;
    if (!g_key_file_save_to_file(config_kf, config_ini_path, &err)) {
        g_warning("config: cannot save %s: %s", config_ini_path,
                  err->message);
        g_clear_error(&err);
    }
}

gchar *
on_app_config_get(const gchar *key)
{
    if (config_kf == NULL)
        return NULL;
    gchar *value = g_key_file_get_string(config_kf, CONFIG_GROUP, key,
                                         NULL);
    if (value != NULL && *value == '\0') {
        g_free(value);
        value = NULL;
    }
    return value;
}

gboolean
on_app_config_get_bool(const gchar *key, gboolean def)
{
    gchar *v = on_app_config_get(key);
    gboolean r = (v == NULL) ? def : g_strcmp0(v, "0") != 0;
    g_free(v);
    return r;
}

void
on_app_config_set(const gchar *key, const gchar *value)
{
    if (config_kf == NULL)
        return;

    /* Skip the file rewrite when the value isn't changing — some callers
     * fire per keystroke (the image-viewer entry).                         */
    gchar *cur = g_key_file_get_string(config_kf, CONFIG_GROUP, key, NULL);
    gboolean same = g_strcmp0(cur, value) == 0;
    g_free(cur);
    if (same)
        return;

    if (value != NULL)
        g_key_file_set_string(config_kf, CONFIG_GROUP, key, value);
    else
        g_key_file_remove_key(config_kf, CONFIG_GROUP, key, NULL);
    config_write();
}

void
on_app_config_get_size(const gchar *key_w, const gchar *key_h,
                       gint *w, gint *h)
{
    gchar *w_str = on_app_config_get(key_w);
    gchar *h_str = on_app_config_get(key_h);
    if (w_str != NULL && h_str != NULL) {
        gint pw = (gint)g_ascii_strtoll(w_str, NULL, 10);  /* parsed width  */
        gint ph = (gint)g_ascii_strtoll(h_str, NULL, 10);  /* parsed height */
        if (pw > 0 && ph > 0) {      /* both or neither: never a half size  */
            *w = pw;
            *h = ph;
        }
    }
    g_free(w_str);
    g_free(h_str);
}

gchar *
on_app_read_stream(FILE *f, gboolean rewind_first)
{
    GString *s = g_string_new(NULL);
    if (rewind_first)
        rewind(f);
    gchar  buf[4096];                /* read chunk                          */
    gsize  n;                        /* bytes in this chunk                 */
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        g_string_append_len(s, buf, (gssize)n);
    return g_string_free(s, FALSE);
}

gchar *
on_app_config_load_db_dir(void)
{
    return on_app_config_get("db_dir");
}

/* config_save_db_dir() — persist (or clear, with NULL) the custom db dir.   */
static void
config_save_db_dir(const gchar *dir)
{
    on_app_config_set("db_dir", dir);
}

void
on_app_apply_touch_assist(OnApp *app)
{
    gboolean assist =                /* default: disabled                   */
        on_app_config_get_bool("touch_assist", FALSE);

    if (!assist && app->touch_css == NULL) {
        GtkCssProvider *css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            /* Selection/cursor handles: collapse the nodes entirely — no
             * themed teardrop graphic and a 0x0 allocation, so they
             * neither draw nor grab pointer input.                         */
            "cursor-handle {"
            "  -gtk-icon-source: none;"
            "  background: none;"
            "  border: none;"
            "  box-shadow: none;"
            "  min-width: 0;"
            "  min-height: 0;"
            "  padding: 0;"
            "  margin: 0;"
            "}"
            /* Touch-selection magnifier: its popover cannot be collapsed
             * (GtkTextView puts a hard size request on the content), so
             * render the whole thing fully transparent instead.            */
            "popover.magnifier {"
            "  opacity: 0;"
            "  background: none;"
            "  border: none;"
            "  box-shadow: none;"
            "}",
            -1, NULL);
        gtk_style_context_add_provider_for_screen(
            gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        app->touch_css = css;
    } else if (assist && app->touch_css != NULL) {
        gtk_style_context_remove_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(app->touch_css));
        g_object_unref(app->touch_css);
        app->touch_css = NULL;
    }
}

void
on_app_close_all_editors(OnApp *app)
{
    /* Destroying an editor removes it from the table, so copy the window
     * list first.                                                          */
    GList *windows = g_hash_table_get_values(app->editors);
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(windows);
}

/* ---------------------------------------------------------------------------
 * copy_file() — overwrite-copy `src` to `dest` via GIO.
 * Returns TRUE on success (warning logged otherwise).
 * ------------------------------------------------------------------------- */
static gboolean
copy_file(const gchar *src, const gchar *dest)
{
    GFile *fsrc  = g_file_new_for_path(src);
    GFile *fdest = g_file_new_for_path(dest);
    GError *err = NULL;
    gboolean ok = g_file_copy(fsrc, fdest, G_FILE_COPY_OVERWRITE,
                              NULL, NULL, NULL, &err);
    if (!ok) {
        g_warning("config: copy %s -> %s failed: %s", src, dest,
                  err->message);
        g_clear_error(&err);
    }
    g_object_unref(fsrc);
    g_object_unref(fdest);
    return ok;
}

/* The user_version that says the action_items table has been backfilled
 * from pre-existing note content.  2: re-indexed once after the
 * due-date-blind change comparison left due-only edits stale in the
 * table (2026-07).                                                          */
#define DB_VERSION_ACTIONS 2

/* The user_version that says every action_items row carries a stable uid
 * (2026-08).  Strictly above DB_VERSION_ACTIONS: the rows must exist
 * before they can be given ids.                                            */
#define DB_VERSION_ACTION_UIDS 3

void
on_app_actions_backfill(OnDatabase *db)
{
    if (db == NULL || on_db_user_version(db) >= DB_VERSION_ACTIONS)
        return;

    /* Trashed notes are included so a later restore is already indexed;
     * the library's queries filter them out.  Same cheap record walk as
     * the body_text extraction (image payloads skipped) — one-time cost.   */
    GList *notes = on_db_note_list_all(db, TRUE);
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */
        gsize   blob_len = 0;        /* stored blob size                    */
        guint8 *blob = on_db_note_load(db, m->id, &blob_len);
        if (blob == NULL)
            continue;
        GList *actions = on_note_extract_actions(blob, blob_len);
        if (actions != NULL)         /* empty sets have no rows to write    */
            on_db_note_set_actions(db, m->id, actions);
        on_db_action_list_free(actions);
        g_free(blob);
    }
    on_db_note_list_free(notes);
    on_db_set_user_version(db, DB_VERSION_ACTIONS);
}

void
on_app_action_uids_backfill(OnDatabase *db)
{
    if (db == NULL)
        return;
    /* Normally a one-shot gated by the version stamp.  It also re-runs
     * whenever any row has no uid, which happens when an OLDER build
     * (whose INSERT predates the column) saves a note in a database this
     * one has already migrated: without the self-heal those rows would
     * keep uid 0 forever, since the stamp is already current.  The probe
     * is an indexed existence check, so the common case costs nothing.    */
    if (on_db_user_version(db) >= DB_VERSION_ACTION_UIDS &&
        !on_db_action_uids_missing(db))
        return;
    if (on_db_action_uids_fill(db))
        on_db_set_user_version(db, DB_VERSION_ACTION_UIDS);
}


gboolean
on_app_switch_database(OnApp *app, const gchar *new_dir)
{
    /* Resolve the target file inside the requested directory.              */
    gchar *target;                   /* path of the db at the new home      */
    if (new_dir != NULL) {
        g_mkdir_with_parents(new_dir, 0755);
        target = g_build_filename(new_dir, ON_DB_FILENAME, NULL);
    } else {
        target = on_db_default_path();
    }
    if (g_strcmp0(target, app->db->path) == 0) {
        g_free(target);
        return TRUE;                 /* already there: nothing to do        */
    }

    /* A database already living at the target must never be adopted (or
     * destroyed) silently: ask BEFORE anything is torn down, so Cancel
     * leaves the running app completely untouched.                         */
    gboolean overwrite = FALSE;      /* replace the target file?            */
    if (g_file_test(target, G_FILE_TEST_EXISTS)) {
        GtkWidget *dialog = gtk_message_dialog_new(
            app->library_window != NULL
                ? GTK_WINDOW(app->library_window) : NULL,
            GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "That folder already contains a notes database.\n\n"
            "Use the notes stored there, or overwrite it with a copy "
            "of your current database?\n"
            "(Overwriting permanently replaces the file at %s.)",
            target);
        gtk_window_set_title(GTK_WINDOW(dialog),
                             "Notes - Existing Database");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog),
                               "_Cancel",                GTK_RESPONSE_CANCEL,
                               "_Use Existing Database", 1,
                               "_Overwrite It",          2,
                               NULL);
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != 1 && response != 2) {
            g_free(target);
            return FALSE;            /* cancelled: nothing was touched      */
        }
        overwrite = response == 2;
    }

    /* Flush and detach everything that touches the current database.       */
    on_app_close_all_editors(app);

    gchar *old_path = g_strdup(app->db->path);   /* current db file         */
    on_db_close(app->db);
    app->db = NULL;

    /* Copy to the new location when needed.  Fail fast on copy error so we
     * never delete old_path without having a good copy at target.  The
     * "Use Existing" path skips the copy entirely — deletion on success is
     * the new intended behaviour there, not a side-effect of a failed copy. */
    if (g_file_test(old_path, G_FILE_TEST_EXISTS) &&
        (overwrite || !g_file_test(target, G_FILE_TEST_EXISTS))) {
        if (!copy_file(old_path, target)) {
            on_app_notice(app->library_window != NULL
                              ? GTK_WINDOW(app->library_window) : NULL,
                          GTK_MESSAGE_ERROR, NULL,
                          "Could not copy the database to that location.\n"
                          "The previous database is still in use.");
            app->db = on_db_open(old_path);
            g_free(target);
            g_free(old_path);
            return FALSE;
        }
    }

    app->db = on_db_open(target);
    on_app_actions_backfill(app->db);   /* adopted dbs may predate actions  */
    on_app_action_uids_backfill(app->db);   /* ...and predate their uids    */
    gboolean ok = app->db != NULL;   /* did the new location work?          */

    if (!ok) {
        /* Fall back to the previous database so the app stays usable.      */
        g_warning("config: cannot use %s; reverting to %s",
                  target, old_path);
        on_app_notice(app->library_window != NULL
                          ? GTK_WINDOW(app->library_window) : NULL,
                      GTK_MESSAGE_ERROR, NULL,
                      "Could not open a database at that location.\n"
                      "The previous database is still in use.");
        app->db = on_db_open(old_path);
    } else {
        /* Delete the original file: either we copied it to the new path,
         * or the user chose to switch to an existing database there.       */
        if (g_file_test(old_path, G_FILE_TEST_EXISTS)) {
            GFile *fold = g_file_new_for_path(old_path);
            if (!g_file_delete(fold, NULL, NULL))
                g_warning("config: could not remove old db at %s", old_path);
            g_object_unref(fold);
        }
        g_free(app->db_dir);
        app->db_dir = g_strdup(new_dir);
        app->db_transient = FALSE;
        config_save_db_dir(new_dir);
    }

    g_free(target);
    g_free(old_path);
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
    if (ok)
        on_app_status(app, "DB at %s loaded", app->db->path);
    return ok;
}

