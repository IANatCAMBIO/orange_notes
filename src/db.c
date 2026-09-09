/* ===========================================================================
 * db.c — SQLite persistence layer for Notes (implementation)
 *
 * See db.h for the public API and schema overview.  All functions log
 * failures through g_warning() and return a "failed" value rather than
 * aborting, so the UI can degrade gracefully.
 * =========================================================================== */

#include "db.h"

#include <string.h>
#include <glib/gstdio.h>

/* ---------------------------------------------------------------------------
 * SCHEMA_SQL — DDL executed every time the database is opened.
 * Every statement uses IF NOT EXISTS so re-running is harmless.
 * ------------------------------------------------------------------------- */
static const char *SCHEMA_SQL =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS folders ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  parent_id  INTEGER REFERENCES folders(id) ON DELETE CASCADE,"
    "  name       TEXT NOT NULL,"
    "  sort_order INTEGER NOT NULL DEFAULT 0"
    ");"
    "CREATE TABLE IF NOT EXISTS notes ("
    "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  folder_id  INTEGER REFERENCES folders(id) ON DELETE CASCADE,"
    "  title      TEXT NOT NULL DEFAULT 'New Note',"
    "  content    BLOB,"
    "  sort_order INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL,"
    "  updated_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS tags ("
    "  id   INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT NOT NULL UNIQUE"
    ");"
    "CREATE TABLE IF NOT EXISTS note_tags ("
    "  note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
    "  tag_id  INTEGER NOT NULL REFERENCES tags(id)  ON DELETE CASCADE,"
    "  PRIMARY KEY (note_id, tag_id)"
    ");"
    "CREATE TABLE IF NOT EXISTS action_items ("
    "  note_id INTEGER NOT NULL REFERENCES notes(id) ON DELETE CASCADE,"
    "  ord     INTEGER NOT NULL,"
    "  text    TEXT NOT NULL,"
    "  done    INTEGER NOT NULL DEFAULT 0,"
    "  due     INTEGER NOT NULL DEFAULT 0,"
    "  uid     INTEGER NOT NULL DEFAULT 0,"
    "  PRIMARY KEY (note_id, ord)"
    ");"
    /* The uid high-water mark: one row, monotonically increasing, so a
     * uid retired by a deleted item is never handed out again.             */
    "CREATE TABLE IF NOT EXISTS action_uid_seq ("
    "  id   INTEGER PRIMARY KEY CHECK (id = 1),"
    "  next INTEGER NOT NULL"
    ");"
    "INSERT OR IGNORE INTO action_uid_seq (id, next) VALUES (1, 1);"
    /* NOTE: the index on action_items(uid) is NOT here — on a database
     * predating that column this string runs BEFORE the ALTER that adds
     * it, and indexing a missing column fails the whole batch (which
     * means on_db_open returns NULL and the app refuses to start).  It is
     * created after the migrations instead, with the Trash view.          */
    "CREATE INDEX IF NOT EXISTS idx_notes_folder  ON notes(folder_id);"
    "CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);"
    "CREATE INDEX IF NOT EXISTS idx_note_tags_tag ON note_tags(tag_id);";

/* ---------------------------------------------------------------------------
 * TRASH_VIEW_SQL — the recursive closure of the Trash: every folder that
 * is flagged trashed OR sits anywhere below a flagged folder.  Created
 * AFTER the column migrations in on_db_open (it references
 * folders.trashed).  Notes inside these folders are implicitly trashed
 * without carrying their own flag.
 * ------------------------------------------------------------------------- */
static const char *TRASH_VIEW_SQL =
    "CREATE VIEW IF NOT EXISTS trash_folder_ids AS "
    "WITH RECURSIVE tf(id) AS ("
    "  SELECT id FROM folders WHERE trashed=1"
    "  UNION"
    "  SELECT f.id FROM folders f JOIN tf ON f.parent_id = tf.id"
    ") SELECT id FROM tf;";

/* WHERE fragment: a note is visible in normal (non-Trash) views — not
 * directly trashed and not inside a trashed folder's subtree.               */
#define NOTE_VISIBLE_SQL \
    "trashed=0 AND (folder_id IS NULL OR " \
    "folder_id NOT IN (SELECT id FROM trash_folder_ids))"

/* Remove tags that no longer label any note; run (via exec_simple) after
 * anything that can delete note_tags rows, so the library's tag list
 * stays tidy.                                                                */
#define PRUNE_ORPHAN_TAGS_SQL \
    "DELETE FROM tags WHERE id NOT IN (SELECT tag_id FROM note_tags)"

/* ---------------------------------------------------------------------------
 * exec_simple() — run a parameterless SQL string, logging any error.
 *   db  — open database.
 *   sql — SQL text to execute.
 * Returns TRUE if the statement(s) executed without error.
 * ------------------------------------------------------------------------- */
static gboolean
exec_simple(OnDatabase *db, const char *sql)
{
    char *errmsg = NULL;                 /* sqlite-allocated error string   */
    if (sqlite3_exec(db->handle, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        g_warning("db: exec failed: %s (sql: %.80s)", errmsg, sql);
        sqlite3_free(errmsg);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * prepare() — wrap sqlite3_prepare_v2 with error logging.
 *   db  — open database.
 *   sql — single SQL statement with '?' placeholders.
 * Returns a prepared statement to finalize with sqlite3_finalize(), or
 * NULL on error.
 * ------------------------------------------------------------------------- */
static sqlite3_stmt *
prepare(OnDatabase *db, const char *sql)
{
    sqlite3_stmt *stmt = NULL;           /* the compiled statement          */
    if (sqlite3_prepare_v2(db->handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        g_warning("db: prepare failed: %s (sql: %.80s)",
                  sqlite3_errmsg(db->handle), sql);
        return NULL;
    }
    return stmt;
}

/* ---------------------------------------------------------------------------
 * bind_id_or_null() — bind a folder/parent id, mapping 0 to SQL NULL.
 * The schema uses NULL (not 0) to mean "top level", so every id bind for
 * a nullable column goes through this helper.
 *   stmt — statement to bind into.
 *   idx  — 1-based parameter index.
 *   id   — the id value; 0 means NULL.
 * ------------------------------------------------------------------------- */
static void
bind_id_or_null(sqlite3_stmt *stmt, int idx, gint64 id)
{
    if (id > 0)
        sqlite3_bind_int64(stmt, idx, id);
    else
        sqlite3_bind_null(stmt, idx);
}

/* ---------------------------------------------------------------------------
 * stmt_done() — run a fully-bound single DML statement to completion.
 *   db   — open database (for the error message).
 *   stmt — prepared and bound statement, or NULL (a failed prepare);
 *          consumed (finalized) in every case.
 * Steps once and logs a g_warning() on anything but SQLITE_DONE.
 * Returns TRUE when the statement completed.
 * ------------------------------------------------------------------------- */
static gboolean
stmt_done(OnDatabase *db, sqlite3_stmt *stmt);      /* see stmt_finish below */

/* stmt_cache_finalize() — GDestroyNotify wrapper for sqlite3_finalize,
 * used as the value-destroy function of stmt_cache.                          */
static void
stmt_cache_finalize(gpointer stmt)
{
    sqlite3_finalize((sqlite3_stmt *)stmt);
}

/* ---------------------------------------------------------------------------
 * cached_prepare() — look up a compiled statement in the per-connection cache;
 * prepare and cache it on the first call.  Returns the statement reset and
 * with all bindings cleared, ready for fresh binding.  The caller must NOT
 * finalize it — the cache owns the lifetime.
 *   db  — open database.
 *   sql — string literal (the pointer is used as the cache key).
 * Returns the statement, or NULL on a prepare failure.
 * ------------------------------------------------------------------------- */
static sqlite3_stmt *
cached_prepare(OnDatabase *db, const char *sql)
{
    if (db->stmt_cache == NULL)
        return prepare(db, sql);
    sqlite3_stmt *stmt = g_hash_table_lookup(db->stmt_cache, sql);
    if (stmt != NULL) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        return stmt;
    }
    stmt = prepare(db, sql);
    if (stmt != NULL)
        g_hash_table_insert(db->stmt_cache, (gpointer)sql, stmt);
    return stmt;
}

/* ---------------------------------------------------------------------------
 * stmt_finish() — run a fully-bound single DML statement to completion and
 * dispose of it according to who owns it.  The shared body of stmt_done()
 * and cached_stmt_done(), which differ only in that.
 *   cached — TRUE for a statement the cache owns (reset + clear bindings,
 *            never finalize); FALSE for a one-off (finalize).
 * ------------------------------------------------------------------------- */
static gboolean
stmt_finish(OnDatabase *db, sqlite3_stmt *stmt, gboolean cached)
{
    if (stmt == NULL)
        return FALSE;
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    if (!ok)
        g_warning("db: statement failed: %s", sqlite3_errmsg(db->handle));
    if (cached) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    } else {
        sqlite3_finalize(stmt);
    }
    return ok;
}

static gboolean
stmt_done(OnDatabase *db, sqlite3_stmt *stmt)
{
    return stmt_finish(db, stmt, FALSE);
}

/* ---------------------------------------------------------------------------
 * cached_stmt_done() — execute a cached DML statement then reset it for
 * reuse.  Unlike stmt_done(), the statement is NOT finalized.
 * ------------------------------------------------------------------------- */
static gboolean
cached_stmt_done(OnDatabase *db, sqlite3_stmt *stmt)
{
    return stmt_finish(db, stmt, TRUE);
}

/* ---------------------------------------------------------------------------
 * query_int64() — first column of the first row of a scalar SELECT.
 *   db      — open database.
 *   sql     — single SELECT with at most one integer '?' placeholder.
 *   bind_id — value for that placeholder, or a negative value for none.
 * Returns the value, or 0 when the query fails or returns no row.
 * ------------------------------------------------------------------------- */
static gint64
query_int64(OnDatabase *db, const char *sql, gint64 bind_id)
{
    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt == NULL)
        return 0;
    if (bind_id >= 0)
        sqlite3_bind_int64(stmt, 1, bind_id);
    gint64 value = 0;                    /* result, 0 when no row           */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

/* =========================================================================
 * lifecycle
 * ========================================================================= */

gchar *
on_db_default_path(void)
{
    gchar *dir = g_build_filename(g_get_user_data_dir(),
                                  "notes", NULL);
    g_mkdir_with_parents(dir, 0700);
    gchar *path = g_build_filename(dir, ON_DB_FILENAME, NULL);
    g_free(dir);
    return path;
}

OnDatabase *
on_db_open(const gchar *path_override)
{
    /* Resolve the database path: explicit override, or the per-user data
     * directory default.                                                    */
    gchar *path = (path_override != NULL)   /* final absolute db path      */
                  ? g_strdup(path_override)
                  : on_db_default_path();

    OnDatabase *db = g_new0(OnDatabase, 1);
    db->path = path;
    /* Statements are keyed by SQL literal pointer; finalized on db_close.   */
    db->stmt_cache = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                           stmt_cache_finalize);

    if (sqlite3_open(path, &db->handle) != SQLITE_OK) {
        g_warning("db: cannot open %s: %s", path, sqlite3_errmsg(db->handle));
        on_db_close(db);
        return NULL;
    }

    /* Wait out short write locks instead of failing instantly — e.g. a
     * CLI command landing while the GUI is mid-autosave.                   */
    sqlite3_busy_timeout(db->handle, ON_DB_BUSY_TIMEOUT_MS);

    if (!exec_simple(db, SCHEMA_SQL)) {
        on_db_close(db);
        return NULL;
    }

    /* Migrations: these columns arrived after the original schema.
     * ALTER fails harmlessly when the column already exists.               */
    sqlite3_exec(db->handle,
                 "ALTER TABLE notes ADD COLUMN pinned INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE notes ADD COLUMN body_text TEXT",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE notes ADD COLUMN trashed INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE folders ADD COLUMN trashed INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE folders ADD COLUMN ai_mode INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE action_items ADD COLUMN due INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);
    sqlite3_exec(db->handle,
                 "ALTER TABLE folders ADD COLUMN emoji TEXT "
                 "NOT NULL DEFAULT ''",
                 NULL, NULL, NULL);
    /* 0 = no uid yet; on_app_action_uids_backfill() fills those in once.   */
    sqlite3_exec(db->handle,
                 "ALTER TABLE action_items ADD COLUMN uid INTEGER "
                 "NOT NULL DEFAULT 0",
                 NULL, NULL, NULL);

    /* The Trash view references the trashed columns, and the uid index the
     * uid column, so both are created only after the migrations above.      */
    if (!exec_simple(db, TRASH_VIEW_SQL)) {
        on_db_close(db);
        return NULL;
    }
    if (!exec_simple(db, "CREATE INDEX IF NOT EXISTS idx_action_items_uid "
                         "ON action_items(uid)")) {
        on_db_close(db);
        return NULL;
    }
    return db;
}

gboolean
on_db_backup_to(OnDatabase *db, const gchar *dest_path)
{
    sqlite3 *dest = NULL;            /* the backup file's connection        */
    if (sqlite3_open(dest_path, &dest) != SQLITE_OK) {
        g_warning("db: backup: cannot open %s: %s",
                  dest_path, sqlite3_errmsg(dest));
        sqlite3_close(dest);
        return FALSE;
    }

    /* The online backup API snapshots a live database safely.  Step with
     * -1 to copy all pages in one call; retry on transient SQLITE_BUSY
     * (a concurrent CLI write may briefly hold the read lock).            */
    sqlite3_backup *backup =
        sqlite3_backup_init(dest, "main", db->handle, "main");
    gboolean ok = FALSE;             /* overall success                     */
    if (backup != NULL) {
        int rc, tries = 0;
        do {
            rc = sqlite3_backup_step(backup, -1);
            if ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && tries++ < 5)
                g_usleep(200 * 1000);    /* 200 ms between retries          */
        } while ((rc == SQLITE_BUSY || rc == SQLITE_LOCKED) && tries < 5);
        sqlite3_backup_finish(backup);
        ok = (rc == SQLITE_DONE);
    }
    if (!ok)
        g_warning("db: backup to %s failed: %s",
                  dest_path, sqlite3_errmsg(dest));
    sqlite3_close(dest);
    return ok;
}

void
on_db_close(OnDatabase *db)
{
    if (db == NULL)
        return;
    /* Finalize all cached statements BEFORE closing the connection.          */
    if (db->stmt_cache != NULL) {
        g_hash_table_destroy(db->stmt_cache);
        db->stmt_cache = NULL;
    }
    if (db->handle != NULL)
        sqlite3_close(db->handle);
    g_free(db->path);
    g_free(db);
}

/* =========================================================================
 * folders
 * ========================================================================= */

gint64
on_db_folder_create(OnDatabase *db, gint64 parent_id, const gchar *name,
                    gint ai_mode, const gchar *emoji)
{
    /* ai_mode and emoji ride the INSERT: setting them afterwards meant
     * three separate statements (and fsyncs) to create one folder.          */
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO folders (parent_id, name, ai_mode, emoji, sort_order) "
        "VALUES (?, ?, ?, ?, "
        "  COALESCE((SELECT MAX(sort_order)+1 FROM folders "
        "            WHERE parent_id IS ?), 0))");
    if (stmt == NULL)
        return 0;

    bind_id_or_null(stmt, 1, parent_id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, ai_mode);
    sqlite3_bind_text(stmt, 4, emoji != NULL ? emoji : "", -1,
                      SQLITE_TRANSIENT);
    bind_id_or_null(stmt, 5, parent_id);

    gint64 new_id = 0;                   /* id of the inserted row          */
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->handle);
    else
        g_warning("db: folder_create: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return new_id;
}

gboolean
on_db_folder_set_ai_mode(OnDatabase *db, gint64 id, gint mode)
{
    sqlite3_stmt *stmt =
        prepare(db, "UPDATE folders SET ai_mode=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int(stmt, 1, mode);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

gint
on_db_folder_get_ai_mode(OnDatabase *db, gint64 id)
{
    return (gint)query_int64(db,
        "SELECT ai_mode FROM folders WHERE id=?", id);
}

gchar *
on_db_folder_get_emoji(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COALESCE(emoji,'') FROM folders WHERE id=?");
    if (stmt == NULL)
        return g_strdup("");
    sqlite3_bind_int64(stmt, 1, id);
    gchar *emoji = NULL;              /* result, empty when no row / null    */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        emoji = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return emoji != NULL ? emoji : g_strdup("");
}

gboolean
on_db_folder_set_emoji(OnDatabase *db, gint64 id, const gchar *emoji)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET emoji=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1,
                          emoji != NULL ? emoji : "",
                          -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

gboolean
on_db_folder_rename(OnDatabase *db, gint64 id, const gchar *name)
{
    sqlite3_stmt *stmt = prepare(db, "UPDATE folders SET name=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

gboolean
on_db_folder_update(OnDatabase *db, gint64 id, const gchar *name,
                    gint ai_mode, const gchar *emoji)
{
    /* The Info dialog can change all three at once; one UPDATE instead of a
     * rename plus two setters.                                              */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET name=?, ai_mode=?, emoji=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, ai_mode);
        sqlite3_bind_text(stmt, 3, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, id);
    }
    return stmt_done(db, stmt);
}

/* folder_parent_of() — COALESCE(parent_id,0) of folder `id`, or 0 when
 * the row doesn't exist (the walk in on_db_folder_move just stops).         */
static gint64
folder_parent_of(OnDatabase *db, gint64 id)
{
    return query_int64(db,
        "SELECT COALESCE(parent_id,0) FROM folders WHERE id=?", id);
}

gboolean
on_db_folder_move(OnDatabase *db, gint64 id, gint64 parent_id)
{
    if (id == 0 || parent_id == id)
        return FALSE;

    /* Cycle guard: walk up from the target parent — if the chain passes
     * through the folder being moved, the move would detach a subtree
     * into itself.  The depth cap only matters if the table already
     * holds a corrupt cycle; it turns a hang into a refusal.                */
    gint64 p = parent_id;                /* ancestor cursor                 */
    for (gint depth = 0; p != 0; depth++) {
        if (p == id || depth > ON_DB_FOLDER_CYCLE_DEPTH_LIMIT)
            return FALSE;
        p = folder_parent_of(db, p);
    }

    /* Re-parent, appending after the new parent's existing children
     * (the moved folder is excluded from the MAX in case it is already
     * there).  trashed=0 makes drag-out-of-Trash a restore-to-location,
     * mirroring on_db_notes_move.                                           */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET parent_id=?, trashed=0, sort_order="
        "  COALESCE((SELECT MAX(sort_order)+1 FROM folders "
        "            WHERE parent_id IS ? AND id<>?), 0) "
        "WHERE id=?");
    if (stmt != NULL) {
        bind_id_or_null(stmt, 1, parent_id);
        bind_id_or_null(stmt, 2, parent_id);
        sqlite3_bind_int64(stmt, 3, id);
        sqlite3_bind_int64(stmt, 4, id);
    }
    return stmt_done(db, stmt);
}

/* ---------------------------------------------------------------------------
 * BULK ID OPERATIONS
 *
 * Four callers (delete, trash, move, reorder) all need the same shape: one
 * transaction around a statement stepped once per id, rolled back whole on
 * the first failure.  Only the binding differs, so that is the callback.
 *
 * The transaction matters for more than atomicity: in autocommit mode SQLite
 * fsyncs per statement, which froze the GUI on a big multi-note drop.
 * ------------------------------------------------------------------------- */

/* BulkBind — bind one row's parameters.  `i` is the row's index in `ids`
 * (reorder uses it as the sort_order), `id` the row id, `data` the caller's
 * context (the destination folder for a move, NULL otherwise).              */
typedef void (*BulkBind)(sqlite3_stmt *stmt, gsize i, gint64 id,
                         gpointer data);

/* ---------------------------------------------------------------------------
 * bulk_ids() — run `sql` once per id inside one transaction.
 *   begin     — "BEGIN" or "BEGIN IMMEDIATE" (the latter takes the write
 *               lock up front, for the destructive paths).
 *   sql       — the statement to step per id.
 *   ids / n   — the rows to act on.
 *   bind      — binds one row's parameters.
 *   data      — passed through to `bind`.
 *   after_sql — a parameterless statement to run inside the SAME transaction
 *               once every row succeeded (the orphan-tag prune), or NULL.
 * Returns TRUE when everything committed.  n == 0 is a successful no-op that
 * opens no transaction at all.
 * ------------------------------------------------------------------------- */
static gboolean
bulk_ids(OnDatabase *db, const char *begin, const char *sql,
         const gint64 *ids, gsize n, BulkBind bind, gpointer data,
         const char *after_sql)
{
    if (n == 0)
        return TRUE;
    if (!exec_simple(db, begin))
        return FALSE;

    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt == NULL) {
        exec_simple(db, "ROLLBACK");
        return FALSE;
    }

    gboolean ok = TRUE;                  /* set FALSE on first failure      */
    for (gsize i = 0; i < n && ok; i++) {
        bind(stmt, i, ids[i], data);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_reset(stmt);
    }
    if (!ok)
        g_warning("db: bulk statement failed: %s",
                  sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);

    if (ok && after_sql != NULL)
        ok = exec_simple(db, after_sql);
    if (ok)
        ok = exec_simple(db, "COMMIT");
    if (!ok)
        exec_simple(db, "ROLLBACK");
    return ok;
}

/* bind_id_only() — BulkBind for statements whose only parameter is the id.  */
static void
bind_id_only(sqlite3_stmt *stmt, gsize i, gint64 id, gpointer data)
{
    (void)i; (void)data;
    sqlite3_bind_int64(stmt, 1, id);
}

/* bind_order_id() — BulkBind for the reorder UPDATEs: sort_order = the row's
 * index in the caller's array, then the id.                                 */
static void
bind_order_id(sqlite3_stmt *stmt, gsize i, gint64 id, gpointer data)
{
    (void)data;
    sqlite3_bind_int(stmt, 1, (int)i);
    sqlite3_bind_int64(stmt, 2, id);
}

/* ---------------------------------------------------------------------------
 * reorder_rows() — persist an explicit row ordering in one transaction.
 *   sql — an UPDATE with two placeholders: sort_order, then row id.
 *   ids — row ids in the desired order (sort_order = array index).
 *   n   — number of ids.
 * Shared body of on_db_folder_reorder/on_db_note_reorder.  Returns TRUE
 * when every update succeeded (failure rolls the batch back).
 * ------------------------------------------------------------------------- */
static gboolean
reorder_rows(OnDatabase *db, const char *sql, const gint64 *ids, gsize n)
{
    return bulk_ids(db, "BEGIN", sql, ids, n, bind_order_id, NULL, NULL);
}

gboolean
on_db_folder_reorder(OnDatabase *db, const gint64 *folder_ids, gsize n)
{
    return reorder_rows(db, "UPDATE folders SET sort_order=? WHERE id=?",
                        folder_ids, n);
}

/* folder_name_cmp() — GCompareFunc ordering two OnFolder* by casefolded
 * name, the comparison on_db_folder_sort_children() persists.              */
static gint
folder_name_cmp(gconstpointer a, gconstpointer b)
{
    const OnFolder *fa = a, *fb = b;
    gchar *ca = g_utf8_casefold(fa->name != NULL ? fa->name : "", -1);
    gchar *cb = g_utf8_casefold(fb->name != NULL ? fb->name : "", -1);
    gint result = g_strcmp0(ca, cb);
    g_free(ca);
    g_free(cb);
    return result;
}

gint
on_db_folder_sort_children(OnDatabase *db, gint64 parent_id)
{
    GList *kids = on_db_folder_list(db, parent_id);
    kids = g_list_sort(kids, folder_name_cmp);

    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (GList *l = kids; l != NULL; l = l->next)
        g_array_append_val(ids, ((OnFolder *)l->data)->id);
    on_db_folder_list_free(kids);

    /* Nothing to persist for an empty or single-child folder.              */
    gint n = (gint)ids->len;
    gboolean ok = (ids->len < 2) ||
                  on_db_folder_reorder(db, (const gint64 *)ids->data,
                                       ids->len);
    g_array_free(ids, TRUE);
    return ok ? n : -1;
}

gboolean
on_db_folder_delete(OnDatabase *db, gint64 id)
{
    if (!exec_simple(db, "BEGIN IMMEDIATE"))
        return FALSE;
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM folders WHERE id=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, id);
    gboolean ok = stmt_done(db, stmt);
    if (ok)
        ok = exec_simple(db, PRUNE_ORPHAN_TAGS_SQL);
    if (ok)
        ok = exec_simple(db, "COMMIT");
    if (!ok)
        exec_simple(db, "ROLLBACK");
    return ok;
}

/* Column list shared by every folder query, matching run_folder_query()'s
 * expectations.  OnFolder has no parent/sort_order fields, so neither is
 * selected — ORDER BY sort_order works without it being in the result.      */
#define FOLDER_COLS "id, name, COALESCE(emoji,'')"

/* run_folder_query() — collect every row of `stmt` (columns: FOLDER_COLS)
 * into a list of OnFolder* and finalize it.                                 */
static GList *
run_folder_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnFolder* rows      */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnFolder *f  = g_new0(OnFolder, 1);
        f->id    = sqlite3_column_int64(stmt, 0);
        f->name  = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        f->emoji = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        if (f->emoji == NULL)
            f->emoji = g_strdup("");
        out = g_list_prepend(out, f);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_folder_list(OnDatabase *db, gint64 parent_id)
{
    /* trashed=0: a directly-trashed folder disappears from the normal
     * tree (it lives under the Trash section); its untouched descendants
     * never get listed because nothing recurses into it.                    */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " FOLDER_COLS " FROM folders "
        "WHERE parent_id IS ? AND trashed=0 "
        "ORDER BY sort_order, name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    bind_id_or_null(stmt, 1, parent_id);
    return run_folder_query(stmt);
}

/* free_folder() — GDestroyNotify for one OnFolder.                          */
static void
free_folder(gpointer data)
{
    OnFolder *f = data;
    g_free(f->name);
    g_free(f->emoji);
    g_free(f);
}

void
on_db_folder_list_free(GList *folders)
{
    g_list_free_full(folders, free_folder);
}

/* free_folder_list() — GDestroyNotify for on_db_folder_child_map values.    */
static void
free_folder_list(gpointer data)
{
    g_list_free_full(data, free_folder);
}

GHashTable *
on_db_folder_child_map(OnDatabase *db)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, free_folder_list);

    /* Same filter and ordering as on_db_folder_list(), so a tree built by
     * walking this map comes out row-for-row identical to the recursive
     * one-query-per-folder walk it replaces.                                */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " FOLDER_COLS ", COALESCE(parent_id,0) FROM folders "
        "WHERE trashed=0 "
        "ORDER BY sort_order, name COLLATE NOCASE");
    if (stmt == NULL)
        return map;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnFolder *f  = g_new0(OnFolder, 1);
        f->id    = sqlite3_column_int64(stmt, 0);
        f->name  = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        f->emoji = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        gint64 parent = sqlite3_column_int64(stmt, 3);   /* 0 = top level   */

        /* Rows arrive in display order, so appending keeps each sibling
         * list ordered (walking to the tail is free at sibling counts).
         * g_list_append returns the unchanged head of a non-empty list, so
         * only the FIRST child of a parent has to be stored in the map —
         * re-inserting later would make the table free the list we are
         * still building.                                                   */
        GList *kids = g_hash_table_lookup(map, &parent);
        GList *head = g_list_append(kids, f);
        if (kids == NULL) {
            gint64 *key = g_new(gint64, 1);
            *key = parent;
            g_hash_table_insert(map, key, head);
        }
    }
    sqlite3_finalize(stmt);
    return map;
}

GList *
on_db_folder_children(GHashTable *map, gint64 parent_id)
{
    return g_hash_table_lookup(map, &parent_id);
}

void
on_db_folder_child_map_free(GHashTable *map)
{
    if (map != NULL)
        g_hash_table_destroy(map);
}

/* =========================================================================
 * notes
 * ========================================================================= */

gint64
on_db_note_create(OnDatabase *db, gint64 folder_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "INSERT INTO notes (folder_id, title, sort_order, created_at, "
        "                   updated_at) "
        "VALUES (?, '" ON_DEFAULT_NOTE_TITLE "', "
        "  COALESCE((SELECT MAX(sort_order)+1 FROM notes "
        "            WHERE folder_id IS ?), 0), "
        "  strftime('%s','now'), strftime('%s','now'))");
    if (stmt == NULL)
        return 0;
    bind_id_or_null(stmt, 1, folder_id);
    bind_id_or_null(stmt, 2, folder_id);

    gint64 new_id = 0;                   /* id of the inserted note         */
    if (sqlite3_step(stmt) == SQLITE_DONE)
        new_id = sqlite3_last_insert_rowid(db->handle);
    else
        g_warning("db: note_create: %s", sqlite3_errmsg(db->handle));
    sqlite3_finalize(stmt);
    return new_id;
}

gboolean
on_db_notes_delete(OnDatabase *db, const gint64 *ids, gsize n)
{
    /* One transaction for the lot — autocommit would fsync per note — and
     * the orphan-tag prune runs once at the end instead of per note.        */
    return bulk_ids(db, "BEGIN IMMEDIATE", "DELETE FROM notes WHERE id=?",
                    ids, n, bind_id_only, NULL, PRUNE_ORPHAN_TAGS_SQL);
}

/* bind_move() — BulkBind for on_db_notes_move: the destination folder twice
 * (column and MAX subselect), then the note id.  `data` points at the
 * destination folder id.                                                    */
static void
bind_move(sqlite3_stmt *stmt, gsize i, gint64 id, gpointer data)
{
    (void)i;
    gint64 folder_id = *(const gint64 *)data;
    bind_id_or_null(stmt, 1, folder_id);
    bind_id_or_null(stmt, 2, folder_id);
    sqlite3_bind_int64(stmt, 3, id);
}

gboolean
on_db_notes_move(OnDatabase *db, const gint64 *note_ids, gsize n,
                 gint64 folder_id)
{
    /* trashed=0: dragging a note out of the Trash into a folder is a
     * restore — a moved note is always meant to be visible where it
     * lands.  The MAX subselect re-evaluates per row inside the
     * transaction, so the notes land appended in array order.               */
    return bulk_ids(db, "BEGIN",
                    "UPDATE notes SET folder_id=?, trashed=0, sort_order="
                    "  COALESCE((SELECT MAX(sort_order)+1 FROM notes "
                    "            WHERE folder_id IS ?), 0) "
                    "WHERE id=?",
                    note_ids, n, bind_move, &folder_id, NULL);
}

gboolean
on_db_note_save(OnDatabase *db, gint64 id, const gchar *title,
                const guint8 *content, gsize len, const gchar *body_text)
{
    sqlite3_stmt *stmt = cached_prepare(db,
        "UPDATE notes SET title=?, content=?, body_text=?, "
        "updated_at=strftime('%s','now') WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, title, -1, SQLITE_TRANSIENT);
        if (content != NULL && len > 0)
            sqlite3_bind_blob(stmt, 2, content, (int)len, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(stmt, 2);
        if (body_text != NULL)
            sqlite3_bind_text(stmt, 3, body_text, -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(stmt, 3);
        sqlite3_bind_int64(stmt, 4, id);
    }
    return cached_stmt_done(db, stmt);
}

gchar *
on_db_note_body_text(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT body_text FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);
    gchar *text = NULL;              /* cached text, NULL if unfilled       */
    if (sqlite3_step(stmt) == SQLITE_ROW &&
        sqlite3_column_type(stmt, 0) != SQLITE_NULL)
        text = g_strdup((const gchar *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
    return text;
}

gboolean
on_db_note_set_body_text(OnDatabase *db, gint64 id, const gchar *body_text)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET body_text=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_text(stmt, 1, body_text, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

gboolean
on_db_note_set_updated_at(OnDatabase *db, gint64 id, gint64 ts)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET updated_at=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int64(stmt, 1, ts);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

guint8 *
on_db_note_load(OnDatabase *db, gint64 id, gsize *out_len)
{
    *out_len = 0;
    sqlite3_stmt *stmt = prepare(db, "SELECT content FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);

    guint8 *copy = NULL;                 /* caller-owned copy of the blob   */
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const void *blob = sqlite3_column_blob(stmt, 0);
        int         n    = sqlite3_column_bytes(stmt, 0);
        if (blob != NULL && n > 0) {
            copy = g_memdup2(blob, (gsize)n);
            *out_len = (gsize)n;
        }
    }
    sqlite3_finalize(stmt);
    return copy;
}

/* Column list shared by every note-metadata query, matching
 * meta_from_row()'s expectations.                                           */
#define NOTE_META_COLS \
    "id, COALESCE(folder_id,0), title, sort_order, updated_at, pinned, " \
    "created_at"

/* ---------------------------------------------------------------------------
 * meta_from_row() — build one OnNoteMeta from the current result row of a
 * statement selecting NOTE_META_COLS in that order.
 * ------------------------------------------------------------------------- */
static OnNoteMeta *
meta_from_row(sqlite3_stmt *stmt)
{
    OnNoteMeta *m = g_new0(OnNoteMeta, 1);
    m->id         = sqlite3_column_int64(stmt, 0);
    m->folder_id  = sqlite3_column_int64(stmt, 1);
    m->title      = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
    m->updated_at = sqlite3_column_int64(stmt, 4);
    m->pinned     = sqlite3_column_int(stmt, 5) != 0;
    m->created_at = sqlite3_column_int64(stmt, 6);
    return m;
}

OnNoteMeta *
on_db_note_get(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE id=?");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);

    OnNoteMeta *meta = NULL;             /* result, NULL if no such row     */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        meta = meta_from_row(stmt);
    sqlite3_finalize(stmt);
    return meta;
}

/* ---------------------------------------------------------------------------
 * collect_meta() — drain `stmt` (columns = NOTE_META_COLS) into a list of
 * OnNoteMeta*, disposing of the statement by ownership: a cached one is
 * reset for reuse, a one-off is finalized.  The shared body of the four
 * meta-query entry points below.
 * ------------------------------------------------------------------------- */
static GList *
collect_meta(sqlite3_stmt *stmt, gboolean cached)
{
    GList *out = NULL;                   /* accumulated OnNoteMeta* rows    */
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out = g_list_prepend(out, meta_from_row(stmt));
    if (cached) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
    } else {
        sqlite3_finalize(stmt);
    }
    return g_list_reverse(out);
}

/* run_meta_query() — collect a one-off stmt's rows and finalize it.         */
static GList *
run_meta_query(sqlite3_stmt *stmt)
{
    return collect_meta(stmt, FALSE);
}

/* run_meta_query_cached() — collect a cached stmt's rows and reset it.      */
static GList *
run_meta_query_cached(sqlite3_stmt *stmt)
{
    return collect_meta(stmt, TRUE);
}

/* meta_query() — prepare a parameterless note-metadata SELECT (columns =
 * NOTE_META_COLS) and collect its rows.  Returns a GList of OnNoteMeta*
 * (free with on_db_note_list_free), or NULL on a prepare failure.            */
static GList *
meta_query(OnDatabase *db, const char *sql)
{
    sqlite3_stmt *stmt = prepare(db, sql);
    return (stmt != NULL) ? run_meta_query(stmt) : NULL;
}

/* meta_query_cached() — look up or compile a parameterless note-metadata
 * SELECT and collect its rows via run_meta_query_cached().                   */
static GList *
meta_query_cached(OnDatabase *db, const char *sql)
{
    sqlite3_stmt *stmt = cached_prepare(db, sql);
    return (stmt != NULL) ? run_meta_query_cached(stmt) : NULL;
}

GList *
on_db_note_list(OnDatabase *db, gint64 folder_id)
{
    /* trashed=0 keeps directly-trashed notes out; when the folder itself
     * sits in the Trash (browsing it from the Trash section) its regular
     * notes carry no flag and still list here.                              */
    sqlite3_stmt *stmt = cached_prepare(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE folder_id IS ? AND trashed=0 "
        "ORDER BY sort_order, updated_at DESC");
    if (stmt == NULL)
        return NULL;
    bind_id_or_null(stmt, 1, folder_id);
    return run_meta_query_cached(stmt);
}

GList *
on_db_note_list_all(OnDatabase *db, gboolean include_trash)
{
    return meta_query(db, include_trash
        ? "SELECT " NOTE_META_COLS " "
          "FROM notes ORDER BY folder_id, sort_order"
        : "SELECT " NOTE_META_COLS " "
          "FROM notes WHERE " NOTE_VISIBLE_SQL " "
          "ORDER BY folder_id, sort_order");
}

GList *
on_db_note_list_recent(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE " NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
}

gboolean
on_db_note_set_pinned(OnDatabase *db, gint64 id, gboolean pinned)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET pinned=? WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int(stmt, 1, pinned ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

GList *
on_db_note_list_pinned(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE pinned=1 AND " NOTE_VISIBLE_SQL " "
        "ORDER BY updated_at DESC");
}

gint
on_db_note_count_pinned(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE pinned=1 AND " NOTE_VISIBLE_SQL,
        -1);
}

gboolean
on_db_note_reorder(OnDatabase *db, const gint64 *note_ids, gsize n)
{
    return reorder_rows(db, "UPDATE notes SET sort_order=? WHERE id=?",
                        note_ids, n);
}

void
on_db_note_meta_free(OnNoteMeta *meta)
{
    if (meta == NULL)
        return;
    g_free(meta->title);
    g_free(meta);
}

void
on_db_note_list_free(GList *notes)
{
    g_list_free_full(notes, (GDestroyNotify)on_db_note_meta_free);
}

/* =========================================================================
 * tags
 * ========================================================================= */

/* on_db_tag_get_or_create() — look up tag `name`, creating it if
 * missing.  Returns the tag id, or 0 on failure.                            */
static gint64
on_db_tag_get_or_create(OnDatabase *db, const gchar *name)
{
    /* Try the fast path first: the tag already exists.                     */
    gint64 tag_id = on_db_tag_find(db, name);   /* resulting tag id         */
    if (tag_id != 0)
        return tag_id;

    /* Not found: insert it.                                                */
    sqlite3_stmt *stmt = prepare(db, "INSERT INTO tags (name) VALUES (?)");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_DONE)
        tag_id = sqlite3_last_insert_rowid(db->handle);
    sqlite3_finalize(stmt);
    return tag_id;
}

gint64
on_db_tag_find(OnDatabase *db, const gchar *name)
{
    sqlite3_stmt *stmt = prepare(db, "SELECT id FROM tags WHERE name=?");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
    gint64 tag_id = 0;               /* found id, or 0                      */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        tag_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return tag_id;
}

gboolean
on_db_tag_delete(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db, "DELETE FROM tags WHERE id=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, id);
    return stmt_done(db, stmt);
}

/* run_tag_query() — step a prepared statement whose result columns are
 * (id, name) and collect the rows as OnTag structs.  Finalizes `stmt`.
 * Returns a GList of OnTag*; free with on_db_tag_list_free().               */
static GList *
run_tag_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnTag* rows         */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnTag *t = g_new0(OnTag, 1);
        t->id   = sqlite3_column_int64(stmt, 0);
        t->name = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
        out = g_list_prepend(out, t);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_tag_list(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, name FROM tags ORDER BY name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    return run_tag_query(stmt);
}

/* free_tag() — GDestroyNotify for one OnTag.                                */
static void
free_tag(gpointer data)
{
    OnTag *t = data;
    g_free(t->name);
    g_free(t);
}

void
on_db_tag_list_free(GList *tags)
{
    g_list_free_full(tags, free_tag);
}

gboolean
on_db_note_set_tags(OnDatabase *db, gint64 note_id, GList *tag_names)
{
    if (!exec_simple(db, "BEGIN"))
        return FALSE;

    /* Drop the note's old tag links, then re-add the current set.          */
    sqlite3_stmt *stmt = prepare(db,
        "DELETE FROM note_tags WHERE note_id=?");
    gboolean ok = stmt != NULL;          /* overall success flag            */
    if (ok) {
        sqlite3_bind_int64(stmt, 1, note_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    for (GList *l = tag_names; ok && l != NULL; l = l->next) {
        const gchar *name = l->data;     /* one tag name, no leading '#'    */
        gint64 tag_id = on_db_tag_get_or_create(db, name);
        if (tag_id == 0) {
            ok = FALSE;
            break;
        }
        stmt = prepare(db,
            "INSERT OR IGNORE INTO note_tags (note_id, tag_id) VALUES (?,?)");
        if (stmt == NULL) {
            ok = FALSE;
            break;
        }
        sqlite3_bind_int64(stmt, 1, note_id);
        sqlite3_bind_int64(stmt, 2, tag_id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }

    /* Remove tags that no longer label any note so the tag list in the
     * library window stays tidy.                                           */
    if (ok)
        ok = exec_simple(db, PRUNE_ORPHAN_TAGS_SQL);

    if (ok)
        ok = exec_simple(db, "COMMIT");
    if (!ok)
        exec_simple(db, "ROLLBACK");
    return ok;
}

GList *
on_db_notes_by_tag(OnDatabase *db, gint64 tag_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT n.id, COALESCE(n.folder_id,0), n.title, n.sort_order, "
        "       n.updated_at, n.pinned, n.created_at "
        "FROM notes n JOIN note_tags nt ON nt.note_id = n.id "
        "WHERE nt.tag_id=? AND n.trashed=0 AND (n.folder_id IS NULL OR "
        "      n.folder_id NOT IN (SELECT id FROM trash_folder_ids)) "
        "ORDER BY n.updated_at DESC");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, tag_id);
    return run_meta_query(stmt);
}

GList *
on_db_note_tag_list(OnDatabase *db, gint64 note_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT t.id, t.name FROM tags t "
        "JOIN note_tags nt ON nt.tag_id = t.id "
        "WHERE nt.note_id=? ORDER BY t.name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, note_id);
    return run_tag_query(stmt);
}

/* =========================================================================
 * action items — a queryable mirror of the '!' lines in note content,
 * rebuilt whenever a save changes them (see on_note_extract_actions)
 * ========================================================================= */

/* ---------------------------------------------------------------------------
 * action_uid_alloc() — take the next stable uid from the action_uid_seq
 * high-water mark and advance it.  MUST run inside the caller's
 * transaction so a rolled-back rebuild does not burn ids (and, more
 * importantly, so two allocations can never collide).  The counter is
 * only ever incremented, so a uid retired with a deleted item is never
 * handed out to a different item.
 * Returns the new uid, or 0 on failure.
 * ------------------------------------------------------------------------- */
static gint64
action_uid_alloc(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT next FROM action_uid_seq WHERE id=1");
    if (stmt == NULL)
        return 0;
    gint64 uid = 0;                      /* the id to hand out              */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        uid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    if (uid <= 0)
        return 0;

    stmt = prepare(db, "UPDATE action_uid_seq SET next=? WHERE id=1");
    if (stmt == NULL)
        return 0;
    sqlite3_bind_int64(stmt, 1, uid + 1);
    gboolean ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok ? uid : 0;
}

/* ---------------------------------------------------------------------------
 * OldAction — one of the note's pre-rebuild rows, used to carry uids
 * across the DELETE-then-INSERT (see on_db_note_set_actions).
 *
 * Fields:
 *   uid     — the stable id to hand on (0 for pre-column rows).
 *   ord     — the row's old position.
 *   text    — the row's old text (owned).
 *   claimed — already matched to one of the new items?
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64   uid;
    gint     ord;
    gchar   *text;
    gboolean claimed;
} OldAction;

/* ---------------------------------------------------------------------------
 * action_uids_carry() — assign every item in `items` its stable uid,
 * reusing the note's old rows wherever one can be identified.  Four
 * passes over the whole list, strongest evidence first (see the
 * on_db_note_set_actions banner in db.h): identical text, then the
 * caller's it->uid hint, then the same ord, then a fresh id.  Each pass
 * runs to completion before the next so a strong match anywhere always
 * beats a weak one — a text match must not lose a row to a positional
 * guess made earlier in the list.
 *   old — the pre-rebuild rows (claimed flags are updated in place).
 * Returns TRUE on success (FALSE only when the allocator fails).
 * ------------------------------------------------------------------------- */
static gboolean
action_uids_carry(OnDatabase *db, GList *items, GPtrArray *old)
{
    /* Hints must be read before pass 1 overwrites any of them.            */
    GArray *hints = g_array_new(FALSE, FALSE, sizeof(gint64));
    for (GList *l = items; l != NULL; l = l->next) {
        gint64 hint = ((OnActionItem *)l->data)->uid;
        g_array_append_val(hints, hint);
        ((OnActionItem *)l->data)->uid = 0;   /* 0 until a pass assigns it  */
    }

    /* Pass 1 — identical text: the item did not change, only moved.       */
    for (GList *l = items; l != NULL; l = l->next) {
        OnActionItem *it = l->data;
        for (guint i = 0; i < old->len; i++) {
            OldAction *o = g_ptr_array_index(old, i);
            if (!o->claimed && o->uid != 0 &&
                g_strcmp0(o->text, it->text) == 0) {
                o->claimed = TRUE;
                it->uid = o->uid;
                break;
            }
        }
    }

    /* Pass 2 — the caller's hint: a live editor mark still sitting on
     * this item's line, the one signal that survives a reword.           */
    guint idx = 0;                       /* position in the hints array    */
    for (GList *l = items; l != NULL; l = l->next, idx++) {
        OnActionItem *it = l->data;
        gint64 hint = g_array_index(hints, gint64, idx);
        if (it->uid != 0 || hint == 0)
            continue;
        for (guint i = 0; i < old->len; i++) {
            OldAction *o = g_ptr_array_index(old, i);
            if (!o->claimed && o->uid == hint) {
                o->claimed = TRUE;
                it->uid = o->uid;
                break;
            }
        }
    }

    /* Pass 3 — same position: the headless reword, with no hint to go on. */
    gint ord = 0;                        /* the item's new position        */
    for (GList *l = items; l != NULL; l = l->next, ord++) {
        OnActionItem *it = l->data;
        if (it->uid != 0)
            continue;
        for (guint i = 0; i < old->len; i++) {
            OldAction *o = g_ptr_array_index(old, i);
            if (!o->claimed && o->uid != 0 && o->ord == ord) {
                o->claimed = TRUE;
                it->uid = o->uid;
                break;
            }
        }
    }

    /* Pass 4 — nothing matched: this is a new item.                       */
    gboolean ok = TRUE;                  /* allocator still healthy?       */
    for (GList *l = items; ok && l != NULL; l = l->next) {
        OnActionItem *it = l->data;
        if (it->uid == 0)
            ok = (it->uid = action_uid_alloc(db)) != 0;
    }

    g_array_unref(hints);
    return ok;
}

gboolean
on_db_note_set_actions(OnDatabase *db, gint64 note_id, GList *items)
{
    if (!exec_simple(db, "BEGIN"))
        return FALSE;

    /* The note's old rows, read BEFORE the delete so their uids can be
     * carried across the rebuild.                                         */
    GPtrArray *old = g_ptr_array_new_with_free_func(g_free);
    sqlite3_stmt *stmt = prepare(db,
        "SELECT uid, ord, text FROM action_items WHERE note_id=?");
    gboolean ok = stmt != NULL;          /* overall success flag            */
    if (ok) {
        sqlite3_bind_int64(stmt, 1, note_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            OldAction *o = g_new0(OldAction, 1);
            o->uid  = sqlite3_column_int64(stmt, 0);
            o->ord  = sqlite3_column_int(stmt, 1);
            o->text = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
            g_ptr_array_add(old, o);
        }
        sqlite3_finalize(stmt);
    }
    /* OldAction.text is owned by the struct, freed with it below.         */

    if (ok && items != NULL)
        ok = action_uids_carry(db, items, old);

    /* Drop the note's old items, then insert the current set.              */
    if (ok) {
        stmt = prepare(db, "DELETE FROM action_items WHERE note_id=?");
        ok = stmt != NULL;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, note_id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_finalize(stmt);
        }
    }

    if (ok && items != NULL) {
        stmt = prepare(db,
            "INSERT INTO action_items (note_id, ord, text, done, due, uid) "
            "VALUES (?,?,?,?,?,?)");
        ok = stmt != NULL;
        gint ord = 0;                    /* row position, list order        */
        for (GList *l = items; ok && l != NULL; l = l->next, ord++) {
            OnActionItem *it = l->data;
            sqlite3_bind_int64(stmt, 1, note_id);
            sqlite3_bind_int(stmt, 2, ord);
            sqlite3_bind_text(stmt, 3, it->text, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 4, it->done ? 1 : 0);
            sqlite3_bind_int64(stmt, 5, it->due);
            sqlite3_bind_int64(stmt, 6, it->uid);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_reset(stmt);
        }
        if (stmt != NULL)
            sqlite3_finalize(stmt);
    }

    for (guint i = 0; i < old->len; i++)
        g_free(((OldAction *)g_ptr_array_index(old, i))->text);
    g_ptr_array_unref(old);

    if (ok)
        ok = exec_simple(db, "COMMIT");
    if (!ok)
        exec_simple(db, "ROLLBACK");
    return ok;
}

gboolean
on_db_action_find_uid(OnDatabase *db, gint64 uid, gint64 *note_id, gint *ord)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT note_id, ord FROM action_items WHERE uid=?");
    if (stmt == NULL)
        return FALSE;
    sqlite3_bind_int64(stmt, 1, uid);
    gboolean found = sqlite3_step(stmt) == SQLITE_ROW;
    if (found) {
        *note_id = sqlite3_column_int64(stmt, 0);
        *ord     = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
    return found;
}

gboolean
on_db_action_uids_missing(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT 1 FROM action_items WHERE uid=0 LIMIT 1");
    if (stmt == NULL)
        return FALSE;
    gboolean missing = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return missing;
}

gboolean
on_db_action_uids_fill(OnDatabase *db)
{
    if (!exec_simple(db, "BEGIN"))
        return FALSE;

    /* Collect first, update after: the rows are addressed by
     * (note_id, ord) — the table's primary key — and rewriting them
     * while the SELECT is still stepping would be reading a moving set.  */
    typedef struct { gint64 note_id; gint ord; } Addr;
    GArray *todo = g_array_new(FALSE, FALSE, sizeof(Addr));
    sqlite3_stmt *stmt = prepare(db,
        "SELECT note_id, ord FROM action_items WHERE uid=0");
    gboolean ok = stmt != NULL;          /* overall success flag            */
    if (ok) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Addr a = { sqlite3_column_int64(stmt, 0),
                       sqlite3_column_int(stmt, 1) };
            g_array_append_val(todo, a);
        }
        sqlite3_finalize(stmt);
    }

    if (ok && todo->len > 0) {
        stmt = prepare(db,
            "UPDATE action_items SET uid=? WHERE note_id=? AND ord=?");
        ok = stmt != NULL;
        for (guint i = 0; ok && i < todo->len; i++) {
            Addr   a   = g_array_index(todo, Addr, i);
            gint64 uid = action_uid_alloc(db);
            ok = uid != 0;
            if (!ok)
                break;
            sqlite3_bind_int64(stmt, 1, uid);
            sqlite3_bind_int64(stmt, 2, a.note_id);
            sqlite3_bind_int(stmt, 3, a.ord);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
            sqlite3_reset(stmt);
        }
        if (stmt != NULL)
            sqlite3_finalize(stmt);
    }

    g_array_unref(todo);
    if (ok)
        ok = exec_simple(db, "COMMIT");
    if (!ok)
        exec_simple(db, "ROLLBACK");
    return ok;
}

/* Column list shared by the action-item queries, matching
 * run_action_query()'s expectations.                                        */
#define ACTION_COLS "note_id, ord, text, done, due, uid"

/* run_action_query() — collect every row of `stmt` (columns: ACTION_COLS)
 * into a list of OnActionItem* and finalize it — the action-item counterpart
 * of run_meta_query()/run_tag_query().                                      */
static GList *
run_action_query(sqlite3_stmt *stmt)
{
    GList *out = NULL;                   /* accumulated OnActionItem rows   */
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        OnActionItem *it = g_new0(OnActionItem, 1);
        it->note_id = sqlite3_column_int64(stmt, 0);
        it->ord     = sqlite3_column_int(stmt, 1);
        it->text    = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        it->done    = sqlite3_column_int(stmt, 3) != 0;
        it->due     = sqlite3_column_int64(stmt, 4);
        it->uid     = sqlite3_column_int64(stmt, 5);
        out = g_list_prepend(out, it);
    }
    sqlite3_finalize(stmt);
    return g_list_reverse(out);
}

GList *
on_db_action_list(OnDatabase *db)
{
    /* The join needs the "a." qualifier, so the column list is spelled with
     * the alias here; the order still matches ACTION_COLS.                  */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT a.note_id, a.ord, a.text, a.done, a.due, a.uid "
        "FROM action_items a JOIN notes n ON n.id = a.note_id "
        "WHERE n." NOTE_VISIBLE_SQL " "
        "ORDER BY n.updated_at DESC, a.ord");
    return (stmt != NULL) ? run_action_query(stmt) : NULL;
}

GList *
on_db_action_list_for_note(OnDatabase *db, gint64 note_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " ACTION_COLS " FROM action_items "
        "WHERE note_id=? ORDER BY ord");
    if (stmt == NULL)
        return NULL;
    sqlite3_bind_int64(stmt, 1, note_id);
    return run_action_query(stmt);
}

gboolean
on_db_action_set_done(OnDatabase *db, gint64 note_id, gint ord,
                      gboolean done)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE action_items SET done=? WHERE note_id=? AND ord=?");
    if (stmt != NULL) {
        sqlite3_bind_int(stmt, 1, done ? 1 : 0);
        sqlite3_bind_int64(stmt, 2, note_id);
        sqlite3_bind_int(stmt, 3, ord);
    }
    return stmt_done(db, stmt);
}

void
on_db_action_counts(OnDatabase *db, gint *total, gint *open)
{
    if (total != NULL) *total = 0;
    if (open  != NULL) *open  = 0;
    sqlite3_stmt *stmt = prepare(db,
        "SELECT COUNT(*), COALESCE(SUM(a.done=0),0) "
        "FROM action_items a JOIN notes n ON n.id = a.note_id "
        "WHERE n." NOTE_VISIBLE_SQL);
    if (stmt == NULL)
        return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (total != NULL) *total = sqlite3_column_int(stmt, 0);
        if (open  != NULL) *open  = sqlite3_column_int(stmt, 1);
    }
    sqlite3_finalize(stmt);
}

/* free_action_item() — GDestroyNotify for one OnActionItem.                 */
static void
free_action_item(gpointer data)
{
    OnActionItem *it = data;
    g_free(it->text);
    g_free(it);
}

void
on_db_action_list_free(GList *items)
{
    g_list_free_full(items, free_action_item);
}

/* --- schema version (PRAGMA user_version) — gates one-time backfills ---- */

gint
on_db_user_version(OnDatabase *db)
{
    sqlite3_stmt *stmt = prepare(db, "PRAGMA user_version");
    if (stmt == NULL)
        return 0;
    gint v = 0;                          /* stored version, 0 = never set  */
    if (sqlite3_step(stmt) == SQLITE_ROW)
        v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return v;
}

gboolean
on_db_set_user_version(OnDatabase *db, gint version)
{
    gchar *sql = g_strdup_printf("PRAGMA user_version=%d", version);
    gboolean ok = exec_simple(db, sql);
    g_free(sql);
    return ok;
}

/* =========================================================================
 * trash
 * ========================================================================= */

gboolean
on_db_notes_trash(OnDatabase *db, const gint64 *ids, gsize n)
{
    /* One transaction for the lot, like on_db_notes_delete.                 */
    return bulk_ids(db, "BEGIN IMMEDIATE",
                    "UPDATE notes SET trashed=1 WHERE id=?",
                    ids, n, bind_id_only, NULL, NULL);
}

gboolean
on_db_note_restore(OnDatabase *db, gint64 id)
{
    /* The stored folder_id IS the "where it was deleted from"; it only
     * moves to the top level when that folder is itself in the Trash.       */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE notes SET trashed=0, folder_id="
        "  CASE WHEN folder_id IN (SELECT id FROM trash_folder_ids) "
        "       THEN NULL ELSE folder_id END "
        "WHERE id=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, id);
    return stmt_done(db, stmt);
}

gboolean
on_db_folder_trash(OnDatabase *db, gint64 id)
{
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET trashed=1 WHERE id=?");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, id);
    return stmt_done(db, stmt);
}

gboolean
on_db_folder_restore(OnDatabase *db, gint64 id)
{
    /* Re-parent to the top level when the original parent is still in
     * the Trash (the CASE subquery sees the pre-update flags; the folder
     * can never be its own parent, so clearing its flag in the same
     * statement is safe).                                                   */
    sqlite3_stmt *stmt = prepare(db,
        "UPDATE folders SET trashed=0, parent_id="
        "  CASE WHEN parent_id IN (SELECT id FROM trash_folder_ids "
        "                          WHERE id<>?) "
        "       THEN NULL ELSE parent_id END "
        "WHERE id=?");
    if (stmt != NULL) {
        sqlite3_bind_int64(stmt, 1, id);
        sqlite3_bind_int64(stmt, 2, id);
    }
    return stmt_done(db, stmt);
}

GList *
on_db_folder_list_trashed(OnDatabase *db)
{
    /* FOLDER_COLS, like every other folder query: run_folder_query() reads
     * the columns positionally, so a hand-written list here silently loaded
     * parent_id as the name (and the name as the emoji).                   */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT " FOLDER_COLS " FROM folders "
        "WHERE trashed=1 ORDER BY name COLLATE NOCASE");
    if (stmt == NULL)
        return NULL;
    return run_folder_query(stmt);
}

GList *
on_db_note_list_trashed(OnDatabase *db)
{
    return meta_query_cached(db,
        "SELECT " NOTE_META_COLS " "
        "FROM notes WHERE trashed=1 ORDER BY updated_at DESC");
}

gint
on_db_trash_count(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT (SELECT COUNT(*) FROM notes   WHERE trashed=1) + "
        "       (SELECT COUNT(*) FROM folders WHERE trashed=1)", -1);
}

/* run_id_query() — collect a one-column id result set into a GArray of
 * gint64 and finalize the statement.                                        */
static GArray *
run_id_query(sqlite3_stmt *stmt)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    if (stmt != NULL) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 id = sqlite3_column_int64(stmt, 0);
            g_array_append_val(ids, id);
        }
        sqlite3_finalize(stmt);
    }
    return ids;
}

GArray *
on_db_trash_note_ids(OnDatabase *db)
{
    return run_id_query(prepare(db,
        "SELECT id FROM notes WHERE trashed=1 OR "
        "folder_id IN (SELECT id FROM trash_folder_ids)"));
}

GArray *
on_db_folder_note_ids(OnDatabase *db, gint64 folder_id)
{
    sqlite3_stmt *stmt = prepare(db,
        "WITH RECURSIVE sub(id) AS ("
        "  SELECT ? UNION "
        "  SELECT f.id FROM folders f JOIN sub ON f.parent_id = sub.id"
        ") SELECT id FROM notes WHERE folder_id IN (SELECT id FROM sub)");
    if (stmt != NULL)
        sqlite3_bind_int64(stmt, 1, folder_id);
    return run_id_query(stmt);
}

gboolean
on_db_trash_empty(OnDatabase *db)
{
    if (!exec_simple(db, "BEGIN IMMEDIATE"))
        return FALSE;

    /* The explicit notes delete covers directly-trashed notes; deleting
     * the flagged folders cascades through their subtrees (descendant
     * folders and every note inside).                                       */
    gboolean ok =
        exec_simple(db,
            "DELETE FROM notes WHERE trashed=1 OR "
            "folder_id IN (SELECT id FROM trash_folder_ids)") &&
        exec_simple(db, "DELETE FROM folders WHERE trashed=1") &&
        exec_simple(db, PRUNE_ORPHAN_TAGS_SQL);
    if (ok)
        ok = exec_simple(db, "COMMIT");
    if (!ok)
        exec_simple(db, "ROLLBACK");
    return ok;
}

/* =========================================================================
 * counts
 * ========================================================================= */

gboolean
on_db_note_is_trashed(OnDatabase *db, gint64 id)
{
    /* Ask for the note by id AND the visibility rule: a row comes back only
     * when it exists and is visible, so "no row" means trashed — or gone,
     * which the callers have already ruled out.                            */
    return query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE id=? AND " NOTE_VISIBLE_SQL,
        id) == 0;
}

gint
on_db_note_count_visible(OnDatabase *db)
{
    return (gint)query_int64(db,
        "SELECT COUNT(*) FROM notes WHERE " NOTE_VISIBLE_SQL, -1);
}

void
on_db_totals(OnDatabase *db, gint *notes, gint *folders, gint *tags)
{
    if (notes   != NULL) *notes   = 0;
    if (folders != NULL) *folders = 0;
    if (tags    != NULL) *tags    = 0;

    /* Three scalar subqueries in one row: the three totals used to be three
     * separate prepare/step/finalize cycles (each with its SQL built by
     * g_strdup_printf) for one About dialog.                                */
    sqlite3_stmt *stmt = prepare(db,
        "SELECT (SELECT COUNT(*) FROM notes), "
        "       (SELECT COUNT(*) FROM folders), "
        "       (SELECT COUNT(*) FROM tags)");
    if (stmt == NULL)
        return;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (notes   != NULL) *notes   = sqlite3_column_int(stmt, 0);
        if (folders != NULL) *folders = sqlite3_column_int(stmt, 1);
        if (tags    != NULL) *tags    = sqlite3_column_int(stmt, 2);
    }
    sqlite3_finalize(stmt);
}

/* ---------------------------------------------------------------------------
 * count_map_from_query() — run a two-column (id, count) query into a
 * gint64* → GINT_TO_POINTER(count) hash table.
 * ------------------------------------------------------------------------- */
static GHashTable *
count_map_from_query(OnDatabase *db, const gchar *sql)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, NULL);
    sqlite3_stmt *stmt = prepare(db, sql);
    if (stmt != NULL) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            gint64 *key = g_new(gint64, 1);
            *key = sqlite3_column_int64(stmt, 0);
            g_hash_table_insert(map, key,
                                GINT_TO_POINTER(
                                    sqlite3_column_int(stmt, 1)));
        }
        sqlite3_finalize(stmt);
    }
    return map;
}

GHashTable *
on_db_note_count_map(OnDatabase *db)
{
    /* trashed=0 only (no subtree filter): folders inside the Trash keep
     * their counts — the map also labels the Trash section's folder rows.   */
    return count_map_from_query(db,
        "SELECT COALESCE(folder_id,0), COUNT(*) FROM notes WHERE trashed=0 "
        "GROUP BY COALESCE(folder_id,0)");
}

GHashTable *
on_db_tag_count_map(OnDatabase *db)
{
    return count_map_from_query(db,
        "SELECT nt.tag_id, COUNT(*) FROM note_tags nt "
        "JOIN notes n ON n.id = nt.note_id "
        "WHERE n.trashed=0 AND (n.folder_id IS NULL OR "
        "      n.folder_id NOT IN (SELECT id FROM trash_folder_ids)) "
        "GROUP BY nt.tag_id");
}

GHashTable *
on_db_note_text_map(OnDatabase *db, gint max_chars)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);
    /* One statement, two uses: full text for searching, a leading slice for
     * the list-view previews.  The truncated form used to be a second,
     * near-identical function.                                              */
    sqlite3_stmt *stmt = (max_chars > 0)
        ? prepare(db, "SELECT id, SUBSTR(body_text, 1, ?) FROM notes "
                      "WHERE body_text IS NOT NULL")
        : prepare(db, "SELECT id, body_text FROM notes "
                      "WHERE body_text IS NOT NULL");
    if (stmt == NULL)
        return map;
    if (max_chars > 0)
        sqlite3_bind_int(stmt, 1, max_chars);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 *key = g_new(gint64, 1);
        *key = sqlite3_column_int64(stmt, 0);
        g_hash_table_insert(map, key,
            g_strdup((const gchar *)sqlite3_column_text(stmt, 1)));
    }
    sqlite3_finalize(stmt);
    return map;
}

/* =========================================================================
 * utilities
 * ========================================================================= */

/* FolderRow — one folders row held in memory while building the path
 * map: parent id + name.                                                    */
typedef struct {
    gint64  parent;
    gchar  *name;
} FolderRow;

/* folder_row_free() — GDestroyNotify for FolderRow values.                  */
static void
folder_row_free(gpointer data)
{
    FolderRow *r = data;
    g_free(r->name);
    g_free(r);
}

GHashTable *
on_db_folder_path_map(OnDatabase *db)
{
    GHashTable *map = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                            g_free, g_free);

    /* Load every folder row once (trashed included), then build each
     * path by walking parents in memory — no per-folder queries.            */
    GHashTable *rows = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                             g_free, folder_row_free);
    sqlite3_stmt *stmt = prepare(db,
        "SELECT id, COALESCE(parent_id,0), name FROM folders");
    if (stmt == NULL) {
        g_hash_table_destroy(rows);
        return map;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        gint64 *key = g_new(gint64, 1);
        *key = sqlite3_column_int64(stmt, 0);
        FolderRow *r = g_new0(FolderRow, 1);
        r->parent = sqlite3_column_int64(stmt, 1);
        r->name   = g_strdup((const gchar *)sqlite3_column_text(stmt, 2));
        g_hash_table_insert(rows, key, r);
    }
    sqlite3_finalize(stmt);

    GHashTableIter it;               /* over the loaded rows                */
    gpointer k, v;
    g_hash_table_iter_init(&it, rows);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        FolderRow *r = v;            /* the folder itself                   */
        GString *path = g_string_new(r->name);
        gint64 cur   = r->parent;    /* ancestor cursor                     */
        gint   depth = 0;            /* cap guards a corrupt cycle          */
        while (cur != 0 && depth++ < ON_DB_FOLDER_PATH_DEPTH_LIMIT) {
            FolderRow *p = g_hash_table_lookup(rows, &cur);
            if (p == NULL)
                break;
            g_string_prepend(path, "/");
            g_string_prepend(path, p->name);
            cur = p->parent;
        }
        gint64 *key = g_new(gint64, 1);
        *key = *(gint64 *)k;
        g_hash_table_insert(map, key, g_string_free(path, FALSE));
    }
    g_hash_table_destroy(rows);
    return map;
}

gchar *
on_db_folder_path(OnDatabase *db, gint64 folder_id)
{
    /* Walk from the folder up to the root, prepending each name.  The
     * depth cap keeps a corrupt parent_id cycle (hand-edited db) from
     * spinning forever — no real tree is anywhere near that deep.          */
    GString *path = g_string_new("");    /* built back-to-front             */
    gint64   cur  = folder_id;           /* current folder in the walk      */
    gint     depth = 0;                  /* levels walked so far            */

    while (cur > 0 && depth++ < ON_DB_FOLDER_PATH_DEPTH_LIMIT) {
        /* Cached statement: this loop runs once per tree LEVEL, and the
         * function itself runs on every library navigation and every editor
         * focus-in, so a fresh prepare/finalize per level added up.        */
        sqlite3_stmt *stmt = cached_prepare(db,
            "SELECT name, COALESCE(parent_id,0) FROM folders WHERE id=?");
        if (stmt == NULL)
            break;
        sqlite3_bind_int64(stmt, 1, cur);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const gchar *name = (const gchar *)sqlite3_column_text(stmt, 0);
            /* Prepend "name/" to whatever we have so far (both prepends
             * copy, so the reset below is safe).                           */
            g_string_prepend(path, "/");
            g_string_prepend(path, name);
            cur = sqlite3_column_int64(stmt, 1);
        } else {
            cur = 0;
        }
        sqlite3_reset(stmt);             /* cache owns it: reset, never
                                            finalize                        */
    }

    /* Trim the trailing slash left by the loop, if any.                    */
    if (path->len > 0 && path->str[path->len - 1] == '/')
        g_string_truncate(path, path->len - 1);
    return g_string_free(path, FALSE);
}
