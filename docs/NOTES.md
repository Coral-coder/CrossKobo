# Handwritten notes

CrossKobo's notebooks are built for the Kobo Stylus 2 on a Libra Colour,
and they work with a finger on devices with no stylus.

## Writing

Open *Notebooks* from the home screen and tap *New notebook*, or, while
reading, open the reader menu and choose *New note for this book* — that
creates a notebook linked to the book, and reopens the same one next time.

The toolbar, left to right: back, pen, highlighter, eraser, nib width, ink
colour, undo, redo — then, on the right, page back, the page number, page
forward, add page, and the notebook menu.

- **Pen** draws with pressure: press harder for a heavier line. The width
  swatch shows the current nib; tap it to cycle through six widths.
- **Highlighter** draws wide translucent strokes, four times the nib width.
- **Eraser** removes whole strokes rather than nibbling at pixels, which is
  both faster on e-ink and easier to undo. The stylus's own eraser end
  works whatever tool is selected.
- **Undo/redo** goes eighty steps back.
- Page turn buttons and left/right swipes move between pages.

While the pen is down, CrossKobo draws each segment straight to the panel
using its fastest waveform, so the line appears under the nib rather than a
frame later. When you lift the pen, the page is redrawn properly — you may
see the ink settle from grainy to solid. If you prefer the cleaner line and
can live with the lag, turn off *Fast ink refresh* in the notebook menu or
*Settings → Notes*.

**Palm rejection** ignores touch input for about a second after the pen was
last seen, so resting a hand on the page does not draw. It can be turned
off in *Settings → Notes*.

**No stylus?** A Libra Colour reports a digitiser whether or not you own a
stylus, so CrossKobo lets you draw with a finger until a stylus is actually
used. From that point on, touch is treated as a palm and only the pen
draws.

## Templates

Blank, lined, grid, dots and Cornell. Set the default for new pages in
*Settings → Notes → Default page template*, or change the current page from
the notebook menu. Templates are drawn light enough not to compete with the
ink, and they are included in exports.

## Colours

Ten inks, picked so they stay distinct through the Kaleido colour filter:
black, graphite, red, orange, yellow, green, teal, blue, violet, magenta.
Pastels are deliberately absent — the colour filter array roughly halves
effective saturation, so a light colour ends up indistinguishable from grey
on paper-white.

## Where notebooks live

Each notebook is one file in the `Notebooks` folder on the Kobo's drive:

```
Notebooks/Notes 13 Sep 2026.ckn
Notebooks/Notes on The Colour of Ink.ckn
```

`.ckn` files are plain JSON: a header with title, size and timestamps, then
pages, then strokes as flat integer arrays of x, y and pressure. A page of
dense handwriting is a few tens of kilobytes. You can copy them off over
USB, back them up, or diff them.

To rename a notebook, rename its `.ckn` file from a computer; the new name
appears the next time you open the list.

## Exporting

From inside a notebook, or by long-pressing it in the notebook list:

- **PDF** writes the whole notebook as one file next to the `.ckn`, with
  strokes as vector paths. It stays sharp at any zoom, prints properly, and
  a long notebook is still tens of kilobytes rather than megabytes.
  Pressure variation is preserved by splitting strokes where the width
  changes.
- **PNG** writes one image per page into a folder next to the `.ckn`, at
  the notebook's own pixel size.

Both land on the Kobo's drive, so they are there the next time you plug in.

## Notes attached to books

*New note for this book* in the reader menu links a notebook to the book
you are reading. The link is stored in the notebook, and the notebook list
shows which book each one belongs to. Opening the same book again and
choosing *New note for this book* reopens that notebook rather than making
another.

## Limits worth knowing

- A notebook page is a fixed pixel grid, adopted from the screen it was
  created on, and scaled to fit if you later rotate the device or open it
  on a different model. Strokes are never re-flowed.
- There is no text recognition, and no text boxes: this is ink.
- There are no layers. The eraser removes strokes, and undo is the safety
  net.
- Notebooks are saved when you leave a page, leave the notebook, or the
  device sleeps — not on every stroke, to spare the flash. A hard
  power-off can lose the strokes made since the last page change.
