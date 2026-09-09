/* ===========================================================================
 * serialize.c — BNBF binary note format (implementation)
 *
 * See serialize.h for the format specification.  The general strategy:
 *
 *   serialize:   walk the buffer character by character, grouping runs of
 *                identical formatting into TEXT records and emitting an
 *                IMAGE record (PNG bytes) wherever a GdkPixbuf is embedded.
 *
 *   deserialize: read records back, inserting text with the matching
 *                GtkTextTags applied, and decoding PNG bytes back into
 *                embedded pixbufs.
 * =========================================================================== */

#include "serialize.h"
#include "db.h"                      /* OnActionItem (extraction result)    */

#include <string.h>

/* Magic bytes at the start of every BNBF blob.  (The pre-rename "ONBF"
 * magic was retired 2026-07 after an offline migration verified zero
 * such blobs remained in the database.)                                     */
static const guint8 BNBF_MAGIC[4] = { 'B', 'N', 'B', 'F' };

/* Maximum character count for a title derived from a note's first line.     */
#define ON_TITLE_MAX_CHARS 80

/* magic_ok() — does this blob start with the BNBF magic?                    */
static gboolean
magic_ok(const guint8 *data, gsize len)
{
    return data != NULL && len >= 8 && memcmp(data, BNBF_MAGIC, 4) == 0;
}

/* Current format version written by on_note_serialize().  Version 2 added
 * the display_width field to IMAGE records; version 3 added TABLE
 * records; version 4 added the tflags field to TABLE records; version 5
 * added CHECK records.  All older versions are still readable.              */
#define BNBF_VERSION 5u

/* TABLE record flag bits (the tflags field).                                */
#define TABLE_FLAG_HEADER 1u         /* first row is a header row           */

/* Record type bytes.                                                        */
#define REC_END   0x00               /* end of document                     */
#define REC_TEXT  0x01               /* formatted text run                  */
#define REC_IMAGE 0x02               /* inline PNG image                    */
#define REC_TABLE 0x03               /* embedded table of text cells        */
#define REC_CHECK 0x04               /* task-list checkbox                  */

/* ---------------------------------------------------------------------------
 * on_flag_tags — THE flag ⇄ tag-name table (declared in serialize.h).
 * Serializer, editor, undo and export all iterate this single copy so the
 * mapping can never fall out of sync.
 * ------------------------------------------------------------------------- */
const OnFlagTag on_flag_tags[] = {
    { ON_FMT_BOLD,        ON_TAGNAME_BOLD        },
    { ON_FMT_ITALIC,      ON_TAGNAME_ITALIC      },
    { ON_FMT_UNDERLINE,   ON_TAGNAME_UNDERLINE   },
    { ON_FMT_STRIKE,      ON_TAGNAME_STRIKE      },
    { ON_FMT_H1,          ON_TAGNAME_H1          },
    { ON_FMT_H2,          ON_TAGNAME_H2          },
    { ON_FMT_CODEBLOCK,   ON_TAGNAME_CODEBLOCK   },
    { ON_FMT_LIST_BULLET, ON_TAGNAME_LIST_BULLET },
    { ON_FMT_LIST_NUMBER, ON_TAGNAME_LIST_NUMBER },
    { ON_FMT_LIST_CHECK,  ON_TAGNAME_LIST_CHECK  },
    { ON_FMT_TAG,         ON_TAGNAME_TAG         },
};

void
on_buffer_ensure_tags(GtkTextBuffer *buffer)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);

    /* If one of our tags exists they all do — creation is atomic below.    */
    if (gtk_text_tag_table_lookup(table, ON_TAGNAME_BOLD) != NULL)
        return;

    /* Inline character styles.                                             */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_BOLD,
                               "weight", PANGO_WEIGHT_BOLD, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_ITALIC,
                               "style", PANGO_STYLE_ITALIC, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_UNDERLINE,
                               "underline", PANGO_UNDERLINE_SINGLE, NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_STRIKE,
                               "strikethrough", TRUE, NULL);

    /* Headings: larger, bold text applied to whole lines.                  */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_H1,
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.6,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_H2,
                               "weight", PANGO_WEIGHT_BOLD,
                               "scale",  1.3,
                               NULL);

    /* Code block: monospace on a subtle grey background, slightly inset.   */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_CODEBLOCK,
                               "family",             "monospace",
                               "paragraph-background", "#f0f0f0",
                               "left-margin",        24,
                               "right-margin",       24,
                               NULL);

    /* List items: indented paragraphs.  The visible "• " / "1. " prefix is
     * inserted as literal text by the editor; the tag provides indent so
     * wrapped lines align under the text, Apple Notes style.               */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_BULLET,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_NUMBER,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_LIST_CHECK,
                               "left-margin", 32,
                               "indent",      -16,
                               NULL);

    /* Inline #tag token: tinted so tags stand out from prose.              */
    gtk_text_buffer_create_tag(buffer, ON_TAGNAME_TAG,
                               "foreground", "#c35a00",
                               "weight",     PANGO_WEIGHT_SEMIBOLD,
                               NULL);
}

const gsize on_n_flag_tags = G_N_ELEMENTS(on_flag_tags);

guint32
on_flags_at_iter(GtkTextBuffer *buffer, const GtkTextIter *iter,
                 guint32 mask)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    guint32 flags = 0;               /* accumulated format bits             */

    for (gsize i = 0; i < on_n_flag_tags; i++) {
        if ((on_flag_tags[i].flag & mask) == 0)
            continue;
        GtkTextTag *tag =
            gtk_text_tag_table_lookup(table, on_flag_tags[i].tag_name);
        if (tag != NULL && gtk_text_iter_has_tag(iter, tag))
            flags |= on_flag_tags[i].flag;
    }
    return flags;
}

void
on_flag_run_init(OnFlagRun *run, GtkTextBuffer *buffer, guint32 mask)
{
    run->buffer      = buffer;
    run->mask        = mask;
    run->flags       = 0;
    run->next_toggle = -1;           /* nothing probed yet                  */
}

guint32
on_flag_run_at(OnFlagRun *run, const GtkTextIter *iter)
{
    if (gtk_text_iter_get_offset(iter) < run->next_toggle)
        return run->flags;           /* still inside the probed run         */

    run->flags = on_flags_at_iter(run->buffer, iter, run->mask);

    /* Where can the flag set change next?  A NULL tag means "any tag", so
     * this also stops at editor-only tags (emoji padding, search hits, the
     * action tint).  Those toggle more often than the serialized ones, which
     * only costs a few extra probes — never a missed one.                   */
    GtkTextIter next = *iter;        /* scan cursor for the next toggle     */
    run->next_toggle = gtk_text_iter_forward_to_tag_toggle(&next, NULL)
                       ? gtk_text_iter_get_offset(&next) : G_MAXINT;
    return run->flags;
}

const gchar *
on_tag_name_for_flag(guint32 flag)
{
    for (gsize i = 0; i < on_n_flag_tags; i++)
        if (on_flag_tags[i].flag == (OnFormatFlags)flag)
            return on_flag_tags[i].tag_name;
    return NULL;
}

/* ---------------------------------------------------------------------------
 * put_u32() — append a little-endian u32 to a byte array.
 *   buf — destination array.
 *   v   — value to append.
 * ------------------------------------------------------------------------- */
static void
put_u32(GByteArray *buf, guint32 v)
{
    guint8 b[4] = {
        (guint8)(v & 0xff),          (guint8)((v >> 8) & 0xff),
        (guint8)((v >> 16) & 0xff),  (guint8)((v >> 24) & 0xff),
    };
    g_byte_array_append(buf, b, 4);
}

/* ===========================================================================
 * BUFFER WALK
 *
 * ONE traversal of a GtkTextBuffer, shared by the serializer and the
 * editor's undo snapshot.  Both need exactly the same decomposition —
 * anchors interrupt text runs, runs split where the flag set changes,
 * payloadless anchors and stray U+FFFC characters are dropped — and each
 * used to carry its own copy, with comments warning they must stay in step.
 * Now they differ only in what they do with a segment.
 * ------------------------------------------------------------------------- */

/* UTF-8 encoding of U+FFFC, the object-replacement character a child anchor
 * or embedded pixbuf occupies in a text slice.                              */
#define OBJ_REPLACEMENT "\xef\xbf\xbc"

/* seg_flush() — hand the pending text run to the callback and reset it.     */
static void
seg_flush(OnBufferSeg *seg, GString *run, guint32 flags,
          OnBufferSegFn cb, gpointer data)
{
    if (run->len == 0)
        return;
    memset(seg, 0, sizeof *seg);
    seg->kind   = ON_SEG_TEXT;
    seg->flags  = flags;
    seg->text   = run->str;
    seg->n_text = run->len;
    cb(seg, data);
    g_string_truncate(run, 0);
}

void
on_buffer_walk(GtkTextBuffer *buffer, OnBufferSegFn cb, gpointer data)
{
    GtkTextIter iter;                /* walk position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);

    GString  *run       = g_string_new(NULL);  /* text of the pending run   */
    guint32   run_flags = 0;                   /* formatting of the run     */
    OnFlagRun frun;                            /* per-run flag probing      */
    OnBufferSeg seg;                           /* reused, copied by callers */
    on_flag_run_init(&frun, buffer, ~0u);

    while (!gtk_text_iter_is_end(&iter)) {
        /* Images and tables live on child anchors (raw pixbufs are also
         * accepted for robustness against buffers built elsewhere).        */
        GtkTextChildAnchor *anchor = gtk_text_iter_get_child_anchor(&iter);

        gboolean checked;            /* the checkbox's state                */
        if (anchor != NULL && on_anchor_is_checkbox(anchor, &checked)) {
            seg_flush(&seg, run, run_flags, cb, data);
            memset(&seg, 0, sizeof seg);
            seg.kind    = ON_SEG_CHECK;
            seg.checked = checked;
            seg.flags   = on_flag_run_at(&frun, &iter);
            cb(&seg, data);
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        OnTable *table = (anchor != NULL)
                         ? on_anchor_get_table(anchor) : NULL;
        if (table != NULL) {
            seg_flush(&seg, run, run_flags, cb, data);
            memset(&seg, 0, sizeof seg);
            seg.kind  = ON_SEG_TABLE;
            seg.table = table;       /* borrowed: the anchor owns it        */
            seg.flags = on_flag_run_at(&frun, &iter);
            cb(&seg, data);
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        GdkPixbuf *original = NULL;  /* full-resolution image               */
        gint display_width = 0;      /* the user's chosen display width     */
        if (anchor != NULL) {
            original = on_anchor_get_image(anchor, &display_width);
        } else {
            original = gtk_text_iter_get_pixbuf(&iter);
            if (original != NULL)
                display_width = gdk_pixbuf_get_width(original);
        }
        if (original != NULL) {
            seg_flush(&seg, run, run_flags, cb, data);
            memset(&seg, 0, sizeof seg);
            seg.kind          = ON_SEG_IMAGE;
            seg.pixbuf        = original;   /* borrowed                     */
            seg.display_width = display_width;
            seg.flags         = on_flag_run_at(&frun, &iter);
            cb(&seg, data);
            gtk_text_iter_forward_char(&iter);
            continue;
        }
        if (anchor != NULL) {        /* payloadless anchor: skip its 0xFFFC
                                        WITHOUT breaking the run           */
            gtk_text_iter_forward_char(&iter);
            continue;
        }

        /* A plain text position.  The formatting holds until the next tag
         * toggle, so take the whole stretch at once instead of one
         * character at a time — measured 6.9 ms -> 0.1 ms across a 20 000
         * character note.  A stretch containing an anchor (U+FFFC in the
         * slice) falls back to the careful path so the anchor branches
         * above still see it.                                              */
        guint32 flags = on_flag_run_at(&frun, &iter);
        if (flags != run_flags) {
            seg_flush(&seg, run, run_flags, cb, data);
            run_flags = flags;
        }

        GtkTextIter stop = iter;     /* end of this same-formatting stretch */
        if (!gtk_text_iter_forward_to_tag_toggle(&stop, NULL))
            gtk_text_buffer_get_end_iter(buffer, &stop);
        gchar *slice = gtk_text_buffer_get_slice(buffer, &iter, &stop, TRUE);
        const gchar *obj = strstr(slice, OBJ_REPLACEMENT);
        if (obj == NULL) {
            g_string_append(run, slice);
            iter = stop;
        } else if (obj == slice) {
            /* An object sits right here.  Anchors were handled above, so
             * this is a stray replacement character from a paste: drop it. */
            gtk_text_iter_forward_char(&iter);
        } else {
            /* Take the text up to the object and STOP THERE, so the next
             * turn of the loop meets the object in the branches above.
             * Advancing a single character here instead would re-slice the
             * rest of the stretch per character — quadratic on a long run
             * that happens to contain an image.                            */
            g_string_append_len(run, slice, (gssize)(obj - slice));
            gtk_text_iter_forward_chars(
                &iter, (gint)g_utf8_strlen(slice, obj - slice));
        }
        g_free(slice);
    }
    seg_flush(&seg, run, run_flags, cb, data);
    g_string_free(run, TRUE);
}

GBytes *
on_image_png_bytes(GdkPixbuf *pixbuf)
{
    /* The pixbuf never changes once attached, so one encoding serves every
     * writer: the cache is attached by the full-resolution load (the note's
     * ORIGINAL bytes, so nothing is ever recompressed) or filled here on the
     * first write of a freshly pasted image.                               */
    GBytes *cached = g_object_get_data(G_OBJECT(pixbuf), "on-png");
    if (cached != NULL)
        return cached;

    gchar  *png   = NULL;            /* freshly encoded bytes               */
    gsize   n_png = 0;               /* their length                        */
    GError *err   = NULL;            /* encode failure                      */
    if (!gdk_pixbuf_save_to_buffer(pixbuf, &png, &n_png, "png", &err, NULL)) {
        g_warning("image: PNG encode failed: %s",
                  err != NULL ? err->message : "unknown");
        g_clear_error(&err);
        return NULL;
    }

    GBytes *bytes = g_bytes_new_take(png, n_png);
    g_object_set_data_full(G_OBJECT(pixbuf), "on-png", bytes,
                           (GDestroyNotify)g_bytes_unref);
    return bytes;                    /* owned by the pixbuf                 */
}

/* ---------------------------------------------------------------------------
 * serialize_seg() — OnBufferSegFn writing each segment out as a BNBF record.
 * ------------------------------------------------------------------------- */
static void
serialize_seg(const OnBufferSeg *seg, gpointer data)
{
    GByteArray *out = data;          /* the growing BNBF blob               */
    guint8 rec;                      /* record type byte                    */

    switch (seg->kind) {
    case ON_SEG_TEXT:
        rec = REC_TEXT;
        g_byte_array_append(out, &rec, 1);
        put_u32(out, seg->flags);
        put_u32(out, (guint32)seg->n_text);
        g_byte_array_append(out, (const guint8 *)seg->text,
                            (guint)seg->n_text);
        break;

    case ON_SEG_CHECK: {
        rec = REC_CHECK;
        g_byte_array_append(out, &rec, 1);
        guint8 state = seg->checked ? 1 : 0;
        g_byte_array_append(out, &state, 1);
        break;
    }

    case ON_SEG_TABLE:
        rec = REC_TABLE;
        g_byte_array_append(out, &rec, 1);
        put_u32(out, seg->table->header ? TABLE_FLAG_HEADER : 0);
        put_u32(out, (guint32)seg->table->rows);
        put_u32(out, (guint32)seg->table->cols);
        for (gint i = 0; i < seg->table->rows * seg->table->cols; i++) {
            const gchar *cell = g_ptr_array_index(seg->table->cells, i);
            put_u32(out, (guint32)strlen(cell));
            g_byte_array_append(out, (const guint8 *)cell,
                                (guint)strlen(cell));
        }
        break;

    case ON_SEG_IMAGE: {
        GBytes *png_bytes = on_image_png_bytes(seg->pixbuf);
        if (png_bytes != NULL) {
            gsize n_png = 0;         /* PNG byte count                      */
            gconstpointer png = g_bytes_get_data(png_bytes, &n_png);
            rec = REC_IMAGE;
            g_byte_array_append(out, &rec, 1);
            put_u32(out, (guint32)seg->display_width);
            put_u32(out, (guint32)n_png);
            g_byte_array_append(out, (const guint8 *)png, n_png);
        }
        break;
    }
    }
}

guint8 *
on_note_serialize(GtkTextBuffer *buffer, gsize *out_len)
{
    GByteArray *out = g_byte_array_new();   /* the growing BNBF blob        */
    g_byte_array_append(out, BNBF_MAGIC, 4);
    put_u32(out, BNBF_VERSION);

    on_buffer_walk(buffer, serialize_seg, out);

    guint8 end = REC_END;            /* terminating record                  */
    g_byte_array_append(out, &end, 1);

    *out_len = out->len;
    return g_byte_array_free(out, FALSE);
}

/* ---------------------------------------------------------------------------
 * get_u32() — read a little-endian u32, advancing *pos.
 *   data — blob bytes.
 *   len  — blob length.
 *   pos  — in/out read cursor.
 *   out  — receives the value.
 * Returns FALSE if fewer than 4 bytes remain.
 * ------------------------------------------------------------------------- */
static gboolean
get_u32(const guint8 *data, gsize len, gsize *pos, guint32 *out)
{
    if (*pos + 4 > len)
        return FALSE;
    *out = (guint32)data[*pos]
         | ((guint32)data[*pos + 1] << 8)
         | ((guint32)data[*pos + 2] << 16)
         | ((guint32)data[*pos + 3] << 24);
    *pos += 4;
    return TRUE;
}

/* ===========================================================================
 * BNBF READER
 *
 * One cursor over a blob's records, so the header validation, the
 * per-record-type framing and every truncation check exist ONCE.  Both
 * consumers drive it: the full deserializer (which builds a GtkTextBuffer)
 * and the extractor (which only wants text and '!' lines).  They used to
 * carry their own copy of this dispatch, which is how a format tweak could
 * be applied to one and forgotten in the other.
 *
 * The reader never warns; it records why it stopped in `error` and lets the
 * caller decide (the deserializer reports, the extractor stops quietly).
 * ------------------------------------------------------------------------- */

typedef struct {
    const guint8 *data;              /* the blob                            */
    gsize         len;               /* its size                            */
    gsize         pos;               /* read cursor                         */
    guint32       version;           /* format version from the header      */
    gboolean      saw_end;           /* a REC_END was reached               */
    const gchar  *error;             /* why the walk stopped, or NULL       */
} OnBnbfReader;

/* One record, as handed to the caller.  Only the fields belonging to
 * `type` are meaningful.                                                    */
typedef struct {
    guint8        type;              /* REC_TEXT / IMAGE / TABLE / CHECK    */
    guint32       flags;             /* TEXT: ON_FMT_* bits of the run      */
    const gchar  *text;              /* TEXT: run bytes (NOT terminated)    */
    guint32       n_text;
    const guint8 *png;               /* IMAGE: encoded bytes                */
    guint32       n_png;
    guint32       display_width;     /* IMAGE: stored display width (v2+)   */
    gboolean      checked;           /* CHECK: the box's state              */
    OnTable      *table;             /* TABLE: parsed; the CALLER owns it   */
} OnBnbfRecord;

/* bnbf_open() — validate the header and position at the first record.
 * Returns FALSE (with reader->error set) on a bad magic or version.         */
static gboolean
bnbf_open(OnBnbfReader *r, const guint8 *data, gsize len)
{
    r->data = data;
    r->len  = len;
    r->pos  = 4;                     /* past the magic                      */
    r->version = 0;
    r->saw_end = FALSE;
    r->error   = NULL;

    if (!magic_ok(data, len)) {
        r->error = "bad or missing BNBF header";
        return FALSE;
    }
    if (!get_u32(data, len, &r->pos, &r->version) ||
        r->version < 1 || r->version > BNBF_VERSION) {
        r->error = "unsupported BNBF version";
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * bnbf_next() — read the next record, fully consuming it.
 * Returns FALSE at REC_END (reader->saw_end set), when the blob runs out, or
 * on a malformed record (reader->error set).  A TABLE record arrives with
 * rec->table allocated; the caller frees it with on_table_free().
 * ------------------------------------------------------------------------- */
static gboolean
bnbf_next(OnBnbfReader *r, OnBnbfRecord *rec)
{
    if (r->error != NULL || r->pos >= r->len)
        return FALSE;

    memset(rec, 0, sizeof *rec);
    rec->type = r->data[r->pos++];

    switch (rec->type) {
    case REC_END:
        r->saw_end = TRUE;
        return FALSE;

    case REC_TEXT:
        if (!get_u32(r->data, r->len, &r->pos, &rec->flags) ||
            !get_u32(r->data, r->len, &r->pos, &rec->n_text) ||
            r->pos + rec->n_text > r->len) {
            r->error = "truncated TEXT record";
            return FALSE;
        }
        rec->text = (const gchar *)r->data + r->pos;
        r->pos += rec->n_text;
        return TRUE;

    case REC_IMAGE:
        if (r->version >= 2 &&
            !get_u32(r->data, r->len, &r->pos, &rec->display_width)) {
            r->error = "truncated IMAGE record";
            return FALSE;
        }
        if (!get_u32(r->data, r->len, &r->pos, &rec->n_png) ||
            r->pos + rec->n_png > r->len) {
            r->error = "truncated IMAGE record";
            return FALSE;
        }
        rec->png = r->data + r->pos;
        r->pos += rec->n_png;
        return TRUE;

    case REC_CHECK:
        if (r->pos >= r->len) {
            r->error = "truncated CHECK record";
            return FALSE;
        }
        rec->checked = r->data[r->pos++] != 0;
        return TRUE;

    case REC_TABLE: {
        guint32 tflags = 0, rows, cols;
        if (r->version >= 4 &&
            !get_u32(r->data, r->len, &r->pos, &tflags)) {
            r->error = "truncated TABLE record";
            return FALSE;
        }
        if (!get_u32(r->data, r->len, &r->pos, &rows) ||
            !get_u32(r->data, r->len, &r->pos, &cols) ||
            rows == 0 || cols == 0 || rows > 1024 || cols > 1024) {
            r->error = "bad TABLE record";
            return FALSE;
        }
        OnTable *t = on_table_new((gint)rows, (gint)cols);
        t->header = (tflags & TABLE_FLAG_HEADER) != 0;
        for (guint32 i = 0; i < rows * cols; i++) {
            guint32 n;               /* cell byte length                    */
            if (!get_u32(r->data, r->len, &r->pos, &n) ||
                r->pos + n > r->len) {
                on_table_free(t);
                r->error = "truncated TABLE cell";
                return FALSE;
            }
            gchar *cell = g_strndup((const gchar *)r->data + r->pos, n);
            on_table_set(t, (gint)(i / cols), (gint)(i % cols), cell);
            g_free(cell);
            r->pos += n;
        }
        rec->table = t;              /* caller owns it                      */
        return TRUE;
    }

    default:
        r->error = "unknown record type";
        return FALSE;
    }
}

/* ---------------------------------------------------------------------------
 * insert_with_flags() — insert `text` at the buffer end with every tag
 * named by `flags` applied.
 *   buffer — destination buffer.
 *   text   — UTF-8 text to insert.
 *   n      — byte length of `text`.
 *   flags  — ON_FMT_* bits to apply.
 * ------------------------------------------------------------------------- */
static void
insert_with_flags(GtkTextBuffer *buffer, const gchar *text, gssize n,
                  guint32 flags)
{
    GtkTextIter end;                 /* insertion point (buffer end)        */
    gtk_text_buffer_get_end_iter(buffer, &end);

    /* Remember where the inserted span starts so tags can be applied.      */
    gint start_offset = gtk_text_iter_get_offset(&end);
    gtk_text_buffer_insert(buffer, &end, text, n);

    if (flags == 0)
        return;

    GtkTextIter start;               /* start of the span just inserted     */
    gtk_text_buffer_get_iter_at_offset(buffer, &start, start_offset);
    gtk_text_buffer_get_end_iter(buffer, &end);

    for (gsize i = 0; i < on_n_flag_tags; i++) {
        if (flags & on_flag_tags[i].flag)
            gtk_text_buffer_apply_tag_by_name(
                buffer, on_flag_tags[i].tag_name, &start, &end);
    }
}

/* ---------------------------------------------------------------------------
 * on_size_prepared() — GdkPixbufLoader callback capping decode size:
 * shrink to at most `max_px` (passed via user_data) on the longest side,
 * preserving aspect ratio.  Never upscales.
 * ------------------------------------------------------------------------- */
static void
on_size_prepared(GdkPixbufLoader *loader, gint width, gint height,
                 gpointer user_data)
{
    gint max_px = GPOINTER_TO_INT(user_data);
    gint longest = MAX(width, height);
    if (longest > max_px) {
        gdouble scale = (gdouble)max_px / longest;
        gdk_pixbuf_loader_set_size(loader,
                                   MAX(1, (gint)(width * scale)),
                                   MAX(1, (gint)(height * scale)));
    }
}

/* ---------------------------------------------------------------------------
 * png_decode_capped() — decode one IMAGE record's payload into a pixbuf,
 * shrinking it during decode to at most `max_px` on its longest side.  THE
 * one decode path: the full deserializer, the media view's thumbnails and
 * on_note_image_nth() all come through here, so the size cap and the
 * failure reporting exist once.
 *   png    — encoded bytes (PNG as written by the serializer).
 *   n_png  — their length.
 *   max_px — longest-side cap in pixels, 0 for full resolution.
 * Returns a new pixbuf reference (g_object_unref() it), or NULL when the
 * payload will not decode.
 * ------------------------------------------------------------------------- */
static GdkPixbuf *
png_decode_capped(const guint8 *png, gsize n_png, gint max_px)
{
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    if (max_px > 0)
        g_signal_connect(loader, "size-prepared",
                         G_CALLBACK(on_size_prepared),
                         GINT_TO_POINTER(max_px));

    GdkPixbuf *out = NULL;           /* the decoded image, owned            */
    GError    *err = NULL;           /* decode failure, if any              */
    if (gdk_pixbuf_loader_write(loader, png, n_png, &err) &&
        gdk_pixbuf_loader_close(loader, &err)) {
        GdkPixbuf *pixbuf =          /* borrowed from the loader            */
            gdk_pixbuf_loader_get_pixbuf(loader);
        if (pixbuf != NULL)
            out = g_object_ref(pixbuf);
    } else {
        g_warning("image: bad image data: %s",
                  err != NULL ? err->message : "unknown");
        g_clear_error(&err);
    }
    g_object_unref(loader);
    return out;
}

gboolean
on_note_deserialize(GtkTextBuffer *buffer, const guint8 *data, gsize len)
{
    return on_note_deserialize_scaled(buffer, data, len, 0);
}

gboolean
on_note_deserialize_scaled(GtkTextBuffer *buffer, const guint8 *data,
                           gsize len, gint max_img_px)
{
    on_buffer_ensure_tags(buffer);
    gtk_text_buffer_set_text(buffer, "", -1);

    OnBnbfReader r;                  /* the one record walker               */
    if (!bnbf_open(&r, data, len)) {
        g_warning("deserialize: %s", r.error);
        return FALSE;
    }

    OnBnbfRecord rec;                /* the record being built from         */
    while (bnbf_next(&r, &rec)) {
        switch (rec.type) {
        case REC_TEXT:
            insert_with_flags(buffer, rec.text, (gssize)rec.n_text,
                              rec.flags);
            break;

        case REC_IMAGE: {
            /* Decode the PNG bytes and embed an image-carrying anchor.
             * Widgets (for on-screen display) are attached separately by
             * the editor; offscreen consumers just read the anchor data.   */
            GdkPixbuf *pixbuf =      /* owned; the anchor takes its own ref */
                png_decode_capped(rec.png, rec.n_png, max_img_px);
            if (pixbuf != NULL) {
                GtkTextIter end;
                gtk_text_buffer_get_end_iter(buffer, &end);
                GtkTextChildAnchor *anchor =
                    gtk_text_buffer_create_child_anchor(buffer, &end);
                on_anchor_set_image(anchor, pixbuf,
                                    (gint)rec.display_width);
                /* Full-resolution load: keep the source PNG bytes on the
                 * pixbuf so saves emit them verbatim instead of
                 * re-encoding (see the "on-png" cache in
                 * on_note_serialize).  Scaled loads (thumbnails) never
                 * save, and their pixbuf no longer matches the bytes —
                 * skip those.                                              */
                if (max_img_px == 0)
                    g_object_set_data_full(G_OBJECT(pixbuf), "on-png",
                        g_bytes_new(rec.png, rec.n_png),
                        (GDestroyNotify)g_bytes_unref);
                g_object_unref(pixbuf);
            }
            break;
        }

        case REC_TABLE: {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            GtkTextChildAnchor *anchor =
                gtk_text_buffer_create_child_anchor(buffer, &end);
            on_anchor_set_table(anchor, rec.table);   /* takes ownership    */
            break;
        }

        case REC_CHECK: {
            GtkTextIter end;
            gtk_text_buffer_get_end_iter(buffer, &end);
            GtkTextChildAnchor *anchor =
                gtk_text_buffer_create_child_anchor(buffer, &end);
            on_anchor_set_checkbox(anchor, rec.checked);
            break;
        }

        default:
            break;                   /* bnbf_next only yields the four      */
        }
    }

    if (r.error != NULL) {
        g_warning("deserialize: %s", r.error);
        return FALSE;
    }
    if (!r.saw_end) {
        /* Ran off the end without seeing REC_END — tolerate but report.    */
        g_warning("deserialize: missing end marker");
        return FALSE;
    }
    return TRUE;
}

void
on_anchor_set_checkbox(GtkTextChildAnchor *anchor, gboolean checked)
{
    /* Encoded as 1 (unchecked) / 2 (checked) so NULL means "no checkbox". */
    g_object_set_data(G_OBJECT(anchor), "on-checkbox",
                      GINT_TO_POINTER(checked ? 2 : 1));
}

gboolean
on_anchor_is_checkbox(GtkTextChildAnchor *anchor, gboolean *out_checked)
{
    gint v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(anchor),
                                               "on-checkbox"));
    if (out_checked != NULL)
        *out_checked = (v == 2);
    return v != 0;
}

glong
on_list_prefix_chars(const gchar *head)
{
    if (g_str_has_prefix(head, "\xe2\x80\xa2 "))
        return 2;                    /* bullet + one space                  */

    glong d = 0;                     /* leading digit characters            */
    while (g_ascii_isdigit(head[d]))
        d++;
    if (d > 0 && head[d] == '.' && head[d + 1] == ' ')
        return d + 2;                /* "12. "                              */
    return 0;
}

OnTable *
on_table_new(gint rows, gint cols)
{
    OnTable *t = g_new0(OnTable, 1);
    t->rows  = MAX(1, rows);
    t->cols  = MAX(1, cols);
    t->cells = g_ptr_array_new_with_free_func(g_free);
    for (gint i = 0; i < t->rows * t->cols; i++)
        g_ptr_array_add(t->cells, g_strdup(""));
    return t;
}

void
on_table_free(OnTable *table)
{
    if (table == NULL)
        return;
    g_ptr_array_free(table->cells, TRUE);
    g_free(table);
}

const gchar *
on_table_get(OnTable *table, gint r, gint c)
{
    if (r < 0 || r >= table->rows || c < 0 || c >= table->cols)
        return "";
    return g_ptr_array_index(table->cells, r * table->cols + c);
}

void
on_table_set(OnTable *table, gint r, gint c, const gchar *text)
{
    if (r < 0 || r >= table->rows || c < 0 || c >= table->cols)
        return;
    gint i = r * table->cols + c;    /* row-major cell index                */
    g_free(g_ptr_array_index(table->cells, i));
    g_ptr_array_index(table->cells, i) =
        g_strdup(text != NULL ? text : "");
}

void
on_table_resize(OnTable *table, gint rows, gint cols)
{
    rows = MAX(1, rows);
    cols = MAX(1, cols);

    /* Build the new cell array, carrying over overlapping content.         */
    GPtrArray *cells = g_ptr_array_new_with_free_func(g_free);
    for (gint r = 0; r < rows; r++)
        for (gint c = 0; c < cols; c++)
            g_ptr_array_add(cells,
                            g_strdup((r < table->rows && c < table->cols)
                                     ? on_table_get(table, r, c) : ""));
    g_ptr_array_free(table->cells, TRUE);
    table->cells = cells;
    table->rows  = rows;
    table->cols  = cols;
}

void
on_anchor_set_table(GtkTextChildAnchor *anchor, OnTable *table)
{
    g_object_set_data_full(G_OBJECT(anchor), "on-table", table,
                           (GDestroyNotify)on_table_free);
}

OnTable *
on_anchor_get_table(GtkTextChildAnchor *anchor)
{
    return g_object_get_data(G_OBJECT(anchor), "on-table");
}

void
on_anchor_set_image(GtkTextChildAnchor *anchor, GdkPixbuf *original,
                    gint display_width)
{
    g_object_set_data_full(G_OBJECT(anchor), "on-original",
                           g_object_ref(original), g_object_unref);
    g_object_set_data(G_OBJECT(anchor), "on-display-width",
                      GINT_TO_POINTER(display_width));
}

GdkPixbuf *
on_anchor_get_image(GtkTextChildAnchor *anchor, gint *display_width)
{
    if (display_width != NULL)
        *display_width = GPOINTER_TO_INT(
            g_object_get_data(G_OBJECT(anchor), "on-display-width"));
    return g_object_get_data(G_OBJECT(anchor), "on-original");
}

gint
on_note_count_images(const guint8 *data, gsize len)
{
    OnBnbfReader r;                  /* the shared record walker            */
    if (data == NULL || !bnbf_open(&r, data, len))
        return 0;

    gint n = 0;                      /* images seen so far                  */
    OnBnbfRecord rec;                /* the record being walked past        */
    while (bnbf_next(&r, &rec)) {
        if (rec.type == REC_IMAGE)
            n++;
        else if (rec.type == REC_TABLE)
            on_table_free(rec.table);    /* the reader hands ownership over */
    }
    return n;
}

GBytes *
on_note_image_nth_png(const guint8 *data, gsize len, gint ord)
{
    OnBnbfReader r;                  /* the shared record walker            */
    if (data == NULL || ord < 0 || !bnbf_open(&r, data, len))
        return NULL;

    gint n = 0;                      /* images seen so far                  */
    OnBnbfRecord rec;                /* the record being walked past        */
    GBytes *out = NULL;              /* the one payload we copy out         */
    while (bnbf_next(&r, &rec)) {
        if (rec.type == REC_TABLE) {
            on_table_free(rec.table);
        } else if (rec.type == REC_IMAGE && n++ == ord) {
            out = g_bytes_new(rec.png, rec.n_png);
            break;
        }
    }
    return out;
}

GdkPixbuf *
on_note_image_nth(const guint8 *data, gsize len, gint ord, gint max_px)
{
    /* The copy the GBytes makes is one image's payload, alive only until
     * the decode finishes — the media browser holds one at a time.        */
    GBytes *png = on_note_image_nth_png(data, len, ord);
    if (png == NULL)
        return NULL;

    gsize n_png;                     /* payload size                        */
    const guint8 *bytes = g_bytes_get_data(png, &n_png);
    GdkPixbuf *out = png_decode_capped(bytes, n_png, max_px);
    g_bytes_unref(png);
    return out;
}

/* ---------------------------------------------------------------------------
 * probe_size_prepared() — "size-prepared" handler for on_png_probe_size():
 * records the image's declared dimensions and asks for nothing else.
 * ------------------------------------------------------------------------- */
static void
probe_size_prepared(GdkPixbufLoader *loader, gint width, gint height,
                    gpointer user_data)
{
    (void)loader;
    gint *wh = user_data;            /* [0]=width, [1]=height               */
    wh[0] = width;
    wh[1] = height;
}

gboolean
on_png_probe_size(const guint8 *png, gsize n_png, gint *w, gint *h)
{
    if (png == NULL || n_png == 0)
        return FALSE;

    gint wh[2] = { 0, 0 };           /* dimensions the header declares      */
    GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
    g_signal_connect(loader, "size-prepared",
                     G_CALLBACK(probe_size_prepared), wh);

    /* A PNG declares its size in the IHDR chunk, within the first few dozen
     * bytes, and the loader emits size-prepared as soon as it has read it —
     * so feeding it the head of the file is enough.  Closing a loader that
     * was given a truncated image fails; that error is expected and
     * discarded, since the dimensions are already in hand.                 */
    GError *err = NULL;              /* the expected truncation failure     */
    gdk_pixbuf_loader_write(loader, png, MIN(n_png, 1024), &err);
    g_clear_error(&err);
    gdk_pixbuf_loader_close(loader, &err);
    g_clear_error(&err);
    g_object_unref(loader);

    if (wh[0] <= 0 || wh[1] <= 0)
        return FALSE;
    if (w != NULL)
        *w = wh[0];
    if (h != NULL)
        *h = wh[1];
    return TRUE;
}

gchar *
on_note_extract_text(const guint8 *data, gsize len)
{
    gchar *text = NULL;              /* the concatenated plain text         */
    on_note_extract(data, len, &text, NULL);
    return text;
}

gchar *
on_note_text_cached(OnDatabase *db, gint64 id)
{
    gchar *cached = on_db_note_body_text(db, id);
    if (cached != NULL)
        return cached;

    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    if (blob == NULL)
        return g_strdup("");

    gchar *text = on_note_extract_text(blob, blob_len);
    g_free(blob);
    on_db_note_set_body_text(db, id, text);
    return text;
}

GtkTextBuffer *
on_note_buffer_load(OnDatabase *db, gint64 id, gint max_img_px)
{
    GtkTextBuffer *buffer = gtk_text_buffer_new(NULL);
    on_buffer_ensure_tags(buffer);
    gsize   blob_len = 0;            /* stored blob size                    */
    guint8 *blob = on_db_note_load(db, id, &blob_len);
    if (blob != NULL) {
        on_note_deserialize_scaled(buffer, blob, blob_len, max_img_px);
        g_free(blob);
    }
    return buffer;
}

/* ---------------------------------------------------------------------------
 * parse_due_date() — parse one date string: ISO "YYYY-MM-DD" (the form
 * the app writes) or the shorthand "M/D/YY" / "M/D/YYYY".  On success
 * *out_ts receives local midnight of that day as a UNIX timestamp.
 * ------------------------------------------------------------------------- */
static gboolean
parse_due_date(const gchar *s, gint64 *out_ts)
{
    gint y = 0, m = 0, d = 0;        /* parsed components                   */
    gchar tail;                      /* catches trailing garbage            */
    if (sscanf(s, "%d-%d-%d%c", &y, &m, &d, &tail) != 3 &&
        sscanf(s, "%d/%d/%d%c", &m, &d, &y, &tail) != 3)
        return FALSE;
    if (y < 100)
        y += 2000;                   /* "26" means 2026                     */
    if (!g_date_valid_dmy((GDateDay)d, (GDateMonth)m, (GDateYear)y))
        return FALSE;

    GDateTime *dt = g_date_time_new_local(y, m, d, 0, 0, 0);
    if (dt == NULL)
        return FALSE;
    *out_ts = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return TRUE;
}

gboolean
on_action_split_due(const gchar *rest, gsize *due_start, gint64 *due)
{
    /* The LAST word-boundary "due" whose remainder parses as a date wins
     * ("send due diligence report due 2026-07-07" keeps its text).         */
    const gchar *limit = rest + strlen(rest);   /* scan window end          */
    while (limit > rest) {
        const gchar *p = g_strrstr_len(rest, limit - rest, "due");
        if (p == NULL)
            return FALSE;
        gboolean word = (p == rest ||
                         g_ascii_isspace((guchar)p[-1])) &&
                        g_ascii_isspace((guchar)p[3]);
        if (word) {
            gchar *date = g_strstrip(g_strdup(p + 3));
            gboolean ok = parse_due_date(date, due);
            g_free(date);
            if (ok) {
                *due_start = (gsize)(p - rest);
                return TRUE;
            }
        }
        limit = p;                   /* keep scanning leftward              */
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * action_finish_line() — helper for on_note_extract_actions(): if the
 * line just ended was an action line with real text, append it to *items
 * (text trimmed, any trailing "due <date>" split off into `due`,
 * ord = list position); either way reset the line state.
 * ------------------------------------------------------------------------- */
typedef struct {
    gboolean at_start;               /* cursor sits at a line start         */
    gboolean is_action;              /* current line began with '!'         */
    gboolean struck;                 /* every rest non-space char struck?   */
    gboolean have_rest;              /* any non-space char after the '!'?   */
    GString *text;                   /* rest-of-line accumulator            */
} ActionScan;

static void
action_finish_line(ActionScan *s, GList **items, gint *ord)
{
    if (s->is_action && s->have_rest) {
        gchar *text = g_strdup(s->text->str);
        gsize  due_start;            /* where the text part ends            */
        gint64 due = 0;              /* parsed due date, 0 = none           */
        if (on_action_split_due(text, &due_start, &due))
            text[due_start] = '\0';
        g_strstrip(text);
        if (*text != '\0') {         /* a bare "! due 7/7/26" is no item    */
            OnActionItem *it = g_new0(OnActionItem, 1);
            it->text = text;
            it->done = s->struck;
            it->due  = due;
            it->ord  = (*ord)++;
            *items = g_list_prepend(*items, it);
        } else {
            g_free(text);
        }
    }
    g_string_truncate(s->text, 0);
    s->at_start  = TRUE;
    s->is_action = FALSE;
}

GList *
on_note_extract_actions(const guint8 *data, gsize len)
{
    GList *actions = NULL;           /* collected OnActionItem*             */
    on_note_extract(data, len, NULL, &actions);
    return actions;
}

void
on_note_extract(const guint8 *data, gsize len, gchar **out_text,
                GList **out_actions)
{
    GString *text = (out_text != NULL) ? g_string_new(NULL) : NULL;
    GList   *items = NULL;           /* collected OnActionItem*, reversed   */
    gint     ord   = 0;              /* next item's position index          */
    ActionScan s = { TRUE, FALSE, TRUE, FALSE, g_string_new(NULL) };
    gboolean want_actions = out_actions != NULL;

    OnBnbfReader r;                  /* the same walker the loader uses     */
    OnBnbfRecord rec;
    if (bnbf_open(&r, data, len)) {
        while (bnbf_next(&r, &rec)) {
            if (rec.type == REC_TEXT) {
                if (text != NULL)
                    g_string_append_len(text, rec.text, rec.n_text);
                for (guint32 i = 0; want_actions && i < rec.n_text; i++) {
                    gchar c = rec.text[i];
                                     /* one BYTE — '\n'/'!' are ASCII, and
                                        UTF-8 tail bytes are all >= 0x80    */
                    if (c == '\n') {
                        action_finish_line(&s, &items, &ord);
                    } else if (s.at_start) {
                        s.at_start  = FALSE;
                        s.is_action = c == '!' &&
                                      (rec.flags & ON_FMT_CODEBLOCK) == 0;
                        if (s.is_action) {  /* the '!' is not item text     */
                            s.struck    = TRUE;
                            s.have_rest = FALSE;
                        }
                    } else if (s.is_action) {
                        g_string_append_c(s.text, c);
                        if (!g_ascii_isspace((guchar)c)) {
                            s.have_rest = TRUE;
                            if ((rec.flags & ON_FMT_STRIKE) == 0)
                                s.struck = FALSE;
                        }
                    }
                }
            } else {
                /* Images, tables and checkboxes all occupy the line's first
                 * slot like any character, so such a line is never an
                 * action line.  Table cells additionally join the text,
                 * space-separated.                                         */
                if (rec.type == REC_TABLE) {
                    if (text != NULL)
                        for (gint cell = 0;
                             cell < rec.table->rows * rec.table->cols;
                             cell++) {
                            g_string_append(text,
                                g_ptr_array_index(rec.table->cells, cell));
                            g_string_append_c(text, ' ');
                        }
                    on_table_free(rec.table);
                }
                s.at_start = FALSE;
            }
        }
    }
    if (want_actions)
        action_finish_line(&s, &items, &ord);   /* line without trailing \n */

    g_string_free(s.text, TRUE);
    if (out_text != NULL)
        *out_text = g_string_free(text, FALSE);
    if (out_actions != NULL)
        *out_actions = g_list_reverse(items);
}

gchar *
on_buffer_first_line(GtkTextBuffer *buffer)
{
    GtkTextIter start, line_end;     /* span of the first line              */
    gtk_text_buffer_get_start_iter(buffer, &start);

    /* Skip leading blank lines so a note starting with newlines still
     * gets a meaningful title.                                             */
    while (gtk_text_iter_ends_line(&start) &&
           !gtk_text_iter_is_end(&start))
        gtk_text_iter_forward_line(&start);

    line_end = start;
    if (!gtk_text_iter_ends_line(&line_end))
        gtk_text_iter_forward_to_line_end(&line_end);

    gchar *text = gtk_text_buffer_get_text(buffer, &start, &line_end, FALSE);
    g_strstrip(text);

    if (*text == '\0') {
        g_free(text);
        return g_strdup(ON_DEFAULT_NOTE_TITLE);
    }
    /* Keep titles a sane length for the list views.                        */
    if (g_utf8_strlen(text, -1) > ON_TITLE_MAX_CHARS) {
        gchar *cut = g_utf8_substring(text, 0, ON_TITLE_MAX_CHARS);
        g_free(text);
        return cut;
    }
    return text;
}

GList *
on_buffer_collect_tags(GtkTextBuffer *buffer)
{
    GtkTextTagTable *table = gtk_text_buffer_get_tag_table(buffer);
    GtkTextTag *tag = gtk_text_tag_table_lookup(table, ON_TAGNAME_TAG);
    if (tag == NULL)
        return NULL;

    GList *names = NULL;             /* collected unique tag names          */
    GtkTextIter iter;                /* scan position                       */
    gtk_text_buffer_get_start_iter(buffer, &iter);

    /* Jump from tag-span to tag-span using forward_to_tag_toggle.          */
    while (TRUE) {
        if (!gtk_text_iter_starts_tag(&iter, tag)) {
            if (!gtk_text_iter_forward_to_tag_toggle(&iter, tag))
                break;
            if (!gtk_text_iter_starts_tag(&iter, tag))
                continue;
        }
        GtkTextIter span_end = iter; /* end of this tag span                */
        gtk_text_iter_forward_to_tag_toggle(&span_end, tag);

        gchar *text = gtk_text_buffer_get_text(buffer, &iter,
                                               &span_end, FALSE);
        /* Spans include the leading '#'; strip it and surrounding space.   */
        g_strstrip(text);
        const gchar *name = (*text == '#') ? text + 1 : text;
        if (*name != '\0' &&
            g_list_find_custom(names, name,
                               (GCompareFunc)g_strcmp0) == NULL)
            names = g_list_prepend(names, g_strdup(name));
        g_free(text);

        iter = span_end;
    }
    return g_list_reverse(names);
}
