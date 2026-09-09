/* ===========================================================================
 * search_window.c — the note search window (implementation)
 *
 * The query itself is parsed by search_query.[ch] — quoted phrases,
 * '-' exclusions and implicit AND between terms, or one GRegex pattern
 * when the Regular expression box is ticked — and the resulting OnQuery
 * does all the matching, exactly as it does for the headless `search`
 * command.
 *
 * Note bodies come from the notes.body_text cache column so a search
 * never decodes images or builds text buffers.  Rows saved before the
 * column existed have a NULL cache; those fall back to a cheap scan of
 * the BNBF blob (on_note_extract_text) and the result is written back,
 * so the first search after upgrading backfills the cache.
 *
 * Searches run OFF the GTK main thread so the GUI never blocks: the
 * scope is resolved up front (it reads library widgets), then a worker
 * thread with its OWN SQLite connection (one connection must not be
 * shared across threads) does all the reading and matching while a
 * spinner runs in the window.  The worker hands its results back via
 * g_idle_add; a cancelled flag lets a new search, an empty query, or
 * closing the window abandon an in-flight job — the job always frees
 * itself on the main thread after checking that flag, so it never
 * touches a destroyed window.
 * =========================================================================== */

#include "search_window.h"
#include "search_query.h"
#include "serialize.h"
#include "editor_window.h"
#include "library_window.h"

#include <string.h>

/* Result-list store columns.                                                */
enum {
    SR_ID,                           /* gint64: note id                     */
    SR_PATH,                         /* gchar*: /Folder/Sub/Title path      */
    SR_MODIFIED,                     /* gchar*: formatted updated_at        */
    SR_N_COLS
};

typedef struct SearchJob SearchJob;  /* forward: one in-flight search       */

/* ---------------------------------------------------------------------------
 * OnSearch — all state for one search window.
 *
 * Fields:
 *   app           — global application context (not owned).
 *   window        — the search window itself.
 *   entry         — the query text entry.
 *   radio_all     — "All Notes" scope radio button.
 *   radio_scoped  — "Selected Folder/Tag" radio button; resolved against
 *                   the library's live selection on every search.
 *   check_case    — "Case sensitive" checkbox.
 *   check_regex   — "Regular expression" checkbox.
 *   store         — results list model.
 *   status        — label under the results showing match counts/errors.
 *   spinner       — GtkSpinner next to the Search button; visible and
 *                   spinning while a worker thread is searching.
 *   job           — the in-flight search, or NULL when idle (the job is
 *                   owned by its worker/idle chain, never freed here).
 *   highlight     — the last search's highlight term (owned), carried into
 *                   the editor when a result is opened.  Taken from the
 *                   parsed query, not the entry text, so opening a hit for
 *                   `cats -dogs` seeds the in-note search with "cats".
 *   win_w/win_h   — the window's current size, tracked by configure
 *                   events and persisted on close so the next search
 *                   window opens at the size this one was left at.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp         *app;
    GtkWidget     *window;
    GtkWidget     *entry;
    GtkWidget     *radio_all;
    GtkWidget     *radio_scoped;
    GtkWidget     *check_case;
    GtkWidget     *check_regex;
    GtkListStore  *store;
    GtkWidget     *status;
    GtkWidget     *spinner;
    SearchJob     *job;
    gchar         *highlight;
    gint           win_w;
    gint           win_h;
} OnSearch;

/* Above this many candidate notes, one bulk body_text query beats reading
 * the rows one at a time; at or below it the bulk fetch would pull the whole
 * column to inspect a handful of notes (a folder- or tag-scoped search).    */
#define SEARCH_BULK_TEXT_MIN 64

/* Fallback dimensions before any search window has been resized.            */
#define SEARCH_WIN_DEFAULT_W 575
#define SEARCH_WIN_DEFAULT_H 360

/* ---------------------------------------------------------------------------
 * SearchHit — one matching note, fully formatted by the worker so the
 * main thread only copies strings into the list store.
 *
 * Fields:
 *   id   — note id (for opening on activation).
 *   path — "/Folder/Sub/Title" display path (owned).
 *   when — formatted updated_at (owned).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  id;
    gchar  *path;
    gchar  *when;
} SearchHit;

/* search_hit_free() — GDestroyNotify for the job's hits array.              */
static void
search_hit_free(gpointer data)
{
    SearchHit *h = data;
    g_free(h->path);
    g_free(h->when);
    g_free(h);
}

/* ---------------------------------------------------------------------------
 * SearchJob — everything one background search needs, snapshotted on the
 * main thread so the worker never touches GTK or the shared db handle.
 *
 * Fields:
 *   sw             — owning window; ONLY dereferenced on the main thread
 *                    and ONLY while `cancelled` is unset.
 *   cancelled      — set (atomically, from the main thread) when the
 *                    window closes or a newer search supersedes this one.
 *   db_path        — database file to open privately (owned).
 *   query          — the parsed query (owned); immutable, so the worker
 *                    thread may match with it freely.
 *   scoped/
 *   scope_tag/
 *   scope_id       — candidate-note scope resolved before the thread ran.
 *   scope_desc     — " in “name”" status suffix, or NULL (owned).
 *   hits           — SearchHit* results, filled by the worker.
 *   error          — worker-side failure message, or NULL (owned).
 * ------------------------------------------------------------------------- */
struct SearchJob {
    OnSearch  *sw;
    gint       cancelled;
    gchar     *db_path;
    OnQuery   *query;
    gboolean   scoped;
    gboolean   scope_tag;
    gint64     scope_id;
    gchar     *scope_desc;
    GPtrArray *hits;
    gchar     *error;
};

/* search_job_free() — release a job and everything it owns.                 */
static void
search_job_free(SearchJob *job)
{
    g_ptr_array_free(job->hits, TRUE);
    g_free(job->db_path);
    on_query_free(job->query);
    g_free(job->scope_desc);
    g_free(job->error);
    g_free(job);
}

/* search_job_cancel() — detach an in-flight job from its window.  The
 * worker/idle chain still owns the job and frees it; it just won't touch
 * the window any more.                                                      */
static void
search_job_cancel(OnSearch *sw)
{
    if (sw->job != NULL) {
        g_atomic_int_set(&sw->job->cancelled, 1);
        sw->job = NULL;
    }
}

/* ---------------------------------------------------------------------------
 * note_full_path() — build the "/Folder/Sub/Title" display path of one
 * result from the pre-fetched folder-path map (one query for the whole
 * search, never per-row — see on_db_folder_path_map).  Returns a newly
 * allocated string.
 * ------------------------------------------------------------------------- */
static gchar *
note_full_path(OnNoteMeta *m, GHashTable *paths)
{
    const gchar *fpath = g_hash_table_lookup(paths, &m->folder_id);
    return g_strdup_printf("%s/%s", fpath != NULL ? fpath : "", m->title);
}

/* ---------------------------------------------------------------------------
 * search_done() — idle callback on the main thread: deliver a finished
 * job's results to its window (unless the job was cancelled, in which
 * case the window is gone or has moved on) and free the job.
 * ------------------------------------------------------------------------- */
static gboolean
search_done(gpointer user_data)
{
    SearchJob *job = user_data;
    if (!g_atomic_int_get(&job->cancelled)) {
        OnSearch *sw = job->sw;      /* safe: not cancelled ⇒ window alive  */
        sw->job = NULL;
        gtk_spinner_stop(GTK_SPINNER(sw->spinner));
        gtk_widget_hide(sw->spinner);

        if (job->error != NULL) {
            gtk_label_set_text(GTK_LABEL(sw->status), job->error);
        } else {
            for (guint i = 0; i < job->hits->len; i++) {
                SearchHit *h = g_ptr_array_index(job->hits, i);
                GtkTreeIter iter;
                gtk_list_store_append(sw->store, &iter);
                gtk_list_store_set(sw->store, &iter,
                                   SR_ID,       h->id,
                                   SR_PATH,     h->path,
                                   SR_MODIFIED, h->when,
                                   -1);
            }
            gchar *msg = g_strdup_printf(
                "%u match%s%s", job->hits->len,
                job->hits->len == 1 ? "" : "es",
                job->scope_desc != NULL ? job->scope_desc : "");
            gtk_label_set_text(GTK_LABEL(sw->status), msg);
            g_free(msg);
        }
    }
    search_job_free(job);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * search_worker() — the search thread: open a private connection, walk
 * the candidate notes, and collect formatted hits.  Touches nothing of
 * the window; finishes by posting search_done() to the main loop.
 * ------------------------------------------------------------------------- */
static gpointer
search_worker(gpointer user_data)
{
    SearchJob *job = user_data;

    /* One SQLite connection must not be shared across threads, so the
     * worker opens its own (same file, same 5 s busy timeout).             */
    OnDatabase *db = on_db_open(job->db_path);
    if (db == NULL) {
        job->error = g_strdup("Could not open the database.");
        g_idle_add(search_done, job);
        return NULL;
    }

    GList *notes;                    /* OnNoteMeta* candidates              */
    if (!job->scoped)
        /* All-scope deliberately includes the Trash: deleted notes stay
         * findable until the Trash is emptied.                              */
        notes = on_db_note_list_all(db, TRUE);
    else if (job->scope_tag)
        notes = on_db_notes_by_tag(db, job->scope_id);
    else
        notes = on_db_note_list(db, job->scope_id);

    /* Folder-id → path strings, all fetched in one query.                  */
    GHashTable *paths = on_db_folder_path_map(db);

    /* Note bodies: ONE query for the whole column beats a SELECT per
     * candidate when searching everything — but a folder- or tag-scoped
     * search may have only a handful of candidates, and fetching all
     * ~1.5 MB of body_text for those was most of the work.  Above the
     * threshold the bulk map wins; below it, read the few notes directly.  */
    guint n_candidates = g_list_length(notes);
    GHashTable *bodies = (n_candidates > SEARCH_BULK_TEXT_MIN)
                         ? on_db_note_text_map(db, 0) : NULL;

    for (GList *l = notes; l != NULL; l = l->next) {
        if (g_atomic_int_get(&job->cancelled))
            break;                   /* superseded/closed: stop early       */
        OnNoteMeta *m = l->data;     /* one candidate                       */
        const gchar *body = (bodies != NULL)
                            ? g_hash_table_lookup(bodies, &m->id) : NULL;
        gchar *extracted = NULL;     /* small scope, or an uncached row     */
        if (body == NULL) {
            extracted = on_note_text_cached(db, m->id);
            body = extracted;
        }

        gboolean match =             /* does this note match the query?     */
            on_query_matches(job->query, m->title, body);
        g_free(extracted);
        if (!match)
            continue;

        GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
        SearchHit *h = g_new0(SearchHit, 1);
        h->id   = m->id;
        h->path = note_full_path(m, paths);
        h->when = g_date_time_format(dt, "%b %e, %Y %H:%M");
        g_date_time_unref(dt);
        g_ptr_array_add(job->hits, h);
    }
    if (bodies != NULL)
        g_hash_table_destroy(bodies);
    g_hash_table_destroy(paths);
    on_db_note_list_free(notes);
    on_db_close(db);

    g_idle_add(search_done, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * run_search() — validate the query, snapshot everything the worker
 * needs (scope resolution reads library widgets, so it happens here on
 * the main thread), and kick off the search thread.  Any search already
 * in flight is abandoned first.
 * ------------------------------------------------------------------------- */
static void
run_search(OnSearch *sw)
{
    /* A new request supersedes whatever is still running.                  */
    search_job_cancel(sw);
    gtk_spinner_stop(GTK_SPINNER(sw->spinner));
    gtk_widget_hide(sw->spinner);

    const gchar *query = gtk_entry_get_text(GTK_ENTRY(sw->entry));
    gtk_list_store_clear(sw->store);
    if (query == NULL || *query == '\0') {
        gtk_label_set_text(GTK_LABEL(sw->status), "Type something to search for.");
        return;
    }

    gboolean case_sensitive = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->check_case));
    gboolean use_regex = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->check_regex));
    gboolean scoped = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->radio_scoped));

    /* Parse (and, in regex mode, compile) up front so a bad pattern errors
     * immediately, on the main thread.                                     */
    GError  *err = NULL;             /* regex compile failure               */
    OnQuery *parsed = on_query_new(query, case_sensitive, use_regex, &err);
    if (parsed == NULL) {
        gchar *msg = g_strdup_printf("Bad pattern: %s", err->message);
        gtk_label_set_text(GTK_LABEL(sw->status), msg);
        g_free(msg);
        g_clear_error(&err);
        return;
    }
    /* Punctuation only ("" or a lone quote): nothing to match with.        */
    if (on_query_is_empty(parsed)) {
        gtk_label_set_text(GTK_LABEL(sw->status), "Type something to search for.");
        on_query_free(parsed);
        return;
    }

    /* What an opened result highlights in the note — the query's first
     * positive term, never the raw entry text with its operators.          */
    g_free(sw->highlight);
    sw->highlight = g_strdup(on_query_highlight_term(parsed));

    SearchJob *job = g_new0(SearchJob, 1);
    job->sw             = sw;
    job->db_path        = g_strdup(sw->app->db->path);
    job->query          = parsed;    /* ownership passes to the job         */
    job->scoped         = scoped;
    job->hits           = g_ptr_array_new_with_free_func(search_hit_free);

    /* The scoped variant reads the library's selection NOW, so it always
     * matches what is highlighted in the sidebar at the moment Search is
     * pressed.                                                             */
    if (scoped) {
        OnSearchScope scope;         /* live library scope                  */
        gchar *scope_name;           /* live selection name                 */
        on_library_get_scope(sw->app, &scope, &job->scope_id, &scope_name);
        job->scope_tag  = scope == ON_SCOPE_TAG;
        job->scope_desc = g_strdup_printf(
            " in \xe2\x80\x9c%s\xe2\x80\x9d", scope_name);
        g_free(scope_name);
    }

    sw->job = job;
    gtk_label_set_text(GTK_LABEL(sw->status), "Searching\xe2\x80\xa6");
    gtk_widget_show(sw->spinner);
    gtk_spinner_start(GTK_SPINNER(sw->spinner));
    g_thread_unref(g_thread_new("on-search", search_worker, job));
}

/* on_result_activated() — double-click/Enter on a result opens the note.    */
static void
on_result_activated(GtkTreeView *view, GtkTreePath *path,
                    GtkTreeViewColumn *col, gpointer user_data)
{
    (void)view; (void)col;
    OnSearch *sw = user_data;        /* owning search window                */
    GtkTreeIter iter;                /* activated row                       */
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(sw->store), &iter, path))
        return;
    gint64 id;                       /* note id of the row                  */
    gtk_tree_model_get(GTK_TREE_MODEL(sw->store), &iter, SR_ID, &id, -1);
    /* Carry the searched-for term into the editor so it is highlighted in
     * the note: the query's first positive term (a regex query is seeded
     * as-is), which is NULL when the query only excluded things.           */
    on_editor_window_open_search(sw->app, id, sw->highlight);
}

/* on_search_configure() — track the window's live size so it can be
 * persisted at close (the GdkWindow is already gone by "destroy").          */
static gboolean
on_search_configure(GtkWidget *widget, GdkEventConfigure *event,
                    gpointer user_data)
{
    (void)widget;
    OnSearch *sw = user_data;        /* owning search window                */
    sw->win_w = event->width;
    sw->win_h = event->height;
    return FALSE;                    /* never consume: default handling     */
}

/* on_search_destroy() — abandon any running search (the job frees itself
 * once its worker finishes), remember the window's size as the default
 * for the next search window, and free the state struct.                    */
static void
on_search_destroy(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnSearch *sw = user_data;        /* owning search window                */
    search_job_cancel(sw);
    if (sw->win_w > 0 && sw->win_h > 0) {
        gchar *w = g_strdup_printf("%d", sw->win_w);
        gchar *h = g_strdup_printf("%d", sw->win_h);
        on_app_config_set("search_win_w", w);
        on_app_config_set("search_win_h", h);
        g_free(w);
        g_free(h);
    }
    g_free(sw->highlight);
    g_free(sw);
}

/* ---------------------------------------------------------------------------
 * search_window_build() — construct, populate and show one search window.
 * The shared body of both public entry points; returns the live OnSearch so
 * the query variant can seed the entry and fire a search immediately.  The
 * window owns itself from here on (on_search_destroy frees the struct).
 *   app          — global application context.
 *   scope_to_sel — TRUE preselects "Selected Folder/Tag" instead of
 *                  "All Notes".
 * Returns the window's state struct (never NULL).
 * ------------------------------------------------------------------------- */
static OnSearch *
search_window_build(OnApp *app, gboolean scope_to_sel)
{
    OnSearch *sw = g_new0(OnSearch, 1);
    sw->app = app;

    /* --- window (standard titlebar) --------------------------------------*/
    sw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(sw->window), "Notes - Search");

    /* Open at whatever size the last search window was left at.            */
    gint win_w = SEARCH_WIN_DEFAULT_W;
    gint win_h = SEARCH_WIN_DEFAULT_H;
    on_app_config_get_size("search_win_w", "search_win_h", &win_w, &win_h);
    gtk_window_set_default_size(GTK_WINDOW(sw->window), win_w, win_h);

    gtk_window_set_transient_for(GTK_WINDOW(sw->window),
                                 GTK_WINDOW(app->library_window));
    g_signal_connect(sw->window, "configure-event",
                     G_CALLBACK(on_search_configure), sw);
    g_signal_connect(sw->window, "destroy",
                     G_CALLBACK(on_search_destroy), sw);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    gtk_container_add(GTK_CONTAINER(sw->window), vbox);

    /* --- query row --------------------------------------------------------*/
    GtkWidget *query_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    sw->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw->entry),
                                   "Search titles and note text\xe2\x80\xa6");
    gtk_widget_set_tooltip_text(sw->entry,
        "Every word must appear somewhere in the note.\n"
        "\"in quotes\" matches the whole phrase; -word excludes notes "
        "that contain it.\n"
        "Regular expression mode takes the query as one pattern instead.");
    /* Both triggers run the search; swapped-connect passes `sw` as the
     * handler's (only used) argument, so no wrapper callbacks needed.      */
    g_signal_connect_swapped(sw->entry, "activate",
                             G_CALLBACK(run_search), sw);
    gtk_box_pack_start(GTK_BOX(query_row), sw->entry, TRUE, TRUE, 0);

    GtkWidget *btn = gtk_button_new_with_label("Search");
    g_signal_connect_swapped(btn, "clicked", G_CALLBACK(run_search), sw);
    gtk_box_pack_start(GTK_BOX(query_row), btn, FALSE, FALSE, 0);

    /* Spinner shown while a background search runs (hidden when idle).     */
    sw->spinner = gtk_spinner_new();
    gtk_widget_set_no_show_all(sw->spinner, TRUE);
    gtk_box_pack_start(GTK_BOX(query_row), sw->spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), query_row, FALSE, FALSE, 0);

    /* --- scope radios -------------------------------------------------------*/
    GtkWidget *scope_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    sw->radio_all = gtk_radio_button_new_with_label(NULL, "All Notes");
    gtk_box_pack_start(GTK_BOX(scope_row), sw->radio_all, FALSE, FALSE, 0);

    sw->radio_scoped = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(sw->radio_all), "Selected Folder/Tag");
    gtk_widget_set_tooltip_text(sw->radio_scoped,
        "Search only whatever folder or tag is selected in the library "
        "when you press Search");
    gtk_box_pack_start(GTK_BOX(scope_row), sw->radio_scoped,
                       FALSE, FALSE, 0);
    if (scope_to_sel)
        gtk_toggle_button_set_active(
            GTK_TOGGLE_BUTTON(sw->radio_scoped), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), scope_row, FALSE, FALSE, 0);

    /* --- matching options ---------------------------------------------------*/
    GtkWidget *opt_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    sw->check_case  = gtk_check_button_new_with_label("Case sensitive");
    sw->check_regex = gtk_check_button_new_with_label("Regular expression");
    gtk_widget_set_tooltip_text(sw->check_regex,
        "Match the whole query as one pattern; quoting and -exclusions "
        "keep their regular-expression meaning instead");
    gtk_box_pack_start(GTK_BOX(opt_row), sw->check_case,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(opt_row), sw->check_regex, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), opt_row, FALSE, FALSE, 0);

    /* --- results -------------------------------------------------------------*/
    sw->store = gtk_list_store_new(SR_N_COLS,
                                   G_TYPE_INT64,    /* SR_ID               */
                                   G_TYPE_STRING,   /* SR_PATH             */
                                   G_TYPE_STRING);  /* SR_MODIFIED         */

    GtkWidget *results = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(sw->store));
    g_object_unref(sw->store);       /* the view holds the ref now          */
    /* No GTK type-ahead popup (auto-picked search column, see quirk 16).  */
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(results), FALSE);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(results),
        gtk_tree_view_column_new_with_attributes(
            "Path", gtk_cell_renderer_text_new(),
            "text", SR_PATH, NULL));
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(results),
        gtk_tree_view_column_new_with_attributes(
            "Modified", gtk_cell_renderer_text_new(),
            "text", SR_MODIFIED, NULL));
    gtk_tree_view_column_set_expand(
        gtk_tree_view_get_column(GTK_TREE_VIEW(results), 0), TRUE);
    g_signal_connect(results, "row-activated",
                     G_CALLBACK(on_result_activated), sw);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll),
                                              FALSE);
    gtk_container_add(GTK_CONTAINER(scroll), results);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    /* --- status line -----------------------------------------------------------*/
    sw->status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(sw->status), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), sw->status, FALSE, FALSE, 0);

    gtk_widget_show_all(sw->window);
    gtk_widget_grab_focus(sw->entry);
    return sw;
}

void
on_search_window_open(OnApp *app, gboolean scope_to_sel)
{
    search_window_build(app, scope_to_sel);
}

void
on_search_window_open_query(OnApp *app, const gchar *query)
{
    OnSearch *sw = search_window_build(app, FALSE);
    if (query == NULL || *query == '\0')
        return;                      /* empty: just the idle window         */

    /* All Notes + case-insensitive + plain text are exactly the freshly
     * built window's defaults, so only the query needs seeding.            */
    gtk_entry_set_text(GTK_ENTRY(sw->entry), query);
    gtk_editable_set_position(GTK_EDITABLE(sw->entry), -1);
    run_search(sw);
}
