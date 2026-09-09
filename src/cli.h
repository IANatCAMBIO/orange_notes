/* ===========================================================================
 * cli.h — command-line automation interface
 *
 * When the binary is invoked with a subcommand, the action runs headless
 * (no windows) against the same database the GUI uses — including a
 * configured custom/shared location — and the process exits.  With no
 * subcommand the GUI starts as usual.
 *
 * Commands follow a noun-verb structure:
 *
 *   notes tag list
 *   notes tag notes NAME             (notes labeled with a tag)
 *   notes tag delete NAME
 *   notes folder list
 *   notes folder info PATH           (id, path, emoji, AI mode, contents)
 *   notes folder add PATH            (nested, created like mkdir -p)
 *   notes folder rename PATH NAME
 *   notes folder move PATH DEST|/    (re-nests the whole subtree)
 *   notes folder emoji PATH EMOJI|-  (the sidebar prefix; - clears it)
 *   notes folder ai-mode PATH normal|project|custom
 *   notes folder sort PATH|/         (its subfolders, alphabetically)
 *   notes folder restore ID          (out of the Trash, BY ID: a trashed
 *                                        folder's path no longer resolves)
 *   notes folder delete [--permanent] PATH    (default: to the Trash)
 *   notes note list [PATH|--all|--recent|--pinned]
 *   notes note info ID               (dates, tags, pinned/trashed, and
 *                                        image/action counts, in one call)
 *   notes note cat ID [--md]         (plain text, or Markdown render)
 *   notes note new [--folder PATH] CONTENT|-   (- reads stdin)
 *   notes note append ID CONTENT|-   (plain text, on a fresh line)
 *   notes note set ID CONTENT|-      (REPLACES the note's content)
 *   notes note delete [--permanent] ID [ID...] (default: to the Trash)
 *   notes note restore ID [ID...]
 *   notes note move ID [ID...] PATH  (/ = top level)
 *   notes note tags ID
 *   notes note tag ID NAME           (appends the literal #NAME token)
 *   notes note untag ID NAME
 *   notes note pin|unpin ID [ID...]
 *   notes note images ID             (N, bytes, WxH — N is 1-based, the
 *                                        numbering 'note cat --md' uses)
 *   notes note image ID N FILE       (writes the stored PNG out verbatim)
 *   notes note open PATH             (id or Folder/Title; uses the GUI)
 *   notes action list [--open|--done] [--uid]  ('!' items; NOTEID:ORD
 *                                        positions, --uid prepends the
 *                                        stable per-item id that survives
 *                                        edits and renumbering)
 *   notes action show UID|NOTEID:ORD (one item, as an 'action list --uid'
 *                                        record: the read-back an external
 *                                        mirror needs for a pinned item)
 *   notes action done|undone UID|NOTEID:ORD
 *   notes action due UID|NOTEID:ORD DATE|-  (rewrites the note line)
 *   notes action text UID|NOTEID:ORD TEXT|- (renames the item in the note
 *                                        line, keeping its done state, its
 *                                        due date and its UID)
 *   notes search TEXT [--regex]      (titles + full text, all notes;
 *                                        TEXT is ANDed words, "quoted
 *                                        phrases" and -exclusions, or
 *                                        one pattern with --regex)
 *   notes trash list                 (deleted folders and notes)
 *   notes trash empty --yes          (PERMANENT; refuses without --yes)
 *   notes stats                      (whole-database counts)
 *   notes quicknote                  (new root note in the running GUI)
 *   notes backup FILE.db
 *   notes export-md DIR
 *   notes export-html DIR
 *
 * Every command that PRINTS RECORDS also takes --json (note
 * list/info/cat/tags/images, folder list/info, tag list/notes, action
 * list/show, search, trash list, stats), emitting one JSON array — or one
 * object for the single-record commands — instead of tab-separated lines.
 * A title, folder name or action text may itself contain a tab, which
 * silently shifts the plain form's columns; JSON escapes it instead.  The
 * flag is recognised ONLY on those commands, so content that happens to be
 * the literal "--json" still reaches `note new`.
 * =========================================================================== */

#ifndef BLUE_CLI_H
#define BLUE_CLI_H

#include "db.h"

/* ---------------------------------------------------------------------------
 * on_cli_run() — dispatch a command-line invocation.
 *   argc/argv — the program arguments as passed to main().
 * Returns a process exit code (>= 0) when a subcommand was handled, or
 * -1 when no subcommand was given (or a GUI-interacting command found no
 * running instance) and the GUI should start.
 * ------------------------------------------------------------------------- */
int on_cli_run(int argc, char **argv);

/* ---------------------------------------------------------------------------
 * on_cli_resolve_folder_path() — walk "A/B/C" through the folder tree.
 *   db     — open database.
 *   path   — folder path; "" or "/" mean the top level.
 *   create — TRUE to create missing components (mkdir -p style).
 *   out_id — receives the folder id (0 = top level).
 * Returns TRUE if the full path resolved (or was created).  Shared with the
 * IPC note-path resolver (ipc.c).
 * ------------------------------------------------------------------------- */
gboolean on_cli_resolve_folder_path(OnDatabase *db, const gchar *path,
                                    gboolean create, gint64 *out_id);

/* ---------------------------------------------------------------------------
 * on_cli_dispatch_db() — run one validated data command (tag/folder/note/
 * backup/export-*) against an already-open database and return its process
 * exit code.  Output goes to stdout/stderr.  Used by the headless path AND by
 * a running instance executing a command on behalf of a remote CLI (ipc.c),
 * so the same code path serves both.  argv/argc are the full program args
 * (argv[1] is the command).
 * ------------------------------------------------------------------------- */
int on_cli_dispatch_db(OnDatabase *db, int argc, char **argv);

/* ---------------------------------------------------------------------------
 * on_cli_set_stdin_data() — supply the text "note new -" should use instead
 * of reading the process stdin (a running instance has no access to the
 * remote CLI's stdin).  NULL restores normal stdin reading.
 * ------------------------------------------------------------------------- */
void on_cli_set_stdin_data(const gchar *data);

/* ---------------------------------------------------------------------------
 * on_cli_command_reads_stdin() — TRUE when this invocation would read stdin
 * ("note new" whose content argument is "-").  The remote-CLI client slurps
 * stdin and ships it only when this is true.
 * ------------------------------------------------------------------------- */
gboolean on_cli_command_reads_stdin(int argc, char **argv);

/* ---------------------------------------------------------------------------
 * on_cli_command_mutates() — TRUE when the command changes the database
 * (create/delete/move/…), so a running instance can refresh its windows
 * after executing it.  Read-only lists/exports/backup return FALSE.
 * ------------------------------------------------------------------------- */
gboolean on_cli_command_mutates(int argc, char **argv);

#endif /* BLUE_CLI_H */
