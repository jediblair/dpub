// Timing harness for the MuPDF paths the reader uses.
// usage: bench.exe book [book...]
//   BENCH_RUNS=n     repeat each book n times and report the minimum of each figure
//   BENCH_SKIP=search  skip the search-every-page pass
//   BENCH_PROF=file  sample the whole run (see sampler.c, prof.py)
#include <mupdf/fitz.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#ifdef USE_MIMALLOC
#include <mimalloc.h>
static void *mi_m(void *u, size_t n) { return mi_malloc(n); }
static void *mi_r(void *u, void *p, size_t n) { return mi_realloc(p, n); }
static void mi_f(void *u, void *p) { mi_free(p); }
static fz_alloc_context mi_alloc = { NULL, mi_m, mi_r, mi_f };
#define ALLOC &mi_alloc
#else
#define ALLOC NULL
#endif

void sampler_start(const char *path);
void sampler_stop(void);

static double now(void)
{
	LARGE_INTEGER f, c;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&c);
	return c.QuadPart * 1000.0 / f.QuadPart;
}

static double render(fz_context *ctx, fz_document *doc, int pn, float scale)
{
	double t = now();
	fz_page *page = fz_load_page(ctx, doc, pn);
	fz_rect b = fz_bound_page(ctx, page);
	fz_matrix m = fz_scale(scale, scale);
	fz_pixmap *pix = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), fz_round_rect(fz_transform_rect(b, m)), NULL, 1);
	fz_clear_pixmap_with_value(ctx, pix, 255);
	fz_device *dev = fz_new_draw_device(ctx, fz_identity, pix);
	fz_run_page(ctx, page, dev, m, NULL);
	fz_close_device(ctx, dev);
	fz_drop_device(ctx, dev);
	fz_drop_pixmap(ctx, pix);
	fz_drop_page(ctx, page);
	return now() - t;
}

enum { OPEN, LAYOUT, FIRST, PAGE, THUMB, SEARCH, RELAYOUT, TOTAL, NFIG };

static void run_book(fz_context *ctx, const char *path, int skip_search, double *fig, int *np)
{
	double t = now(), t0 = t;
	fz_document *doc = fz_open_document(ctx, path);
	fig[OPEN] = now() - t;
	t = now();
	int reflow = fz_is_document_reflowable(ctx, doc);
	if (reflow)
		fz_layout_document(ctx, doc, 480, 680, 12);
	int n = fz_count_pages(ctx, doc);
	*np = n;
	fig[LAYOUT] = now() - t;
	fig[FIRST] = render(ctx, doc, 0, 2.2f);
	double tmid = 0, tthumb = 0;
	int k = n < 20 ? n : 20;
	for (int i = 0; i < k; i++) tmid += render(ctx, doc, n / 2 + i < n ? n / 2 + i : i, 2.2f);
	for (int i = 0; i < k; i++) tthumb += render(ctx, doc, i, 0.4f);
	fig[PAGE] = tmid / k;
	fig[THUMB] = tthumb / k;
	t = now();
	for (int i = 0; i < n && !skip_search; i++) {
		fz_quad q[8];
		fz_page *p = fz_load_page(ctx, doc, i);
		fz_search_page(ctx, p, "zzqx", NULL, q, 8);
		fz_drop_page(ctx, p);
	}
	fig[SEARCH] = now() - t;
	t = now();
	if (reflow) {
		fz_layout_document(ctx, doc, 480, 680, 14);
		fz_count_pages(ctx, doc);
	}
	fig[RELAYOUT] = now() - t;
	fig[TOTAL] = now() - t0;
	fz_drop_document(ctx, doc);
}

int main(int argc, char **argv)
{
	const char *prof = getenv("BENCH_PROF"), *skip = getenv("BENCH_SKIP");
	int runs = getenv("BENCH_RUNS") ? atoi(getenv("BENCH_RUNS")) : 1;
	int skip_search = skip && strstr(skip, "search");
	double sum = 0;

	// pin to one P-core so hybrid-CPU scheduling doesn't add noise
	SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << 2);
	if (prof) sampler_start(prof);
	fz_context *ctx = fz_new_context(ALLOC, NULL, 256 << 20);
	fz_register_document_handlers(ctx);
	for (int a = 1; a < argc; a++) {
		double best[NFIG], fig[NFIG];
		int n = 0;
		for (int i = 0; i < NFIG; i++) best[i] = 1e30;
		for (int r = 0; r < runs; r++) {
			run_book(ctx, argv[a], skip_search, fig, &n);
			for (int i = 0; i < NFIG; i++) if (fig[i] < best[i]) best[i] = fig[i];
		}
		const char *name = strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a];
		printf("%-14s pages %5d | open %6.1f layout %6.1f first %5.1f page %5.1f thumb %5.1f search-all %7.1f relayout %6.1f | total %7.1f ms\n",
			name, n, best[OPEN], best[LAYOUT], best[FIRST], best[PAGE], best[THUMB], best[SEARCH], best[RELAYOUT], best[TOTAL]);
		sum += best[TOTAL];
	}
	printf("SUM %.1f ms\n", sum);
	fz_drop_context(ctx);
	if (prof) sampler_stop();
	return 0;
}
