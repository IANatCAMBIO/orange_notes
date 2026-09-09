/* ===========================================================================
 * search_window.h — the note search window
 *
 * A small window opened from the library's sidebar toolbar:
 *
 *   +--------------------------------------+
 *   | [ search text            ] [Search]  |
 *   | ( ) All notes  (o) Only "Work"       |
 *   | [ ] Case sensitive  [ ] Regex        |
 *   |--------------------------------------|
 *   | result                               |
 *   | result          (double-click opens) |
 *   +--------------------------------------+
 *
 * Matching is against note titles and their full plain text.  The scope
 * radio limits the search to the folder or tag currently selected in the
 * library.  Case-sensitive and regular-expression matching are optional.
 *
 * The query language is search_query.[ch]'s, shared with the headless
 * `notes search`: words are ANDed, "quoted phrases" match whole, and a
 * -word excludes.  Ticking Regular expression turns all of that off and
 * takes the query as one pattern.
 * =========================================================================== */

#ifndef BLUE_SEARCH_WINDOW_H
#define BLUE_SEARCH_WINDOW_H

#include "app.h"

/* What a scoped search targets (mirrors the library selection; the
 * "All Notes" radio bypasses scoping entirely).                             */
typedef enum {
    ON_SCOPE_FOLDER,                 /* one folder's direct notes           */
    ON_SCOPE_TAG,                    /* notes carrying one tag              */
} OnSearchScope;

/* ---------------------------------------------------------------------------
 * on_search_window_open() — show a new search window.
 *
 * The window offers two scopes: "All Notes" and "Selected Folder/Tag".
 * The latter is resolved against the library's live sidebar selection
 * each time the Search button is pressed (via on_library_get_scope), so
 * changing the selection between searches changes what gets searched.
 *
 *   app          — global application context.
 *   scope_to_sel — TRUE preselects the "Selected Folder/Tag" radio (used
 *                  when search is launched from a folder's context menu);
 *                  FALSE preselects "All Notes".
 * ------------------------------------------------------------------------- */
void on_search_window_open(OnApp *app, gboolean scope_to_sel);

/* ---------------------------------------------------------------------------
 * on_search_window_open_query() — show a new search window already loaded
 * with a query and run that search immediately.  Used by the library
 * toolbar's search entry: scope is All Notes, matching is plain and
 * case-insensitive (the window's own defaults), so the results are on
 * screen without the user pressing Search a second time.  The query goes
 * through the same operators as one typed into the window itself.
 *
 *   app   — global application context.
 *   query — text to search for; NULL/empty just opens an idle window.
 * ------------------------------------------------------------------------- */
void on_search_window_open_query(OnApp *app, const gchar *query);

#endif /* BLUE_SEARCH_WINDOW_H */
