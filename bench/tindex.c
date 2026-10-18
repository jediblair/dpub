#include <mupdf/fitz.h>
#include <mupdf/ucdn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *project(fz_context *ctx, const char *text)
{
	char *folded = fz_search_fold_text(ctx, text, FZ_SEARCH_IGNORE_CASE|FZ_SEARCH_IGNORE_DIACRITICS);
	char *out = malloc(strlen(folded) + 1), *o = out;
	for (const char *s = folded; *s;) {
		int c; const char *start = s;
		s += fz_chartorune(&c, s);
		switch (ucdn_get_general_category(c)) {
		case UCDN_GENERAL_CATEGORY_LU: case UCDN_GENERAL_CATEGORY_LL: case UCDN_GENERAL_CATEGORY_LT:
		case UCDN_GENERAL_CATEGORY_LM: case UCDN_GENERAL_CATEGORY_LO:
		case UCDN_GENERAL_CATEGORY_MN: case UCDN_GENERAL_CATEGORY_MC: case UCDN_GENERAL_CATEGORY_ME:
		case UCDN_GENERAL_CATEGORY_ND: case UCDN_GENERAL_CATEGORY_NL: case UCDN_GENERAL_CATEGORY_NO:
			memcpy(o, start, s - start); o += s - start; break;
		}
	}
	*o = 0; fz_free(ctx, folded); return out;
}
static int cnt_cb(fz_context *c, void *o, int n, fz_quad *q, int ch, int pg) { return 0; }
int main(int argc, char **argv)
{
	fz_context *ctx = fz_new_context(NULL, NULL, 256 << 20);
	fz_register_document_handlers(ctx);
	fz_document *doc = fz_open_document(ctx, argv[1]);
	fz_layout_document(ctx, doc, 480, 680, 12);
	int n = fz_count_pages(ctx, doc), idx = 0, real = 0, miss = 0;
	char *np = project(ctx, argv[2]);
	printf("needle proj '%s'\n", np);
	fz_stext_options opts = { FZ_STEXT_DEHYPHENATE };
	for (int p = 0; p < n; p++) {
		fz_page *page = fz_load_page(ctx, doc, p);
		fz_stext_page *st = fz_new_stext_page_from_page(ctx, page, &opts);
		fz_buffer *buf = fz_new_buffer_from_flattened_stext_page(ctx, st, FZ_TEXT_FLATTEN_ALL, NULL);
		char *pr = project(ctx, fz_string_from_buffer(ctx, buf));
		if (p == 10) printf("page10 proj: %.120s\n", pr);
		int c = strstr(pr, np) != NULL, h = 0;
		{ fz_stext_page *s2 = fz_new_stext_page_from_page(ctx, page, &opts); h = fz_match_stext_page_cb(ctx, s2, argv[2], cnt_cb, NULL, FZ_SEARCH_IGNORE_CASE|FZ_SEARCH_IGNORE_DIACRITICS) > 0; fz_drop_stext_page(ctx, s2); }
		idx += c; real += h; miss += h && !c;
		free(pr); fz_drop_buffer(ctx, buf); fz_drop_stext_page(ctx, st); fz_drop_page(ctx, page);
	}
	printf("pages %d candidates %d real %d missed %d\n", n, idx, real, miss);
	return 0;
}
