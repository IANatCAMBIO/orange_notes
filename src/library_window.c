/* ===========================================================================
 * library_window.c — the Notes Library window (implementation)
 *
 * See library_window.h for the layout overview.  Key mechanics:
 *
 *   sidebar    — a GtkTreeView over a GtkTreeStore holding the sections
 *                "All Notes", the folder hierarchy (rooted at a fixed
 *                "Notes" row), a flat "Tags" section, "Pinned Notes",
 *                and — while non-empty — "Trash" (trashed folders as its
 *                children).  Row kinds are distinguished by the SB_KIND
 *                column.
 *
 *   notes pane — a GtkListStore shown either as a GtkTreeView (list mode,
 *                drag-reorderable) or a GtkIconView (grid mode).  Both
 *                views stay attached to the same store; a GtkStack flips
 *                between them.
 *
 *   drag&drop  — list rows use GtkTreeView's built-in reordering; the
 *                resulting order is persisted from the "row-deleted"
 *                signal.  Both views are also GTK_TREE_MODEL_ROW drag
 *                sources, and the sidebar accepts those drops to move a
 *                note into a folder.  The sidebar is a drag source too:
 *                a folder row drops INTO a folder (re-nest), BETWEEN
 *                folders (reorder/re-nest beside the sibling), onto
 *                Trash (delete gesture), or out of Trash (restore).
 *                The sidebar's dest protocol is fully custom (quirk #13).
 * =========================================================================== */

#include "library_window.h"
#include "editor_window.h"
#include "export.h"
#include "media_window.h"
#include "search_window.h"
#include "serialize.h"
#include "settings_window.h"

#include <cairo-gobject.h>
#include <glib/gstdio.h>
#include <string.h>

#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* Logical pixel size of the square grid-view thumbnails.                    */
#define THUMB_SIZE 140

/* Background for even rows in list view (odd rows stay white).              */
#define ROW_TINT "#e8f2fb"

/* How much of a note's body text is fetched for the Comfortable-density
 * preview: enough for the first non-blank line after the title.             */
#define NL_PREVIEW_CHARS 200

/* strftime format of the Modified and Created cells ("Jun 3, 2026 14:05").
 * ONE definition: list_autofit_time_width() bounds the column width from
 * this same pattern, so the two can never disagree about the shape.         */
#define LIST_TIME_FORMAT "%b %e, %Y %H:%M"

/* Blank strip above the sidebar tree, to line its first row's text up with
 * the notes list's column-header text (see library_build_sidebar).          */
#define SB_TOP_PAD 3

/* How far the sidebar backdrop sits below the toolbar/window background it
 * is shaded from — a CSS shade() factor, < 1 darkens.  0.96 turns Adwaita's
 * rgb(246,245,244) into rgb(238,236,234).  A string, not a number: it is
 * pasted into two CSS declarations in library_build_sidebar.                */
#define SB_BG_SHADE "0.96"

/* Sidebar row kinds (SB_KIND column).                                       */
enum {
    SB_KIND_ROOT = 0,                /* the fixed "Notes" root row          */
    SB_KIND_FOLDER,                  /* a real folder                       */
    SB_KIND_TAGS_HEADER,             /* the "Tags" section header           */
    SB_KIND_TAG,                     /* one tag                             */
    SB_KIND_PINNED,                  /* the "Pinned Notes" section          */
    SB_KIND_ALL,                     /* the "All Notes" section             */
    SB_KIND_TRASH,                   /* the "Trash" section                 */
    SB_KIND_TRASH_FOLDER,            /* a trashed folder under Trash        */
    SB_KIND_ACTIONS,                 /* the "Action Items" section          */
};

/* Sidebar GtkTreeStore columns.                                             */
enum {
    SB_KIND,                         /* gint: one of SB_KIND_*              */
    SB_ID,                           /* gint64: folder id or tag id         */
    SB_NAME,                         /* gchar*: display text (with count)   */
    SB_RAW,                          /* gchar*: bare name (no count suffix) */
    SB_N_COLS
};

/* Notes GtkListStore columns.                                               */
enum {
    NL_ID,                           /* gint64: note id                     */
    NL_TITLE,                        /* gchar*: note title                  */
    NL_MODIFIED,                     /* gchar*: formatted updated_at        */
    NL_THUMB,                        /* cairo_surface_t*: grid thumbnail    */
    NL_UPDATED,                      /* gint64: raw updated_at (sort key)   */
    NL_PATH,                         /* gchar*: "/Folder/Sub" location      */
    NL_CREATED,                      /* gchar*: formatted created_at        */
    NL_CREATED_RAW,                  /* gint64: raw created_at (sort key)   */
    NL_PREVIEW,                      /* gchar*: first line of body text      */
    NL_N_COLS
};

/* Columns of the Action Items list model (the third notes-pane view).       */
enum {
    AL_NOTE_ID,                      /* gint64: owning note id              */
    AL_ORD,                          /* gint: position among the note's
                                        action lines (addresses the item)   */
    AL_DONE,                         /* gboolean: checkbox state            */
    AL_TEXT,                         /* gchar*: the item text               */
    AL_DUE,                          /* gchar*: formatted due date ("")     */
    AL_DUE_RAW,                      /* gint64: due timestamp (sort key;
                                        0 = none, sorts after any date)     */
    AL_N_COLS
};

/* How many columns the Action Items list owns (done, Action, Due Date).    */
#define N_ACTION_COLUMNS 3

/* Drag-and-drop target shared by the notes views (sources) and the
 * sidebar (destination).  GTK_TREE_MODEL_ROW is the built-in target used
 * by GtkTreeView's own reordering machinery, so one target serves both
 * in-list reordering and note→folder drops.                                */
static const GtkTargetEntry ROW_TARGET =
    { (gchar *)"GTK_TREE_MODEL_ROW", GTK_TARGET_SAME_APP, 0 };

/* ---------------------------------------------------------------------------
 * OnLibrary — all state for the library window.
 *
 * Fields:
 *   app           — global application context (not owned).
 *   window        — the top-level GtkWindow.
 *   sidebar_store — tree model behind the sidebar.
 *   sidebar       — the sidebar GtkTreeView.
 *   notes_store   — list model behind both notes views.
 *   notes_list    — list-mode view (GtkTreeView).
 *   notes_grid    — grid-mode view (GtkIconView).
 *   stack         — GtkStack switching between list and grid.
 *   sel_kind      — SB_KIND_* of the current sidebar selection; controls
 *                   which notes are listed.
 *   sel_id        — folder id (for ROOT/FOLDER) or tag id (for TAG) of
 *                   the current selection.
 *   sel_name      — bare name of the current selection (owned string),
 *                   used for rename pre-fill and the search scope label.
 *   populating    — nesting counter; >0 while code (not the user) is
 *                   rewriting the models, so persistence signal handlers
 *                   know to stand down.
 *   thumb_cache   — note id (gint64*) → ThumbEntry*, so grid thumbnails
 *                   are only re-rendered when a note actually changed.
 *   shown_kind/
 *   shown_id      — the selection refresh_notes() last populated for;
 *                   a matching refresh (e.g. after an editor autosave)
 *                   keeps the notes-pane scroll position instead of
 *                   jumping back to the top.
 *   sidebar_box   — the whole folder/tag pane, so the toolbar's
 *                   show/hide toggle can flip its visibility.
 *   status_path   — status-bar label (bottom left): the path of the
 *                   current sidebar selection.
 *   status_event  — status-bar label (bottom right): the latest event
 *                   message ("DB saved", …); see on_app_status().  Lives
 *                   inside status_revealer (crossfade) and fades out
 *                   after STATUS_FADE_SECONDS via status_timeout.
 * ------------------------------------------------------------------------- */
typedef struct {
    OnApp        *app;
    GtkWidget    *window;
    GtkTreeStore *sidebar_store;
    GtkTreeView  *sidebar;
    GtkListStore *notes_store;
    GtkTreeView  *notes_list;
    GtkIconView  *notes_grid;
    GtkListStore *actions_store;         /* Action Items model (AL_*)      */
    GtkTreeView  *actions_view;          /* Action Items list view         */
    gboolean      grid_pref;             /* the user's list/grid choice, so
                                            leaving the Action Items view
                                            restores the right mode        */
    GtkWidget    *stack;
    gint          sel_kind;
    gint64        sel_id;
    gchar        *sel_name;
    gboolean      list_autofit;          /* list columns auto-size to their
                                            contents on every refresh (ini
                                            key "list_autofit")            */
    gboolean      notes_sel_blocked;     /* selection changes vetoed for
                                            the span of a press on an
                                            already-selected list row, so
                                            a drag keeps the whole
                                            multi-selection (quirk #15)    */
    GtkTreePath  *notes_press_path;      /* the row that press landed on
                                            (owned): a plain click
                                            collapses to it on release     */
    gint          populating;
    GHashTable   *thumb_cache;
    GHashTable   *folder_path_cache;     /* folder_id→path string; populated
                                            lazily by refresh_notes and
                                            invalidated by refresh_sidebar
                                            so autosave refreshes reuse it  */
    GQueue        thumb_pending;         /* ThumbJob* rows awaiting a
                                            render by thumb_fill_idle       */
    guint         thumb_idle;            /* the idle source doing it        */
    gint          shown_kind;
    gint64        shown_id;
    GtkWidget    *sidebar_box;
    GtkWidget    *sidebar_paned;         /* horizontal paned holding the sidebar */
    GtkWidget    *status_path;
    GtkWidget    *status_event;
    GtkWidget    *status_revealer;
    guint         status_timeout;
    GtkToolItem  *ai_btn;              /* microchip AI toolbar button          */
    GtkWidget    *ai_pane;             /* output pane below the notes stack    */
    GtkWidget    *ai_text;             /* non-editable text view inside it     */
    guint         ai_throbber_id;       /* g_timeout_add id while AI running    */
    gint          ai_throbber_step;    /* animation frame counter              */
    GtkPaned     *notes_paned;         /* vertical paned: stack / AI pane      */
    gboolean      ai_running;          /* TRUE while subprocess is in flight   */
    GCancellable *ai_cancel;           /* cancels the in-flight subprocess     */
} OnLibrary;

/* How long a status-bar event message stays before fading out.              */
#define STATUS_FADE_SECONDS 4

/* ---------------------------------------------------------------------------
 * ThumbEntry — one cached grid thumbnail.
 *
 * Fields:
 *   updated_at — the note's updated_at when the thumbnail was rendered;
 *                a mismatch means the cache entry is stale.
 *   pix        — the rendered thumbnail (owned reference).
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64           updated_at;
    cairo_surface_t *surface;
} ThumbEntry;

/* thumb_entry_free() — GDestroyNotify for cache values.                     */
static void
thumb_entry_free(gpointer data)
{
    ThumbEntry *e = data;
    if (e->surface != NULL)
        cairo_surface_destroy(e->surface);
    g_free(e);
}

/* ---------------------------------------------------------------------------
 * ThumbJob — one grid row whose thumbnail still has to be rendered.
 * refresh_notes() only sets thumbnails it finds fresh in the cache; every
 * stale/missing one is queued as a job and rendered by thumb_fill_idle()
 * in small time slices, so a cold cache (first grid showing of a big
 * folder or All Notes) can't freeze the GUI for the whole render — that
 * once hung the window for ~40 s on a 1200-note database.
 *
 * Fields:
 *   row        — where to deliver the surface (owned; safely goes
 *                invalid if the model is rebuilt or the row removed).
 *   id         — the note to render.
 *   updated_at — its updated_at when the row was populated (cache key).
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkTreeRowReference *row;
    gint64               id;
    gint64               updated_at;
} ThumbJob;

/* thumb_job_free() — release one pending-thumbnail job.                     */
static void
thumb_job_free(ThumbJob *job)
{
    gtk_tree_row_reference_free(job->row);
    g_free(job);
}

/* thumb_pending_clear() — drop every queued job and stop the fill idle;
 * run before any notes-model rebuild (the rows are about to vanish) and
 * at window teardown.                                                       */
static void
thumb_pending_clear(OnLibrary *lw)
{
    ThumbJob *job;                   /* one drained queue entry             */
    while ((job = g_queue_pop_head(&lw->thumb_pending)) != NULL)
        thumb_job_free(job);
    if (lw->thumb_idle != 0) {
        g_source_remove(lw->thumb_idle);
        lw->thumb_idle = 0;
    }
}

/* Forward declarations.                                                     */
static void    refresh_sidebar(OnLibrary *lw);
static void    refresh_notes(OnLibrary *lw);
static void    status_path_update(OnLibrary *lw);
static GArray *selected_note_ids(OnLibrary *lw);
static guint   selected_note_count(OnLibrary *lw);
static void    list_autofit_set(OnLibrary *lw, PangoLayout *lay,
                                const gchar *key, gint content_w);
static gboolean list_column_shown(OnLibrary *lw, const gchar *key);
static gint    list_autofit_time_width(PangoLayout *lay);
static GtkTreePath *notes_sel_unblock(OnLibrary *lw, gboolean want_path);
static void    close_editors_for_ids(OnLibrary *lw, const gint64 *ids,
                                     gsize n);
static gboolean trash_notes_core(OnLibrary *lw, const gint64 *ids,
                                 guint n);
static gboolean trash_folder(OnLibrary *lw, gint64 folder_id,
                             const gchar *name);
static void    add_menu_item(GtkWidget *menu, const gchar *label,
                             GCallback callback, gpointer data);
static void    run_ai_summary(OnLibrary *lw);
static GtkWidget *build_ai_pane(OnLibrary *lw);
static void    on_ai_copy_clicked(GtkButton *btn, gpointer user_data);
static gboolean ai_throbber_tick(gpointer user_data);

/* ===========================================================================
 * scroll preservation
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * ScrollKeep — one vadjustment position being restored after a model
 * rebuild (clearing a store zeroes its view's scrollbar).  The restore
 * runs at idle so the rebuilt view has re-validated its height first.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkAdjustment *adj;              /* the scrollbar to restore (owned ref)*/
    gdouble        value;            /* position before the rebuild         */
} ScrollKeep;

/* scroll_keep_restore() — idle callback: put the scrollbar back.            */
static gboolean
scroll_keep_restore(gpointer user_data)
{
    ScrollKeep *k = user_data;
    gtk_adjustment_set_value(k->adj, k->value);  /* clamps to the range     */
    g_object_unref(k->adj);
    g_free(k);
    return G_SOURCE_REMOVE;
}

/* scroll_keep_queue() — schedule `adj` to be put back at `value`.           */
static void
scroll_keep_queue(GtkAdjustment *adj, gdouble value)
{
    ScrollKeep *k = g_new(ScrollKeep, 1);
    k->adj   = g_object_ref(adj);
    k->value = value;
    g_idle_add(scroll_keep_restore, k);
}

/* view_vadjustment() — the vadjustment of the scrolled window holding
 * `view` (every scrollable view here sits directly in one).  Returns NULL
 * if the parent isn't realized as a GtkScrolledWindow yet.                  */
static GtkAdjustment *
view_vadjustment(GtkWidget *view)
{
    GtkWidget *parent = gtk_widget_get_parent(view);
    if (!GTK_IS_SCROLLED_WINDOW(parent))
        return NULL;
    return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent));
}

/* ===========================================================================
 * sidebar population
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * add_folder_rows() — recursively append the subfolders of `parent_id`
 * beneath tree row `parent_iter`.
 *   lw          — the library window.
 *   parent_id   — database folder id whose children to add (0 = roots).
 *   parent_iter — sidebar row to attach them under.
 * ------------------------------------------------------------------------- */
/* count_from_map() — look one id up in a count map (0 when absent, and 0
 * for a NULL map: the maps are only built while sidebar counts are shown). */
static gint
count_from_map(GHashTable *map, gint64 id)
{
    return (map != NULL)
        ? GPOINTER_TO_INT(g_hash_table_lookup(map, &id)) : 0;
}

/* in_trash_view() — is the notes pane showing Trash contents (the Trash
 * row itself or a trashed folder)?  Deletes there are permanent and the
 * note context menu switches to Restore/Delete Permanently.                 */
static gboolean
in_trash_view(OnLibrary *lw)
{
    return lw->sel_kind == SB_KIND_TRASH ||
           lw->sel_kind == SB_KIND_TRASH_FOLDER;
}

/* sb_kind_is_section() — is `kind` one of the bold sidebar section rows
 * (the Notes root, the Tags header, Pinned Notes, All Notes, Trash)?        */
static gboolean
sb_kind_is_section(gint kind)
{
    return kind == SB_KIND_ROOT ||
           kind == SB_KIND_TAGS_HEADER ||
           kind == SB_KIND_PINNED ||
           kind == SB_KIND_ALL ||
           kind == SB_KIND_TRASH ||
           kind == SB_KIND_ACTIONS;
}

/* ---------------------------------------------------------------------------
 * sb_folder_append() — add one folder row to the sidebar, formatting its
 * display text the one way folder rows are formatted: an optional emoji
 * prefix separated by two spaces, and an optional "(n)" note count.  Shared
 * by the normal tree and the Trash section, which differ only in SB_KIND.
 *   parent_iter — row to nest under (the Notes root, a folder, or Trash).
 *   f           — the folder.
 *   kind        — SB_KIND_FOLDER or SB_KIND_TRASH_FOLDER.
 *   note_counts — count map, or NULL while counts are hidden.
 *   iter        — receives the new row (for recursing into it); may be NULL.
 * ------------------------------------------------------------------------- */
static void
sb_folder_append(OnLibrary *lw, GtkTreeIter *parent_iter, const OnFolder *f,
                 gint kind, GHashTable *note_counts, GtkTreeIter *iter)
{
    gboolean has_emoji = f->emoji != NULL && *f->emoji != '\0';
    gchar   *display;                /* name (+ emoji prefix, + count)      */
    if (lw->app->sidebar_counts) {
        gint count = count_from_map(note_counts, f->id);
        display = has_emoji
            ? g_strdup_printf("%s  %s (%d)", f->emoji, f->name, count)
            : g_strdup_printf("%s (%d)", f->name, count);
    } else {
        display = has_emoji
            ? g_strdup_printf("%s  %s", f->emoji, f->name)
            : g_strdup(f->name);
    }

    GtkTreeIter local;               /* used when the caller passed NULL    */
    if (iter == NULL)
        iter = &local;
    gtk_tree_store_append(lw->sidebar_store, iter, parent_iter);
    gtk_tree_store_set(lw->sidebar_store, iter,
                       SB_KIND, kind,
                       SB_ID,   f->id,
                       SB_NAME, display,
                       SB_RAW,  f->name,
                       -1);
    g_free(display);
}

static void
add_folder_rows(OnLibrary *lw, gint64 parent_id, GtkTreeIter *parent_iter,
                GHashTable *note_counts, GHashTable *children)
{
    /* Borrowed from the pre-fetched child map — one query for the whole
     * tree, instead of one per folder as this recursion used to do.         */
    GList *folders = on_db_folder_children(children, parent_id);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder   *f = l->data;     /* one child folder                    */
        GtkTreeIter iter;            /* its new row, to recurse under       */
        sb_folder_append(lw, parent_iter, f, SB_KIND_FOLDER, note_counts,
                         &iter);
        add_folder_rows(lw, f->id, &iter, note_counts, children);
    }
    /* `folders` belongs to the child map — nothing to free here.            */
}

/* sb_row_key() — hashable identity of a sidebar row for state that must
 * survive a model rebuild (paths shift when folders move; kind+id don't). */
static gint64
sb_row_key(gint kind, gint64 id)
{
    return id * 16 + kind;
}

/* SbExpandCtx — working state for the expansion-capture walk below.        */
typedef struct {
    OnLibrary  *lw;
    GHashTable *expanded;                /* set of sb_row_key()s            */
} SbExpandCtx;

/* sb_expand_capture() — gtk_tree_model_foreach() callback: record the
 * key of every currently-expanded row.                                     */
static gboolean
sb_expand_capture(GtkTreeModel *model, GtkTreePath *path,
                  GtkTreeIter *iter, gpointer data)
{
    SbExpandCtx *ctx = data;
    if (gtk_tree_view_row_expanded(ctx->lw->sidebar, path)) {
        gint   kind;                 /* row kind                            */
        gint64 id;                   /* row id                              */
        gtk_tree_model_get(model, iter, SB_KIND, &kind, SB_ID, &id, -1);
        gint64 *key = g_new(gint64, 1);
        *key = sb_row_key(kind, id);
        g_hash_table_add(ctx->expanded, key);
    }
    return FALSE;
}

/* sb_reveal_path() — expand the ANCESTORS of `path` so the row itself is
 * visible (never the row itself: a collapsed selected folder stays
 * collapsed).                                                               */
static void
sb_reveal_path(GtkTreeView *view, GtkTreePath *path)
{
    GtkTreePath *parent = gtk_tree_path_copy(path);
    if (gtk_tree_path_up(parent) && gtk_tree_path_get_depth(parent) > 0)
        gtk_tree_view_expand_to_path(view, parent);
    gtk_tree_path_free(parent);
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the whole sidebar model: the folder tree
 * under the fixed "Notes" root, then the "Tags" section.  Attempts to
 * restore the previous selection by (kind, id), and puts back which rows
 * were expanded (only the very first population expands everything) —
 * a drop used to re-expand every folder because every successful drag
 * refreshes the sidebar.
 * ------------------------------------------------------------------------- */
static void
refresh_sidebar(OnLibrary *lw)
{
    gint   want_kind = lw->sel_kind; /* selection to restore                */
    gint64 want_id   = lw->sel_id;

    /* Folder mutations flow through refresh_sidebar; invalidate the cached
     * path map so refresh_notes fetches a fresh one on the next call.       */
    if (lw->folder_path_cache != NULL) {
        g_hash_table_destroy(lw->folder_path_cache);
        lw->folder_path_cache = NULL;
    }

    /* Capture the expansion state (keyed by kind+id, which survive the
     * rebuild) before the clear wipes it.                                  */
    SbExpandCtx ectx = {
        lw, g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                  g_free, NULL) };
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store),
                           sb_expand_capture, &ectx);

    /* Clearing the store zeroes the sidebar scrollbar; a sidebar rebuild
     * is never a navigation (counts changed, a folder was added, …), so
     * the position is always put back.                                     */
    GtkAdjustment *vadj      = view_vadjustment(GTK_WIDGET(lw->sidebar));
    gdouble        scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    gtk_tree_store_clear(lw->sidebar_store);

    /* Batched counts: one query for all folders, one for all tags —
     * refresh_sidebar runs after every autosave, so per-row COUNT
     * queries added up (especially against a shared/networked db).
     * Built ONLY when the counts are actually displayed: every reader below
     * sits inside a `sidebar_counts` branch, so with the setting off (the
     * default) these two GROUP BY queries were run and thrown away on
     * every rebuild.                                                        */
    GHashTable *note_counts = NULL;  /* folder id → note count, or NULL     */
    GHashTable *tag_counts  = NULL;  /* tag id → note count, or NULL        */
    if (lw->app->sidebar_counts) {
        note_counts = on_db_note_count_map(lw->app->db);
        tag_counts  = on_db_tag_count_map(lw->app->db);
    }

    /* The whole folder tree in one query; add_folder_rows walks it.         */
    GHashTable *children = on_db_folder_child_map(lw->app->db);

    /* "Pinned Notes" on top — a live view, not a real location; notes
     * stay in their folders.                                               */
    gint n_pinned = on_db_note_count_pinned(lw->app->db);
    if (n_pinned > 0) {
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("\xf0\x9f\x93\x8c\xc2\xa0 Pinned Notes (%d)",
                              n_pinned)
            : g_strdup("\xf0\x9f\x93\x8c\xc2\xa0 Pinned Notes");
        GtkTreeIter iter;
        gtk_tree_store_append(lw->sidebar_store, &iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &iter,
                           SB_KIND, SB_KIND_PINNED,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Pinned Notes",
                           -1);
        g_free(label);
    }

    /* "All Notes" — a live view of every note outside the Trash.           */
    gchar *all_label = lw->app->sidebar_counts
        ? g_strdup_printf("\xf0\x9f\x94\xae\xc2\xa0 All Notes (%d)",
                          on_db_note_count_visible(lw->app->db))
        : g_strdup("\xf0\x9f\x94\xae\xc2\xa0 All Notes");
    GtkTreeIter all_iter;            /* the fixed "All Notes" row           */
    gtk_tree_store_append(lw->sidebar_store, &all_iter, NULL);
    gtk_tree_store_set(lw->sidebar_store, &all_iter,
                       SB_KIND, SB_KIND_ALL,
                       SB_ID,   (gint64)0,
                       SB_NAME, all_label,
                       SB_RAW,  "All Notes",
                       -1);
    g_free(all_label);

    /* "Action Items" directly under All Notes — every '!' line across the
     * visible notes (mirrored into the action_items table by saves).
     * Shown only while any exist; the optional count is the OPEN ones.     */
    gint n_actions = 0, n_open = 0;  /* all items / unchecked items         */
    on_db_action_counts(lw->app->db, &n_actions, &n_open);
    if (n_actions > 0) {
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("\xe2\x9d\x97\xc2\xa0 Action Items (%d)", n_open)
            : g_strdup("\xe2\x9d\x97\xc2\xa0 Action Items");
        GtkTreeIter iter;
        gtk_tree_store_append(lw->sidebar_store, &iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &iter,
                           SB_KIND, SB_KIND_ACTIONS,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Action Items",
                           -1);
        g_free(label);
    }

    /* "Notes" root — selecting it shows the top-level notes.               */
    gchar *root_label = lw->app->sidebar_counts
        ? g_strdup_printf("\xf0\x9f\x93\x93\xc2\xa0 Notes (%d)",
                          count_from_map(note_counts, 0))
        : g_strdup("\xf0\x9f\x93\x93\xc2\xa0 Notes");
    GtkTreeIter root;                /* the fixed root row                  */
    gtk_tree_store_append(lw->sidebar_store, &root, NULL);
    gtk_tree_store_set(lw->sidebar_store, &root,
                       SB_KIND, SB_KIND_ROOT,
                       SB_ID,   (gint64)0,
                       SB_NAME, root_label,
                       SB_RAW,  "Notes",
                       -1);
    g_free(root_label);
    add_folder_rows(lw, 0, &root, note_counts, children);

    /* "Tags" header + one row per known tag.                               */
    GList *tags = on_db_tag_list(lw->app->db);
    if (tags != NULL) {
        GtkTreeIter header;          /* the "Tags" section row              */
        gtk_tree_store_append(lw->sidebar_store, &header, NULL);
        gtk_tree_store_set(lw->sidebar_store, &header,
                           SB_KIND, SB_KIND_TAGS_HEADER,
                           SB_ID,   (gint64)0,
                           SB_NAME, "\xf0\x9f\x8f\xb7\xef\xb8\x8f\xc2\xa0 Tags",
                           SB_RAW,  "Tags",
                           -1);
        for (GList *l = tags; l != NULL; l = l->next) {
            OnTag *t = l->data;      /* one tag                             */
            gchar *raw   = g_strdup_printf("#%s", t->name);
            gchar *label = lw->app->sidebar_counts
                ? g_strdup_printf("#%s (%d)", t->name,
                                  count_from_map(tag_counts, t->id))
                : g_strdup(raw);
            GtkTreeIter iter;
            gtk_tree_store_append(lw->sidebar_store, &iter, &header);
            gtk_tree_store_set(lw->sidebar_store, &iter,
                               SB_KIND, SB_KIND_TAG,
                               SB_ID,   t->id,
                               SB_NAME, label,
                               SB_RAW,  raw,
                               -1);
            g_free(label);
            g_free(raw);
        }
    }
    on_db_tag_list_free(tags);

    /* "Trash" at the bottom, only while it holds something.  Selecting
     * it lists the directly-trashed notes; trashed folders hang under it
     * as browsable children (their subtrees stay hidden until restore).    */
    gint n_trash = on_db_trash_count(lw->app->db);
    if (n_trash > 0) {
        gchar *label = lw->app->sidebar_counts
            ? g_strdup_printf("\xf0\x9f\x97\x91\xc2\xa0 Trash (%d)", n_trash)
            : g_strdup("\xf0\x9f\x97\x91\xc2\xa0 Trash");
        GtkTreeIter trash_iter;      /* the "Trash" section row             */
        gtk_tree_store_append(lw->sidebar_store, &trash_iter, NULL);
        gtk_tree_store_set(lw->sidebar_store, &trash_iter,
                           SB_KIND, SB_KIND_TRASH,
                           SB_ID,   (gint64)0,
                           SB_NAME, label,
                           SB_RAW,  "Trash",
                           -1);
        g_free(label);

        GList *trashed = on_db_folder_list_trashed(lw->app->db);
        for (GList *l = trashed; l != NULL; l = l->next)
            sb_folder_append(lw, &trash_iter, l->data,
                             SB_KIND_TRASH_FOLDER, note_counts, NULL);
        on_db_folder_list_free(trashed);
    }

    on_db_folder_child_map_free(children);
    if (note_counts != NULL)
        g_hash_table_destroy(note_counts);
    if (tag_counts != NULL)
        g_hash_table_destroy(tag_counts);

    /* Every rebuild (including the first) restores the captured expansion
     * state in the walk below; first launch has nothing captured so all
     * folders stay collapsed.                                               */

    /* Restore the previous selection, falling back to "All Notes".  The
     * populating guard stays up through the restore: the select_iter
     * below would otherwise fire the changed handler and rebuild the
     * notes pane a second time — every refresh_sidebar caller already
     * pairs it with an explicit refresh_notes.                             */
    GtkTreeSelection *sel = gtk_tree_view_get_selection(lw->sidebar);
    GtkTreeIter iter;                /* candidate row while searching       */
    gboolean restored = FALSE;       /* did we find the old selection?      */

    gboolean valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(lw->sidebar_store), &iter);
    /* Depth-first walk of the whole sidebar model.                         */
    GQueue queue = G_QUEUE_INIT;     /* pending iters (BFS is fine too)     */
    while (valid || !g_queue_is_empty(&queue)) {
        if (!valid) {
            GtkTreeIter *q = g_queue_pop_head(&queue);
            iter = *q;
            g_free(q);
            valid = TRUE;
        }
        gint   kind;                 /* row kind                            */
        gint64 id;                   /* row id                              */
        gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                           SB_KIND, &kind, SB_ID, &id, -1);
        GtkTreePath *row_path = gtk_tree_model_get_path(
            GTK_TREE_MODEL(lw->sidebar_store), &iter);

        /* Re-expand rows that were expanded before the rebuild.            */
        gint64 ekey = sb_row_key(kind, id);
        if (g_hash_table_contains(ectx.expanded, &ekey))
            gtk_tree_view_expand_row(lw->sidebar, row_path, FALSE);

        if (kind == want_kind && id == want_id && !restored) {
            /* The suppressed handler would have refreshed sel_name; do it
             * here so a renamed folder/tag keeps it current.               */
            gchar *raw = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                               SB_RAW, &raw, -1);
            g_free(lw->sel_name);
            lw->sel_name = raw;      /* ownership transferred               */
            sb_reveal_path(lw->sidebar, row_path);
            gtk_tree_selection_select_iter(sel, &iter);
            restored = TRUE;         /* no break: the walk must finish
                                        restoring the expansion state       */
        }
        gtk_tree_path_free(row_path);
        GtkTreeIter child;           /* first child, if any                 */
        if (gtk_tree_model_iter_children(GTK_TREE_MODEL(lw->sidebar_store),
                                         &child, &iter)) {
            GtkTreeIter *copy = g_new(GtkTreeIter, 1);
            *copy = child;
            g_queue_push_tail(&queue, copy);
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(lw->sidebar_store),
                                         &iter);
    }
    g_queue_clear_full(&queue, g_free);
    g_hash_table_destroy(ectx.expanded);

    if (!restored) {
        /* Fall back to the first row (Pinned Notes when any are pinned,
         * All Notes otherwise) — the state must match the row the
         * fallback highlights, so read it from the model.                  */
        if (gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(lw->sidebar_store), &iter)) {
            gint   kind;             /* the first row's identity            */
            gint64 id;
            gchar *raw = NULL;
            gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                               SB_KIND, &kind, SB_ID, &id, SB_RAW, &raw,
                               -1);
            lw->sel_kind = kind;
            lw->sel_id   = id;
            g_free(lw->sel_name);
            lw->sel_name = raw;      /* ownership transferred               */
            gtk_tree_selection_select_iter(sel, &iter);
        }
    }
    lw->populating--;

    /* The old selection no longer exists (deleted folder/pruned tag), so
     * the notes pane still shows its contents: refresh for the new
     * fallback selection.  When the selection was restored, the caller's
     * own refresh_notes covers it.                                         */
    if (!restored)
        refresh_notes(lw);

    if (scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);
}

/* ===========================================================================
 * notes pane population + grid thumbnails
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * render_note_thumb() — draw a square THUMB_SIZE (logical) card for one
 * note: its first embedded image (if any) above the beginning of its body
 * text.  The card is rendered at the window's scale factor and returned
 * as a cairo surface with a matching device scale, so it is sharp on
 * HiDPI/Retina displays.  The title is NOT drawn here — the grid shows it
 * as a real text label under the card.
 *   lw — the library window (for the database and scale factor).
 *   id — the note to render.
 * Returns a new cairo surface reference.
 * ------------------------------------------------------------------------- */
static cairo_surface_t *
render_note_thumb(OnLibrary *lw, gint64 id)
{
    /* Load the note into an offscreen buffer, capping image decode at
     * 512 px: the card preview is at most ~256 physical pixels wide, so a
     * full-resolution decode (tens of MB per screenshot) is pure waste.
     * This buffer is never saved, so the scaled pixbufs are fine.          */
    GtkTextBuffer *buf = on_note_buffer_load(lw->app->db, id, 512);

    /* First embedded image, if any (borrowed ref, kept alive by buf).      */
    GdkPixbuf *img = NULL;           /* preview image for the card          */
    GtkTextIter it;                  /* scan cursor                         */
    gtk_text_buffer_get_start_iter(buf, &it);
    do {
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&it);
        img = (anchor != NULL) ? on_anchor_get_image(anchor, NULL)
                               : gtk_text_iter_get_pixbuf(&it);
        if (img != NULL)
            break;
    } while (gtk_text_iter_forward_char(&it));

    /* Body text: everything after the TITLE line.  The title is the first
     * non-empty line (matching on_buffer_first_line, which derives the
     * name shown under the card) — skipping only the literal first line
     * used to leave the title duplicated inside the thumbnail whenever a
     * note began with blank lines.                                         */
    GtkTextIter s, e;                /* full buffer bounds                  */
    gtk_text_buffer_get_bounds(buf, &s, &e);
    gchar *text = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    const gchar *body = text;        /* start of the post-title content     */
    while (*body == '\n')
        body++;                      /* skip leading blank lines            */
    const gchar *nl = strchr(body, '\n');
    body = (nl != NULL) ? nl + 1 : "";
    while (*body == '\n')
        body++;                      /* don't lead the card with blanks     */
    /* Walk at most 300 UTF-8 codepoints — avoids a full g_utf8_strlen scan
     * of the whole body followed by a second g_utf8_substring walk.       */
    const gchar *p = body;
    gint n = 0;
    while (*p && n < 300) { p = g_utf8_next_char(p); n++; }
    gchar *body_cut = g_strndup(body, (gsize)(p - body));

    /* Render at the display's scale factor so the card is pixel-sharp.     */
    gint sf = gtk_widget_get_scale_factor(lw->window);
    const gint SZ = THUMB_SIZE;      /* logical square edge length          */
    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, SZ * sf, SZ * sf);
    cairo_surface_set_device_scale(surface, sf, sf);
    cairo_t *cr = cairo_create(surface);

    /* White background with a light border.                                */
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.78, 0.78, 0.78);
    cairo_set_line_width(cr, 1);
    cairo_rectangle(cr, 0.5, 0.5, SZ - 1, SZ - 1);
    cairo_stroke(cr);

    gdouble y = 6;                   /* current vertical drawing position   */

    if (img != NULL) {
        /* Fit the image into the card's top area (logical units; cairo's
         * device scale keeps the physical pixels dense).                   */
        gint iw = gdk_pixbuf_get_width(img);
        gint ih = gdk_pixbuf_get_height(img);
        gdouble scale = MIN((gdouble)(SZ - 12) / iw, 72.0 / ih);
        scale = MIN(scale, 1.0);
        gdouble dw = iw * scale, dh = ih * scale;

        cairo_save(cr);
        cairo_translate(cr, (SZ - dw) / 2.0, y);
        cairo_scale(cr, scale, scale);
        gdk_cairo_set_source_pixbuf(cr, img, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr),
                                 CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_restore(cr);
        y += dh + 4;
    }

    /* Body preview (small grey), clipped to the card.                      */
    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_width(layout, (SZ - 12) * PANGO_SCALE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    PangoFontDescription *fd = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    gchar *markup = g_markup_printf_escaped(
        "<span foreground=\"#444444\">%s</span>", body_cut);
    pango_layout_set_markup(layout, markup, -1);
    g_free(markup);

    cairo_rectangle(cr, 6, y, SZ - 12, SZ - y - 6);
    cairo_clip(cr);
    cairo_move_to(cr, 6, y);
    pango_cairo_show_layout(cr, layout);

    g_object_unref(layout);
    cairo_destroy(cr);

    g_free(body_cut);
    g_free(text);
    g_object_unref(buf);
    return surface;
}

/* ---------------------------------------------------------------------------
 * get_note_thumb() — cached access to a note's thumbnail; re-renders only
 * when the note's updated_at changed since the cached render.
 * Returns a borrowed reference owned by the cache.
 * ------------------------------------------------------------------------- */
static cairo_surface_t *
get_note_thumb(OnLibrary *lw, gint64 id, gint64 updated_at)
{
    ThumbEntry *e = g_hash_table_lookup(lw->thumb_cache, &id);
    if (e != NULL && e->updated_at == updated_at)
        return e->surface;

    cairo_surface_t *thumb = render_note_thumb(lw, id);
    e = g_new0(ThumbEntry, 1);
    e->updated_at = updated_at;
    e->surface    = thumb;

    gint64 *key = g_new(gint64, 1);
    *key = id;
    g_hash_table_replace(lw->thumb_cache, key, e);
    return thumb;
}

/* How long one thumb_fill_idle() slice may run before yielding back to
 * the main loop (µs).  Big enough to batch several typical cards, small
 * enough to keep scrolling and typing smooth while a cold grid fills.       */
#define THUMB_IDLE_BUDGET_US (40 * 1000)

/* ---------------------------------------------------------------------------
 * thumb_fill_idle() — render queued thumbnails a time slice at a time and
 * deliver each into its grid row.  Jobs whose row vanished (model rebuilt
 * mid-fill) are simply dropped — the rebuild queued fresh jobs.
 * ------------------------------------------------------------------------- */
static gboolean
thumb_fill_idle(gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gint64 slice_start = g_get_monotonic_time();

    while (!g_queue_is_empty(&lw->thumb_pending)) {
        ThumbJob *job = g_queue_pop_head(&lw->thumb_pending);
        GtkTreePath *path = gtk_tree_row_reference_valid(job->row)
            ? gtk_tree_row_reference_get_path(job->row)
            : NULL;
        if (path != NULL) {
            cairo_surface_t *thumb =     /* borrowed from the cache         */
                get_note_thumb(lw, job->id, job->updated_at);
            GtkTreeIter iter;            /* the row to update               */
            if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                        &iter, path))
                gtk_list_store_set(lw->notes_store, &iter,
                                   NL_THUMB, thumb, -1);
            gtk_tree_path_free(path);
        }
        thumb_job_free(job);

        if (g_get_monotonic_time() - slice_start > THUMB_IDLE_BUDGET_US)
            break;                   /* yield; the idle re-runs             */
    }

    if (g_queue_is_empty(&lw->thumb_pending)) {
        lw->thumb_idle = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * refresh_actions() — repopulate the Action Items model (the notes
 * pane's third view) and show it: one row per '!' line across every
 * visible note, newest note first.  Same scroll-keeping contract as
 * refresh_notes.
 * ------------------------------------------------------------------------- */
static void
refresh_actions(OnLibrary *lw)
{
    gboolean keep_scroll = lw->shown_kind == lw->sel_kind;
    GtkWidget *sw =                  /* the view's scrolled window          */
        gtk_widget_get_parent(GTK_WIDGET(lw->actions_view));
    GtkAdjustment *vadj = GTK_IS_SCROLLED_WINDOW(sw)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(sw))
        : NULL;
    gdouble scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    lw->populating++;
    gtk_list_store_clear(lw->actions_store);
    GList *items = on_db_action_list(lw->app->db);
    for (GList *l = items; l != NULL; l = l->next) {
        OnActionItem *it = l->data;  /* one action item                     */
        if (it->done && !lw->app->show_done_actions)
            continue;                /* Settings: hide completed items      */
        gchar *when = NULL;          /* formatted due date, or NULL         */
        if (it->due != 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(it->due);
            when = g_date_time_format(dt, "%b %e, %Y");
            g_date_time_unref(dt);
        }
        GtkTreeIter iter;
        gtk_list_store_append(lw->actions_store, &iter);
        gtk_list_store_set(lw->actions_store, &iter,
                           AL_NOTE_ID, it->note_id,
                           AL_ORD,     it->ord,
                           AL_DONE,    it->done,
                           AL_TEXT,    it->text,
                           AL_DUE,     when != NULL ? when : "",
                           AL_DUE_RAW, it->due,
                           -1);
        g_free(when);
    }
    on_db_action_list_free(items);
    lw->populating--;

    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "actions");
    lw->shown_kind = lw->sel_kind;
    lw->shown_id   = lw->sel_id;
    if (keep_scroll && scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);
    status_path_update(lw);
}

/* ---------------------------------------------------------------------------
 * notes_for_selection() — the notes the current sidebar selection lists, in
 * display order.  THE one place that maps a selection onto a note set: the
 * notes pane populates from it, and so does the media browser, so the two
 * can never disagree about what "this folder" means.
 *   lw — the library window.
 * Returns a GList of OnNoteMeta*; free with on_db_note_list_free().
 * ------------------------------------------------------------------------- */
static GList *
notes_for_selection(OnLibrary *lw)
{
    if (lw->sel_kind == SB_KIND_TAG)
        return on_db_notes_by_tag(lw->app->db, lw->sel_id);
    if (lw->sel_kind == SB_KIND_PINNED)
        return on_db_note_list_pinned(lw->app->db);
    if (lw->sel_kind == SB_KIND_ALL)
        return on_db_note_list_recent(lw->app->db);
    if (lw->sel_kind == SB_KIND_TRASH)
        return on_db_note_list_trashed(lw->app->db);
    if (lw->sel_kind == SB_KIND_ACTIONS)
        return on_db_note_list_recent(lw->app->db);
                                     /* the Action Items view spans every
                                        note; refresh_notes never asks (it
                                        swaps in its own model first), but
                                        the media browser does             */
    return on_db_note_list(lw->app->db, lw->sel_id);
                                     /* root, folder, or trashed folder     */
}

/* ---------------------------------------------------------------------------
 * notes_preview_line() — the Comfortable-density preview for one note: the
 * first non-blank line of its body text AFTER the title line.  body_text
 * starts with the title followed by '\n', so that first line is skipped.
 *   body — the note's cached body text (may be NULL or truncated).
 * Returns a newly allocated line, or NULL when there is nothing to show.
 * ------------------------------------------------------------------------- */
static gchar *
notes_preview_line(const gchar *body)
{
    if (body == NULL)
        return NULL;
    const gchar *pos = strchr(body, '\n');
    if (pos != NULL)
        pos++;                       /* step past the title's newline        */
    while (pos != NULL && *pos != '\0') {
        const gchar *eol = strchr(pos, '\n');
        gchar *line = (eol != NULL) ? g_strndup(pos, eol - pos)
                                    : g_strdup(pos);
        g_strstrip(line);
        if (*line != '\0')
            return line;
        g_free(line);
        pos = (eol != NULL) ? eol + 1 : NULL;
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * refresh_notes() — repopulate the notes model from the current sidebar
 * selection (a folder's notes, or a tag's notes).  When the selection is
 * the same one already shown — a content refresh (autosave, editor
 * close), not a navigation — the scroll position is preserved.
 * ------------------------------------------------------------------------- */
static void
refresh_notes(OnLibrary *lw)
{
    /* Whatever happens below, the rows any queued thumbnail jobs point
     * at are stale (or about to be cleared): drop them.                    */
    thumb_pending_clear(lw);

    /* The Action Items selection swaps in its own view and model.          */
    if (lw->sel_kind == SB_KIND_ACTIONS) {
        refresh_actions(lw);
        return;
    }
    /* Leaving the actions view: restore the user's list/grid mode BEFORE
     * the thumbnail decision below reads the visible child.                */
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "actions") == 0)
        gtk_stack_set_visible_child_name(GTK_STACK(lw->stack),
                                         lw->grid_pref ? "grid" : "list");

    /* Thumbnails are only rendered while the grid is showing: list mode
     * never pays for them (they used to be regenerated for the edited
     * note on EVERY autosave), and switching to grid refreshes.            */
    gboolean want_thumbs =
        g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "grid") == 0;

    /* Same selection as last time?  Then remember where the visible
     * notes pane is scrolled so the rebuild doesn't jump to the top.       */
    gboolean keep_scroll = lw->shown_kind == lw->sel_kind &&
                           lw->shown_id   == lw->sel_id;
    GtkWidget     *vis_child = gtk_stack_get_visible_child(GTK_STACK(lw->stack));
    GtkAdjustment *vadj      = GTK_IS_SCROLLED_WINDOW(vis_child)
        ? gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(vis_child))
        : NULL;
    gdouble scroll_pos = vadj ? gtk_adjustment_get_value(vadj) : 0.0;

    /* On a content refresh (autosave, editor close) — same view, not a
     * navigation — capture the selection so we can put it back after the
     * store rebuild.  Navigations start with no selection by design.        */
    GArray *sel_ids = keep_scroll ? selected_note_ids(lw) : NULL;

    lw->populating++;
    gtk_list_store_clear(lw->notes_store);

    /* Pick the note list matching the selection.                           */
    GList *notes = notes_for_selection(lw);

    /* Folder paths for the list view's Path column — ONE query for all
     * folders, never per note (shared/network DBs).  The result is cached
     * in lw and reused across autosave-triggered refreshes; refresh_sidebar
     * clears it whenever folders change.                                    */
    if (lw->folder_path_cache == NULL)
        lw->folder_path_cache = on_db_folder_path_map(lw->app->db);
    GHashTable *paths = lw->folder_path_cache;

    /* Body-text previews for the Comfortable list density — ONE query, and
     * only where the preview is actually drawn: compact density never shows
     * it, and the grid draws thumbnails and the title, never NL_PREVIEW.    */
    GHashTable *previews = (lw->app->comfortable_list && !want_thumbs)
        ? on_db_note_text_map(lw->app->db, NL_PREVIEW_CHARS) : NULL;

    /* Autofit measuring rides this population loop (no second model
     * walk, no re-fetching the strings) and only while the LIST is the
     * visible view — the grid doesn't show these columns, and switching
     * back to list re-measures (on_view_list).  Repeated folder paths
     * are measured once, keyed by folder id.
     *
     * Only VISIBLE columns are measured: list_autofit_set() throws away the
     * width of a hidden one, so measuring it was pure waste — and Path and
     * Created are hidden by default.  The two timestamp columns are not
     * measured here at all: every value shares LIST_TIME_FORMAT, so
     * list_autofit_time_width() bounds them once after the loop instead of
     * once per row (measured ~68 ms for 1300 rows, on every refresh).       */
    gboolean fit      = lw->list_autofit && !want_thumbs;
    gboolean fit_path = fit && list_column_shown(lw, "path");
    gboolean fit_mod  = fit && list_column_shown(lw, "modified");
    gboolean fit_cre  = fit && list_column_shown(lw, "created");
    PangoLayout *fit_lay  = NULL;    /* reused measuring layout             */
    GHashTable  *fit_seen = NULL;    /* folder id → measured path width     */
    gint fit_path_w = 0;             /* running Path content maximum        */
    if (fit)
        fit_lay = gtk_widget_create_pango_layout(
            GTK_WIDGET(lw->notes_list), NULL);
    if (fit_path)
        fit_seen = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, NULL);

    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one note                            */

        /* Format the modification and creation times like
         * "Jun 3, 2026 14:05".                                             */
        GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
        gchar *when = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);
        dt = g_date_time_new_from_unix_local(m->created_at);
        gchar *born = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);

        /* "/Folder/Sub" location, "/" for the top level — the same
         * format as the status bar's path label.                           */
        const gchar *fpath = m->folder_id != 0
            ? g_hash_table_lookup(paths, &m->folder_id) : NULL;
        gchar *where = g_strdup_printf("/%s", fpath != NULL ? fpath : "");

        if (fit_path) {
            gint w;                  /* width of this note's path           */
            gpointer cached =
                g_hash_table_lookup(fit_seen, &m->folder_id);
            if (cached != NULL) {
                w = GPOINTER_TO_INT(cached);
            } else {
                pango_layout_set_text(fit_lay, where, -1);
                pango_layout_get_pixel_size(fit_lay, &w, NULL);
                gint64 *k = g_new(gint64, 1);
                *k = m->folder_id;
                g_hash_table_insert(fit_seen, k, GINT_TO_POINTER(w));
            }
            fit_path_w = MAX(fit_path_w, w);
        }

        /* Thumbnails: only what the cache already has goes in right away
         * — a stale entry still shows (better than a blank card) while
         * thumb_fill_idle (queued below) renders the replacement.
         * Rendering every stale/missing thumbnail here froze the GUI.      */
        cairo_surface_t *thumb = NULL;   /* borrowed from the cache         */
        gboolean thumb_todo = FALSE;     /* queue a render for this row?    */
        if (want_thumbs) {
            ThumbEntry *e = g_hash_table_lookup(lw->thumb_cache, &m->id);
            if (e != NULL)
                thumb = e->surface;
            thumb_todo = e == NULL || e->updated_at != m->updated_at;
        }

        gchar *preview = notes_preview_line(
            previews ? g_hash_table_lookup(previews, &m->id) : NULL);

        GtkTreeIter iter;
        gtk_list_store_append(lw->notes_store, &iter);
        gtk_list_store_set(lw->notes_store, &iter,
                           NL_ID,          m->id,
                           NL_TITLE,       m->title,
                           NL_MODIFIED,    when,
                           NL_THUMB,       thumb,
                           NL_UPDATED,     m->updated_at,
                           NL_PATH,        where,
                           NL_CREATED,     born,
                           NL_CREATED_RAW, m->created_at,
                           NL_PREVIEW,     preview,
                           -1);
        g_free(preview);
        if (thumb_todo) {
            GtkTreePath *path = gtk_tree_model_get_path(
                GTK_TREE_MODEL(lw->notes_store), &iter);
            ThumbJob *job = g_new0(ThumbJob, 1);
            job->row = gtk_tree_row_reference_new(
                GTK_TREE_MODEL(lw->notes_store), path);
            job->id         = m->id;
            job->updated_at = m->updated_at;
            g_queue_push_tail(&lw->thumb_pending, job);
            gtk_tree_path_free(path);
        }
        g_free(where);
        g_free(when);
        g_free(born);
    }
    /* paths == lw->folder_path_cache — kept alive for the next refresh.     */
    if (previews != NULL) g_hash_table_destroy(previews);
    on_db_note_list_free(notes);

    if (fit) {
        /* Both timestamp columns render the same format, so one bound
         * serves both.  Computed only if one of them is actually shown.     */
        gint time_w = (fit_mod || fit_cre)
                      ? list_autofit_time_width(fit_lay) : 0;
        if (fit_path)
            list_autofit_set(lw, fit_lay, "path",     fit_path_w);
        if (fit_mod)
            list_autofit_set(lw, fit_lay, "modified", time_w);
        if (fit_cre)
            list_autofit_set(lw, fit_lay, "created",  time_w);
        if (fit_seen != NULL)
            g_hash_table_destroy(fit_seen);
        g_object_unref(fit_lay);
    }
    lw->populating--;

    /* Restore the note selection that existed before the store rebuild.     */
    if (sel_ids != NULL) {
        if (sel_ids->len > 0) {
            const gchar *mode = gtk_stack_get_visible_child_name(
                GTK_STACK(lw->stack));
            gboolean in_grid  = g_strcmp0(mode, "grid") == 0;
            GtkTreeSelection *list_sel =
                gtk_tree_view_get_selection(lw->notes_list);
            GtkTreeIter it;
            gboolean valid = gtk_tree_model_get_iter_first(
                GTK_TREE_MODEL(lw->notes_store), &it);
            while (valid) {
                gint64 id;
                gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &it,
                                   NL_ID, &id, -1);
                for (guint k = 0; k < sel_ids->len; k++) {
                    if (g_array_index(sel_ids, gint64, k) == id) {
                        if (in_grid) {
                            GtkTreePath *p = gtk_tree_model_get_path(
                                GTK_TREE_MODEL(lw->notes_store), &it);
                            gtk_icon_view_select_path(lw->notes_grid, p);
                            gtk_tree_path_free(p);
                        } else {
                            gtk_tree_selection_select_iter(list_sel, &it);
                        }
                        break;
                    }
                }
                valid = gtk_tree_model_iter_next(
                    GTK_TREE_MODEL(lw->notes_store), &it);
            }
        }
        g_array_free(sel_ids, TRUE);
    }

    /* Render the queued (stale/missing) thumbnails in idle time slices.    */
    if (!g_queue_is_empty(&lw->thumb_pending) && lw->thumb_idle == 0)
        lw->thumb_idle = g_idle_add(thumb_fill_idle, lw);

    lw->shown_kind = lw->sel_kind;
    lw->shown_id   = lw->sel_id;
    if (keep_scroll && scroll_pos > 0)
        scroll_keep_queue(vadj, scroll_pos);

    status_path_update(lw);
}

/* ---------------------------------------------------------------------------
 * refresh_all() — rebuild the sidebar and the notes pane together: the
 * standard follow-up to any change that can touch both (moves, deletes,
 * tag edits, …).  refresh_sidebar keeps its populating guard up through
 * the selection restore, so the explicit refresh_notes here is the one
 * that repopulates the pane (the sidebar's own fallback refresh — taken
 * when the old selection vanished — is redundant but harmless).
 * ------------------------------------------------------------------------- */
static void
refresh_all(OnLibrary *lw)
{
    refresh_sidebar(lw);
    refresh_notes(lw);
}

/* ---------------------------------------------------------------------------
 * persist_note_order() — write the notes model's current row order back
 * to the database.  Only meaningful when a folder (not a tag) is shown.
 * ------------------------------------------------------------------------- */
static void
persist_note_order(OnLibrary *lw)
{
    /* Only real folder views own an ordering; the tag, pinned, all-notes
     * and trash views are assembled from elsewhere (persisting a drag in
     * All Notes would scramble every folder's internal order).             */
    if (lw->sel_kind == SB_KIND_TAG || lw->sel_kind == SB_KIND_PINNED ||
        lw->sel_kind == SB_KIND_ALL || lw->sel_kind == SB_KIND_ACTIONS ||
        in_trash_view(lw))
        return;

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeIter iter;                /* row cursor                          */
    gboolean valid = gtk_tree_model_get_iter_first(
        GTK_TREE_MODEL(lw->notes_store), &iter);
    while (valid) {
        gint64 id;                   /* note id of this row                 */
        gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                           NL_ID, &id, -1);
        g_array_append_val(ids, id);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(lw->notes_store),
                                         &iter);
    }
    on_db_note_reorder(lw->app->db, (gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
}

/* ---------------------------------------------------------------------------
 * on_notes_row_deleted() — fires at the end of a built-in drag-reorder
 * (GtkTreeView implements reordering as insert+delete).  Persist the new
 * order unless we are the ones rewriting the model.
 * ------------------------------------------------------------------------- */
static void
on_notes_row_deleted(GtkTreeModel *model, GtkTreePath *path,
                     gpointer user_data)
{
    (void)model; (void)path;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating == 0)
        persist_note_order(lw);
}

/* ===========================================================================
 * selection and activation
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * on_sidebar_selection_changed() — a folder or tag was selected: remember
 * it and refresh the notes pane.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_selection_changed(GtkTreeSelection *sel, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating > 0)
        return;

    GtkTreeModel *model;             /* the sidebar model                   */
    GtkTreeIter iter;                /* selected row                        */
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;

    gint   kind;                     /* selected row kind                   */
    gint64 id;                       /* selected row id                     */
    gchar *raw = NULL;               /* bare name of the row                */
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, SB_ID, &id,
                       SB_RAW, &raw, -1);
    if (kind == SB_KIND_TAGS_HEADER) {
        g_free(raw);
        return;                      /* header row: not a real selection    */
    }

    lw->sel_kind = kind;
    lw->sel_id   = id;
    g_free(lw->sel_name);
    lw->sel_name = raw;              /* ownership transferred               */
    refresh_notes(lw);
}

/* sidebar_select_func() — forbid selecting the "Tags" header row.           */
static gboolean
sidebar_select_func(GtkTreeSelection *sel, GtkTreeModel *model,
                    GtkTreePath *path, gboolean currently_selected,
                    gpointer user_data)
{
    (void)sel; (void)currently_selected; (void)user_data;
    GtkTreeIter iter;                /* row being (de)selected              */
    gint kind;                       /* its kind                            */
    gtk_tree_model_get_iter(model, &iter, path);
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
    return kind != SB_KIND_TAGS_HEADER;
}

/* ---------------------------------------------------------------------------
 * open_note_at_path() — open the editor for the note at `path` (NOT
 * owned) in the notes model; the shared tail of both views' activation
 * handlers.
 * ------------------------------------------------------------------------- */
static void
open_note_at_path(OnLibrary *lw, GtkTreePath *path)
{
    GtkTreeIter iter;                /* activated row                       */
    gint64 id;                       /* its note id                         */
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                 &iter, path))
        return;
    gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                       NL_ID, &id, -1);
    on_editor_window_open(lw->app, id);
}

/* on_note_list_activated() — double-click/Enter in list mode opens it.      */
static void
on_note_list_activated(GtkTreeView *view, GtkTreePath *path,
                       GtkTreeViewColumn *col, gpointer user_data)
{
    (void)view; (void)col;
    open_note_at_path(user_data, path);
}

/* on_note_grid_activated() — double-click in grid mode opens the note.      */
static void
on_note_grid_activated(GtkIconView *view, GtkTreePath *path,
                       gpointer user_data)
{
    (void)view;
    open_note_at_path(user_data, path);
}

/* ---------------------------------------------------------------------------
 * on_action_toggled() — the Action Items checkbox column: flip the item's
 * done state everywhere — the model row (instant feedback), its
 * action_items row, and the note text itself (strikethrough) via
 * on_editor_action_set_done.
 * ------------------------------------------------------------------------- */
static void
on_action_toggled(GtkCellRendererToggle *cell, gchar *path_str,
                  gpointer user_data)
{
    (void)cell;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeIter iter;                /* the clicked row                     */
    if (!gtk_tree_model_get_iter_from_string(
            GTK_TREE_MODEL(lw->actions_store), &iter, path_str))
        return;

    gint64   note_id;                /* the item's address                  */
    gint     ord;
    gboolean done;
    gtk_tree_model_get(GTK_TREE_MODEL(lw->actions_store), &iter,
                       AL_NOTE_ID, &note_id,
                       AL_ORD,     &ord,
                       AL_DONE,    &done,
                       -1);
    done = !done;

    /* A just-completed item disappears immediately when completed items
     * are hidden; otherwise the row simply re-renders checked + struck.    */
    if (done && !lw->app->show_done_actions)
        gtk_list_store_remove(lw->actions_store, &iter);
    else
        gtk_list_store_set(lw->actions_store, &iter, AL_DONE, done, -1);
    /* The content rewrite is authoritative and normally rebuilds the
     * mirror itself; only a LIVE editor defers that to its autosave, and
     * only then does the flag need writing here as well.                    */
    gboolean synced = FALSE;         /* did the rewrite update the table?   */
    if (on_editor_action_set_done(lw->app, note_id, ord, done, &synced) &&
        !synced)
        on_db_action_set_done(lw->app->db, note_id, ord, done);
    if (lw->app->sidebar_counts)
        refresh_sidebar(lw);         /* the section's open count changed    */
}

/* ---------------------------------------------------------------------------
 * action_due_dialog() — modal calendar for one action item's due date.
 * Set rewrites the "due YYYY-MM-DD" suffix of the '!' line in the note
 * text (on_editor_action_set_due), Clear removes it; the store row
 * updates immediately, the durable action_items row follows from the
 * content rewrite.
 * ------------------------------------------------------------------------- */
static void
action_due_dialog(OnLibrary *lw, GtkTreeIter *iter)
{
    gint64 note_id, due;             /* the item's address + current due    */
    gint   ord;
    gtk_tree_model_get(GTK_TREE_MODEL(lw->actions_store), iter,
                       AL_NOTE_ID, &note_id,
                       AL_ORD,     &ord,
                       AL_DUE_RAW, &due,
                       -1);

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "Notes - Due Date", GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        "_Clear",  1,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Set",    GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *cal = gtk_calendar_new();
    if (due != 0) {                  /* open on the current due date        */
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        gtk_calendar_select_month(GTK_CALENDAR(cal),
                                  (guint)(g_date_time_get_month(dt) - 1),
                                  (guint)g_date_time_get_year(dt));
        gtk_calendar_select_day(GTK_CALENDAR(cal),
                                (guint)g_date_time_get_day_of_month(dt));
        g_date_time_unref(dt);
    }
    /* GtkCalendar is a plain widget, not a container: use margins.         */
    gtk_widget_set_margin_start(cal, 8);
    gtk_widget_set_margin_end(cal, 8);
    gtk_widget_set_margin_top(cal, 8);
    gtk_widget_set_margin_bottom(cal, 8);
    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
        cal, TRUE, TRUE, 0);
    gtk_widget_show_all(cal);

    gint response = gtk_dialog_run(GTK_DIALOG(dlg));
    gint64 new_due = -1;             /* -1 = leave unchanged                */
    if (response == GTK_RESPONSE_OK) {
        guint y, m, d;               /* calendar month is 0-based           */
        gtk_calendar_get_date(GTK_CALENDAR(cal), &y, &m, &d);
        GDateTime *dt = g_date_time_new_local((gint)y, (gint)m + 1,
                                              (gint)d, 0, 0, 0);
        if (dt != NULL) {
            new_due = g_date_time_to_unix(dt);
            g_date_time_unref(dt);
        }
    } else if (response == 1) {
        new_due = 0;                 /* Clear                               */
    }
    gtk_widget_destroy(dlg);
    if (new_due < 0)
        return;

    if (on_editor_action_set_due(lw->app, note_id, ord, new_due)) {
        gchar *when = NULL;          /* formatted for the Due Date cell     */
        if (new_due != 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(new_due);
            when = g_date_time_format(dt, "%b %e, %Y");
            g_date_time_unref(dt);
        }
        gtk_list_store_set(lw->actions_store, iter,
                           AL_DUE,     when != NULL ? when : "",
                           AL_DUE_RAW, new_due,
                           -1);
        g_free(when);
    }
}

/* on_action_row_activated() — double-click: the Due Date cell opens the
 * date selector; any other cell opens the item's note.                      */
static void
on_action_row_activated(GtkTreeView *view, GtkTreePath *path,
                        GtkTreeViewColumn *column, gpointer user_data)
{
    (void)view;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeIter iter;                /* the activated row                   */
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->actions_store),
                                 &iter, path))
        return;

    if (column != NULL &&
        g_strcmp0(g_object_get_data(G_OBJECT(column), "on-colkey"),
                  "due") == 0) {
        action_due_dialog(lw, &iter);
        return;
    }

    gint64 note_id;                  /* owning note                         */
    gtk_tree_model_get(GTK_TREE_MODEL(lw->actions_store), &iter,
                       AL_NOTE_ID, &note_id, -1);
    GtkWidget *win = on_editor_window_open(lw->app, note_id);
    if (win != NULL)
        gtk_window_present(GTK_WINDOW(win));
}

/* ===========================================================================
 * drag & drop: note → folder, folder → folder
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * folder_move_beside() — re-parent folder `folder_id` under `new_parent`
 * and slot it directly before/after sibling `anchor_id` (the row the drop
 * indicator pointed at).  The move appends at the end of the new parent;
 * the reorder then writes the full sibling sequence with the moved folder
 * re-inserted at the anchor.
 * ------------------------------------------------------------------------- */
static gboolean
folder_move_beside(OnLibrary *lw, gint64 folder_id, gint64 new_parent,
                   gint64 anchor_id, gboolean after)
{
    if (!on_db_folder_move(lw->app->db, folder_id, new_parent))
        return FALSE;

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GList *sibs = on_db_folder_list(lw->app->db, new_parent);
    for (GList *l = sibs; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one sibling                         */
        if (f->id == folder_id)
            continue;                /* re-inserted at the anchor below     */
        if (f->id == anchor_id && !after)
            g_array_append_val(ids, folder_id);
        g_array_append_val(ids, f->id);
        if (f->id == anchor_id && after)
            g_array_append_val(ids, folder_id);
    }
    on_db_folder_list_free(sibs);

    gboolean ok = on_db_folder_reorder(lw->app->db,
                                       (const gint64 *)ids->data, ids->len);
    g_array_free(ids, TRUE);
    return ok;
}

/* ---------------------------------------------------------------------------
 * sidebar_drop_target() — resolve and validate the drop target under the
 * pointer for the drag in `context`.  Which rows are legal depends on the
 * drag's SOURCE widget: from the sidebar itself only folder rows move
 * (onto folders, the root, or the Trash, never onto themselves or into
 * their own subtree — the dragged row is the sidebar's selected row,
 * since a button press always moves the single-mode selection before the
 * drag threshold is crossed); from the notes views any folder-ish row
 * accepts, and the position is coerced to INTO (a note drops *into* a
 * folder, never beside it).
 * Returns TRUE and fills `path_out` (caller frees) + `pos_out` when the
 * drop is legal.
 * ------------------------------------------------------------------------- */
static gboolean
sidebar_drop_target(OnLibrary *lw, GdkDragContext *context, gint x, gint y,
                    GtkTreePath **path_out, GtkTreeViewDropPosition *pos_out)
{
    *path_out = NULL;
    *pos_out  = GTK_TREE_VIEW_DROP_BEFORE;

    if (gtk_drag_dest_find_target(GTK_WIDGET(lw->sidebar), context,
                                  NULL) == GDK_NONE)
        return FALSE;                /* not a GTK_TREE_MODEL_ROW drag       */

    GtkTreePath *path = NULL;        /* row under the pointer               */
    GtkTreeViewDropPosition pos;     /* before/into/after                   */
    if (!gtk_tree_view_get_dest_row_at_pos(lw->sidebar, x, y, &path, &pos))
        return FALSE;

    GtkTreeModel *model = GTK_TREE_MODEL(lw->sidebar_store);
    GtkTreeIter iter;                /* target row                          */
    gint kind = -1;                  /* its kind                            */
    if (gtk_tree_model_get_iter(model, &iter, path))
        gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);

    gboolean ok = FALSE;             /* is this drop legal?                 */
    if (gtk_drag_get_source_widget(context) == GTK_WIDGET(lw->sidebar)) {
        /* --- folder drag ---------------------------------------------------*/
        GtkTreeIter  src_iter;       /* dragged (= selected) row            */
        GtkTreeModel *m;
        gint  src_kind = -1;         /* its kind                            */
        GtkTreePath *src_path = NULL;
        if (gtk_tree_selection_get_selected(
                gtk_tree_view_get_selection(lw->sidebar), &m, &src_iter)) {
            gtk_tree_model_get(m, &src_iter, SB_KIND, &src_kind, -1);
            src_path = gtk_tree_model_get_path(m, &src_iter);
        }
        if ((src_kind == SB_KIND_FOLDER ||
             src_kind == SB_KIND_TRASH_FOLDER) &&
            src_path != NULL &&
            gtk_tree_path_compare(src_path, path) != 0 &&
            !gtk_tree_path_is_descendant(path, src_path)) {
            if (kind == SB_KIND_FOLDER) {
                ok = TRUE;           /* nest INTO or reorder beside it      */
            } else if (kind == SB_KIND_ROOT ||
                       (kind == SB_KIND_TRASH &&
                        src_kind == SB_KIND_FOLDER)) {
                ok  = TRUE;
                pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
            }
        }
        if (src_path != NULL)
            gtk_tree_path_free(src_path);
    } else {
        /* --- note drag -----------------------------------------------------*/
        if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT ||
            kind == SB_KIND_TRASH) {
            ok  = TRUE;
            pos = GTK_TREE_VIEW_DROP_INTO_OR_BEFORE;
        }
    }

    if (ok) {
        *path_out = path;
        *pos_out  = pos;
    } else {
        gtk_tree_path_free(path);
    }
    return ok;
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_motion() — answer every motion with gdk_drag_status()
 * and draw the drop indicator ourselves.  This REPLACES GtkTreeView's
 * default handler (returning TRUE stops the class closure), which for
 * GTK_TREE_MODEL_ROW targets requests the drag DATA on every motion to
 * validate the drop — those requests fire "drag-data-received" mid-drag,
 * and on quartz the reply arrives before the drop, so a received handler
 * that treats every delivery as a drop runs with stale coordinates and
 * gtk_drag_finish()es the drag while the button is still down (drops
 * landing only when the X11-style late reply happens to slip past the
 * release).  See quirk #13.
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_drag_motion(GtkWidget *widget, GdkDragContext *context,
                       gint x, gint y, guint time, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = NULL;        /* legal target row (or NULL)          */
    GtkTreeViewDropPosition pos;     /* indicator position                  */
    gboolean ok = sidebar_drop_target(lw, context, x, y, &path, &pos);

    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget),
                                    ok ? path : NULL, pos);
    gdk_drag_status(context, ok ? GDK_ACTION_MOVE : 0, time);
    if (path != NULL)
        gtk_tree_path_free(path);
    return TRUE;
}

/* on_sidebar_drag_leave() — clear the drop indicator (also fires right
 * before every drop; the class handler additionally kills its timers).     */
static void
on_sidebar_drag_leave(GtkWidget *widget, GdkDragContext *context,
                      guint time, gpointer user_data)
{
    (void)context; (void)time; (void)user_data;
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL,
                                    GTK_TREE_VIEW_DROP_BEFORE);
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_drop() — the button was released on a legal target:
 * request the row data (the actual move runs in drag-data-received, the
 * only place the dragged row can be decoded).  Returning TRUE keeps the
 * default handler out; FALSE (no legal target) cancels the drop cleanly.
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_drag_drop(GtkWidget *widget, GdkDragContext *context,
                     gint x, gint y, guint time, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = NULL;        /* legal target row (or NULL)          */
    GtkTreeViewDropPosition pos;     /* unused here                         */
    gboolean ok = sidebar_drop_target(lw, context, x, y, &path, &pos);
    if (path != NULL)
        gtk_tree_path_free(path);
    if (!ok)
        return FALSE;
    gtk_drag_get_data(widget, context,
                      gdk_atom_intern_static_string("GTK_TREE_MODEL_ROW"),
                      time);
    return TRUE;
}

/* Logical pixel size of the custom drag-under-cursor icons.                 */
#define DRAG_ICON_SIZE 32

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_begin() — replace the drag-under-cursor icon of a
 * folder drag with the folder glyph (non-folder rows keep the default
 * row snapshot).  Connected AFTER the class handler, which would
 * otherwise override our icon with its own.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_drag_begin(GtkWidget *widget, GdkDragContext *context,
                      gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */

    GtkTreeModel *m;                 /* the sidebar model                   */
    GtkTreeIter iter;                /* dragged (= selected) row            */
    gint kind = -1;                  /* its kind                            */
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), &m, &iter))
        gtk_tree_model_get(m, &iter, SB_KIND, &kind, -1);
    if (kind != SB_KIND_FOLDER && kind != SB_KIND_TRASH_FOLDER)
        return;

    cairo_surface_t *icon =
        on_app_icon_surface(lw->app, "folder", DRAG_ICON_SIZE);
    if (icon != NULL) {
        gtk_drag_set_icon_surface(context, icon);
        cairo_surface_destroy(icon);
    }
}

/* ---------------------------------------------------------------------------
 * on_notes_drag_begin() — drag-under-cursor icon for note drags (both
 * views): one file for a single note, a document stack for several.
 * The press that started the drag has already settled the selection, so
 * its count IS the drag count.  Connected AFTER the class handler, as
 * above.
 * ------------------------------------------------------------------------- */
static void
on_notes_drag_begin(GtkWidget *widget, GdkDragContext *context,
                    gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */

    /* A drag from a blocked press keeps the whole multi-selection: just
     * lift the veto — the matching release lands in the DnD machinery,
     * never as a button-release on the view.                               */
    notes_sel_unblock(lw, FALSE);

    cairo_surface_t *icon = on_app_icon_surface(
        lw->app, selected_note_count(lw) > 1 ? "documents" : "file",
        DRAG_ICON_SIZE);
    if (icon != NULL) {
        gtk_drag_set_icon_surface(context, icon);
        cairo_surface_destroy(icon);
    }
}

/* ---------------------------------------------------------------------------
 * on_sidebar_drag_received() — a row was dropped on the sidebar: either a
 * notes row (move the note — or the whole multi-selection — into the
 * target folder, or trash it) or one of the sidebar's own folder rows
 * (re-nest INTO a folder, reorder BEFORE/AFTER a sibling, trash, or
 * restore-by-drag out of the Trash).  Fires exactly once per drop — only
 * on_sidebar_drag_drop() requests the data (the default motion-time
 * requests never happen, see on_sidebar_drag_motion), so x/y are the real
 * drop coordinates.  The default GtkTreeView handler is suppressed
 * because it would try to splice the dragged row into the *sidebar*
 * model.
 * ------------------------------------------------------------------------- */
static void
on_sidebar_drag_received(GtkWidget *widget, GdkDragContext *context,
                         gint x, gint y, GtkSelectionData *seldata,
                         guint info, guint time, gpointer user_data)
{
    (void)info;
    OnLibrary *lw = user_data;       /* owning library window               */
    g_signal_stop_emission_by_name(widget, "drag-data-received");

    gboolean success = FALSE;        /* whether anything moved              */

    /* Decode the dragged row.                                              */
    GtkTreeModel *src_model = NULL;  /* model the drag started in           */
    GtkTreePath  *src_path  = NULL;  /* dragged row's path                  */
    if (!gtk_tree_get_row_drag_data(seldata, &src_model, &src_path)) {
        gtk_drag_finish(context, FALSE, FALSE, time);
        return;
    }

    /* Resolve the drop target row (shared by both branches below).         */
    GtkTreeModel *sb_model  = GTK_TREE_MODEL(lw->sidebar_store);
    GtkTreePath  *dest_path = NULL;  /* row under the pointer               */
    GtkTreeViewDropPosition pos = GTK_TREE_VIEW_DROP_BEFORE;
    GtkTreeIter dest_iter;           /* target sidebar row                  */
    gint   dest_kind = -1;           /* its kind (-1 = no target row)       */
    gint64 dest_id   = 0;            /* its folder/tag id                   */
    gchar *dest_raw  = NULL;         /* its bare name                       */
    if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget),
                                          x, y, &dest_path, &pos) &&
        gtk_tree_model_get_iter(sb_model, &dest_iter, dest_path))
        gtk_tree_model_get(sb_model, &dest_iter,
                           SB_KIND, &dest_kind, SB_ID, &dest_id,
                           SB_RAW,  &dest_raw, -1);

    if (src_model == GTK_TREE_MODEL(lw->notes_store) && dest_kind != -1) {
        /* --- a note row from the notes pane -------------------------------*/
        GtkTreeIter src_iter;        /* dragged note row                    */
        gint64 note_id = 0;          /* dragged note id                     */
        if (gtk_tree_model_get_iter(src_model, &src_iter, src_path))
            gtk_tree_model_get(src_model, &src_iter, NL_ID, &note_id, -1);

        if (note_id != 0 &&
            (dest_kind == SB_KIND_FOLDER || dest_kind == SB_KIND_ROOT ||
             dest_kind == SB_KIND_TRASH)) {
            /* Multi-select support: when the dragged note is part of
             * the current selection, the whole selection moves.            */
            GArray *ids = selected_note_ids(lw);
            gboolean drag_in_selection = FALSE;
            for (guint i = 0; i < ids->len; i++)
                if (g_array_index(ids, gint64, i) == note_id)
                    drag_in_selection = TRUE;
            if (!drag_in_selection) {
                g_array_set_size(ids, 0);
                g_array_append_val(ids, note_id);
            }
            if (dest_kind == SB_KIND_TRASH) {
                /* Dropping on Trash IS the delete gesture.                 */
                success = trash_notes_core(
                    lw, (const gint64 *)ids->data, ids->len);
            } else {
                /* ONE transaction for the whole selection: per-note
                 * moves fsync per call and froze the GUI on big drops.  */
                success = on_db_notes_move(
                    lw->app->db, (const gint64 *)ids->data, ids->len,
                    dest_id);
                if (success)
                    on_app_status(lw->app,
                                  "Moved %u note%s to "
                                  "\xe2\x80\x9c%s\xe2\x80\x9d",
                                  ids->len, ids->len == 1 ? "" : "s",
                                  dest_raw);
            }
            g_array_free(ids, TRUE);
        }
    } else if (src_model == sb_model && dest_kind != -1) {
        /* --- one of the sidebar's own folder rows --------------------------*/
        GtkTreeIter src_iter;        /* dragged sidebar row                 */
        gint   src_kind  = -1;       /* its kind                            */
        gint64 folder_id = 0;        /* its folder id                       */
        gchar *fname     = NULL;     /* its bare name                       */
        if (gtk_tree_model_get_iter(src_model, &src_iter, src_path))
            gtk_tree_model_get(src_model, &src_iter,
                               SB_KIND, &src_kind, SB_ID, &folder_id,
                               SB_RAW,  &fname, -1);

        /* Only real folders move (trashed ones too — that's drag-restore);
         * a drop onto the row itself or into its own subtree is refused
         * (on_db_folder_move re-checks against the database).              */
        gboolean movable =
            (src_kind == SB_KIND_FOLDER ||
             src_kind == SB_KIND_TRASH_FOLDER) &&
            folder_id != 0 &&
            gtk_tree_path_compare(src_path, dest_path) != 0 &&
            !gtk_tree_path_is_descendant(dest_path, src_path);

        if (movable && dest_kind == SB_KIND_TRASH &&
            src_kind == SB_KIND_FOLDER) {
            /* Dropping on Trash IS the delete gesture.                     */
            success = trash_folder(lw, folder_id, fname);
        } else if (movable && (dest_kind == SB_KIND_FOLDER ||
                               dest_kind == SB_KIND_ROOT)) {
            /* INTO a folder (or anywhere on the root) re-nests, appended
             * at the end; BEFORE/AFTER a folder row slots the dragged
             * folder beside that sibling (re-nesting when the sibling
             * lives under a different parent).                             */
            gchar *where = NULL;     /* name for the status message         */
            if (dest_kind == SB_KIND_ROOT ||
                pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE ||
                pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER) {
                gint64 new_parent =  /* root drops land at top level        */
                    (dest_kind == SB_KIND_FOLDER) ? dest_id : 0;
                success = on_db_folder_move(lw->app->db, folder_id,
                                            new_parent);
                where = g_strdup(dest_raw);
            } else {
                /* The dest folder's parent row is the new parent: the
                 * "Notes" root maps to top level (0).                      */
                GtkTreeIter par_iter;        /* dest's parent row           */
                gint   par_kind = SB_KIND_ROOT;
                gint64 par_id   = 0;
                gchar *par_raw  = NULL;
                if (gtk_tree_model_iter_parent(sb_model, &par_iter,
                                               &dest_iter))
                    gtk_tree_model_get(sb_model, &par_iter,
                                       SB_KIND, &par_kind,
                                       SB_ID,   &par_id,
                                       SB_RAW,  &par_raw, -1);
                gint64 new_parent =
                    (par_kind == SB_KIND_FOLDER) ? par_id : 0;
                success = folder_move_beside(
                    lw, folder_id, new_parent, dest_id,
                    pos == GTK_TREE_VIEW_DROP_AFTER);
                where = par_raw ? par_raw : g_strdup("Notes");
            }
            if (success) {
                on_app_status(lw->app,
                              src_kind == SB_KIND_TRASH_FOLDER
                                  ? "Folder \xe2\x80\x9c%s\xe2\x80\x9d "
                                    "restored to \xe2\x80\x9c%s\xe2\x80\x9d"
                                  : "Moved folder \xe2\x80\x9c%s\xe2\x80\x9d "
                                    "to \xe2\x80\x9c%s\xe2\x80\x9d",
                              fname, where);
                /* A trashed folder dragged out re-enters the tree as a
                 * normal folder; keep the selection on it.                 */
                if (lw->sel_kind == SB_KIND_TRASH_FOLDER &&
                    lw->sel_id == folder_id)
                    lw->sel_kind = SB_KIND_FOLDER;
            }
            g_free(where);
        }
        g_free(fname);
    }

    g_free(dest_raw);
    if (dest_path != NULL)
        gtk_tree_path_free(dest_path);
    gtk_tree_path_free(src_path);

    /* Finish the DnD handshake FIRST — the refreshes below rebuild both
     * models, and running them before gtk_drag_finish() stalls the drop
     * (the source sits waiting while we repaint).                          */
    gtk_drag_finish(context, success, FALSE, time);
    if (success)
        refresh_all(lw);             /* tree shape and counts changed       */
}

/* ===========================================================================
 * commands: new note / new folder / delete / rename
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * current_folder_id() — the folder new notes/folders should land in:
 * the selected folder, or the top level when the root or a tag is
 * selected.
 * ------------------------------------------------------------------------- */
static gint64
current_folder_id(OnLibrary *lw)
{
    return (lw->sel_kind == SB_KIND_FOLDER) ? lw->sel_id : 0;
}

/* ---------------------------------------------------------------------------
 * prompt_for_folder() — modal dialog: name entry + Normal/Project/Custom
 * AI mode radios + emoji picker.
 *   lw            — the library window (dialog parent).
 *   title         — dialog window title.
 *   initial_name  — pre-filled name, or NULL.
 *   initial_mode  — ON_AI_MODE_* to pre-select.
 *   ai_mode_out   — receives the chosen ON_AI_MODE_* (may be NULL).
 *   initial_emoji — pre-filled emoji string, or NULL / "".
 *   emoji_out     — receives the chosen emoji (owned; g_free() it; may be
 *                   NULL if caller doesn't need it).
 * Returns the entered name (g_free() it), or NULL if cancelled/empty.
 * ------------------------------------------------------------------------- */

/* folder_emoji_chooser_closed() — resize the dialog back to natural size
 * after the GTK emoji chooser popover closes.                               */
static void
folder_emoji_chooser_closed(GtkWidget *chooser, gpointer data)
{
    (void)chooser;
    gtk_window_resize(GTK_WINDOW(data), 1, 1);
}

/* folder_emoji_open_idle() — deferred: fire the GTK emoji picker on the
 * emoji entry and hook its closed signal.                                   */
static gboolean
folder_emoji_open_idle(gpointer data)
{
    GtkWidget *entry = data;
    g_signal_emit_by_name(entry, "insert-emoji");
    GtkWidget *chooser =
        g_object_get_data(G_OBJECT(entry), "gtk-emoji-chooser");
    if (chooser != NULL &&
        !g_object_get_data(G_OBJECT(entry), "on-close-hooked")) {
        GtkWidget *dlg = g_object_get_data(G_OBJECT(entry), "on-dialog");
        g_signal_connect(chooser, "closed",
                         G_CALLBACK(folder_emoji_chooser_closed), dlg);
        g_object_set_data(G_OBJECT(entry), "on-close-hooked",
                          GINT_TO_POINTER(1));
    }
    return G_SOURCE_REMOVE;
}

/* folder_emoji_pressed() — button-press on the emoji entry: clear it,
 * grow the dialog to give the popover room, open the picker next idle.      */
static gboolean
folder_emoji_pressed(GtkWidget *entry, GdkEventButton *ev, gpointer data)
{
    (void)ev;
    GtkWidget *dlg = data;
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    gint w, h;
    gtk_window_get_size(GTK_WINDOW(dlg), &w, &h);
    gtk_window_resize(GTK_WINDOW(dlg), MAX(w, 440), 470);
    g_idle_add(folder_emoji_open_idle, entry);
    return TRUE;
}

static gchar *
prompt_for_folder(OnLibrary *lw, const gchar *title,
                  const gchar *initial_name, gint initial_mode,
                  gint *ai_mode_out,
                  const gchar *initial_emoji, gchar **emoji_out)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK",     GTK_RESPONSE_OK,
        NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

    /* ── Field grid: labelled emoji + name rows ─────────────────────────── */
    GtkWidget *field_grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(field_grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(field_grid), 6);
    gtk_widget_set_margin_start(field_grid, 12);
    gtk_widget_set_margin_end(field_grid, 12);
    gtk_widget_set_margin_top(field_grid, 12);
    gtk_widget_set_margin_bottom(field_grid, 10);

    /* Emoji row */
    GtkWidget *emoji_lbl = gtk_label_new("Emoji (optional):");
    gtk_label_set_xalign(GTK_LABEL(emoji_lbl), 1.0);
    gtk_grid_attach(GTK_GRID(field_grid), emoji_lbl, 0, 0, 1, 1);

    GtkWidget *emoji_entry = gtk_entry_new();
    gtk_entry_set_max_length(GTK_ENTRY(emoji_entry), 4);
    gtk_entry_set_width_chars(GTK_ENTRY(emoji_entry), 3);
    gtk_entry_set_alignment(GTK_ENTRY(emoji_entry), 0.5);
    gtk_widget_set_tooltip_text(emoji_entry,
                                "Optional emoji \xe2\x80\x94 click to pick");
    GtkCssProvider *cprov = gtk_css_provider_new();
    gtk_css_provider_load_from_data(cprov,
                                    "entry { font-size: 18px; }", -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(emoji_entry),
        GTK_STYLE_PROVIDER(cprov),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(cprov);
    if (initial_emoji != NULL && *initial_emoji != '\0')
        gtk_entry_set_text(GTK_ENTRY(emoji_entry), initial_emoji);
    gtk_widget_set_halign(emoji_entry, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(emoji_entry), "on-dialog", dialog);
    g_signal_connect(emoji_entry, "button-press-event",
                     G_CALLBACK(folder_emoji_pressed), dialog);
    gtk_grid_attach(GTK_GRID(field_grid), emoji_entry, 1, 0, 1, 1);

    /* Folder Name row */
    GtkWidget *name_lbl = gtk_label_new("Folder Name:");
    gtk_label_set_xalign(GTK_LABEL(name_lbl), 1.0);
    gtk_grid_attach(GTK_GRID(field_grid), name_lbl, 0, 1, 1, 1);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    if (initial_name != NULL)
        gtk_entry_set_text(GTK_ENTRY(entry), initial_name);
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_grid_attach(GTK_GRID(field_grid), entry, 1, 1, 1, 1);

    /* AI summary mode row (row 2 of field_grid — aligns with labels above)   */
    GtkWidget *mode_lbl = gtk_label_new("AI summary mode:");
    gtk_label_set_xalign(GTK_LABEL(mode_lbl), 1.0);
    gtk_grid_attach(GTK_GRID(field_grid), mode_lbl, 0, 2, 1, 1);

    GtkWidget *radios_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *normal_radio  = gtk_radio_button_new_with_label(NULL, "Normal");
    GtkWidget *project_radio = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(normal_radio), "Project");
    GtkWidget *custom_radio  = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(normal_radio), "Custom");

    if (initial_mode == ON_AI_MODE_PROJECT)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(project_radio), TRUE);
    else if (initial_mode == ON_AI_MODE_CUSTOM)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(custom_radio), TRUE);

    gtk_box_pack_start(GTK_BOX(radios_box), normal_radio,  FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(radios_box), project_radio, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(radios_box), custom_radio,  FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(field_grid), radios_box, 1, 2, 1, 1);

    gtk_box_pack_start(GTK_BOX(box), field_grid, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);

    gchar *result = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        gchar *text =
            g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(entry))));
        if (*text != '\0') {
            result = text;
            if (ai_mode_out != NULL) {
                if (gtk_toggle_button_get_active(
                        GTK_TOGGLE_BUTTON(project_radio)))
                    *ai_mode_out = ON_AI_MODE_PROJECT;
                else if (gtk_toggle_button_get_active(
                        GTK_TOGGLE_BUTTON(custom_radio)))
                    *ai_mode_out = ON_AI_MODE_CUSTOM;
                else
                    *ai_mode_out = ON_AI_MODE_NORMAL;
            }
            if (emoji_out != NULL)
                *emoji_out = g_strstrip(
                    g_strdup(gtk_entry_get_text(GTK_ENTRY(emoji_entry))));
        } else {
            g_free(text);
        }
    }
    gtk_widget_destroy(dialog);
    return result;
}

/* ---------------------------------------------------------------------------
 * confirm() — modal yes/no warning dialog: a 32px warning icon above a
 * centered primary question and a centered secondary line.
 *   lw        — the library window (dialog parent).
 *   primary   — the question (e.g. "Delete this note?").
 *   secondary — supporting line (e.g. "This cannot be undone."); may be
 *               NULL.
 * Returns TRUE if the user accepted.
 * ------------------------------------------------------------------------- */
static gboolean
confirm(OnLibrary *lw, const gchar *primary, const gchar *secondary)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Confirm", GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_No",  GTK_RESPONSE_NO,
        "_Yes", GTK_RESPONSE_YES,
        NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);

    /* Warning icon on top (custom PNG; ⚠ as fallback).                     */
    GtkWidget *icon = on_app_icon_image_sized(lw->app, "warning", 32);
    if (icon == NULL)
        icon = gtk_label_new("\xe2\x9a\xa0");
    gtk_widget_set_halign(icon, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

    GtkWidget *primary_label = gtk_label_new(primary);
    gtk_label_set_justify(GTK_LABEL(primary_label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_halign(primary_label, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(box), primary_label, FALSE, FALSE, 0);

    if (secondary != NULL) {
        GtkWidget *secondary_label = gtk_label_new(secondary);
        gtk_label_set_justify(GTK_LABEL(secondary_label),
                              GTK_JUSTIFY_CENTER);
        gtk_widget_set_halign(secondary_label, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(box), secondary_label, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))),
        box, TRUE, TRUE, 0);

    /* Center the No/Yes buttons under the text, 8px apart.  The action
     * area accessor is deprecated but remains the only way to restyle
     * the button row in GTK3.                                              */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GtkWidget *action_area = gtk_dialog_get_action_area(GTK_DIALOG(dialog));
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_button_box_set_layout(GTK_BUTTON_BOX(action_area),
                              GTK_BUTTONBOX_CENTER);
    gtk_box_set_spacing(GTK_BOX(action_area), 8);

    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return response == GTK_RESPONSE_YES;
}

/* on_new_note() — create a note in the current folder and open it.          */
static void
on_new_note(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gint64 id = on_db_note_create(lw->app->db, current_folder_id(lw));
    if (id != 0) {
        refresh_all(lw);             /* the folder's count just grew        */
        on_editor_window_open(lw->app, id);
    }
}

gint64
on_library_quicknote(OnApp *app)
{
    gint64 id = on_db_note_create(app->db, 0);    /* 0 = the root folder    */
    if (id == 0)
        return 0;

    /* The full notify: a new note changes the root's count as well as the
     * notes pane, and the library may not even be the caller (the CLI's
     * "quicknote" lands here too).                                         */
    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);

    GtkWidget *win = on_editor_window_open(app, id);
    if (win != NULL)
        gtk_window_present(GTK_WINDOW(win));
    return id;
}

/* on_quicknote() — toolbar button: a note in the root folder, whatever is
 * selected, with its editor to the front.                                   */
static void
on_quicknote(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    on_library_quicknote(lw->app);
}

/* on_new_folder() — prompt for a name and AI mode; create under current.    */
static void
on_new_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gint   mode  = ON_AI_MODE_NORMAL;
    gchar *emoji = NULL;
    gchar *name  = prompt_for_folder(lw, "New Folder", NULL,
                                     ON_AI_MODE_NORMAL, &mode,
                                     NULL, &emoji);
    if (name != NULL) {
        on_db_folder_create(lw->app->db, current_folder_id(lw), name,
                            mode, emoji);
        g_free(name);
        refresh_sidebar(lw);
    }
    g_free(emoji);
}

/* ---------------------------------------------------------------------------
 * selected_note_ids() — the ids of every note selected in whichever notes
 * view is active (both views allow multi-selection).
 * Returns a GArray of gint64; free with g_array_free(ids, TRUE).
 * ------------------------------------------------------------------------- */
static GArray *
selected_note_ids(OnLibrary *lw)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    const gchar *mode =              /* "list" or "grid"                    */
        gtk_stack_get_visible_child_name(GTK_STACK(lw->stack));

    GList *paths;                    /* selected row paths                  */
    if (g_strcmp0(mode, "grid") == 0) {
        paths = gtk_icon_view_get_selected_items(lw->notes_grid);
    } else {
        paths = gtk_tree_selection_get_selected_rows(
            gtk_tree_view_get_selection(lw->notes_list), NULL);
    }

    for (GList *l = paths; l != NULL; l = l->next) {
        GtkTreeIter iter;            /* one selected row                    */
        if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                    &iter, l->data)) {
            gint64 id;               /* its note id                         */
            gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                               NL_ID, &id, -1);
            g_array_append_val(ids, id);
        }
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
    return ids;
}

/* ---------------------------------------------------------------------------
 * selected_note_count() — how many notes are selected in the active view.
 * Cheaper than selected_note_ids() when only the count is wanted (the drag
 * icon just needs "one or several").
 * ------------------------------------------------------------------------- */
static guint
selected_note_count(OnLibrary *lw)
{
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "grid") == 0) {
        GList *paths = gtk_icon_view_get_selected_items(lw->notes_grid);
        guint n = g_list_length(paths);
        g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
        return n;
    }
    return (guint)gtk_tree_selection_count_selected_rows(
        gtk_tree_view_get_selection(lw->notes_list));
}

/* ---------------------------------------------------------------------------
 * close_editors_for_ids() — destroy any open editor window for the notes
 * in `ids`; their destroy handlers flush pending autosaves first.
 * ------------------------------------------------------------------------- */
static void
close_editors_for_ids(OnLibrary *lw, const gint64 *ids, gsize n)
{
    for (gsize i = 0; i < n; i++) {
        gint64 note_id = ids[i];
        GtkWidget *editor =
            g_hash_table_lookup(lw->app->editors, &note_id);
        if (editor != NULL)
            gtk_widget_destroy(editor);
    }
}

/* ---------------------------------------------------------------------------
 * trash_notes_core() — move the notes in `ids` to the Trash: close any
 * open editors, flip the trash flag (ONE transaction) and post the status
 * message.  No confirmation: the move is reversible.  Refresh-free — the
 * callers refresh on their own schedule (the drag path must finish the
 * DnD handshake first).
 *   lw  — the library window.
 *   ids — the note ids.
 *   n   — how many.
 * Returns TRUE when the database move succeeded.
 * ------------------------------------------------------------------------- */
static gboolean
trash_notes_core(OnLibrary *lw, const gint64 *ids, guint n)
{
    close_editors_for_ids(lw, ids, n);
    if (!on_db_notes_trash(lw->app->db, ids, n))
        return FALSE;
    on_app_status(lw->app, "Moved %u note%s to Trash",
                  n, n == 1 ? "" : "s");
    return TRUE;
}

/* trash_notes() — action/menu path of the note-trash gesture: the shared
 * core plus the immediate refresh.                                          */
static void
trash_notes(OnLibrary *lw, GArray *ids)
{
    if (ids->len == 0)
        return;
    if (trash_notes_core(lw, (const gint64 *)ids->data, ids->len))
        refresh_all(lw);             /* counts + the Trash section          */
}

/* ---------------------------------------------------------------------------
 * trash_folder() — move folder `folder_id` (with its whole subtree) to
 * the Trash: close any editors open on its notes, flip the trash flag,
 * post the status message, and pull the selection off the now-hidden
 * folder.  Refresh-free, like trash_notes_core above.
 *   lw        — the library window.
 *   folder_id — the folder to trash.
 *   name      — its bare name (for the status message).
 * Returns TRUE when the database move succeeded.
 * ------------------------------------------------------------------------- */
static gboolean
trash_folder(OnLibrary *lw, gint64 folder_id, const gchar *name)
{
    GArray *nids = on_db_folder_note_ids(lw->app->db, folder_id);
    close_editors_for_ids(lw, (const gint64 *)nids->data, nids->len);
    g_array_free(nids, TRUE);
    if (!on_db_folder_trash(lw->app->db, folder_id))
        return FALSE;
    on_app_status(lw->app,
                  "Folder \xe2\x80\x9c%s\xe2\x80\x9d moved to Trash", name);
    if (lw->sel_kind == SB_KIND_FOLDER && lw->sel_id == folder_id) {
        lw->sel_kind = SB_KIND_ROOT;
        lw->sel_id   = 0;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * delete_notes_permanently() — confirm once, then permanently delete every
 * note in `ids` (closing any open editors first).  This is the Trash-view
 * delete; normal views go through trash_notes().
 * ------------------------------------------------------------------------- */
static void
delete_notes_permanently(OnLibrary *lw, GArray *ids)
{
    if (ids->len == 0)
        return;
    gchar *question = (ids->len == 1)
        ? g_strdup("Permanently delete this note?")
        : g_strdup_printf("Permanently delete these %u notes?", ids->len);
    gboolean ok = confirm(lw, question, "This cannot be undone.");
    g_free(question);
    if (!ok)
        return;

    const gint64 *note_ids = (const gint64 *)ids->data;
    close_editors_for_ids(lw, note_ids, ids->len);
    on_db_notes_delete(lw->app->db, note_ids, ids->len);
    /* Evict deleted entries from the thumbnail cache so their surfaces
     * are freed now rather than held until the window closes.             */
    for (gsize i = 0; i < ids->len; i++)
        g_hash_table_remove(lw->thumb_cache, &note_ids[i]);
    refresh_all(lw);                 /* tag list/counts may have changed    */
}

/* on_delete_note() — action-bar Delete: trash every selected note, or
 * permanently delete it when the Trash is what's being viewed.              */
static void
on_delete_note(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    GArray *ids = selected_note_ids(lw);
    if (in_trash_view(lw))
        delete_notes_permanently(lw, ids);
    else
        trash_notes(lw, ids);
    g_array_free(ids, TRUE);
}

/* on_delete_folder() — sidebar-toolbar Delete: move the selected folder
 * (with its whole subtree) to the Trash; a folder already in the Trash is
 * permanently deleted instead (after confirmation).                         */
static void
on_delete_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind == SB_KIND_FOLDER) {
        if (trash_folder(lw, lw->sel_id, lw->sel_name))
            refresh_all(lw);
    } else if (lw->sel_kind == SB_KIND_TRASH_FOLDER) {
        if (confirm(lw,
                    "Permanently delete this folder and everything "
                    "inside it?",
                    "This cannot be undone.")) {
            GArray *ids = on_db_folder_note_ids(lw->app->db, lw->sel_id);
            close_editors_for_ids(lw, (const gint64 *)ids->data, ids->len);
            g_array_free(ids, TRUE);
            on_db_folder_delete(lw->app->db, lw->sel_id);
            lw->sel_kind = SB_KIND_TRASH;
            lw->sel_id   = 0;
            refresh_all(lw);
        }
    }
}

/* on_restore_folder() — Trash context menu: put the selected trashed
 * folder (and its subtree) back where it was deleted from; the selection
 * follows it to its restored spot.                                          */
static void
on_restore_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind != SB_KIND_TRASH_FOLDER)
        return;
    if (on_db_folder_restore(lw->app->db, lw->sel_id)) {
        on_app_status(lw->app,
                      "Folder \xe2\x80\x9c%s\xe2\x80\x9d restored",
                      lw->sel_name);
        lw->sel_kind = SB_KIND_FOLDER;   /* same id, back in the tree       */
        refresh_all(lw);
    }
}

/* on_empty_trash() — Trash context menu: permanently delete everything in
 * the Trash after one confirmation.                                         */
static void
on_empty_trash(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (!confirm(lw, "Permanently delete everything in the Trash?",
                 "This cannot be undone."))
        return;

    /* Close editors for every note the purge will take with it —
     * including notes inside trashed folder subtrees.  Evict from the
     * thumbnail cache at the same time so the surfaces are freed now.     */
    GArray *ids = on_db_trash_note_ids(lw->app->db);
    const gint64 *note_ids = (const gint64 *)ids->data;
    close_editors_for_ids(lw, note_ids, ids->len);
    for (gsize i = 0; i < ids->len; i++)
        g_hash_table_remove(lw->thumb_cache, &note_ids[i]);
    g_array_free(ids, TRUE);

    if (on_db_trash_empty(lw->app->db)) {
        on_app_status(lw->app, "Trash emptied");
        refresh_all(lw);             /* the Trash section disappears        */
    }
}

/* on_rename_folder() — "Info…" menu: edit folder name and AI mode.          */
static void
on_rename_folder(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind != SB_KIND_FOLDER)
        return;
    gint   cur_mode  =               /* pre-fill radios with current setting */
        on_db_folder_get_ai_mode(lw->app->db, lw->sel_id);
    gchar *cur_emoji =               /* pre-fill emoji with current value    */
        on_db_folder_get_emoji(lw->app->db, lw->sel_id);
    gint   mode  = cur_mode;
    gchar *emoji = NULL;
    gchar *name  = prompt_for_folder(lw, "Folder Info",
                                     lw->sel_name, cur_mode, &mode,
                                     cur_emoji, &emoji);
    g_free(cur_emoji);
    if (name != NULL) {
        on_db_folder_update(lw->app->db, lw->sel_id, name, mode, emoji);
        g_free(name);
        refresh_sidebar(lw);
    }
    g_free(emoji);
}

/* on_open_search() — sidebar-toolbar Search: open the search window (it
 * reads the live library selection each time Search is pressed).  The
 * scope radio defaults to the selection when it is narrower than "all" —
 * a folder or tag; the All Notes/Trash/Pinned sections have no folder
 * scope, so they default to searching everything.                           */
static void
on_open_search(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gboolean scoped = lw->sel_kind == SB_KIND_FOLDER ||
                      lw->sel_kind == SB_KIND_TAG ||
                      lw->sel_kind == SB_KIND_TRASH_FOLDER;
    on_search_window_open(lw->app, scoped);
}

/* ---------------------------------------------------------------------------
 * on_open_media() — toolbar Media: open the media browser over EXACTLY the
 * notes the notes pane is currently listing (notes_for_selection), so
 * "the images in this folder" means the same thing the pane does.  The
 * window takes a snapshot, so the list is fetched once here.
 * ------------------------------------------------------------------------- */
static void
on_open_media(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */

    /* The Action Items view is not a place, and its media set is every
     * note's — say so rather than naming a row that isn't a folder.        */
    const gchar *label = (lw->sel_kind == SB_KIND_ACTIONS)
        ? "All Notes"
        : (lw->sel_name != NULL ? lw->sel_name : "Notes");

    GList *notes = notes_for_selection(lw);
    on_media_window_open(lw->app, label, notes);
    on_db_note_list_free(notes);
}

/* ---------------------------------------------------------------------------
 * on_toolbar_search_activate() — Enter in the toolbar's search entry: open
 * a search window already loaded with the typed query and run it against
 * All Notes, case insensitively.  The text is left in the entry so the same
 * query can be fired again.
 *   entry     — the GtkSearchEntry that was activated.
 *   user_data — the owning library window.
 * ------------------------------------------------------------------------- */
static void
on_toolbar_search_activate(GtkEntry *entry, gpointer user_data)
{
    OnLibrary *lw = user_data;        /* owning library window               */
    const gchar *query = gtk_entry_get_text(entry);
    if (query == NULL || *query == '\0')
        return;                      /* nothing typed: no window            */
    on_search_window_open_query(lw->app, query);
}

/* on_open_settings() — File → Settings…                                     */
static void
on_open_settings(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    on_settings_window_open(lw->app);
}

/* ---------------------------------------------------------------------------
 * on_open_db() — File → Open Database…: let the user pick any .db file and
 * open it, either as the new permanent default or for this session only.
 * ------------------------------------------------------------------------- */
static void
on_open_db(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;
    OnApp     *app = lw->app;

    /* Step 1: pick the file. */
    gchar *file_path = on_app_pick_path(
        GTK_WINDOW(lw->window), "Open Database",
        GTK_FILE_CHOOSER_ACTION_OPEN, "_Open",
        "SQLite Database (*.db)", "*.db");
    if (file_path == NULL)
        return;

    /* Already open: nothing to do. */
    if (g_strcmp0(file_path, app->db->path) == 0) {
        g_free(file_path);
        return;
    }

    /* Step 2: ask how to open it. */
    gchar *display = g_path_get_basename(file_path);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Open “%s” as your new default database, or for this "
        "session only?", display);
    g_free(display);
    gtk_window_set_title(GTK_WINDOW(dlg), "Notes - Open Database");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "_Cancel",         GTK_RESPONSE_CANCEL,
        "_Session Only",   1,
        "Set as _Default", 2,
        NULL);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
        g_free(file_path);
        return;
    }
    gboolean set_default = (resp == 2);

    /* Step 3: switch to the chosen database. */
    on_app_close_all_editors(app);
    gchar *old_path = g_strdup(app->db->path);
    on_db_close(app->db);
    app->db = on_db_open(file_path);

    if (app->db == NULL) {
        on_app_notice(GTK_WINDOW(lw->window), GTK_MESSAGE_ERROR,
                      "Notes - Database Error",
                      "Could not open:\n%s", file_path);
        /* Revert to old database. */
        app->db = on_db_open(old_path);
        g_free(old_path);
        g_free(file_path);
        return;
    }

    if (set_default) {
        gchar *new_dir = g_path_get_dirname(file_path);
        g_free(app->db_dir);
        app->db_dir = g_strdup(new_dir);
        app->db_transient = FALSE;
        on_app_config_set("db_dir",  new_dir);
        g_free(new_dir);
    } else {
        app->db_transient = TRUE;   /* session only: don't persist anything */
    }

    g_free(old_path);
    g_free(file_path);

    if (app->notify_notes_changed != NULL)
        app->notify_notes_changed(app);
    on_app_status(app, "DB at %s loaded", app->db->path);
}

/* find_gtk_image() — first GtkImage in a widget subtree (depth-first).
 * Used to reach GtkAboutDialog's internal logo image, which the public
 * API only feeds with a plain (blurry-on-Retina) GdkPixbuf.                 */
static GtkWidget *
find_gtk_image(GtkWidget *widget)
{
    if (GTK_IS_IMAGE(widget))
        return widget;
    GtkWidget *hit = NULL;           /* first image found in the subtree    */
    if (GTK_IS_CONTAINER(widget)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = kids; l != NULL && hit == NULL; l = l->next)
            hit = find_gtk_image(l->data);
        g_list_free(kids);
    }
    return hit;
}

/* ---------------------------------------------------------------------------
 * on_about() — File → About: the standard about dialog with the app icon,
 * author, build date and a link to the BSD license.
 * ------------------------------------------------------------------------- */
static void
on_about(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */

    /* 128x128-logical logo from composition.png, decoded at the display's
     * scale factor so it stays sharp on Retina (quirk #5).                 */
    gint sf = gtk_widget_get_scale_factor(lw->window);
    gchar *icon_path = g_build_filename(lw->app->icons_dir, "composition.png",
                                        NULL);
    GdkPixbuf *logo = gdk_pixbuf_new_from_file_at_size(icon_path,
                                                       128 * sf, 128 * sf,
                                                       NULL);
    g_free(icon_path);

    const gchar *authors[] = { "Ian Campbell", "Claude Sonnet 4.5", "And thanks to Blue Note Records. Both for the great double entendre and for amazing music that shaped my youth.", NULL };

    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(lw->window));
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog),
                                      "Notes");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), ON_VERSION);
    if (logo != NULL) {
        /* set_logo() first (it makes the internal image visible and
         * sized), then swap that image's content for a cairo surface
         * with the device scale — the pixbuf API renders 1 buffer px
         * per logical px and looks soft on HiDPI.                          */
        GdkPixbuf *at_128 = (sf > 1)
            ? gdk_pixbuf_scale_simple(logo, 128, 128, GDK_INTERP_BILINEAR)
            : g_object_ref(logo);
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), at_128);
        g_object_unref(at_128);

        if (sf > 1) {
            GtkWidget *img = find_gtk_image(
                gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
            if (img != NULL) {
                cairo_surface_t *surface =
                    gdk_cairo_surface_create_from_pixbuf(logo, sf, NULL);
                gtk_image_set_from_surface(GTK_IMAGE(img), surface);
                cairo_surface_destroy(surface);
            }
        }
        g_object_unref(logo);
    }
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

    /* Database vitals: entry counts, location, on-disk size.               */
    gint n_notes, n_folders, n_tags;     /* totals across the database      */
    on_db_totals(lw->app->db, &n_notes, &n_folders, &n_tags);
    GStatBuf st;                     /* for the database file size          */
    gchar *size_str = (g_stat(lw->app->db->path, &st) == 0)
                      ? g_format_size((guint64)st.st_size)
                      : g_strdup("unknown");

    /* __DATE__/__TIME__ expand when this file is compiled — the closest
     * portable thing to a "last compiled" stamp.                           */
    gchar *comments = g_strdup_printf(
        "Compiled " __DATE__ " " __TIME__ "\n\n"
        "Database: %s\n"
        "%d notes in %d folders, %d tags \xe2\x80\x94 %s on disk",
        lw->app->db->path, n_notes, n_folders, n_tags, size_str);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), comments);
    g_free(comments);
    g_free(size_str);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_BSD);
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                                 "https://opensource.org/license/bsd-3-clause");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                       "BSD License");

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ---------------------------------------------------------------------------
 * on_quit() — File → Quit: destroy every application window.  Editor
 * windows flush their final autosave from their destroy handlers, and the
 * GTK main loop ends once the last window is gone.
 * ------------------------------------------------------------------------- */
static void
on_quit(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    GList *windows =                 /* copy: destroying mutates the list   */
        g_list_copy(gtk_application_get_windows(lw->app->gtk_app));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(windows);
}

/* on_sidebar_search_here() — sidebar context menu Search: the clicked row
 * is already selected, so a scoped search targets it directly.              */
static void
on_sidebar_search_here(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    on_search_window_open(lw->app, TRUE);
}

/* utf8_casecmp() — case-insensitive UTF-8 string comparison (casefold
 * both sides; NULL compares as the empty string).                           */
static gint
utf8_casecmp(const gchar *a, const gchar *b)
{
    gchar *ca = g_utf8_casefold(a != NULL ? a : "", -1);
    gchar *cb = g_utf8_casefold(b != NULL ? b : "", -1);
    gint result = g_strcmp0(ca, cb);
    g_free(ca);
    g_free(cb);
    return result;
}

/* ---------------------------------------------------------------------------
 * on_sort_subfolders() — folder context menu: order the selected
 * folder's (or the root's) DIRECT children alphabetically and persist
 * that as their sort_order.  One level only — each folder's own order
 * stays whatever the user made it.
 * ------------------------------------------------------------------------- */
static void
on_sort_subfolders(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->sel_kind != SB_KIND_FOLDER && lw->sel_kind != SB_KIND_ROOT)
        return;
    gint64 parent =                  /* whose children get sorted           */
        (lw->sel_kind == SB_KIND_FOLDER) ? lw->sel_id : 0;

    gint n = on_db_folder_sort_children(lw->app->db, parent);
    if (n > 1) {
        on_app_status(lw->app, "Sorted %d subfolders alphabetically", n);
        refresh_all(lw);
    }
}

/* ---------------------------------------------------------------------------
 * menu_new_transient() — one-shot context-menu scaffold: a fresh GtkMenu
 * attached to the library window that destroys itself once its selection
 * is done.
 * ------------------------------------------------------------------------- */
static GtkWidget *
menu_new_transient(OnLibrary *lw)
{
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), lw->window, NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);
    return menu;
}

/* menu_popup() — show a fully-built menu at the pointer of `event`.         */
static void
menu_popup(GtkWidget *menu, GdkEventButton *event)
{
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

/* ---------------------------------------------------------------------------
 * on_sidebar_button_press() — right click in the folder/tag tree: select
 * the row under the pointer and show a context menu mirroring the sidebar
 * toolbar (folder actions + scoped search).
 * ------------------------------------------------------------------------- */
static gboolean
on_sidebar_button_press(GtkWidget *widget, GdkEventButton *event,
                        gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkTreePath *path = NULL;        /* row under the pointer               */
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                       (gint)event->x, (gint)event->y,
                                       &path, NULL, NULL, NULL))
        return FALSE;

    gtk_tree_selection_select_path(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(widget)), path);

    /* What kind of row was clicked decides which actions make sense.       */
    GtkTreeIter iter;                /* the clicked row                     */
    gint kind = SB_KIND_TAGS_HEADER; /* default: nothing to offer           */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->sidebar_store),
                                &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->sidebar_store), &iter,
                           SB_KIND, &kind, -1);
    gtk_tree_path_free(path);
    if (kind == SB_KIND_TAGS_HEADER)
        return TRUE;                 /* consumed, but no menu               */

    GtkWidget *menu = menu_new_transient(lw);

    if (kind == SB_KIND_TRASH) {
        /* The Trash row's only action: purge it.                           */
        add_menu_item(menu, "_Empty Trash\xe2\x80\xa6",
                      G_CALLBACK(on_empty_trash), lw);
        menu_popup(menu, event);
        return TRUE;
    }

    if (kind == SB_KIND_TRASH_FOLDER) {
        add_menu_item(menu, "_Restore Folder",
                      G_CALLBACK(on_restore_folder), lw);
        add_menu_item(menu, "Delete _Permanently",
                      G_CALLBACK(on_delete_folder), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
    } else if (kind == SB_KIND_FOLDER || kind == SB_KIND_ROOT) {
        add_menu_item(menu,
                      kind == SB_KIND_FOLDER ? "New _Subfolder\xe2\x80\xa6"
                                             : "New _Folder\xe2\x80\xa6",
                      G_CALLBACK(on_new_folder), lw);
        if (kind == SB_KIND_FOLDER) {
            add_menu_item(menu, "Info\xe2\x80\xa6",
                          G_CALLBACK(on_rename_folder), lw);
            add_menu_item(menu, "Move to _Trash",
                          G_CALLBACK(on_delete_folder), lw);
        }
        add_menu_item(menu, "Sort Subfolders _Alphabetically",
                      G_CALLBACK(on_sort_subfolders), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
    }
    add_menu_item(menu, "Search _Here\xe2\x80\xa6",
                  G_CALLBACK(on_sidebar_search_here), lw);

    menu_popup(menu, event);
    return TRUE;
}

/* ===========================================================================
 * view mode & export
 * =========================================================================== */

/* on_view_list() / on_view_grid() — flip the stack between the modes.
 * Both record the choice in grid_pref so leaving the Action Items view
 * restores it; while that view is showing they only set the preference
 * (refresh_notes keeps the actions child on top for that selection).       */
static void
on_view_list(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    lw->grid_pref = FALSE;
    if (lw->sel_kind == SB_KIND_ACTIONS)
        return;
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "list");
    if (lw->list_autofit)
        refresh_notes(lw);           /* re-measure the widths grid skipped  */
}

static void
on_view_grid(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    lw->grid_pref = TRUE;
    if (lw->sel_kind == SB_KIND_ACTIONS)
        return;
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "grid");
    refresh_notes(lw);               /* fill the thumbnails list mode skips */
}

/* on_toggle_sidebar() — toolbar show/hide button for the folder/tag
 * pane: the notes view takes the whole window while it is hidden.           */
static void
on_toggle_sidebar(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    gtk_widget_set_visible(lw->sidebar_box,
                           !gtk_widget_get_visible(lw->sidebar_box));
}

/* on_toggle_view() — toolbar List/Grid button: switch to whichever notes
 * view is not currently showing.                                            */
static void
on_toggle_view(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    const gchar *mode =              /* "list" or "grid"                    */
        gtk_stack_get_visible_child_name(GTK_STACK(lw->stack));
    if (g_strcmp0(mode, "list") == 0)
        on_view_grid(NULL, lw);
    else
        on_view_list(NULL, lw);
}

/* pick_export_dir() — run the "Choose Export Folder" chooser shared by
 * every export flow.  Returns the chosen directory (g_free), or NULL if
 * the user cancelled.                                                       */
static gchar *
pick_export_dir(OnLibrary *lw)
{
    return on_app_pick_path(GTK_WINDOW(lw->window), "Choose Export Folder",
                            GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                            "_Export", NULL, NULL);
}

/* ---------------------------------------------------------------------------
 * report_exported() — modal result dialog for an export run.
 *   lw  — the library window (dialog parent).
 *   n   — how many notes were written; negative means the run failed.
 *   dir — the destination directory (for the success message).
 *   err — exporter error message for a failed run, or NULL.
 * ------------------------------------------------------------------------- */
static void
report_exported(OnLibrary *lw, gint n, const gchar *dir, const gchar *err)
{
    if (n >= 0)
        on_app_notice(GTK_WINDOW(lw->window), GTK_MESSAGE_INFO, NULL,
                      "Exported %d note%s to\n%s",
                      n, n == 1 ? "" : "s", dir);
    else
        on_app_notice(GTK_WINDOW(lw->window), GTK_MESSAGE_ERROR, NULL,
                      "Export failed: %s",
                      err != NULL ? err : "unknown error");
}

/* ---------------------------------------------------------------------------
 * run_export() — pick a destination directory and export every note in
 * the requested format, then report the result.
 *   lw     — the library window.
 *   format — ON_EXPORT_HTML or ON_EXPORT_MARKDOWN.
 * ------------------------------------------------------------------------- */
static void
run_export(OnLibrary *lw, OnExportFormat format)
{
    gchar *dir = pick_export_dir(lw);
    if (dir == NULL)
        return;

    gchar *err = NULL;               /* exporter error message              */
    gint   n   = on_export_all(lw->app, dir, format, &err);
    report_exported(lw, n, dir, err);
    g_free(err);
    g_free(dir);
}

/* on_export_html() / on_export_markdown() — File-menu export entries.       */
static void
on_export_html(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    run_export((OnLibrary *)user_data, ON_EXPORT_HTML);
}

static void
on_export_markdown(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    run_export((OnLibrary *)user_data, ON_EXPORT_MARKDOWN);
}

/* ===========================================================================
 * per-note context menu (right click in list or grid)
 * =========================================================================== */

/* Context-menu item handlers.  Delete and export act on the whole
 * selection; Open uses the note id stashed on the item as boxed object
 * data "on-note-id".                                                        */
static void
on_note_ctx_open(GtkMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gint64 *id = g_object_get_data(G_OBJECT(item), "on-note-id");
    if (id != NULL)
        on_editor_window_open(lw->app, *id);
}

/* ctx_export_selection() — export every selected note to one directory.     */
static void
ctx_export_selection(OnLibrary *lw, OnExportFormat format)
{
    GArray *ids = selected_note_ids(lw);
    if (ids->len == 0) {
        g_array_free(ids, TRUE);
        return;
    }

    gchar *dir = pick_export_dir(lw);
    if (dir == NULL) {
        g_array_free(ids, TRUE);
        return;
    }

    gint exported = 0;               /* how many notes were written         */
    for (guint i = 0; i < ids->len; i++)
        if (on_export_note(lw->app, g_array_index(ids, gint64, i),
                           dir, format))
            exported++;

    report_exported(lw, exported, dir, NULL);
    g_free(dir);
    g_array_free(ids, TRUE);
}

static void
on_note_ctx_export_html(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    ctx_export_selection((OnLibrary *)user_data, ON_EXPORT_HTML);
}

static void
on_note_ctx_export_md(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    ctx_export_selection((OnLibrary *)user_data, ON_EXPORT_MARKDOWN);
}

static void
on_note_ctx_delete(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    OnLibrary *lw = user_data;       /* owning library window               */
    GArray *ids = selected_note_ids(lw);
    if (in_trash_view(lw))
        delete_notes_permanently(lw, ids);
    else
        trash_notes(lw, ids);
    g_array_free(ids, TRUE);
}

/* on_note_ctx_restore() — Trash-view context menu: put every selected
 * note back where it was deleted from (top level when that folder is
 * itself still in the Trash).                                               */
static void
on_note_ctx_restore(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    OnLibrary *lw = user_data;       /* owning library window               */
    GArray *ids = selected_note_ids(lw);
    if (ids->len > 0) {
        for (guint i = 0; i < ids->len; i++)
            on_db_note_restore(lw->app->db, g_array_index(ids, gint64, i));
        on_app_status(lw->app, "Restored %u note%s",
                      ids->len, ids->len == 1 ? "" : "s");
        refresh_all(lw);             /* counts + the Trash section          */
    }
    g_array_free(ids, TRUE);
}

/* on_note_ctx_toggle_pin() — pin or unpin every selected note (the new
 * state is the opposite of the clicked note's current state).               */
static void
on_note_ctx_toggle_pin(GtkMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    gboolean pin =                   /* target state for the selection      */
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "on-pin"));

    GArray *ids = selected_note_ids(lw);
    for (guint i = 0; i < ids->len; i++)
        on_db_note_set_pinned(lw->app->db,
                              g_array_index(ids, gint64, i), pin);
    g_array_free(ids, TRUE);

    refresh_all(lw);                 /* the Pinned Notes count/section, and
                                        the pinned view may be showing      */
}

/* ---------------------------------------------------------------------------
 * show_note_context_menu() — build and pop up the per-note menu.
 *   lw      — the library window.
 *   note_id — the note that was right-clicked.
 *   event   — the triggering button event (for popup placement).
 * ------------------------------------------------------------------------- */
static void
show_note_context_menu(OnLibrary *lw, gint64 note_id, GdkEventButton *event)
{
    GtkWidget *menu = menu_new_transient(lw);

    /* Pin/Unpin reflects the clicked note's current state.                 */
    OnNoteMeta *meta = on_db_note_get(lw->app->db, note_id);
    gboolean pinned = meta != NULL && meta->pinned;
    on_db_note_meta_free(meta);

    /* Two menus: the normal one, and the Trash view's restore/purge one.   */
    typedef struct { const gchar *label; GCallback cb; } NoteMenuItem;
    NoteMenuItem normal_items[] = {
        { "_Open",                          G_CALLBACK(on_note_ctx_open) },
        { pinned ? "Un_pin" : "_Pin",       G_CALLBACK(on_note_ctx_toggle_pin) },
        { NULL,                             NULL /* separator */          },
        { "Export as _HTML\xe2\x80\xa6",    G_CALLBACK(on_note_ctx_export_html) },
        { "Export as _Markdown\xe2\x80\xa6",G_CALLBACK(on_note_ctx_export_md)   },
        { NULL,                             NULL /* separator */          },
        { "Move to _Trash",                 G_CALLBACK(on_note_ctx_delete) },
    };
    NoteMenuItem trash_items[] = {
        { "_Open",                          G_CALLBACK(on_note_ctx_open) },
        { "_Restore",                       G_CALLBACK(on_note_ctx_restore) },
        { NULL,                             NULL /* separator */          },
        { "Delete _Permanently",            G_CALLBACK(on_note_ctx_delete) },
    };
    NoteMenuItem *items = in_trash_view(lw) ? trash_items : normal_items;
    gsize n_items = in_trash_view(lw) ? G_N_ELEMENTS(trash_items)
                                      : G_N_ELEMENTS(normal_items);
    for (gsize i = 0; i < n_items; i++) {
        GtkWidget *mi;               /* menu item (or separator)            */
        if (items[i].label == NULL) {
            mi = gtk_separator_menu_item_new();
        } else {
            mi = gtk_menu_item_new_with_mnemonic(items[i].label);
            gint64 *boxed = g_new(gint64, 1);
            *boxed = note_id;
            g_object_set_data_full(G_OBJECT(mi), "on-note-id",
                                   boxed, g_free);
            if (items[i].cb == G_CALLBACK(on_note_ctx_toggle_pin))
                g_object_set_data(G_OBJECT(mi), "on-pin",
                                  GINT_TO_POINTER(!pinned));
            g_signal_connect(mi, "activate", items[i].cb, lw);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    menu_popup(menu, event);
}

/* ---------------------------------------------------------------------------
 * notes_ctx_popup() — shared right-click tail of both notes views: read
 * the note id at `path` (owned — freed here) and pop up the note context
 * menu for it.  Returns TRUE: the click is consumed either way.
 * ------------------------------------------------------------------------- */
static gboolean
notes_ctx_popup(OnLibrary *lw, GtkTreePath *path, GdkEventButton *event)
{
    GtkTreeIter iter;                /* the clicked row                     */
    gint64 id = 0;                   /* its note id                         */
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(lw->notes_store),
                                &iter, path))
        gtk_tree_model_get(GTK_TREE_MODEL(lw->notes_store), &iter,
                           NL_ID, &id, -1);
    gtk_tree_path_free(path);

    if (id != 0)
        show_note_context_menu(lw, id, event);
    return TRUE;
}

/* notes_sel_block_func() / notes_sel_allow_func() — temporary select
 * functions for the span of a press on an already-selected list row:
 * block vetoes EVERY selection change, allow puts normality back.          */
static gboolean
notes_sel_block_func(GtkTreeSelection *sel, GtkTreeModel *model,
                     GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)sel; (void)model; (void)path; (void)selected; (void)data;
    return FALSE;
}

static gboolean
notes_sel_allow_func(GtkTreeSelection *sel, GtkTreeModel *model,
                     GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)sel; (void)model; (void)path; (void)selected; (void)data;
    return TRUE;
}

/* notes_sel_unblock() — end a blocked press: selection changes work
 * again; the press path is optionally handed to the caller (transfer),
 * otherwise freed.                                                          */
static GtkTreePath *
notes_sel_unblock(OnLibrary *lw, gboolean want_path)
{
    if (!lw->notes_sel_blocked)
        return NULL;
    gtk_tree_selection_set_select_function(
        gtk_tree_view_get_selection(lw->notes_list),
        notes_sel_allow_func, NULL, NULL);
    lw->notes_sel_blocked = FALSE;
    GtkTreePath *path = lw->notes_press_path;
    lw->notes_press_path = NULL;
    if (!want_path) {
        gtk_tree_path_free(path);
        path = NULL;
    }
    return path;
}

/* ---------------------------------------------------------------------------
 * on_notes_list_button_press() — two jobs:
 *
 * 1. Left press on an already-selected row of a multi-selection: GTK
 *    3.24's tree view CLEAR_AND_SELECTs on PRESS with no deferral for a
 *    possible drag (quirk #15), which collapsed the selection before a
 *    multi-note drag could start.  Install a selection veto for the span
 *    of the press; on_notes_drag_begin keeps the selection, a plain
 *    release applies the collapse GTK wanted.  The event itself is NOT
 *    consumed — the view's drag gesture must still see it.
 *
 * 2. Right click: select the row under the pointer and show the note
 *    menu (an existing multi-selection is kept when clicked inside).
 * ------------------------------------------------------------------------- */
static gboolean
on_notes_list_button_press(GtkWidget *widget, GdkEventButton *event,
                           gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */

    if (event->button == GDK_BUTTON_PRIMARY &&
        event->type == GDK_BUTTON_PRESS &&
        !(event->state & gtk_accelerator_get_default_mod_mask())) {
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        GtkTreePath *path = NULL;    /* row under the pointer               */
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                          (gint)event->x, (gint)event->y,
                                          &path, NULL, NULL, NULL)) {
            if (gtk_tree_selection_path_is_selected(sel, path) &&
                gtk_tree_selection_count_selected_rows(sel) > 1) {
                gtk_tree_selection_set_select_function(
                    sel, notes_sel_block_func, NULL, NULL);
                lw->notes_sel_blocked = TRUE;
                gtk_tree_path_free(lw->notes_press_path);
                lw->notes_press_path = path;     /* ownership taken         */
                return FALSE;
            }
            gtk_tree_path_free(path);
        }
        return FALSE;
    }

    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkTreePath *path = NULL;        /* row under the pointer               */
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                       (gint)event->x, (gint)event->y,
                                       &path, NULL, NULL, NULL))
        return FALSE;

    /* Right-clicking inside an existing multi-selection keeps it (so bulk
     * actions can target it); clicking elsewhere selects just that row.    */
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }

    return notes_ctx_popup(lw, path, event);
}

/* on_notes_list_button_release() — a blocked press ended WITHOUT a drag:
 * lift the veto and apply the collapse GTK wanted on press (a plain
 * click on a selected row means "select just this one").                    */
static gboolean
on_notes_list_button_release(GtkWidget *widget, GdkEventButton *event,
                             gpointer user_data)
{
    (void)event;
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreePath *path = notes_sel_unblock(lw, TRUE);
    if (path != NULL) {
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, NULL, FALSE);
        gtk_tree_path_free(path);
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * on_notes_grid_button_press() — right click in grid mode: same as above
 * for the icon view.
 * ------------------------------------------------------------------------- */
static gboolean
on_notes_grid_button_press(GtkWidget *widget, GdkEventButton *event,
                           gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;

    GtkTreePath *path = gtk_icon_view_get_path_at_pos(
        GTK_ICON_VIEW(widget), (gint)event->x, (gint)event->y);
    if (path == NULL)
        return FALSE;

    /* Keep an existing multi-selection when right-clicking inside it.      */
    if (!gtk_icon_view_path_is_selected(GTK_ICON_VIEW(widget), path)) {
        gtk_icon_view_unselect_all(GTK_ICON_VIEW(widget));
        gtk_icon_view_select_path(GTK_ICON_VIEW(widget), path);
    }

    return notes_ctx_popup(lw, path, event);
}

/* ===========================================================================
 * status bar
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * status_path_update() — put the current selection's location in the
 * status bar's left label: "/" for the root, "/Folder/Sub" for folders,
 * and the raw sidebar name ("#tag", "Pinned Notes") for the non-path
 * views.  Runs from refresh_notes(), so it tracks every navigation.
 * ------------------------------------------------------------------------- */
static void
status_path_update(OnLibrary *lw)
{
    if (lw->status_path == NULL)
        return;

    gchar *text;                     /* the location part                   */
    if (lw->sel_kind == SB_KIND_TAG || lw->sel_kind == SB_KIND_PINNED ||
        lw->sel_kind == SB_KIND_ALL || lw->sel_kind == SB_KIND_TRASH ||
        lw->sel_kind == SB_KIND_ACTIONS) {
        text = g_strdup(lw->sel_name != NULL ? lw->sel_name : "");
    } else if (lw->sel_kind == SB_KIND_TRASH_FOLDER) {
        text = g_strdup_printf("Trash/%s",
                               lw->sel_name != NULL ? lw->sel_name : "");
    } else {
        gchar *path = on_db_folder_path(lw->app->db, lw->sel_id);
        text = g_strdup_printf("/%s", path);
        g_free(path);
    }

    gchar *full = on_app_location_text(lw->app, text);
    gtk_label_set_text(GTK_LABEL(lw->status_path), full);
    g_free(full);
    g_free(text);
}

/* on_notes_selection_status() — selection changed in either notes view:
 * post "N files selected" as a transient event message (right label,
 * fades like any other).  Nothing is posted when the selection empties,
 * and refresh_notes() repopulation (which clears the selection row by
 * row) is skipped via the populating guard.                                 */
static void
on_notes_selection_status(gpointer view_or_selection, gpointer user_data)
{
    (void)view_or_selection;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (lw->populating != 0)
        return;
    GArray *sel = selected_note_ids(lw);
    if (sel->len > 0)
        on_app_status(lw->app, "%u file%s selected",
                      sel->len, sel->len == 1 ? "" : "s");
    g_array_free(sel, TRUE);
}

/* status_fade_timeout() — the display period ended: fade the event
 * message out (the revealer crossfades to nothing).                         */
static gboolean
status_fade_timeout(gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    lw->status_timeout = 0;
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_revealer),
                                  FALSE);
    return G_SOURCE_REMOVE;
}

/* lw_from_app() — the library state stashed on the library window, or
 * NULL when the window (or the state on it) doesn't exist.                  */
static OnLibrary *
lw_from_app(OnApp *app)
{
    return (app->library_window != NULL)
        ? g_object_get_data(G_OBJECT(app->library_window), "on-library")
        : NULL;
}

/* ---------------------------------------------------------------------------
 * library_notify_status() — installed as app->notify_status; shows an
 * event message in the status bar's right label, fading it out after
 * STATUS_FADE_SECONDS (each new message restarts the clock).  Post
 * through on_app_status(), never directly.
 * ------------------------------------------------------------------------- */
static void
library_notify_status(OnApp *app, const gchar *message)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL || lw->status_event == NULL)
        return;

    gtk_label_set_text(GTK_LABEL(lw->status_event), message);
    gtk_revealer_set_reveal_child(GTK_REVEALER(lw->status_revealer), TRUE);

    if (lw->status_timeout != 0)
        g_source_remove(lw->status_timeout);
    lw->status_timeout = g_timeout_add_seconds(STATUS_FADE_SECONDS,
                                               status_fade_timeout, lw);
}

/* ===========================================================================
 * AI summary
 * =========================================================================== */

/* AiJobData — context passed to the subprocess completion callback.         */
typedef struct {
    OnLibrary    *lw;
    GCancellable *cancellable;   /* owned ref; unrefed in the callback       */
} AiJobData;

/* ai_throbber_tick() — g_timeout_add callback: cycle the status-bar dots
 * animation while an AI request is in flight.                                */
static gboolean
ai_throbber_tick(gpointer user_data)
{
    static const gchar * const frames[] = {
        "\xe2\xa3\xbe", "\xe2\xa3\xbd", "\xe2\xa3\xbb", "\xe2\xa2\xbf",
        "\xe2\xa1\xbf", "\xe2\xa3\x9f", "\xe2\xa3\xaf", "\xe2\xa3\xb7"
    };
    OnLibrary *lw = user_data;
    on_app_status(lw->app, "Generating AI Summary %s",
                  frames[lw->ai_throbber_step % 8]);
    lw->ai_throbber_step++;
    return G_SOURCE_CONTINUE;
}

/* ai_throbber_stop() — cancel the status-bar throbber if running.           */
static void
ai_throbber_stop(OnLibrary *lw)
{
    if (lw->ai_throbber_id) {
        g_source_remove(lw->ai_throbber_id);
        lw->ai_throbber_id = 0;
    }
}

/* on_ai_subprocess_done() — GAsyncReadyCallback: the AI subprocess has
 * exited; read its stdout and put it in the text view.                       */
static void
on_ai_subprocess_done(GObject *source, GAsyncResult *result,
                      gpointer user_data)
{
    AiJobData    *job       = user_data;
    OnLibrary    *lw        = job->lw;
    gboolean      cancelled = g_cancellable_is_cancelled(job->cancellable);
    g_object_unref(job->cancellable);
    g_free(job);

    GBytes  *out   = NULL;             /* stdout bytes from the subprocess    */
    GError  *error = NULL;
    g_subprocess_communicate_finish(G_SUBPROCESS(source),
                                    result, &out, NULL, &error);

    /* When the library window was destroyed while the subprocess was
     * running, library_free() cancelled the GCancellable and freed lw.
     * Clean up the result and return without touching lw.                  */
    if (cancelled) {
        if (out   != NULL) g_bytes_unref(out);
        if (error != NULL) g_error_free(error);
        return;
    }

    lw->ai_running = FALSE;
    ai_throbber_stop(lw);
    g_clear_object(&lw->ai_cancel);
    on_app_status(lw->app, "Summary generation complete");

    GtkTextBuffer *buf =               /* the pane's text buffer              */
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));

    if (error != NULL) {
        gtk_text_buffer_set_text(buf, error->message, -1);
        g_error_free(error);
    } else if (out != NULL) {
        gsize        len  = 0;
        const gchar *data = g_bytes_get_data(out, &len);
        gtk_text_buffer_set_text(buf, data != NULL ? data : "", (gint)len);
        g_bytes_unref(out);
    } else {
        gtk_text_buffer_set_text(buf, "(no output)", -1);
    }

    GtkTextIter start;
    gtk_text_buffer_get_start_iter(buf, &start);
    gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(lw->ai_text),
                                 &start, 0.0, FALSE, 0.0, 0.0);
}

/* run_ai_summary() — collect note content for the current selection, build
 * the prompt (project or normal mode), spawn the configured AI command,
 * and show the output asynchronously.                                        */
/* ai_fail() — show msg in the AI pane, stop the throbber.  Call before
 * any resources are allocated so the caller can return immediately.         */
static void
ai_fail(GtkTextBuffer *buf, OnLibrary *lw, const gchar *msg)
{
    gtk_text_buffer_set_text(buf, msg, -1);
    ai_throbber_stop(lw);
}

static void
run_ai_summary(OnLibrary *lw)
{
    GtkTextBuffer *buf =               /* the pane's text buffer              */
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));

    if (lw->app->ai_command == NULL || *lw->app->ai_command == '\0') {
        ai_fail(buf, lw, "No AI command configured. Please set one in "
                         "File \xe2\x86\x92 Settings \xe2\x86\x92 AI Features.");
        return;
    }

    /* Reject views where summarization doesn't make sense before loading
     * any notes — keeps all early exits before the expensive allocations.   */
    if (lw->sel_kind == SB_KIND_TRASH ||
        lw->sel_kind == SB_KIND_TRASH_FOLDER ||
        lw->sel_kind == SB_KIND_ACTIONS) {
        ai_fail(buf, lw, "Select notes to summarize.");
        return;
    }

    /* Validate the per-folder AI mode before allocating anything.           */
    gint ai_mode = (lw->sel_kind == SB_KIND_FOLDER)
        ? on_db_folder_get_ai_mode(lw->app->db, lw->sel_id)
        : ON_AI_MODE_NORMAL;
    if (ai_mode == ON_AI_MODE_CUSTOM &&
            (lw->app->ai_custom_prompt == NULL ||
             *lw->app->ai_custom_prompt == '\0')) {
        ai_fail(buf, lw, "No custom AI prompt configured. Set one in "
                         "File \xe2\x86\x92 Settings \xe2\x86\x92 AI Features.");
        return;
    }

    GArray *sel_ids = selected_note_ids(lw);
    if (sel_ids->len == 0) {
        g_array_free(sel_ids, TRUE);
        ai_fail(buf, lw, "Select one or more notes to summarize.");
        return;
    }

    GList *notes = NULL;
    for (guint i = 0; i < sel_ids->len; i++) {
        gint64     id = g_array_index(sel_ids, gint64, i);
        OnNoteMeta *m = on_db_note_get(lw->app->db, id);
        if (m != NULL)
            notes = g_list_prepend(notes, m);
    }
    notes = g_list_reverse(notes);
    g_array_free(sel_ids, TRUE);

    if (notes == NULL) {
        ai_fail(buf, lw, "No notes to summarize.");
        return;
    }

    GString *prompt = g_string_new(NULL);

    /* Build the prompt header based on the folder's AI mode.               */
    if (ai_mode == ON_AI_MODE_PROJECT) {
        g_string_append(prompt,
            "This series of notes chronologically details the progress of a "
            "project. Provide me a summary of them all especially focusing on "
            "the current state and any remaining action items.\n\n");
    } else if (ai_mode == ON_AI_MODE_CUSTOM) {
        g_string_append(prompt, lw->app->ai_custom_prompt);
        g_string_append(prompt, "\n\n");
    } else {
        g_string_append(prompt,
            "Provide me a brief summary of these notes.\n\n");
    }

    g_string_append(prompt,
        "Respond in plain text only. Do not use markdown formatting "
        "such as headers, bold, italics, or bullet symbols.\n\n");
    g_string_append(prompt, "=== Notes ===\n\n");

    for (GList *nl = notes; nl != NULL; nl = nl->next) {
        OnNoteMeta *m  = nl->data;     /* one note's metadata                 */
        GDateTime  *dt = g_date_time_new_from_unix_local(m->updated_at);
        gchar      *mod = g_date_time_format(dt, "%Y-%m-%d");
        g_date_time_unref(dt);

        g_string_append_printf(prompt, "--- %s (modified %s) ---\n",
                               m->title != NULL ? m->title : "(untitled)",
                               mod);
        g_free(mod);

        /* Fetch body text for just this note — avoids loading the whole DB. */
        gchar *body = on_db_note_body_text(lw->app->db, m->id);
        if (body != NULL && *body != '\0')
            g_string_append_printf(prompt, "%s\n", body);
        else
            g_string_append(prompt, "(empty note)\n");
        g_free(body);

        /* Action items for just this note.                                  */
        GList   *note_actions = on_db_action_list_for_note(lw->app->db, m->id);
        gboolean first_action = TRUE;
        for (GList *al = note_actions; al != NULL; al = al->next) {
            OnActionItem *it = al->data;
            if (first_action) {
                g_string_append(prompt, "\nAction items:\n");
                first_action = FALSE;
            }
            const gchar *status = it->done ? "[done]" : "[    ]";
            g_string_append_printf(prompt, "  %s %s", status, it->text);
            if (it->due != 0) {
                GDateTime *ddt = g_date_time_new_from_unix_local(it->due);
                gchar     *ds  = g_date_time_format(ddt, "%Y-%m-%d");
                g_date_time_unref(ddt);
                g_string_append_printf(prompt, " (due %s)", ds);
                g_free(ds);
            }
            g_string_append_c(prompt, '\n');
        }
        on_db_action_list_free(note_actions);
        g_string_append_c(prompt, '\n');
    }

    on_db_note_list_free(notes);

    gint    argc_ai = 0;
    gchar **argv_ai = NULL;
    GError *error   = NULL;
    if (!g_shell_parse_argv(lw->app->ai_command, &argc_ai, &argv_ai, &error)) {
        gchar *msg = g_strdup_printf("Invalid AI command: %s", error->message);
        gtk_text_buffer_set_text(buf, msg, -1);
        g_free(msg);
        g_error_free(error);
        g_string_free(prompt, TRUE);
        return;
    }
    (void)argc_ai;

    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE  |
        G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_MERGE);
    GSubprocess *proc = g_subprocess_launcher_spawnv(
        launcher, (const gchar * const *)argv_ai, &error);
    g_strfreev(argv_ai);
    g_object_unref(launcher);

    if (proc == NULL) {
        gchar *msg = g_strdup_printf("Failed to start AI command: %s",
                                     error->message);
        g_error_free(error);
        g_string_free(prompt, TRUE);
        gtk_text_buffer_set_text(buf, msg, -1);
        g_free(msg);
        ai_throbber_stop(lw);
        return;
    }

    lw->ai_running = TRUE;

    gsize  plen        = prompt->len;
    gchar *pstr        = g_string_free(prompt, FALSE);
    GBytes *stdin_bytes = g_bytes_new_take(pstr, plen);

    AiJobData *job        = g_new(AiJobData, 1);
    job->lw               = lw;
    job->cancellable      = g_cancellable_new();
    lw->ai_cancel         = g_object_ref(job->cancellable);

    g_subprocess_communicate_async(proc, stdin_bytes, job->cancellable,
                                   on_ai_subprocess_done, job);
    g_bytes_unref(stdin_bytes);
    g_object_unref(proc);
}

/* on_ai_copy_clicked() — copy the AI summary text to the clipboard.         */
static void
on_ai_copy_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    OnLibrary     *lw  = user_data;
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buf, &start, &end);
    gchar *text = gtk_text_buffer_get_text(buf, &start, &end, FALSE);
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, text, -1);
    g_free(text);
}

/* on_ai_button_clicked() — toolbar AI button: show the pane and (re)run the
 * summary; re-clicking while a run is in progress is a no-op.               */
static void
on_ai_button_clicked(GtkToolButton *btn, gpointer user_data)
{
    (void)btn;
    OnLibrary *lw = user_data;         /* owning library window               */

    if (lw->ai_running)
        return;

    /* Set a comfortable initial split the first time the pane opens.         */
    if (!gtk_widget_get_visible(lw->ai_pane)) {
        gint total = gtk_widget_get_allocated_height(
            GTK_WIDGET(lw->notes_paned));
        if (total > 300)
            gtk_paned_set_position(GTK_PANED(lw->notes_paned),
                                   total - 220);
    }
    gtk_widget_show(lw->ai_pane);

    /* Clear text and start the status-bar throbber for immediate feedback.   */
    GtkTextBuffer *buf =
        gtk_text_view_get_buffer(GTK_TEXT_VIEW(lw->ai_text));
    gtk_text_buffer_set_text(buf, "", -1);
    lw->ai_throbber_step = 0;
    on_app_status(lw->app, "Generating AI Summary \xe2\xa3\xbe");
    lw->ai_throbber_id = g_timeout_add(120, ai_throbber_tick, lw);

    run_ai_summary(lw);
}

/* build_ai_pane() — construct the AI output pane (hidden by default): a
 * header row with a copy button and close button, then a scrolled
 * non-editable text view.  lw->ai_text is set here.
 * Uses no_show_all so gtk_widget_show_all on the window doesn't reveal it.  */
static GtkWidget *
build_ai_pane(OnLibrary *lw)
{
    GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_box_pack_start(GTK_BOX(pane),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(header, 6);
    gtk_widget_set_margin_end(header, 2);
    gtk_widget_set_margin_top(header, 1);
    gtk_widget_set_margin_bottom(header, 1);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<small><b>AI Summary</b></small>");
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);

    /* Compact CSS shared by both header buttons.                             */
    const gchar *btn_css =
        "button { padding: 0 4px; min-height: 0; font-size: 85%; }";

    /* pack_end places items right-to-left, so close (✕) goes first to land  */
    /* at the far right, then copy before it.                                 */
    GtkWidget *close_btn = gtk_button_new_with_label("\xe2\x9c\x95");
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close_btn, "Close AI summary");
    on_app_widget_add_css(close_btn, btn_css);
    g_signal_connect_swapped(close_btn, "clicked",
                             G_CALLBACK(gtk_widget_hide), pane);
    gtk_box_pack_end(GTK_BOX(header), close_btn, FALSE, FALSE, 0);

    GtkWidget *copy_btn = gtk_button_new_with_label("Copy");
    gtk_button_set_relief(GTK_BUTTON(copy_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(copy_btn, "Copy summary to clipboard");
    on_app_widget_add_css(copy_btn, btn_css);
    g_signal_connect(copy_btn, "clicked",
                     G_CALLBACK(on_ai_copy_clicked), lw);
    gtk_box_pack_end(GTK_BOX(header), copy_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(pane), header, FALSE, FALSE, 0);

    lw->ai_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(lw->ai_text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(lw->ai_text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(lw->ai_text),
                                GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(lw->ai_text), 8);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(lw->ai_text), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(lw->ai_text), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(lw->ai_text), 4);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(scroll), lw->ai_text);
    gtk_box_pack_start(GTK_BOX(pane), scroll, TRUE, TRUE, 0);

    gtk_widget_show_all(pane);
    /* no_show_all AFTER show_all so children are realized but the pane       */
    /* itself is excluded from future show_all sweeps.                        */
    gtk_widget_set_no_show_all(pane, TRUE);
    gtk_widget_hide(pane);             /* starts hidden until AI is clicked   */
    return pane;
}

/* library_notify_ai_changed() — installed as app->notify_ai_changed;
 * shows or hides the AI toolbar button based on app->ai_enabled.            */
static void
library_notify_ai_changed(OnApp *app)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL || lw->ai_btn == NULL)
        return;
    if (app->ai_enabled) {
        gtk_widget_show(GTK_WIDGET(lw->ai_btn));
    } else {
        gtk_widget_hide(GTK_WIDGET(lw->ai_btn));
        if (lw->ai_pane != NULL)
            gtk_widget_hide(lw->ai_pane);
    }
}

/* ===========================================================================
 * refresh hook + construction
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * library_notify_notes_changed() — installed as app->notify_notes_changed;
 * editor windows call it after each save so titles, ordering and the tag
 * sidebar stay current.
 * ------------------------------------------------------------------------- */
static void
library_notify_notes_changed(OnApp *app)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw != NULL)
        refresh_all(lw);
}

/* ---------------------------------------------------------------------------
 * library_notify_note_saved() — installed as app->notify_note_saved: update
 * the ONE row the saved note owns, instead of rebuilding the notes pane.
 *
 * A save can change a note's title, its modified time and its preview line —
 * never WHICH notes are listed.  Membership in every view the light path can
 * be reached from is decided by things a save leaves alone: the folder tree,
 * the pinned flag, the trashed flag, and the tag set (a tag change takes the
 * full notify instead).  So the model, the selection and the scroll position
 * can all stay exactly as they are.
 *
 * That matters because this fires on every autosave — roughly every 1.2 s of
 * continuous typing.  The old full repopulate re-queried the note list,
 * re-formatted two timestamps and re-measured Pango widths for every row,
 * then walked the model again to restore the selection: ~93 ms of per-row
 * work at 1300 notes, for one changed row.
 * ------------------------------------------------------------------------- */
static void
library_notify_note_saved(OnApp *app, gint64 note_id)
{
    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL)
        return;

    /* The Action Items view lists items, not notes, and orders them by the
     * owning note's modified time — a save can reshuffle it, so that view
     * still takes the full repopulate (it is not the typing-hot case).      */
    if (lw->sel_kind == SB_KIND_ACTIONS) {
        refresh_notes(lw);
        return;
    }

    /* Find the row.  Absent means the note is not in the current view, and
     * a save cannot have changed that — nothing to do.                     */
    GtkTreeModel *model = GTK_TREE_MODEL(lw->notes_store);
    GtkTreeIter   iter;              /* the saved note's row                */
    gboolean      found = FALSE;
    gboolean      valid = gtk_tree_model_get_iter_first(model, &iter);
    /* NOT a for loop: its increment would run after the match and advance
     * `iter` off the row we just found — invalidating it outright when the
     * match is the last row, which is where a just-saved note usually is.   */
    while (valid && !found) {
        gint64 id;                   /* this row's note id                  */
        gtk_tree_model_get(model, &iter, NL_ID, &id, -1);
        if (id == note_id)
            found = TRUE;            /* leave `iter` ON the match           */
        else
            valid = gtk_tree_model_iter_next(model, &iter);
    }
    if (!found)
        return;

    OnNoteMeta *m = on_db_note_get(lw->app->db, note_id);
    if (m == NULL)
        return;

    GDateTime *dt = g_date_time_new_from_unix_local(m->updated_at);
    gchar *when = g_date_time_format(dt, LIST_TIME_FORMAT);
    g_date_time_unref(dt);

    /* Preview only where it is shown (Comfortable density, list view).      */
    gchar *preview = NULL;
    if (lw->app->comfortable_list) {
        gchar *body = on_db_note_body_text(lw->app->db, note_id);
        preview = notes_preview_line(body);
        g_free(body);
    }

    /* Setting NL_UPDATED is what re-sorts the row to the top under the
     * default Modified ordering — the same place the old repopulate's
     * "ORDER BY updated_at DESC" put it.  populating stays DOWN: this is a
     * real content change and the selection handlers should see it.         */
    gtk_list_store_set(lw->notes_store, &iter,
                       NL_TITLE,    m->title,
                       NL_MODIFIED, when,
                       NL_UPDATED,  m->updated_at,
                       NL_PREVIEW,  preview,
                       -1);

    /* The grid's thumbnail for this note is now stale; re-render it in idle
     * time exactly as a repopulate would have (list mode pays nothing).     */
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(lw->stack)),
                  "grid") == 0) {
        GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
        ThumbJob *job = g_new0(ThumbJob, 1);
        job->row        = gtk_tree_row_reference_new(model, path);
        job->id         = note_id;
        job->updated_at = m->updated_at;
        g_queue_push_tail(&lw->thumb_pending, job);
        gtk_tree_path_free(path);
        if (lw->thumb_idle == 0)
            lw->thumb_idle = g_idle_add(thumb_fill_idle, lw);
    }

    g_free(preview);
    g_free(when);
    on_db_note_meta_free(m);
}

void
on_library_apply_native_menubar(OnApp *app, gboolean native)
{
#ifdef HAVE_GTKOSX
    GtkWidget *menubar =             /* the in-window GtkMenuBar            */
        g_object_get_data(G_OBJECT(app->library_window), "on-menubar");
    if (menubar == NULL)
        return;

    GtkosxApplication *osx = gtkosx_application_get();
    if (native) {
        /* The same menu shell drives the macOS bar; the in-window widget
         * just has to be hidden.                                           */
        gtk_widget_hide(menubar);
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(menubar));
    } else {
        gtk_widget_show(menubar);
        /* Hand macOS an empty bar so the app menu stays functional.        */
        GtkWidget *empty = gtk_menu_bar_new();
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(empty));
    }
    gtkosx_application_sync_menubar(osx);
#else
    (void)app; (void)native;
#endif
}

void
on_library_get_scope(OnApp *app, OnSearchScope *scope, gint64 *id,
                     gchar **name)
{
    *scope = ON_SCOPE_FOLDER;
    *id    = 0;
    *name  = g_strdup("Notes");

    OnLibrary *lw = lw_from_app(app);
    if (lw == NULL)
        return;

    /* Only tags and folder-like selections (including a trashed folder
     * browsed from the Trash section) make a meaningful scope; All Notes,
     * Trash and Pinned keep the "everything" default above.                 */
    if (lw->sel_kind != SB_KIND_TAG &&
        lw->sel_kind != SB_KIND_FOLDER &&
        lw->sel_kind != SB_KIND_TRASH_FOLDER &&
        lw->sel_kind != SB_KIND_ROOT)
        return;

    *scope = (lw->sel_kind == SB_KIND_TAG) ? ON_SCOPE_TAG
                                           : ON_SCOPE_FOLDER;
    *id    = lw->sel_id;
    if (lw->sel_name != NULL) {
        g_free(*name);
        *name = g_strdup(lw->sel_name);
    }
}

/* ---------------------------------------------------------------------------
 * sort_by_title() — case-insensitive alphabetical sort for the Title
 * header.
 * ------------------------------------------------------------------------- */
static gint
sort_by_title(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
              gpointer user_data)
{
    (void)user_data;
    gchar *ta, *tb;                  /* the two titles                      */
    gtk_tree_model_get(model, a, NL_TITLE, &ta, -1);
    gtk_tree_model_get(model, b, NL_TITLE, &tb, -1);
    gint result = utf8_casecmp(ta, tb);
    g_free(ta); g_free(tb);
    return result;
}

/* ===========================================================================
 * list-view column layout (order + visibility)
 *
 * Headers drag to reorder (GtkTreeViewColumn reorderable) and right-click
 * for a show/hide menu.  The layout persists in the ini as
 * "<cfgkey>=key:vis,key:vis,..." in display order; each column carries
 * its stable key as object data ("on-colkey").  The machinery is shared
 * by the notes list and the Action Items list: each VIEW carries its ini
 * key ("on-colcfg"), its expected column count ("on-ncols") and its
 * default layout ("on-coldefault") as object data.
 * =========================================================================== */

/* How many columns the notes list owns (Title, Path, Modified, Created) —
 * keep in sync with the COLS[] table in the window constructor.             */
#define N_LIST_COLUMNS 4

/* ---------------------------------------------------------------------------
 * view_columns_persist() — write a list view's current column order and
 * visibility to the ini.  A partial column list (the view tearing down
 * removes columns one by one) is never persisted.
 * ------------------------------------------------------------------------- */
static void
view_columns_persist(GtkTreeView *view)
{
    const gchar *cfg_key =           /* the view's ini key                  */
        g_object_get_data(G_OBJECT(view), "on-colcfg");
    gint n_cols =                    /* the view's full column count        */
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(view), "on-ncols"));
    GList *cols = gtk_tree_view_get_columns(view);
    if (cfg_key == NULL || g_list_length(cols) != (guint)n_cols) {
        g_list_free(cols);
        return;
    }
    GString *s = g_string_new(NULL);
    for (GList *l = cols; l != NULL; l = l->next) {
        const gchar *key =           /* stable column identity              */
            g_object_get_data(G_OBJECT(l->data), "on-colkey");
        if (key == NULL)
            continue;
        if (s->len > 0)
            g_string_append_c(s, ',');
        g_string_append_printf(s, "%s:%d", key,
            gtk_tree_view_column_get_visible(l->data) ? 1 : 0);
    }
    g_list_free(cols);
    on_app_config_set(cfg_key, s->str);
    g_string_free(s, TRUE);
}

/* view_column_by_key() — the view's column carrying `key`, or NULL.         */
static GtkTreeViewColumn *
view_column_by_key(GtkTreeView *view, const gchar *key)
{
    GtkTreeViewColumn *found = NULL;
    GList *cols = gtk_tree_view_get_columns(view);
    for (GList *l = cols; l != NULL && found == NULL; l = l->next)
        if (g_strcmp0(g_object_get_data(G_OBJECT(l->data), "on-colkey"),
                      key) == 0)
            found = l->data;
    g_list_free(cols);
    return found;
}

/* list_column_by_key() — notes-list shorthand (autofit uses it a lot).      */
static GtkTreeViewColumn *
list_column_by_key(OnLibrary *lw, const gchar *key)
{
    return view_column_by_key(lw->notes_list, key);
}

/* list_column_shown() — is the notes list's `key` column visible right now?
 * Autofit consults this BEFORE measuring: list_autofit_set() discards the
 * width of a hidden column, so measuring one is wasted work.                */
static gboolean
list_column_shown(OnLibrary *lw, const gchar *key)
{
    GtkTreeViewColumn *c = list_column_by_key(lw, key);
    return c != NULL && gtk_tree_view_column_get_visible(c);
}

/* ---------------------------------------------------------------------------
 * list_autofit_time_width() — an upper bound, in pixels, on any timestamp
 * the Modified or Created column can hold.
 *
 * Both columns render one fixed pattern (LIST_TIME_FORMAT), so every value
 * has the same shape and differs only in which month abbreviation and which
 * digits it contains.  Measuring the widest localized month against the
 * widest digit therefore bounds the whole column in ~22 measurements instead
 * of one per row — which at a thousand-plus notes was the most expensive
 * thing refresh_notes() did, repeated on every autosave-driven refresh.
 *
 * A bound rather than the exact maximum is the right answer here: the caller
 * adds AUTOFIT_CELL_EXTRA px of cell chrome on top anyway, and erring wide
 * only pads the column, while erring narrow would clip text (these columns
 * do not ellipsize under autofit).
 *   lay — the measuring layout, in the view's font.
 * Returns the bound in pixels.
 * ------------------------------------------------------------------------- */
static gint
list_autofit_time_width(PangoLayout *lay)
{
    /* Widest digit glyph — proportional fonts do vary ('1' vs '8').         */
    gchar widest  = '0';             /* digit with the largest advance      */
    gint  digit_w = 0;               /* its width                           */
    for (gchar d = '0'; d <= '9'; d++) {
        gint w;                      /* width of this digit                 */
        pango_layout_set_text(lay, &d, 1);
        pango_layout_get_pixel_size(lay, &w, NULL);
        if (w > digit_w) {
            digit_w = w;
            widest  = d;
        }
    }

    /* Each month's name in a full synthetic stamp whose every digit is the
     * widest one, so no real timestamp can come out wider.                  */
    gint max_w = 0;                  /* running maximum                     */
    for (gint month = 1; month <= 12; month++) {
        GDateTime *dt = g_date_time_new_local(2026, month, 28, 23, 59, 0);
        if (dt == NULL)
            continue;
        gchar *stamp = g_date_time_format(dt, LIST_TIME_FORMAT);
        g_date_time_unref(dt);
        if (stamp == NULL)
            continue;
        for (gchar *p = stamp; *p != '\0'; p++)
            if (g_ascii_isdigit(*p))
                *p = widest;
        gint w;                      /* width of this month's stamp         */
        pango_layout_set_text(lay, stamp, -1);
        pango_layout_get_pixel_size(lay, &w, NULL);
        max_w = MAX(max_w, w);
        g_free(stamp);
    }
    return max_w;
}

/* ---------------------------------------------------------------------------
 * view_columns_apply() — put a view's saved column order and visibility
 * back (falling back to its "on-coldefault" layout).  Unknown keys are
 * skipped, missing ones keep their built order and visibility, and at
 * least one column is forced visible (a hand-edited ini can't blank the
 * view).
 * ------------------------------------------------------------------------- */
static void
view_columns_apply(GtkTreeView *view)
{
    const gchar *cfg_key =           /* the view's ini key                  */
        g_object_get_data(G_OBJECT(view), "on-colcfg");
    gchar *cfg = cfg_key != NULL ? on_app_config_get(cfg_key) : NULL;
    if (cfg == NULL || *cfg == '\0') {
        g_free(cfg);
        cfg = g_strdup(g_object_get_data(G_OBJECT(view), "on-coldefault"));
    }

    GtkTreeViewColumn *prev = NULL;  /* each entry moves after the last     */
    gchar **entries = g_strsplit(cfg, ",", -1);
    for (gsize i = 0; entries[i] != NULL; i++) {
        gchar **kv = g_strsplit(entries[i], ":", 2);
        GtkTreeViewColumn *c =
            kv[0] != NULL ? view_column_by_key(view, kv[0]) : NULL;
        if (c != NULL) {
            gtk_tree_view_move_column_after(view, c, prev);
            gtk_tree_view_column_set_visible(
                c, kv[1] == NULL || g_strcmp0(kv[1], "0") != 0);
            prev = c;
        }
        g_strfreev(kv);
    }
    g_strfreev(entries);
    g_free(cfg);

    GList *cols = gtk_tree_view_get_columns(view);
    gboolean any_visible = FALSE;    /* is at least one column shown?       */
    for (GList *l = cols; l != NULL; l = l->next)
        any_visible |= gtk_tree_view_column_get_visible(l->data);
    if (!any_visible && cols != NULL)
        gtk_tree_view_column_set_visible(cols->data, TRUE);
    g_list_free(cols);
}

/* on_view_columns_changed() — a header drag reordered the columns:
 * persist the new layout.  Fires per-column during teardown too, when
 * the library state may already be gone — bail out then.                    */
static void
on_view_columns_changed(GtkTreeView *view, gpointer user_data)
{
    (void)user_data;
    if (gtk_widget_in_destruction(GTK_WIDGET(view)))
        return;
    view_columns_persist(view);
}

/* ---------------------------------------------------------------------------
 * list_autofit_apply() — put every column into (or out of) autofit mode.
 *
 * Autofit does NOT use GTK_TREE_VIEW_COLUMN_AUTOSIZE: tree-view columns
 * cache resized/requested widths that override it (they neither grow to
 * long content nor shrink back for short content).  Instead every column
 * goes FIXED, and refresh_notes() MEASURES the content with a
 * PangoLayout as it populates the model (see list_autofit_set) and
 * sets the exact widths — same technique as the sidebar width fit.
 *
 *   ON  — Path and Modified: no ellipsize, FIXED at their measured
 *         content width (grips off — the next refresh would reclaim
 *         them anyway).  Title takes the ellipsis + expand: it fills
 *         the remaining space and is the one column that truncates.
 *   OFF — back to the built modes: Title GROW_ONLY + expand, no
 *         ellipsize; Path FIXED at its current width, ellipsized;
 *         Modified GROW_ONLY; all user-resizable.
 * ------------------------------------------------------------------------- */
static void
list_autofit_apply(OnLibrary *lw)
{
    GList *cols = gtk_tree_view_get_columns(lw->notes_list);
    for (GList *l = cols; l != NULL; l = l->next) {
        GtkTreeViewColumn *c = l->data;  /* one column                      */
        const gchar *key =               /* stable column identity          */
            g_object_get_data(G_OBJECT(c), "on-colkey");
        GtkCellRenderer *cell =          /* its text renderer               */
            g_object_get_data(G_OBJECT(c), "on-cell");
        gboolean title = g_strcmp0(key, "title") == 0;
        gboolean path  = g_strcmp0(key, "path")  == 0;

        if (lw->list_autofit) {
            g_object_set(cell, "ellipsize",
                         title ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE,
                         NULL);
            gtk_tree_view_column_set_resizable(c, FALSE);
            gtk_tree_view_column_set_sizing(
                c, GTK_TREE_VIEW_COLUMN_FIXED);
            if (title)                   /* floor; expand fills the rest    */
                gtk_tree_view_column_set_fixed_width(c, 100);
            gtk_tree_view_column_set_expand(c, title);
        } else {
            g_object_set(cell, "ellipsize",
                         path ? PANGO_ELLIPSIZE_END : PANGO_ELLIPSIZE_NONE,
                         NULL);
            if (path) {
                gint w = gtk_tree_view_column_get_width(c);
                gtk_tree_view_column_set_sizing(
                    c, GTK_TREE_VIEW_COLUMN_FIXED);
                gtk_tree_view_column_set_fixed_width(c, w > 0 ? w : 180);
            } else {
                gtk_tree_view_column_set_sizing(
                    c, GTK_TREE_VIEW_COLUMN_GROW_ONLY);
            }
            gtk_tree_view_column_set_expand(c, title);
            gtk_tree_view_column_set_resizable(c, TRUE);
        }
        gtk_tree_view_column_queue_resize(c);
    }
    g_list_free(cols);
}

/* Extra pixels beyond the raw text: the renderer's xpad (10 a side)
 * plus tree-view cell chrome; headers get room for the sort arrow.        */
#define AUTOFIT_CELL_EXTRA   30
#define AUTOFIT_HEADER_EXTRA 40

/* ---------------------------------------------------------------------------
 * list_autofit_set() — apply a measured content width to one column
 * (bounded below by what its header label needs).  The measuring itself
 * rides refresh_notes()'s population loop — no second model walk.
 * ------------------------------------------------------------------------- */
static void
list_autofit_set(OnLibrary *lw, PangoLayout *lay, const gchar *key,
                 gint content_w)
{
    GtkTreeViewColumn *c = list_column_by_key(lw, key);
    if (c == NULL || !gtk_tree_view_column_get_visible(c))
        return;
    gint w = 0;                      /* header label width                  */
    pango_layout_set_text(lay, gtk_tree_view_column_get_title(c), -1);
    pango_layout_get_pixel_size(lay, &w, NULL);
    gtk_tree_view_column_set_fixed_width(
        c, MAX(content_w + AUTOFIT_CELL_EXTRA, w + AUTOFIT_HEADER_EXTRA));
}

/* on_autofit_toggled() — the "Autofit Column Widths" check item flipped:
 * remember, persist and apply.                                              */
static void
on_autofit_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    lw->list_autofit = gtk_check_menu_item_get_active(item);
    on_app_config_set("list_autofit", lw->list_autofit ? "1" : "0");
    list_autofit_apply(lw);
    refresh_notes(lw);               /* measuring rides the populate loop   */
}

/* on_column_toggled() — a check item in the header menu flipped:
 * show/hide that column and persist.  Autofit re-measuring applies to
 * the notes list only (the Action Items list doesn't autofit).             */
static void
on_column_toggled(GtkCheckMenuItem *item, gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    GtkTreeViewColumn *c = g_object_get_data(G_OBJECT(item), "on-column");
    GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_column_get_tree_view(c));
    gtk_tree_view_column_set_visible(
        c, gtk_check_menu_item_get_active(item));
    view_columns_persist(view);
    if (view == lw->notes_list && lw->list_autofit)
        refresh_notes(lw);           /* a re-shown column needs its width   */
}

/* ---------------------------------------------------------------------------
 * on_column_header_press() — right click on a list-view column header:
 * a menu of check items showing/hiding each column of the header's view
 * (the button carries its view as "on-view").  The only remaining
 * visible column's item is disabled so the view can't go empty; the
 * notes list's menu also offers the autofit toggle.
 * ------------------------------------------------------------------------- */
static gboolean
on_column_header_press(GtkWidget *button, GdkEventButton *event,
                       gpointer user_data)
{
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->button != GDK_BUTTON_SECONDARY)
        return FALSE;
    GtkTreeView *view =              /* the view this header belongs to     */
        g_object_get_data(G_OBJECT(button), "on-view");
    if (view == NULL)
        return FALSE;

    GtkWidget *menu = menu_new_transient(lw);

    GList *cols = gtk_tree_view_get_columns(view);
    gint n_visible = 0;              /* how many columns are shown          */
    for (GList *l = cols; l != NULL; l = l->next)
        if (gtk_tree_view_column_get_visible(l->data))
            n_visible++;

    for (GList *l = cols; l != NULL; l = l->next) {
        GtkTreeViewColumn *c = l->data;  /* one column                      */
        gboolean visible = gtk_tree_view_column_get_visible(c);
        const gchar *label =         /* a titleless column (the checkbox
                                        one) still needs a menu label       */
            g_object_get_data(G_OBJECT(c), "on-collabel");
        if (label == NULL)
            label = gtk_tree_view_column_get_title(c);
        GtkWidget *item = gtk_check_menu_item_new_with_label(label);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), visible);
        if (visible && n_visible == 1)
            gtk_widget_set_sensitive(item, FALSE);
        g_object_set_data(G_OBJECT(item), "on-column", c);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_column_toggled), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    g_list_free(cols);

    if (view == lw->notes_list) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
        GtkWidget *fit = gtk_check_menu_item_new_with_label(
            "Autofit Column Widths");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(fit),
                                       lw->list_autofit);
        g_signal_connect(fit, "toggled",
                         G_CALLBACK(on_autofit_toggled), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), fit);
    }

    menu_popup(menu, event);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * sort_by_path() — case-insensitive alphabetical sort for the Path
 * header; equal paths fall back to the title so folders group cleanly.
 * ------------------------------------------------------------------------- */
static gint
sort_by_path(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
             gpointer user_data)
{
    (void)user_data;
    gchar *pa, *pb;                  /* the two paths                       */
    gtk_tree_model_get(model, a, NL_PATH, &pa, -1);
    gtk_tree_model_get(model, b, NL_PATH, &pb, -1);
    gint result = utf8_casecmp(pa, pb);
    g_free(pa); g_free(pb);
    return result != 0 ? result : sort_by_title(model, a, b, NULL);
}

/* ---------------------------------------------------------------------------
 * sort_by_time() — sort for the Modified and Created headers; the model
 * column holding the raw timestamp (NL_UPDATED or NL_CREATED_RAW) rides
 * in as GINT_TO_POINTER user_data.  Deliberately inverted so the FIRST
 * click on either header shows the newest notes on top.
 * ------------------------------------------------------------------------- */
static gint
sort_by_time(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
             gpointer user_data)
{
    gint col = GPOINTER_TO_INT(user_data);  /* the timestamp column         */
    gint64 ta, tb;                   /* the two timestamps                  */
    gtk_tree_model_get(model, a, col, &ta, -1);
    gtk_tree_model_get(model, b, col, &tb, -1);
    return (tb > ta) - (tb < ta);
}

/* sort_actions_by_text() — alphabetical sort for the Action header.         */
static gint
sort_actions_by_text(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                     gpointer user_data)
{
    (void)user_data;
    gchar *ta, *tb;                  /* the two item texts                  */
    gtk_tree_model_get(model, a, AL_TEXT, &ta, -1);
    gtk_tree_model_get(model, b, AL_TEXT, &tb, -1);
    gint result = utf8_casecmp(ta, tb);
    g_free(ta); g_free(tb);
    return result;
}

/* ---------------------------------------------------------------------------
 * action_due_color_func() — cell data function tinting the Due Date cell
 * by urgency: already passed = red, today = yellow, still ahead = green
 * (each darkened enough to read on the white/light-blue row stripes).
 * Runs at draw time, so the colors roll over at midnight without a
 * refresh.  Rows without a due date reset to the theme color — the
 * renderer is shared, so a stale "foreground" would leak across rows.
 * ------------------------------------------------------------------------- */
static void
action_due_color_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                      GtkTreeModel *model, GtkTreeIter *iter,
                      gpointer user_data)
{
    (void)col; (void)user_data;
    gint64 due;                      /* the row's due timestamp             */
    gtk_tree_model_get(model, iter, AL_DUE_RAW, &due, -1);
    if (due == 0) {
        g_object_set(cell, "foreground-set", FALSE, NULL);
        return;
    }

    /* Compare calendar DAYS in local time (the stored value is already
     * local midnight, but day-level compare keeps this DST-proof).         */
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *dt  = g_date_time_new_from_unix_local(due);
    gint today = g_date_time_get_year(now) * 10000 +
                 g_date_time_get_month(now) * 100 +
                 g_date_time_get_day_of_month(now);
    gint day   = g_date_time_get_year(dt) * 10000 +
                 g_date_time_get_month(dt) * 100 +
                 g_date_time_get_day_of_month(dt);
    g_date_time_unref(now);
    g_date_time_unref(dt);

    const gchar *color = day < today   ? "#c01c28"   /* overdue: red       */
                       : day == today  ? "#d19a00"   /* today: gold        */
                                       : "#26a269";  /* ahead: green       */
    g_object_set(cell, "foreground", color, NULL);
}

/* sort_actions_by_due() — Due Date header: the first click shows the
 * soonest deadline on top; items without a due date sort after every
 * dated one.                                                                */
static gint
sort_actions_by_due(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
                    gpointer user_data)
{
    (void)user_data;
    gint64 da, db;                   /* the two due timestamps              */
    gtk_tree_model_get(model, a, AL_DUE_RAW, &da, -1);
    gtk_tree_model_get(model, b, AL_DUE_RAW, &db, -1);
    if (da == 0) da = G_MAXINT64;    /* undated: always last                */
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* ---------------------------------------------------------------------------
 * notes_title_cell_func() — cell data function for the Title column.
 * Handles the alternating row tint AND the density-dependent rendering:
 *   Compact      — plain text title, minimal ypad.
 *   Comfortable  — bold Pango markup title with a small dimmed body-text
 *                  preview on the second line, generous ypad.
 * Alpha-based dimming for the preview keeps it readable on both the
 * selection highlight and the plain row background (fixed grey fails on
 * the blue selection — see hacienda task_desc_markup for the same rule).
 * ------------------------------------------------------------------------- */
static void
notes_title_cell_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                      GtkTreeModel *model, GtkTreeIter *iter,
                      gpointer user_data)
{
    (void)col;
    OnLibrary *lw = user_data;         /* owning library window               */

    /* Alternating row tint — same as notes_row_bg_func.                    */
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even = (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    gtk_tree_path_free(path);
    g_object_set(cell, "cell-background", even ? NULL : ROW_TINT, NULL);

    gchar *title   = NULL;
    gchar *preview = NULL;
    gtk_tree_model_get(model, iter,
                       NL_TITLE,   &title,
                       NL_PREVIEW, &preview,
                       -1);

    if (lw->app->comfortable_list) {
        gchar *esc = g_markup_escape_text(
            title != NULL && *title != '\0' ? title : "Untitled", -1);
        gchar *markup;
        gboolean bold = lw->app->bold_list_titles;
        if (preview != NULL && *preview != '\0') {
            gchar *esc_prev = g_markup_escape_text(preview, -1);
            markup = g_strdup_printf(
                bold ? "<b>%s</b>\n<small><span alpha=\"65%%\">%s</span></small>"
                     :    "%s\n<small><span alpha=\"65%%\">%s</span></small>",
                esc, esc_prev);
            g_free(esc_prev);
        } else {
            markup = g_strdup_printf(bold ? "<b>%s</b>" : "%s", esc);
        }
        g_object_set(cell, "markup", markup, "ypad", 7, NULL);
        g_free(markup);
        g_free(esc);
    } else {
        /* Always drive via "markup" so the Pango attribute list (set when
         * comfortable mode was last active) gets replaced, not left behind
         * to render the plain title in bold.                                */
        gchar *esc = g_markup_escape_text(title != NULL ? title : "", -1);
        g_object_set(cell, "markup", esc, "ypad", 2, NULL);
        g_free(esc);
    }
    g_free(title);
    g_free(preview);
}

/* ---------------------------------------------------------------------------
 * notes_row_bg_func() — cell data function giving list rows alternating
 * white / light-blue backgrounds regardless of theme.
 * ------------------------------------------------------------------------- */
static void
notes_row_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                  GtkTreeModel *model, GtkTreeIter *iter,
                  gpointer user_data)
{
    (void)col; (void)user_data;
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even =                  /* row parity drives the tint          */
        (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    gtk_tree_path_free(path);
    g_object_set(cell,
                 "cell-background", even ? NULL : ROW_TINT,
                 NULL);
}

/* ---------------------------------------------------------------------------
 * add_menu_item() — helper: append one item with a callback to a menu.
 *   menu     — the GtkMenu to append to.
 *   label    — item label (mnemonics with '_').
 *   callback — "activate" handler.
 *   data     — user data for the handler.
 * ------------------------------------------------------------------------- */
static void
add_menu_item(GtkWidget *menu, const gchar *label, GCallback callback,
              gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(item, "activate", callback, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* ---------------------------------------------------------------------------
 * build_menubar() — the File and View menus.
 * Returns the GtkMenuBar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_menubar(OnLibrary *lw)
{
    GtkWidget *bar = gtk_menu_bar_new();

    /* File menu.                                                           */
    GtkWidget *file_menu = gtk_menu_new();
    add_menu_item(file_menu, "_New Note",
                  G_CALLBACK(on_new_note), lw);
    add_menu_item(file_menu, "New _Folder\xe2\x80\xa6",
                  G_CALLBACK(on_new_folder), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "Export All as _HTML\xe2\x80\xa6",
                  G_CALLBACK(on_export_html), lw);
    add_menu_item(file_menu, "Export All as _Markdown\xe2\x80\xa6",
                  G_CALLBACK(on_export_markdown), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_Open Database\xe2\x80\xa6",
                  G_CALLBACK(on_open_db), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_Settings\xe2\x80\xa6",
                  G_CALLBACK(on_open_settings), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_About", G_CALLBACK(on_about), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(file_menu, "_Quit", G_CALLBACK(on_quit), lw);

    GtkWidget *file_root = gtk_menu_item_new_with_mnemonic("_File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_root), file_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), file_root);

    /* View menu.  (Toolbar styles now live in File → Settings….)           */
    GtkWidget *view_menu = gtk_menu_new();
    add_menu_item(view_menu, "Notes as _List", G_CALLBACK(on_view_list), lw);
    add_menu_item(view_menu, "Notes as _Grid", G_CALLBACK(on_view_grid), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          gtk_separator_menu_item_new());
    add_menu_item(view_menu, "_Media\xe2\x80\xa6",
                  G_CALLBACK(on_open_media), lw);
    add_menu_item(view_menu, "_Search Notes\xe2\x80\xa6",
                  G_CALLBACK(on_open_search), lw);

    GtkWidget *view_root = gtk_menu_item_new_with_mnemonic("_View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_root), view_menu);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), view_root);

    return bar;
}

/* ---------------------------------------------------------------------------
 * add_tool_button() — helper: append a tool button with a callback.
 *   lw       — the library window.
 *   toolbar  — the GtkToolbar to append to.
 *   icon     — local icon file basename, or NULL.
 *   fallback — markup shown as the icon when the file is missing.
 *   label    — button text label.
 *   tooltip  — hover help.
 *   cb       — "clicked" handler.
 * ------------------------------------------------------------------------- */
static void
add_tool_button(OnLibrary *lw, GtkWidget *toolbar, const gchar *icon,
                const gchar *fallback, const gchar *label,
                const gchar *tooltip, GCallback cb)
{
    GtkToolItem *item = on_app_tool_item_new(lw->app, FALSE, icon,
                                             fallback, label, tooltip);
    g_signal_connect(item, "clicked", cb, lw);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), item, -1);
}

/* ---------------------------------------------------------------------------
 * build_action_bar() — the single unified toolbar spanning the window:
 * a folder-actions area, a drawn separator, a note-actions area (ending
 * with the List/Grid toggle and Media), another separator, Settings and
 * Search, a third separator, the AI Summary button (shown only while AI is
 * enabled) — and the search entry pinned to the right edge by an expanding
 * spacer.
 * Returns the toolbar widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
build_action_bar(OnLibrary *lw)
{
    GtkWidget *toolbar = gtk_toolbar_new();
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), GTK_TOOLBAR_ICONS);

    /* --- folder area ---------------------------------------------------- */
    add_tool_button(lw, toolbar, "sidebar", "\xe2\x97\xa7",
                    "Folders", "Show or hide the folder pane",
                    G_CALLBACK(on_toggle_sidebar));
    add_tool_button(lw, toolbar, "new-folder", "+\xf0\x9f\x93\x81",
                    "New Folder", "Create a folder inside the selection",
                    G_CALLBACK(on_new_folder));
    add_tool_button(lw, toolbar, "delete-folder", "\xe2\x9c\x95",
                    "Delete Folder",
                    "Move the selected folder to the Trash",
                    G_CALLBACK(on_delete_folder));
    /* Rename lives in the folder's right-click menu only.                  */

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    /* --- notes area ------------------------------------------------------*/
    add_tool_button(lw, toolbar, "file", "+", "New Note",
                    "Create a note in the current folder",
                    G_CALLBACK(on_new_note));
    add_tool_button(lw, toolbar, "archive", "\xe2\x9a\xa1", "Quicknote",
                    "Create a note in the root folder",
                    G_CALLBACK(on_quicknote));
    add_tool_button(lw, toolbar, "delete", "\xe2\x9c\x95",
                    "Delete Note",
                    "Move the selected notes to the Trash",
                    G_CALLBACK(on_delete_note));
    add_tool_button(lw, toolbar, "view", "\xe2\x8a\x9e",
                    "List/Grid", "Toggle between list and grid view",
                    G_CALLBACK(on_toggle_view));
    add_tool_button(lw, toolbar, "images", "\xf0\x9f\x96\xbc",
                    "Media",
                    "Show every image in the listed notes as thumbnails",
                    G_CALLBACK(on_open_media));

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    /* --- app actions ------------------------------------------------------*/
    add_tool_button(lw, toolbar, "settings", "\xe2\x9a\x99",
                    "Settings", "Open the settings window",
                    G_CALLBACK(on_open_settings));
    add_tool_button(lw, toolbar, "search", "\xf0\x9f\x94\x8d",
                    "Search", "Open search window",
                    G_CALLBACK(on_open_search));

    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    lw->ai_btn = on_app_tool_item_new(lw->app, FALSE,
        "microchip", "\xf0\x9f\xa4\x96",
        "AI Summary", "Summarize notes with AI");
    g_signal_connect(lw->ai_btn, "clicked",
                     G_CALLBACK(on_ai_button_clicked), lw);
    /* no_show_all: visibility is controlled entirely by ai_enabled; we don't
     * want gtk_widget_show_all() on the window to unhide it.                */
    gtk_widget_set_no_show_all(GTK_WIDGET(lw->ai_btn), TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), lw->ai_btn, -1);
    if (lw->app->ai_enabled)
        gtk_widget_show(GTK_WIDGET(lw->ai_btn));

    /* --- right edge: the query entry -------------------------------------
     * An expanding blank spacer pushes the rest of the toolbar left, the
     * same recipe the editor uses for its find-in-note entry.  Enter in the
     * entry opens the search window on the query (All Notes, case
     * insensitive); the Search button opens it empty as before.            */
    GtkToolItem *spacer = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(spacer), FALSE);
    gtk_tool_item_set_expand(spacer, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spacer, -1);

    GtkWidget *entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search all notes");
    gtk_widget_set_tooltip_text(entry,
        "Search every note for this text (Enter)");
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 18);
    /* 5 px of air between the entry and the window edge.                    */
    gtk_widget_set_margin_end(entry, 5);
    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_toolbar_search_activate), lw);

    GtkToolItem *entry_item = gtk_tool_item_new();
    gtk_container_add(GTK_CONTAINER(entry_item), entry);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), entry_item, -1);

    return toolbar;
}

/* ---------------------------------------------------------------------------
 * on_library_key_press() — window-level shortcuts: Ctrl/Cmd+N creates a
 * note in the currently selected folder; Ctrl/Cmd+F opens the search
 * window; Ctrl/Cmd+M opens the media browser.
 * ------------------------------------------------------------------------- */
static gboolean
on_library_key_press(GtkWidget *widget, GdkEventKey *event,
                     gpointer user_data)
{
    (void)widget;
    OnLibrary *lw = user_data;       /* owning library window               */
    if (event->state & (GDK_CONTROL_MASK | GDK_META_MASK)) {
        guint key = gdk_keyval_to_lower(event->keyval);
        if (key == GDK_KEY_n) {
            on_new_note(NULL, lw);
            return TRUE;
        }
        if (key == GDK_KEY_f) {
            on_open_search(NULL, lw);
            return TRUE;
        }
        if (key == GDK_KEY_m) {
            on_open_media(NULL, lw);
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * sidebar_name_cell_func() — bold the sidebar's section rows (the Notes
 * root, the Tags header, and Pinned Notes); folders and tags render at
 * normal weight.  Runs per row draw, keyed on SB_KIND.
 * ------------------------------------------------------------------------- */
static void
sidebar_name_cell_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                       GtkTreeModel *model, GtkTreeIter *iter,
                       gpointer user_data)
{
    (void)col; (void)user_data;
    gint kind;                       /* SB_KIND_* of this row               */
    gtk_tree_model_get(model, iter, SB_KIND, &kind, -1);
    g_object_set(cell, "weight",
                 sb_kind_is_section(kind) ? PANGO_WEIGHT_BOLD
                                          : PANGO_WEIGHT_NORMAL, NULL);
}

/* library_free() — destructor for the OnLibrary attached to the window.     */
static void
library_free(gpointer data)
{
    OnLibrary *lw = data;
    if (lw->status_timeout != 0)
        g_source_remove(lw->status_timeout);
    /* Cancel any in-flight AI subprocess before freeing lw.  The
     * GCancellable keeps the callback safe after the pointer is gone.    */
    ai_throbber_stop(lw);
    if (lw->ai_cancel != NULL) {
        g_cancellable_cancel(lw->ai_cancel);
        g_clear_object(&lw->ai_cancel);
    }
    thumb_pending_clear(lw);
    g_hash_table_destroy(lw->thumb_cache);
    if (lw->folder_path_cache != NULL)
        g_hash_table_destroy(lw->folder_path_cache);
    if (lw->notes_press_path != NULL)
        gtk_tree_path_free(lw->notes_press_path);
    g_free(lw->sel_name);
    g_free(lw);
}

/* sb_fit_ctx — working state passed through gtk_tree_model_foreach() for
 * the sidebar width measurement walk.                                       */
typedef struct {
    PangoLayout *lay;   /* reused layout (same font as the sidebar)          */
    gint         max_w; /* running maximum row pixel width                   */
} SbFitCtx;

/* sb_fit_measure() — foreach callback: measure one sidebar row and update
 * the running maximum in ctx->max_w.
 *   model / path / iter — standard foreach signature.
 *   data                — SbFitCtx *.
 * Returns FALSE to continue the walk.                                       */
static gboolean
sb_fit_measure(GtkTreeModel *model, GtkTreePath *path,
               GtkTreeIter *iter, gpointer data)
{
    SbFitCtx *ctx  = data;
    gchar    *name = NULL;
    gint      kind;
    gtk_tree_model_get(model, iter, SB_NAME, &name, SB_KIND, &kind, -1);
    if (name && *name) {
        PangoAttrList *al = pango_attr_list_new();
        if (sb_kind_is_section(kind))
            pango_attr_list_insert(al,
                pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        pango_layout_set_attributes(ctx->lay, al);
        pango_attr_list_unref(al);
        pango_layout_set_text(ctx->lay, name, -1);

        gint tw, th;
        pango_layout_get_pixel_size(ctx->lay, &tw, &th);

        /* 22 px per depth level covers the GTK3 expander column width +
         * level-indentation; 10 px base for cell left/right padding.        */
        gint depth = gtk_tree_path_get_depth(path);
        gint row_w = tw + depth * 22 + 10;
        if (row_w > ctx->max_w)
            ctx->max_w = row_w;
    }
    g_free(name);
    return FALSE;
}

/* on_sidebar_fit_to_content() — idle callback: set the paned divider to the
 * sidebar tree view's content width, measured with Pango so that ellipsizing
 * on the cell renderer doesn't cause get_preferred_width() to return a tiny
 * value.  Runs once after the window is realized and the model is populated. */
static gboolean
on_sidebar_fit_to_content(gpointer user_data)
{
    OnLibrary *lw = user_data;
    SbFitCtx ctx;
    ctx.lay   = gtk_widget_create_pango_layout(GTK_WIDGET(lw->sidebar), NULL);
    ctx.max_w = 160;   /* floor: never collapse the sidebar to unusable width */
    gtk_tree_model_foreach(GTK_TREE_MODEL(lw->sidebar_store),
                           sb_fit_measure, &ctx);
    g_object_unref(ctx.lay);
    gtk_paned_set_position(GTK_PANED(lw->sidebar_paned), ctx.max_w);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * library_build_sidebar() — build lw->sidebar (GtkTreeView) and
 * lw->sidebar_box (its scroll container), ready to be packed into the paned.
 * ------------------------------------------------------------------------- */
static void
library_build_sidebar(OnLibrary *lw)
{
    lw->sidebar = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->sidebar_store)));
    gtk_tree_view_set_headers_visible(lw->sidebar, FALSE);
    /* No GTK type-ahead popup: set_model auto-picks the first column
     * transformable to string as the search column — here the int id,
     * so typing raised a search box that matched nothing.               */
    gtk_tree_view_set_enable_search(lw->sidebar, FALSE);
    {
        /* Ellipsizing names keeps the pane's MINIMUM width small: without
         * it the widest row dictates the minimum and the divider can't be
         * dragged past it.                                                 */
        GtkCellRenderer *name_cell = gtk_cell_renderer_text_new();
        g_object_set(name_cell,
                     "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *name_col =
            gtk_tree_view_column_new_with_attributes(
                "Name", name_cell, "text", SB_NAME, NULL);
        /* Section rows (Notes root, Tags, Pinned Notes) render bold.       */
        gtk_tree_view_column_set_cell_data_func(
            name_col, name_cell, sidebar_name_cell_func, NULL, NULL);
        gtk_tree_view_column_set_sizing(name_col,
                                        GTK_TREE_VIEW_COLUMN_AUTOSIZE);
        gtk_tree_view_append_column(lw->sidebar, name_col);
    }

    /* Sidebar palette: the backdrop (rows AND the empty area below them —
     * the tree view paints the whole widget) is the theme's window/toolbar
     * background taken down a step, so the pane sits just behind the
     * toolbar above it and reads as distinct from the white notes list
     * without pinning a grey of its own.  A tree view left alone would
     * paint the white theme BASE colour instead.  Both CSS colour
     * functions work from this widget-scoped provider (verified on GTK
     * 3.24 / Adwaita: @theme_bg_color = rgb(246,245,244), exactly what the
     * toolbar renders, and shade(…, 0.96) = rgb(238,236,234)); beware that
     * an UNDEFINED colour name is NOT a parse error here — it silently
     * renders transparent.  Then muted grey text and a blue selection bar
     * (white text for contrast).                                           */
    on_app_widget_add_css(GTK_WIDGET(lw->sidebar),
        "treeview.view {"
        "  background-color: shade(@theme_bg_color, " SB_BG_SHADE ");"
        "  color: rgb(65,65,65);"
        "}"
        "treeview.view:selected {"
        "  background-color: rgb(86,131,224);"
        "  color: white;"
        "}"
        /* Drop indicator (drawn as the border of the row under the
         * pointer, state :drop(active) + a position class): a 2px
         * line in the selection blue — top edge for BEFORE, bottom
         * for AFTER, a full box for INTO.                                 */
        "treeview.view:drop(active) {"
        "  border-color: rgb(86,131,224);"
        "  border-width: 2px;"
        "  border-style: solid;"
        "}"
        "treeview.view:drop(active).before {"
        "  border-style: solid none none none;"
        "}"
        "treeview.view:drop(active).after {"
        "  border-style: none none solid none;"
        "}");

    GtkTreeSelection *sb_sel = gtk_tree_view_get_selection(lw->sidebar);
    gtk_tree_selection_set_select_function(sb_sel, sidebar_select_func,
                                           NULL, NULL);
    g_signal_connect(sb_sel, "changed",
                     G_CALLBACK(on_sidebar_selection_changed), lw);

    /* Accept dragged note rows to move notes between folders, and let
     * folder rows be dragged to re-nest/reorder them.  The dest protocol
     * is fully custom (motion answers the status itself; only the drop
     * requests the row data) — see quirk #13.                              */
    gtk_tree_view_enable_model_drag_dest(lw->sidebar, &ROW_TARGET, 1,
                                         GDK_ACTION_MOVE);
    gtk_tree_view_enable_model_drag_source(lw->sidebar, GDK_BUTTON1_MASK,
                                           &ROW_TARGET, 1,
                                           GDK_ACTION_MOVE);
    g_signal_connect(lw->sidebar, "drag-motion",
                     G_CALLBACK(on_sidebar_drag_motion), lw);
    g_signal_connect(lw->sidebar, "drag-leave",
                     G_CALLBACK(on_sidebar_drag_leave), NULL);
    g_signal_connect(lw->sidebar, "drag-drop",
                     G_CALLBACK(on_sidebar_drag_drop), lw);
    g_signal_connect(lw->sidebar, "drag-data-received",
                     G_CALLBACK(on_sidebar_drag_received), lw);
    /* AFTER: the class handler sets a row-snapshot icon; ours overrides.   */
    g_signal_connect_after(lw->sidebar, "drag-begin",
                           G_CALLBACK(on_sidebar_drag_begin), lw);
    g_signal_connect(lw->sidebar, "button-press-event",
                     G_CALLBACK(on_sidebar_button_press), lw);

    GtkWidget *sidebar_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(sidebar_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(sidebar_scroll),
                      GTK_WIDGET(lw->sidebar));

    /* Sidebar column: a fixed spacer, then the tree (all buttons live in
     * the unified toolbar above the paned).  Its minimum width is whatever
     * the tree content needs — the scrolled window never scrolls
     * horizontally, so it requests the tree's full natural width.          */
    GtkWidget *sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    /* Top padding, so the first row's text sits level with the text in the
     * notes list's column headers (the sidebar has no headers of its own).
     * It is a SPACER WIDGET rather than CSS padding: GtkScrolledWindow
     * ignores padding when allocating its child, and a margin on the tree
     * view would scroll away with it.  Painted in the sidebar grey so the
     * strip reads as part of the pane.  A GtkBox has no background of its
     * own, so it repeats the tree view's backdrop expression verbatim —
     * keep the two in step.                                                */
    GtkWidget *sidebar_pad = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(sidebar_pad, -1, SB_TOP_PAD);
    on_app_widget_add_css(sidebar_pad,
        "box { background-color: shade(@theme_bg_color, "
        SB_BG_SHADE "); }");
    gtk_box_pack_start(GTK_BOX(sidebar_box), sidebar_pad, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar_box), sidebar_scroll,
                       TRUE, TRUE, 0);
    lw->sidebar_box = sidebar_box;   /* for the toolbar show/hide toggle    */
}

/* ---------------------------------------------------------------------------
 * library_build_notes_list() — build lw->notes_list (GtkTreeView) with its
 * four columns, sort functions, column layout, and DnD; returns the scroll
 * container ready to be added to the notes stack.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_notes_list(OnLibrary *lw)
{
    lw->notes_list = GTK_TREE_VIEW(
        gtk_tree_view_new_with_model(GTK_TREE_MODEL(lw->notes_store)));
    /* No GTK type-ahead popup (auto-picked search column, see quirk 16).  */
    gtk_tree_view_set_enable_search(lw->notes_list, FALSE);

    /* Same drop-indicator styling as the sidebar: a 2px line in the
     * selection blue for the in-list reorder drag.  Notes are a flat
     * list, so there is no real "into" — GTK still reports INTO_OR_*
     * over the middle of a row, but a list-store drop there INSERTS
     * BEFORE that row, so the .into indicator is drawn as the same
     * between-rows line (top edge), never a box.                          */
    on_app_widget_add_css(GTK_WIDGET(lw->notes_list),
        "treeview.view:drop(active) {"
        "  border-color: rgb(86,131,224);"
        "  border-width: 2px;"
        "  border-style: solid none none none;"
        "}"
        "treeview.view:drop(active).after {"
        "  border-style: none none solid none;"
        "}");
    {
        /* Title: no static attribute binding — the cell data function drives
         * both the row tint and the compact/comfortable rendering.          */
        GtkCellRenderer *r1 = gtk_cell_renderer_text_new();
        g_object_set(r1, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *c1 = gtk_tree_view_column_new();
        gtk_tree_view_column_set_title(c1, "Title");
        gtk_tree_view_column_pack_start(c1, r1, TRUE);
        gtk_tree_view_column_set_cell_data_func(c1, r1,
                                                notes_title_cell_func,
                                                lw, NULL);
        gtk_tree_view_column_set_resizable(c1, TRUE);
        gtk_tree_view_append_column(lw->notes_list, c1);

        /* Path column: the note's folder location ("/Work/Projects").
         * Fixed width + ellipsize so deep trees can't blow the layout
         * out; the divider is user-draggable like the others.             */
        GtkCellRenderer *rp = gtk_cell_renderer_text_new();
        g_object_set(rp,
                     "ellipsize", PANGO_ELLIPSIZE_END,
                     "xpad",      10,
                     NULL);
        GtkTreeViewColumn *cp =
            gtk_tree_view_column_new_with_attributes("Path", rp,
                                                     "text", NL_PATH,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(cp, rp, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_sizing(cp, GTK_TREE_VIEW_COLUMN_FIXED);
        gtk_tree_view_column_set_fixed_width(cp, 180);
        gtk_tree_view_column_set_resizable(cp, TRUE);
        gtk_tree_view_append_column(lw->notes_list, cp);

        GtkCellRenderer *r2 = gtk_cell_renderer_text_new();
        /* Horizontal padding so the timestamps don't hug the column
         * edges.                                                           */
        g_object_set(r2, "xpad", 10, NULL);
        GtkTreeViewColumn *c2 =
            gtk_tree_view_column_new_with_attributes("Modified", r2,
                                                     "text", NL_MODIFIED,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(c2, r2, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_resizable(c2, TRUE);
        gtk_tree_view_append_column(lw->notes_list, c2);

        /* Created column — built HIDDEN: a saved list_columns without a
         * "created" entry (every pre-existing ini) keeps the built state,
         * so the column stays off until toggled in the header menu.        */
        GtkCellRenderer *rc = gtk_cell_renderer_text_new();
        g_object_set(rc, "xpad", 10, NULL);
        GtkTreeViewColumn *cc =
            gtk_tree_view_column_new_with_attributes("Created", rc,
                                                     "text", NL_CREATED,
                                                     NULL);
        gtk_tree_view_column_set_cell_data_func(cc, rc, notes_row_bg_func,
                                                NULL, NULL);
        gtk_tree_view_column_set_resizable(cc, TRUE);
        gtk_tree_view_column_set_visible(cc, FALSE);
        gtk_tree_view_append_column(lw->notes_list, cc);
        gtk_tree_view_column_set_expand(c1, TRUE);

        /* Clickable headers: Title sorts alphabetically, Modified sorts
         * most-recent-first (drag reordering works while unsorted).        */
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_TITLE,
            sort_by_title, NULL, NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_UPDATED,
            sort_by_time, GINT_TO_POINTER(NL_UPDATED), NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_PATH,
            sort_by_path, NULL, NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->notes_store), NL_CREATED_RAW,
            sort_by_time, GINT_TO_POINTER(NL_CREATED_RAW), NULL);
        gtk_tree_view_column_set_sort_column_id(c1, NL_TITLE);
        gtk_tree_view_column_set_sort_column_id(cp, NL_PATH);
        gtk_tree_view_column_set_sort_column_id(c2, NL_UPDATED);
        gtk_tree_view_column_set_sort_column_id(cc, NL_CREATED_RAW);

        /* Default sort: Modified with the most recent on top
         * (sort_by_time is deliberately inverted, so ASCENDING =
         * newest first).  While any sort is active the list store
         * refuses in-list row drops, so manual drag-reordering of notes
         * is off in this mode — moves to folders are unaffected.          */
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(lw->notes_store), NL_UPDATED,
            GTK_SORT_ASCENDING);

        /* Column layout management: drag a header to reorder, right-click
         * one for the show/hide menu; the layout persists in the ini
         * (applied below once all columns exist).  Each column carries
         * its renderer so autofit can move the ellipsis around.           */
        struct { GtkTreeViewColumn *col; GtkCellRenderer *cell;
                 const gchar *key; } COLS[N_LIST_COLUMNS] = {
            { c1, r1, "title" }, { cp, rp, "path" }, { c2, r2, "modified" },
            { cc, rc, "created" },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(COLS); i++) {
            g_object_set_data(G_OBJECT(COLS[i].col), "on-colkey",
                              (gpointer)COLS[i].key);
            g_object_set_data(G_OBJECT(COLS[i].col), "on-cell",
                              COLS[i].cell);
            gtk_tree_view_column_set_reorderable(COLS[i].col, TRUE);
            GtkWidget *btn = gtk_tree_view_column_get_button(COLS[i].col);
            g_object_set_data(G_OBJECT(btn), "on-view", lw->notes_list);
            g_signal_connect(btn, "button-press-event",
                             G_CALLBACK(on_column_header_press), lw);
        }
    }
    g_object_set_data(G_OBJECT(lw->notes_list), "on-colcfg",
                      (gpointer)"list_columns");
    g_object_set_data(G_OBJECT(lw->notes_list), "on-ncols",
                      GINT_TO_POINTER(N_LIST_COLUMNS));
    g_object_set_data(G_OBJECT(lw->notes_list), "on-coldefault",
                      (gpointer)"path:0,title:1,modified:1,created:0");
    view_columns_apply(lw->notes_list);  /* saved order + visibility        */
    {
        /* Autofit is the default; only an explicit "0" turns it off.       */
        lw->list_autofit = on_app_config_get_bool("list_autofit", TRUE);
        if (lw->list_autofit)
            list_autofit_apply(lw);
    }
    /* Connected only now, so applying the saved layout doesn't re-persist
     * it; from here on every header drag writes the ini.                   */
    g_signal_connect(lw->notes_list, "columns-changed",
                     G_CALLBACK(on_view_columns_changed), lw);
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(lw->notes_list),
        GTK_SELECTION_MULTIPLE);
    g_signal_connect(gtk_tree_view_get_selection(lw->notes_list),
                     "changed",
                     G_CALLBACK(on_notes_selection_status), lw);

    /* Built-in drag reordering; the new order is persisted from the
     * model's row-deleted signal.                                          */
    gtk_tree_view_set_reorderable(lw->notes_list, TRUE);
    /* AFTER: the class handler sets a row-snapshot icon; ours overrides.   */
    g_signal_connect_after(lw->notes_list, "drag-begin",
                           G_CALLBACK(on_notes_drag_begin), lw);
    g_signal_connect(lw->notes_list, "row-activated",
                     G_CALLBACK(on_note_list_activated), lw);
    g_signal_connect(lw->notes_list, "button-press-event",
                     G_CALLBACK(on_notes_list_button_press), lw);
    g_signal_connect(lw->notes_list, "button-release-event",
                     G_CALLBACK(on_notes_list_button_release), lw);

    GtkWidget *list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(list_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(list_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(list_scroll),
                      GTK_WIDGET(lw->notes_list));
    return list_scroll;
}

/* ---------------------------------------------------------------------------
 * library_build_notes_grid() — build lw->notes_grid (GtkIconView) with its
 * HiDPI thumbnail + title cell layout and DnD; returns the scroll container.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_notes_grid(OnLibrary *lw)
{
    lw->notes_grid = GTK_ICON_VIEW(
        gtk_icon_view_new_with_model(GTK_TREE_MODEL(lw->notes_store)));
    {
        /* Custom cell layout: the HiDPI thumbnail surface with the note
         * title as a real text label underneath.                           */
        GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(lw->notes_grid),
                                   pix, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(lw->notes_grid),
                                       pix, "surface", NL_THUMB, NULL);

        GtkCellRenderer *txt = gtk_cell_renderer_text_new();
        g_object_set(txt,
                     "xalign",      0.5,
                     "alignment",   PANGO_ALIGN_CENTER,
                     "wrap-mode",   PANGO_WRAP_WORD_CHAR,
                     "wrap-width",  THUMB_SIZE,
                     "weight",      PANGO_WEIGHT_BOLD,
                     NULL);
        gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(lw->notes_grid),
                                   txt, FALSE);
        gtk_cell_layout_set_attributes(GTK_CELL_LAYOUT(lw->notes_grid),
                                       txt, "text", NL_TITLE, NULL);
    }
    gtk_icon_view_set_item_width(lw->notes_grid, THUMB_SIZE);
    gtk_icon_view_set_selection_mode(lw->notes_grid,
                                     GTK_SELECTION_MULTIPLE);
    g_signal_connect(lw->notes_grid, "selection-changed",
                     G_CALLBACK(on_notes_selection_status), lw);
    gtk_icon_view_enable_model_drag_source(lw->notes_grid,
                                           GDK_BUTTON1_MASK,
                                           &ROW_TARGET, 1,
                                           GDK_ACTION_MOVE);
    g_signal_connect_after(lw->notes_grid, "drag-begin",
                           G_CALLBACK(on_notes_drag_begin), lw);
    g_signal_connect(lw->notes_grid, "item-activated",
                     G_CALLBACK(on_note_grid_activated), lw);
    g_signal_connect(lw->notes_grid, "button-press-event",
                     G_CALLBACK(on_notes_grid_button_press), lw);

    GtkWidget *grid_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(grid_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(grid_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(grid_scroll),
                      GTK_WIDGET(lw->notes_grid));
    return grid_scroll;
}

/* ---------------------------------------------------------------------------
 * library_build_actions_view() — build lw->actions_store and lw->actions_view
 * with their three columns, sort functions, and column layout; returns the
 * scroll container ready to be added to the notes stack.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_actions_view(OnLibrary *lw)
{
    lw->actions_store = gtk_list_store_new(AL_N_COLS,
                                           G_TYPE_INT64,   /* AL_NOTE_ID   */
                                           G_TYPE_INT,     /* AL_ORD       */
                                           G_TYPE_BOOLEAN, /* AL_DONE      */
                                           G_TYPE_STRING,  /* AL_TEXT      */
                                           G_TYPE_STRING,  /* AL_DUE       */
                                           G_TYPE_INT64);  /* AL_DUE_RAW   */
    lw->actions_view = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(lw->actions_store)));
    gtk_tree_view_set_enable_search(lw->actions_view, FALSE); /* quirk 16   */
    {
        /* Untitled checkbox column + the item text + the due date; done
         * rows also render struck through, matching the editor.            */
        GtkCellRenderer *tog = gtk_cell_renderer_toggle_new();
        g_signal_connect(tog, "toggled",
                         G_CALLBACK(on_action_toggled), lw);
        GtkTreeViewColumn *cd = gtk_tree_view_column_new_with_attributes(
            "", tog, "active", AL_DONE, NULL);
        gtk_tree_view_append_column(lw->actions_view, cd);

        GtkCellRenderer *txt = gtk_cell_renderer_text_new();
        g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, "xpad", 10,
                     NULL);
        GtkTreeViewColumn *ca = gtk_tree_view_column_new_with_attributes(
            "Action", txt,
            "text",          AL_TEXT,
            "strikethrough", AL_DONE,
            NULL);
        gtk_tree_view_column_set_expand(ca, TRUE);
        gtk_tree_view_column_set_resizable(ca, TRUE);
        gtk_tree_view_append_column(lw->actions_view, ca);

        GtkCellRenderer *rdue = gtk_cell_renderer_text_new();
        g_object_set(rdue, "xpad", 10, NULL);
        GtkTreeViewColumn *cdue = gtk_tree_view_column_new_with_attributes(
            "Due Date", rdue,
            "text",          AL_DUE,
            "strikethrough", AL_DONE,
            NULL);
        gtk_tree_view_column_set_cell_data_func(cdue, rdue,
            action_due_color_func, NULL, NULL);
        gtk_tree_view_column_set_resizable(cdue, TRUE);
        gtk_tree_view_append_column(lw->actions_view, cdue);

        /* Clickable headers, notes-list style.  AL_DONE sorts with the
         * default boolean compare.                                         */
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->actions_store), AL_TEXT,
            sort_actions_by_text, NULL, NULL);
        gtk_tree_sortable_set_sort_func(
            GTK_TREE_SORTABLE(lw->actions_store), AL_DUE_RAW,
            sort_actions_by_due, NULL, NULL);
        gtk_tree_view_column_set_sort_column_id(cd, AL_DONE);
        gtk_tree_view_column_set_sort_column_id(ca, AL_TEXT);
        gtk_tree_view_column_set_sort_column_id(cdue, AL_DUE_RAW);

        /* Column layout: the notes list's conventions — drag a header to
         * reorder, right-click for show/hide; persists per view.           */
        struct { GtkTreeViewColumn *col; const gchar *key;
                 const gchar *label; } ACOLS[N_ACTION_COLUMNS] = {
            { cd,   "done",   "Done" },   /* titleless: needs a menu label */
            { ca,   "action", NULL },
            { cdue, "due",    NULL },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(ACOLS); i++) {
            g_object_set_data(G_OBJECT(ACOLS[i].col), "on-colkey",
                              (gpointer)ACOLS[i].key);
            if (ACOLS[i].label != NULL)
                g_object_set_data(G_OBJECT(ACOLS[i].col), "on-collabel",
                                  (gpointer)ACOLS[i].label);
            gtk_tree_view_column_set_reorderable(ACOLS[i].col, TRUE);
            GtkWidget *btn =
                gtk_tree_view_column_get_button(ACOLS[i].col);
            g_object_set_data(G_OBJECT(btn), "on-view", lw->actions_view);
            g_signal_connect(btn, "button-press-event",
                             G_CALLBACK(on_column_header_press), lw);
        }
    }
    g_object_set_data(G_OBJECT(lw->actions_view), "on-colcfg",
                      (gpointer)"action_columns");
    g_object_set_data(G_OBJECT(lw->actions_view), "on-ncols",
                      GINT_TO_POINTER(N_ACTION_COLUMNS));
    g_object_set_data(G_OBJECT(lw->actions_view), "on-coldefault",
                      (gpointer)"done:1,action:1,due:1");
    view_columns_apply(lw->actions_view);
    g_signal_connect(lw->actions_view, "columns-changed",
                     G_CALLBACK(on_view_columns_changed), lw);
    g_signal_connect(lw->actions_view, "row-activated",
                     G_CALLBACK(on_action_row_activated), lw);

    GtkWidget *actions_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(actions_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_overlay_scrolling(
        GTK_SCROLLED_WINDOW(actions_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(actions_scroll),
                      GTK_WIDGET(lw->actions_view));
    return actions_scroll;
}

/* ---------------------------------------------------------------------------
 * library_build_notes_pane() — build the three note views (list, grid,
 * actions) and assemble them into lw->stack, ready to be packed into the
 * notes paned.
 * ------------------------------------------------------------------------- */
static void
library_build_notes_pane(OnLibrary *lw)
{
    GtkWidget *list_scroll    = library_build_notes_list(lw);
    GtkWidget *grid_scroll    = library_build_notes_grid(lw);
    GtkWidget *actions_scroll = library_build_actions_view(lw);

    /* --- stack: list <-> grid <-> action items ----------------------------*/
    lw->stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(lw->stack), list_scroll,    "list");
    gtk_stack_add_named(GTK_STACK(lw->stack), grid_scroll,    "grid");
    gtk_stack_add_named(GTK_STACK(lw->stack), actions_scroll, "actions");
    gtk_stack_set_visible_child_name(GTK_STACK(lw->stack), "list");
}

/* ---------------------------------------------------------------------------
 * library_build_status_bar() — build the status bar with lw->status_path
 * (left) and lw->status_event in lw->status_revealer (right); returns the
 * assembled box widget.
 * ------------------------------------------------------------------------- */
static GtkWidget *
library_build_status_bar(OnLibrary *lw)
{
    /* --- status bar: selection path (left) + latest event (right) ----------*/
    lw->status_path = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lw->status_path), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_path),
                            PANGO_ELLIPSIZE_MIDDLE);

    lw->status_event = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(lw->status_event), 1.0);
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_event),
                            PANGO_ELLIPSIZE_MIDDLE);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(lw->status_event), "dim-label");

    /* Event messages fade: the label sits in a crossfading revealer that
     * library_notify_status() opens and a timer closes.                     */
    lw->status_revealer = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(lw->status_revealer),
                                     GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_revealer_set_transition_duration(GTK_REVEALER(lw->status_revealer),
                                         600);
    gtk_container_add(GTK_CONTAINER(lw->status_revealer), lw->status_event);

    GtkWidget *status_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status_bar, 8);
    gtk_widget_set_margin_end(status_bar, 8);
    gtk_widget_set_margin_top(status_bar, 3);
    gtk_widget_set_margin_bottom(status_bar, 3);
    gtk_box_pack_start(GTK_BOX(status_bar), lw->status_path,
                       TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(status_bar), lw->status_revealer,
                     FALSE, FALSE, 0);

    /* Both labels a step smaller than the UI font.                          */
    on_app_widget_add_css(lw->status_path,  "label { font-size: 85%; }");
    on_app_widget_add_css(lw->status_event, "label { font-size: 85%; }");

    return status_bar;
}

/* ---------------------------------------------------------------------------
 * on_library_window_create() — build and show the library window.
 * Creates the OnLibrary state, models, and sub-panes via the builder helpers
 * above, assembles the layout, and triggers the initial data load.
 * ------------------------------------------------------------------------- */
GtkWidget *
on_library_window_create(OnApp *app)
{
    OnLibrary *lw = g_new0(OnLibrary, 1);
    lw->app      = app;
    lw->sel_kind = SB_KIND_ROOT;
    lw->sel_id   = 0;
    lw->sel_name = g_strdup("Notes");
    lw->thumb_cache = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, thumb_entry_free);

    /* --- window (standard titlebar, no HeaderBar) ------------------------*/
    lw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(lw->window), "Notes - Library");
    gtk_window_set_default_size(GTK_WINDOW(lw->window), 900, 620);
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(lw->window));
    g_object_set_data_full(G_OBJECT(lw->window), "on-library", lw,
                           library_free);

    app->library_window       = lw->window;
    app->notify_notes_changed = library_notify_notes_changed;
    app->notify_note_saved    = library_notify_note_saved;
    app->notify_status        = library_notify_status;
    app->notify_ai_changed    = library_notify_ai_changed;

    g_signal_connect(lw->window, "key-press-event",
                     G_CALLBACK(on_library_key_press), lw);

    /* --- models -----------------------------------------------------------*/
    lw->sidebar_store = gtk_tree_store_new(SB_N_COLS,
                                           G_TYPE_INT,     /* SB_KIND      */
                                           G_TYPE_INT64,   /* SB_ID        */
                                           G_TYPE_STRING,  /* SB_NAME      */
                                           G_TYPE_STRING); /* SB_RAW       */
    lw->notes_store = gtk_list_store_new(
        NL_N_COLS,
        G_TYPE_INT64,                    /* NL_ID                          */
        G_TYPE_STRING,                   /* NL_TITLE                       */
        G_TYPE_STRING,                   /* NL_MODIFIED                    */
        CAIRO_GOBJECT_TYPE_SURFACE,      /* NL_THUMB                       */
        G_TYPE_INT64,                    /* NL_UPDATED                     */
        G_TYPE_STRING,                   /* NL_PATH                        */
        G_TYPE_STRING,                   /* NL_CREATED                     */
        G_TYPE_INT64,                    /* NL_CREATED_RAW                 */
        G_TYPE_STRING);                  /* NL_PREVIEW                     */
    g_signal_connect(lw->notes_store, "row-deleted",
                     G_CALLBACK(on_notes_row_deleted), lw);

    /* --- panes + status bar -----------------------------------------------*/
    library_build_sidebar(lw);           /* sets lw->sidebar, lw->sidebar_box */
    library_build_notes_pane(lw);        /* sets lw->notes_list, lw->notes_grid,
                                          * lw->actions_view, lw->stack       */
    GtkWidget *status_bar = library_build_status_bar(lw);

    /* --- assemble -----------------------------------------------------------*/
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    lw->sidebar_paned = paned;
    /* A 6 px divider: wide-handle switches GtkPaned off its hairline style,
     * and the exact width comes from CSS on the handle's own `separator`
     * node (this paned is horizontal, so its separator is vertical and
     * min-WIDTH is the lever).                                             */
    gtk_paned_set_wide_handle(GTK_PANED(paned), TRUE);
    on_app_widget_add_css(paned, "paned > separator { min-width: 6px; }");
    gtk_paned_pack1(GTK_PANED(paned), lw->sidebar_box, FALSE, FALSE);
    lw->ai_pane = build_ai_pane(lw);
    /* Vertical paned so the user can drag the divider between the notes list
     * and the AI summary pane.  GtkPaned collapses the divider automatically
     * when child2 is hidden.                                                  */
    GtkWidget *notes_paned = gtk_paned_new(GTK_ORIENTATION_VERTICAL);
    lw->notes_paned = GTK_PANED(notes_paned);
    gtk_paned_pack1(GTK_PANED(notes_paned), lw->stack, TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(notes_paned), lw->ai_pane, FALSE, FALSE);
    gtk_paned_pack2(GTK_PANED(paned), notes_paned, TRUE, FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *menubar = build_menubar(lw);
    /* Remembered so the settings window can move it into the native
     * macOS menu bar (see on_library_apply_native_menubar).                */
    g_object_set_data(G_OBJECT(lw->window), "on-menubar", menubar);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_action_bar(lw),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(lw->window), vbox);

    /* --- initial population -------------------------------------------------*/
    refresh_all(lw);
    on_app_status(app, "DB at %s loaded", app->db->path);

    gtk_widget_show_all(lw->window);

    /* Fit the sidebar pane to its content width on first show.  Done in an
     * idle so the tree view is fully realized and has measured its rows.     */
    g_idle_add(on_sidebar_fit_to_content, lw);

    return lw->window;
}
