# Third-party notices

Netloom includes the following third-party material. Each keeps its own licence.

## Hershey fonts (glyph data)

`core/third_party/hershey/*.jhf`, used by `tools/gen_stroke_font.py` to generate
`core/src/stroke_glyphs.cpp`.

- The Hershey Fonts were originally created by Dr. A. V. Hershey while working at the U. S.
  National Bureau of Standards.
- The format of the Font data in this distribution was originally created by James Hurt,
  Cognition, Inc., 900 Technology Park Drive, Billerica, MA 01821 (mit-eddie!ci-dandelion!hurt).

Use, including commercial use, is permitted with these acknowledgements; the full notice is in
`core/third_party/hershey/NOTICE`. The accented letters and symbols of the Netloom stroke font
were composed or drawn for Netloom.

## earcut.hpp (polygon triangulation)

`core/third_party/earcut.hpp` - Copyright (c) 2015, Mapbox. ISC License:

> Permission to use, copy, modify, and/or distribute this software for any purpose with or
> without fee is hereby granted, provided that the above copyright notice and this permission
> notice appear in all copies.
>
> THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
> SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
> ISC BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
> WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
> NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
> PERFORMANCE OF THIS SOFTWARE.

## stb_image_write.h (desktop test tool only)

`tools/stb_image_write.h` - Sean Barrett, public domain / MIT (dual). Used only by the `kpcli`
desktop tool, not by the app.

## Example boards

`samples/pic_programmer.kicad_pcb`, `samples/ecc83-pp.kicad_pcb` - KiCad demo projects,
© KiCad developers, licensed under Creative Commons Attribution-ShareAlike 4.0 (CC BY-SA 4.0,
https://creativecommons.org/licenses/by-sa/4.0/). Bundled unchanged as example files.
