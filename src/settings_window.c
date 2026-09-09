/* ===========================================================================
 * settings_window.c — the application settings window (implementation)
 *
 * See settings_window.h for the overview.  Every control writes through
 * to the OnApp setters, which persist to the notes.ini config file
 * and live-update all open windows, so there is no OK/Apply button.
 * =========================================================================== */

#include "settings_window.h"
#include "editor_window.h"
#include "library_window.h"

#include <stddef.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * svg_loader_available() — TRUE if gdk-pixbuf can decode SVG files (i.e.
 * the librsvg loader is installed).  Used to warn about the elementary
 * theme, which is SVG-only.
 * ------------------------------------------------------------------------- */
static gboolean
svg_loader_available(void)
{
    GSList *formats = gdk_pixbuf_get_formats();
    gboolean found = FALSE;          /* did we see an svg decoder?          */
    for (GSList *l = formats; l != NULL && !found; l = l->next) {
        gchar *name = gdk_pixbuf_format_get_name(l->data);
        found = g_ascii_strcasecmp(name, "svg") == 0;
        g_free(name);
    }
    g_slist_free(formats);
    return found;
}

/* ---------------------------------------------------------------------------
 * on_density_combo_changed() — List Density combo changed: update the field,
 * persist, and trigger a full notes-list refresh so row heights update.
 * ------------------------------------------------------------------------- */
static void
on_density_combo_changed(GtkComboBox *combo, gpointer user_data)
{
    OnApp *app = user_data;            /* application context                 */
    app->comfortable_list = (gtk_combo_box_get_active(combo) == 1);
    on_app_config_set("list_density_comfortable",
                      app->comfortable_list ? "1" : "0");
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
}

/* apply_notes_changed() — fire the library's full refresh, if installed.    */
static void
apply_notes_changed(OnApp *app)
{
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
}

/* apply_statusbar_db_path() — re-render every window's status-bar path.     */
static void
apply_statusbar_db_path(OnApp *app)
{
    apply_notes_changed(app);        /* the library's path label            */
    on_editor_status_refresh_all(app);
}

/* ---------------------------------------------------------------------------
 * BOOL_SETTINGS — the simple on/off preferences: one checkbox each, one
 * OnApp gboolean field each, persisted under `key`, with an optional
 * live-apply hook.  bool_check_new() builds the checkbox; on_bool_toggled
 * (shared by all of them) reads the spec back off the widget.  Settings
 * with extra behavior (touch assist's inverted sense, the gtkosx native
 * menubar) stay hand-written below.
 * ------------------------------------------------------------------------- */
typedef enum {
    BS_SIDEBAR_COUNTS,               /* Appearance                          */
    BS_BOLD_LIST_TITLES,
    BS_SHOW_DONE_ACTIONS,
    BS_CODE_COPY,                    /* Editor                              */
    BS_CODE_LINES,
    BS_FIRST_LINE_TITLE,
    BS_COMPACT_TOOLBAR,
    BS_STATUSBAR_NOTE_ID,
    BS_STATUSBAR_DB_PATH,            /* Appearance                          */
    BS_DB_INTEGRITY,                 /* Database                            */
} BoolSettingId;

typedef struct {
    const gchar *label;              /* checkbox text                       */
    const gchar *key;                /* ini key                             */
    gsize        field_off;          /* offsetof(OnApp, <field>)            */
    void       (*apply)(OnApp *app); /* live-apply hook, or NULL            */
} BoolSetting;

static const BoolSetting BOOL_SETTINGS[] = {
    [BS_SIDEBAR_COUNTS] = {
        "Show note counts next to folders and tags",
        "sidebar_counts", offsetof(OnApp, sidebar_counts),
        apply_notes_changed },
    [BS_BOLD_LIST_TITLES] = {
        "Bold titles in comfortable list density",
        "bold_list_titles", offsetof(OnApp, bold_list_titles),
        apply_notes_changed },
    [BS_SHOW_DONE_ACTIONS] = {
        "Show completed action items",
        "show_done_actions", offsetof(OnApp, show_done_actions),
        apply_notes_changed },
    [BS_CODE_COPY] = {
        "Show copy button on code blocks",
        "code_copy_button", offsetof(OnApp, code_copy_buttons),
        on_editor_rebuild_code_buttons_all },
    [BS_CODE_LINES] = {
        "Show line numbers in code blocks",
        "code_line_numbers", offsetof(OnApp, code_line_numbers),
        on_editor_apply_line_numbers_all },
    [BS_FIRST_LINE_TITLE] = {
        "First line is Title formatted",
        "first_line_title", offsetof(OnApp, first_line_title),
        on_editor_title_refresh_all },
    [BS_COMPACT_TOOLBAR] = {
        "Compact toolbar (group paragraph styles and lists into menus)",
        "compact_editor_toolbar", offsetof(OnApp, compact_editor_toolbar),
        on_editor_rebuild_toolbars_all },
    [BS_STATUSBAR_NOTE_ID] = {
        "Show note id in the editor status bar",
        "statusbar_note_id", offsetof(OnApp, statusbar_note_id),
        on_editor_status_refresh_all },
    [BS_DB_INTEGRITY] = {
        "Check database integrity on startup (PRAGMA integrity_check)",
        "db_integrity_check", offsetof(OnApp, db_integrity_check), NULL },
    [BS_STATUSBAR_DB_PATH] = {
        "Show database path prefix in status bar",
        "statusbar_db_path", offsetof(OnApp, statusbar_db_path),
        apply_statusbar_db_path },
};

/* on_bool_toggled() — shared handler: flip the field, persist, apply.       */
static void
on_bool_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    const BoolSetting *bs = g_object_get_data(G_OBJECT(check), "on-spec");
    gboolean *field = (gboolean *)((gchar *)app + bs->field_off);
    *field = gtk_toggle_button_get_active(check);
    on_app_config_set(bs->key, *field ? "1" : "0");
    if (bs->apply != NULL)
        bs->apply(app);
}

/* bool_check_new() — build the checkbox for one BOOL_SETTINGS entry.        */
static GtkWidget *
bool_check_new(OnApp *app, BoolSettingId id)
{
    const BoolSetting *bs = &BOOL_SETTINGS[id];
    GtkWidget *check = gtk_check_button_new_with_label(bs->label);
    gtk_widget_set_margin_start(check, 12);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(check),
        *(gboolean *)((gchar *)app + bs->field_off));
    g_object_set_data(G_OBJECT(check), "on-spec", (gpointer)bs);
    g_signal_connect(check, "toggled", G_CALLBACK(on_bool_toggled), app);
    return check;
}

#ifdef HAVE_GTKOSX
/* on_native_menubar_toggled() — move the library menu into (or out of)
 * the native macOS menu bar, live.                                          */
static void
on_native_menubar_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    gboolean native = gtk_toggle_button_get_active(check);
    on_app_config_set("native_menubar", native ? "1" : "0");
    on_library_apply_native_menubar(app, native);
}
#endif /* HAVE_GTKOSX */

/* ---------------------------------------------------------------------------
 * DbSection — widgets of the "Database" settings block, so its handlers
 * can keep the labels in sync after a location switch.
 *
 * Fields:
 *   app        — application context.
 *   check      — "custom folder" checkbox.
 *   choose_btn — folder-picker button (sensitive only when custom).
 *   path_label — shows the active database file path.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp     *app;
    GtkWidget *check;
    GtkWidget *choose_btn;
    GtkWidget *path_label;
} DbSection;

/* db_section_refresh() — sync the database widgets with reality.            */
static void
db_section_refresh(DbSection *s)
{
    gchar *markup = g_markup_printf_escaped(
        "<small>%s: %s\n"
        "<i>Do not open the same database from two machines at the same "
        "time.</i></small>",
        s->app->db_dir != NULL ? "Custom database" : "Default database",
        s->app->db->path);
    gtk_label_set_markup(GTK_LABEL(s->path_label), markup);
    g_free(markup);
    gtk_widget_set_sensitive(s->choose_btn, s->app->db_dir != NULL);
}

static void on_db_custom_toggled(GtkToggleButton *check,
                                 gpointer user_data);

/* db_switch_report() — run the switch (it reports its own errors and
 * asks about existing databases) and re-sync this section's widgets with
 * whatever actually happened — a cancelled or failed switch leaves the
 * old location active.                                                      */
static void
db_switch_report(DbSection *s, const gchar *new_dir)
{
    on_app_switch_database(s->app, new_dir);
    g_signal_handlers_block_by_func(s->check, on_db_custom_toggled, s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->check),
                                 s->app->db_dir != NULL);
    g_signal_handlers_unblock_by_func(s->check, on_db_custom_toggled, s);
    db_section_refresh(s);
}

/* db_pick_folder() — folder chooser; returns a new path or NULL.            */
static gchar *
db_pick_folder(DbSection *s)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Database Folder",
        GTK_WINDOW(gtk_widget_get_toplevel(s->check)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    if (s->app->db_dir != NULL)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                            s->app->db_dir);
    gchar *dir = NULL;               /* the picked folder, if any           */
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return dir;
}

/* on_db_custom_toggled() — enable/disable the custom database location.     */
static void
on_db_custom_toggled(GtkToggleButton *check, gpointer user_data)
{
    DbSection *s = user_data;        /* the database section                */
    gboolean want_custom = gtk_toggle_button_get_active(check);

    if (want_custom && s->app->db_dir == NULL) {
        /* Enabling needs a folder right away.                              */
        gchar *dir = db_pick_folder(s);
        if (dir == NULL) {
            /* Cancelled: silently revert the checkbox.                     */
            g_signal_handlers_block_by_func(check,
                                            on_db_custom_toggled, s);
            gtk_toggle_button_set_active(check, FALSE);
            g_signal_handlers_unblock_by_func(check,
                                              on_db_custom_toggled, s);
            return;
        }
        db_switch_report(s, dir);
        g_free(dir);
    } else if (!want_custom && s->app->db_dir != NULL) {
        db_switch_report(s, NULL);   /* back to the default location        */
    }
}

/* on_db_choose_clicked() — re-pick the custom folder.                       */
static void
on_db_choose_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;        /* the database section                */
    gchar *dir = db_pick_folder(s);
    if (dir != NULL) {
        db_switch_report(s, dir);
        g_free(dir);
    }
}

/* on_viewer_entry_changed() — persist the image-viewer program path as
 * the user types; an empty entry clears the setting so "Open" on an
 * image falls back to the system default viewer.                            */
static void
on_viewer_entry_changed(GtkEditable *editable, gpointer user_data)
{
    (void)user_data;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(editable));
    if (text != NULL && *text != '\0')
        on_app_config_set("image_viewer", text);
    else
        on_app_config_set("image_viewer", NULL);
}

/* on_viewer_browse_clicked() — pick the viewer program with a file
 * chooser; the entry's "changed" handler does the persisting.               */
static void
on_viewer_browse_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *entry = user_data;    /* the viewer-path entry               */
    gchar *file = on_app_pick_path(
        GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(btn))),
        "Choose Image Viewer", GTK_FILE_CHOOSER_ACTION_OPEN, "_Select",
        NULL, NULL);
    if (file != NULL) {
        gtk_entry_set_text(GTK_ENTRY(entry), file);
        g_free(file);
    }
}

/* on_touch_assist_toggled() — the checkbox DISABLES the touch aids, so
 * active = touch_assist off.  The CSS half (handles, magnifier) applies
 * live; the tap-popup half is the GDK_CORE_DEVICE_EVENTS env var in
 * main(), which only takes effect on the next start.                        */
static void
on_touch_assist_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;          /* application context                 */
    on_app_config_set("touch_assist",
                      gtk_toggle_button_get_active(check) ? "0" : "1");
    on_app_apply_touch_assist(app);
    on_app_status(app, "Touch assistance fully applies after a restart");
}

/* ---------------------------------------------------------------------------
 * section_label() — bold section header for the settings layout.
 * ------------------------------------------------------------------------- */
static GtkWidget *
section_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    return label;
}

/* on_ai_enabled_toggled() — master AI kill switch.                          */
static void
on_ai_enabled_toggled(GtkToggleButton *check, gpointer user_data)
{
    OnApp *app = user_data;
    app->ai_enabled = gtk_toggle_button_get_active(check);
    on_app_config_set("ai_enabled", app->ai_enabled ? "1" : "0");
    GtkWidget *sub = g_object_get_data(G_OBJECT(check), "on-ai-sub");
    if (sub != NULL)
        gtk_widget_set_sensitive(sub, app->ai_enabled);
    if (app->notify_ai_changed != NULL)
        app->notify_ai_changed(app);
}

/* on_ai_custom_prompt_changed() — persist the custom AI prompt as it changes.*/
static void
on_ai_custom_prompt_changed(GtkTextBuffer *buf, gpointer user_data)
{
    OnApp *app = user_data;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    g_free(app->ai_custom_prompt);
    app->ai_custom_prompt = (*text != '\0') ? g_strdup(text) : NULL;
    g_free(text);
    on_app_config_set("ai_custom_prompt", app->ai_custom_prompt);
}

/* on_ai_command_changed() — persist the AI command path as the user types.  */
static void
on_ai_command_changed(GtkEditable *editable, gpointer user_data)
{
    OnApp *app = user_data;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(editable));
    g_free(app->ai_command);
    app->ai_command = (text != NULL && *text != '\0')
        ? g_strdup(text) : NULL;
    on_app_config_set("ai_command",
                      app->ai_command != NULL ? app->ai_command : NULL);
}


void
on_settings_window_open(OnApp *app)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Notes - Settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 210, -1);
    gtk_window_set_transient_for(GTK_WINDOW(window),
                                 GTK_WINDOW(app->library_window));
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);

    /* Wrap in a scrolled window so the settings are reachable on low-res
     * screens.  propagate_natural_height + max_content_height lets the
     * window size to content when content fits, and scroll when it doesn't. */
    GtkWidget *outer_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(outer_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(outer_scroll), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(outer_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(outer_scroll), 600);
    gtk_container_add(GTK_CONTAINER(outer_scroll), vbox);
    gtk_container_add(GTK_CONTAINER(window), outer_scroll);

    /* --- appearance ----------------------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Appearance"),
                       FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_start(grid, 12);

    GtkWidget *density_label = gtk_label_new("List density:");
    gtk_label_set_xalign(GTK_LABEL(density_label), 0.0);
    gtk_grid_attach(GTK_GRID(grid), density_label, 0, 0, 1, 1);
    GtkWidget *density_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(density_combo),
                                   "Compact");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(density_combo),
                                   "Comfortable");
    gtk_combo_box_set_active(GTK_COMBO_BOX(density_combo),
                             app->comfortable_list ? 1 : 0);
    g_signal_connect(density_combo, "changed",
                     G_CALLBACK(on_density_combo_changed), app);
    gtk_grid_attach(GTK_GRID(grid), density_combo, 1, 0, 1, 1);

    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       bool_check_new(app, BS_SIDEBAR_COUNTS),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       bool_check_new(app, BS_BOLD_LIST_TITLES),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       bool_check_new(app, BS_SHOW_DONE_ACTIONS),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       bool_check_new(app, BS_STATUSBAR_DB_PATH),
                       FALSE, FALSE, 0);

#ifdef __APPLE__
    /* Native macOS menu bar belongs with the other appearance choices.     */
    GtkWidget *mac_check = gtk_check_button_new_with_label(
        "Use the native macOS menu bar (hide the in-window menu)");
    gtk_widget_set_margin_start(mac_check, 12);
#ifdef HAVE_GTKOSX
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(mac_check),
        on_app_config_get_bool("native_menubar", FALSE));
    g_signal_connect(mac_check, "toggled",
                     G_CALLBACK(on_native_menubar_toggled), app);
#else
    gtk_widget_set_sensitive(mac_check, FALSE);
    gtk_widget_set_tooltip_text(mac_check,
        "Requires the gtk-mac-integration library:\n"
        "sudo port install gtk-osx-application-gtk3, then rebuild "
        "(make clean && make)");
#endif
    gtk_box_pack_start(GTK_BOX(vbox), mac_check, FALSE, FALSE, 0);
#endif /* __APPLE__ */

    /* The bundled symbolic tree arrows are still SVG: mention the loader
     * when it is missing.  Everything still works without it — those
     * icons just fall back to the theme defaults.                          */
    if (!svg_loader_available()) {
        GtkWidget *warn = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(warn),
            "<small><i>Some icons (tree arrows) render best "
            "with the librsvg loader:\nsudo port install librsvg "
            "(then restart Notes)</i></small>");
        gtk_label_set_xalign(GTK_LABEL(warn), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(warn), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(warn), 40);
        gtk_widget_set_margin_start(warn, 12);
        gtk_box_pack_start(GTK_BOX(vbox), warn, FALSE, FALSE, 2);
    }

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    /* --- editor options ------------------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Editor"),
                       FALSE, FALSE, 0);

    /* The five table-driven editor checkboxes, in display order.           */
    static const BoolSettingId EDITOR_CHECKS[] = {
        BS_CODE_COPY, BS_CODE_LINES, BS_FIRST_LINE_TITLE, BS_COMPACT_TOOLBAR,
        BS_STATUSBAR_NOTE_ID,
    };
    for (gsize i = 0; i < G_N_ELEMENTS(EDITOR_CHECKS); i++)
        gtk_box_pack_start(GTK_BOX(vbox),
                           bool_check_new(app, EDITOR_CHECKS[i]),
                           FALSE, FALSE, 0);

    GtkWidget *touch_check = gtk_check_button_new_with_label(
        "Disable touch assistance (selection handles, magnifier, "
        "tap popup)");
    gtk_widget_set_tooltip_text(touch_check,
        "Hides the touch aids GTK pops up under text selections.\n"
        "The tap cut/copy/paste popup needs a restart to change.");
    gtk_widget_set_margin_start(touch_check, 12);
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(touch_check),
        on_app_config_get_bool("touch_assist", FALSE));
    g_signal_connect(touch_check, "toggled",
                     G_CALLBACK(on_touch_assist_toggled), app);
    gtk_box_pack_start(GTK_BOX(vbox), touch_check, FALSE, FALSE, 0);

    /* Program used by an image's "Open" action; empty = system default.   */
    GtkWidget *viewer_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(viewer_row, 12);
    GtkWidget *viewer_label = gtk_label_new("Image viewer:");
    gtk_box_pack_start(GTK_BOX(viewer_row), viewer_label, FALSE, FALSE, 0);

    GtkWidget *viewer_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(viewer_entry),
                                   "System default");
    {
        gchar *viewer = on_app_config_get("image_viewer");
        if (viewer != NULL) {
            gtk_entry_set_text(GTK_ENTRY(viewer_entry), viewer);
            g_free(viewer);
        }
    }
    g_signal_connect(viewer_entry, "changed",
                     G_CALLBACK(on_viewer_entry_changed), app);
    gtk_box_pack_start(GTK_BOX(viewer_row), viewer_entry, TRUE, TRUE, 0);

    GtkWidget *viewer_btn = gtk_button_new_with_label(
        "Browse\xe2\x80\xa6");
    g_signal_connect(viewer_btn, "clicked",
                     G_CALLBACK(on_viewer_browse_clicked), viewer_entry);
    gtk_box_pack_start(GTK_BOX(viewer_row), viewer_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), viewer_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    /* --- database location ---------------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Database"),
                       FALSE, FALSE, 0);

    DbSection *dbs = g_new0(DbSection, 1);
    dbs->app = app;
    /* Freed with the window.                                               */
    g_object_set_data_full(G_OBJECT(window), "on-db-section", dbs, g_free);

    dbs->check = gtk_check_button_new_with_label(
        "Store the database in a custom folder (e.g. a shared drive)");
    gtk_widget_set_margin_start(dbs->check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dbs->check),
                                 app->db_dir != NULL);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->check, FALSE, FALSE, 0);

    GtkWidget *db_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(db_row, 12);
    dbs->choose_btn = gtk_button_new_with_label(
        "Choose Folder\xe2\x80\xa6");
    gtk_box_pack_start(GTK_BOX(db_row), dbs->choose_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), db_row, FALSE, FALSE, 0);

    dbs->path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(dbs->path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dbs->path_label), 40);
    gtk_widget_set_margin_start(dbs->path_label, 12);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->path_label, FALSE, FALSE, 0);

    db_section_refresh(dbs);
    g_signal_connect(dbs->check, "toggled",
                     G_CALLBACK(on_db_custom_toggled), dbs);
    g_signal_connect(dbs->choose_btn, "clicked",
                     G_CALLBACK(on_db_choose_clicked), dbs);

    gtk_box_pack_start(GTK_BOX(vbox), bool_check_new(app, BS_DB_INTEGRITY),
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 4);

    /* --- AI features ---------------------------------------------------------*/
    gtk_box_pack_start(GTK_BOX(vbox), section_label("AI Features"),
                       FALSE, FALSE, 0);

    GtkWidget *ai_desc = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ai_desc),
        "<small>The AI button in the library toolbar generates a summary of "
        "the current folder\xe2\x80\x99s notes using an external AI command. "
        "Normal or Project mode is set per-folder in the New/Rename Folder "
        "dialog. This switch is a master kill switch: disabling it hides the "
        "button and prevents all AI commands from running.</small>");
    gtk_label_set_xalign(GTK_LABEL(ai_desc), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(ai_desc), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(ai_desc), 40);
    gtk_widget_set_margin_start(ai_desc, 12);
    gtk_box_pack_start(GTK_BOX(vbox), ai_desc, FALSE, FALSE, 2);

    GtkWidget *ai_check = gtk_check_button_new_with_label(
        "Enable AI features");
    gtk_widget_set_margin_start(ai_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ai_check),
                                 app->ai_enabled);
    gtk_box_pack_start(GTK_BOX(vbox), ai_check, FALSE, FALSE, 0);

    /* Sub-group (command), sensitive only when AI is enabled.                */
    GtkWidget *ai_sub = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(ai_sub, 24);
    gtk_widget_set_sensitive(ai_sub, app->ai_enabled);
    gtk_box_pack_start(GTK_BOX(vbox), ai_sub, FALSE, FALSE, 0);

    GtkWidget *cmd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *cmd_label = gtk_label_new("AI command:");
    gtk_label_set_xalign(GTK_LABEL(cmd_label), 0.0);
    gtk_box_pack_start(GTK_BOX(cmd_row), cmd_label, FALSE, FALSE, 0);

    GtkWidget *cmd_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cmd_entry),
                                   "e.g. /usr/local/bin/claude");
    if (app->ai_command != NULL)
        gtk_entry_set_text(GTK_ENTRY(cmd_entry), app->ai_command);
    gtk_box_pack_start(GTK_BOX(cmd_row), cmd_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(ai_sub), cmd_row, FALSE, FALSE, 0);

    GtkWidget *cmd_hint = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(cmd_hint),
        "<small><i>Run <b>which claude</b> in a terminal to find your "
        "Claude Code command path. The command must read the prompt from "
        "stdin and write the response to stdout.</i></small>");
    gtk_label_set_xalign(GTK_LABEL(cmd_hint), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(cmd_hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(cmd_hint), 40);
    gtk_box_pack_start(GTK_BOX(ai_sub), cmd_hint, FALSE, FALSE, 0);

    GtkWidget *custom_lbl = gtk_label_new("Custom prompt:");
    gtk_label_set_xalign(GTK_LABEL(custom_lbl), 0.0);
    gtk_widget_set_margin_top(custom_lbl, 4);
    gtk_box_pack_start(GTK_BOX(ai_sub), custom_lbl, FALSE, FALSE, 0);

    GtkWidget *custom_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(custom_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(custom_view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(custom_view), 4);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(custom_view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(custom_view), 4);
    GtkTextBuffer *custom_buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(custom_view));
    if (app->ai_custom_prompt != NULL)
        gtk_text_buffer_set_text(custom_buf, app->ai_custom_prompt, -1);
    GtkWidget *custom_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(custom_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(custom_scroll), FALSE);
    gtk_widget_set_size_request(custom_scroll, -1, 72);
    gtk_container_add(GTK_CONTAINER(custom_scroll), custom_view);
    gtk_box_pack_start(GTK_BOX(ai_sub), custom_scroll, FALSE, FALSE, 0);

    GtkWidget *custom_hint = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(custom_hint),
        "<small><i>Used when a folder\xe2\x80\x99s AI mode is set to "
        "Custom. Notes and action items are appended after this prompt."
        "</i></small>");
    gtk_label_set_xalign(GTK_LABEL(custom_hint), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(custom_hint), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(custom_hint), 40);
    gtk_box_pack_start(GTK_BOX(ai_sub), custom_hint, FALSE, FALSE, 0);

    g_signal_connect(ai_check, "toggled",
                     G_CALLBACK(on_ai_enabled_toggled), app);
    g_object_set_data(G_OBJECT(ai_check), "on-ai-sub", ai_sub);
    g_signal_connect(cmd_entry, "changed",
                     G_CALLBACK(on_ai_command_changed), app);
    g_signal_connect(custom_buf, "changed",
                     G_CALLBACK(on_ai_custom_prompt_changed), app);

    gtk_widget_show_all(window);
}
