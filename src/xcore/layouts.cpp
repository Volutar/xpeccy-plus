#include <stdio.h>
#include <stdlib.h>
#include <algorithm>

#include <QFile>

#include "xcore.h"

xLayout* findLayout(std::string nm) {
	xLayout* res = NULL;
	for (int i = 0; i < conf.layList.size(); i++) {
		if (conf.layList[i].name == nm) res = &conf.layList[i];
	}
	return res;
}

// todo: case insensitive

bool ly_compare(const xLayout lay1, const xLayout lay2) {
	return (lay1.name < lay2.name);
}

bool addLayout(std::string nm, vLayout vlay) {
	if (findLayout(nm) != NULL) return false;
	xLayout nlay;
	nlay.name = nm;
	nlay.lay = vlay;
	conf.layList.push_back(nlay);
	std::sort(conf.layList.begin(), conf.layList.end(), ly_compare);
	return true;
}

void rmLayout(std::string nm) {
	for (int i = 0; i < conf.layList.size(); i++) {
		if (conf.layList[i].name == nm) {
			conf.layList.erase(conf.layList.begin() + i);
		}
	}
	std::sort(conf.layList.begin(), conf.layList.end(), ly_compare);
}

// A layout ships with the binary; a layouts.conf in the config directory adds
// to that list, and an entry of the same name there replaces the shipped one.
// Only what the user added or changed is written back, so a fix in an update
// reaches a layout nobody has touched.

#define	LAY_RES_FILE	":/res/layouts.conf"
#define	LAY_USER_FILE	"layouts.conf"

static QList<xLayout> layShipped;

static bool lay_same(const vLayout& a, const vLayout& b) {
	return (a.full.x == b.full.x) && (a.full.y == b.full.y)
		&& (a.bord.x == b.bord.x) && (a.bord.y == b.bord.y)
		&& (a.blank.x == b.blank.x) && (a.blank.y == b.blank.y)
		&& (a.scr.x == b.scr.x) && (a.scr.y == b.scr.y)
		&& (a.intpos.x == b.intpos.x) && (a.intpos.y == b.intpos.y)
		&& (a.intSize == b.intSize);
}

// name:full.x:full.y:bord.x:bord.y:blank.x:blank.y:intSize:intpos.y:intpos.x:scr.x:scr.y

bool addLayoutString(const std::string& str) {
	std::vector<std::string> part = splitstr(str, ":");
	if (part.size() < 9) return false;
	vLayout lay;
	lay.full.x = atoi(part[1].c_str());
	lay.full.y = atoi(part[2].c_str());
	lay.bord.x = atoi(part[3].c_str());
	lay.bord.y = atoi(part[4].c_str());
	lay.blank.x = atoi(part[5].c_str());
	lay.blank.y = atoi(part[6].c_str());
	lay.intSize = atoi(part[7].c_str());
	lay.intpos.y = atoi(part[8].c_str());
	lay.intpos.x = (part.size() > 9) ? atoi(part[9].c_str()) : 0;
	lay.scr.x = (part.size() > 10) ? atoi(part[10].c_str()) : 256;
	lay.scr.y = (part.size() > 11) ? atoi(part[11].c_str()) : 192;
	if (lay.full.x > 512) lay.full.x = 512;
	if (lay.full.y > 512) lay.full.y = 512;
	xLayout* old = findLayout(part[0]);
	if (old) {
		old->lay = lay;
	} else {
		addLayout(part[0], lay);
	}
	return true;
}

std::string layoutString(const xLayout& lay) {
	char buf[256];
	snprintf(buf, sizeof(buf), "%s:%i:%i:%i:%i:%i:%i:%i:%i:%i:%i:%i", lay.name.c_str(),
		lay.lay.full.x, lay.lay.full.y, lay.lay.bord.x, lay.lay.bord.y,
		lay.lay.blank.x, lay.lay.blank.y, lay.lay.intSize, lay.lay.intpos.y,
		lay.lay.intpos.x, lay.lay.scr.x, lay.lay.scr.y);
	return std::string(buf);
}

static void lay_read(const QString& path) {
	QFile file(path);		// QFile, not ifstream: the shipped one is a resource
	if (!file.open(QFile::ReadOnly | QFile::Text)) return;
	// not a stream: Qt 5's follows the code page, and an older build wrote the system's
	foreach(QString line, text_8bit(file.readAll()).split('\n')) {
		line = line.section('#', 0, 0).section(';', 0, 0).trimmed();
		if (line.isEmpty() || !line.contains('=')) continue;
		std::pair<std::string,std::string> spl = splitline(line.toLocal8Bit().data());
		if (spl.first == "layout")
			addLayoutString(spl.second);
	}
}

void layouts_load_all() {
	conf.layList.clear();
	lay_read(LAY_RES_FILE);
	layShipped = conf.layList;
	lay_read(QString::fromLocal8Bit(conf.path.confDir.c_str()) + SLASH LAY_USER_FILE);
}

// the shipped version of a layout, for putting one back after it was changed
// or deleted

const xLayout* layout_shipped(const std::string& name) {
	foreach(const xLayout& lay, layShipped) {
		if (lay.name == name) return &lay;
	}
	return NULL;
}

void layouts_save() {
	QStringList out;
	foreach(xLayout lay, conf.layList) {
		const xLayout* shp = layout_shipped(lay.name);
		if (!shp || !lay_same(shp->lay, lay.lay)) out << QString("layout = %1").arg(QString::fromLocal8Bit(layoutString(lay).c_str()));
	}
	std::string path = conf.path.confDir + SLASH LAY_USER_FILE;
	if (out.isEmpty()) {
		remove(path.c_str());		// nothing of the user's own left in it
		return;
	}
	FILE* file = fopen(path.c_str(), "wb");
	if (!file) {
		xlog(XLG_CONF, XLL_ERROR, "can't write %s", path.c_str());
		return;
	}
	fprintf(file, "# Screen layouts of your own. The ones that ship are in the binary.\n\n");
	fprintf(file, "%s\n", out.join("\n").toLocal8Bit().data());
	fclose(file);
}
