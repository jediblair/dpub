// dpub (speeD ePUB): minimal EPUB / MOBI / PDF reader. Win32 + GDI user interface.
// All document work happens on the engine threads (engine.c); this thread
// only publishes what it wants to see (shared.h) and paints what is ready,
// so it never waits on the document.
#include "shared.h"
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define APPNAME L"dpub"
#define EM_MIN 6
#define EM_MAX 48
#define ZOOM_MIN 0.5f
#define ZOOM_MAX 4.0f
#define ID_EDIT 100
#define TM_STATUS 1
#define SNAP_MAGIC 0x50414E53

enum { B_OPEN, B_PREV, B_NEXT, B_PREVIEWS, B_VIEW, B_ZOUT, B_ZIN, B_FONT, B_THEME, B_COUNT };
static const wchar_t *btn_label[B_COUNT] = {
	L"Open", L"◀", L"▶", L"Previews", L"2 pages", L"A−", L"A+", L"Sans", L"Dark"
};

typedef struct { COLORREF bar, view, text, dim, hot, sel, page; } Theme;
static const Theme themes[2] = {
	{ 0xEEEEEE, 0xC8C8C8, 0x202020, 0x707070, 0xDADADA, 0xE0B070, 0xFFFFFF },
	{ 0x2B2B2B, 0x1A1A1A, 0xE6E6E6, 0x909090, 0x3C3C3C, 0x8A5A20, 0x1E1E1E },
};

// ------------------------------------------------------------------- state

static HWND hwnd, hedit;
static HFONT font;
static HBRUSH br_edit;
static int dpi = 96;
static wchar_t ini_path[MAX_PATH], snap_path[MAX_PATH];

static wchar_t cur_path[MAX_PATH];
static unsigned doc_serial;   // our latest open request
static Info ui;               // last engine state seen for doc_serial
static unsigned start_seen, find_seen;
static int cur, pending;      // current page; navigation made before pages are counted
static int anchor;            // unrounded reading position (see Req.anchor)

static int dark, previews = 1, multi, em = EM_DEFAULT, sans;
static float zoom = 1.0f;
static int pan_x, pan_y;

static unsigned needle_serial, find_serial;
static char last_find[512];
static wchar_t status[64];

static RECT btn_rc[B_COUNT], info_rc;
static int hot_btn = -1;
static int thumb_scroll;

// where each visible page was last drawn, for clicks
typedef struct { int valid, page, x, y, w, h; float sx; Box b; } Drawn;
static Drawn drawn[2];

// startup snapshot: the last screen of the previous session
typedef struct {
	unsigned magic, version;
	wchar_t path[MAX_PATH];
	unsigned long long fsize, ftime;
	int em, sans, dark, multi, previews, dpi, cw, ch, cur, npages, n;
	float zoom;
} SnapHdr;
typedef struct { int x, y, w, h; } SnapImg;
static struct { int on; SnapHdr h; SnapImg im[2]; unsigned char *px[2]; } snap;

static int S(int v) { return MulDiv(v, dpi, 96); }
static const Theme *T(void) { return &themes[dark]; }

static int loaded(void) { return doc_serial && ui.doc_serial == doc_serial && ui.state != DOC_NONE && ui.state != DOC_FAILED; }
static int counted(void) { return loaded() && ui.counted; }

static void utf8(const wchar_t *w, char *out, int cap)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
}

static int toolbar_h(void) { return S(40); }
static int sidebar_w(void) { return previews && cur_path[0] ? S(150) : 0; }
static int nslots(void) { return multi && counted() && cur + 1 < ui.npages ? 2 : 1; }
static int step(void) { return multi ? 2 : 1; }

static int scrollbar_w(void) { return cur_path[0] ? S(14) : 0; }

static void view_rect(RECT *r)
{
	GetClientRect(hwnd, r);
	r->top = toolbar_h();
	r->left = sidebar_w();
	r->right -= scrollbar_w();
}

// ---------------------------------------------------------------- scrollbar
// A whole-book scrollbar on the right: the thumb covers the visible pages.

static int sb_drag = -1;     // grab offset within the thumb while dragging, -1 when not
static int sb_hot;
static int sb_label = -1;    // page shown in the drag label

static void sb_track(RECT *r)
{
	GetClientRect(hwnd, r);
	r->left = r->right - scrollbar_w();
	r->top = toolbar_h();
}

// Thumb position and size for the current page (pixels from the track top).
static void sb_thumb(int *y, int *h)
{
	RECT t;
	sb_track(&t);
	int th = t.bottom - t.top, n = ui.npages > 0 ? ui.npages : 1, vis = nslots();
	*h = (int)((long long)th * vis / n);
	if (*h < S(28)) *h = S(28);
	if (*h > th) *h = th;
	int span = n - vis;
	*y = span > 0 ? (int)((long long)(th - *h) * cur / span) : 0;
}

// Page for a thumb whose top is at y (pixels from the track top).
static int sb_page_at(int y)
{
	RECT t;
	int ty, th;
	sb_track(&t);
	sb_thumb(&ty, &th);
	int range = t.bottom - t.top - th, span = ui.npages - nslots();
	if (range <= 0 || span <= 0) return 0;
	if (y < 0) y = 0;
	if (y > range) y = range;
	return (int)(((long long)y * span + range / 2) / range);
}

static void slot_rect(int i, RECT *r)
{
	RECT v;
	view_rect(&v);
	int n = nslots(), w = (v.right - v.left) / n;
	*r = (RECT){ v.left + w * i, v.top, v.left + w * (i + 1), v.bottom };
}

// Space a page may fill in one slot (device pixels).
static void slot_box(int *w, int *h)
{
	RECT r;
	slot_rect(0, &r);
	*w = r.right - r.left - 2 * S(10);
	*h = r.bottom - r.top - 2 * S(10);
	if (*w < 0) *w = 0;
	if (*h < 0) *h = 0;
}

static void file_id(const wchar_t *p, unsigned long long *size, unsigned long long *time)
{
	WIN32_FILE_ATTRIBUTE_DATA fa = { 0 };
	GetFileAttributesExW(p, GetFileExInfoStandard, &fa);
	*size = fa.nFileSizeLow | (unsigned long long)fa.nFileSizeHigh << 32;
	*time = fa.ftLastWriteTime.dwLowDateTime | (unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32;
}

// --------------------------------------------------------------- settings

static void ini_int(const wchar_t *key, int v)
{
	wchar_t b[16];
	wsprintfW(b, L"%d", v);
	WritePrivateProfileStringW(L"settings", key, b, ini_path);
}

static int ini_get(const wchar_t *key, int def)
{
	return GetPrivateProfileIntW(L"settings", key, def, ini_path);
}

// Under lk: the best image to show for `page` in the current view.
static Img *pick(int page, int key)
{
	Img *exact = NULL, *same = NULL, *old = NULL;
	for (int i = 0; i < NIMG; i++) {
		Img *m = &img[i];
		if (!m->used) continue;
		if (m->gen == ui.gen && m->page == page) {
			if (m->key == key && m->dark == dark) exact = m;
			else if (!same || m->stamp > same->stamp) same = m;
		} else if (ui.busy && m->gen != ui.gen && m->page == cur) {
			if (!old || m->stamp > old->stamp) old = m; // previous layout while relaying out
		}
	}
	return exact ? exact : same ? same : old;
}

// Remember the reading position as chapter, page in chapter, pages in chapter, em.
static void save_pos(void)
{
	if (!cur_path[0] || !counted()) return;
	int c = -1, p = 0, n = 0;
	EnterCriticalSection(&lk);
	for (int i = 0; i < NIMG; i++)
		if (img[i].used && img[i].gen == ui.gen && img[i].page == cur) {
			c = img[i].chapter, p = img[i].chapter_page, n = img[i].chapter_pages;
			break;
		}
	LeaveCriticalSection(&lk);
	if (c < 0) return;
	wchar_t b[64];
	swprintf(b, 64, L"%d %d %d %d", c, p, n, em);
	WritePrivateProfileStringW(L"pos", cur_path, b, ini_path);
}

static void save_settings(void)
{
	ini_int(L"dark", dark);
	ini_int(L"previews", previews);
	ini_int(L"multi", multi);
	ini_int(L"em", em);
	ini_int(L"sans", sans);
	WINDOWPLACEMENT wp = { sizeof wp };
	GetWindowPlacement(hwnd, &wp);
	ini_int(L"x", wp.rcNormalPosition.left);
	ini_int(L"y", wp.rcNormalPosition.top);
	ini_int(L"w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
	ini_int(L"h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
	ini_int(L"max", wp.showCmd == SW_SHOWMAXIMIZED);
	WritePrivateProfileStringW(L"settings", L"last", cur_path, ini_path);
	save_pos();
}

static int system_dark(void)
{
	DWORD v = 1, sz = sizeof v;
	RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
		L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &v, &sz);
	return !v;
}

// ---------------------------------------------------------------- snapshot

static void snapshot_save(void)
{
	if (!counted() || !cur_path[0]) return;
	RECT c;
	GetClientRect(hwnd, &c);
	SnapHdr h = { SNAP_MAGIC, 2 };
	lstrcpyW(h.path, cur_path);
	file_id(cur_path, &h.fsize, &h.ftime);
	h.em = em, h.sans = sans, h.dark = dark, h.multi = multi, h.previews = previews, h.dpi = dpi;
	h.cw = c.right, h.ch = c.bottom, h.cur = cur, h.npages = ui.npages, h.zoom = zoom;
	int bw, bh;
	slot_box(&bw, &bh);
	int key = view_key(bw, bh, zoom);

	HANDLE f = CreateFileW(snap_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD wr;
	EnterCriticalSection(&lk);
	Img *ms[2] = { NULL, NULL };
	for (int i = 0; i < nslots(); i++) {
		Img *m = pick(cur + i, key);
		if (m && m->px && m->key == key && m->dark == dark && drawn[i].valid && drawn[i].page == cur + i)
			ms[h.n++] = m;
	}
	WriteFile(f, &h, sizeof h, &wr, NULL);
	for (int i = 0; i < h.n; i++) {
		SnapImg si = { drawn[i].x, drawn[i].y, ms[i]->w, ms[i]->h };
		WriteFile(f, &si, sizeof si, &wr, NULL);
		WriteFile(f, ms[i]->px, (DWORD)ms[i]->w * ms[i]->h * 4, &wr, NULL);
	}
	LeaveCriticalSection(&lk);
	CloseHandle(f);
}

static void snapshot_load(const wchar_t *path)
{
	HANDLE f = CreateFileW(snap_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
	if (f == INVALID_HANDLE_VALUE) return;
	DWORD rd;
	SnapHdr h;
	RECT c;
	unsigned long long fs, ft;
	GetClientRect(hwnd, &c);
	file_id(path, &fs, &ft);
	if (ReadFile(f, &h, sizeof h, &rd, NULL) && rd == sizeof h && h.magic == SNAP_MAGIC && h.version == 2 &&
		!lstrcmpiW(h.path, path) && h.fsize == fs && h.ftime == ft && h.em == em && h.sans == sans &&
		h.dark == dark && h.multi == multi && h.previews == previews && h.dpi == dpi &&
		h.cw == c.right && h.ch == c.bottom && h.n >= 1 && h.n <= 2) {
		snap.h = h;
		snap.on = 1;
		for (int i = 0; i < h.n && snap.on; i++) {
			SnapImg *si = &snap.im[i];
			if (!ReadFile(f, si, sizeof *si, &rd, NULL) || rd != sizeof *si || si->w <= 0 || si->h <= 0 || si->w > 16384 || si->h > 16384) {
				snap.on = 0;
				break;
			}
			DWORD n = (DWORD)si->w * si->h * 4;
			snap.px[i] = malloc(n);
			if (!snap.px[i] || !ReadFile(f, snap.px[i], n, &rd, NULL) || rd != n)
				snap.on = 0;
		}
	}
	CloseHandle(f);
	if (!snap.on)
		for (int i = 0; i < 2; i++) { free(snap.px[i]); snap.px[i] = NULL; }
}

static void snapshot_drop(void)
{
	if (!snap.on) return;
	snap.on = 0;
	for (int i = 0; i < 2; i++) { free(snap.px[i]); snap.px[i] = NULL; }
}

// ---------------------------------------------------------------- requests

static void thumb_view_range(int *first, int *last);

// Publish the current view to the engine.
static void push(void)
{
	int bw, bh, a = 0, b = -1;
	slot_box(&bw, &bh);
	if (previews && counted()) thumb_view_range(&a, &b);
	EnterCriticalSection(&lk);
	rq.cur = cur;
	rq.view_gen = ui.gen;
	rq.anchor = anchor >= cur && anchor < cur + nslots() ? anchor : cur;
	rq.nslots = nslots();
	rq.slot_w = bw;
	rq.slot_h = bh;
	rq.zoom = zoom;
	rq.dark = dark;
	rq.em = em;
	rq.thumb_a = a;
	rq.thumb_b = b;
	rq.thumb_w = sidebar_w() - S(24);
	LeaveCriticalSection(&lk);
	engine_wake();
}

static void update_title(void)
{
	wchar_t t[MAX_PATH + 64];
	if (!cur_path[0]) { SetWindowTextW(hwnd, APPNAME); return; }
	const wchar_t *name = wcsrchr(cur_path, L'\\');
	name = name ? name + 1 : cur_path;
	if (snap.on && !counted())
		wsprintfW(t, L"%s — %d / %d", name, snap.h.cur + 1, snap.h.npages);
	else if (!counted())
		wsprintfW(t, L"%s", name);
	else if (nslots() == 2)
		wsprintfW(t, L"%s — %d-%d / %d", name, cur + 1, cur + 2, ui.npages);
	else
		wsprintfW(t, L"%s — %d / %d", name, cur + 1, ui.npages);
	SetWindowTextW(hwnd, t);
}

static void set_status(const wchar_t *s)
{
	lstrcpynW(status, s, 64);
	InvalidateRect(hwnd, &(RECT){ 0, 0, 10000, toolbar_h() }, FALSE);
}

static void open_path(const wchar_t *path, int reload)
{
	int c = 0, p = 0, n = 0, e = em;
	if (reload) {
		// keep the current position: take it from what is on screen
		EnterCriticalSection(&lk);
		for (int i = 0; i < NIMG; i++)
			if (img[i].used && img[i].gen == ui.gen && img[i].page == cur) {
				c = img[i].chapter, p = img[i].chapter_page, n = img[i].chapter_pages;
				break;
			}
		LeaveCriticalSection(&lk);
	} else {
		save_pos();
		wchar_t b[64];
		GetPrivateProfileStringW(L"pos", path, L"", b, 64, ini_path);
		if (swscanf(b, L"%d %d %d %d", &c, &p, &n, &e) != 4) c = p = n = 0, e = em;
	}
	if (!reload)
		snapshot_drop();
	lstrcpynW(cur_path, path, MAX_PATH);
	EnterCriticalSection(&lk);
	rq.open_serial = ++doc_serial;
	lstrcpyW(rq.open_path, path);
	rq.open_chapter = c;
	rq.open_page = p;
	rq.open_pages = n;
	rq.open_em = e;
	rq.reload = reload;
	rq.sans = sans;
	rq.find_serial = ++find_serial; // cancel any search in progress
	rq.find_from = 0;
	rq.needle[0] = 0;
	rq.needle_serial = ++needle_serial;
	LeaveCriticalSection(&lk);
	find_seen = find_serial;
	if (!reload) {
		cur = 0;
		zoom = 1.0f;
		SetWindowTextW(hedit, L"");
		last_find[0] = 0;
	}
	pending = 0;
	start_seen = 0;
	status[0] = 0;
	if (reload) {
		// keep painting the current pages until the engine reports back
		ui.doc_serial = doc_serial;
		ui.busy = 1;
	} else {
		pan_x = pan_y = 0;
		memset(&ui, 0, sizeof ui);
	}
	push();
	update_title();
	InvalidateRect(hwnd, NULL, FALSE);
}

static void open_dialog(void)
{
	wchar_t f[MAX_PATH] = L"";
	OPENFILENAMEW o = { sizeof o };
	o.hwndOwner = hwnd;
	o.lpstrFilter = L"Books (epub, mobi, pdf, fb2, cbz, txt)\0*.epub;*.mobi;*.azw;*.azw3;*.pdf;*.fb2;*.cbz;*.txt;*.html;*.xhtml\0All files\0*.*\0";
	o.lpstrFile = f;
	o.nMaxFile = MAX_PATH;
	o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
	if (GetOpenFileNameW(&o)) open_path(f, 0);
}

// ------------------------------------------------------------- navigation

static void thumb_reveal(void);

static void goto_page(int p)
{
	if (!counted()) return;
	if (p >= ui.npages) p = ui.npages - 1;
	if (p < 0) p = 0;
	if (multi) p &= ~1;
	if (p == cur) return;
	cur = p;
	pan_x = pan_y = 0;
	snapshot_drop();
	update_title();
	thumb_reveal();
	push();
	InvalidateRect(hwnd, NULL, FALSE);
}

static void nav(int delta)
{
	if (!loaded()) return;
	if (!counted()) { pending += delta; return; } // applied once pages are counted
	goto_page(cur + delta);
}

static void change_size(int dir)
{
	if (!loaded()) return;
	if (ui.reflow) {
		int n = em + dir;
		if (n < EM_MIN || n > EM_MAX) return;
		em = n;
	} else {
		float z = dir > 0 ? zoom * 1.25f : zoom / 1.25f;
		zoom = z < ZOOM_MIN ? ZOOM_MIN : z > ZOOM_MAX ? ZOOM_MAX : z;
		if (zoom <= 1.0f) pan_x = pan_y = 0;
	}
	snapshot_drop();
	push();
	InvalidateRect(hwnd, NULL, FALSE);
}

static void reset_size(void)
{
	if (!loaded()) return;
	zoom = 1.0f;
	em = EM_DEFAULT;
	pan_x = pan_y = 0;
	push();
	InvalidateRect(hwnd, NULL, FALSE);
}

static void toggle_font(void)
{
	if (!counted() || !ui.reflow) return;
	sans = !sans;
	snapshot_drop();
	open_path(cur_path, 1);
}

// ------------------------------------------------------------- thumbnails

static int thumb_item_h(void)
{
	int tw = sidebar_w() - S(24);
	float a = ui.aspect > 0.1f ? ui.aspect : 1.4f;
	return (int)(tw * a) + S(26);
}

static void thumb_view_range(int *first, int *last)
{
	RECT v;
	view_rect(&v);
	int ih = thumb_item_h(), h = v.bottom - v.top;
	*first = thumb_scroll / ih;
	*last = (thumb_scroll + h) / ih;
	if (*first < 0) *first = 0;
	if (*last >= ui.npages) *last = ui.npages - 1;
}

static void thumb_clamp(void)
{
	RECT v;
	view_rect(&v);
	int max = ui.npages * thumb_item_h() - (v.bottom - v.top);
	if (thumb_scroll > max) thumb_scroll = max;
	if (thumb_scroll < 0) thumb_scroll = 0;
}

static void thumb_reveal(void)
{
	if (!previews || !counted()) return;
	RECT v;
	view_rect(&v);
	int ih = thumb_item_h(), h = v.bottom - v.top, top = cur * ih;
	if (top < thumb_scroll || top + ih > thumb_scroll + h) thumb_scroll = top - (h - ih) / 2;
	thumb_clamp();
}

// ------------------------------------------------------------------ search

static void search_needle(char *out, int cap)
{
	wchar_t w[256];
	GetWindowTextW(hedit, w, 256);
	utf8(w, out, cap);
}

// Live highlighting while typing.
static void needle_changed(void)
{
	char n[512];
	search_needle(n, sizeof n);
	EnterCriticalSection(&lk);
	strcpy(rq.needle, n);
	rq.needle_serial = ++needle_serial;
	LeaveCriticalSection(&lk);
	engine_wake();
	InvalidateRect(hwnd, NULL, FALSE);
}

static void find(int dir)
{
	if (!counted()) return;
	char n[512];
	search_needle(n, sizeof n);
	if (!n[0]) return;
	int from = strcmp(n, last_find) ? cur : dir > 0 ? cur + nslots() : cur - 1;
	strcpy(last_find, n);
	EnterCriticalSection(&lk);
	strcpy(rq.needle, n);
	rq.find_serial = ++find_serial;
	rq.find_from = from;
	rq.find_dir = dir;
	LeaveCriticalSection(&lk);
	engine_wake();
	SetTimer(hwnd, TM_STATUS, 150, NULL); // only say "Searching" if it takes a while
}

// ----------------------------------------------------------------- toolbar

static void toolbar_label(int i, wchar_t *out)
{
	lstrcpyW(out, btn_label[i]);
	if (i == B_VIEW) lstrcpyW(out, multi ? L"1 page" : L"2 pages");
	if (i == B_THEME) lstrcpyW(out, dark ? L"Light" : L"Dark");
	if (i == B_FONT) lstrcpyW(out, sans ? L"Serif" : L"Sans");
}

static void layout_toolbar(void)
{
	RECT c;
	GetClientRect(hwnd, &c);
	int h = toolbar_h(), pad = S(6), x = pad, bh = h - 2 * pad;
	HDC dc = GetDC(hwnd);
	HFONT old = SelectObject(dc, font);
	for (int i = 0; i < B_COUNT; i++) {
		SIZE sz;
		const wchar_t *l = i == B_VIEW ? L"2 pages" : i == B_THEME ? L"Light" : i == B_FONT ? L"Serif" : btn_label[i];
		GetTextExtentPoint32W(dc, l, lstrlenW(l), &sz);
		int w = sz.cx + S(20);
		if (i == B_PREV || i == B_NEXT || i == B_ZOUT || i == B_ZIN) w = bh;
		btn_rc[i] = (RECT){ x, pad, x + w, pad + bh };
		x += w + S(4);
		if (i == B_NEXT) {
			info_rc = (RECT){ x, pad, x + S(130), pad + bh };
			x += S(134);
		}
	}
	SelectObject(dc, old);
	ReleaseDC(hwnd, dc);
	int ew = S(160), ex = c.right - ew - pad;
	if (ex < x + S(60)) ex = x + S(60);
	MoveWindow(hedit, ex, pad + S(2), ew, bh - S(4), TRUE);
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
	SetBkColor(dc, c);
	ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}

static void paint_toolbar(HDC dc, int width)
{
	const Theme *t = T();
	fill(dc, &(RECT){ 0, 0, width, toolbar_h() }, t->bar);
	SetBkMode(dc, TRANSPARENT);
	SelectObject(dc, font);
	HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
	for (int i = 0; i < B_COUNT; i++) {
		int on = (i == B_PREVIEWS && previews) || (i == B_VIEW && multi);
		if (i == hot_btn || on) {
			HBRUSH hb = CreateSolidBrush(on ? t->sel : t->hot);
			HBRUSH ob = SelectObject(dc, hb);
			RoundRect(dc, btn_rc[i].left, btn_rc[i].top, btn_rc[i].right, btn_rc[i].bottom, S(6), S(6));
			SelectObject(dc, ob);
			DeleteObject(hb);
		}
		wchar_t l[16];
		toolbar_label(i, l);
		SetTextColor(dc, t->text);
		DrawTextW(dc, l, -1, &btn_rc[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	SelectObject(dc, op);
	wchar_t txt[64] = L"";
	if (counted()) {
		if (nslots() == 2) wsprintfW(txt, L"%d-%d / %d", cur + 1, cur + 2, ui.npages);
		else wsprintfW(txt, L"%d / %d", cur + 1, ui.npages);
	} else if (snap.on)
		wsprintfW(txt, L"%d / %d", snap.h.cur + 1, snap.h.npages);
	SetTextColor(dc, t->dim);
	DrawTextW(dc, txt, -1, &info_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	const wchar_t *st = status[0] ? status : loaded() && ui.busy ? L"Loading…" : L"";
	if (st[0]) {
		RECT er, r = { btn_rc[B_THEME].right + S(10), btn_rc[B_THEME].top, width, btn_rc[B_THEME].bottom };
		GetWindowRect(hedit, &er);
		MapWindowPoints(NULL, hwnd, (POINT *)&er, 2);
		r.right = er.left - S(8);
		DrawTextW(dc, st, -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
	}
}

// ---------------------------------------------------------------- painting

static void blit(HDC dc, const unsigned char *px, int w, int h, int x, int y, int tw, int th, int invert)
{
	BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
	if (tw == w && th == h && !invert)
		SetDIBitsToDevice(dc, x, y, w, h, 0, 0, 0, h, px, &bi, DIB_RGB_COLORS);
	else {
		SetStretchBltMode(dc, COLORONCOLOR);
		StretchDIBits(dc, x, y, tw, th, 0, 0, w, h, px, &bi, DIB_RGB_COLORS, invert ? NOTSRCCOPY : SRCCOPY);
	}
}

static void paint_sidebar(HDC dc, const RECT *v)
{
	const Theme *t = T();
	int sw = sidebar_w();
	fill(dc, &(RECT){ 0, v->top, sw, v->bottom }, t->bar);
	if (!counted()) return;
	int ih = thumb_item_h(), a, z, tw = sw - S(24);
	thumb_view_range(&a, &z);
	SaveDC(dc);
	IntersectClipRect(dc, 0, v->top, sw, v->bottom);
	SetBkMode(dc, TRANSPARENT);
	EnterCriticalSection(&lk);
	for (int i = a; i <= z; i++) {
		int y = v->top + i * ih - thumb_scroll;
		if (i == cur || (nslots() == 2 && i == cur + 1))
			fill(dc, &(RECT){ S(4), y + S(2), sw - S(4), y + ih - S(2) }, t->sel);
		Thumb *th = thumbs && i < nthumbs ? &thumbs[i] : NULL;
		if (th && th->px && th->gen == ui.gen)
			blit(dc, th->px, th->w, th->h, (sw - th->w) / 2, y + S(6), th->w, th->h, th->dark != dark);
		else
			fill(dc, &(RECT){ (sw - tw) / 2, y + S(6), (sw + tw) / 2, y + ih - S(22) }, t->page);
		wchar_t n[16];
		wsprintfW(n, L"%d", i + 1);
		SetTextColor(dc, t->text);
		DrawTextW(dc, n, -1, &(RECT){ 0, y + ih - S(22), sw, y + ih - S(4) }, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
	LeaveCriticalSection(&lk);
	RestoreDC(dc, -1);
}

static void clamp_pan(int w, int h, const RECT *sr)
{
	int ex = (w - (sr->right - sr->left)) / 2, ey = (h - (sr->bottom - sr->top)) / 2;
	if (ex < 0) ex = 0;
	if (ey < 0) ey = 0;
	if (pan_x > ex) pan_x = ex;
	if (pan_x < -ex) pan_x = -ex;
	if (pan_y > ey) pan_y = ey;
	if (pan_y < -ey) pan_y = -ey;
}

static void paint_scrollbar(HDC dc)
{
	const Theme *t = T();
	RECT tr;
	sb_track(&tr);
	fill(dc, &tr, t->bar);
	if (!counted() || ui.npages <= nslots()) return;
	int y, h, pad = S(3);
	sb_thumb(&y, &h);
	RECT th = { tr.left + pad, tr.top + y + pad / 2, tr.right - pad, tr.top + y + h - pad / 2 };
	HBRUSH b = CreateSolidBrush(sb_drag >= 0 ? t->sel : sb_hot ? t->dim : t->hot);
	HBRUSH ob = SelectObject(dc, b);
	HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
	RoundRect(dc, th.left, th.top, th.right + 1, th.bottom + 1, S(8), S(8));
	SelectObject(dc, op);
	SelectObject(dc, ob);
	DeleteObject(b);

	if (sb_drag >= 0 && sb_label >= 0) {
		// page label beside the thumb while dragging
		wchar_t s[32];
		wsprintfW(s, L"%d / %d", sb_label + 1, ui.npages);
		SelectObject(dc, font);
		SIZE sz;
		GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
		int lh = sz.cy + S(8), lw = sz.cx + S(16), ly = tr.top + y + h / 2 - lh / 2;
		if (ly < tr.top) ly = tr.top;
		if (ly + lh > tr.bottom) ly = tr.bottom - lh;
		RECT lr = { tr.left - S(6) - lw, ly, tr.left - S(6), ly + lh };
		fill(dc, &lr, t->bar);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, t->text);
		DrawTextW(dc, s, -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

static void paint_view(HDC dc, const RECT *c)
{
	const Theme *t = T();
	RECT r = *c;
	r.left = sidebar_w();
	r.top = toolbar_h();
	r.right -= scrollbar_w();
	fill(dc, &r, t->view);
	memset(drawn, 0, sizeof drawn);

	int have_exact = 0, ns = nslots();
	if (counted() && snap.on) {
		// keep the snapshot until the real pages are there
		int bw, bh;
		slot_box(&bw, &bh);
		int key = view_key(bw, bh, zoom);
		EnterCriticalSection(&lk);
		have_exact = 1;
		for (int i = 0; i < ns; i++) {
			Img *m = pick(cur + i, key);
			if (!m || m->key != key || m->dark != dark || m->gen != ui.gen) have_exact = 0;
		}
		LeaveCriticalSection(&lk);
		if (have_exact) snapshot_drop();
	}
	if (snap.on) {
		for (int i = 0; i < snap.h.n; i++)
			blit(dc, snap.px[i], snap.im[i].w, snap.im[i].h, snap.im[i].x, snap.im[i].y, snap.im[i].w, snap.im[i].h, 0);
		return;
	}

	if (!loaded()) {
		const wchar_t *m = ui.doc_serial == doc_serial && ui.state == DOC_FAILED ? L"Could not open this file" :
			cur_path[0] ? L"" : L"Press Ctrl+O to open a book, or drop a file here";
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, t->dim);
		SelectObject(dc, font);
		DrawTextW(dc, m, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		return;
	}

	int bw, bh;
	slot_box(&bw, &bh);
	int key = view_key(bw, bh, zoom);
	EnterCriticalSection(&lk);
	for (int i = 0; i < ns; i++) {
		int page = counted() ? cur + i : -1;
		Img *m = pick(page, key);
		if (!m && !counted()) m = pick(cur + i, key); // relayout in progress: old layout
		if (!m || !m->px) continue;
		RECT sr;
		slot_rect(i, &sr);
		float pw = m->bounds.x1 - m->bounds.x0, ph = m->bounds.y1 - m->bounds.y0;
		float ts = fminf(bw / pw, bh / ph) * (ui.reflow ? 1.0f : zoom);
		int exact = m->key == key && m->gen == ui.gen;
		int tw = exact ? m->w : (int)(pw * ts + 0.5f), th = exact ? m->h : (int)(ph * ts + 0.5f);
		clamp_pan(tw, th, &sr);
		int x = sr.left + (sr.right - sr.left - tw) / 2 + (tw > sr.right - sr.left ? pan_x : 0);
		int y = sr.top + (sr.bottom - sr.top - th) / 2 + (th > sr.bottom - sr.top ? pan_y : 0);
		SaveDC(dc);
		IntersectClipRect(dc, sr.left, sr.top, sr.right, sr.bottom);
		blit(dc, m->px, m->w, m->h, x, y, tw, th, m->dark != dark);
		float sx = tw / pw;
		if (m->hl_serial == needle_serial)
			for (int k = 0; k < m->nhl; k++) {
				Box b = m->hl[k];
				RECT hr = { x + (int)((b.x0 - m->bounds.x0) * sx), y + (int)((b.y0 - m->bounds.y0) * sx),
					x + (int)ceilf((b.x1 - m->bounds.x0) * sx), y + (int)ceilf((b.y1 - m->bounds.y0) * sx) };
				InvertRect(dc, &hr);
			}
		RestoreDC(dc, -1);
		drawn[i] = (Drawn){ 1, m->page, x, y, tw, th, sx, m->bounds };
	}
	LeaveCriticalSection(&lk);
}

static void on_paint(void)
{
	PAINTSTRUCT ps;
	HDC wdc = BeginPaint(hwnd, &ps);
	RECT c, pr = ps.rcPaint;
	GetClientRect(hwnd, &c);
	HDC dc = CreateCompatibleDC(wdc);
	HBITMAP bm = CreateCompatibleBitmap(wdc, c.right, c.bottom);
	HBITMAP ob = SelectObject(dc, bm);
	IntersectClipRect(dc, pr.left, pr.top, pr.right, pr.bottom);
	if (pr.bottom > toolbar_h()) {
		paint_view(dc, &c);
		if (sidebar_w()) paint_sidebar(dc, &(RECT){ 0, toolbar_h(), c.right, c.bottom });
		if (scrollbar_w()) paint_scrollbar(dc);
	}
	paint_toolbar(dc, c.right);
	BitBlt(wdc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, dc, pr.left, pr.top, SRCCOPY);
	SelectObject(dc, ob);
	DeleteObject(bm);
	DeleteDC(dc);
	EndPaint(hwnd, &ps);
}

// ------------------------------------------------------------- engine news

static void on_engine(void)
{
	InterlockedExchange(&engine_notify_pending, 0);
	Info n;
	EnterCriticalSection(&lk);
	n = info;
	LeaveCriticalSection(&lk);
	if (n.doc_serial != doc_serial) return; // still working on an older request
	int was_failed = ui.state == DOC_FAILED;
	ui = n;

	if (ui.state == DOC_FAILED && !was_failed) {
		snapshot_drop();
		wchar_t msg[MAX_PATH + 64];
		wsprintfW(msg, L"Could not open:\n%s", cur_path);
		cur_path[0] = 0;
		update_title();
		InvalidateRect(hwnd, NULL, FALSE);
		MessageBoxW(hwnd, msg, APPNAME, MB_OK | MB_ICONWARNING);
		return;
	}
	if (ui.counted && ui.start_serial != start_seen) {
		start_seen = ui.start_serial;
		cur = ui.start_page + pending;
		anchor = pending ? cur : ui.start_page;
		pending = 0;
		if (cur >= ui.npages) cur = ui.npages - 1;
		if (cur < 0) cur = 0;
		if (multi) cur &= ~1;
		pan_x = pan_y = 0;
		thumb_reveal();
		push();
	}
	if (ui.find_done == find_serial && find_seen != find_serial) {
		find_seen = find_serial;
		KillTimer(hwnd, TM_STATUS);
		if (ui.find_page >= 0) {
			set_status(L"");
			goto_page(ui.find_page);
		} else
			set_status(L"No matches");
	}
	update_title();
	InvalidateRect(hwnd, NULL, FALSE);
}

// ------------------------------------------------------------------- input

static void apply_theme(void)
{
	BOOL d = dark;
	DwmSetWindowAttribute(hwnd, 20, &d, sizeof d); // DWMWA_USE_IMMERSIVE_DARK_MODE
	if (br_edit) DeleteObject(br_edit);
	br_edit = CreateSolidBrush(T()->hot);
	snapshot_drop();
	push();
	InvalidateRect(hwnd, NULL, FALSE);
	InvalidateRect(hedit, NULL, TRUE);
}

static void toggle_multi(void)
{
	multi = !multi;
	if (multi) cur &= ~1;
	pan_x = pan_y = 0;
	snapshot_drop();
	update_title();
	thumb_reveal();
	push();
	InvalidateRect(hwnd, NULL, FALSE);
}

static void toggle_previews(void)
{
	previews = !previews;
	snapshot_drop();
	thumb_reveal();
	push();
	InvalidateRect(hwnd, NULL, FALSE);
}

static void do_button(int i)
{
	switch (i) {
	case B_OPEN: open_dialog(); break;
	case B_PREV: nav(-step()); break;
	case B_NEXT: nav(step()); break;
	case B_PREVIEWS: toggle_previews(); break;
	case B_VIEW: toggle_multi(); break;
	case B_ZOUT: change_size(-1); break;
	case B_ZIN: change_size(+1); break;
	case B_FONT: toggle_font(); break;
	case B_THEME: dark = !dark; apply_theme(); break;
	}
}

static int hit_button(int x, int y)
{
	POINT p = { x, y };
	for (int i = 0; i < B_COUNT; i++)
		if (PtInRect(&btn_rc[i], p)) return i;
	return -1;
}

static void follow_link(int x, int y)
{
	for (int i = 0; i < 2; i++) {
		Drawn *d = &drawn[i];
		if (!d->valid || x < d->x || y < d->y || x >= d->x + d->w || y >= d->y + d->h) continue;
		float px = (x - d->x) / d->sx + d->b.x0, py = (y - d->y) / d->sx + d->b.y0;
		int target = -1;
		wchar_t uri[1024] = L"";
		EnterCriticalSection(&lk);
		for (int k = 0; k < NIMG; k++) {
			Img *m = &img[k];
			if (!m->used || m->gen != ui.gen || m->page != d->page) continue;
			for (int j = 0; j < m->nlinks; j++) {
				Link *l = &m->links[j];
				if (px < l->r.x0 || px >= l->r.x1 || py < l->r.y0 || py >= l->r.y1) continue;
				if (l->uri) MultiByteToWideChar(CP_UTF8, 0, l->uri, -1, uri, 1024);
				else target = l->page;
				break;
			}
			break;
		}
		LeaveCriticalSection(&lk);
		if (uri[0]) ShellExecuteW(hwnd, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
		else if (target >= 0) goto_page(target);
		return;
	}
}

static void on_key(WPARAM k)
{
	int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
	if (ctrl) {
		switch (k) {
		case 'O': open_dialog(); return;
		case 'F': SetFocus(hedit); SendMessageW(hedit, EM_SETSEL, 0, -1); return;
		case VK_OEM_PLUS: case VK_ADD: change_size(+1); return;
		case VK_OEM_MINUS: case VK_SUBTRACT: change_size(-1); return;
		case '0': reset_size(); return;
		}
		return;
	}
	switch (k) {
	case VK_RIGHT: case VK_NEXT: case VK_DOWN: nav(step()); break;
	case VK_LEFT: case VK_PRIOR: case VK_UP: case VK_BACK: nav(-step()); break;
	case VK_SPACE: nav(shift ? -step() : step()); break;
	case VK_HOME: goto_page(0); break;
	case VK_END: goto_page(ui.npages - 1); break;
	case VK_F3: find(shift ? -1 : 1); break;
	case 'P': toggle_previews(); break;
	case 'V': toggle_multi(); break;
	case 'S': toggle_font(); break;
	case 'D': dark = !dark; apply_theme(); break;
	case VK_ESCAPE:
		SetWindowTextW(hedit, L"");
		last_find[0] = 0;
		set_status(L"");
		break;
	}
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref)
{
	if (m == WM_KEYDOWN) {
		if (w == VK_RETURN || w == VK_F3) { find(GetKeyState(VK_SHIFT) < 0 ? -1 : 1); return 0; }
		if (w == VK_ESCAPE) { SetFocus(hwnd); return 0; }
	}
	if (m == WM_CHAR && (w == VK_RETURN || w == VK_ESCAPE)) return 0; // no beep
	return DefSubclassProc(h, m, w, l);
}

static void on_wheel(int delta, int ctrl, int x, int y)
{
	if (!loaded()) return;
	if (ctrl) { change_size(delta > 0 ? 1 : -1); return; }
	if (x < sidebar_w()) {
		thumb_scroll -= delta * thumb_item_h() / 240;
		thumb_clamp();
		push();
		InvalidateRect(hwnd, NULL, FALSE);
		return;
	}
	if (!ui.reflow && zoom > 1.0f) {
		pan_y += delta / 2;
		InvalidateRect(hwnd, NULL, FALSE);
		return;
	}
	nav(delta > 0 ? -step() : step());
}

static HFONT make_font(void)
{
	return CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static int c_right(HWND h)
{
	RECT c;
	GetClientRect(h, &c);
	return c.right;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
	static int dragging, drag_x, drag_y, moved;
	switch (m) {
	case WM_CREATE:
		hwnd = h;
		dpi = GetDpiForWindow(h);
		font = make_font();
		hedit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_BORDER,
			0, 0, 10, 10, h, (HMENU)ID_EDIT, NULL, NULL);
		SendMessageW(hedit, WM_SETFONT, (WPARAM)font, TRUE);
		SendMessageW(hedit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
		SetWindowSubclass(hedit, edit_proc, 1, 0);
		DragAcceptFiles(h, TRUE);
		layout_toolbar();
		BOOL d = dark;
		DwmSetWindowAttribute(h, 20, &d, sizeof d);
		br_edit = CreateSolidBrush(T()->hot);
		return 0;
	case WM_SIZE:
		if (hedit) {
			layout_toolbar();
			thumb_clamp();
			if (snap.on) {
				RECT c;
				GetClientRect(h, &c);
				if (c.right != snap.h.cw || c.bottom != snap.h.ch) snapshot_drop();
			}
			push();
			InvalidateRect(h, NULL, FALSE);
		}
		return 0;
	case WM_DPICHANGED: {
		dpi = HIWORD(w);
		DeleteObject(font);
		font = make_font();
		SendMessageW(hedit, WM_SETFONT, (WPARAM)font, TRUE);
		RECT *r = (RECT *)l;
		SetWindowPos(h, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
		snapshot_drop();
		layout_toolbar();
		push();
		return 0;
	}
	case WM_GETMINMAXINFO:
		((MINMAXINFO *)l)->ptMinTrackSize = (POINT){ S(860), S(360) };
		return 0;
	case WM_ERASEBKGND: return 1;
	case WM_PAINT: on_paint(); return 0;
	case WM_ENGINE: on_engine(); return 0;
	case WM_CTLCOLOREDIT:
		SetTextColor((HDC)w, T()->text);
		SetBkColor((HDC)w, T()->hot);
		return (LRESULT)br_edit;
	case WM_COMMAND:
		if (LOWORD(w) == ID_EDIT && HIWORD(w) == EN_CHANGE) needle_changed();
		return 0;
	case WM_TIMER:
		if (w == TM_STATUS) {
			KillTimer(h, TM_STATUS);
			if (find_seen != find_serial) set_status(L"Searching…");
		}
		return 0;
	case WM_KEYDOWN: on_key(w); return 0;
	case WM_MOUSEWHEEL: {
		POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
		ScreenToClient(h, &p);
		on_wheel(GET_WHEEL_DELTA_WPARAM(w), GET_KEYSTATE_WPARAM(w) & MK_CONTROL, p.x, p.y);
		return 0;
	}
	case WM_MOUSEMOVE: {
		int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
		if (sb_drag >= 0) {
			RECT tr;
			sb_track(&tr);
			int p = sb_page_at(y - tr.top - sb_drag);
			if (p != sb_label) {
				sb_label = p;
				goto_page(p);
				InvalidateRect(h, NULL, FALSE);
			}
		} else if (dragging) {
			if (abs(x - drag_x) + abs(y - drag_y) > 3) moved = 1;
			pan_x += x - drag_x;
			pan_y += y - drag_y;
			drag_x = x;
			drag_y = y;
			InvalidateRect(h, NULL, FALSE);
		} else {
			int hb = y < toolbar_h() ? hit_button(x, y) : -1;
			RECT tr;
			sb_track(&tr);
			int hs = x >= tr.left && y >= tr.top;
			if (hb != hot_btn || hs != sb_hot) {
				if (hs != sb_hot) InvalidateRect(h, &tr, FALSE);
				hot_btn = hb;
				sb_hot = hs;
				InvalidateRect(h, &(RECT){ 0, 0, 10000, toolbar_h() }, FALSE);
				TRACKMOUSEEVENT te = { sizeof te, TME_LEAVE, h, 0 };
				TrackMouseEvent(&te);
			}
		}
		return 0;
	}
	case WM_MOUSELEAVE:
		hot_btn = -1;
		sb_hot = 0;
		InvalidateRect(h, NULL, FALSE);
		return 0;
	case WM_LBUTTONDOWN: {
		int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
		SetFocus(h);
		if (y < toolbar_h()) {
			int b = hit_button(x, y);
			if (b >= 0) do_button(b);
		} else if (x < sidebar_w()) {
			if (counted()) goto_page((y - toolbar_h() + thumb_scroll) / thumb_item_h());
		} else if (scrollbar_w() && x >= c_right(h) - scrollbar_w()) {
			if (counted() && ui.npages > nslots()) {
				// grab the thumb where it was clicked; clicking the track centres it there
				RECT tr;
				int ty, th;
				sb_track(&tr);
				sb_thumb(&ty, &th);
				int rel = y - tr.top;
				sb_drag = rel >= ty && rel < ty + th ? rel - ty : th / 2;
				sb_label = cur;
				int p = sb_page_at(rel - sb_drag);
				if (p != cur) { sb_label = p; goto_page(p); }
				SetCapture(h);
				InvalidateRect(h, NULL, FALSE);
			}
		} else if (loaded()) {
			dragging = 1;
			moved = 0;
			drag_x = x;
			drag_y = y;
			SetCapture(h);
		}
		return 0;
	}
	case WM_LBUTTONUP:
		if (sb_drag >= 0) {
			sb_drag = -1;
			sb_label = -1;
			ReleaseCapture();
			InvalidateRect(h, NULL, FALSE);
		}
		if (dragging) {
			dragging = 0;
			ReleaseCapture();
			if (!moved) follow_link(GET_X_LPARAM(l), GET_Y_LPARAM(l));
		}
		return 0;
	case WM_DROPFILES: {
		wchar_t f[MAX_PATH];
		if (DragQueryFileW((HDROP)w, 0, f, MAX_PATH)) open_path(f, 0);
		DragFinish((HDROP)w);
		return 0;
	}
	case WM_CLOSE:
		save_settings();
		snapshot_save();
		DestroyWindow(h);
		return 0;
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}

// -------------------------------------------------------------------- main

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
	// Don't let Windows treat us as a background process: keep the engine
	// off EcoQoS / efficiency-core throttling.
	PROCESS_POWER_THROTTLING_STATE pt = { PROCESS_POWER_THROTTLING_CURRENT_VERSION, PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0 };
	SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);

	GetModuleFileNameW(NULL, ini_path, MAX_PATH);
	wchar_t *dot = wcsrchr(ini_path, L'.');
	if (dot) lstrcpyW(dot, L".ini");
	PWSTR la = NULL;
	if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &la)) && wcslen(la) < MAX_PATH - 32) {
		swprintf(cache_dir, MAX_PATH, L"%ls\\dpub", la);
		CreateDirectoryW(cache_dir, NULL);
	} else
		GetTempPathW(MAX_PATH, cache_dir);
	CoTaskMemFree(la);
	swprintf(snap_path, MAX_PATH, L"%ls\\snapshot.bin", cache_dir);

	dark = ini_get(L"dark", -1);
	if (dark < 0) dark = system_dark();
	previews = ini_get(L"previews", 1);
	multi = ini_get(L"multi", 0);
	em = ini_get(L"em", EM_DEFAULT);
	if (em < EM_MIN || em > EM_MAX) em = EM_DEFAULT;
	sans = ini_get(L"sans", 0) != 0;

	InitializeCriticalSection(&lk);
	rq.em = em;
	rq.sans = sans;
	rq.dark = dark;
	rq.zoom = 1.0f;

	INITCOMMONCONTROLSEX ic = { sizeof ic, ICC_STANDARD_CLASSES };
	InitCommonControlsEx(&ic);

	WNDCLASSEXW wc = { sizeof wc };
	wc.lpfnWndProc = wndproc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hIcon = LoadIcon(inst, MAKEINTRESOURCE(1));
	wc.hIconSm = wc.hIcon;
	wc.lpszClassName = L"dpubWnd";
	RegisterClassExW(&wc);
	CreateWindowExW(0, wc.lpszClassName, APPNAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
		CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, NULL, NULL, inst, NULL);
	if (!hwnd) return 1;

	// window placement from last time (needed for the snapshot to match)
	RECT wa;
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
	int ww = ini_get(L"w", 0), wh = ini_get(L"h", 0);
	WINDOWPLACEMENT wp = { sizeof wp };
	GetWindowPlacement(hwnd, &wp);
	if (ww > 200 && wh > 200) {
		wp.rcNormalPosition = (RECT){ ini_get(L"x", 0), ini_get(L"y", 0), 0, 0 };
		wp.rcNormalPosition.right = wp.rcNormalPosition.left + ww;
		wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + wh;
	} else {
		int w = min(S(1100), wa.right - wa.left), hh = min(S(760), wa.bottom - wa.top);
		wp.rcNormalPosition = (RECT){ wa.left + (wa.right - wa.left - w) / 2, wa.top + (wa.bottom - wa.top - hh) / 2, 0, 0 };
		wp.rcNormalPosition.right = wp.rcNormalPosition.left + w;
		wp.rcNormalPosition.bottom = wp.rcNormalPosition.top + hh;
	}
	wp.showCmd = ini_get(L"max", 0) ? SW_SHOWMAXIMIZED : SW_HIDE;
	SetWindowPlacement(hwnd, &wp);
	dpi = GetDpiForWindow(hwnd);

	engine_start(hwnd);

	int argc;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	wchar_t first[MAX_PATH] = L"";
	if (argc > 1) {
		GetFullPathNameW(argv[1], MAX_PATH, first, NULL);
	} else {
		GetPrivateProfileStringW(L"settings", L"last", L"", first, MAX_PATH, ini_path);
		if (first[0] && GetFileAttributesW(first) == INVALID_FILE_ATTRIBUTES) first[0] = 0;
	}
	LocalFree(argv);

	ShowWindow(hwnd, wp.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : show);
	layout_toolbar();
	if (first[0]) {
		open_path(first, 0);
		snapshot_load(first); // last session's screen, shown until the real one is ready
		update_title();
	}
	UpdateWindow(hwnd);

	MSG msg;
	while (GetMessageW(&msg, NULL, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}
	ExitProcess(0); // the engine threads hold nothing that needs saving
}
