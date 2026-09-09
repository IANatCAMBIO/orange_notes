/* ===========================================================================
 * cli.c — command-line automation interface (implementation)
 *
 * See cli.h for the command list.  Commands print plain, tab-separated
 * text (easy to consume from shell scripts) and exit with:
 *   0 — success
 *   1 — usage error (bad command/arguments)
 *   2 — operation failed (missing folder, database error, …)
 *
 * The database is resolved exactly like the GUI resolves it: the custom
 * location from notes.ini (next to the binary) when set, otherwise
 * the default per-user path.
 * =========================================================================== */

#include "cli.h"
#include "app.h"
#include "db.h"
#include "editor_window.h"           /* action done/due content rewrites    */
#include "export.h"
#include "ipc.h"
#include "search_query.h"
#include "serialize.h"

#include <stdio.h>
#include <string.h>

/* Stdin substitute used while a command runs inside a GUI instance on behalf
 * of a remote CLI (see on_cli_set_stdin_data): the instance has no access to
 * the CLI's stdin, so "note new -" reads this pre-slurped text instead.  NULL
 * in the normal headless path, where the real stdin is read directly.        */
static const gchar *cli_stdin_data = NULL;

void
on_cli_set_stdin_data(const gchar *data)
{
    cli_stdin_data = data;
}

/* ===========================================================================
 * machine-readable output
 *
 * Every listing command takes --json and emits the same records as one JSON
 * array instead of tab-separated lines.  The point is that a note title, a
 * folder name or an action item's text may itself contain a tab, which
 * silently shifts the columns of the plain form; JSON has one unambiguous
 * escape for that (and for the newlines "note cat --json" carries).
 *
 * The flag is stripped from argv by the dispatcher (see cli_json_take), so
 * each verb still validates its own argument count.  It is assigned on every
 * dispatch, never OR-ed in: inside a GUI instance serving remote CLI calls
 * these statics outlive the command.
 * =========================================================================== */

static gboolean cli_json   = FALSE;  /* --json: emit JSON, not TSV          */
static gboolean json_first = TRUE;   /* next element needs no leading comma */

/* ---------------------------------------------------------------------------
 * json_str() — print one JSON string literal, quoted and escaped.  The text
 * is UTF-8 (validated on the way into the database), which JSON takes
 * verbatim, so only the quote, the backslash and the C0 controls are
 * rewritten.  A NULL prints as the JSON null.
 * ------------------------------------------------------------------------- */
static void
json_str(const gchar *s)
{
    if (s == NULL) {
        fputs("null", stdout);
        return;
    }
    putchar('"');
    for (const guchar *p = (const guchar *)s; *p != '\0'; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", stdout); break;
        case '\\': fputs("\\\\", stdout); break;
        case '\n': fputs("\\n", stdout);  break;
        case '\r': fputs("\\r", stdout);  break;
        case '\t': fputs("\\t", stdout);  break;
        case '\b': fputs("\\b", stdout);  break;
        case '\f': fputs("\\f", stdout);  break;
        default:
            if (*p < 0x20)
                printf("\\u%04x", *p);
            else
                putchar((gchar)*p);
        }
    }
    putchar('"');
}

/* ---------------------------------------------------------------------------
 * cli_time_str() — THE CLI's timestamp rendering: a UNIX time as local
 * "YYYY-MM-DD HH:MM" (or "%Y-%m-%d" alone for a due date, which carries no
 * time of day).  Every command printed this with its own four-line
 * GDateTime dance before.
 * Returns a newly allocated string; g_free() it.
 * ------------------------------------------------------------------------- */
static gchar *
cli_time_str(gint64 unix_ts, gboolean date_only)
{
    GDateTime *dt = g_date_time_new_from_unix_local(unix_ts);
    gchar *out = g_date_time_format(dt, date_only ? "%Y-%m-%d"
                                                  : "%Y-%m-%d %H:%M");
    g_date_time_unref(dt);
    return out;
}

/* json_time() — print a timestamp as the pair every record carries: the
 * same "YYYY-MM-DD HH:MM" string the plain output shows, under `key`, plus
 * the raw UNIX seconds under "<key>_at" for anything doing arithmetic.     */
static void
json_time(const gchar *key, gint64 unix_ts)
{
    gchar *when = cli_time_str(unix_ts, FALSE);
    printf("\"%s\":", key);
    json_str(when);
    printf(",\"%s_at\":%" G_GINT64_FORMAT, key, unix_ts);
    g_free(when);
}

/* Array framing.  Each is a no-op in plain mode, so a command's printing
 * loop reads the same either way: begin, one element per record, end.      */
static void
json_array_begin(void)
{
    if (cli_json) {
        putchar('[');
        json_first = TRUE;
    }
}

static void
json_element(void)
{
    if (cli_json) {
        if (!json_first)
            putchar(',');
        json_first = FALSE;
    }
}

static void
json_array_end(void)
{
    if (cli_json)
        fputs("]\n", stdout);
}

/* ---------------------------------------------------------------------------
 * cli_open_db() — open the same database the GUI would use.
 * Returns the handle, or NULL after printing an error.
 * ------------------------------------------------------------------------- */
static OnDatabase *
cli_open_db(void)
{
    gchar *db_dir = on_app_config_load_db_dir();
    gchar *path = (db_dir != NULL)
                  ? g_build_filename(db_dir, ON_DB_FILENAME, NULL)
                  : NULL;
    OnDatabase *db = on_db_open(path);
    if (db == NULL) {
        fprintf(stderr, "error: cannot open database%s%s\n",
                path != NULL ? " at " : "", path != NULL ? path : "");
    }
    g_free(path);
    g_free(db_dir);
    on_app_actions_backfill(db);     /* one-time '!'-line index (gated)     */
    on_app_action_uids_backfill(db); /* then give those rows stable ids     */
    return db;
}

/* ---------------------------------------------------------------------------
 * on_cli_resolve_folder_path() — walk "A/B/C" through the folder tree.
 * (See cli.h — exported so the IPC note-path resolver can reuse it.)
 * ------------------------------------------------------------------------- */
gboolean
on_cli_resolve_folder_path(OnDatabase *db, const gchar *path, gboolean create,
                           gint64 *out_id)
{
    *out_id = 0;
    if (path == NULL || *path == '\0' || g_strcmp0(path, "/") == 0)
        return TRUE;

    gchar **parts = g_strsplit(path, "/", -1);
    gint64 parent = 0;               /* id of the folder walked so far      */
    gboolean ok = TRUE;              /* did every component resolve?        */

    for (gsize i = 0; ok && parts[i] != NULL; i++) {
        if (*parts[i] == '\0')
            continue;                /* tolerate leading//trailing slashes  */

        /* Find a child of `parent` with this name.                         */
        gint64 found = 0;            /* matching child folder id            */
        GList *children = on_db_folder_list(db, parent);
        for (GList *l = children; l != NULL; l = l->next) {
            OnFolder *f = l->data;
            if (g_strcmp0(f->name, parts[i]) == 0) {
                found = f->id;
                break;
            }
        }
        on_db_folder_list_free(children);

        if (found == 0 && create)
            found = on_db_folder_create(db, parent, parts[i],
                                        ON_AI_MODE_NORMAL, NULL);
        if (found == 0)
            ok = FALSE;
        parent = found;
    }
    g_strfreev(parts);
    *out_id = parent;
    return ok;
}

/* ---------------------------------------------------------------------------
 * note_from_arg() — parse a note-id argument and fetch its metadata.
 *   db  — open database.
 *   arg — the id string as typed on the command line.
 * Returns an OnNoteMeta* (free with on_db_note_meta_free), or NULL after
 * printing "error: no such note: <arg>" to stderr when the argument is
 * not a positive number or names no note.
 * ------------------------------------------------------------------------- */
static OnNoteMeta *
note_from_arg(OnDatabase *db, const gchar *arg)
{
    gint64 id = g_ascii_strtoll(arg, NULL, 10);
    OnNoteMeta *meta = (id > 0) ? on_db_note_get(db, id) : NULL;
    if (meta == NULL)
        fprintf(stderr, "error: no such note: %s\n", arg);
    return meta;
}

/* ---------------------------------------------------------------------------
 * cli_require_gtk() — initialize GTK (windowless), needed by any command
 * that (de)serializes note content.  Inside a GUI instance running a
 * remote command this is a no-op (GTK is already up).
 * Returns TRUE on success, FALSE after printing an error.
 * ------------------------------------------------------------------------- */
static gboolean
cli_require_gtk(void)
{
    if (gtk_init_check(NULL, NULL))
        return TRUE;
    fprintf(stderr, "error: GTK could not initialize (needed to "
                    "process note content)\n");
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * cli_read_content() — resolve a CLI content argument: the literal text,
 * or stdin when `arg` is "-" (the pre-slurped cli_stdin_data when running
 * inside a GUI instance on behalf of a remote CLI).
 * Returns owned, UTF-8-validated text, or NULL after printing an error.
 * ------------------------------------------------------------------------- */
static gchar *
cli_read_content(const gchar *arg)
{
    gchar *text;                     /* the resolved content (owned)        */
    if (g_strcmp0(arg, "-") == 0) {
        if (cli_stdin_data != NULL) {
            text = g_strdup(cli_stdin_data);
        } else {
            text = on_app_read_stream(stdin, FALSE);
        }
    } else {
        text = g_strdup(arg);
    }
    if (!g_utf8_validate(text, -1, NULL)) {
        fprintf(stderr, "error: content is not valid UTF-8\n");
        g_free(text);
        return NULL;
    }
    return text;
}

/* ---------------------------------------------------------------------------
 * buffer_append_iter() — put `end` at the buffer's end, ready to append,
 * inserting `sep` first when the buffer already holds something and the end
 * is mid-line.  What "append to a note" means in three places: plain text
 * and an image start a fresh line ("\n"), a #tag token only wants a space.
 *   buffer — the note being extended.
 *   end    — receives the append position.
 *   sep    — separator to insert first, or NULL for none.
 * ------------------------------------------------------------------------- */
static void
buffer_append_iter(GtkTextBuffer *buffer, GtkTextIter *end, const gchar *sep)
{
    gtk_text_buffer_get_end_iter(buffer, end);
    if (sep != NULL && gtk_text_buffer_get_char_count(buffer) > 0 &&
        !gtk_text_iter_starts_line(end))
        gtk_text_buffer_insert(buffer, end, sep, -1);
}

/* ---------------------------------------------------------------------------
 * note_buffer_save() — serialize `buffer` and persist it as note `id`'s
 * content: title from the first line, searchable body text refreshed —
 * the same trio every editor save writes.  Returns TRUE on success.
 * ------------------------------------------------------------------------- */
static gboolean
note_buffer_save(OnDatabase *db, gint64 id, GtkTextBuffer *buffer)
{
    gsize blob_len;                  /* serialized content size             */
    guint8 *blob = on_note_serialize(buffer, &blob_len);
    gchar *title = on_buffer_first_line(buffer);
    /* One walk for both derived values.                                   */
    gchar *body = NULL;              /* searchable plain text               */
    GList *actions = NULL;           /* the note's '!' lines                */
    on_note_extract(blob, blob_len, &body, &actions);
    gboolean ok = on_db_note_save(db, id, title, blob, blob_len, body);
    /* CLI saves are rare enough to sync the '!' action-item mirror
     * unconditionally (the editor compares against its last set instead).
     * Propagate failure so callers can report it.                        */
    if (ok)
        ok = on_db_note_set_actions(db, id, actions);
    on_db_action_list_free(actions);
    g_free(body);
    g_free(title);
    g_free(blob);
    return ok;
}

/* ---------------------------------------------------------------------------
 * tag_name_valid() — TRUE when `name` is a non-empty run of the characters
 * a #tag token may contain (the editor's tag_capture_span rule: letters,
 * digits, '_', '-').
 * ------------------------------------------------------------------------- */
static gboolean
tag_name_valid(const gchar *name)
{
    if (name == NULL || *name == '\0')
        return FALSE;
    for (const gchar *p = name; *p != '\0'; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (!(g_unichar_isalnum(c) || c == '_' || c == '-'))
            return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * cmd_list_tags() — one tag per line: "name<TAB>note-count".
 * ------------------------------------------------------------------------- */
static int
cmd_list_tags(OnDatabase *db)
{
    GHashTable *counts = on_db_tag_count_map(db);   /* tag id → note count  */
    GList *tags = on_db_tag_list(db);
    json_array_begin();
    for (GList *l = tags; l != NULL; l = l->next) {
        OnTag *t = l->data;          /* one tag                             */
        gint n = GPOINTER_TO_INT(g_hash_table_lookup(counts, &t->id));
        if (cli_json) {
            json_element();
            printf("{\"id\":%" G_GINT64_FORMAT ",\"name\":", t->id);
            json_str(t->name);
            printf(",\"notes\":%d}", n);
        } else {
            printf("%s\t%d\n", t->name, n);
        }
    }
    json_array_end();
    on_db_tag_list_free(tags);
    g_hash_table_destroy(counts);
    return 0;
}

/* cmd_delete_tag() — remove one tag by name.                                */
static int
cmd_delete_tag(OnDatabase *db, const gchar *name)
{
    /* Accept the name with or without the leading '#'.                     */
    if (*name == '#')
        name++;
    gint64 id = on_db_tag_find(db, name);
    if (id == 0) {
        fprintf(stderr, "error: no such tag: %s\n", name);
        return 2;
    }
    if (!on_db_tag_delete(db, id)) {
        fprintf(stderr, "error: could not delete tag %s\n", name);
        return 2;
    }
    printf("deleted tag %s\n", name);
    return 0;
}

/* Forward declarations (defined with the other note-line printers below). */
static void print_note_line(OnNoteMeta *m, GHashTable *paths);
static void print_note_list(GList *notes, GHashTable *paths);
static GHashTable *cli_paths_for(OnDatabase *db, gboolean want_path);

/* cmd_tag_notes() — list every note labeled with one tag.                   */
static int
cmd_tag_notes(OnDatabase *db, const gchar *name)
{
    /* Accept the name with or without the leading '#'.                     */
    if (*name == '#')
        name++;
    gint64 id = on_db_tag_find(db, name);
    if (id == 0) {
        fprintf(stderr, "error: no such tag: %s\n", name);
        return 2;
    }
    GList *notes = on_db_notes_by_tag(db, id);
    GHashTable *paths = cli_paths_for(db, FALSE);   /* JSON needs the paths */
    print_note_list(notes, paths);
    if (paths != NULL)
        g_hash_table_destroy(paths);
    on_db_note_list_free(notes);
    return 0;
}

/* ---------------------------------------------------------------------------
 * print_folder_tree() — recursive listing with note counts: indented names
 * in plain mode, one flat object per folder in JSON.  The JSON form is
 * deliberately FLAT rather than nested — the tree is already in each
 * folder's "path", and a flat array is what a caller can filter.
 *   counts — folder id → note count map (on_db_note_count_map), fetched
 *            once by cmd_list_folders; a missing key means zero.
 *   parent — folder whose children this call prints (0 = top level).
 *   depth  — recursion depth, driving the plain form's indent.
 *   prefix — the parent's "Folder/Sub" path, "" at the top level.
 * ------------------------------------------------------------------------- */
static void
print_folder_tree(OnDatabase *db, GHashTable *counts, gint64 parent,
                  gint depth, const gchar *prefix)
{
    GList *folders = on_db_folder_list(db, parent);
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one child folder                    */
        gint n = GPOINTER_TO_INT(g_hash_table_lookup(counts, &f->id));
        gchar *path = g_strdup_printf("%s/%s", prefix, f->name);
        if (cli_json) {
            json_element();
            printf("{\"id\":%" G_GINT64_FORMAT ",\"name\":", f->id);
            json_str(f->name);
            printf(",\"path\":");
            json_str(path);
            printf(",\"parent_id\":%" G_GINT64_FORMAT ",\"emoji\":", parent);
            json_str(f->emoji);
            printf(",\"notes\":%d}", n);
        } else {
            printf("%*s%s\t%d\n", depth * 2, "", f->name, n);
        }
        print_folder_tree(db, counts, f->id, depth + 1, path);
        g_free(path);
    }
    on_db_folder_list_free(folders);
}

static int
cmd_list_folders(OnDatabase *db)
{
    GHashTable *counts = on_db_note_count_map(db);  /* one query, not per row */
    json_array_begin();
    print_folder_tree(db, counts, 0, 0, "");
    json_array_end();
    g_hash_table_destroy(counts);
    return 0;
}

/* cmd_add_folder() — create a (possibly nested) folder path.                */
static int
cmd_add_folder(OnDatabase *db, const gchar *path)
{
    gint64 id;                       /* the created/found folder            */
    if (!on_cli_resolve_folder_path(db, path, TRUE, &id) || id == 0) {
        fprintf(stderr, "error: could not create folder %s\n", path);
        return 2;
    }
    printf("folder %s (id %" G_GINT64_FORMAT ")\n", path, id);
    return 0;
}

/* ---------------------------------------------------------------------------
 * folder_from_arg() — resolve a folder-path argument to its id, refusing
 * the root: every command here acts ON a folder, and the root is not one.
 *   db   — open database.
 *   path — the path as typed ("A/B/C").
 * Returns TRUE with *out_id set, or FALSE after printing an error.
 * ------------------------------------------------------------------------- */
static gboolean
folder_from_arg(OnDatabase *db, const gchar *path, gint64 *out_id)
{
    if (!on_cli_resolve_folder_path(db, path, FALSE, out_id) || *out_id == 0) {
        fprintf(stderr, "error: no such folder: %s\n", path);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * ai_mode_name() / ai_mode_parse() — the folder Info dialog's AI mode as a
 * CLI word.  THE mapping, so the reader and the writer cannot drift.
 * ------------------------------------------------------------------------- */
static const gchar *
ai_mode_name(gint mode)
{
    switch (mode) {
    case ON_AI_MODE_PROJECT: return "project";
    case ON_AI_MODE_CUSTOM:  return "custom";
    default:                 return "normal";
    }
}

static gboolean
ai_mode_parse(const gchar *word, gint *out_mode)
{
    if (g_strcmp0(word, "normal") == 0)
        *out_mode = ON_AI_MODE_NORMAL;
    else if (g_strcmp0(word, "project") == 0)
        *out_mode = ON_AI_MODE_PROJECT;
    else if (g_strcmp0(word, "custom") == 0)
        *out_mode = ON_AI_MODE_CUSTOM;
    else
        return FALSE;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * cmd_folder_info() — everything the folder's Info dialog shows, as
 * "key<TAB>value" lines (or one JSON object): id, path, name, emoji, AI
 * mode, and how many notes and direct subfolders it holds.
 * ------------------------------------------------------------------------- */
static int
cmd_folder_info(OnDatabase *db, const gchar *path)
{
    gint64 id;                       /* the folder in question              */
    if (!folder_from_arg(db, path, &id))
        return 2;

    gchar *full  = on_db_folder_path(db, id);   /* "Folder/Sub" (owned)   */
    gchar *emoji = on_db_folder_get_emoji(db, id);
    gint   mode  = on_db_folder_get_ai_mode(db, id);
    /* The name is the last component of the path — no separate query.      */
    const gchar *name = (full != NULL) ? full : "";
    const gchar *slash = strrchr(name, '/');
    if (slash != NULL)
        name = slash + 1;

    GList *notes = on_db_note_list(db, id);
    gint n_notes = (gint)g_list_length(notes);
    on_db_note_list_free(notes);
    GList *subs = on_db_folder_list(db, id);
    gint n_subs = (gint)g_list_length(subs);
    on_db_folder_list_free(subs);

    if (cli_json) {
        printf("{\"id\":%" G_GINT64_FORMAT ",\"name\":", id);
        json_str(name);
        printf(",\"path\":");
        json_str(full != NULL ? full : "");
        printf(",\"emoji\":");
        json_str(emoji);
        printf(",\"ai_mode\":");
        json_str(ai_mode_name(mode));
        printf(",\"notes\":%d,\"subfolders\":%d}\n", n_notes, n_subs);
    } else {
        printf("id\t%" G_GINT64_FORMAT "\n", id);
        printf("path\t/%s\n", full != NULL ? full : "");
        printf("name\t%s\n", name);
        printf("emoji\t%s\n", *emoji != '\0' ? emoji : "-");
        printf("ai-mode\t%s\n", ai_mode_name(mode));
        printf("notes\t%d\n", n_notes);
        printf("subfolders\t%d\n", n_subs);
    }
    g_free(full);
    g_free(emoji);
    return 0;
}

/* cmd_folder_rename() — give a folder a new name, in place.                 */
static int
cmd_folder_rename(OnDatabase *db, const gchar *path, const gchar *name)
{
    gint64 id;                       /* the folder to rename                */
    if (!folder_from_arg(db, path, &id))
        return 2;
    if (name == NULL || *name == '\0' || strchr(name, '/') != NULL) {
        fprintf(stderr, "error: bad folder name: %s "
                        "(non-empty, and no '/')\n", name);
        return 2;
    }
    if (!on_db_folder_rename(db, id, name)) {
        fprintf(stderr, "error: could not rename folder %s\n", path);
        return 2;
    }
    printf("renamed folder %s -> %s\n", path, name);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_folder_move() — re-nest a folder (with its whole subtree) under a
 * destination path, "/" meaning the top level.  The database refuses to
 * move a folder into itself or one of its own descendants.
 * ------------------------------------------------------------------------- */
static int
cmd_folder_move(OnDatabase *db, const gchar *path, const gchar *dest)
{
    gint64 id;                       /* the folder to move                  */
    if (!folder_from_arg(db, path, &id))
        return 2;
    gint64 parent;                   /* destination parent (0 = top level)  */
    if (!on_cli_resolve_folder_path(db, dest, FALSE, &parent)) {
        fprintf(stderr, "error: no such folder: %s\n", dest);
        return 2;
    }
    if (!on_db_folder_move(db, id, parent)) {
        fprintf(stderr, "error: could not move %s into %s "
                        "(a folder cannot contain itself)\n",
                path, *dest != '\0' ? dest : "/");
        return 2;
    }
    printf("moved folder %s -> %s\n", path, *dest != '\0' ? dest : "/");
    return 0;
}

/* cmd_folder_emoji() — set the folder's sidebar emoji prefix ("-" clears). */
static int
cmd_folder_emoji(OnDatabase *db, const gchar *path, const gchar *emoji)
{
    gint64 id;                       /* the folder to label                 */
    if (!folder_from_arg(db, path, &id))
        return 2;
    const gchar *value = (g_strcmp0(emoji, "-") == 0) ? "" : emoji;
    if (!g_utf8_validate(value, -1, NULL)) {
        fprintf(stderr, "error: emoji is not valid UTF-8\n");
        return 2;
    }
    if (!on_db_folder_set_emoji(db, id, value)) {
        fprintf(stderr, "error: could not set emoji on %s\n", path);
        return 2;
    }
    if (*value != '\0')
        printf("set emoji %s on folder %s\n", value, path);
    else
        printf("cleared emoji on folder %s\n", path);
    return 0;
}

/* cmd_folder_ai_mode() — set the folder's AI mode (its Info dialog's third
 * field): normal, project or custom.                                        */
static int
cmd_folder_ai_mode(OnDatabase *db, const gchar *path, const gchar *word)
{
    gint64 id;                       /* the folder to configure             */
    if (!folder_from_arg(db, path, &id))
        return 2;
    gint mode;                       /* parsed ON_AI_MODE_*                 */
    if (!ai_mode_parse(word, &mode)) {
        fprintf(stderr, "error: bad AI mode: %s "
                        "(normal, project or custom)\n", word);
        return 2;
    }
    if (!on_db_folder_set_ai_mode(db, id, mode)) {
        fprintf(stderr, "error: could not set AI mode on %s\n", path);
        return 2;
    }
    printf("set ai-mode %s on folder %s\n", ai_mode_name(mode), path);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_folder_sort() — order one folder's DIRECT children alphabetically,
 * the folder context menu's "Sort Subfolders Alphabetically".  A path of
 * "/" sorts the top level.  Case-insensitive, like the menu item.
 * ------------------------------------------------------------------------- */
static int
cmd_folder_sort(OnDatabase *db, const gchar *path)
{
    gint64 id;                       /* parent whose children get sorted    */
    if (!on_cli_resolve_folder_path(db, path, FALSE, &id)) {
        fprintf(stderr, "error: no such folder: %s\n", path);
        return 2;
    }

    gint n = on_db_folder_sort_children(db, id);
    if (n < 0) {
        fprintf(stderr, "error: could not sort %s\n", path);
        return 2;
    }
    printf("sorted %d subfolder%s of %s\n", n, n == 1 ? "" : "s",
           *path != '\0' ? path : "/");
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_folder_restore() — take a folder out of the Trash, by ID rather than
 * by path: a trashed folder is absent from the normal listings the path
 * walker uses, so `trash list` is where its id comes from.
 * ------------------------------------------------------------------------- */
static int
cmd_folder_restore(OnDatabase *db, const gchar *id_str)
{
    gchar *endp = NULL;              /* end of the parsed id                */
    gint64 id = g_ascii_strtoll(id_str, &endp, 10);
    if (id <= 0 || endp == NULL || *endp != '\0') {
        fprintf(stderr, "error: bad folder id: %s "
                        "(see 'trash list')\n", id_str);
        return 2;
    }
    if (!on_db_folder_restore(db, id)) {
        fprintf(stderr, "error: could not restore folder %s\n", id_str);
        return 2;
    }
    gchar *path = on_db_folder_path(db, id);
    printf("restored folder %" G_GINT64_FORMAT "\t/%s\n",
           id, path != NULL ? path : "");
    g_free(path);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_delete_folder() — move a folder (with its whole subtree) to the
 * Trash, or with `permanent` delete it and its contents outright.
 * ------------------------------------------------------------------------- */
static int
cmd_delete_folder(OnDatabase *db, const gchar *path, gboolean permanent)
{
    gint64 id;                       /* the folder to delete                */
    if (!on_cli_resolve_folder_path(db, path, FALSE, &id) || id == 0) {
        fprintf(stderr, "error: no such folder: %s\n", path);
        return 2;
    }
    gboolean ok = permanent ? on_db_folder_delete(db, id)
                            : on_db_folder_trash(db, id);
    if (!ok) {
        fprintf(stderr, "error: could not %s folder %s\n",
                permanent ? "delete" : "trash", path);
        return 2;
    }
    printf(permanent ? "deleted folder %s (and its contents)\n"
                     : "trashed folder %s (and its contents)\n", path);
    return 0;
}

/* ---------------------------------------------------------------------------
 * note_path_of() — a note's "/Folder/Sub/Title" display path, built from the
 * pre-fetched folder-path map (one query for a whole listing, never one per
 * note — see on_db_folder_path_map).  A NULL map, or a folder missing from
 * it, yields the top-level form "/Title".
 * Returns a newly allocated string.
 * ------------------------------------------------------------------------- */
static gchar *
note_path_of(OnNoteMeta *m, GHashTable *paths)
{
    const gchar *fpath = (paths != NULL)
                         ? g_hash_table_lookup(paths, &m->folder_id) : NULL;
    if (fpath == NULL)
        fpath = "";
    return g_strdup_printf("/%s%s%s", fpath,
                           *fpath != '\0' ? "/" : "", m->title);
}

/* ---------------------------------------------------------------------------
 * print_note_line() — THE note record, in whichever format is in force.
 *
 * Plain: "ID<TAB>MODIFIED<TAB>TITLE", or with a non-NULL `paths` map
 * "ID<TAB>MODIFIED<TAB>/Folder/Sub/TITLE".
 * JSON:  one object carrying every field, the path included — so a JSON
 * caller must ALWAYS pass the map (each such command fetches it when
 * cli_json is set), or every note would claim to sit at the top level.
 * ------------------------------------------------------------------------- */
static void
print_note_line(OnNoteMeta *m, GHashTable *paths)
{
    if (cli_json) {
        gchar *full = note_path_of(m, paths);   /* display path (owned)     */
        json_element();
        printf("{\"id\":%" G_GINT64_FORMAT ",\"title\":", m->id);
        json_str(m->title);
        printf(",\"path\":");
        json_str(full);
        printf(",\"folder_id\":%" G_GINT64_FORMAT ",", m->folder_id);
        json_time("modified", m->updated_at);
        putchar(',');
        json_time("created", m->created_at);
        printf(",\"pinned\":%s}", m->pinned ? "true" : "false");
        g_free(full);
        return;
    }

    gchar *when = cli_time_str(m->updated_at, FALSE);
    if (paths != NULL) {
        gchar *full = note_path_of(m, paths);   /* display path (owned)     */
        printf("%" G_GINT64_FORMAT "\t%s\t%s\n", m->id, when, full);
        g_free(full);
    } else {
        printf("%" G_GINT64_FORMAT "\t%s\t%s\n", m->id, when, m->title);
    }
    g_free(when);
}

/* ---------------------------------------------------------------------------
 * print_note_list() — a whole result set of notes, framed as a JSON array
 * when --json is in force and as bare lines otherwise.  Every command that
 * prints notes goes through here so all of them gain both formats at once.
 *   notes — OnNoteMeta* list (not consumed).
 *   paths — folder-path map, or NULL for the bare-title plain form.
 * ------------------------------------------------------------------------- */
static void
print_note_list(GList *notes, GHashTable *paths)
{
    json_array_begin();
    for (GList *l = notes; l != NULL; l = l->next)
        print_note_line(l->data, paths);
    json_array_end();
}

/* ---------------------------------------------------------------------------
 * cli_paths_for() — the folder-path map a note listing needs: always in
 * JSON mode (every record carries its path), and in plain mode only when
 * the caller asked for the path form.  NULL means "bare titles".
 * Returns a map to destroy with g_hash_table_destroy(), or NULL.
 * ------------------------------------------------------------------------- */
static GHashTable *
cli_paths_for(OnDatabase *db, gboolean want_path)
{
    return (cli_json || want_path) ? on_db_folder_path_map(db) : NULL;
}

/* ---------------------------------------------------------------------------
 * cmd_list_notes() — notes in one folder, or one of the library's own
 * views: --all (every note), --recent (visible notes, newest first — the
 * "All Notes" row) or --pinned (the "Pinned Notes" section).
 * ------------------------------------------------------------------------- */
static int
cmd_list_notes(OnDatabase *db, const gchar *path)
{
    GList *notes;                    /* the OnNoteMeta* result set          */
    if (g_strcmp0(path, "--all") == 0) {
        notes = on_db_note_list_all(db, FALSE);
    } else if (g_strcmp0(path, "--recent") == 0) {
        notes = on_db_note_list_recent(db);
    } else if (g_strcmp0(path, "--pinned") == 0) {
        notes = on_db_note_list_pinned(db);
    } else {
        gint64 folder;               /* resolved folder id                  */
        if (!on_cli_resolve_folder_path(db, path, FALSE, &folder)) {
            fprintf(stderr, "error: no such folder: %s\n", path);
            return 2;
        }
        notes = on_db_note_list(db, folder);
    }
    GHashTable *paths = cli_paths_for(db, FALSE);   /* JSON needs the paths */
    print_note_list(notes, paths);
    if (paths != NULL)
        g_hash_table_destroy(paths);
    on_db_note_list_free(notes);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_new_note() — create a note from CLI-supplied content.
 *   folder_path — destination ("" = top level; must already exist).
 *   content     — the note text, or "-" to read stdin.
 * Serializing needs GTK's text buffer machinery, so GTK is initialized
 * (windowless) for this command.
 * ------------------------------------------------------------------------- */
static int
cmd_new_note(OnDatabase *db, const gchar *folder_path, const gchar *content)
{
    gint64 folder;                   /* destination folder id               */
    if (!on_cli_resolve_folder_path(db, folder_path, FALSE, &folder)) {
        fprintf(stderr, "error: no such folder: %s "
                        "(create it with 'folder add')\n", folder_path);
        return 2;
    }

    gchar *text = cli_read_content(content);   /* the note body (owned)     */
    if (text == NULL)
        return 2;
    if (!cli_require_gtk()) {
        g_free(text);
        return 2;
    }

    gint64 id = on_db_note_create(db, folder);
    if (id == 0) {
        fprintf(stderr, "error: could not create note\n");
        g_free(text);
        return 2;
    }

    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gtk_text_buffer_set_text(buffer, text, -1);
    note_buffer_save(db, id, buffer);

    gchar *title = on_buffer_first_line(buffer);
    printf("note %" G_GINT64_FORMAT "\t%s\n", id, title);

    g_free(title);
    g_object_unref(buffer);
    g_free(text);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_cat_note() — print a note's content to stdout.
 *   markdown — FALSE: the cached plain text (no GTK needed);
 *              TRUE:  a Markdown render with formatting preserved
 *                     (images become "![image N]()" placeholders).
 * ------------------------------------------------------------------------- */
static int
cmd_cat_note(OnDatabase *db, const gchar *id_str, gboolean markdown)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;

    gchar *text;                     /* what gets printed (owned)           */
    if (markdown) {
        if (!cli_require_gtk()) {
            on_db_note_meta_free(meta);
            return 2;
        }
        /* The exporter only touches app->db.                               */
        OnApp app = { 0 };
        app.db = db;
        text = on_export_note_markdown(&app, meta->id);
    } else {
        text = on_note_text_cached(db, meta->id);
    }

    if (cli_json) {
        /* The whole body as one escaped string: the newlines (and any tabs)
         * that make raw output ambiguous survive exactly.                  */
        printf("{\"id\":%" G_GINT64_FORMAT ",\"title\":", meta->id);
        json_str(meta->title);
        printf(",\"format\":\"%s\",\"text\":", markdown ? "markdown" : "text");
        json_str(text);
        printf("}\n");
    } else {
        fputs(text, stdout);
        if (*text == '\0' || text[strlen(text) - 1] != '\n')
            putchar('\n');
    }

    g_free(text);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_append_note() — append plain text to an existing note, on a fresh
 * line (existing content, formatting and images are untouched).
 * ------------------------------------------------------------------------- */
static int
cmd_append_note(OnDatabase *db, const gchar *id_str, const gchar *content)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    gchar *text = cli_read_content(content);   /* text to append (owned)    */
    if (text == NULL || !cli_require_gtk()) {
        g_free(text);
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = on_note_buffer_load(db, meta->id, 0);
    GtkTextIter end;                 /* append position                     */
    buffer_append_iter(buffer, &end, "\n");
    gtk_text_buffer_insert(buffer, &end, text, -1);

    int rc = 0;                      /* process exit code                   */
    if (note_buffer_save(db, meta->id, buffer)) {
        printf("appended to note %" G_GINT64_FORMAT "\t%s\n",
               meta->id, meta->title);
    } else {
        fprintf(stderr, "error: could not save note %" G_GINT64_FORMAT "\n",
                meta->id);
        rc = 2;
    }
    g_object_unref(buffer);
    g_free(text);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_set_note() — REPLACE a note's content with plain text.  Anything the
 * old content held (formatting, images, tables, checkboxes, #tags) is
 * gone, exactly like select-all + retype in the editor — so the note's
 * tag links are rewritten to the (empty) tag set of the new text.
 * ------------------------------------------------------------------------- */
static int
cmd_set_note(OnDatabase *db, const gchar *id_str, const gchar *content)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    gchar *text = cli_read_content(content);   /* the new body (owned)      */
    if (text == NULL || !cli_require_gtk()) {
        g_free(text);
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gtk_text_buffer_set_text(buffer, text, -1);

    int rc = 0;                      /* process exit code                   */
    if (note_buffer_save(db, meta->id, buffer)) {
        /* plain text: clear any tag links from the previous content; check
         * the result so a failed sync isn't silently reported as success. */
        if (!on_db_note_set_tags(db, meta->id, NULL)) {
            fprintf(stderr,
                    "error: could not clear tags for note %"
                    G_GINT64_FORMAT "\n", meta->id);
            rc = 2;
        } else {
            gchar *title = on_buffer_first_line(buffer);
            printf("set note %" G_GINT64_FORMAT "\t%s\n", meta->id, title);
            g_free(title);
        }
    } else {
        fprintf(stderr, "error: could not save note %" G_GINT64_FORMAT "\n",
                meta->id);
        rc = 2;
    }
    g_object_unref(buffer);
    g_free(text);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_add_image() — append an image file to an existing note.  The image
 * is stored at full resolution (same as pasting it in the editor) and
 * displayed at the default thumbnail width.
 * ------------------------------------------------------------------------- */
static int
cmd_add_image(OnDatabase *db, const gchar *id_str, const gchar *file)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    gint64 id = meta->id;            /* validated note id                   */

    if (!cli_require_gtk()) {
        on_db_note_meta_free(meta);
        return 2;
    }

    GError *err = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(file, &err);
    if (pixbuf == NULL) {
        fprintf(stderr, "error: cannot load image %s: %s\n",
                file, err->message);
        g_clear_error(&err);
        on_db_note_meta_free(meta);
        return 2;
    }

    /* Load the note, append the image on a fresh line, save it back.       */
    GtkTextBuffer *buffer = on_note_buffer_load(db, id, 0);

    GtkTextIter end;                 /* append position                     */
    buffer_append_iter(buffer, &end, "\n");
    GtkTextChildAnchor *anchor =
        gtk_text_buffer_create_child_anchor(buffer, &end);
    on_anchor_set_image(anchor, pixbuf, 0);

    note_buffer_save(db, id, buffer);
    gchar *title = on_buffer_first_line(buffer);
    printf("added image to note %" G_GINT64_FORMAT "\t%s\n", id, title);

    g_free(title);
    g_object_unref(buffer);
    g_object_unref(pixbuf);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_note_info() — everything about one note that is not its content, as
 * "key<TAB>value" lines or one JSON object: identity, location, dates,
 * pinned/trashed state, tag names, and how many images, action items and
 * characters it holds.  One call instead of the three or four a caller
 * would otherwise stitch together.
 * ------------------------------------------------------------------------- */
static int
cmd_note_info(OnDatabase *db, const gchar *id_str)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;

    GHashTable *paths = on_db_folder_path_map(db);
    gchar *path = note_path_of(meta, paths);        /* display path (owned) */
    g_hash_table_destroy(paths);

    GList *tags = on_db_note_tag_list(db, meta->id);
    GList *acts = on_db_action_list_for_note(db, meta->id);
    gint n_open = 0;                 /* not-yet-done action items           */
    for (GList *l = acts; l != NULL; l = l->next)
        if (!((OnActionItem *)l->data)->done)
            n_open++;

    /* Images need the blob, but only its record headers — no PNG is
     * decoded (on_note_count_images walks past every payload).             */
    gsize blob_len = 0;              /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, meta->id, &blob_len);
    gint n_images = on_note_count_images(blob, blob_len);
    g_free(blob);

    gchar *body = on_note_text_cached(db, meta->id);
    glong n_chars = g_utf8_strlen(body, -1);
    g_free(body);

    gboolean trashed = on_db_note_is_trashed(db, meta->id);

    if (cli_json) {
        printf("{\"id\":%" G_GINT64_FORMAT ",\"title\":", meta->id);
        json_str(meta->title);
        printf(",\"path\":");
        json_str(path);
        printf(",\"folder_id\":%" G_GINT64_FORMAT ",", meta->folder_id);
        json_time("modified", meta->updated_at);
        putchar(',');
        json_time("created", meta->created_at);
        printf(",\"pinned\":%s,\"trashed\":%s,\"tags\":[",
               meta->pinned ? "true" : "false",
               trashed ? "true" : "false");
        for (GList *l = tags; l != NULL; l = l->next) {
            if (l != tags)
                putchar(',');
            json_str(((OnTag *)l->data)->name);
        }
        printf("],\"images\":%d,\"actions\":%d,\"actions_open\":%d,"
               "\"characters\":%ld}\n",
               n_images, g_list_length(acts), n_open, n_chars);
    } else {
        printf("id\t%" G_GINT64_FORMAT "\n", meta->id);
        printf("title\t%s\n", meta->title);
        printf("path\t%s\n", path);
        printf("folder_id\t%" G_GINT64_FORMAT "\n", meta->folder_id);
        gchar *when = cli_time_str(meta->updated_at, FALSE);
        printf("modified\t%s\n", when);
        g_free(when);
        when = cli_time_str(meta->created_at, FALSE);
        printf("created\t%s\n", when);
        g_free(when);
        printf("pinned\t%s\n", meta->pinned ? "yes" : "no");
        printf("trashed\t%s\n", trashed ? "yes" : "no");
        for (GList *l = tags; l != NULL; l = l->next)
            printf("tag\t%s\n", ((OnTag *)l->data)->name);
        printf("images\t%d\n", n_images);
        printf("actions\t%u\t%d open\n", g_list_length(acts), n_open);
        printf("characters\t%ld\n", n_chars);
    }

    on_db_action_list_free(acts);
    on_db_tag_list_free(tags);
    g_free(path);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_note_pin() — set or clear the pinned flag on each given note (the
 * sidebar's "Pinned Notes" section).
 * ------------------------------------------------------------------------- */
static int
cmd_note_pin(OnDatabase *db, char **ids, int n, gboolean pinned)
{
    int rc = 0;                      /* worst exit code seen                */
    for (int i = 0; i < n; i++) {
        OnNoteMeta *meta = note_from_arg(db, ids[i]);
        if (meta == NULL) {
            rc = 2;
            continue;
        }
        if (on_db_note_set_pinned(db, meta->id, pinned)) {
            printf("%s note %" G_GINT64_FORMAT "\t%s\n",
                   pinned ? "pinned" : "unpinned", meta->id, meta->title);
        } else {
            fprintf(stderr, "error: could not %s note %" G_GINT64_FORMAT
                    "\n", pinned ? "pin" : "unpin", meta->id);
            rc = 2;
        }
        on_db_note_meta_free(meta);
    }
    return rc;
}

/* ---------------------------------------------------------------------------
 * note_image_blob() — load one note's stored blob for the image commands.
 *   meta     — the note (already validated).
 *   out_len  — receives the blob length.
 * Returns the blob (g_free it), or NULL after printing an error.
 * ------------------------------------------------------------------------- */
static guint8 *
note_image_blob(OnDatabase *db, OnNoteMeta *meta, gsize *out_len)
{
    guint8 *blob = on_db_note_load(db, meta->id, out_len);
    if (blob == NULL)
        fprintf(stderr, "error: note %" G_GINT64_FORMAT " has no content\n",
                meta->id);
    return blob;
}

/* ---------------------------------------------------------------------------
 * cmd_note_images() — list a note's embedded images: ordinal, byte size and
 * pixel dimensions, one per line (or as a JSON array).  Ordinals are
 * 1-BASED so they match the "![image N]()" placeholders `note cat --md`
 * writes and the argument `note image` takes.  No image is decoded — the
 * dimensions come from each PNG's header.
 * ------------------------------------------------------------------------- */
static int
cmd_note_images(OnDatabase *db, const gchar *id_str)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    gsize blob_len = 0;              /* stored blob size                    */
    guint8 *blob = note_image_blob(db, meta, &blob_len);
    if (blob == NULL) {
        on_db_note_meta_free(meta);
        return 2;
    }

    gint n = on_note_count_images(blob, blob_len);
    json_array_begin();
    for (gint i = 0; i < n; i++) {
        GBytes *png = on_note_image_nth_png(blob, blob_len, i);
        if (png == NULL)
            continue;
        gsize n_png;                 /* encoded size                        */
        const guint8 *bytes = g_bytes_get_data(png, &n_png);
        gint w = 0, h = 0;           /* pixel dimensions from the header    */
        on_png_probe_size(bytes, n_png, &w, &h);
        if (cli_json) {
            json_element();
            printf("{\"ord\":%d,\"bytes\":%" G_GSIZE_FORMAT
                   ",\"width\":%d,\"height\":%d}", i + 1, n_png, w, h);
        } else {
            printf("%d\t%" G_GSIZE_FORMAT "\t%dx%d\n", i + 1, n_png, w, h);
        }
        g_bytes_unref(png);
    }
    json_array_end();

    g_free(blob);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_note_image() — write one embedded image to a file, byte for byte as
 * the note stores it: no decode, no re-encode, so what lands on disk is the
 * original PNG.  `ord` is 1-based (see cmd_note_images).
 * ------------------------------------------------------------------------- */
static int
cmd_note_image(OnDatabase *db, const gchar *id_str, const gchar *ord_str,
               const gchar *file)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;

    gchar *endp = NULL;              /* end of the parsed ordinal           */
    gint64 ord = g_ascii_strtoll(ord_str, &endp, 10);
    if (ord < 1 || endp == NULL || *endp != '\0') {
        fprintf(stderr, "error: bad image number: %s "
                        "(1-based, see 'note images')\n", ord_str);
        on_db_note_meta_free(meta);
        return 2;
    }

    gsize blob_len = 0;              /* stored blob size                    */
    guint8 *blob = note_image_blob(db, meta, &blob_len);
    if (blob == NULL) {
        on_db_note_meta_free(meta);
        return 2;
    }

    int rc = 0;                      /* process exit code                   */
    GBytes *png = on_note_image_nth_png(blob, blob_len, (gint)ord - 1);
    if (png == NULL) {
        fprintf(stderr, "error: note %" G_GINT64_FORMAT
                " has no image %s (it has %d)\n",
                meta->id, ord_str, on_note_count_images(blob, blob_len));
        rc = 2;
    } else {
        gsize n_png;                 /* encoded size                        */
        const gchar *bytes = g_bytes_get_data(png, &n_png);
        GError *err = NULL;          /* write failure                       */
        if (g_file_set_contents(file, bytes, (gssize)n_png, &err)) {
            printf("wrote image %" G_GINT64_FORMAT " of note %"
                   G_GINT64_FORMAT " to %s\t%" G_GSIZE_FORMAT " bytes\n",
                   ord, meta->id, file, n_png);
        } else {
            fprintf(stderr, "error: could not write %s: %s\n",
                    file, err->message);
            g_clear_error(&err);
            rc = 2;
        }
        g_bytes_unref(png);
    }

    g_free(blob);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_note_tags() — print a note's tag names, one per line.
 * ------------------------------------------------------------------------- */
static int
cmd_note_tags(OnDatabase *db, const gchar *id_str)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    GList *tags = on_db_note_tag_list(db, meta->id);
    json_array_begin();
    for (GList *l = tags; l != NULL; l = l->next) {
        const gchar *name = ((OnTag *)l->data)->name;
        if (cli_json) {
            json_element();
            json_str(name);
        } else {
            printf("%s\n", name);
        }
    }
    json_array_end();
    on_db_tag_list_free(tags);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_tag_note() — label a note with a #tag the way the editor does: the
 * literal "#name" token is appended to the note text (under the on-tag
 * text tag, so it survives GUI edits and re-saves), and the note's tag
 * links are rewritten from the buffer.  A tag living only in note_tags
 * would be silently dropped by the next tag-touching GUI save.
 * ------------------------------------------------------------------------- */
static int
cmd_tag_note(OnDatabase *db, const gchar *id_str, const gchar *name)
{
    /* Accept the name with or without the leading '#'.                     */
    if (*name == '#')
        name++;
    if (!tag_name_valid(name)) {
        fprintf(stderr, "error: bad tag name: %s (letters, digits, "
                        "'_' and '-' only)\n", name);
        return 2;
    }
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    if (!cli_require_gtk()) {
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = on_note_buffer_load(db, meta->id, 0);
    GList *existing = on_buffer_collect_tags(buffer);
    int rc = 0;                      /* process exit code                   */

    if (g_list_find_custom(existing, name, (GCompareFunc)g_strcmp0) != NULL) {
        /* Already in the text; just make sure the db link agrees.          */
        on_db_note_set_tags(db, meta->id, existing);
        printf("note %" G_GINT64_FORMAT " already tagged #%s\n",
               meta->id, name);
    } else {
        GtkTextIter end;             /* append position                     */
        buffer_append_iter(buffer, &end, " ");
        gchar *token = g_strdup_printf("#%s", name);
        gtk_text_buffer_insert_with_tags_by_name(buffer, &end, token, -1,
                                                 ON_TAGNAME_TAG, NULL);
        g_free(token);

        GList *tags = on_buffer_collect_tags(buffer);
        if (note_buffer_save(db, meta->id, buffer) &&
            on_db_note_set_tags(db, meta->id, tags)) {
            printf("tagged note %" G_GINT64_FORMAT "\t#%s\n",
                   meta->id, name);
        } else {
            fprintf(stderr, "error: could not tag note %" G_GINT64_FORMAT
                    "\n", meta->id);
            rc = 2;
        }
        g_list_free_full(tags, g_free);
    }

    g_list_free_full(existing, g_free);
    g_object_unref(buffer);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_untag_note() — remove a #tag from a note: every "#name" token is
 * deleted from the note text (plus one separating space before it) and
 * the tag links are rewritten from the remaining buffer tags.  A tag
 * linked in note_tags but absent from the text is unlinked directly.
 * ------------------------------------------------------------------------- */
static int
cmd_untag_note(OnDatabase *db, const gchar *id_str, const gchar *name)
{
    if (*name == '#')
        name++;
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;
    if (!cli_require_gtk()) {
        on_db_note_meta_free(meta);
        return 2;
    }

    GtkTextBuffer *buffer = on_note_buffer_load(db, meta->id, 0);
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, ON_TAGNAME_TAG);

    /* Collect [start,end) offset pairs of every matching "#name" span
     * (ascending, the walk on_buffer_collect_tags uses), then delete them
     * back-to-front so earlier offsets stay valid.                          */
    GArray *spans = g_array_new(FALSE, FALSE, sizeof(gint));
    GtkTextIter iter;                /* scan position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);
    while (tag != NULL) {
        if (!gtk_text_iter_starts_tag(&iter, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&iter, tag))
                break;
            if (!gtk_text_iter_starts_tag(&iter, tag))
                continue;
        }
        GtkTextIter span_end = iter; /* end of this tag span                */
        gtk_text_iter_forward_to_tag_toggle(&span_end, tag);

        gchar *text = gtk_text_buffer_get_text(buffer, &iter, &span_end,
                                               FALSE);
        g_strstrip(text);
        const gchar *span_name = (*text == '#') ? text + 1 : text;
        if (g_strcmp0(span_name, name) == 0) {
            gint a = gtk_text_iter_get_offset(&iter);
            gint b = gtk_text_iter_get_offset(&span_end);
            GtkTextIter before = iter;   /* eat one separating space        */
            if (gtk_text_iter_backward_char(&before) &&
                gtk_text_iter_get_char(&before) == ' ')
                a--;
            g_array_append_val(spans, a);
            g_array_append_val(spans, b);
        }
        g_free(text);
        iter = span_end;
    }

    int rc = 0;                      /* process exit code                   */
    if (spans->len == 0) {
        /* Not in the text; drop a db-only link if one exists.              */
        GList *linked = on_db_note_tag_list(db, meta->id);
        GList *keep = NULL;          /* names to keep (borrowed strings)    */
        gboolean had = FALSE;        /* was the tag linked at all?          */
        for (GList *l = linked; l != NULL; l = l->next) {
            OnTag *t = l->data;
            if (g_strcmp0(t->name, name) == 0)
                had = TRUE;
            else
                keep = g_list_prepend(keep, t->name);
        }
        if (had && on_db_note_set_tags(db, meta->id, keep)) {
            printf("untagged note %" G_GINT64_FORMAT "\t#%s\n",
                   meta->id, name);
        } else {
            fprintf(stderr, "error: note %" G_GINT64_FORMAT
                    " has no tag #%s\n", meta->id, name);
            rc = 2;
        }
        g_list_free(keep);
        on_db_tag_list_free(linked);
    } else {
        for (guint i = spans->len; i >= 2; i -= 2) {
            GtkTextIter a, b;        /* span bounds to delete               */
            gtk_text_buffer_get_iter_at_offset(buffer, &a,
                g_array_index(spans, gint, i - 2));
            gtk_text_buffer_get_iter_at_offset(buffer, &b,
                g_array_index(spans, gint, i - 1));
            gtk_text_buffer_delete(buffer, &a, &b);
        }
        GList *tags = on_buffer_collect_tags(buffer);
        if (note_buffer_save(db, meta->id, buffer) &&
            on_db_note_set_tags(db, meta->id, tags)) {
            printf("untagged note %" G_GINT64_FORMAT "\t#%s\n",
                   meta->id, name);
        } else {
            fprintf(stderr, "error: could not untag note %" G_GINT64_FORMAT
                    "\n", meta->id);
            rc = 2;
        }
        g_list_free_full(tags, g_free);
    }

    g_array_free(spans, TRUE);
    g_object_unref(buffer);
    on_db_note_meta_free(meta);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_delete_notes() — move the given note ids to the Trash (the GUI's
 * soft delete), or with `permanent` remove them outright.  Either way the
 * valid ids go through ONE bulk call/transaction.
 * ------------------------------------------------------------------------- */
static int
cmd_delete_notes(OnDatabase *db, char **ids, int n, gboolean permanent)
{
    int rc = 0;                      /* worst exit code seen                */
    GArray *valid = g_array_new(FALSE, FALSE, sizeof(gint64));
    GList  *metas = NULL;            /* matched notes, for the messages     */
    for (int i = 0; i < n; i++) {
        OnNoteMeta *meta = note_from_arg(db, ids[i]);
        if (meta == NULL) {
            rc = 2;
            continue;
        }
        g_array_append_val(valid, meta->id);
        metas = g_list_prepend(metas, meta);
    }
    metas = g_list_reverse(metas);

    gboolean ok = TRUE;              /* did the bulk operation succeed?     */
    if (valid->len > 0)
        ok = permanent
             ? on_db_notes_delete(db, (const gint64 *)valid->data, valid->len)
             : on_db_notes_trash(db, (const gint64 *)valid->data, valid->len);
    if (!ok) {
        fprintf(stderr, "error: could not %s notes\n",
                permanent ? "delete" : "trash");
        rc = 2;
    } else {
        for (GList *l = metas; l != NULL; l = l->next) {
            OnNoteMeta *m = l->data;
            printf("%s note %" G_GINT64_FORMAT "\t%s\n",
                   permanent ? "deleted" : "trashed", m->id, m->title);
        }
    }

    g_list_free_full(metas, (GDestroyNotify)on_db_note_meta_free);
    g_array_free(valid, TRUE);
    return rc;
}

/* cmd_restore_notes() — restore each note id from the Trash.                */
static int
cmd_restore_notes(OnDatabase *db, char **ids, int n)
{
    int rc = 0;                      /* worst exit code seen                */
    for (int i = 0; i < n; i++) {
        OnNoteMeta *meta = note_from_arg(db, ids[i]);
        if (meta == NULL) {
            rc = 2;
            continue;
        }
        if (on_db_note_restore(db, meta->id)) {
            printf("restored note %" G_GINT64_FORMAT "\t%s\n",
                   meta->id, meta->title);
        } else {
            fprintf(stderr, "error: could not restore note %"
                    G_GINT64_FORMAT "\n", meta->id);
            rc = 2;
        }
        on_db_note_meta_free(meta);
    }
    return rc;
}

/* cmd_move_notes() — move note ids into a destination folder path.          */
static int
cmd_move_notes(OnDatabase *db, char **ids, int n, const gchar *dest)
{
    gint64 folder;                   /* destination folder id               */
    if (!on_cli_resolve_folder_path(db, dest, FALSE, &folder)) {
        fprintf(stderr, "error: no such folder: %s\n", dest);
        return 2;
    }

    int rc = 0;                      /* worst exit code seen                */
    for (int i = 0; i < n; i++) {
        OnNoteMeta *meta = note_from_arg(db, ids[i]);
        if (meta == NULL) {
            rc = 2;
            continue;
        }
        on_db_notes_move(db, &meta->id, 1, folder);
        printf("moved note %" G_GINT64_FORMAT "\t%s -> %s\n",
               meta->id, meta->title, *dest ? dest : "/");
        on_db_note_meta_free(meta);
    }
    return rc;
}

/* cmd_set_modified() — overwrite a note's modification date with a UNIX
 * timestamp.  Lets importers preserve the original edit date (a normal
 * save always stamps the current time).                                     */
static int
cmd_set_modified(OnDatabase *db, const gchar *id_str, const gchar *ts_str)
{
    OnNoteMeta *meta = note_from_arg(db, id_str);
    if (meta == NULL)
        return 2;

    gchar *endp = NULL;              /* end of the parsed number            */
    gint64 ts = g_ascii_strtoll(ts_str, &endp, 10);
    if (ts <= 0 || endp == NULL || *endp != '\0') {
        fprintf(stderr, "error: bad UNIX timestamp: %s\n", ts_str);
        on_db_note_meta_free(meta);
        return 2;
    }

    on_db_note_set_updated_at(db, meta->id, ts);
    printf("set modified of note %" G_GINT64_FORMAT "\t%s\n",
           meta->id, meta->title);
    on_db_note_meta_free(meta);
    return 0;
}

/* ---------------------------------------------------------------------------
 * action_token_parse() — resolve an item address to (note_id, ord).
 *
 * Two forms, told apart BY SHAPE: a token containing ':' is the legacy
 * positional "NOTEID:ORD" (the first column of plain `action list`, whose
 * meaning shifts as lines are added and removed); anything else is a
 * stable uid (the first column of `action list --uid`), looked up in the
 * table.  Since a uid is a bare decimal and the positional form always
 * carries a colon, neither can ever be read as the other.
 *
 * Validates that the note (or the uid) actually exists.
 * Returns TRUE on success; prints an error otherwise.
 * ------------------------------------------------------------------------- */
static gboolean
action_token_parse(OnDatabase *db, const gchar *token,
                   gint64 *note_id, gint *ord)
{
    gchar *colon = strchr(token, ':');
    if (colon == NULL) {             /* the stable-uid form                 */
        gchar *endp = NULL;          /* end of the parsed uid               */
        gint64 uid = g_ascii_strtoll(token, &endp, 10);
        if (uid <= 0 || endp == NULL || *endp != '\0') {
            fprintf(stderr, "error: bad action item id: %s (expected a UID "
                            "from 'action list --uid', or NOTEID:ORD)\n",
                    token);
            return FALSE;
        }
        if (!on_db_action_find_uid(db, uid, note_id, ord)) {
            fprintf(stderr, "error: no such action item: %s\n", token);
            return FALSE;
        }
        return TRUE;
    }
    if (colon == token || colon[1] == '\0') {
        fprintf(stderr, "error: bad action item id: %s "
                        "(expected NOTEID:ORD, see 'action list')\n", token);
        return FALSE;
    }
    gchar *id_part = g_strndup(token, (gsize)(colon - token));
    OnNoteMeta *meta = note_from_arg(db, id_part);
    g_free(id_part);
    if (meta == NULL)
        return FALSE;
    *note_id = meta->id;
    on_db_note_meta_free(meta);

    gchar *endp = NULL;              /* end of the parsed ordinal           */
    gint64 o = g_ascii_strtoll(colon + 1, &endp, 10);
    if (o < 0 || endp == NULL || *endp != '\0') {
        fprintf(stderr, "error: bad action item id: %s\n", token);
        return FALSE;
    }
    *ord = (gint)o;
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * action_print_line() — write one action item as the CLI's tab-separated
 * record, THE one definition of that layout (shared by `action list` and
 * `action show`, so a consumer needs a single parser):
 *   "NOTEID:ORD<TAB>[x]/[ ]<TAB>due-date<TAB>text"   ('-' = no due date)
 * with_uid prepends the item's STABLE uid as a further FIRST column:
 *   "UID<TAB>NOTEID:ORD<TAB>[x]/[ ]<TAB>due-date<TAB>text"
 * The text (which may itself contain tabs) has to stay last, so any new
 * column can only go in front of the existing ones.
 * ------------------------------------------------------------------------- */
static void
action_print_line(const OnActionItem *it, gboolean with_uid)
{
    gchar *when = (it->due != 0)     /* ISO due date, or NULL               */
                  ? cli_time_str(it->due, TRUE) : NULL;
    if (cli_json) {
        /* JSON always carries the uid: there is no reason for a machine
         * format to hide the one stable way to address an item.            */
        json_element();
        printf("{\"uid\":%" G_GINT64_FORMAT ",\"note_id\":%" G_GINT64_FORMAT
               ",\"ord\":%d,\"done\":%s,\"due\":",
               it->uid, it->note_id, it->ord, it->done ? "true" : "false");
        json_str(when);              /* null when the item has no due date  */
        printf(",\"due_at\":%" G_GINT64_FORMAT ",\"text\":", it->due);
        json_str(it->text);
        putchar('}');
        g_free(when);
        return;
    }
    if (with_uid)
        printf("%" G_GINT64_FORMAT "\t", it->uid);
    printf("%" G_GINT64_FORMAT ":%d\t%s\t%s\t%s\n",
           it->note_id, it->ord,
           it->done ? "[x]" : "[ ]",
           when != NULL ? when : "-",
           it->text);
    g_free(when);
}

/* ---------------------------------------------------------------------------
 * cmd_action_list() — every action item across the visible notes, newest
 * note first, one action_print_line() record each.  `filter` narrows to
 * open or done items; with_uid adds the uid column.  The default form is
 * byte-for-byte what it always was.
 * ------------------------------------------------------------------------- */
typedef enum { ACTION_ALL, ACTION_OPEN, ACTION_DONE } ActionFilter;

static int
cmd_action_list(OnDatabase *db, ActionFilter filter, gboolean with_uid)
{
    GList *items = on_db_action_list(db);
    json_array_begin();
    for (GList *l = items; l != NULL; l = l->next) {
        OnActionItem *it = l->data;  /* one action item                     */
        if ((filter == ACTION_OPEN && it->done) ||
            (filter == ACTION_DONE && !it->done))
            continue;
        action_print_line(it, with_uid);
    }
    json_array_end();
    on_db_action_list_free(items);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_action_show() — print ONE item, addressed by stable uid (or by the
 * positional NOTEID:ORD), as the same record `action list --uid` emits.
 * The point is the O(1) read-back a mirror needs after writing: fetching
 * one pinned item's current text, done state and due date without
 * listing (and diffing) the whole table.
 * The uid column is always present — a caller that asked for one item by
 * uid has no reason to be denied it, and `action list` keeps the shorter
 * default form for the bulk case.
 * Returns 0, or 2 when no item carries that id.
 * ------------------------------------------------------------------------- */
static int
cmd_action_show(OnDatabase *db, const gchar *token)
{
    gint64 note_id;                  /* the item's address                  */
    gint   ord;
    if (!action_token_parse(db, token, &note_id, &ord))
        return 2;

    /* One query for the owning note, then the ord-th row — the same list
     * the mirror itself is built from, so nothing new can drift.          */
    GList *items = on_db_action_list_for_note(db, note_id);
    int    rc = 2;                   /* process exit code                   */
    for (GList *l = items; l != NULL; l = l->next) {
        OnActionItem *it = l->data;  /* candidate row                       */
        if (it->ord != ord)
            continue;
        action_print_line(it, TRUE);
        if (cli_json)
            putchar('\n');           /* a lone object, not an array         */
        rc = 0;
        break;
    }
    if (rc != 0)
        fprintf(stderr, "error: no such action item: %s\n", token);
    on_db_action_list_free(items);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cmd_action_done() — mark one item done (strike its text in the note)
 * or reopen it.  The note content is authoritative: the rewrite resyncs
 * the action_items mirror.  NOTE: with the GUI running and that note
 * open in an editor, the editor's next autosave can overwrite this —
 * same caveat as `note append`/`note set`.
 * ------------------------------------------------------------------------- */
static int
cmd_action_done(OnDatabase *db, const gchar *token, gboolean done)
{
    gint64 note_id;                  /* the item's address                  */
    gint   ord;
    if (!action_token_parse(db, token, &note_id, &ord))
        return 2;
    if (!cli_require_gtk())
        return 2;

    OnApp app = { 0 };               /* headless context: db only           */
    app.db = db;
    if (!on_editor_action_set_done(&app, note_id, ord, done, NULL)) {
        fprintf(stderr, "error: no such action item: %s\n", token);
        return 2;
    }
    printf("%s action item %s\n", done ? "completed" : "reopened", token);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_action_due() — set or clear one item's due date: the "due <date>"
 * suffix of its '!' line is rewritten in the note text.
 *   date_arg — "YYYY-MM-DD" (or M/D/YY), or "-" to clear.
 * ------------------------------------------------------------------------- */
static int
cmd_action_due(OnDatabase *db, const gchar *token, const gchar *date_arg)
{
    gint64 due = 0;                  /* parsed date, 0 = clear              */
    if (g_strcmp0(date_arg, "-") != 0) {
        /* Reuse the one due-date parser via a synthesized "due X" text.    */
        gchar *probe = g_strdup_printf("due %s", date_arg);
        gsize  off;
        gboolean ok = on_action_split_due(probe, &off, &due);
        g_free(probe);
        if (!ok) {
            fprintf(stderr, "error: bad date: %s (use YYYY-MM-DD, or '-' "
                            "to clear)\n", date_arg);
            return 2;
        }
    }

    gint64 note_id;                  /* the item's address                  */
    gint   ord;
    if (!action_token_parse(db, token, &note_id, &ord))
        return 2;
    if (!cli_require_gtk())
        return 2;

    OnApp app = { 0 };               /* headless context: db only           */
    app.db = db;
    if (!on_editor_action_set_due(&app, note_id, ord, due)) {
        fprintf(stderr, "error: no such action item: %s\n", token);
        return 2;
    }
    if (due != 0)
        printf("set due date of action item %s\t%s\n", token, date_arg);
    else
        printf("cleared due date of action item %s\n", token);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_action_text() — rename one item: the text of its '!' line is
 * replaced, keeping the line's '!' prefix, its spacing and any trailing
 * "due <date>", and carrying the done state over.  For an external mirror
 * renaming an item it pinned by uid — that uid survives the rewrite (see
 * on_editor_action_set_text).
 *   content — the new text, or "-" to read it from stdin.
 * ------------------------------------------------------------------------- */
static int
cmd_action_text(OnDatabase *db, const gchar *token, const gchar *content)
{
    gchar *text = cli_read_content(content);   /* the new text (owned)      */
    if (text == NULL)
        return 2;

    /* Two shapes would destroy the item rather than rename it, taking its
     * uid with them: a blank text stops the line being an action item at
     * all, and an embedded newline splits it into two lines.              */
    g_strstrip(text);
    if (*text == '\0') {
        fprintf(stderr, "error: action item text is empty (an item needs "
                        "text; use 'action done' to complete it, or edit "
                        "the note to remove the line)\n");
        g_free(text);
        return 2;
    }
    if (strpbrk(text, "\n\r") != NULL) {
        fprintf(stderr, "error: action item text contains a line break "
                        "(one item is one line)\n");
        g_free(text);
        return 2;
    }

    gint64 note_id;                  /* the item's address                  */
    gint   ord;
    if (!action_token_parse(db, token, &note_id, &ord) ||
        !cli_require_gtk()) {
        g_free(text);
        return 2;
    }

    OnApp app = { 0 };               /* headless context: db only           */
    app.db = db;
    if (!on_editor_action_set_text(&app, note_id, ord, text)) {
        fprintf(stderr, "error: no such action item: %s\n", token);
        g_free(text);
        return 2;
    }
    printf("renamed action item %s\t%s\n", token, text);
    g_free(text);
    return 0;
}

/* cmd_backup() — snapshot the database to a file.                           */
static int
cmd_backup(OnDatabase *db, const gchar *dest)
{
    if (!on_db_backup_to(db, dest)) {
        fprintf(stderr, "error: backup to %s failed\n", dest);
        return 2;
    }
    printf("backed up to %s\n", dest);
    return 0;
}

/* cmd_export() — export every note as HTML or Markdown into a directory.    */
static int
cmd_export(OnDatabase *db, const gchar *dir, OnExportFormat format)
{
    if (!gtk_init_check(NULL, NULL)) {
        fprintf(stderr, "error: GTK could not initialize (needed to "
                        "render notes)\n");
        return 2;
    }
    /* The exporter only touches app->db.                                   */
    OnApp app = { 0 };
    app.db = db;

    gchar *err = NULL;               /* exporter error message              */
    gint n = on_export_all(&app, dir, format, &err);
    if (n < 0) {
        fprintf(stderr, "error: %s\n", err != NULL ? err : "export failed");
        g_free(err);
        return 2;
    }
    printf("exported %d note%s to %s\n", n, n == 1 ? "" : "s", dir);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_trash_list() — what the library's Trash section shows: the folders
 * that were deleted (their subtrees go with them, implicitly) and the
 * individually deleted notes.
 *
 * Plain output labels each row so one listing can carry both kinds:
 *   "folder<TAB>ID<TAB>NAME"   /   "note<TAB>ID<TAB>MODIFIED<TAB>TITLE"
 * JSON emits one array of objects, each with a "kind".  Folder ids matter
 * here: `folder restore` takes an id, because a trashed folder's path no
 * longer resolves.
 * ------------------------------------------------------------------------- */
static int
cmd_trash_list(OnDatabase *db)
{
    GList *folders = on_db_folder_list_trashed(db);
    GList *notes   = on_db_note_list_trashed(db);

    json_array_begin();
    for (GList *l = folders; l != NULL; l = l->next) {
        OnFolder *f = l->data;       /* one deleted folder                  */
        if (cli_json) {
            json_element();
            printf("{\"kind\":\"folder\",\"id\":%" G_GINT64_FORMAT
                   ",\"name\":", f->id);
            json_str(f->name);
            putchar('}');
        } else {
            printf("folder\t%" G_GINT64_FORMAT "\t%s\n", f->id, f->name);
        }
    }
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* one deleted note                    */
        if (cli_json) {
            json_element();
            printf("{\"kind\":\"note\",\"id\":%" G_GINT64_FORMAT
                   ",\"title\":", m->id);
            json_str(m->title);
            printf(",");
            json_time("modified", m->updated_at);
            putchar('}');
        } else {
            gchar *when = cli_time_str(m->updated_at, FALSE);
            printf("note\t%" G_GINT64_FORMAT "\t%s\t%s\n",
                   m->id, when, m->title);
            g_free(when);
        }
    }
    json_array_end();

    on_db_note_list_free(notes);
    on_db_folder_list_free(folders);
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_trash_empty() — permanently delete everything in the Trash.  This is
 * the one CLI command with no undo, so it insists on --yes rather than
 * trusting an argument list that came from a script.
 * ------------------------------------------------------------------------- */
static int
cmd_trash_empty(OnDatabase *db, gboolean confirmed)
{
    gint n = on_db_trash_count(db);
    if (!confirmed) {
        fprintf(stderr, "error: 'trash empty' permanently deletes %d item%s "
                        "and cannot be undone; pass --yes to confirm\n",
                n, n == 1 ? "" : "s");
        return 2;
    }
    if (n == 0) {
        printf("trash is already empty\n");
        return 0;
    }
    if (!on_db_trash_empty(db)) {
        fprintf(stderr, "error: could not empty the trash\n");
        return 2;
    }
    printf("emptied trash\t%d item%s\n", n, n == 1 ? "" : "s");
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_stats() — database-wide counts in one call: notes (total and
 * visible), folders, tags, pinned notes, trash size and action items.
 * "key<TAB>value" lines, or one JSON object.
 * ------------------------------------------------------------------------- */
static int
cmd_stats(OnDatabase *db)
{
    gint notes = 0, folders = 0, tags = 0;      /* whole-table totals       */
    on_db_totals(db, &notes, &folders, &tags);
    gint visible = on_db_note_count_visible(db);
    gint pinned  = on_db_note_count_pinned(db);
    gint trash   = on_db_trash_count(db);
    gint acts = 0, acts_open = 0;               /* action-item counts       */
    on_db_action_counts(db, &acts, &acts_open);

    if (cli_json) {
        printf("{\"notes\":%d,\"notes_visible\":%d,\"folders\":%d,"
               "\"tags\":%d,\"pinned\":%d,\"trash\":%d,"
               "\"actions\":%d,\"actions_open\":%d,\"database\":",
               notes, visible, folders, tags, pinned, trash,
               acts, acts_open);
        json_str(db->path);
        printf("}\n");
    } else {
        printf("notes\t%d\n", notes);
        printf("notes-visible\t%d\n", visible);
        printf("folders\t%d\n", folders);
        printf("tags\t%d\n", tags);
        printf("pinned\t%d\n", pinned);
        printf("trash\t%d\n", trash);
        printf("actions\t%d\t%d open\n", acts, acts_open);
        printf("database\t%s\n", db->path != NULL ? db->path : "");
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * cmd_search() — case-insensitive search of every visible note's title +
 * plain text (the search window's strategy: the body_text cache fetched
 * as ONE map query, extraction fallback per unfilled row).  Prints one
 * "ID<TAB>MODIFIED<TAB>/Folder/Sub/Title" line per hit; no GTK needed.
 *   query     — the query, in the same language the search window takes:
 *               words ANDed, "quoted phrases", -exclusions (search_query.h).
 *               Quote the whole thing for the shell, which eats the quotes
 *               and would read a leading '-' as an option.
 *   use_regex — TRUE to treat `query` as one GRegex pattern (still
 *               case-insensitive) instead, with no term syntax.
 * ------------------------------------------------------------------------- */
static int
cmd_search(OnDatabase *db, const gchar *query, gboolean use_regex)
{
    GError  *err = NULL;             /* regex compile failure               */
    OnQuery *q = on_query_new(query, FALSE, use_regex, &err);
    if (q == NULL) {
        fprintf(stderr, "error: bad pattern: %s\n", err->message);
        g_clear_error(&err);
        return 2;
    }
    if (on_query_is_empty(q)) {
        fprintf(stderr, "error: empty search query\n");
        on_query_free(q);
        return 2;
    }

    GList      *notes  = on_db_note_list_all(db, FALSE);
    GHashTable *bodies = on_db_note_text_map(db, 0);    /* id → body text      */
    GHashTable *paths  = on_db_folder_path_map(db);  /* folder id → path    */

    /* Hits print as they are found, so the array is framed around the walk
     * rather than collected first.                                         */
    json_array_begin();
    for (GList *l = notes; l != NULL; l = l->next) {
        OnNoteMeta *m = l->data;     /* candidate note                      */
        const gchar *body = g_hash_table_lookup(bodies, &m->id);
        gchar *fallback = NULL;      /* extracted text for unfilled rows    */
        if (body == NULL) {
            fallback = on_note_text_cached(db, m->id);
            body = fallback;
        }

        /* Same matcher the search window's worker uses.                    */
        if (on_query_matches(q, m->title, body))
            print_note_line(m, paths);
        g_free(fallback);
    }
    json_array_end();

    g_hash_table_destroy(paths);
    g_hash_table_destroy(bodies);
    on_db_note_list_free(notes);
    on_query_free(q);
    return 0;
}

/* usage() — the help text.                                                  */
static int
usage(FILE *out)
{
    fputs(
"Usage: notes [COMMAND ...]   (no command starts the GUI)\n"
"\n"
"  tag list                          print every tag with its note count\n"
"  tag notes NAME                    print the notes labeled with a tag\n"
"  tag delete NAME                   remove a tag from the database\n"
"\n"
"  folder list                       print the folder tree with note counts\n"
"  folder info PATH                  id, path, emoji, AI mode and contents\n"
"  folder add PATH                   create a folder path (like mkdir -p)\n"
"  folder rename PATH NAME           rename a folder in place\n"
"  folder move PATH DEST|/           re-nest a folder (with its subtree)\n"
"  folder emoji PATH EMOJI|-         set the sidebar emoji ('-' clears it)\n"
"  folder ai-mode PATH MODE          normal | project | custom\n"
"  folder sort PATH|/                order its subfolders alphabetically\n"
"  folder restore ID                 take a folder out of the Trash (by ID,\n"
"                                    from 'trash list': a trashed folder's\n"
"                                    path no longer resolves)\n"
"  folder delete [--permanent] PATH  move a folder AND everything inside to\n"
"                                    the Trash (--permanent: delete outright)\n"
"\n"
"  note list [PATH|--all|--recent|--pinned]\n"
"                                    print ID/modified/title per note\n"
"  note info ID                      title, path, dates, pinned/trashed,\n"
"                                    tags, image and action-item counts\n"
"  note cat ID [--md]                print a note's plain text (--md:\n"
"                                    Markdown keeping the formatting;\n"
"                                    images become placeholders)\n"
"  note new [--folder PATH] TEXT     create a note (TEXT '-' reads stdin;\n"
"                                    the first line becomes the title)\n"
"  note append ID TEXT|-             append plain text to a note, on a\n"
"                                    fresh line (existing content is kept)\n"
"  note set ID TEXT|-                REPLACE a note's content with plain\n"
"                                    text (formatting, images, tables and\n"
"                                    tags of the old content are LOST)\n"
"  note delete [--permanent] ID...   move notes to the Trash\n"
"                                    (--permanent: delete outright)\n"
"  note restore ID [ID...]           restore notes from the Trash\n"
"  note move ID [ID...] PATH         move notes into a folder ('/' = top)\n"
"  note tags ID                      print a note's tags, one per line\n"
"  note tag ID NAME                  add a #tag to a note (the literal\n"
"                                    '#NAME' token is appended to the text)\n"
"  note untag ID NAME                remove a #tag from a note's text\n"
"  note pin ID...                    pin notes (sidebar's Pinned Notes)\n"
"  note unpin ID...                  unpin them again\n"
"  note images ID                    list a note's images: N, bytes, WxH\n"
"                                    (N is 1-based, matching the\n"
"                                    '![image N]()' of 'note cat --md')\n"
"  note image ID N FILE              write image N out, byte for byte as\n"
"                                    stored (no decode, no re-encode)\n"
"  note add-image ID FILE            append an image file to a note\n"
"  note set-modified ID TIMESTAMP    set a note's modified date (UNIX\n"
"                                    seconds; for importers)\n"
"  note open PATH                    open a note's editor in the running\n"
"                                    instance (PATH = id or Folder/Title);\n"
"                                    starts Notes if it is not running\n"
"\n"
"  action list [--open|--done]       print every '!' action item across\n"
"              [--uid]               the notes: NOTEID:ORD, [x]/[ ],\n"
"                                    due date ('-' = none), text.\n"
"                                    --uid adds the item's stable UID as\n"
"                                    a first column: that id survives\n"
"                                    editing, reordering and renumbering,\n"
"                                    while NOTEID:ORD does not\n"
"  action show UID|NOTEID:ORD        print one item as a single UID-first\n"
"                                    'action list --uid' record (read one\n"
"                                    pinned item back without listing all)\n"
"  action done UID|NOTEID:ORD        mark an item done (strikes its line\n"
"                                    in the note text)\n"
"  action undone UID|NOTEID:ORD      reopen a completed item\n"
"  action due UID|NOTEID:ORD DATE|-  set the item's due date (written\n"
"                                    into the note line as 'due DATE';\n"
"                                    '-' clears it)\n"
"  action text UID|NOTEID:ORD TEXT|- rename an item: its '!' line's text\n"
"                                    is replaced, keeping the done state\n"
"                                    and any 'due DATE'; the UID survives\n"
"                                    ('-' reads the text from stdin)\n"
"\n"
"  search TEXT [--regex]             case-insensitive search of all note\n"
"                                    titles + text; prints one\n"
"                                    ID/modified/path line per hit.\n"
"                                    TEXT is ANDed words, \"quoted\n"
"                                    phrases\" and -excluded words (quote\n"
"                                    the whole query for the shell);\n"
"                                    --regex takes it as one pattern\n"
"\n"
"  trash list                        what the Trash holds: deleted folders\n"
"                                    (kind/id/name) and notes\n"
"  trash empty --yes                 PERMANENTLY delete all of it\n"
"\n"
"  stats                             database-wide counts (notes, folders,\n"
"                                    tags, pinned, trash, action items)\n"
"\n"
"  quicknote                         create a note in the root folder and\n"
"                                    open its editor in the running instance\n"
"                                    (starts Notes if not running)\n"
"\n"
"  backup FILE.db                    snapshot the database to FILE.db\n"
"  export-md DIR                     export all notes as Markdown into DIR\n"
"  export-html DIR                   export all notes as HTML into DIR\n"
"  help                              show this text\n"
"\n"
"  --json                            on the commands that print records\n"
"                                    (note list/info/cat/tags/images,\n"
"                                    folder list/info, tag list/notes,\n"
"                                    action list/show, search, trash list,\n"
"                                    stats): emit JSON instead of tab-\n"
"                                    separated lines, so a title or item\n"
"                                    text containing a tab or newline\n"
"                                    cannot shift the columns\n",
        out);
    return out == stderr ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * dispatch_tag()/dispatch_folder()/dispatch_note() — verb handling for
 * each noun group.  argv/argc are the arguments AFTER the verb.
 * ------------------------------------------------------------------------- */
static int
dispatch_tag(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc == 0)
        return cmd_list_tags(db);
    if (g_strcmp0(verb, "notes") == 0 && argc == 1)
        return cmd_tag_notes(db, argv[0]);
    if (g_strcmp0(verb, "delete") == 0 && argc == 1)
        return cmd_delete_tag(db, argv[0]);
    return usage(stderr);
}

static int
dispatch_folder(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc == 0)
        return cmd_list_folders(db);
    if (g_strcmp0(verb, "add") == 0 && argc == 1)
        return cmd_add_folder(db, argv[0]);
    if (g_strcmp0(verb, "info") == 0 && argc == 1)
        return cmd_folder_info(db, argv[0]);
    if (g_strcmp0(verb, "rename") == 0 && argc == 2)
        return cmd_folder_rename(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "move") == 0 && argc == 2)
        return cmd_folder_move(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "emoji") == 0 && argc == 2)
        return cmd_folder_emoji(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "ai-mode") == 0 && argc == 2)
        return cmd_folder_ai_mode(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "sort") == 0 && argc == 1)
        return cmd_folder_sort(db, argv[0]);
    if (g_strcmp0(verb, "restore") == 0 && argc == 1)
        return cmd_folder_restore(db, argv[0]);
    if (g_strcmp0(verb, "delete") == 0) {
        /* folder delete [--permanent] PATH  (flag accepted either side)    */
        if (argc == 1 && g_strcmp0(argv[0], "--permanent") != 0)
            return cmd_delete_folder(db, argv[0], FALSE);
        if (argc == 2 && g_strcmp0(argv[0], "--permanent") == 0)
            return cmd_delete_folder(db, argv[1], TRUE);
        if (argc == 2 && g_strcmp0(argv[1], "--permanent") == 0)
            return cmd_delete_folder(db, argv[0], TRUE);
        return usage(stderr);
    }
    return usage(stderr);
}

static int
dispatch_action(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0) {
        /* Flags in any order; anything unrecognised is a usage error (exit
         * 1), which is how a caller probes an older build for --uid.       */
        ActionFilter filter   = ACTION_ALL;
        gboolean     with_uid = FALSE;
        for (int i = 0; i < argc; i++) {
            if (g_strcmp0(argv[i], "--open") == 0)
                filter = ACTION_OPEN;
            else if (g_strcmp0(argv[i], "--done") == 0)
                filter = ACTION_DONE;
            else if (g_strcmp0(argv[i], "--uid") == 0)
                with_uid = TRUE;
            else
                return usage(stderr);
        }
        return cmd_action_list(db, filter, with_uid);
    }
    if (g_strcmp0(verb, "show") == 0 && argc == 1)
        return cmd_action_show(db, argv[0]);
    if (g_strcmp0(verb, "done") == 0 && argc == 1)
        return cmd_action_done(db, argv[0], TRUE);
    if (g_strcmp0(verb, "undone") == 0 && argc == 1)
        return cmd_action_done(db, argv[0], FALSE);
    if (g_strcmp0(verb, "due") == 0 && argc == 2)
        return cmd_action_due(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "text") == 0 && argc == 2)
        return cmd_action_text(db, argv[0], argv[1]);
    return usage(stderr);
}

static int
dispatch_trash(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc == 0)
        return cmd_trash_list(db);
    if (g_strcmp0(verb, "empty") == 0) {
        if (argc == 0)
            return cmd_trash_empty(db, FALSE);
        if (argc == 1 && g_strcmp0(argv[0], "--yes") == 0)
            return cmd_trash_empty(db, TRUE);
    }
    return usage(stderr);
}

static int
dispatch_note(OnDatabase *db, const char *verb, char **argv, int argc)
{
    if (g_strcmp0(verb, "list") == 0 && argc <= 1)
        return cmd_list_notes(db, argc == 1 ? argv[0] : "--all");
    if (g_strcmp0(verb, "cat") == 0) {
        if (argc == 1)
            return cmd_cat_note(db, argv[0], FALSE);
        if (argc == 2 && g_strcmp0(argv[1], "--md") == 0)
            return cmd_cat_note(db, argv[0], TRUE);
        return usage(stderr);
    }
    if (g_strcmp0(verb, "new") == 0) {
        /* note new [--folder PATH] CONTENT                                 */
        if (argc == 1)
            return cmd_new_note(db, "", argv[0]);
        if (argc == 3 && g_strcmp0(argv[0], "--folder") == 0)
            return cmd_new_note(db, argv[1], argv[2]);
        return usage(stderr);
    }
    if (g_strcmp0(verb, "append") == 0 && argc == 2)
        return cmd_append_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "set") == 0 && argc == 2)
        return cmd_set_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "delete") == 0 && argc >= 1) {
        /* note delete [--permanent] ID...  (flag accepted anywhere)        */
        gboolean permanent = FALSE;
        char **ids = g_new(char *, (gsize)argc);
        int n = 0;                   /* ids kept after flag filtering       */
        for (int i = 0; i < argc; i++) {
            if (g_strcmp0(argv[i], "--permanent") == 0)
                permanent = TRUE;
            else
                ids[n++] = argv[i];
        }
        int rc = (n > 0) ? cmd_delete_notes(db, ids, n, permanent)
                         : usage(stderr);
        g_free(ids);
        return rc;
    }
    if (g_strcmp0(verb, "restore") == 0 && argc >= 1)
        return cmd_restore_notes(db, argv, argc);
    if (g_strcmp0(verb, "move") == 0 && argc >= 2)
        return cmd_move_notes(db, argv, argc - 1, argv[argc - 1]);
    if (g_strcmp0(verb, "tags") == 0 && argc == 1)
        return cmd_note_tags(db, argv[0]);
    if (g_strcmp0(verb, "tag") == 0 && argc == 2)
        return cmd_tag_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "untag") == 0 && argc == 2)
        return cmd_untag_note(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "info") == 0 && argc == 1)
        return cmd_note_info(db, argv[0]);
    if (g_strcmp0(verb, "pin") == 0 && argc >= 1)
        return cmd_note_pin(db, argv, argc, TRUE);
    if (g_strcmp0(verb, "unpin") == 0 && argc >= 1)
        return cmd_note_pin(db, argv, argc, FALSE);
    if (g_strcmp0(verb, "images") == 0 && argc == 1)
        return cmd_note_images(db, argv[0]);
    if (g_strcmp0(verb, "image") == 0 && argc == 3)
        return cmd_note_image(db, argv[0], argv[1], argv[2]);
    if (g_strcmp0(verb, "add-image") == 0 && argc == 2)
        return cmd_add_image(db, argv[0], argv[1]);
    if (g_strcmp0(verb, "set-modified") == 0 && argc == 2)
        return cmd_set_modified(db, argv[0], argv[1]);
    return usage(stderr);
}

/* ---------------------------------------------------------------------------
 * cli_gui_command() — handle the two commands that act on a GUI editor:
 * "quicknote" and "note open PATH".  Each first tries to reach a running
 * instance over the IPC socket; if one answers it did the work and we exit.
 * Otherwise the request is recorded as a pending action and -1 is returned so
 * main() starts the GUI, which performs it at activate.
 *   remote_cmd — the line to send a running instance ("quicknote"/"open ...").
 *   set_pending — records the same action for the not-running case.
 * Returns a process exit code, or -1 to start the GUI.
 * ------------------------------------------------------------------------- */
static int
cli_gui_command(const gchar *remote_cmd, void (*set_pending)(void),
                const gchar *pending_path)
{
    gchar *reply = NULL;             /* running instance's message           */
    OnIpcResult r = on_ipc_try_remote(remote_cmd, &reply);
    if (r == ON_IPC_NO_SERVER) {
        /* No instance: arrange for the GUI we are about to start to do it.  */
        if (set_pending != NULL)
            set_pending();
        else
            on_ipc_set_pending_open(pending_path);
        return -1;
    }
    if (reply != NULL && *reply != '\0')
        fprintf(r == ON_IPC_OK ? stdout : stderr, "%s\n", reply);
    g_free(reply);
    return (r == ON_IPC_OK) ? 0 : 2;
}

gboolean
on_cli_command_reads_stdin(int argc, char **argv)
{
    /* "note new … -", "note append ID -", "note set ID -" and
     * "action text ID -": a content argument of "-" consumes stdin.        */
    if (argc < 4 || g_strcmp0(argv[argc - 1], "-") != 0)
        return FALSE;
    const char *verb = argv[2];
    if (g_strcmp0(argv[1], "action") == 0)
        return g_strcmp0(verb, "text") == 0;
    if (g_strcmp0(argv[1], "note") != 0)
        return FALSE;
    return g_strcmp0(verb, "new") == 0 ||
           g_strcmp0(verb, "append") == 0 ||
           g_strcmp0(verb, "set") == 0;
}

gboolean
on_cli_command_mutates(int argc, char **argv)
{
    if (argc < 2)
        return FALSE;
    const char *cmd  = argv[1];
    const char *verb = (argc >= 3) ? argv[2] : "";
    if (g_strcmp0(cmd, "tag") == 0)
        return g_strcmp0(verb, "delete") == 0;
    if (g_strcmp0(cmd, "folder") == 0)
        return g_strcmp0(verb, "add") == 0 ||
               g_strcmp0(verb, "delete") == 0 ||
               g_strcmp0(verb, "rename") == 0 ||
               g_strcmp0(verb, "move") == 0 ||
               g_strcmp0(verb, "emoji") == 0 ||
               g_strcmp0(verb, "ai-mode") == 0 ||
               g_strcmp0(verb, "sort") == 0 ||
               g_strcmp0(verb, "restore") == 0;
    if (g_strcmp0(cmd, "trash") == 0)
        return g_strcmp0(verb, "empty") == 0;
    if (g_strcmp0(cmd, "action") == 0)
        return g_strcmp0(verb, "done") == 0 ||
               g_strcmp0(verb, "undone") == 0 ||
               g_strcmp0(verb, "due") == 0 ||
               g_strcmp0(verb, "text") == 0;   /* "show" is read-only       */
    if (g_strcmp0(cmd, "note") == 0)
        return g_strcmp0(verb, "new") == 0 ||
               g_strcmp0(verb, "append") == 0 ||
               g_strcmp0(verb, "set") == 0 ||
               g_strcmp0(verb, "delete") == 0 ||
               g_strcmp0(verb, "restore") == 0 ||
               g_strcmp0(verb, "move") == 0 ||
               g_strcmp0(verb, "tag") == 0 ||
               g_strcmp0(verb, "untag") == 0 ||
               g_strcmp0(verb, "add-image") == 0 ||
               g_strcmp0(verb, "set-modified") == 0 ||
               g_strcmp0(verb, "pin") == 0 ||
               g_strcmp0(verb, "unpin") == 0;
    return FALSE;         /* search / backup / export-* are read-only       */
}

/* ---------------------------------------------------------------------------
 * cli_is_noun() — is `cmd` one of the noun groups that take a verb?  Both
 * the entry point (deciding whether to handle the command at all) and the
 * dispatcher (rejecting a bare noun) ask this, and used to spell out the
 * same four comparisons.
 * ------------------------------------------------------------------------- */
static gboolean
cli_is_noun(const char *cmd)
{
    return g_strcmp0(cmd, "tag")    == 0 ||
           g_strcmp0(cmd, "folder") == 0 ||
           g_strcmp0(cmd, "note")   == 0 ||
           g_strcmp0(cmd, "action") == 0 ||
           g_strcmp0(cmd, "trash")  == 0;
}

/* ---------------------------------------------------------------------------
 * cli_json_capable() — does this command take --json?  Only the commands
 * that PRINT records do; everywhere else "--json" is ordinary text (a note
 * whose content is the literal "--json" must survive `note new`), so the
 * flag is recognised here and nowhere else.
 * ------------------------------------------------------------------------- */
static gboolean
cli_json_capable(const char *cmd, const char *verb)
{
    if (g_strcmp0(cmd, "note") == 0)
        return g_strcmp0(verb, "list") == 0 ||
               g_strcmp0(verb, "info") == 0 ||
               g_strcmp0(verb, "cat") == 0 ||
               g_strcmp0(verb, "tags") == 0 ||
               g_strcmp0(verb, "images") == 0;
    if (g_strcmp0(cmd, "folder") == 0)
        return g_strcmp0(verb, "list") == 0 || g_strcmp0(verb, "info") == 0;
    if (g_strcmp0(cmd, "tag") == 0)
        return g_strcmp0(verb, "list") == 0 || g_strcmp0(verb, "notes") == 0;
    if (g_strcmp0(cmd, "action") == 0)
        return g_strcmp0(verb, "list") == 0 || g_strcmp0(verb, "show") == 0;
    if (g_strcmp0(cmd, "trash") == 0)
        return g_strcmp0(verb, "list") == 0;
    return g_strcmp0(cmd, "search") == 0 || g_strcmp0(cmd, "stats") == 0;
}

/* ---------------------------------------------------------------------------
 * cli_json_take() — set the output mode for this invocation and remove the
 * flag from the argument vector, so every verb below still validates its own
 * argument count without knowing the flag exists.
 *   argv — the invocation, copied by the caller (this shuffles it).
 *   argc — its length, lowered by one when the flag was taken.
 * ------------------------------------------------------------------------- */
static void
cli_json_take(char **argv, int *argc)
{
    /* Assign, never OR: inside a GUI instance serving remote CLI calls the
     * flag outlives the command that set it.                               */
    cli_json = FALSE;
    if (!cli_json_capable(argv[1], (*argc >= 3) ? argv[2] : ""))
        return;
    for (int i = 2; i < *argc; i++) {
        if (g_strcmp0(argv[i], "--json") != 0)
            continue;
        cli_json = TRUE;
        for (int j = i; j + 1 < *argc; j++)
            argv[j] = argv[j + 1];
        (*argc)--;
        return;
    }
}

/* Forward declaration: the router below, called with the flags stripped.   */
static int cli_dispatch_verbs(OnDatabase *db, int argc, char **argv);

int
on_cli_dispatch_db(OnDatabase *db, int argc, char **argv)
{
    /* --json is stripped from a COPY, leaving the caller's argv alone (the
     * IPC server hands us the vector it also logs).                        */
    char **args = g_memdup2(argv, (gsize)argc * sizeof *argv);
    cli_json_take(args, &argc);
    int rc = cli_dispatch_verbs(db, argc, args);
    g_free(args);
    return rc;
}

/* ---------------------------------------------------------------------------
 * cli_dispatch_verbs() — route one (already flag-stripped) invocation to
 * its command.  Split from on_cli_dispatch_db so every exit path frees the
 * argument copy.
 * ------------------------------------------------------------------------- */
static int
cli_dispatch_verbs(OnDatabase *db, int argc, char **argv)
{
    const char *cmd = argv[1];       /* the noun/flat command               */

    /* Noun groups need a verb.                                             */
    if (cli_is_noun(cmd) && argc < 3)
        return usage(stderr);

    if (g_strcmp0(cmd, "tag") == 0)
        return dispatch_tag(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "folder") == 0)
        return dispatch_folder(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "note") == 0)
        return dispatch_note(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "action") == 0)
        return dispatch_action(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "trash") == 0)
        return dispatch_trash(db, argv[2], argv + 3, argc - 3);
    if (g_strcmp0(cmd, "stats") == 0)
        return (argc == 2) ? cmd_stats(db) : usage(stderr);
    if (g_strcmp0(cmd, "search") == 0) {
        if (argc == 3)
            return cmd_search(db, argv[2], FALSE);
        if (argc == 4 && g_strcmp0(argv[3], "--regex") == 0)
            return cmd_search(db, argv[2], TRUE);
        return usage(stderr);
    }
    if (g_strcmp0(cmd, "backup") == 0)
        return (argc == 3) ? cmd_backup(db, argv[2]) : usage(stderr);
    if (g_strcmp0(cmd, "export-md") == 0)
        return (argc == 3) ? cmd_export(db, argv[2], ON_EXPORT_MARKDOWN)
                           : usage(stderr);
    if (g_strcmp0(cmd, "export-html") == 0)
        return (argc == 3) ? cmd_export(db, argv[2], ON_EXPORT_HTML)
                           : usage(stderr);
    return usage(stderr);            /* unreachable for validated commands   */
}

int
on_cli_run(int argc, char **argv)
{
    if (argc < 2)
        return -1;                   /* no subcommand: start the GUI        */
    const char *cmd = argv[1];

    if (g_strcmp0(cmd, "help") == 0 || g_strcmp0(cmd, "--help") == 0 ||
        g_strcmp0(cmd, "-h") == 0)
        return usage(stdout);

    /* GUI-interacting commands: open an editor in the running instance, or
     * start the GUI and do it there.  Handled before the headless db path.  */
    if (g_strcmp0(cmd, "quicknote") == 0) {
        if (argc != 2)
            return usage(stderr);
        return cli_gui_command("quicknote", on_ipc_set_pending_quicknote,
                               NULL);
    }
    if (g_strcmp0(cmd, "note") == 0 && argc >= 3 &&
        g_strcmp0(argv[2], "open") == 0) {
        if (argc != 4)
            return usage(stderr);
        gchar *remote = g_strdup_printf("open %s", argv[3]);
        int rc = cli_gui_command(remote, NULL, argv[3]);
        g_free(remote);
        return rc;
    }

    /* Anything that is not a known noun/command falls through to GTK
     * (which has its own option handling for things like --display).       */
    gboolean is_flat = g_strcmp0(cmd, "search") == 0 ||
                       g_strcmp0(cmd, "stats") == 0 ||
                       g_strcmp0(cmd, "backup") == 0 ||
                       g_strcmp0(cmd, "export-md") == 0 ||
                       g_strcmp0(cmd, "export-html") == 0;
    if (!cli_is_noun(cmd) && !is_flat)
        return -1;

    /* Route every data command through a running instance when one exists,
     * so a single process owns the database connection and the GUI refreshes
     * live.  With no instance, run headless against our own connection.      */
    gboolean ran = FALSE;            /* did a running instance handle it?    */
    int rc = on_ipc_try_remote_run(argc, argv, &ran);
    if (ran)
        return rc;

    OnDatabase *db = cli_open_db();
    if (db == NULL)
        return 2;
    rc = on_cli_dispatch_db(db, argc, argv);
    on_db_close(db);
    return rc;
}
