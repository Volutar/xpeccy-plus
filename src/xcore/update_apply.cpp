// Putting a new build in place of this one (update.h)

#include "update.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QSet>

QString up_file_sha(const QString& path) {
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) return QString();
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!hash.addData(&file)) return QString();
	return QString::fromLatin1(hash.result().toHex());
}

// sha256sum's format: "<64 hex>  path", or " *path" for a binary file
QMap<QString, QString> up_manifest(const QString& file) {
	QMap<QString, QString> map;
	QFile fl(file);
	if (!fl.open(QFile::ReadOnly)) return map;
	foreach(QByteArray raw, fl.readAll().split('\n')) {
		QString line = QString::fromUtf8(raw).trimmed();
		if (line.size() < 67) continue;
		QString sha = line.left(64).toLower();
		QString path = line.mid(64).trimmed();
		if (path.startsWith('*')) path.remove(0, 1);
		if (!path.isEmpty()) map.insert(path, sha);
	}
	return map;
}

static bool up_in_config(const QString& rel) {
	return rel.startsWith("config/");
}

QList<upStep> up_plan(const QString& app, const QString& stage) {
	QList<upStep> plan;
	QMap<QString, QString> old = up_manifest(app + "/" UP_MANIFEST);
	QDir sdir(stage);
	QSet<QString> fresh;
	QStringList files;
	QDirIterator it(stage, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
	while (it.hasNext()) files.append(sdir.relativeFilePath(it.next()));
	// no program there, no plan: an empty tree would have every old file dropped
	bool exe = false;
	foreach(QString rel, files) exe |= !rel.contains('/') && rel.endsWith(".exe", Qt::CaseInsensitive);
	if (!exe) return plan;
	files.sort();
	foreach(QString rel, files) {
		fresh.insert(rel);
		QString dst = app + "/" + rel;
		if (!QFileInfo::exists(dst)) {
			plan.append({UPS_ADD, rel});
			continue;
		}
		QString now = up_file_sha(dst);
		QString next = up_file_sha(stage + "/" + rel);
		if (now == next) continue;
		// the release has nothing new for a file the user changed: theirs stays, unsaid
		if (up_in_config(rel) && !old.value(rel).isEmpty() && (old.value(rel) == next)) continue;
		// in config/, a file is the release's only when a manifest says the release made it so
		if (!up_in_config(rel) || (!now.isEmpty() && (old.value(rel) == now))) {
			plan.append({UPS_REPLACE, rel});
		} else {
			plan.append({UPS_KEEP, rel});
		}
	}
	// the program's own files a release no longer ships, unchanged since it did
	for (auto i = old.constBegin(); i != old.constEnd(); ++i) {
		const QString& rel = i.key();
		if (fresh.contains(rel) || up_in_config(rel)) continue;
		QString dst = app + "/" + rel;
		if (QFileInfo::exists(dst) && (up_file_sha(dst) == i.value()))
			plan.append({UPS_DROP, rel});
	}
	return plan;
}

// a name beside rel for the old file, free of an earlier update's leftover
static QString up_aside(const QString& app, const QString& rel) {
	QString name = rel + ".old";
	for (int n = 1; QFileInfo::exists(app + "/" + name); n++) {
		if (QFile::remove(app + "/" + name)) break;
		name = QString("%0.old%1").arg(rel).arg(n);
	}
	return name;
}

namespace {
struct upJournal {
	QString app;
	QFile file;
	QList<QPair<QString, QString> > done;	// renames made, from -> to
	int stopAfter = -1;
	bool stopped = false;
	QString err;
	void line(const QString& text) {
		file.write(text.toUtf8() + "\n");
		file.flush();
	}
	bool move(const QString& from, const QString& to) {
		if ((stopAfter >= 0) && (done.size() >= stopAfter)) {
			stopped = true;
			err = "stopped";
			return false;
		}
		QDir().mkpath(QFileInfo(app + "/" + to).path());
		line("R\t" + from + "\t" + to);
		if (!QFile::rename(app + "/" + from, app + "/" + to)) {
			err = QString("Cannot move %0 to %1").arg(from, to);
			return false;
		}
		done.append(qMakePair(from, to));
		return true;
	}
};
}

bool up_apply(const QString& app, const QString& stage, const QString& version,
		const QList<upStep>& plan, QString* err, int stopAfter) {
	upJournal jr;
	jr.app = app;
	jr.stopAfter = stopAfter;
	jr.file.setFileName(app + "/" UP_JOURNAL);
	if (!jr.file.open(QFile::WriteOnly | QFile::Truncate)) {
		if (err) *err = "Cannot write in the program's folder";
		return false;
	}
	jr.line("update\t" + version);
	QString from = QDir(app).relativeFilePath(stage) + "/";
	QString keep = "config/update-" + version + "/";
	QStringList kept;
	bool ok = true;
	foreach(const upStep& st, plan) {
		switch (st.kind) {
			case UPS_ADD:
				ok = jr.move(from + st.rel, st.rel);
				break;
			case UPS_REPLACE:
				ok = jr.move(st.rel, up_aside(app, st.rel)) && jr.move(from + st.rel, st.rel);
				break;
			case UPS_KEEP:
				QFile::remove(app + "/" + keep + st.rel.mid(7));	// an earlier try's
				ok = jr.move(from + st.rel, keep + st.rel.mid(7));
				kept.append(st.rel);
				break;
			case UPS_DROP:
				ok = jr.move(st.rel, up_aside(app, st.rel));
				break;
		}
		if (!ok) break;
	}
	if (jr.stopped) {
		jr.file.close();
		if (err) *err = jr.err;
		return false;
	}
	if (!ok) {
		// put back what was moved, newest first
		for (int i = jr.done.size() - 1; i >= 0; i--)
			QFile::rename(app + "/" + jr.done[i].second, app + "/" + jr.done[i].first);
		jr.file.close();
		jr.file.remove();
		if (err) *err = jr.err;
		return false;
	}
	foreach(QString rel, kept) jr.line("KEEP\t" + rel);
	jr.line("DONE");
	jr.file.close();
	return true;
}

// .old files a running program held when last cleaned up
static void up_left(const QString& app, const QStringList& add, upResume* res) {
	QFile lf(app + "/" UP_LEFT);
	QStringList left = add;
	if (lf.open(QFile::ReadOnly)) {
		foreach(QByteArray raw, lf.readAll().split('\n')) {
			QString rel = QString::fromUtf8(raw).trimmed();
			if (!rel.isEmpty() && !left.contains(rel)) left.append(rel);
		}
		lf.close();
	}
	QStringList still;
	foreach(QString rel, left) {
		QString path = app + "/" + rel;
		if (QFileInfo(path).isDir()) QDir(path).removeRecursively();
		else QFile::remove(path);
		if (QFileInfo::exists(path)) still.append(rel);
	}
	if (still.isEmpty()) {
		lf.remove();
	} else if (lf.open(QFile::WriteOnly | QFile::Truncate)) {
		lf.write(still.join('\n').toUtf8() + "\n");
		lf.close();
	}
	if (res) res->failed = still;
}

upResume up_resume(const QString& app) {
	upResume res;
	QFile jf(app + "/" UP_JOURNAL);
	if (!jf.open(QFile::ReadOnly)) {
		up_left(app, QStringList(), NULL);
		return res;
	}
	QList<QPair<QString, QString> > moves;
	bool done = false;
	foreach(QByteArray raw, jf.readAll().split('\n')) {
		QStringList f = QString::fromUtf8(raw).trimmed().split('\t');
		if ((f[0] == "update") && (f.size() > 1)) res.version = f[1];
		if ((f[0] == "R") && (f.size() > 2)) moves.append(qMakePair(f[1], f[2]));
		if ((f[0] == "KEEP") && (f.size() > 1)) res.kept.append(f[1]);
		if (f[0] == "DONE") done = true;
	}
	jf.close();
	jf.remove();
	if (done) {
		res.state = UPR_DONE;
		// the unpacked build goes too: a file in it a running program holds, the next time
		QStringList old;
		for (auto& mv : moves) {
			if (QFileInfo(mv.second).suffix().startsWith("old")) old.append(mv.second);
		}
		old.append(UP_STAGE);
		up_left(app, old, &res);
	} else {
		// cut short: the renames undone, newest first; a line written before its rename
		// failed or happened, so the file is where either end says
		res.state = UPR_UNDONE;
		for (int i = moves.size() - 1; i >= 0; i--) {
			QString from = app + "/" + moves[i].first;
			QString to = app + "/" + moves[i].second;
			if (!QFileInfo::exists(to)) continue;
			if (!QFileInfo::exists(from) && QFile::rename(to, from)) {
				res.moved.append(moves[i].first);
			} else {
				res.failed.append(moves[i].first);
			}
		}
		// a DLL of the new build this program loaded stays in it until the program ends
		QStringList failed = res.failed;
		up_left(app, QStringList() << UP_STAGE, NULL);
		res.failed = failed;
	}
	return res;
}
