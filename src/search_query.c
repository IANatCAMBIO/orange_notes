/* ===========================================================================
 * search_query.c — the search query language (implementation)
 *
 * The parser is one left-to-right byte scan.  Every character it makes a
 * decision about ('-', '"', whitespace) is ASCII, and no ASCII byte can
 * appear inside a multi-byte UTF-8 sequence, so a byte scan is safe on
 * UTF-8 input; the curly quotes are matched as their literal three-byte
 * encodings.
 *
 * Matching folds the note's title and body once and then runs every term
 * against those two strings — the reverse of the old one-needle matcher,
 * which folded the haystack per call.  A note with a dozen terms thrown at
 * it therefore still pays for exactly two casefolds.
 * =========================================================================== */

#include "search_query.h"

#include <string.h>

/* The typographic quotes, as they arrive from macOS input methods and
 * pasted prose.  Treated exactly like an ASCII '"'.                       */
#define Q_OPEN  "\xe2\x80\x9c"           /* U+201C                          */
#define Q_CLOSE "\xe2\x80\x9d"           /* U+201D                          */

/* ---------------------------------------------------------------------------
 * OnQueryTerm — one parsed term.
 *
 * Fields:
 *   text    — the term as typed, quotes stripped (owned).
 *   folded  — `text` casefolded, or NULL in case-sensitive mode (owned).
 *   negated — TRUE for a '-' term: the note must NOT contain it.
 * ------------------------------------------------------------------------- */
typedef struct {
    gchar    *text;
    gchar    *folded;
    gboolean  negated;
} OnQueryTerm;

/* ---------------------------------------------------------------------------
 * OnQuery — a parsed query (see search_query.h for the syntax).
 *
 * Fields:
 *   terms          — OnQueryTerm array, empty in regex mode.
 *   case_sensitive — selects raw or casefolded comparison.
 *   regex          — compiled pattern in regex mode, else NULL (GRegex is
 *                    immutable, so sharing one across threads is safe).
 *   highlight      — first positive term / the pattern, or NULL (owned).
 * ------------------------------------------------------------------------- */
struct OnQuery {
    GArray   *terms;
    gboolean  case_sensitive;
    GRegex   *regex;
    gchar    *highlight;
};

/* term_clear() — GArray clear function for one term's strings.            */
static void
term_clear(gpointer data)
{
    OnQueryTerm *t = data;
    g_free(t->text);
    g_free(t->folded);
}

/* ---------------------------------------------------------------------------
 * quote_len() — is `p` sitting on a quote character?
 * Returns its length in bytes (1 for '"', 3 for a curly quote), 0 if not.
 * ------------------------------------------------------------------------- */
static gsize
quote_len(const gchar *p)
{
    if (*p == '"')
        return 1;
    if (strncmp(p, Q_OPEN, 3) == 0 || strncmp(p, Q_CLOSE, 3) == 0)
        return 3;
    return 0;
}

/* ---------------------------------------------------------------------------
 * query_parse() — split `text` into `q`'s terms.
 *
 * One pass: skip whitespace, take a leading '-' as negation when something
 * follows it, then accumulate bytes until unquoted whitespace ends the
 * term.  Quote characters toggle quoting and are dropped, so "black cat"
 * is one term and cat"alog" is simply catalog.  Terms that come out empty
 * (a lone "" or a stray quote) are discarded.
 * ------------------------------------------------------------------------- */
static void
query_parse(OnQuery *q, const gchar *text)
{
    const gchar *p = text;           /* scan position                       */

    while (*p != '\0') {
        while (g_ascii_isspace((guchar)*p))
            p++;
        if (*p == '\0')
            break;

        /* A '-' negates only when a term actually follows it; a dangling
         * one is ordinary text (as in a query for "-" itself).            */
        gboolean negated = FALSE;    /* this term excludes rather than requires */
        if (*p == '-' && p[1] != '\0' && !g_ascii_isspace((guchar)p[1])) {
            negated = TRUE;
            p++;
        }

        GString  *buf = g_string_new(NULL);  /* the term being built       */
        gboolean  quoted = FALSE;            /* inside a quoted phrase     */
        while (*p != '\0' && (quoted || !g_ascii_isspace((guchar)*p))) {
            gsize qn = quote_len(p);         /* quote here? its byte size  */
            if (qn > 0) {
                quoted = !quoted;
                p += qn;
                continue;
            }
            g_string_append_c(buf, *p);
            p++;
        }

        if (buf->len == 0) {
            g_string_free(buf, TRUE);
            continue;                /* "" and friends: nothing to match   */
        }

        OnQueryTerm t;               /* the finished term                  */
        t.negated = negated;
        t.text    = g_string_free(buf, FALSE);
        t.folded  = q->case_sensitive ? NULL : g_utf8_casefold(t.text, -1);
        g_array_append_val(q->terms, t);

        if (!negated && q->highlight == NULL)
            q->highlight = g_strdup(t.text);
    }
}

OnQuery *
on_query_new(const gchar *text, gboolean case_sensitive,
             gboolean use_regex, GError **error)
{
    if (text == NULL)
        text = "";

    OnQuery *q = g_new0(OnQuery, 1);
    q->case_sensitive = case_sensitive;
    q->terms = g_array_new(FALSE, FALSE, sizeof(OnQueryTerm));
    g_array_set_clear_func(q->terms, term_clear);

    if (use_regex) {
        /* The pattern is compiled here rather than by the caller so a bad
         * one is reported before any searching starts.                    */
        q->regex = g_regex_new(text, case_sensitive ? 0 : G_REGEX_CASELESS,
                               0, error);
        if (q->regex == NULL) {
            on_query_free(q);
            return NULL;
        }
        if (*text != '\0')
            q->highlight = g_strdup(text);
        return q;
    }

    query_parse(q, text);
    return q;
}

void
on_query_free(OnQuery *q)
{
    if (q == NULL)
        return;
    if (q->regex != NULL)
        g_regex_unref(q->regex);
    g_array_free(q->terms, TRUE);
    g_free(q->highlight);
    g_free(q);
}

gboolean
on_query_is_empty(const OnQuery *q)
{
    if (q->regex != NULL)
        return q->highlight == NULL;         /* an empty pattern           */
    return q->terms->len == 0;
}

gboolean
on_query_matches(const OnQuery *q, const gchar *title, const gchar *body)
{
    if (q->regex != NULL)
        return (title != NULL && g_regex_match(q->regex, title, 0, NULL)) ||
               (body  != NULL && g_regex_match(q->regex, body,  0, NULL));

    if (q->terms->len == 0)
        return FALSE;                        /* nothing to match with      */

    /* Fold the note ONCE, whatever the term count.                        */
    gchar *fold_title = NULL;                /* owned folded copies, or    */
    gchar *fold_body  = NULL;                /* NULL when case-sensitive   */
    const gchar *hay_title = title;          /* what the terms are run     */
    const gchar *hay_body  = body;           /* against                    */
    if (!q->case_sensitive) {
        fold_title = (title != NULL) ? g_utf8_casefold(title, -1) : NULL;
        fold_body  = (body  != NULL) ? g_utf8_casefold(body,  -1) : NULL;
        hay_title  = fold_title;
        hay_body   = fold_body;
    }

    gboolean ok = TRUE;                      /* the note satisfies them all */
    for (guint i = 0; i < q->terms->len && ok; i++) {
        const OnQueryTerm *t = &g_array_index(q->terms, OnQueryTerm, i);
        const gchar *needle = q->case_sensitive ? t->text : t->folded;
        gboolean found =
            (hay_title != NULL && strstr(hay_title, needle) != NULL) ||
            (hay_body  != NULL && strstr(hay_body,  needle) != NULL);
        ok = (found != t->negated);
    }

    g_free(fold_title);
    g_free(fold_body);
    return ok;
}

const gchar *
on_query_highlight_term(const OnQuery *q)
{
    return q->highlight;
}
