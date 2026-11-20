# Cut Liberation Sans Bold down (Latin-1, Cyrillic) for the on-screen keyboard and rename it (OFL).
# usage: python tools/vkbd_font.py <LiberationSans-Bold.ttf of liberation-fonts-ttf-2.1.5> res/vkbd/keys-font.ttf
# needs fonttools (pip install fonttools); the same input gives the same bytes
import sys
from fontTools.ttLib import TTFont
from fontTools import subset

src, out = sys.argv[1], sys.argv[2]
font = TTFont(src, recalcTimestamp=False)
opt = subset.Options()
opt.layout_features = []
opt.drop_tables += ["GSUB", "GPOS", "GDEF", "FFTM"]
opt.hinting = True          # TrueType hints kept: the keyboard is hinted by them, not by the auto-hinter
opt.legacy_kern = True      # the 'kern' table is where the kerning comes from
opt.name_IDs = ["*"]
opt.notdef_outline = True
sub = subset.Subsetter(opt)
# Latin-1, and the Cyrillic letters (the Robik's caps)
sub.populate(unicodes=list(range(0x20, 0x7F)) + list(range(0xA0, 0x100)) + [0x401, 0x451] + list(range(0x410, 0x450)))
sub.subset(font)

# OFL: a Modified Version may not use the Reserved Font Names (Liberation, Arimo)
fam, full, ps = "Xpeccy Keys Sans", "Xpeccy Keys Sans Bold", "XpeccyKeysSans-Bold"
name = font["name"]
for rec in list(name.names):
    if rec.nameID in (16, 17, 18, 21, 22, 25):
        name.removeNames(nameID=rec.nameID)
for pid, eid, lid in ((3, 1, 0x409), (1, 0, 0)):
    name.setName(fam, 1, pid, eid, lid)
    name.setName("Bold", 2, pid, eid, lid)
    name.setName(full + "; derived from Liberation Sans Bold 2.1.5", 3, pid, eid, lid)
    name.setName(full, 4, pid, eid, lid)
    name.setName(ps, 6, pid, eid, lid)
    name.setName("Liberation Sans Bold 2.1.5, cut down to Latin-1 and Cyrillic and renamed for the "
                 "Xpeccy+ on-screen keyboard", 10, pid, eid, lid)
# the timestamps stay the release's, so the same input gives the same bytes
font.save(out, reorderTables=True)
print(out)
