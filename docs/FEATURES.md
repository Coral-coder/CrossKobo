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
- **At a glance**: when the shelf leaves room, four glass tiles above the
  buttons show time read today, the current streak, books finished and how
  many notebooks you have. Tapping one goes to statistics or notebooks.
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

- Four interface themes: Classic (rules and panels), Minimal (typography
  only), Dashboard (statistics forward) and **Aero** — the glossy,
  sky-blue, water-droplet look of the late 2000s: vertical gradients, a
  glass wash over the chrome, soft shadows, droplets on the page ground,
  four-colour navigation buttons, and coverless books given their own hue
  so a shelf is colourful. Saturated on purpose: the Kaleido colour filter
  halves effective saturation, so gentle tints wash out to grey on the
  panel. Night mode has its own deep-water Aero palette.
- **Night mode** inverts the interface, using the panel's eclipse waveforms
  on MediaTek devices and software inversion elsewhere.
- Front light brightness and, on devices with natural light, warmth.
- Rotation in 90° steps, with input rotated to match.
- Sleep after 0-120 minutes; power off after 0-48 hours **asleep**, so a
  reader left in a bag does not come out flat.
- Sleep screens: book cover, reading progress, statistics, a custom image,
  or blank. For a custom image, drop a `sleep.png` into the `.crosskobo`
  folder on the drive (or set an explicit path in the settings file).
- Sleep uses the kernel sequence the stock software uses, and refuses to
  suspend while charging on MediaTek devices, where that hangs the kernel.

## Statistics

Books finished, total time reading, sessions, pages turned, average
session, longest session, pages per minute, time today, reading streak in
days, reading-since date, and per-book progress. Sessions shorter than five
seconds are ignored so opening a book by accident does not pollute them.

## Catalogues (OPDS)

- *Settings → Catalogues*, or the **Catalogues** button in the library,
  keeps a list of OPDS servers: Shelfmark, Calibre-Web, Kavita, Komga,
  BookLore, Project Gutenberg - anything that speaks OPDS.
- Navigation entries drill in, books show their author and summary, and
  downloading writes straight into `Downloads` on the drive, where the
  library picks it up like anything copied over USB. EPUB is preferred when
  a catalogue offers several formats.
- **A list of servers in a file.** `.crosskobo/catalogues.txt` on the drive
  is read every time the catalogue list appears, so a server added there
  shows up without a restart - and a friend can send you a line to paste:

  ```
  A friend's library | https://books.example.net/opds | reader | secret
  Their server       | https://fic.example.net/search.php?req={searchTerms}
  ```

  Addresses rather than IP numbers, so the same file works at home and away.
  The client is worked out from the address, or say `libgen` or `opds`
  outright. A JSON array in `.crosskobo/catalogues.json` works too. These
  entries are never written into `settings.json`: the file stays the one
  place they are defined, so deleting a line really removes it. CrossKobo
  writes a commented template on first run.
- **Find on network** sweeps the local subnet for a catalogue rather than
  making you type an address: a TCP probe of the ports these servers
  actually use (8083 Calibre-Web, 8080 Calibre and Komga, 5000 Kavita,
  25600 Komga, 6060 BookLore, and a few of the Docker crowd), then an OPDS
  request to `/opds`, `/opds/v1.2/catalog`, `/api/opds`, `/opds/root.xml`
  and `/catalog` on whatever answered. Anything that returns a feed is
  offered by its own title. It only runs when you ask, never in the
  background.
- Search uses the catalogue's own search link. Paged catalogues get a
  **More** button. Servers behind Basic authentication work: put `user` and
  `password` in the catalogue's entry in `.crosskobo/settings.json`.
- **Search-format servers.** Some self-hosted catalogue software answers a
  search rather than offering a feed - including anything forked from
  Library Genesis, which is a common starting point because the code is
  there to fork. Give CrossKobo the address of a server you run and it
  searches it and downloads to the device: it reads both the JSON answer and
  the simple HTML table, identifies the columns by what they hold rather
  than by position (forks move them around), and follows the mirror page to
  the file when the answer does not link to it directly. CrossKobo ships no
  addresses for this; the format is only how it talks to the server you
  point it at. An address naming `search.php` or `json.php` is recognised as
  this format automatically.
- **Your own search format.** An address with `{searchTerms}` in it is
  added as a search source rather than a feed: CrossKobo fills the
  placeholder in and reads the answer as OPDS, so a server with its own
  search endpoint - or several of them - is used exactly as it is. Such a
  catalogue opens straight into the keyboard.
- **Ships with the public-domain libraries** already in the list: Standard
  Ebooks, Project Gutenberg and the Internet Archive. Remove them and they
  stay removed.
- **In the stock library, with nothing else installed.** The add-on puts
  "CrossKobo Catalogues" and "CrossKobo Shelfmark" on the drive as library
  entries and runs a small watcher that notices the Kobo software opening
  one of them. Tapping an entry opens the browser; leaving it hands the
  screen straight back. The bottom bar of the stock home screen belongs to
  nickel itself and cannot be extended without patching it, which
  CrossKobo does not do.
- **Without replacing the interface at all.** There is a separate
  `CrossKobo-catalogues-<version>-install.zip` that installs only the
  catalogue browser and its menu entries: no boot hook, no replacement
  shell, the stock Kobo software stays in charge. See docs/INSTALL.md.
- **In the stock Kobo software.** Its Discover tab belongs to a closed Qt
  application and cannot be extended from outside it. Where
  [NickelMenu](https://github.com/pgaskin/NickelMenu) is installed -
  the established way to add entries to the stock menus - CrossKobo drops a
  config file into `.adds/nm/crosskobo` on first run, which adds
  **CrossKobo**, **Catalogues (OPDS)** and **Shelfmark** to the stock menu.
  Each one borrows the screen and hands it back when you leave. The
  uninstaller removes that file again.

## Getting around

- **Swipe up from the bottom edge** to go back, from anywhere. There is no
  hardware back button on these devices, and a screen that fills itself can
  leave the chevron in the top bar easy to miss.
- **Swipe down from the top edge** for the quick panel: front light, night
  mode, Wi-Fi, sharing the drive over USB, and the way home or into
  settings. The note canvas opts out of both, so a stroke that starts at an
  edge still draws.
- The page-turn buttons work anywhere they mean something: they open the
  last book from the home screen, answer the USB prompt, and dismiss the
  quick panel.
- **Both page buttons together** opens touch calibration, from any screen.

## Touch calibration

Panels differ: the Libra Colour reports finger and stylus on one device,
with its X axis running down the screen. CrossKobo works the transposition
out from the kernel's own axis ranges, which gets most panels right, and
*Settings → Controls → Calibrate touch* settles the rest:

- Tap three marked corners - top left, top right, bottom left. The wizard
  reads the digitiser's raw coordinates, so it works even when taps land
  nowhere near where they are drawn.
- Three taps and not two, because a diagonal is symmetric under
  transposition: top-left and bottom-right cannot tell a transposed panel
  from a mirrored one.
- Reachable with **both page buttons together** from anywhere, so a
  mis-mapped panel can always be fixed on the device.
- Once calibrated, your answer is the whole mapping and CrossKobo stops
  guessing. *Touch and stylus test* still shows where input lands.

## Software updates

- *Settings → Software update* asks the release page for the latest
  version, downloads the package for this device's firmware generation, and
  stages it the way the firmware already understands: a `KoboRoot.tgz` in
  `.kobo`, installed on the next restart. It offers to restart for you.
- A background check runs once a day, and only when Wi-Fi is already
  connected - CrossKobo never turns the radio on by itself. When there is
  something new it says so in a toast, and nothing else happens until you
  ask. Turn it off with `"autoUpdateCheck": false` in
  `.crosskobo/settings.json`.
- Downloads go through CrossKobo's own bundled fetcher, so they work the
  same on every device (see below).

## The bundled fetcher

CrossKobo replaces the software that would otherwise do its downloading, and
not every firmware ships `curl` or `wget` - so https worked on one Kobo and
not the next. It now carries its own:

- A static armhf **curl** with **Mbed-TLS** for the TLS and **c-ares** for
  the DNS, installed at `/usr/local/crosskobo/bin/curl` and preferred over
  anything the firmware has. c-ares matters as much as the TLS: a statically
  linked glibc cannot load the NSS modules that normally resolve names, so
  without it an address - as opposed to an IP - would fail, which is exactly
  what a catalogue away from home is.
- **Mozilla's certificate set** at `/usr/local/crosskobo/cacert.pem`,
  verified against on every request. Certificate verification is never
  turned off.
- A **private CA** of your own is supported: put it in
  `.crosskobo/extra-ca.pem` on the drive and it is trusted *alongside* the
  bundled set, not instead of it.
- It is built and then **proven in CI on ARM** before a release ships: a
  real request to a real server, a verified certificate, and a refusal of a
  certificate it cannot verify. The build is what fails if the fetcher
  cannot do those, rather than the reader's catalogue.
- The log records which fetcher answered, bundled or otherwise, because that
  is the first thing worth knowing when a catalogue will not load.

## Controls and diagnostics

- Swap page-turn buttons; buttons follow rotation.
- Tap zones and centre-tap behaviour.
- USB behaviour: ask, share the drive, hand over to the Kobo UI, or ignore.
  The USB prompt answers the hardware page buttons as well as touch -
  page-forward shares the drive, page-back hands over to the stock
  software - so a cable and one button are always enough to reach a
  computer, even if you cannot see or tap what is on screen.
- **Touch and stylus test**: shows where input lands, with per-axis mirror
  and swap toggles for both the touch panel and the digitiser. This is the
  screen to use if taps or ink land in the wrong place on a model this
  build has not been calibrated against.
- **About** reports the detected device, panel geometry, display controller,
  whether a stylus and a colour panel were found, and where files live.

## Added since 0.1.0

- **Hold a page-turn button while the device starts** and the stock Kobo
  software boots instead, just for that boot. No files to place, nothing to
  undo.

- **Boot takeover**: CrossKobo owns the boot. The stock software is started
  only to inherit its environment and is then stopped, the Kobo boot
  animation is stopped with it, and a watchdog keeps it from coming back.
  The indicator LED, which the firmware blinks until the stock software
  reports itself up, is quietened too.
- **Wi-Fi**: scan, connect, saved networks, status in the bar.
- **USB mass storage in-app**: share the drive without leaving CrossKobo.
- **Aero theme** and the at-a-glance tiles.

## Deliberately not here yet

OPDS catalogues, KOReader progress sync, dictionaries, PDF reading, and
text selection with highlights. The reader engine handles EPUB, TXT and
CBZ; PDF needs a rendering engine this build does not carry.
