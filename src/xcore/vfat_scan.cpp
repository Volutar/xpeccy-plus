// GetFinalPathNameByHandleW is Vista's; a 32-bit MinGW defaults below it (Qt 5 needs Windows 7)
#if defined(_WIN32) && (!defined(_WIN32_WINNT) || (_WIN32_WINNT < 0x0600))
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QStringList>
#include <QSet>
#include <QMutex>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#else
#include <stdio.h>
#endif

#include "vfat_scan.h"
#include "xcore.h"
#include "pacing.h"
#include "../libxpeccy/xlog.h"

// Host folder -> synthetic FAT32 volume. The scan and the host side of the guest's
// writes live here so libxpeccy stays free of platform code; vfat.c only ever sees
// a tree and calls through vfHost.

#define VFS_MAXDEPTH	16		// deeper folders are left out
#define VFS_MAXNODES	200000		// and so is anything past this many entries: the scan holds the gui
#define VFS_ROOM	(256u * 2048)	// a folder taking writes has this much free room at least, in sectors
#define VFS_SLOW_NS	1000000000LL	// a read this long is said while it goes on (vfat_scan_notify)
#define VFS_SLOW_EVERY	1024		// entries between looks at the clock

static void (*vfsSlow)(const QString&, bool) = NULL;

void vfat_scan_notify(void (*hook)(const QString& path, bool on)) {
	vfsSlow = hook;
}

// == the host side, all paths UTF-8 ==

static QString vfh_str(const char* path) {
	return QString::fromUtf8(path);
}

static FILE* vfh_open(void*, const char* path, const char* mode) {
#ifdef _WIN32
	return _wfopen(vfh_str(path).toStdWString().c_str(), QString(mode).toStdWString().c_str());
#else
	return fopen(QFile::encodeName(vfh_str(path)).constData(), mode);
#endif
}

static int vfh_mkdir(void*, const char* path) {
	return QDir().mkdir(vfh_str(path)) ? 0 : -1;
}

#ifdef _WIN32
// Qt takes any path ending in ".lnk" for a shell shortcut and answers for its target, even with
// no file there: whether a file exists, its size and time, and renames are asked of Windows.
// The \\?\ form keeps paths past MAX_PATH working, as Qt has them.
static std::wstring vfh_wide(const QString& path) {
	QString p = QDir::toNativeSeparators(QDir::cleanPath(path));
	if (p.startsWith("\\\\") && !p.startsWith("\\\\?\\")) p = "\\\\?\\UNC\\" + p.mid(2);
	else if ((p.size() > 2) && (p[1] == ':') && (p[2] == '\\')) p = "\\\\?\\" + p;
	return p.toStdWString();
}

static bool vfh_attr(const QString& path, WIN32_FILE_ATTRIBUTE_DATA* d) {
	return GetFileAttributesExW(vfh_wide(path).c_str(), GetFileExInfoStandard, d) != 0;
}

// in seconds, cut as QFileInfo::lastModified() gives them: a host file's time is compared
// with the one the scan took
static long long vfh_mtime(const WIN32_FILE_ATTRIBUTE_DATA& d) {
	unsigned long long t = ((unsigned long long)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
	return ((long long)(t / 10000) - 11644473600000LL) / 1000;
}

// a ".lnk" Qt calls a link that is a plain file on the disk - a link NextZXOS made, or a
// Windows shortcut: on the card it is the file it is
static bool vfs_lnk_file(const QFileInfo& inf, long long* size, long long* mtime) {
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!inf.fileName().endsWith(".lnk", Qt::CaseInsensitive) || !vfh_attr(inf.absoluteFilePath(), &d)) return false;
	if (d.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	*size = ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
	*mtime = vfh_mtime(d);
	return true;
}
#endif

static int vfh_kind(void*, const char* path) {
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!vfh_attr(vfh_str(path), &d)) return 0;
	return (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 2 : 1;
#else
	QFileInfo inf(vfh_str(path));
	if (!inf.exists() && !inf.isSymLink()) return 0;
	return inf.isDir() ? 2 : 1;
#endif
}

static int vfh_rename(void* ctx, const char* from, const char* to) {
	if (vfh_kind(ctx, to)) return -1;		// a plain rename() would take its place
#ifdef _WIN32
	return MoveFileW(vfh_wide(vfh_str(from)).c_str(), vfh_wide(vfh_str(to)).c_str()) ? 0 : -1;
#else
	return QDir().rename(vfh_str(from), vfh_str(to)) ? 0 : -1;
#endif
}

static int vfh_replace(void*, const char* from, const char* to) {
#ifdef _WIN32
	return MoveFileExW(vfh_str(from).toStdWString().c_str(), vfh_str(to).toStdWString().c_str(),
		MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
	return ::rename(QFile::encodeName(vfh_str(from)).constData(), QFile::encodeName(vfh_str(to)).constData());
#endif
}

static int vfh_remove(void*, const char* path, int isdir) {
	QString p = vfh_str(path);
	if (conf.fold.trash) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
#ifdef _WIN32
		// the trash is a shell operation, which wants COM on the thread asking
		static thread_local bool com = false;
		if (!com) {
			CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
			com = true;
		}
#endif
		if (QFile::moveToTrash(p)) return 0;
#endif
		// no trash there: the file stays, and comes back on the next read of the folder
		xlog(XLG_DISK, XLL_WARN, "folder: %s could not go to the trash, kept", path);
		return -1;
	}
	return (isdir ? QDir().rmdir(p) : QFile::remove(p)) ? 0 : -1;
}

static int vfh_truncate(void*, const char* path, long long size) {
	QFile file(vfh_str(path));
	return file.resize(size) ? 0 : -1;
}

static int vfh_empty(void*, const char* path) {
	QDir dir(vfh_str(path));
	return dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).isEmpty() ? 1 : 0;
}

static void vfh_settime(void*, const char* path, unsigned int mtime) {
	QFile file(vfh_str(path));
	if (file.open(QIODevice::ReadWrite))
		file.setFileTime(QDateTime::fromMSecsSinceEpoch((qint64)mtime * 1000), QFileDevice::FileModificationTime);
}

static int vfh_stat(void*, const char* path, long long* size, long long* mtime) {
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA d;
	if (!vfh_attr(vfh_str(path), &d)) return -1;
	*size = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0 : (((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow);
	*mtime = vfh_mtime(d);
#else
	QFileInfo inf(vfh_str(path));
	if (!inf.exists()) return -1;
	*size = inf.size();
	*mtime = inf.lastModified().toMSecsSinceEpoch() / 1000;	// as the scan takes it
#endif
	return 0;
}

static const vfHost vfs_host = {
	NULL, vfh_open, vfh_mkdir, vfh_rename, vfh_replace, vfh_remove, vfh_truncate, vfh_kind, vfh_empty, vfh_settime, vfh_stat
};

// == the scan ==

// what a scan had to leave out
struct vfsDrop {
	int deep = 0;			// folders past VFS_MAXDEPTH
	int full = 0;			// entries past VF_MAXDENTS in their folder
	int big = 0;			// files too big for FAT
	bool count = false;		// stopped at VFS_MAXNODES
	unsigned long long bytes = 0;	// what the files hold
	long long start = 0;		// when the read began
	bool slow = false;		// it took long last time
	bool told = false;		// ...and it has been said to take long
	unsigned looked = 0;		// entries seen, taken or not
};

// a read past VFS_SLOW_NS is said once; a folder that took that long last time, at once
// (a card image is made of a folder on a thread of its own)
static QSet<QString> vfsSlowRoots;
static QMutex vfsSlowLock;

static void vfs_slow_check(const QString& root, vfsDrop& drop) {
	if (!vfsSlow || drop.told) return;
	if (!drop.slow && (paceClockNs() - drop.start <= VFS_SLOW_NS)) return;
	drop.told = true;
	vfsSlow(root, true);
}

struct vfsDir {
	int idx;
	QString path;
	int depth;
};

// A file of the System/Next folder beside the program that is not shown on a card that takes no
// writes: its autoexec.1st runs the welcome pages, which such a card can never switch
// off, so it boots to the menu
static bool vfs_hidden(const QFileInfo& inf) {
	if (conf.path.sysnextDir.empty()) return false;
	static const QString rel = "nextzxos/autoexec.1st";
	QString hide = QDir(QString::fromLocal8Bit(conf.path.sysnextDir.c_str())).absoluteFilePath(rel);
	return QFileInfo(hide) == inf;
}

// where a folder really is, junctions followed, in one spelling to compare: Qt's
// canonical path does not see through a junction
static QString real_dir(const QString& path) {
#ifdef _WIN32
	HANDLE h = CreateFileW((const wchar_t*)QDir::toNativeSeparators(path).utf16(), 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (h != INVALID_HANDLE_VALUE) {
		wchar_t buf[4096];
		DWORD n = GetFinalPathNameByHandleW(h, buf, 4096, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
		CloseHandle(h);
		if (n && (n < 4096)) {
			QString res = QString::fromWCharArray(buf, n);
			if (res.startsWith("\\\\?\\UNC\\")) res = "\\\\" + res.mid(8);
			else if (res.startsWith("\\\\?\\")) res = res.mid(4);
			return QDir::fromNativeSeparators(res).toLower();
		}
	}
	return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
#else
	QString res = QFileInfo(path).canonicalFilePath();
	return res.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : res;
#endif
}

// breadth first, so whatever a limit cuts off is the deepest level and never
// the loader in the root. A folder reached a second time - a junction back up the tree,
// two to one place - is taken once, and a junction into the tree itself not at all:
// deleting one copy would take the file from the other.
static void vfat_scan_tree(vFat* vf, const QString& root, const QString& sub, const QString& subName, const QString* only, bool writable, vfsDrop& drop) {
	QList<vfsDir> queue;
	QSet<QString> seen;
	QString top = real_dir(root);
	seen.insert(top);
	queue.append({0, root, 0});
	if (!sub.isEmpty()) {
		// a folder from elsewhere: the guest sees it in the root, the host side leaves it be
		QByteArray name = subName.toUtf8();
		int idx = vfat_add(vf, 0, name.constData(), 0, QFileInfo(sub).lastModified().toMSecsSinceEpoch() / 1000, 1);
		if (idx >= 0) {
			vfat_pin(vf, idx, QDir::cleanPath(QFileInfo(sub).absoluteFilePath()).toUtf8().constData());
			if (!only) {
				queue.append({idx, sub, 1});
			} else {
				// the folder is not listed: the one file is asked for by its name
				vf->node[idx].cut = 1;
				QFileInfo inf(QDir(sub).filePath(*only));
				if (!only->isEmpty() && inf.isFile() && (inf.size() <= VF_MAXFILE)) {
					if (vfat_add(vf, idx, only->toUtf8().constData(), (unsigned int)inf.size(), inf.lastModified().toMSecsSinceEpoch() / 1000, 0) >= 0)
						drop.bytes += inf.size();
				}
			}
		}
	}
	for (int q = 0; q < queue.size(); q++) {
		vfsDir cur = queue[q];			// a copy: appending moves the list
		QString curReal = real_dir(cur.path);
		// links are left out by hand, not by QDir::NoSymLinks: a ".lnk" file Qt calls one is
		// let through (System lets in what Qt takes for a link to nothing)
		vfs_slow_check(root, drop);		// one folder of thousands is a long listing of its own
		QFileInfoList list = QDir(cur.path).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::System, QDir::Name | QDir::DirsFirst);
		foreach(QFileInfo inf, list) {
			if (!(++drop.looked % VFS_SLOW_EVERY)) vfs_slow_check(root, drop);
			if (vf->nodes >= VFS_MAXNODES) {
				drop.count = true;
				return;
			}
			if (inf.fileName().startsWith(VF_TMPMARK)) {		// left by an interrupted sync
				char* back = (writable && !cur.idx) ? vfat_tmp_restore(vf, inf.fileName().toUtf8().constData()) : NULL;
				if (!back) continue;
				inf = QFileInfo(QDir(cur.path).filePath(QString::fromUtf8(back)));
				free(back);
			}
			long long lsize = 0, ltime = 0;
			bool lnk = false;
			if (inf.isSymLink()) {
#ifdef _WIN32
				lnk = vfs_lnk_file(inf, &lsize, &ltime);
#endif
				if (!lnk) continue;
			}
			unsigned int mtime = lnk ? (unsigned int)ltime : inf.lastModified().toMSecsSinceEpoch() / 1000;
			unsigned long long size = lnk ? (unsigned long long)lsize : (unsigned long long)inf.size();
			QByteArray name = inf.fileName().toUtf8();
			int idx;
			if (!lnk && inf.isDir()) {
				if (cur.depth >= VFS_MAXDEPTH) {
					drop.deep++;
					continue;
				}
				QString real = real_dir(inf.absoluteFilePath());
				if (seen.contains(real)) continue;
#ifdef _WIN32
				bool link = (real != (curReal + "/" + inf.fileName()).toLower());
#else
				bool link = (real != curReal + "/" + inf.fileName());
#endif
				if (link && real.startsWith(top + "/")) continue;
				seen.insert(real);
				idx = vfat_add(vf, cur.idx, name.constData(), 0, mtime, 1);
				if (idx >= 0) queue.append({idx, inf.absoluteFilePath(), cur.depth + 1});
			} else if (lnk || inf.isFile()) {
				if ((cur.depth == 1) && !writable && vfs_hidden(inf)) continue;
				if (size > VF_MAXFILE) {
					drop.big++;
					continue;
				}
				idx = vfat_add(vf, cur.idx, name.constData(), (unsigned int)size, mtime, 0);
				if (idx >= 0) drop.bytes += size;
			} else {
				continue;
			}
			if (idx == VF_FULL) drop.full++;
			else if (idx < 0) return;
		}
	}
}

// sub: one more folder, put in the root as subName (a name the root does not have), with
// only that file of it if only is given (none if it is empty); sectors: the volume's size,
// 0 for what it holds plus room to write in when writable. build false: the tree alone, and
// *cut says whether the entries' limit cut it short
static vFat* vfat_scan_in(const QString& path, bool writable, const QString& sub, const QString& subName, const QString* only, unsigned int sectors,
		bool build = true, bool* cut = NULL) {
	QFileInfo inf(path);
	if (!inf.isDir() || !inf.isReadable()) return NULL;
	vFat* vf = vfat_create();
	if (!vf) return NULL;
	QByteArray lpath = QDir::cleanPath(inf.absoluteFilePath()).toUtf8();
	vfat_set_root(vf, lpath.constData(), &vfs_host);
	unsigned int mtime = inf.lastModified().toMSecsSinceEpoch() / 1000;
	vfsDrop drop;
	drop.start = paceClockNs();
	{
		QMutexLocker lock(&vfsSlowLock);
		drop.slow = vfsSlowRoots.contains(path);
	}
	vfat_add(vf, -1, "", 0, mtime, 1);		// root
	vfat_scan_tree(vf, path, sub, subName, only, writable, drop);
	{
		QMutexLocker lock(&vfsSlowLock);
		if (paceClockNs() - drop.start > VFS_SLOW_NS) vfsSlowRoots.insert(path);
		else vfsSlowRoots.remove(path);
	}
	if (drop.told) vfsSlow(path, false);
	QStringList why;
	if (drop.count) why << QString("everything past %0 entries").arg(VFS_MAXNODES);
	if (drop.deep) why << QString("%0 folders deeper than %1").arg(drop.deep).arg(VFS_MAXDEPTH);
	if (drop.full) why << QString("%0 names past the %1 entries of their folder").arg(drop.full).arg(VF_MAXDENTS);
	if (drop.big) why << QString("%0 files of 4G and over").arg(drop.big);
	if (!why.isEmpty()) {
		xlog(XLG_DISK, XLL_WARN, "folder %s: %i entries taken, left out %s", lpath.constData(), vf->nodes - 1, qPrintable(why.join(", ")));
		if (conf.zx && build) conf.zx->msg = (char*)" folder: not all of it fits ";
	}
	if (!build) {
		if (cut) *cut = drop.count;
		return vf;
	}
	// room to write in: as much again as it holds, and VFS_ROOM at least
	unsigned long long minsec = sectors;
	if (!sectors && writable) {
		minsec = (drop.bytes / VF_SECSIZE) * 2 + VFS_ROOM;
		if (minsec > 0x7fffffffull) minsec = 0x7fffffffull;
	}
	if (!vfat_build(vf, (unsigned int)minsec)) {
		xlog(XLG_DISK, XLL_WARN, "folder %s: more than a 32G disk holds, not mounted", lpath.constData());
		if (conf.zx) conf.zx->msg = (char*)" folder too big for a disk ";
		vfat_free(vf);
		return NULL;
	}
	vf->writable = writable ? 1 : 0;
	vf->hold = (writable && !conf.fold.asked) ? 1 : 0;	// the first write asks first (MainWin::folderAsk)
	xlog(XLG_DISK, XLL_DEBUG, "folder %s: %i entries, %s", lpath.constData(), vf->nodes - 1, writable ? "read/write" : "read only");
	return vf;
}

vFat* vfat_scan(const QString& path, bool writable, const QString& sub, const QString& subName, const QString* only) {
	return vfat_scan_in(path, writable, sub, subName, only, 0);
}

// for an image made of the folder: laid out as a volume that takes writes
vFat* vfat_scan_sized(const QString& path, unsigned int sectors) {
	return vfat_scan_in(path, true, QString(), QString(), NULL, sectors);
}

// the Next's own card, made in the Next SD Card window: it takes writes and asks nothing
static bool vfs_own(const QString& path) {
	if (!conf.next.own || conf.next.card.empty()) return false;
	return QFileInfo(path) == QFileInfo(QString::fromLocal8Bit(conf.next.card.c_str()));
}

// every volume a folder serves, NULL where there is none
static void fold_volumes(Computer* comp, vFat** vf) {
	vf[0] = comp->sdc->vfat;
	vf[1] = comp->div->card[0]->vfat;
	vf[2] = comp->div->card[1]->vfat;
	vf[3] = comp->ide->master->vfat;
	vf[4] = comp->ide->slave->vfat;
}

vFat* fold_waiting(Computer* comp) {
	vFat* vf[5];
	fold_volumes(comp, vf);
	for (int i = 0; i < 5; i++) {
		if (vfat_waiting(vf[i])) return vf[i];
	}
	return NULL;
}

QString fold_news(Computer* comp) {
	vFat* vf[5];
	fold_volumes(comp, vf);
	for (int i = 0; i < 5; i++) {
		const char* why = "";
		int errors = 0;
		int k = vfat_news(vf[i], &why, &errors);
		if (!k) continue;
		QString root = QDir::toNativeSeparators(QString::fromUtf8(vf[i]->root ? vf[i]->root : ""));
		if (k == 1)
			return QString("The folder\n%0\nno longer follows the machine: %1.\n\n"
				"What the machine writes there stays in memory, and is lost when the folder is "
				"read again or the emulator closes.").arg(root).arg(why);
		return QString("Not all of what the machine wrote reached the folder\n%0\n\n%1\n\n"
			"Content that could not be put in its place is kept in the folder's top level, "
			"with (recovered) in its name.").arg(root).arg(QString::fromUtf8(why));
	}
	return QString();
}

void fold_release(Computer* comp, bool allow) {
	vFat* vf[5];
	fold_volumes(comp, vf);
	for (int i = 0; i < 5; i++)
		vfat_release(vf[i], allow ? 1 : 0);
}

void fold_read_only(Computer* comp) {
	vFat* vf[5];
	fold_volumes(comp, vf);
	for (int i = 0; i < 5; i++) {
		if (vf[i] && !vfs_own(QString::fromUtf8(vf[i]->root ? vf[i]->root : "")))
			vfat_read_only(vf[i]);
	}
}

// What the guest wrote to the volume going goes to its folder before the folder is read
// again: the machine is held for it, so the quiet spell that would carry it never comes.
void sdc_mount(SDCard* sdc, const QString& path) {
	if (!sdc) return;
	vfat_flush(sdc->vfat);
	if (path.isEmpty()) {
		sdcSetImage(sdc, "");
	} else if (QFileInfo(path).isDir()) {
		bool own = vfs_own(path);
		vFat* vf = vfat_scan(path, own || conf.fold.rw);
		if (vf && own) vf->hold = 0;
		sdcSetFolder(sdc, path.toLocal8Bit().constData(), vf);
	} else {
		sdcSetImage(sdc, path.toLocal8Bit().constData());
	}
}

// A folder of thousands of entries takes seconds to read: not for a machine that has no
// slot for it, nor for a card that is about to be taken out again
void sdc_mount_later(SDCard* sdc, const QString& path) {
	if (!sdc) return;
	if (!QFileInfo(path).isDir()) {
		sdc_mount(sdc, path);
		return;
	}
	vfat_flush(sdc->vfat);
	sdcSetUnread(sdc, path.toLocal8Bit().constData());
}

void sdc_remount(SDCard* sdc) {
	if (!sdc || !sdc->image) return;
	QString path = QString::fromLocal8Bit(sdc->image);	// own the path: mounting frees it
	sdc_mount(sdc, path);
}

bool sdc_mount_over(SDCard* sdc, const QString& sub, const QString& subName, const QString* only, int* entries) {
	if (!sdc || !sdc->image || !sdc->vfat) return false;
	QString path = QString::fromLocal8Bit(sdc->image);
	vFat* vf = vfat_scan(path, sdc->vfat->writable, sub, subName, only);
	if (!vf) return false;
	if (entries) *entries = vfat_pin_entries(vf, vfat_pin_node(vf, subName.toUtf8().constData()));
	vf->hold = sdc->vfat->hold;		// read again, not mounted anew: asked or not, as it was
	vf->frozen = sdc->vfat->frozen;		// and made read-only or not
	sdcSetFolder(sdc, path.toLocal8Bit().constData(), vf);
	return true;
}

void ide_mount(IDE* ide, int dev, const QString& path) {
	if (!ide) return;
	vfat_flush((dev == IDE_MASTER) ? ide->master->vfat : ide->slave->vfat);
	if (path.isEmpty()) {
		ideSetImage(ide, dev, "");
	} else if (QFileInfo(path).isDir()) {
		ideSetFolder(ide, dev, path.toLocal8Bit().constData(), vfat_scan(path, conf.fold.rw));
	} else {
		ideSetImage(ide, dev, path.toLocal8Bit().constData());
	}
}

// The folder a volume serves changed on the host, the machine running: what the guest wrote
// goes to the folder first, then only what the folder changed goes into the volume (vfat_reload)
QString fold_reload(vFat* vf, vfReload* r) {
	vfReload none;
	if (!r) r = &none;
	memset(r, 0x00, sizeof(vfReload));
	if (!vf || !vf->root) return QString();
	vfat_flush(vf);
	QString path = QString::fromUtf8(vf->root);
	bool cut = false;
	vFat* fresh = vfat_scan_in(path, vf->writable, QString(), QString(), NULL, 0, false, &cut);
	if (!fresh) return QString("The folder\n%0\ncannot be read.").arg(QDir::toNativeSeparators(path));
	if (cut) {
		vfat_free(fresh);
		return QString("The folder holds more than %0 entries: Eject it and open it again.").arg(VFS_MAXNODES);
	}
	int res = vfat_reload(vf, fresh, r);
	vfat_free(fresh);
	switch (res) {
		case VFR_OK:
			x_media_writes++;		// rewind cannot go back past it
			return QString();
		case VFR_WRITES:
			return QString("Not all of what the machine wrote has reached the folder yet; reloading would lose it.");
		case VFR_DETACHED:
			return QString("The folder no longer follows the machine: Eject it and open it again.");
		case VFR_FULL:
			x_media_writes++;		// some of it may have gone in
			return QString("Not all of the folder's changes fit on the volume: Eject it and open it again.");
	}
	return QString("There is no memory left to reload the folder.");
}

QString sdc_reload(SDCard* sdc, vfReload* r) {
	return fold_reload(sdc ? sdc->vfat : NULL, r);
}

QString ide_reload(IDE* ide, int dev, vfReload* r) {
	if (!ide) return QString();
	return fold_reload((dev == IDE_MASTER) ? ide->master->vfat : ide->slave->vfat, r);
}

void ide_remount(IDE* ide) {
	if (!ide) return;
	QString mpath = ide->master->image ? QString::fromLocal8Bit(ide->master->image) : QString();
	QString spath = ide->slave->image ? QString::fromLocal8Bit(ide->slave->image) : QString();
	if (!mpath.isEmpty()) ide_mount(ide, IDE_MASTER, mpath);
	if (!spath.isEmpty()) ide_mount(ide, IDE_SLAVE, spath);
}
