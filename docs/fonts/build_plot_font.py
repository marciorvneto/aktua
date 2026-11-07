# Usage: python build_plot_font.py dejavu-sans-mono-plot.woff2
# Needs fonttools and brotli (pip install fonttools brotli), and the DejaVu
# fonts installed under /usr/share/fonts/truetype/dejavu.
#
# Builds docs/fonts/dejavu-sans-mono-plot.woff2: DejaVu Sans Mono (ASCII and
# box drawing) plus the braille block from DejaVu Sans, re-centred onto the
# monospace advance width so every character in a plot row is the same width.
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen
from fontTools.pens.transformPen import TransformPen
from fontTools.pens.recordingPen import DecomposingRecordingPen
from fontTools import subset

mono = TTFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf")
sans = TTFont("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
advance = mono["hmtx"]["a"][0]
sans_cmap, sans_glyphs = sans.getBestCmap(), sans.getGlyphSet()

for cp in range(0x2800, 0x2900):
    src = sans_cmap[cp]
    shift = -(sans["hmtx"][src][0] - advance) / 2
    pen = TTGlyphPen(None)
    outline = DecomposingRecordingPen(sans_glyphs)  # braille dots are components
    sans_glyphs[src].draw(outline)
    outline.replay(TransformPen(pen, (1, 0, 0, 1, shift, 0)))
    name = "braille%04X" % cp
    glyph = pen.glyph()
    glyph.recalcBounds(mono["glyf"])
    mono["glyf"][name] = glyph
    mono["hmtx"][name] = (advance, glyph.xMin)
    for table in mono["cmap"].tables:
        if table.isUnicode():
            table.cmap[cp] = name
mono.setGlyphOrder(mono["glyf"].glyphOrder)  # glyf appends new names itself
mono["maxp"].numGlyphs = len(mono["glyf"].glyphOrder)

opts = subset.Options()
opts.flavor = "woff2"
opts.drop_tables += ["FFTM"]
sub = subset.Subsetter(opts)
sub.populate(unicodes=list(range(0x20, 0x7F)) + list(range(0x2500, 0x2580)) + list(range(0x2800, 0x2900)))
sub.subset(mono)
mono.flavor = "woff2"
mono.save(sys.argv[1])
