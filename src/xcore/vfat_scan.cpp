#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QStringList>

#include "vfat_scan.h"
#include "xcore.h"
#include "../libxpeccy/xlog.h"

// Host folder -> synthetic FAT32 volume. The scan lives here so libxpeccy stays
// free of platform directory code; vfat.c only ever sees a tree and file paths.

#define VFS_MAXDEPTH	16		// deeper folders are left out
#define VFS_MAXNODES	200000		// and so is anything past this many entries: the scan holds the gui

// what a scan had to leave out
struct vfsDrop {
	int deep = 0;			// folders past VFS_MAXDEPTH
	int full = 0;			// entries past VF_MAXDENTS in their folder
	int big = 0;			// files too big for FAT
	bool count = false;		// stopped at VFS_MAXNODES
};

struct vfsDir {
	int idx;
	QString path;
	int depth;
};

// breadth first, so whatever a limit cuts off is the deepest level and never
// the loader in the root
static void vfat_scan_tree(vFat* vf, const QString& root, vfsDrop& drop) {
	QList<vfsDir> queue;
	queue.append({0, root, 0});
	for (int q = 0; q < queue.size(); q++) {
		vfsDir cur = queue[q];			// a copy: appending moves the list
		QFileInfoList list = QDir(cur.path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name | QDir::DirsFirst);
		foreach(QFileInfo inf, list) {
			if (vf->nodes >= VFS_MAXNODES) {
				drop.count = true;
				return;
			}
			unsigned int mtime = inf.lastModified().toMSecsSinceEpoch() / 1000;
			QByteArray name = inf.fileName().toUtf8();
			int idx;
			if (inf.isDir()) {
				if (cur.depth >= VFS_MAXDEPTH) {
					drop.deep++;
					continue;
				}
				idx = vfat_add(vf, cur.idx, name.constData(), NULL, 0, mtime, 1);
				if (idx >= 0) queue.append({idx, inf.absoluteFilePath(), cur.depth + 1});
			} else if (inf.isFile()) {
				if (inf.size() > VF_MAXFILE) {
					drop.big++;
					continue;
				}
				QByteArray host = inf.absoluteFilePath().toLocal8Bit();
				idx = vfat_add(vf, cur.idx, name.constData(), host.constData(), inf.size(), mtime, 0);
			} else {
				continue;
			}
			if (idx == VF_FULL) drop.full++;
			else if (idx < 0) return;
		}
	}
}

vFat* vfat_scan(const QString& path) {
	QFileInfo inf(path);
	if (!inf.isDir() || !inf.isReadable()) return NULL;
	vFat* vf = vfat_create();
	if (!vf) return NULL;
	unsigned int mtime = inf.lastModified().toMSecsSinceEpoch() / 1000;
	vfsDrop drop;
	vfat_add(vf, -1, "", NULL, 0, mtime, 1);		// root
	vfat_scan_tree(vf, path, drop);
	QByteArray lpath = path.toUtf8();
	QStringList why;
	if (drop.count) why << QString("everything past %0 entries").arg(VFS_MAXNODES);
	if (drop.deep) why << QString("%0 folders deeper than %1").arg(drop.deep).arg(VFS_MAXDEPTH);
	if (drop.full) why << QString("%0 names past the %1 entries of their folder").arg(drop.full).arg(VF_MAXDENTS);
	if (drop.big) why << QString("%0 files of 4G and over").arg(drop.big);
	if (!why.isEmpty()) {
		xlog(XLG_DISK, XLL_WARN, "folder %s: %i entries taken, left out %s", lpath.constData(), vf->nodes - 1, qPrintable(why.join(", ")));
		if (conf.zx) conf.zx->msg = (char*)" folder: not all of it fits ";
	}
	if (!vfat_build(vf, 0)) {
		xlog(XLG_DISK, XLL_WARN, "folder %s: more than a 32G disk holds, not mounted", lpath.constData());
		if (conf.zx) conf.zx->msg = (char*)" folder too big for a disk ";
		vfat_free(vf);
		return NULL;
	}
	return vf;
}

void sdc_mount(SDCard* sdc, const QString& path) {
	if (!sdc) return;
	if (path.isEmpty()) {
		sdcSetImage(sdc, "");
	} else if (QFileInfo(path).isDir()) {
		sdcSetFolder(sdc, path.toLocal8Bit().constData(), vfat_scan(path));
	} else {
		sdcSetImage(sdc, path.toLocal8Bit().constData());
	}
}

void sdc_remount(SDCard* sdc) {
	if (!sdc || !sdc->image) return;
	QString path = QString::fromLocal8Bit(sdc->image);	// own the path: mounting frees it
	sdc_mount(sdc, path);
}

void ide_mount(IDE* ide, int dev, const QString& path) {
	if (!ide) return;
	if (path.isEmpty()) {
		ideSetImage(ide, dev, "");
	} else if (QFileInfo(path).isDir()) {
		ideSetFolder(ide, dev, path.toLocal8Bit().constData(), vfat_scan(path));
	} else {
		ideSetImage(ide, dev, path.toLocal8Bit().constData());
	}
}

void ide_remount(IDE* ide) {
	if (!ide) return;
	QString mpath = ide->master->image ? QString::fromLocal8Bit(ide->master->image) : QString();
	QString spath = ide->slave->image ? QString::fromLocal8Bit(ide->slave->image) : QString();
	if (!mpath.isEmpty()) ide_mount(ide, IDE_MASTER, mpath);
	if (!spath.isEmpty()) ide_mount(ide, IDE_SLAVE, spath);
}
