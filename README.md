# dpub

**dpub** (speeD ePUB) is a small, fast ebook reader for Windows. It is one portable `dpub.exe` (about 8.5 MB) with no installer and no DLLs beyond the ones that ship with Windows. Open the program, open a book, read.

No library, no accounts, no syncing, no store. It is written in C on the Win32 API, with a statically linked and patched copy of [MuPDF](https://mupdf.com) doing the document work.

## Features

- **Formats:** EPUB, MOBI / AZW / AZW3, PDF, FB2, CBZ, TXT and HTML.
- **Single or two-page spread view.**
- **Font size and zoom:** reflowable books (EPUB, MOBI, …) re-paginate at the new size and keep your place. PDFs zoom, and you drag to pan.
- **Serif / sans-serif** text for reflowable books.
- **Dark and light themes:** the first run follows the Windows setting.
- **Page previews:** a toggleable thumbnail sidebar; click a thumbnail to jump there.
- **Whole-book scrollbar:** drag through the book with a live page counter, or click to jump.
- **Search:** case- and accent-insensitive, so `natasha` finds "Natásha". Matches are highlighted as you type, and Enter / F3 jumps to the next one.
- **Links:** internal links jump to their target; external links open in your browser.
- **Remembers things:** the reading position in every book, your settings and the window position. On launch it reopens the last book, showing the page you left on almost instantly.

## Using it

Run `dpub.exe`. To open a book you can:

- pass the file on the command line,
- drag it onto the window, or
- press Ctrl+O.

| Key | Action |
|---|---|
| Ctrl+O | Open a book |
| → ↓ PgDn Space | Next page / spread |
| ← ↑ PgUp Backspace Shift+Space | Previous page / spread |
| Home / End | First / last page |
| Ctrl+= / Ctrl+− / Ctrl+0 | Font size (reflowable) or zoom (PDF); reset |
| Ctrl+mouse wheel | Same as above |
| V | Toggle one / two pages |
| P | Toggle page previews |
| S | Toggle serif / sans-serif |
| D | Toggle dark / light |
| Ctrl+F | Focus the search box |
| Enter / F3 | Next match |
| Shift+Enter / Shift+F3 | Previous match |
| Esc | Clear the search |

You can also use the mouse:

- The wheel turns pages. When a PDF is zoomed in, the wheel scrolls and dragging the page pans it.
- Click a link to follow it.
- Click a thumbnail, or click or drag the scrollbar, to jump.

### Files it creates

| Where | What |
|---|---|
| `dpub.ini` next to the exe | Settings, window position, and the last position in each book |
| `%LOCALAPPDATA%\dpub\*.accel` | Cached EPUB page counts, one small file per book and layout |
| `%LOCALAPPDATA%\dpub\snapshot.bin` | The last screen (a few MB), shown at the next launch while the book loads |

To reset everything, delete these files.

## How it stays fast

The rule is that the window never waits on the document.

```
 UI thread (main.c)               engine thread (engine.c)          index thread (engine.c)
 ─────────────────                ────────────────────────          ───────────────────────
 publishes what it wants  ──────▶ owns the open book                own copy of the book
 (page, size, theme, search)      open / layout / count pages       extracts each page's text
 paints whatever bitmaps  ◀────── renders visible pages first,      into a search index
 are ready                        then highlights, find, prefetch,
                                  thumbnails
```

- **The UI never calls MuPDF.** It writes a small request struct (see `shared.h`) and paints whatever bitmaps the engine has finished. A stale request simply gets overtaken. If you flip ten pages quickly, only the last one is rendered.
- **Stand-ins instead of waiting:**
  - If the right bitmap isn't ready, the closest one is drawn instead: stretched after a resize or zoom, or colour-inverted after a theme switch.
  - While a font-size change is being laid out, the old page stays on screen.
- **Prefetch:** the next two spreads and the previous one are rendered in the background, so turning a page is just a copy to the screen.
- **Progressive open:**
  - For EPUB, only the chapter you are reading is laid out before the first page appears. The rest is counted afterwards.
  - EPUB page counts are cached per layout, so reopening a book doesn't recount it.
- **Startup snapshot:** the last screen of the previous session is saved on exit. It is shown on launch, about 10–30 ms after the window appears, while the book loads behind it.
- **Background search index:**
  - A second thread opens its own copy of the book and indexes every page's text.
  - With the index, a search skips pages that can't contain the term and only asks MuPDF to confirm the few that can.
  - The index uses exactly the same text normalisation as MuPDF's matcher, so it never misses a match.
- **Hybrid CPUs:** dpub opts out of Windows power throttling, so the engine isn't parked on efficiency cores.

### MuPDF changes

MuPDF 1.27.0 is patched (`third_party/mupdf-patches.diff`). Each change was checked against a reference render, and the output is pixel-identical:

| Change | Why |
|---|---|
| Per-font shaping cache | Every word, including every space, used to be shaped through font fallback, FreeType and HarfBuzz on every layout, every draw and every search. Results are now recorded once and replayed. Lookups take no lock. |
| Bidi fast path | Left-to-right text in a single script resolves straight to level 0, skipping the full Unicode bidi algorithm. |
| Skip off-page boxes early | Drawing a page used to emit structure markers for every box in the document. In single-file books such as MOBI, that made search time grow with the square of the book's size. |
| Per-box child index | Binary search finds the children that can touch a page, instead of scanning thousands of siblings. |
| Lock-free allocation | With a thread-safe allocator (mimalloc), MuPDF no longer takes a global lock around every `malloc` / `free`. |
| `fz_search_fold_text` | Exposes the search matcher's text normalisation so an index can agree with it exactly. |

The libraries are built with LTO and profile-guided optimisation, trained on real books, and use mimalloc as the allocator.

### Numbers

Measured on an i7-14700KF, using the `bench` harness and the sample books listed below.

| | Stock MuPDF build | This build |
|---|---|---|
| War and Peace (MOBI, 1,234 pages): open + lay out | 270 ms | 110 ms |
| War and Peace (EPUB): lay out all chapters | 310 ms | 135 ms |
| Re-lay out after a font size change | 125 ms | 25–40 ms |
| Render a page | 2.5 ms | about 1 ms |
| Search every page (direct, without the index) | 0.8–1.7 s | 0.3–0.8 s |

On top of these, the UI hides almost all of the remaining time:

- Turning a page is an instant copy to the screen.
- An indexed search typically returns in about 15 ms.
- A large MOBI opened for the first time appears after about 150 ms. The window responds throughout.

## Building

Requirements:

- [MSYS2](https://www.msys2.org) at `C:\msys64`, with the mingw64 toolchain (`mingw-w64-x86_64-gcc`) and `mingw-w64-x86_64-make`.
- The MuPDF 1.27.0 source (with third-party libraries), unpacked to `third_party/mupdf-src`, with `third_party/mupdf-patches.diff` applied.
- The mimalloc 2.1.7 source, unpacked to `third_party/mimalloc`.

```bash
# quick development build: plain -O2 MuPDF libraries
bash third_party/mk.sh
bash build.sh dev

# release build: LTO + PGO MuPDF libraries
bash third_party/pgo.sh     # instrumented build, training run, optimised rebuild (~5 min)
bash build.sh
```

`pgo.sh` trains on the books in `third_party/samples` (not included):

- `war.mobi`, `war.epub`, `bible.mobi`, `bible.epub`, `alice.epub` and `alice.mobi` are from Project Gutenberg.
- `pdfref.pdf` is the PDF 32000 specification.
- `sample.pdf` is a one-page dummy PDF.

Any similar set of books will do.

The result is a static, stripped `dpub.exe`. It imports only system DLLs: kernel32, user32, gdi32, comctl32, comdlg32, shell32, ole32, advapi32, dwmapi and msvcrt.

### Checking MuPDF changes

```bash
bash bench/check.sh            # rebuild the tools, compare against bench/reference.txt, print timings
```

`bench/verify.c` lays out every sample book at two font sizes. For each one it records the page count, an MD5 of the rendered pixels of sampled pages, and search hit counts. `reference.txt` holds the output of unmodified MuPDF, and any change to MuPDF must reproduce it exactly.

Other tools in `bench/`:

- `bench.c` times each operation. `BENCH_RUNS=n` repeats runs, and `BENCH_SKIP=search` skips the search pass.
- `sampler.c` and `prof.py` form a small sampling profiler: set `BENCH_PROF=out.bin`, then run `python prof.py bench.exe out.bin`.
- `compare.sh` benchmarks one library build against another.

## Source layout

| Path | Contents |
|---|---|
| `main.c` | Window, toolbar, sidebar, scrollbar, input, painting, settings, startup snapshot |
| `engine.c` | Engine and index threads: open, layout, render, prefetch, thumbnails, search |
| `shared.h` | The contract between the UI and engine threads |
| `app.rc`, `app.manifest`, `app.ico` | Icon, per-monitor DPI awareness, UTF-8 code page, common controls |
| `build.sh` | Builds `dpub.exe` |
| `third_party/mk.sh`, `third_party/pgo.sh` | Build the MuPDF libraries |
| `third_party/mupdf-patches.diff` | The MuPDF changes |
| `bench/` | Benchmark, verification oracle and profiler |

## Limitations

- **Windows 10 or later, x64 only.** The build targets `x86-64-v2` (any CPU from about 2009 on).
- **No table-of-contents panel yet.** Use the previews, scrollbar or in-book links instead.
- **First-time opens of very large MOBI files:** these must be laid out completely before the first page shows (about 150 ms for a 2 MB book). EPUBs only lay out the current chapter.
- **Shifting position after font changes:** moving to another font size and back can shift the position by a page or two.
- **No DRM support:** Kindle and Adobe DRM-protected books can't be opened.

## License

dpub is free software, licensed under the [GNU Affero General Public License v3.0](LICENSE).

It builds on [MuPDF](https://mupdf.com) (AGPL v3) and [mimalloc](https://github.com/microsoft/mimalloc) (MIT).
