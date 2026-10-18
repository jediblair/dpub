// Prints page counts, per-page pixel digests and search hits so optimized
// builds can be checked against the reference output.
// usage: verify.exe book [book...]
#include <mupdf/fitz.h>
#include <stdio.h>
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

static void digest_page(fz_context *ctx, fz_document *doc, int pn, unsigned char out[16])
{
	fz_page *page = fz_load_page(ctx, doc, pn);
	fz_matrix m = fz_scale(1.5f, 1.5f);
	fz_pixmap *pix = fz_new_pixmap_from_page(ctx, page, m, fz_device_bgr(ctx), 0);
	fz_md5_pixmap(ctx, pix, out);
	fz_drop_pixmap(ctx, pix);
	fz_drop_page(ctx, page);
}

int main(int argc, char **argv)
{
	fz_context *ctx = fz_new_context(ALLOC, NULL, 256 << 20);
	fz_register_document_handlers(ctx);
	for (int a = 1; a < argc; a++) {
		static const float ems[] = { 12, 17 };
		for (int e = 0; e < 2; e++) {
			fz_document *doc = fz_open_document(ctx, argv[a]);
			int reflow = fz_is_document_reflowable(ctx, doc);
			if (reflow)
				fz_layout_document(ctx, doc, 480, 680, ems[e]);
			int n = fz_count_pages(ctx, doc);
			fz_md5 md5;
			fz_md5_init(&md5);
			int step = n > 60 ? n / 60 : 1;
			for (int i = 0; i < n; i += step) {
				unsigned char d[16];
				digest_page(ctx, doc, i, d);
				fz_md5_update(&md5, d, 16);
			}
			int hits = 0;
			for (int i = 0; i < n; i += step) {
				fz_quad q[64];
				fz_page *p = fz_load_page(ctx, doc, i);
				hits += fz_search_page(ctx, p, "the", NULL, q, 64);
				fz_drop_page(ctx, p);
			}
			unsigned char d[16];
			fz_md5_final(&md5, d);
			printf("%s em=%g pages=%d hits=%d md5=", strrchr(argv[a], '/') ? strrchr(argv[a], '/') + 1 : argv[a], ems[e], n, hits);
			for (int i = 0; i < 16; i++) printf("%02x", d[i]);
			printf("\n");
			fz_drop_document(ctx, doc);
			if (!reflow) break;
		}
	}
	fz_drop_context(ctx);
	return 0;
}
