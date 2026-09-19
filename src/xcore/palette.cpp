#include "xcore.h"

#include <QFile>

// The palette shown when no preset is picked: even 0x00 / 0xbb / 0xff levels
// for all three channels, a wider gap between normal and bright than the
// presets. It shipped as palettes/xpeccy.txt until 2026.5.

static const uint32_t defColors[16] = {
	0x000000, 0x0000bb, 0xbb0000, 0xbb00bb,
	0x00bb00, 0x00bbbb, 0xbbbb00, 0xbbbbbb,
	0x000000, 0x0000ff, 0xff0000, 0xff00ff,
	0x00ff00, 0x00ffff, 0xffff00, 0xffffff
};

// A config naming the old file means the built-in palette, which holds the
// same colors now.

std::string palette_name(const std::string& nam) {
	return (nam == "xpeccy.txt") ? std::string() : nam;
}

// load preset colors for zx palette

QList<QColor> loadColors(std::string fname) {
	QList<QColor> list;
	QColor col;
	QFile file(xres_path("palettes", QString::fromLocal8Bit(fname.c_str())));
	int i = 0;
	QString line;
	QString hexPart;
	uint rgb;
	bool ok;
	int pos;
	if (file.open(QFile::ReadOnly)) {
		while (!file.atEnd() && (i < 16)) {
			line = file.readLine();
			pos = line.indexOf('#');
			if ((pos >= 0) && ((pos + 6) < line.size())) {
				hexPart = line.mid(pos + 1, 6);
				rgb = hexPart.toUInt(&ok, 16);
				if (ok) {
					col.setRed((rgb >> 16) & 0xff);
					col.setGreen((rgb >> 8) & 0xff);
					col.setBlue(rgb & 0xff);
					list.append(col);
					i++;
				}
			}
		}
		file.close();
	}
	return list;
}

int saveColors(std::string fname, QList<QColor> pal) {
	if (pal.size() < 16) return ERR_SIZE;
	std::string path = conf.path.palDir + SLASH + fname;
	QFile file(path.c_str());
	int err = ERR_OK;
	QColor col;
	QString str;
	if (file.open(QFile::WriteOnly)) {
		foreach(col, pal) {
			str = "#";
			str.append(gethexbyte(col.red()));
			str.append(gethexbyte(col.green()));
			str.append(gethexbyte(col.blue()));
			file.write(str.toLocal8Bit());
			file.write("\r\n");
		}
	} else {
		err = ERR_CANT_OPEN;
	}
	return err;
}

void loadPalette() {
	Computer* comp = conf.zx;
	// the preset is always kept as the base palette
	bool updateCurrentPallete = !!vid_zx_palette(comp->vid);
	QList<QColor> pal = loadColors(conf.palette);
	xColor xcol;
	int i;
	if (pal.size() != 16) {		// no preset, or a wrong one: the built-in palette
		pal.clear();
		for (i = 0; i < 16; i++)
			pal.append(QColor((defColors[i] >> 16) & 0xff, (defColors[i] >> 8) & 0xff, defColors[i] & 0xff));
	}
	for (i = 0; i < 16; i++) {
		xcol.r = pal[i].red();
		xcol.g = pal[i].green();
		xcol.b = pal[i].blue();
		vid_set_bcol(comp->vid, i, xcol);
		if (updateCurrentPallete)
			vid_set_col(comp->vid, i, xcol);
	}
}
