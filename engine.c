// Engine: owns MuPDF. One thread renders for the UI, another builds the
// search index from its own copy of the document. See shared.h.
#include "shared.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mupdf/fitz.h>
#include <mupdf/ucdn.h>
#include <mimalloc.h>

CRITICAL_SECTION lk;
Req rq;
Info info;
Img img[NIMG];
Thumb *thumbs;
int nthumbs;
volatile LONG engine_notify_pending;
wchar_t cache_dir[MAX_PATH];

#define SANS_CSS "html, body, div, p, span, li, td, a, blockquote, h1, h2, h3, h4, h5, h6 " \
	"{ font-family: sans-serif !important; }"
#define MAX_THUMBS 200
// Case and accent insensitive: "natasha" finds "Natásha".
#define SEARCH_OPTS (FZ_SEARCH_IGNORE_CASE | FZ_SEARCH_IGNORE_DIACRITICS)

static HWND notify_hwnd;
static HANDLE wake_ev, ix_ev;

// ------------------------------------------------------- MuPDF plumbing

static void *a_malloc(void *u, size_t n) { return mi_malloc(n); }
static void *a_realloc(void *u, void *p, size_t n) { return mi_realloc(p, n); }
static void a_free(void *u, void *p) { mi_free(p); }
static fz_alloc_context alloc = { NULL, a_malloc, a_realloc, a_free };

// All contexts must share one set of locks (HarfBuzz keeps global state).
static CRITICAL_SECTION fzlock[FZ_LOCK_MAX];
static void lock_fn(void *u, int i) { EnterCriticalSection(&fzlock[i]); }
static void unlock_fn(void *u, int i) { LeaveCriticalSection(&fzlock[i]); }
static fz_locks_context locks = { NULL, lock_fn, unlock_fn };

static fz_context *new_ctx(size_t store)
{
	fz_context *c = fz_new_context(&alloc, &locks, store);
	if (!c) return NULL;
	fz_try(c) fz_register_document_handlers(c);
	fz_catch(c) { fz_drop_context(c); return NULL; }
	return c;
}

static void notify(void)
{
	if (!InterlockedExchange(&engine_notify_pending, 1))
		PostMessageW(notify_hwnd, WM_ENGINE, 0, 0);
}

int view_key(int slot_w, int slot_h, float zoom)
{
	unsigned z;
	memcpy(&z, &zoom, sizeof z);
	return (int)((unsigned)slot_w * 2654435761u ^ (unsigned)slot_h * 40503u ^ z);
}

static void utf8(const wchar_t *w, char *out, int cap)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}

static Box to_box(fz_rect r) { return (Box){ r.x0, r.y0, r.x1, r.y1 }; }

// Accelerator files cache EPUB page counts per layout, so a book that has
// been opened before never needs every chapter laid out up front.
static void accel_path(const wchar_t *file, int em, int sans, wchar_t *out)
{
	WIN32_FILE_ATTRIBUTE_DATA fa = { 0 };
	unsigned long long h = 1469598103934665603ull;
	GetFileAttributesExW(file, GetFileExInfoStandard, &fa);
	for (const wchar_t *s = file; *s; s++)
		h = (h ^ (unsigned)towlower(*s)) * 1099511628211ull;
	unsigned long long v[4] = { fa.nFileSizeLow | (unsigned long long)fa.nFileSizeHigh << 32,
		fa.ftLastWriteTime.dwLowDateTime | (unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32,
		(unsigned)em, (unsigned)sans };
	for (int i = 0; i < 4; i++)
		h = (h ^ v[i]) * 1099511628211ull;
	swprintf(out, MAX_PATH, L"%ls\\%016llx.accel", cache_dir, h);
}

static int file_exists(const wchar_t *p)
{
	return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}

// Text as the case-insensitive search sees it, keeping only letters,
// marks and digits. A page can only contain a match if its projection
// contains the projected needle, so this makes a safe search index.
static char *project(fz_context *ctx, const char *text)
{
	char *folded = fz_search_fold_text(ctx, text, SEARCH_OPTS);
	char *out = malloc(strlen(folded) + 1), *o = out;
	for (const char *s = folded; *s;) {
		int c;
		const char *start = s;
		s += fz_chartorune(&c, s);
		switch (ucdn_get_general_category(c)) {
		case UCDN_GENERAL_CATEGORY_LU: case UCDN_GENERAL_CATEGORY_LL: case UCDN_GENERAL_CATEGORY_LT:
		case UCDN_GENERAL_CATEGORY_LM: case UCDN_GENERAL_CATEGORY_LO:
		case UCDN_GENERAL_CATEGORY_MN: case UCDN_GENERAL_CATEGORY_MC: case UCDN_GENERAL_CATEGORY_ME:
		case UCDN_GENERAL_CATEGORY_ND: case UCDN_GENERAL_CATEGORY_NL: case UCDN_GENERAL_CATEGORY_NO:
			memcpy(o, start, s - start);
			o += s - start;
			break;
		}
	}
	*o = 0;
	fz_free(ctx, folded);
	return out;
}

typedef struct { fz_quad *q; int max, n; } hits_t;

static int hit_cb(fz_context *ctx, void *opaque, int nq, fz_quad *q, int chapter, int page)
{
	hits_t *h = opaque;
	for (int i = 0; i < nq && h->n < h->max; i++) h->q[h->n++] = q[i];
	return h->n >= h->max; // stop once full
}

// Like fz_search_page, with SEARCH_OPTS.
static int search_page(fz_context *ctx, fz_page *page, const char *needle, fz_quad *q, int max)
{
	fz_stext_options opts = { FZ_STEXT_DEHYPHENATE };
	fz_stext_page *st = fz_new_stext_page_from_page(ctx, page, &opts);
	hits_t h = { q, max, 0 };
	fz_try(ctx) fz_match_stext_page_cb(ctx, st, needle, hit_cb, &h, SEARCH_OPTS);
	fz_always(ctx) fz_drop_stext_page(ctx, st);
	fz_catch(ctx) fz_rethrow(ctx);
	return h.n;
}

// ------------------------------------------------------------ search index

static fz_context *ictx;
static volatile LONG ix_serial;
static struct { wchar_t path[MAX_PATH]; int em, sans, npages; unsigned gen; } ix_job;
static char **ix_text; // per page projections, owned here; usable when ix_gen == gen
static int ix_n;
static unsigned ix_gen;

static void index_drop_locked(void)
{
	if (ix_text) {
		for (int i = 0; i < ix_n; i++) free(ix_text[i]);
		free(ix_text);
	}
	ix_text = NULL;
	ix_n = 0;
	ix_gen = 0;
	info.indexed = 0;
}

static void index_cancel(void)
{
	InterlockedIncrement(&ix_serial);
	EnterCriticalSection(&lk);
	index_drop_locked();
	LeaveCriticalSection(&lk);
}

static void index_start(const wchar_t *path, int em, int sans, unsigned gen, int npages)
{
	EnterCriticalSection(&lk);
	lstrcpynW(ix_job.path, path, MAX_PATH);
	ix_job.em = em;
	ix_job.sans = sans;
	ix_job.gen = gen;
	ix_job.npages = npages;
	InterlockedIncrement(&ix_serial);
	LeaveCriticalSection(&lk);
	SetEvent(ix_ev);
}

static void index_job(fz_context *ctx, LONG my)
{
	wchar_t path[MAX_PATH], accel[MAX_PATH];
	char u[MAX_PATH * 3];
	int em, sans, npages;
	unsigned gen;
	fz_document *volatile doc = NULL;
	char **volatile texts = NULL;

	EnterCriticalSection(&lk);
	lstrcpyW(path, ix_job.path);
	em = ix_job.em, sans = ix_job.sans, gen = ix_job.gen, npages = ix_job.npages;
	LeaveCriticalSection(&lk);
	if (npages <= 0) return;

	utf8(path, u, sizeof u);
	accel_path(path, em, sans, accel);
	fz_try(ctx) {
		fz_set_user_css(ctx, sans ? SANS_CSS : "");
		if (file_exists(accel)) {
			char a[MAX_PATH * 3];
			utf8(accel, a, sizeof a);
			doc = fz_open_accelerated_document(ctx, u, a);
		} else
			doc = fz_open_document(ctx, u);
		if (fz_is_document_reflowable(ctx, doc))
			fz_layout_document(ctx, doc, PAGE_W, PAGE_H, (float)em);
		if (fz_count_pages(ctx, doc) != npages)
			fz_throw(ctx, FZ_ERROR_GENERIC, "page count mismatch");

		texts = calloc(npages, sizeof *texts);
		EnterCriticalSection(&lk);
		if (ix_serial == my) {
			index_drop_locked();
			ix_text = texts;
			ix_n = npages;
			ix_gen = gen;
		} else {
			free(texts);
			texts = NULL;
		}
		LeaveCriticalSection(&lk);

		fz_stext_options opts = { FZ_STEXT_DEHYPHENATE };
		for (int p = 0; texts && p < npages && ix_serial == my; p++) {
			fz_page *volatile page = NULL;
			fz_stext_page *volatile st = NULL;
			fz_buffer *volatile buf = NULL;
			char *proj = NULL;
			fz_try(ctx) {
				page = fz_load_page(ctx, doc, p);
				st = fz_new_stext_page_from_page(ctx, page, &opts);
				buf = fz_new_buffer_from_flattened_stext_page(ctx, st, FZ_TEXT_FLATTEN_ALL, NULL);
				proj = project(ctx, fz_string_from_buffer(ctx, buf));
			}
			fz_always(ctx) {
				fz_drop_buffer(ctx, buf);
				fz_drop_stext_page(ctx, st);
				fz_drop_page(ctx, page);
			}
			fz_catch(ctx)
				proj = NULL; // page stays unindexed and is searched directly
			EnterCriticalSection(&lk);
			if (ix_serial == my && ix_text == texts) {
				texts[p] = proj;
				info.indexed = p + 1;
			} else
				free(proj);
			LeaveCriticalSection(&lk);
		}
	}
	fz_always(ctx)
		fz_drop_document(ctx, doc);
	fz_catch(ctx) {}
}

static DWORD WINAPI index_main(LPVOID arg)
{
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	ictx = new_ctx(128u << 20);
	if (!ictx) return 0;
	for (;;) {
		WaitForSingleObject(ix_ev, INFINITE);
		index_job(ictx, ix_serial);
	}
}

// ------------------------------------------------------------- page cache

static fz_context *ctx;
static fz_document *doc;
static wchar_t path[MAX_PATH];
static unsigned gen, stamp, open_done;
static int reflow, npages, counted, em = EM_DEFAULT, sans = -1;
static unsigned hl_serial;
static int last_start; // start page published by the last open/relayout
static char needle[512];

static void img_free(Img *m)
{
	free(m->px);
	for (int i = 0; i < m->nlinks; i++) free(m->links[i].uri);
	free(m->links);
	free(m->hl);
	memset(m, 0, sizeof *m);
}

static int wanted(const Req *r, int page)
{
	int ns = r->nslots > 0 ? r->nslots : 1;
	return page >= r->cur - ns && page < r->cur + 3 * ns;
}

// Under lk. A free slot, or the least recently used one not wanted now.
static Img *img_alloc(const Req *r)
{
	Img *best = NULL;
	for (int i = 0; i < NIMG; i++)
		if (!img[i].used) return &img[i];
	for (int pass = 0; pass < 2 && !best; pass++)
		for (int i = 0; i < NIMG; i++) {
			Img *m = &img[i];
			if (pass == 0 && m->gen == gen && wanted(r, m->page)) continue;
			if (!best || m->stamp < best->stamp) best = m;
		}
	img_free(best);
	return best;
}

static Img *img_find(int page, int key, int dark)
{
	for (int i = 0; i < NIMG; i++) {
		Img *m = &img[i];
		if (m->used && m->gen == gen && m->page == page && m->key == key && m->dark == dark)
			return m;
	}
	return NULL;
}

static int have_page(int page, int key, int dark)
{
	EnterCriticalSection(&lk);
	Img *m = img_find(page, key, dark);
	if (m) m->stamp = ++stamp;
	LeaveCriticalSection(&lk);
	return m != NULL;
}

static int search_boxes(fz_page *page, Box **out)
{
	fz_quad q[256];
	int n = needle[0] ? search_page(ctx, page, needle, q, 256) : 0;
	*out = n ? malloc(n * sizeof **out) : NULL;
	for (int i = 0; i < n; i++)
		(*out)[i] = to_box(fz_rect_from_quad(q[i]));
	return n;
}

static void load_links(fz_page *page, Img *m)
{
	fz_link *links = fz_load_links(ctx, page), *l;
	int n = 0;
	for (l = links; l; l = l->next) n++;
	m->links = n ? calloc(n, sizeof *m->links) : NULL;
	fz_try(ctx) {
		for (l = links; l; l = l->next) {
			Link *k = &m->links[m->nlinks++];
			k->r = to_box(l->rect);
			k->page = -1;
			if (!l->uri) continue;
			if (fz_is_external_link(ctx, l->uri))
				k->uri = _strdup(l->uri);
			else {
				float x, y;
				k->page = fz_page_number_from_location(ctx, doc, fz_resolve_link(ctx, doc, l->uri, &x, &y));
			}
		}
	}
	fz_always(ctx)
		fz_drop_link(ctx, links);
	fz_catch(ctx) {}
}

// Records where images land on the page, so dark mode can leave them untinted.
#define MAX_IMG_BOXES 32
typedef struct {
	fz_device super;
	fz_rect box[MAX_IMG_BOXES];
	int n;
} ImgBoxDev;

static void imgbox_fill_image(fz_context *c, fz_device *d, fz_image *img, fz_matrix ctm, float alpha, fz_color_params cp)
{
	ImgBoxDev *b = (ImgBoxDev *)d;
	fz_rect r = fz_transform_rect(fz_unit_rect, ctm);
	if (fz_is_empty_rect(r)) return;
	if (b->n < MAX_IMG_BOXES) b->box[b->n++] = r;
	else b->box[MAX_IMG_BOXES - 1] = fz_union_rect(b->box[MAX_IMG_BOXES - 1], r);
}

// Draw `page` into `pix` (cleared to white first). Dark mode inverts the page
// to light-on-dark, then redraws image areas so pictures keep their colours.
static void draw_page(fz_page *page, fz_pixmap *pix, fz_matrix m, int dark)
{
	fz_device *volatile dev = NULL;
	ImgBoxDev *volatile boxes = NULL;

	fz_clear_pixmap_with_value(ctx, pix, 255);
	fz_try(ctx) {
		if (dark) {
			boxes = fz_new_derived_device(ctx, ImgBoxDev);
			boxes->super.fill_image = imgbox_fill_image;
			fz_run_page(ctx, page, &boxes->super, m, NULL);
			fz_close_device(ctx, &boxes->super);
		}
		dev = fz_new_draw_device(ctx, fz_identity, pix);
		fz_run_page(ctx, page, dev, m, NULL);
		fz_close_device(ctx, dev);
		if (dark) {
			fz_tint_pixmap(ctx, pix, 0xE0E0E0, 0x1E1E1E);
			for (int i = 0; i < boxes->n; i++) {
				fz_irect r = fz_intersect_irect(fz_round_rect(boxes->box[i]), fz_pixmap_bbox(ctx, pix));
				if (fz_is_empty_irect(r)) continue;
				fz_drop_device(ctx, dev);
				dev = NULL;
				fz_clear_pixmap_rect_with_value(ctx, pix, 255, r);
				dev = fz_new_draw_device_with_bbox(ctx, fz_identity, pix, &r);
				fz_run_page(ctx, page, dev, m, NULL);
				fz_close_device(ctx, dev);
			}
		}
	}
	fz_always(ctx) {
		fz_drop_device(ctx, dev);
		fz_drop_device(ctx, (fz_device *)boxes);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

// Render a page for the current view: global page `pn`, or when the
// document is not counted yet, the preview at `loc`.
static int render(const Req *r, int pn, fz_location loc, int key)
{
	fz_page *volatile page = NULL;
	fz_pixmap *volatile pix = NULL;
	Img t = { 0 };

	fz_try(ctx) {
		page = pn >= 0 ? fz_load_page(ctx, doc, pn) : fz_load_chapter_page(ctx, doc, loc.chapter, loc.page);
		fz_rect b = fz_bound_page(ctx, page);
		float pw = b.x1 - b.x0, ph = b.y1 - b.y0;
		if (pw < 1 || ph < 1) fz_throw(ctx, FZ_ERROR_GENERIC, "empty page");
		float scale = fminf(r->slot_w / pw, r->slot_h / ph) * (reflow ? 1.0f : r->zoom);
		fz_matrix m = fz_scale(scale, scale);
		fz_irect bb = fz_round_rect(fz_transform_rect(b, m));
		t.w = bb.x1 - bb.x0;
		t.h = bb.y1 - bb.y0;
		t.px = malloc((size_t)t.w * t.h * 4);
		if (!t.px) fz_throw(ctx, FZ_ERROR_SYSTEM, "out of memory");
		pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_bgr(ctx), bb, NULL, 1, t.px);
		draw_page(page, pix, m, r->dark);

		t.scale = scale;
		t.bounds = to_box(b);
		t.key = key;
		t.dark = r->dark;
		t.page = pn;
		t.gen = gen;
		if (pn >= 0) {
			fz_location l = fz_location_from_page_number(ctx, doc, pn);
			t.chapter = l.chapter;
			t.chapter_page = l.page;
			t.chapter_pages = fz_count_chapter_pages(ctx, doc, l.chapter);
			load_links(page, &t);
		} else {
			t.chapter = loc.chapter;
			t.chapter_page = loc.page;
			t.chapter_pages = fz_count_chapter_pages(ctx, doc, loc.chapter);
		}
		t.nhl = search_boxes(page, &t.hl);
		t.hl_serial = hl_serial;
	}
	fz_always(ctx) {
		fz_drop_pixmap(ctx, pix);
		fz_drop_page(ctx, page);
	}
	fz_catch(ctx) {
		// publish an empty entry so the page is not retried endlessly
		img_free(&t);
		t.key = key;
		t.dark = r->dark;
		t.page = pn;
		t.gen = gen;
		t.hl_serial = hl_serial;
	}

	EnterCriticalSection(&lk);
	Img *m = img_alloc(r);
	*m = t;
	m->used = 1;
	m->stamp = ++stamp;
	if (t.px && (pn < 0 || pn == 0))
		info.aspect = (t.bounds.y1 - t.bounds.y0) / (t.bounds.x1 - t.bounds.x0);
	LeaveCriticalSection(&lk);
	notify();
	return 1;
}

static int render_thumb(int p, int tw, int dark)
{
	fz_page *volatile page = NULL;
	fz_pixmap *volatile pix = NULL;
	Thumb t = { 0 };

	fz_try(ctx) {
		page = fz_load_page(ctx, doc, p);
		fz_rect b = fz_bound_page(ctx, page);
		float scale = tw / (b.x1 - b.x0);
		fz_matrix m = fz_scale(scale, scale);
		fz_irect bb = fz_round_rect(fz_transform_rect(b, m));
		t.w = bb.x1 - bb.x0;
		t.h = bb.y1 - bb.y0;
		t.px = malloc((size_t)t.w * t.h * 4);
		if (!t.px) fz_throw(ctx, FZ_ERROR_SYSTEM, "out of memory");
		pix = fz_new_pixmap_with_bbox_and_data(ctx, fz_device_bgr(ctx), bb, NULL, 1, t.px);
		draw_page(page, pix, m, dark);
	}
	fz_always(ctx) {
		fz_drop_pixmap(ctx, pix);
		fz_drop_page(ctx, page);
	}
	fz_catch(ctx) {
		free(t.px);
		t.px = NULL;
		t.w = t.h = 1; // remember the failure so it is not retried
	}
	t.dark = dark;
	t.gen = gen;
	t.tw = tw;

	EnterCriticalSection(&lk);
	if (thumbs && p < nthumbs) {
		free(thumbs[p].px);
		t.stamp = ++stamp;
		thumbs[p] = t;
		// keep memory bounded: drop the least recently rendered ones
		int n = 0;
		for (int i = 0; i < nthumbs; i++) n += thumbs[i].px != NULL;
		while (n > MAX_THUMBS) {
			int old = -1;
			for (int i = 0; i < nthumbs; i++)
				if (thumbs[i].px && (old < 0 || thumbs[i].stamp < thumbs[old].stamp)) old = i;
			free(thumbs[old].px);
			memset(&thumbs[old], 0, sizeof thumbs[old]);
			n--;
		}
	} else
		free(t.px);
	LeaveCriticalSection(&lk);
	notify();
	return 1;
}

// --------------------------------------------------------- open / layout

static void reset_doc_state_locked(int keep_pages)
{
	// keep_pages: a reload of the same book; the old pages stay on screen
	// (as a previous generation) until the new layout has something to show
	if (!keep_pages)
		for (int i = 0; i < NIMG; i++) img_free(&img[i]);
	if (thumbs) {
		for (int i = 0; i < nthumbs; i++) free(thumbs[i].px);
		free(thumbs);
	}
	thumbs = NULL;
	nthumbs = 0;
}

// After the preview is up: count every page, then tell the UI where it is.
static void finish_count(const wchar_t *file, fz_location loc, int save_accel)
{
	int n = fz_count_pages(ctx, doc);
	int start = fz_page_number_from_location(ctx, doc, loc);
	if (start < 0 || start >= n) start = 0;
	if (save_accel && fz_document_supports_accelerator(ctx, doc)) {
		wchar_t a[MAX_PATH];
		char a8[MAX_PATH * 3];
		accel_path(file, em, sans, a);
		utf8(a, a8, sizeof a8);
		fz_try(ctx) fz_save_accelerator(ctx, doc, a8);
		fz_catch(ctx) {}
	}
	npages = n;
	counted = 1;
	last_start = start;

	EnterCriticalSection(&lk);
	for (int i = 0; i < NIMG; i++)
		if (img[i].used && img[i].gen == gen && img[i].page < 0)
			img[i].page = start; // the preview becomes a normal page
	if (thumbs) {
		for (int i = 0; i < nthumbs; i++) free(thumbs[i].px);
		free(thumbs);
	}
	thumbs = calloc(n, sizeof *thumbs);
	nthumbs = thumbs ? n : 0;
	info.npages = n;
	info.counted = 1;
	info.busy = 0;
	info.state = DOC_READY;
	info.start_page = start;
	info.start_serial++;
	LeaveCriticalSection(&lk);
	notify();
	index_start(file, em, sans, gen, n);
}

static void preview(const Req *r, fz_location loc)
{
	if (r->slot_w > 0 && r->slot_h > 0)
		render(r, -1, loc, view_key(r->slot_w, r->slot_h, r->zoom));
}

static void do_open(const Req *r)
{
	char u[MAX_PATH * 3];
	wchar_t accel[MAX_PATH];
	int accel_ok = 0;

	index_cancel();
	fz_drop_document(ctx, doc);
	doc = NULL;
	counted = 0;
	npages = 0;
	gen++;
	EnterCriticalSection(&lk);
	reset_doc_state_locked(r->reload);
	unsigned fd = info.find_done;
	memset(&info, 0, sizeof info);
	info.find_done = fd;
	info.find_page = -1;
	info.doc_serial = r->open_serial;
	info.state = DOC_LOADING;
	info.busy = 1;
	info.gen = gen;
	LeaveCriticalSection(&lk);
	notify();

	lstrcpyW(path, r->open_path);
	utf8(path, u, sizeof u);
	em = r->em;
	if (sans != r->sans) {
		sans = r->sans;
		fz_set_user_css(ctx, sans ? SANS_CSS : "");
	}
	accel_path(path, em, sans, accel);

	fz_try(ctx) {
		if (file_exists(accel)) {
			char a[MAX_PATH * 3];
			utf8(accel, a, sizeof a);
			doc = fz_open_accelerated_document(ctx, u, a);
			accel_ok = 1;
		} else
			doc = fz_open_document(ctx, u);
		reflow = fz_is_document_reflowable(ctx, doc);
		if (reflow)
			fz_layout_document(ctx, doc, PAGE_W, PAGE_H, (float)em);

		// Resume position: exact when the layout matches, else proportional
		// within the chapter. Only that chapter needs laying out to show it.
		int nch = fz_count_chapters(ctx, doc);
		int c = r->open_chapter >= 0 && r->open_chapter < nch ? r->open_chapter : 0;
		int cp = fz_count_chapter_pages(ctx, doc, c);
		while (cp <= 0 && c + 1 < nch) cp = fz_count_chapter_pages(ctx, doc, ++c);
		int p = r->open_page;
		if (r->open_pages > 0 && (r->open_pages != cp || r->open_em != em))
			p = (int)((long long)p * cp / r->open_pages);
		if (p >= cp) p = cp - 1;
		if (p < 0) p = 0;
		fz_location loc = fz_make_location(c, p);

		EnterCriticalSection(&lk);
		info.reflow = reflow;
		LeaveCriticalSection(&lk);
		preview(r, loc);
		finish_count(path, loc, !accel_ok);
	}
	fz_catch(ctx) {
		fz_drop_document(ctx, doc);
		doc = NULL;
		EnterCriticalSection(&lk);
		info.state = DOC_FAILED;
		info.busy = 0;
		LeaveCriticalSection(&lk);
		notify();
	}
}

// Font size change: keep the exact position with a bookmark.
static void do_relayout(const Req *r)
{
	fz_location loc = fz_make_location(0, 0);
	fz_bookmark mark = 0;
	int old_cp = 1;

	index_cancel();
	fz_try(ctx) {
		// If the UI hasn't caught up with the previous layout yet, its page
		// numbers belong to an older layout: use where we last put it.
		int pos = r->view_gen != gen ? last_start : r->anchor >= 0 && r->anchor < npages ? r->anchor : r->cur;
		loc = fz_location_from_page_number(ctx, doc, pos);
		old_cp = fz_count_chapter_pages(ctx, doc, loc.chapter);
		mark = fz_make_bookmark(ctx, doc, loc);
	}
	fz_catch(ctx) {}

	em = r->em;
	counted = 0;
	gen++;
	EnterCriticalSection(&lk);
	info.busy = 1;
	info.counted = 0;
	info.gen = gen;
	LeaveCriticalSection(&lk);
	notify();

	fz_try(ctx) {
		fz_layout_document(ctx, doc, PAGE_W, PAGE_H, (float)em);
		fz_location l = mark ? fz_lookup_bookmark(ctx, doc, mark) : fz_make_location(-1, -1);
		if (l.chapter < 0) {
			int cp = fz_count_chapter_pages(ctx, doc, loc.chapter);
			l = fz_make_location(loc.chapter, old_cp > 0 ? (int)((long long)loc.page * cp / old_cp) : 0);
		}
		preview(r, l);
		finish_count(path, l, 1);
	}
	fz_catch(ctx) {
		EnterCriticalSection(&lk);
		info.state = DOC_FAILED;
		info.busy = 0;
		LeaveCriticalSection(&lk);
		notify();
	}
}

// ------------------------------------------------------------------ find

static unsigned f_serial;
static int f_active, f_page, f_left, f_dir;
static char f_needle[512], *f_proj;

static void find_publish(int page)
{
	f_active = 0;
	EnterCriticalSection(&lk);
	info.find_done = f_serial;
	info.find_page = page;
	LeaveCriticalSection(&lk);
	notify();
}

static void find_begin(const Req *r)
{
	f_serial = r->find_serial;
	strcpy(f_needle, r->needle);
	free(f_proj);
	f_proj = NULL;
	if (!f_needle[0] || npages <= 0) {
		find_publish(-1);
		return;
	}
	fz_try(ctx) f_proj = project(ctx, f_needle);
	fz_catch(ctx) f_proj = NULL;
	f_dir = r->find_dir < 0 ? -1 : 1;
	f_page = ((r->find_from % npages) + npages) % npages;
	f_left = npages;
	f_active = 1;
}

// Check pages until a few ms have passed. Indexed pages that cannot match
// are skipped without touching the document.
static void find_step(void)
{
	ULONGLONG t0 = GetTickCount64();
	while (f_left > 0) {
		int p = f_page, cand = 1;
		EnterCriticalSection(&lk);
		if (f_proj && ix_text && ix_gen == gen && p < ix_n && ix_text[p])
			cand = strstr(ix_text[p], f_proj) != NULL;
		LeaveCriticalSection(&lk);
		if (cand) {
			int hits = 0;
			fz_page *volatile pg = NULL;
			fz_try(ctx) {
				fz_quad q[1];
				pg = fz_load_page(ctx, doc, p);
				hits = search_page(ctx, pg, f_needle, q, 1);
			}
			fz_always(ctx) fz_drop_page(ctx, pg);
			fz_catch(ctx) {}
			if (hits > 0) {
				find_publish(p);
				return;
			}
		}
		f_page = ((p + f_dir) % npages + npages) % npages;
		f_left--;
		if (GetTickCount64() - t0 > 4) return;
	}
	find_publish(-1);
}

// ------------------------------------------------------------ main loop

static int do_visible(const Req *r, int key)
{
	for (int i = 0; i < r->nslots; i++) {
		int p = r->cur + i;
		if (p < npages && !have_page(p, key, r->dark))
			return render(r, p, fz_make_location(0, 0), key);
	}
	return 0;
}

static int do_highlights(const Req *r, int key)
{
	for (int i = 0; i < r->nslots; i++) {
		int p = r->cur + i, stale;
		EnterCriticalSection(&lk);
		Img *m = img_find(p, key, r->dark);
		stale = m && m->hl_serial != hl_serial;
		LeaveCriticalSection(&lk);
		if (!stale) continue;
		Box *hl = NULL;
		int n = 0;
		fz_page *volatile pg = NULL;
		fz_try(ctx) {
			pg = fz_load_page(ctx, doc, p);
			n = search_boxes(pg, &hl);
		}
		fz_always(ctx) fz_drop_page(ctx, pg);
		fz_catch(ctx) {}
		EnterCriticalSection(&lk);
		m = img_find(p, key, r->dark);
		if (m) {
			free(m->hl);
			m->hl = hl;
			m->nhl = n;
			m->hl_serial = hl_serial;
		} else
			free(hl);
		LeaveCriticalSection(&lk);
		notify();
		return 1;
	}
	return 0;
}

static int do_prefetch(const Req *r, int key)
{
	int ns = r->nslots > 0 ? r->nslots : 1;
	int order[] = { r->cur + ns, r->cur - ns, r->cur + 2 * ns };
	for (int k = 0; k < 3; k++)
		for (int i = 0; i < ns; i++) {
			int p = order[k] + i;
			if (p >= 0 && p < npages && !have_page(p, key, r->dark))
				return render(r, p, fz_make_location(0, 0), key);
		}
	return 0;
}

static int do_thumbs(const Req *r)
{
	if (r->thumb_w <= 0 || r->thumb_b < r->thumb_a) return 0;
	int a = r->thumb_a, b = r->thumb_b + (r->thumb_b - r->thumb_a + 1); // visible, then a screen ahead
	for (int p = a; p <= b && p < npages; p++) {
		if (p < 0) continue;
		int need;
		EnterCriticalSection(&lk);
		need = thumbs && p < nthumbs && (thumbs[p].tw == 0 || thumbs[p].gen != gen ||
			(thumbs[p].px && (thumbs[p].dark != r->dark || thumbs[p].tw != r->thumb_w)));
		LeaveCriticalSection(&lk);
		if (need)
			return render_thumb(p, r->thumb_w, r->dark);
	}
	return 0;
}

static DWORD WINAPI engine_main(LPVOID arg)
{
	static Req r;
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	ctx = new_ctx(512u << 20);
	if (!ctx) return 0;
	fz_set_user_css(ctx, "");
	sans = 0;
	CreateThread(NULL, 64u << 20, index_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);

	for (;;) {
		EnterCriticalSection(&lk);
		r = rq;
		LeaveCriticalSection(&lk);

		if (r.open_serial != open_done) {
			open_done = r.open_serial;
			if (r.open_path[0]) do_open(&r);
			continue;
		}
		if (doc && reflow && counted && r.em != em) {
			do_relayout(&r);
			continue;
		}
		if (!doc || !counted || r.slot_w <= 0 || r.slot_h <= 0) {
			WaitForSingleObject(wake_ev, INFINITE);
			continue;
		}
		if (r.needle_serial != hl_serial) {
			hl_serial = r.needle_serial;
			strcpy(needle, r.needle);
		}
		int key = view_key(r.slot_w, r.slot_h, r.zoom);
		if (do_visible(&r, key)) continue;
		if (do_highlights(&r, key)) continue;
		if (r.find_serial != f_serial) find_begin(&r);
		if (f_active) { find_step(); continue; }
		if (do_prefetch(&r, key)) continue;
		if (do_thumbs(&r)) continue;
		WaitForSingleObject(wake_ev, INFINITE);
	}
}

void engine_start(HWND notify_to)
{
	notify_hwnd = notify_to;
	for (int i = 0; i < FZ_LOCK_MAX; i++) InitializeCriticalSection(&fzlock[i]);
	wake_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	ix_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
	CreateThread(NULL, 256u << 20, engine_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
}

void engine_wake(void)
{
	SetEvent(wake_ev);
}
