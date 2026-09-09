/* ===========================================================================
 * main.c — Notes application entry point
 *
 * Wires everything together: opens the SQLite database, creates the
 * shared OnApp context, and shows the library window when the
 * GtkApplication activates.  Editor windows are opened from the library
 * (see library_window.c / editor_window.c).
 * =========================================================================== */

#include <gtk/gtk.h>
#include <glib-unix.h>

#include "app.h"
#include "cli.h"
#include "db.h"
#include "ipc.h"
#include "library_window.h"

#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

#ifdef __APPLE__
/* ---------------------------------------------------------------------------
 * quartz_log_filter() — GLogFunc that drops one specific, benign GDK
 * assertion emitted on macOS and forwards everything else unchanged.
 *
 * When GTK enumerates the clipboard's targets (the "TARGETS" atom — done
 * whenever the right-click/selection menus appear, on rich-text paste, and in
 * drag negotiation), the GDK Quartz backend converts each NSPasteboard type
 * to a GdkAtom via gdk_atom_intern(uti.preferredMIMEType.UTF8String)
 * (gdk/quartz/gdkselection-quartz.c).  Modern macOS pasteboards routinely
 * carry Apple-private types whose UTI has no MIME string, so UTF8String is
 * NULL and gdk_atom_intern trips its "atom_name != NULL" g_return_if_fail.
 * The check is non-fatal — gdk_atom_intern returns GDK_NONE and the
 * enumeration keeps going with the valid types — but it prints a Gdk-CRITICAL
 * on every affected paste/menu.  We cannot reach the upstream call site, so we
 * silence just this message and pass all other logs through untouched.
 *   domain  — log domain ("Gdk" for the offending message).
 *   level   — log level flags.
 *   message — the formatted log text.
 *   data    — unused.
 * ------------------------------------------------------------------------- */
static void
quartz_log_filter(const gchar   *domain,
                  GLogLevelFlags level,
                  const gchar   *message,
                  gpointer       data)
{
    (void)data;
    if (message != NULL &&
        strstr(message, "gdk_atom_intern") != NULL &&
        strstr(message, "atom_name != NULL") != NULL)
        return;                      /* benign macOS pasteboard artifact     */
    g_log_default_handler(domain, level, message, data);
}
#endif /* __APPLE__ */

/* ---------------------------------------------------------------------------
 * integrity_collect() — sqlite3_exec callback: accumulates non-"ok" rows
 * from a PRAGMA integrity_check result into the GString passed as `data`.
 * ------------------------------------------------------------------------- */
static int
integrity_collect(void *data, int argc, char **argv, char **col_names)
{
    (void)col_names;
    GString *out = data;
    for (int i = 0; i < argc; i++) {
        if (argv[i] != NULL && g_strcmp0(argv[i], "ok") != 0) {
            if (out->len > 0)
                g_string_append_c(out, '\n');
            g_string_append(out, argv[i]);
        }
    }
    return 0;
}

/* fk_collect() — sqlite3_exec callback: formats PRAGMA foreign_key_check
 * rows (table, rowid, parent, fkid) into human-readable lines.             */
static int
fk_collect(void *data, int argc, char **argv, char **col_names)
{
    (void)col_names;
    GString *out = data;
    if (argc >= 3 && argv[0] != NULL) {
        if (out->len > 0)
            g_string_append_c(out, '\n');
        g_string_append_printf(out, "  %s (row %s) \xe2\x86\x92 %s",
                               argv[0],
                               argv[1] != NULL ? argv[1] : "?",
                               argv[2] != NULL ? argv[2] : "?");
    }
    return 0;
}

/* startup_integrity_check() — run PRAGMA integrity_check and PRAGMA
 * foreign_key_check against the open database.  Shows a warning dialog if
 * either check reports problems.  Returns TRUE if both passed.             */
static gboolean
startup_integrity_check(OnApp *app)
{
    GString *ic_errors = g_string_new(NULL);
    sqlite3_exec(app->db->handle, "PRAGMA integrity_check",
                 integrity_collect, ic_errors, NULL);

    GString *fk_errors = g_string_new(NULL);
    sqlite3_exec(app->db->handle, "PRAGMA foreign_key_check",
                 fk_collect, fk_errors, NULL);

    gboolean ok = (ic_errors->len == 0 && fk_errors->len == 0);
    if (!ok) {
        GString *msg = g_string_new(NULL);
        if (ic_errors->len > 0) {
            g_string_append(msg, "Integrity check errors:\n");
            g_string_append(msg, ic_errors->str);
        }
        if (fk_errors->len > 0) {
            if (msg->len > 0)
                g_string_append(msg, "\n\n");
            g_string_append(msg, "Foreign key violations:\n");
            g_string_append(msg, fk_errors->str);
        }
        on_app_notice(NULL, GTK_MESSAGE_WARNING,
                      "Notes - Database Integrity Check",
                      "The database integrity check found issues:\n\n%s",
                      msg->str);
        g_string_free(msg, TRUE);
    }

    g_string_free(ic_errors, TRUE);
    g_string_free(fk_errors, TRUE);
    return ok;
}

/* startup_first_run() — no notes.db exists at the expected location:
 * ask whether to open an existing file or create a new one there, instead
 * of silently creating an empty database (a user pointing at a shared
 * folder usually means to OPEN a file that is already there).
 *   expected — the path where the database was looked for (dialog text).
 *   db_dir   — in/out: the configured db directory; replaced (and
 *              persisted to the ini) when an existing file is opened.
 *   db_path  — in/out: the path handed to on_db_open(); replaced when an
 *              existing file is opened.
 * Returns TRUE to proceed with on_db_open(*db_path), FALSE to quit.        */
static gboolean
startup_first_run(const gchar *expected, gchar **db_dir, gchar **db_path)
{
    /* Loop so cancelling the file chooser returns to the choice dialog.    */
    for (;;) {
        GtkWidget *dlg = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "No notes database was found at\n%s",
            expected);
        gtk_window_set_title(GTK_WINDOW(dlg), "Notes - Welcome");
        gtk_dialog_add_buttons(GTK_DIALOG(dlg),
            "_Open a notes.db File",  1,
            "Create a _New notes.db", 2,
            NULL);
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);

        if (resp == 2) {
            return TRUE;             /* on_db_open() creates it             */
        }
        if (resp != 1)
            return FALSE;            /* dialog closed — quit                */

        GtkWidget *chooser = gtk_file_chooser_dialog_new(
            "Open Notes Database", NULL,
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open",   GTK_RESPONSE_ACCEPT,
            NULL);
        gtk_window_set_title(GTK_WINDOW(chooser),
                             "Notes - Open Database");
        /* The app's model is a directory + the fixed name notes.db
         * (the ini stores db_dir only), so only that name is openable.     */
        GtkFileFilter *ff = gtk_file_filter_new();
        gtk_file_filter_add_pattern(ff, ON_DB_FILENAME);
        gtk_file_filter_set_name(ff,
            "Notes Database (" ON_DB_FILENAME ")");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

        gchar *file_path = NULL;     /* the chosen database file            */
        if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
            file_path = gtk_file_chooser_get_filename(
                GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);
        if (file_path == NULL)
            continue;                /* cancelled — back to the choice      */

        gchar *dir = g_path_get_dirname(file_path);
        g_free(file_path);
        on_app_config_set("db_dir",  dir);
        g_free(*db_dir);   *db_dir  = dir;
        g_free(*db_path);  *db_path = g_build_filename(dir, ON_DB_FILENAME,
                                                       NULL);
        return TRUE;
    }
}

/* ---------------------------------------------------------------------------
 * on_activate() — GtkApplication "activate" handler: show the library
 * window, or just raise it if the app is activated a second time.
 *   gtk_app   — the application.
 *   user_data — the OnApp context created in main().
 * ------------------------------------------------------------------------- */
static void
on_activate(GtkApplication *gtk_app, gpointer user_data)
{
    (void)gtk_app;
    OnApp *app = user_data;          /* shared application context          */

    if (app->library_window != NULL) {
        gtk_window_present(GTK_WINDOW(app->library_window));
        return;
    }

    /* Default window icon: the app logo from the icons/ folder.            */
    gchar *icon_path = g_build_filename(app->icons_dir, "composition.png",
                                        NULL);
    gtk_window_set_default_icon_from_file(icon_path, NULL);
    g_free(icon_path);

    /* Bundled scalable theme icons (icons/theme/hicolor/...): provides
     * SVG pan-*-symbolic arrows so tree expanders render crisply on
     * HiDPI displays instead of GTK's built-in 1x raster fallbacks.        */
    gchar *theme_dir = g_build_filename(app->icons_dir, "theme", NULL);
    gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(),
                                       theme_dir);
    g_free(theme_dir);

    /* Hide the touch aids (selection handles, magnifier) unless enabled.   */
    on_app_apply_touch_assist(app);

    /* DB integrity check: run PRAGMA integrity_check + foreign_key_check.  */
    gboolean db_ok = !app->db_integrity_check || startup_integrity_check(app);

    on_library_window_create(app);

    if (app->db_integrity_check && db_ok)
        on_app_status(app, "DB at %s loaded, integrity check passed",
                      app->db->path);

    /* Listen for "quicknote"/"note open" from later CLI invocations, then
     * run any action a CLI already queued because no instance was running.  */
    on_ipc_server_start(app);
    on_ipc_run_pending(app);

#ifdef HAVE_GTKOSX
    /* Honor the persisted native-menu-bar preference, then let the macOS
     * integration finish its launch handshake.                             */
    if (on_app_config_get_bool("native_menubar", FALSE))
        on_library_apply_native_menubar(app, TRUE);
    gtkosx_application_ready(gtkosx_application_get());
#endif
}

/* ---------------------------------------------------------------------------
 * on_sigterm() — terminate gracefully on SIGTERM (pkill, logout, system
 * shutdown): destroying every window flushes editor autosaves and lets
 * the main loop end cleanly.
 * ------------------------------------------------------------------------- */
static gboolean
on_sigterm(gpointer user_data)
{
    OnApp *app = user_data;          /* shared application context          */
    GList *windows =                 /* copy: destroying mutates the list   */
        g_list_copy(gtk_application_get_windows(app->gtk_app));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(windows);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * main() — set up the application context, run the GTK main loop, and
 * tear everything down afterwards.
 *   argc/argv — standard program arguments (GTK consumes its own flags).
 * Returns the process exit status.
 * ------------------------------------------------------------------------- */
int
main(int argc, char *argv[])
{
#ifdef __APPLE__
    /* Silence one benign, upstream GDK-Quartz clipboard critical (see
     * quartz_log_filter).  Installed before GTK so it covers every paste.   */
    g_log_set_handler("Gdk",
                      G_LOG_LEVEL_CRITICAL | G_LOG_FLAG_RECURSION,
                      quartz_log_filter, NULL);
#endif

    /* The application config (notes.ini) lives next to the
     * binary; resolve its location before anything reads it.               */
    on_app_config_init(argv[0]);

    /* Headless automation: a recognized subcommand runs and exits
     * without ever creating windows (see cli.h for the command list).      */
    int cli_status = on_cli_run(argc, argv);
    if (cli_status >= 0)
        return cli_status;

    /* Classic full-width scrollbars everywhere instead of the modern
     * overlay style (must be set before GTK initializes).                  */
    g_setenv("GTK_OVERLAY_SCROLLING", "0", TRUE);

    /* Touch assistance disabled (the default): take X11 CORE pointer
     * events only.  GDK then never classifies the pointer as a
     * touchscreen — some Linux input stacks (VM tablet devices, certain
     * trackpads) otherwise make GTK pop up its touch aids for plain
     * mouse input: selection handles, the magnifier, and the tap
     * cut/copy/paste bubble.  The bubble in particular cannot be hidden
     * with CSS (its buttons would stay clickable while invisible), so
     * this is the one lever that kills all three at the source.  Costs
     * XInput2 niceties (pixel-smooth scrolling); ignored on non-X11
     * backends.  Must be set before GTK initializes, so the Settings
     * toggle fully applies on the next start (the CSS half of
     * on_app_apply_touch_assist still applies live).                       */
    if (!on_app_config_get_bool("touch_assist", FALSE))
        g_setenv("GDK_CORE_DEVICE_EVENTS", "1", TRUE);

    /* Open (or create) the notes database first — without it there is
     * nothing to show.  A custom location (e.g. a shared folder) may be
     * configured in the config file.  There is deliberately NO fallback
     * to another location: silently opening a different database once
     * made a user's notes "disappear" (a startup racing the previous
     * instance's shutdown flush).  One configured database, or a clear
     * error.                                                               */
    gchar *db_dir = on_app_config_load_db_dir();
    gchar *db_path = (db_dir != NULL)
                     ? g_build_filename(db_dir, ON_DB_FILENAME, NULL)
                     : NULL;

    /* First launch (or an emptied configured directory): no database at
     * the expected location.  Ask before creating one — the user may mean
     * to open an existing file elsewhere.  Needs GTK up early for the
     * dialog; skipped without a display (old create-silently behavior).    */
    {
        gchar *expected = (db_path != NULL)  /* where the db was looked for */
                          ? g_strdup(db_path)
                          : on_db_default_path();
        if (!g_file_test(expected, G_FILE_TEST_EXISTS) &&
            gtk_init_check(&argc, &argv) &&
            !startup_first_run(expected, &db_dir, &db_path)) {
            g_free(expected);
            g_free(db_path);
            g_free(db_dir);
            return 0;                /* user closed the dialog — quit       */
        }
        g_free(expected);
    }

    OnDatabase *db = on_db_open(db_path);
    if (db == NULL) {
        g_printerr("notes: could not open the notes database at "
                   "%s\n(if another instance is still shutting down, "
                   "try again in a few seconds)\n",
                   db_path != NULL ? db_path : "the default location");
        g_free(db_path);
        g_free(db_dir);
        return 1;
    }
    g_free(db_path);
    on_app_actions_backfill(db);     /* one-time '!'-line index (gated)     */
    on_app_action_uids_backfill(db); /* then give those rows stable ids     */

    /* The shared context handed to every window.                           */
    OnApp app = {
        .gtk_app              = NULL,
        .db                   = db,
        /* Map of note id -> open editor window.  Keys are heap-allocated
         * gint64s owned (and freed) by the table itself.                   */
        .editors              = g_hash_table_new_full(g_int64_hash,
                                                      g_int64_equal,
                                                      g_free, NULL),
        .library_window       = NULL,
        .notify_notes_changed = NULL,
        .icons_dir            = NULL,
        .db_dir               = NULL,
    };
    app.db_dir = db_dir;             /* ownership transferred               */
    on_app_init_icons_dir(&app, argv[0]);

    /* Boolean preferences (second argument = default when unset).          */
    app.code_copy_buttons =
        on_app_config_get_bool("code_copy_button",       TRUE);
    app.code_line_numbers =
        on_app_config_get_bool("code_line_numbers",      FALSE);
    app.sidebar_counts =
        on_app_config_get_bool("sidebar_counts",         FALSE);
    app.first_line_title =
        on_app_config_get_bool("first_line_title",       TRUE);
    app.compact_editor_toolbar =
        on_app_config_get_bool("compact_editor_toolbar", TRUE);
    app.comfortable_list =
        on_app_config_get_bool("list_density_comfortable", FALSE);
    app.db_integrity_check =
        on_app_config_get_bool("db_integrity_check",     TRUE);
    app.statusbar_db_path =
        on_app_config_get_bool("statusbar_db_path",      FALSE);
    app.statusbar_note_id =
        on_app_config_get_bool("statusbar_note_id",      FALSE);
    app.show_done_actions =
        on_app_config_get_bool("show_done_actions",      TRUE);
    app.bold_list_titles =
        on_app_config_get_bool("bold_list_titles",       TRUE);
    app.ai_enabled =
        on_app_config_get_bool("ai_enabled",             FALSE);
    app.ai_command       = on_app_config_get("ai_command");
    app.ai_custom_prompt = on_app_config_get("ai_custom_prompt");

    app.gtk_app = gtk_application_new("org.example.notes",
                                      G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app.gtk_app, "activate",
                     G_CALLBACK(on_activate), &app);
    g_unix_signal_add(SIGTERM, on_sigterm, &app);

    /* A "quicknote"/"note open" that found no running instance falls
     * through to here to start the GUI.  Those leftover words are not GTK
     * options — run with a clean argv so GApplication doesn't try to open
     * them as files ("This application can not open files").                */
    int status;                      /* main-loop exit status               */
    if (on_ipc_has_pending()) {
        char *solo_argv[] = { argv[0], NULL };
        status = g_application_run(G_APPLICATION(app.gtk_app), 1, solo_argv);
    } else {
        status = g_application_run(G_APPLICATION(app.gtk_app), argc, argv);
    }

    /* Stop serving CLI commands and unlink the socket before teardown.      */
    on_ipc_server_stop();

    /* Windows (and their final autosaves) are done by the time run()
     * returns, so the database can be closed and hashed safely now.        */
    g_object_unref(app.gtk_app);
    g_hash_table_destroy(app.editors);

    on_db_close(app.db);

    g_free(app.icons_dir);
    g_free(app.db_dir);
    return status;
}
