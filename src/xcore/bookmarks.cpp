#include <QFileInfo>
#include <QDir>

#include "xcore.h"

void addBookmark(std::string nm, std::string fp) {
	xBookmark nbm;
	nbm.name = nm;
	nbm.path = fp;
	conf.bookmarkList.push_back(nbm);
}

// one file whichever way its path was written; no disk access, so a favorite
// on a drive that is gone cannot hold the menu up
static QString bkm_key(const QString& path) {
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

static bool bkm_same(const QString& a, const QString& b) {
#ifdef _WIN32
	return bkm_key(a).compare(bkm_key(b), Qt::CaseInsensitive) == 0;
#else
	return bkm_key(a) == bkm_key(b);
#endif
}

int findBookmark(const QString& path) {
	for (int i = 0; i < conf.bookmarkList.size(); i++) {
		if (bkm_same(QString::fromLocal8Bit(conf.bookmarkList[i].path.c_str()), path))
			return i;
	}
	return -1;
}

void swapBookmarks(int p1, int p2) {
	xBookmark bm = conf.bookmarkList[p1];
	conf.bookmarkList[p1] = conf.bookmarkList[p2];
	conf.bookmarkList[p2] = bm;
}

void setBookmark(int idx,std::string nm, std::string fp) {
	conf.bookmarkList[idx].name = nm;
	conf.bookmarkList[idx].path = fp;
}

void delBookmark(int idx) {
	conf.bookmarkList.erase(conf.bookmarkList.begin() + idx);
}

void clearBookmarks() {
	conf.bookmarkList.clear();
}

// Recent files: one entry per file, however its path was written
void recent_remove(const QString& path) {
	for (int i = conf.recentList.size() - 1; i >= 0; i--) {
		if (bkm_same(conf.recentList[i], path))
			conf.recentList.removeAt(i);
	}
}

void recent_add(const QString& path) {
	recent_remove(path);
	conf.recentList.prepend(bkm_key(path));
	while (conf.recentList.size() > RECENT_MAX)
		conf.recentList.removeLast();
}
