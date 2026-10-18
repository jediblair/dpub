// State shared between the UI thread (main.c) and the engine (engine.c).
//
// The UI never calls MuPDF. It describes what it wants in `rq` and paints
// whatever bitmaps the engine has published in `img` / `thumbs`. The engine
// thread owns the document and works through the request in priority order:
// open/relayout, visible pages, search highlights, find, prefetch, thumbnails.
// Everything here is protected by `lk` unless noted.
#pragma once
#define UNICODE
#define _UNICODE
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdint.h>

#define PAGE_W 480.0f // logical page size used to lay out reflowable books
#define PAGE_H 680.0f
#define EM_DEFAULT 12
#define WM_ENGINE (WM_APP + 1) // engine -> UI: published state changed

typedef struct { float x0, y0, x1, y1; } Box;

typedef struct {
	Box r;
	int page;   // internal target, or -1
	char *uri;  // external target (malloc'd), or NULL
} Link;

// A rendered page. Pixels are plain BGRA rows owned by the engine.
typedef struct {
	int used;
	unsigned gen;     // layout generation the page belongs to
	int page;         // global page number; -1 for the preview (see Info.counted)
	int key;          // view key (slot size, zoom) it was rendered for
	int dark;
	int w, h;
	unsigned char *px;
	float scale;      // pixels per point
	Box bounds;       // page bounds in points
	int chapter, chapter_page, chapter_pages; // location, for saving the position
	int nlinks;
	Link *links;
	unsigned hl_serial; // needle serial the highlights belong to
	int nhl;
	Box *hl;          // search hit boxes in points
	unsigned stamp;   // LRU
} Img;

typedef struct {
	unsigned char *px; // NULL if rendering failed
	int w, h, dark;
	int tw;           // requested width
	unsigned gen;
	unsigned stamp;
} Thumb;

typedef struct {
	// open a document (open_serial bumps); position to resume at
	unsigned open_serial;
	wchar_t open_path[MAX_PATH];
	int open_chapter, open_page, open_pages, open_em;
	int reload; // same document, keep the current position (font family change)

	// layout of reflowable documents
	int em, sans;

	// view
	int cur, nslots;     // visible pages
	int anchor;          // reading position to keep across relayouts (cur may be rounded to a spread)
	unsigned view_gen;   // Info.gen that cur/anchor refer to
	int slot_w, slot_h;  // space for one page, device pixels
	float zoom;          // fixed-layout documents only
	int dark;
	int thumb_a, thumb_b, thumb_w; // visible thumbnails, thumbnail width

	// search: live highlight needle, and find requests
	unsigned needle_serial;
	char needle[512];
	unsigned find_serial;
	int find_from, find_dir;
} Req;

enum { DOC_NONE, DOC_LOADING, DOC_READY, DOC_FAILED };

typedef struct {
	unsigned doc_serial; // open_serial this state belongs to
	int state;
	int busy;            // opening / laying out
	int counted;         // npages is known (before that only the preview page exists)
	int npages;
	int reflow;
	unsigned gen;        // bumps whenever page numbering may change
	unsigned start_serial; // bumps when the UI should jump to start_page
	int start_page;
	float aspect;        // page height / width (page 0), for the sidebar
	unsigned find_done;  // find_serial answered
	int find_page;       // result, or -1
	int indexed;         // pages in the search index (progress)
} Info;

#define NIMG 12

extern CRITICAL_SECTION lk;
extern volatile LONG engine_notify_pending; // UI clears it when handling WM_ENGINE
extern wchar_t cache_dir[MAX_PATH];         // %LOCALAPPDATA%\dpub, set by the UI
extern Req rq;
extern Info info;
extern Img img[NIMG];
extern Thumb *thumbs;
extern int nthumbs;

void engine_start(HWND notify);
void engine_wake(void);
int view_key(int slot_w, int slot_h, float zoom);
