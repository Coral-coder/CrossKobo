# Features

Everything CrossKobo 0.1.0 does, and where to find it. Settings paths are
written as *Settings → Section → Item*; the reader's own menu is reached by
tapping the middle of a page, or swiping up.

## Home

- **Continue reading** card with cover, progress and when you last read it.
- **Recents grid**, three across and up to three rows, following CrossInk's
  3x3 layout. Books with no cover get a generated one showing the title.
- **Dashboard theme** replaces the grid with eight statistics tiles
  (*Settings → Display → Interface theme → Dashboard*).
- Bottom navigation: Library, Notebooks, Statistics, Settings.
- The page-turn buttons open the last book you were reading.

## Library

- Browses the whole Kobo drive, folders and all. Stock Kobo folders
  (`.kobo`, `.adds`, …) are hidden.
- Formats: **EPUB 2 and 3**, **plain text** (`.txt`, `.md`), **CBZ** comics.
- Per-book progress, "Read" markers, file size and format on each row.
- **Search** by title or author with an on-screen keyboard.
- Sort by newest, title, or author.
- Long-press a book for: open, mark finished/unread, details, delete.
- *Settings → Library*: show hidden files, move finished books to a "Read"
  folder, drop finished books from Recent, clear the cover cache.

## Reader

### Text and layout
- Justified or ragged-right setting, with hyphenation that only breaks at
  existing hyphens, soft hyphens, or conservative vowel-consonant
  boundaries in long words.
- The book's own CSS is honoured for weight, slant, size, alignment,
  indents, margins, colour, visibility and page breaks — and can be turned
  off entirely (*Book's own styles*).
- Headings, lists (bulleted and numbered), block quotes, preformatted
  blocks, horizontal rules, superscripts and subscripts, underline and
  strikethrough, inline images.
- Images are scaled to the page and kept in colour; a book's own colour
  text stays coloured.
- Font family (bundled fonts, the fonts your Kobo already has in
  `/usr/local/Kobo/fonts`, and anything you drop in a `fonts` folder on the
  drive), size in points, line spacing 100-200%, margins 8-160 px.
- Force paragraph indents and extra paragraph spacing, for books whose own
  styling is poor.

### Navigation
- Page turn: hardware buttons (swappable), taps at the left and right edge,
  or swipes. Tap zones can be standard, inverted, or off.
- Table of contents from the EPUB 3 navigation document or the EPUB 2 NCX,
  nested, with the current chapter marked.
- **Go to** any 5% step through the book.
- **Bookmarks**: long-press a page to add or remove one; a star appears in
  the status bar. The bookmark list shows a snippet of each page.
- **Footnotes**: tapping a footnote marker shows the note in a pop-up
  rather than jumping away from the page. Longer targets navigate properly.
- Reading position, per book, is stored on the user partition and survives
  reinstalls and firmware updates.

### Reading modes
- **Focus reading** hides the status bar completely.
- **Guide dots** draw a faint dotted rule under each line.
- **Auto page turn** at 5 to 120 seconds.
- **Full refresh every N pages** (0 = never) to clear ghosting.
- Status bar contents are individually switchable: title, clock, battery,
  battery percentage, progress bar and its thickness, chapter page count,
  book percentage.

### Per book
- New note for this book: creates (or reopens) a notebook linked to it.
- Mark as finished, which optionally moves the file to a "Read" folder.
- Book details: metadata, series, sections, time spent, sessions, pages
  turned, file path.

## Notebooks

- Unlimited notebooks, each many pages, stored as `.ckn` JSON files in the
  `Notebooks` folder on the drive — copy them off over USB whenever.
- **Pressure-sensitive ink** from the Kobo Stylus 2 (and any other stylus
  the device reports as a pen): stroke width follows pressure between 45%
  and 130% of the nominal width.
- **Ten inks** chosen to survive the Kaleido colour filter: black,
  graphite, red, orange, yellow, green, teal, blue, violet, magenta.
- **Highlighter** mode: wide, translucent strokes.
- **Eraser**: whole strokes, by touch, and the stylus's own eraser end is
  honoured automatically. Undo and redo, eighty steps deep.
- Six pen widths, from hairline to marker.
- **Page templates**: blank, lined, grid, dots, Cornell.
- **Fast ink refresh** (on by default) draws each segment with the panel's
  A2 waveform as the pen moves, then settles the page with a proper
  greyscale refresh when you lift off. Turning it off trades latency for a
  cleaner line.
- **Palm rejection** ignores touch input for 0.9 s after the pen was last
  seen. On devices with no stylus, finger drawing is enabled instead.
- **Export**: a whole notebook to a single vector PDF (strokes stay
  strokes, so it prints sharp and stays small), or every page to PNG.
- Long-press a notebook in the list to export or delete it without opening.

## Colour

Kaleido 3 puts a colour filter array over the same greyscale ink, so
colour resolves at half the linear resolution of black and white, and
saturation is inherently gentle. CrossKobo exposes the controls that matter
rather than hiding them:

- **Colour rendering** (*Settings → Display → Colour*): panel default,
  standard, boosted, vivid, muted, or greyscale. These map to the display
  controller's own colour-filter modes.
- **Extra saturation**: a software boost from -50% to +80%, applied before
  the panel's own filter.
- **Images**: greyscale, colour, or colour with a boost, per book.
- Colour pages are sent with the panel's colour waveform and text pages
  with the fast greyscale one, decided per refresh by looking at what is
  actually on the page.

## Display, light and power

- Three interface themes: Classic (rules and panels), Minimal
  (typography only), Dashboard (statistics forward).
- **Night mode** inverts the interface, using the panel's eclipse waveforms
  on MediaTek devices and software inversion elsewhere.
- Front light brightness and, on devices with natural light, warmth.
- Rotation in 90° steps, with input rotated to match.
- Sleep after 0-120 minutes; power off after 0-48 hours.
- Sleep screens: book cover, reading progress, statistics, a custom image,
  or blank.
- Sleep uses the kernel sequence the stock software uses, and refuses to
  suspend while charging on MediaTek devices, where that hangs the kernel.

## Statistics

Books finished, total time reading, sessions, pages turned, average
session, longest session, pages per minute, time today, reading streak in
days, reading-since date, and per-book progress. Sessions shorter than five
seconds are ignored so opening a book by accident does not pollute them.

## Controls and diagnostics

- Swap page-turn buttons; buttons follow rotation.
- Tap zones and centre-tap behaviour.
- USB behaviour: ask, hand over to the Kobo UI, or ignore.
- **Touch and stylus test**: shows where input lands, with per-axis mirror
  and swap toggles for both the touch panel and the digitiser. This is the
  screen to use if taps or ink land in the wrong place on a model this
  build has not been calibrated against.
- **About** reports the detected device, panel geometry, display controller,
  whether a stylus and a colour panel were found, and where files live.

## Deliberately not in 0.1.0

Wi-Fi, OPDS, KOReader progress sync, dictionaries, PDF reading, text
selection and highlights, and USB mass storage handled in-app. The first
three need a network stack this build does not link; PDF needs a rendering
engine; USB mass storage is left to the stock software on purpose, because
that is also what makes the recovery path reliable.
