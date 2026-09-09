/* ===========================================================================
 * search_query.h — the search query language
 *
 * ONE parsed, immutable query object, shared by the search window's worker
 * thread and the headless `notes search` command so both understand exactly
 * the same syntax.  Replaces the old on_note_text_matches(), which could
 * only test one literal needle.
 *
 * Literal mode (the default) splits the query into TERMS:
 *
 *   cats dogs        both "cats" AND "dogs" appear somewhere in the note
 *   "black cat"      the whole phrase, spaces and all
 *   -cats            notes that do NOT contain "cats"
 *   -"black cat"     notes that do NOT contain the phrase
 *   cats -dogs       "cats", but only where "dogs" is absent
 *
 * A term matches when it appears in the note's TITLE or its plain text.
 * All positive terms must match and no negative term may; a query of
 * nothing but negatives therefore selects every note that avoids them.
 * A '-' with whitespace after it, and a quote character with nothing
 * inside it, are ordinary text.  An unclosed quote runs to the end of
 * the query.  Curly quotes (“ ”) count as quotes, since macOS input
 * methods and pasted prose produce them.
 *
 * Regex mode has no term syntax at all: the query IS the pattern, matched
 * against the title and the body whole, so the operator characters keep
 * their regular-expression meaning.
 * =========================================================================== */

#ifndef BLUE_SEARCH_QUERY_H
#define BLUE_SEARCH_QUERY_H

#include <glib.h>

/* One parsed query.  Immutable once built, and matching allocates only
 * scratch of its own, so a query built on the main thread may be handed
 * to a worker thread (which is exactly what the search window does).      */
typedef struct OnQuery OnQuery;

/* ---------------------------------------------------------------------------
 * on_query_new() — parse a query string into a matcher.
 *
 *   text           — what the user typed.
 *   case_sensitive — FALSE casefolds both needles and haystacks; in regex
 *                    mode it selects G_REGEX_CASELESS.
 *   use_regex      — TRUE treats `text` as one GRegex pattern instead of a
 *                    list of terms (no quoting, no negation).
 *   error          — set when a regex pattern fails to compile (the only
 *                    way this can fail; literal queries always parse).
 *
 * Returns the query — free with on_query_free() — or NULL on a bad
 * pattern.  A query that parsed to no terms at all (an empty string, or
 * just quotes) is returned but reports on_query_is_empty(); callers show
 * their own "type something" message rather than matching nothing.
 * ------------------------------------------------------------------------- */
OnQuery *on_query_new(const gchar *text, gboolean case_sensitive,
                      gboolean use_regex, GError **error);

/* on_query_free() — release a query and everything it owns.               */
void on_query_free(OnQuery *q);

/* on_query_is_empty() — TRUE when the query holds nothing to match with,
 * so a caller can prompt instead of reporting zero hits.                  */
gboolean on_query_is_empty(const OnQuery *q);

/* ---------------------------------------------------------------------------
 * on_query_matches() — does one note satisfy the query?
 *
 *   q     — the parsed query.
 *   title — the note's title (may be NULL).
 *   body  — its plain text, from on_note_text_cached() (may be NULL).
 *
 * Title and body are casefolded ONCE per note here, not once per term.
 * Returns TRUE when every positive term is present and no negative one is.
 * ------------------------------------------------------------------------- */
gboolean on_query_matches(const OnQuery *q, const gchar *title,
                          const gchar *body);

/* ---------------------------------------------------------------------------
 * on_query_highlight_term() — the term worth highlighting inside an opened
 * result: the first POSITIVE term (unquoted, so the editor's in-note search
 * gets "black cat" and not the quotes), or the whole pattern in regex mode.
 * NULL when the query only excludes things — there is nothing to point at.
 * The string belongs to the query.
 * ------------------------------------------------------------------------- */
const gchar *on_query_highlight_term(const OnQuery *q);

#endif /* BLUE_SEARCH_QUERY_H */
