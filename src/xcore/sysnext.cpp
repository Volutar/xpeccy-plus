#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include "sysnext.h"
#include "vfat_scan.h"
#include "xcore.h"
#include "../libxpeccy/defines.h"

#define SN_REPO		"https://gitlab.com/thesmog358/tbblue"
#define SN_PROJECT	"thesmog358%2Ftbblue"
#define SN_TAGPFX	"system/next_"

// The releases known here. A release is checked by the tree it unpacks to, which
// stays the same whatever packed it; GitLab builds its archives on request, so the
// archive's own checksum is only the quick way to the same answer.
static const struct {
	const char* release;
	const char* archiveSha;
	const char* treeSha;
	qint64 bytes;
} snPins[] = {
	{"24.11", "10b7e2c79544f5d5ae5c6e562139e6b8c33b03a0c204ac35d45e00c699eb4e66",
		"968ace3457111d7d9523365dafad1e15a9452a30dfa2328f075e18267bbead6d", 53574377},
};

QString sn_repo_url() {
	return SN_REPO;
}

QString sn_tags_url() {
	return "https://gitlab.com/api/v4/projects/" SN_PROJECT "/repository/tags?per_page=100&search=%5E"
		+ QString(SN_TAGPFX).replace("/", "%2F");
}

QString sn_archive_name(const snRelease& rel) {
	return "tbblue-" + QString(rel.tag).replace('/', '-') + ".tar.gz";
}

QString sn_archive_url(const snRelease& rel) {
	return SN_REPO "/-/archive/" + rel.tag + "/" + sn_archive_name(rel);
}

// newest first: 24.11 before 24.2
static bool sn_newer(const snRelease& a, const snRelease& b) {
	QStringList pa = a.release.split('.');
	QStringList pb = b.release.split('.');
	for (int i = 0; i < qMax(pa.size(), pb.size()); i++) {
		int va = (i < pa.size()) ? pa[i].toInt() : 0;
		int vb = (i < pb.size()) ? pb[i].toInt() : 0;
		if (va != vb) return va > vb;
	}
	return a.release > b.release;
}

static snRelease* sn_find(QList<snRelease>* list, const QString& release) {
	for (snRelease& rel : *list)
		if (rel.release == release) return &rel;
	return nullptr;
}

static snRelease sn_bare(const QString& release) {
	snRelease rel;
	rel.release = release;
	rel.tag = SN_TAGPFX + release;
	return rel;
}

QString sn_cache_dir() {
	return QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/downloads/";
}

QList<snRelease> sn_releases() {
	QList<snRelease> list;
	for (const auto& pin : snPins) {
		snRelease rel = sn_bare(pin.release);
		rel.archiveSha = pin.archiveSha;
		rel.treeSha = pin.treeSha;
		rel.bytes = pin.bytes;
		list.append(rel);
	}
	// an archive downloaded before, then one beside the program, which wins: GitLab's,
	// by its own name, .tar.gz or .zip
	static QRegularExpression rx("^tbblue-system-next_([0-9][0-9A-Za-z.]*)\\.(tar\\.gz|zip)$");
	QString dirs[] = {sn_cache_dir(), QCoreApplication::applicationDirPath()};
	for (int d = 0; d < 2; d++) {
		bool cache = (d == 0);
		foreach(QFileInfo inf, QDir(dirs[d]).entryInfoList(QStringList() << "tbblue-system-next_*", QDir::Files, QDir::Name)) {
			QRegularExpressionMatch m = rx.match(inf.fileName());
			if (!m.hasMatch()) continue;
			snRelease* rel = sn_find(&list, m.captured(1));
			if (!rel) {
				list.append(sn_bare(m.captured(1)));
				rel = &list.last();
			}
			// in one folder the .tar.gz, the one whose own checksum may be known
			if (rel->local.isEmpty() || (rel->cached && !cache) || ((rel->cached == cache) && (m.captured(2) == "tar.gz"))) {
				rel->local = inf.absoluteFilePath();
				rel->cached = cache;
			}
		}
	}
	std::sort(list.begin(), list.end(), sn_newer);
	return list;
}

void sn_add_tags(QList<snRelease>* list, const QByteArray& json) {
	QJsonDocument doc = QJsonDocument::fromJson(json);
	if (!doc.isArray()) return;
	foreach(QJsonValue val, doc.array()) {
		QString tag = val.toObject().value("name").toString();
		if (!tag.startsWith(SN_TAGPFX)) continue;
		QString release = tag.mid(strlen(SN_TAGPFX));
		if (release.isEmpty() || sn_find(list, release)) continue;
		list->append(sn_bare(release));
	}
	std::sort(list->begin(), list->end(), sn_newer);
}

QString sn_file_hash(const QString& path, std::function<bool()> tick) {
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) return QString();
	QCryptographicHash hash(QCryptographicHash::Sha256);
	while (!file.atEnd()) {
		hash.addData(file.read(1 << 20));
		if (tick && !tick()) return QString();
	}
	return QString::fromLatin1(hash.result().toHex());
}

QString sn_tree_hash(const QString& dir, std::function<bool()> tick) {
	QList<QPair<QByteArray, QString> > rows;
	QDir root(dir);
	QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
	while (it.hasNext()) {
		QString path = it.next();
		QString sum = sn_file_hash(path, tick);
		if (sum.isEmpty()) return QString();
		rows.append(qMakePair(root.relativeFilePath(path).toUtf8(), sum));
	}
	// by the bytes of the path, as sort does in the C locale
	std::sort(rows.begin(), rows.end(), [](const QPair<QByteArray, QString>& a, const QPair<QByteArray, QString>& b) {
		return a.first < b.first;
	});
	QCryptographicHash hash(QCryptographicHash::Sha256);
	for (const auto& row : rows)
		hash.addData(row.second.toLatin1() + "  " + row.first + "\n");
	return QString::fromLatin1(hash.result().toHex());
}

QString sn_unpacked_root(const QString& dir) {
	QDir d(dir);
	QStringList files = d.entryList(QDir::Files | QDir::Hidden | QDir::System);
	QStringList dirs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
	if (files.isEmpty() && (dirs.size() == 1)) return d.absoluteFilePath(dirs.first());
	return d.absolutePath();
}

static FILE* sn_open(const QString& path, const char* mode) {
#ifdef _WIN32
	return _wfopen(path.toStdWString().c_str(), QString(mode).toStdWString().c_str());
#else
	return fopen(QFile::encodeName(path).constData(), mode);
#endif
}

// Only the sectors with something in them are written and the file is sparse where
// the system allows, so a big card takes on the disk only what it holds.
bool sn_make_image(const QString& dir, const QString& file, qint64 size, QString* err, std::function<bool(int)> tick) {
	vFat* vf = vfat_scan_sized(dir, (unsigned int)qMin<qint64>(size / VF_SECSIZE, 0xffffffffLL));
	if (!vf) {
		*err = "The files do not fit on a card of this size";
		return false;
	}
	FILE* out = sn_open(file, "wb");
	if (!out) {
		vfat_free(vf);
		*err = "Cannot write " + QDir::toNativeSeparators(file);
		return false;
	}
#ifdef _WIN32
	DWORD got;
	DeviceIoControl((HANDLE)_get_osfhandle(_fileno(out)), FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL);
#endif
	static const unsigned char zero[VF_SECSIZE] = {0};
	unsigned char buf[VF_SECSIZE];
	bool ok = true;
	for (unsigned int lba = 0; ok && (lba < vf->volume); lba++) {
		if (!vfat_read(vf, lba, buf)) {
			*err = "A file of the release cannot be read";
			ok = false;
		} else if (memcmp(buf, zero, VF_SECSIZE)) {
			if (x_fseek(out, (long long)lba * VF_SECSIZE, SEEK_SET) || (fwrite(buf, VF_SECSIZE, 1, out) != 1)) {
				*err = "The disk is full";
				ok = false;
			}
		}
		if (ok && !(lba & 0x3fff) && tick && !tick(int(lba * 100.0 / vf->volume))) {
			*err = "Stopped";
			ok = false;
		}
	}
	long long bytes = (long long)vf->volume * VF_SECSIZE;
	vfat_free(vf);
	if (ok) {
		fflush(out);
#ifdef _WIN32
		// not _chsize_s, which writes the zeros out and takes the room the sparse file saves
		LARGE_INTEGER end;
		end.QuadPart = bytes;
		HANDLE h = (HANDLE)_get_osfhandle(_fileno(out));
		ok = SetFilePointerEx(h, end, NULL, FILE_BEGIN) && SetEndOfFile(h);
#else
		ok = (ftruncate(fileno(out), bytes) == 0);
#endif
		if (!ok) *err = "The disk is full";
	}
	if (fclose(out) && ok) {
		*err = "The disk is full";
		ok = false;
	}
	if (!ok) QFile::remove(file);
	return ok;
}

bool sn_card_ok(const QString& path) {
	static const char* rom = "machines/next/enNextZX.rom";
	QFileInfo inf(path);
	if (inf.isDir()) return QFileInfo(path + "/" + rom).isFile();
	if (!inf.isFile()) return false;
	SDCard* sdc = sdcCreate();
	if (!sdc) return false;
	sdcSetImage(sdc, path.toLocal8Bit().constData());
	bool ok = sdc_has_file(sdc, rom) != 0;
	sdcDestroy(sdc);
	return ok;
}

// == System/Next for programs from elsewhere ==
// <config>/next-system/<release>/ holds a release as its archive has it, with <release>.ok
// beside it once it was checked. A program's card is made of it and never writes there.

static QString sn_system_root() {
	return QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/next-system";
}

static bool sn_system_ready(const QString& dir) {
	return QFileInfo(dir + ".ok").isFile() && QFileInfo(dir + "/machines/next/enNextZX.rom").isFile();
}

// the newest release unpacked and checked, empty if none
static QString sn_system_newest(QString* release) {
	QList<snRelease> got;
	foreach(QFileInfo inf, QDir(sn_system_root()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		if (sn_system_ready(inf.absoluteFilePath())) got.append(sn_bare(inf.fileName()));
	}
	if (got.isEmpty()) return QString();
	std::sort(got.begin(), got.end(), sn_newer);
	*release = got.first().release;
	return sn_system_root() + "/" + got.first().release;
}

// a release's archive unpacked into its folder there, the tree checked when it is known
static QString sn_system_make(const snRelease& rel, QString* err) {
	QString dir = sn_system_root() + "/" + rel.release;
	QString part = dir + ".part";
	QDir(part).removeRecursively();
	QDir(dir).removeRecursively();		// not checked: left by a run that stopped short
	QFile::remove(dir + ".ok");
	qint64 start = QDateTime::currentMSecsSinceEpoch();
	bool known = !rel.archiveSha.isEmpty() && rel.local.endsWith(".tar.gz") && (sn_file_hash(rel.local) == rel.archiveSha);
	bool bad = !sn_unpack_archive(rel.local, part, err);
	QString root = sn_unpacked_root(part);
	QString sum;
	if (bad) {
		// said why in *err
	} else if (!QFileInfo(root + "/machines/next/enNextZX.rom").isFile()) {
		*err = QDir::toNativeSeparators(rel.local) + " is not a System/Next release";
		bad = true;
	} else if (!known && rel.checked() && ((sum = sn_tree_hash(root)) != rel.treeSha)) {
		*err = QString("The files of %0 are not those of System/Next %1").arg(QDir::toNativeSeparators(rel.local), rel.release);
		bad = true;
	} else if (!QDir().rename(root, dir)) {
		*err = "Cannot write into " + QDir::toNativeSeparators(sn_system_root());
	} else {
		QFile ok(dir + ".ok");
		if (ok.open(QFile::WriteOnly)) ok.write((known ? rel.treeSha : sum.isEmpty() ? QString("unchecked") : sum).toLatin1() + "\n");
		if (!ok.isOpen()) *err = "Cannot write into " + QDir::toNativeSeparators(sn_system_root());
	}
	QDir(part).removeRecursively();
	// a download that is not the release goes, so the next try downloads it again
	if (bad && rel.cached) QFile::remove(rel.local);
	if (!err->isEmpty()) {
		QDir(dir).removeRecursively();
		return QString();
	}
	xlog(XLG_FILE, XLL_INFO, "next: System/Next %s unpacked from %s in %lli ms", rel.release.toUtf8().constData(),
		rel.local.toUtf8().constData(), QDateTime::currentMSecsSinceEpoch() - start);
	return dir;
}

QString sn_system_dir(QString* err) {
	err->clear();
	if (!conf.path.nextSystem.empty()) {
		QString dir = QString::fromLocal8Bit(conf.path.nextSystem.c_str());
		if (QFileInfo(dir + "/machines/next/enNextZX.rom").isFile()) return dir;
		*err = "There is no System/Next in " + QDir::toNativeSeparators(dir);
		return QString();
	}
	QString have;
	QString ready = sn_system_newest(&have);
	const snRelease* arch = nullptr;
	QList<snRelease> rels = sn_releases();
	for (const snRelease& rel : rels) {
		if (!rel.local.isEmpty()) {
			arch = &rel;
			break;
		}
	}
	if (!ready.isEmpty() && (!arch || !sn_newer(*arch, sn_bare(have)))) return ready;
	if (arch) {
		QString made = sn_system_make(*arch, err);
		if (!made.isEmpty()) return made;
		xlog(XLG_FILE, XLL_WARN, "next: %s", err->toUtf8().constData());
	}
	if (!ready.isEmpty()) {
		err->clear();
		return ready;
	}
	// last: a folder there may be someone's card, booted and changed since
	QString beside = QString::fromLocal8Bit(conf.path.sysnextDir.c_str());
	if (!beside.isEmpty() && QFileInfo(beside + "/machines/next/enNextZX.rom").isFile()) {
		err->clear();
		return beside;
	}
	if (err->isEmpty()) *err = "There is no System/Next release here to start it from";
	return QString();
}

// where a folder of the settings really is, asked every time (a junction may be repointed)
static QString sn_real_of(const std::string& path) {
	return vfat_real_dir(QString::fromLocal8Bit(path.c_str()));
}

// compared where they really are: a junction or subst of the config's folder is the same one
bool sn_system_is(const QString& path, bool beside) {
	if (!QFileInfo(path).isDir()) return false;
	QString real = vfat_real_dir(path);
	if (!conf.path.nextSystem.empty() && (real == sn_real_of(conf.path.nextSystem)))
		return true;
	if (beside && !conf.path.sysnextDir.empty() && (real == sn_real_of(conf.path.sysnextDir)))
		return true;
	// in this config's next-system/ or another's: checked, its .ok beside it - beside the name
	// it has there (a release that is itself a junction) or beside where it really is
	return sn_system_ready(path) || sn_system_ready(real);
}

QString sn_default_path(bool image) {
	QString dir = QString::fromLocal8Bit(conf.path.confDir.c_str());
	return QDir::toNativeSeparators(QDir::cleanPath(dir + (image ? "/next-sd.img" : "/next-sd")));
}
