#include "saves.h"
#include "arcpath.h"
#include "xcore.h"
#include "../libxpeccy/xlog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <mutex>

// past this an image is a hard disk or a card, keyed by its path: reading
// gigabytes on every open is no way to find a folder
#define GAME_HASH_MAX	(16 << 20)

static const char* save_prefix[SAVE_KINDS] = {"quick", "exit", "auto"};

// what holds the game, GSRC_*: looked at once the media are in
enum {GSRC_NONE, GSRC_TAPE, GSRC_DISK, GSRC_LOOK};

static xGame game = {QString(), QString(), QString(), GSRC_NONE, 0, std::string()};	// image: as opened; hash: sha-1, hex

// what the game is set against: a user reset or a machine switch after it ends it
static void game_stamp() {
	game.resets = xUserResets.load();
	game.mac = conf.macId;
}

void game_restamp() {
	game_stamp();
}

bool game_stale() {
	return (game.resets != xUserResets.load()) || (game.mac != conf.macId);
}

QString saves_root() {
	if (!conf.saves.dir.isEmpty()) return conf.saves.dir;
	return QDir(QString::fromLocal8Bit(conf.path.confDir.c_str())).filePath("saves");
}

// a name any file system takes
static QString saves_clean(QString name) {
	for (QChar& ch : name) {
		if ((ch.unicode() < 32) || QString("<>:\"|?*\\/").contains(ch)) ch = '_';
	}
	name = name.trimmed();
	while (name.endsWith('.')) name.chop(1);
	if (name.length() > 60) name = name.left(60).trimmed();
	return name.isEmpty() ? QString("game") : name;
}

static QString game_hash_read(const QString& image) {
	QString err;
	QString local = arc_local(image, &err);
	QFile file(local);
	QCryptographicHash sha(QCryptographicHash::Sha1);
	if (!local.isEmpty() && (file.size() <= GAME_HASH_MAX) && file.open(QIODevice::ReadOnly)) {
		sha.addData(&file);
	} else {
		// a path, made to differ from any content's hash
		sha.addData(QByteArray("path:") + QDir::fromNativeSeparators(image).toUtf8());
	}
	return QString::fromLatin1(sha.result().toHex());
}

// the last hash worked out ahead, kept while its file is the same
static std::mutex game_ahead_mtx;
static struct {
	QString image;
	qint64 size;
	QDateTime time;
	QString hash;
} game_ahead;

static void game_file_stamp(const QString& image, qint64* size, QDateTime* time) {
	QString archive;
	const QFileInfo info(arc_split(image, &archive, nullptr) ? archive : image);
	*size = info.size();
	*time = info.lastModified();
}

void game_hash_ahead(const QString& image) {
	QString hash = game_hash_read(image);
	qint64 size;
	QDateTime time;
	game_file_stamp(image, &size, &time);
	std::lock_guard<std::mutex> hold(game_ahead_mtx);
	game_ahead.image = image;
	game_ahead.size = size;
	game_ahead.time = time;
	game_ahead.hash = hash;
}

static QString game_hash(const QString& image) {
	qint64 size;
	QDateTime time;
	game_file_stamp(image, &size, &time);
	{
		std::lock_guard<std::mutex> hold(game_ahead_mtx);
		if ((game_ahead.image == image) && (game_ahead.size == size) && (game_ahead.time == time))
			return game_ahead.hash;
	}
	return game_hash_read(image);
}

static QJsonObject saves_json(const QString& dir) {
	QFile file(dir + "/game.json");
	if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
	return QJsonDocument::fromJson(file.readAll()).object();
}

// the json of a save's game: the one beside it, else its folder's
static QJsonObject saves_game_json(const QString& save) {
	QFileInfo fi(save);
	QFile side(fi.path() + "/" + fi.completeBaseName() + ".json");
	if (side.open(QIODevice::ReadOnly))
		return QJsonDocument::fromJson(side.readAll()).object();
	return saves_json(fi.path());
}

// the game's name, or the machine's for a machine's folder; empty when it says neither
static QString saves_folder_name(const QJsonObject& obj) {
	if (!obj.value("hash").toString().isEmpty()) return obj.value("name").toString();
	std::string id = obj.value("machine").toString().toStdString();
	if (id.empty()) return QString();
	const xMachine* m = xm_find(id);
	return QString::fromLocal8Bit(m ? m->name.c_str() : id.c_str());
}

static void saves_json_write(const QString& dir, const QJsonObject& obj) {
	QFile file(dir + "/game.json");
	if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
		file.write(QJsonDocument(obj).toJson());
}

void game_set(const QString& image) {
	if (image.isEmpty()) {
		game_forget();
		return;
	}
	game.image = image;
	game.hash = game_hash(image);
	QString entry;
	arc_split(image, nullptr, &entry);
	game.name = saves_clean(QFileInfo(entry.isEmpty() ? image : entry).completeBaseName());
	game.src = GSRC_LOOK;
	game_stamp();
	// a folder that is there already learns this path too
	QString dir = saves_game_dir();
	if (!dir.isEmpty()) saves_dir(true);
}

// one file, however its path was written; a path no longer there by its text
bool same_image(const QString& a, const QString& b) {
	QString ca = arc_canonical(a);
	QString cb = arc_canonical(b);
	if (ca.isEmpty() || cb.isEmpty()) {
		ca = QDir::cleanPath(QDir::fromNativeSeparators(a));
		cb = QDir::cleanPath(QDir::fromNativeSeparators(b));
	}
	return ca.compare(cb, Qt::CaseInsensitive) == 0;
}

// By its path: a game writing to its own disk is still that game.
void game_follow(const QString& image) {
	if (image.isEmpty()) return;
	if (!game.image.isEmpty() && same_image(image, game.image)) return;
	game_set(image);
}

void game_set_save(const QString& save) {
	QJsonObject obj = saves_game_json(save);
	QJsonArray paths = obj.value("images").toArray();
	if (obj.value("hash").toString().isEmpty() || paths.isEmpty()) {
		game_forget();			// a machine's folder
		return;
	}
	game.image = paths.at(0).toString();
	game.hash = obj.value("hash").toString();
	game.name = obj.value("name").toString();
	game.src = GSRC_LOOK;
	game_stamp();
}

xGame game_now() {
	return game;
}

void game_put(const xGame& g) {
	game = g;
	game_stamp();
}

void game_forget() {
	game.image.clear();
	game.hash.clear();
	game.name.clear();
	game.src = GSRC_NONE;
	game_stamp();
}

// The tape in its place is the game's own next side: the image moves on, the hash, the
// name and the stamp stay, so game_follow finds it the same game when it starts
void game_tape_next(const QString& from, const QString& to) {
	game_side_next(from, to, false);
}

// the path as the drive or the deck has it, which game_check compares by its bytes
bool game_side_next(const QString& from, const QString& to, bool disk) {
	if (game.image.isEmpty() || from.isEmpty() || !same_image(from, game.image)) return false;
	const QString canon = arc_canonical(to);
	game.image = canon.isEmpty() ? to : canon;
	game.src = disk ? GSRC_DISK : GSRC_TAPE;
	return true;
}

// the tape or the disk it was opened as has left its place
void game_check(Computer* comp) {
	if (game.src == GSRC_LOOK) {
		QByteArray raw = game.image.toLocal8Bit();
		game.src = (comp->tape->path && (raw == comp->tape->path)) ? GSRC_TAPE
			: (comp->dif->flp[0]->path && (raw == comp->dif->flp[0]->path)) ? GSRC_DISK : GSRC_NONE;
	}
	if (game.src == GSRC_NONE) return;
	const char* raw = (game.src == GSRC_TAPE) ? comp->tape->path : comp->dif->flp[0]->path;
	if (!raw || (game.image.toLocal8Bit() != raw))
		game_forget();
}

QString game_image() {
	return game.image;
}

QString game_name() {
	return game.name;
}

static QString saves_machine_dir() {
	return QDir(saves_root()).filePath("machine - " + saves_clean(QString::fromLocal8Bit(conf.macId.c_str())));
}

// the folder ending in the hash, whatever the name it was made with
QString saves_game_dir() {
	if (game.hash.isEmpty()) return QString();
	const QString tail = " - " + game.hash.left(8);
	QDir root(saves_root());
	const QStringList found = root.entryList({"*" + tail}, QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString& name : found) {
		QFile file(root.filePath(name) + "/game.json");
		if (!file.open(QIODevice::ReadOnly)) continue;
		QJsonObject obj = QJsonDocument::fromJson(file.readAll()).object();
		if (obj.value("hash").toString() == game.hash) return root.filePath(name);
	}
	return QString();
}

// the machine's one quick save from before there were folders
static QString saves_old_quick() {
	return QDir(QString::fromLocal8Bit(conf.path.confDir.c_str())).filePath(
		"quick/" + QString::fromLocal8Bit(conf.macId.c_str()) + ".szx");
}

// ...which becomes the oldest quick save of the machine's folder
static void saves_migrate(const QString& dir) {
	QString old = saves_old_quick();
	if (!QFileInfo::exists(old)) return;
	QDateTime t = QFileInfo(old).lastModified();
	QString to = QDir(dir).filePath(QString("quick-%1.szx").arg(t.toString("yyyyMMdd-HHmmss-zzz")));
	if (QFile::rename(old, to)) return;
	if (QFile::copy(old, to)) QFile::remove(old);
}

QString saves_dir(bool make) {
	if (game.hash.isEmpty() || !conf.saves.perGame) {
		QString dir = saves_machine_dir();
		if (!make && !QFileInfo(dir).isDir() && !QFileInfo::exists(saves_old_quick())) return QString();
		if (!QDir().mkpath(dir)) return QString();
		if (!QFileInfo::exists(dir + "/game.json")) {
			QJsonObject obj;
			obj.insert("machine", QString::fromLocal8Bit(conf.macId.c_str()));
			saves_json_write(dir, obj);
		}
		saves_migrate(dir);
		return dir;
	}
	QString dir = saves_game_dir();
	if (dir.isEmpty()) {
		if (!make) return QString();
		dir = QDir(saves_root()).filePath(game.name + " - " + game.hash.left(8));
		if (!QDir().mkpath(dir)) return QString();
	}
	if (make) {
		QJsonObject obj = saves_json(dir);
		QJsonArray paths = obj.value("images").toArray();
		const QString img = QDir::fromNativeSeparators(game.image);
		for (int i = paths.size() - 1; i >= 0; i--) {
			if (paths.at(i).toString() == img) paths.removeAt(i);
		}
		paths.prepend(img);			// the last one first
		while (paths.size() > 8) paths.removeLast();
		obj.insert("name", game.name);
		obj.insert("hash", game.hash);
		obj.insert("images", paths);
		obj.insert("machine", QString::fromLocal8Bit(conf.macId.c_str()));
		saves_json_write(dir, obj);
	}
	return dir;
}

QString saves_session_path() {
	return QDir(saves_root()).filePath("last-session.szx");
}

QString saves_folder_image(const QString& dir) {
	QJsonArray paths = saves_json(dir).value("images").toArray();
	return paths.isEmpty() ? QString() : paths.at(0).toString();
}

static int saves_kind_of(const QString& name) {
	for (int k = 0; k < SAVE_KINDS; k++) {
		if (name.startsWith(QString(save_prefix[k]) + "-")) return k;
	}
	return -1;
}

// a save as its name and the picture beside it say; kind -1 for a name of no kind
static xSave saves_info(const QFileInfo& fi) {
	xSave sv;
	sv.path = fi.filePath();
	sv.kind = saves_kind_of(fi.fileName());
	if (sv.kind >= 0)
		sv.time = QDateTime::fromString(fi.completeBaseName().mid(strlen(save_prefix[sv.kind]) + 1), "yyyyMMdd-HHmmss-zzz");
	if (!sv.time.isValid()) sv.time = fi.lastModified();
	sv.size = fi.size();
	QString pic = fi.path() + "/" + fi.completeBaseName() + ".png";
	if (QFileInfo::exists(pic)) {
		sv.pic = pic;
		sv.size += QFileInfo(pic).size();
	}
	return sv;
}

QList<xSave> saves_list(const QString& dir, int kind) {
	QList<xSave> res;
	if (dir.isEmpty()) return res;
	QDir d(dir);
	const QFileInfoList files = d.entryInfoList({"*.szx"}, QDir::Files, QDir::Name | QDir::Reversed);
	for (const QFileInfo& fi : files) {
		int k = saves_kind_of(fi.fileName());
		if ((k < 0) || ((kind >= 0) && (k != kind))) continue;
		res.append(saves_info(fi));
	}
	// all kinds together: by time, not by name
	std::stable_sort(res.begin(), res.end(), [](const xSave& a, const xSave& b) {return a.time > b.time;});
	return res;
}

QString saves_new_path(const QString& dir, int kind) {
	QDateTime now = QDateTime::currentDateTime();
	QString path;
	// two saves in one millisecond do not share a name
	do {
		path = QDir(dir).filePath(QString("%1-%2.szx").arg(save_prefix[kind], now.toString("yyyyMMdd-HHmmss-zzz")));
		now = now.addMSecs(1);
	} while (QFileInfo::exists(path));
	return path;
}

void saves_remove(const xSave& sv) {
	QFile::remove(sv.path);
	if (!sv.pic.isEmpty()) QFile::remove(sv.pic);
}

void saves_trim(const QString& dir, int kind, int keep) {
	QList<xSave> list = saves_list(dir, kind);
	for (int i = qMax(keep, 0); i < list.size(); i++)
		saves_remove(list.at(i));
}

QString saves_time_text(const QDateTime& t) {
	QDate today = QDate::currentDate();
	QString time = t.toString("HH:mm:ss");
	if (t.date() == today) return "Today " + time;
	if (t.date() == today.addDays(-1)) return "Yesterday " + time;
	return t.toString("yyyy-MM-dd ") + time;
}

QString saves_kind_name(int kind) {
	switch (kind) {
		case SAVE_QUICK: return "Quick";
		case SAVE_EXIT: return "On exit";
		case SAVE_TIMED: return "Auto";
	}
	return QString();
}

QList<xSaveFolder> saves_folders() {
	QList<xSaveFolder> res;
	QDir root(saves_root());
	const QStringList dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
	for (const QString& name : dirs) {
		xSaveFolder f;
		f.dir = root.filePath(name);
		f.saves = saves_list(f.dir);
		if (f.saves.isEmpty()) continue;
		QJsonObject obj = saves_json(f.dir);
		f.machine = obj.value("hash").toString().isEmpty();
		f.name = saves_folder_name(obj);
		if (!f.machine) {
			for (const QJsonValue& v : obj.value("images").toArray())
				f.images.append(v.toString());
		}
		f.size = 0;
		for (const xSave& sv : f.saves) f.size += sv.size;
		res.append(f);
	}
	// the last session's common file
	QFileInfo last(saves_session_path());
	if (last.isFile()) {
		xSaveFolder f;
		f.machine = false;
		f.name = "Last session";
		xSave sv = saves_info(last);
		sv.kind = SAVE_EXIT;
		f.saves.append(sv);
		f.size = sv.size;
		res.append(f);
	}
	std::stable_sort(res.begin(), res.end(), [](const xSaveFolder& a, const xSaveFolder& b) {
		return a.saves.first().time > b.saves.first().time;
	});
	return res;
}

QString saves_newest_auto() {
	xSave best;
	for (const xSaveFolder& f : saves_folders()) {
		for (const xSave& sv : f.saves) {
			if ((sv.kind == SAVE_QUICK) || (!best.path.isEmpty() && (sv.time <= best.time))) continue;
			best = sv;
			break;		// newest first: the first one is the folder's newest
		}
	}
	return best.path;
}

xSave saves_resume(QString* name) {
	QString path = conf.saves.last;
	if (path.isEmpty() || !QFileInfo::exists(path)) path = saves_newest_auto();
	if (name) *name = path.isEmpty() ? QString() : saves_folder_name(saves_game_json(path));
	return path.isEmpty() ? xSave() : saves_info(QFileInfo(path));
}

QString saves_folder_of(const QString& image) {
	const QString img = QDir::fromNativeSeparators(image);
	QDir root(saves_root());
	const QStringList dirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
	for (const QString& name : dirs) {
		QJsonArray paths = saves_json(root.filePath(name)).value("images").toArray();
		for (const QJsonValue& v : paths) {
			if (v.toString().compare(img, Qt::CaseInsensitive) == 0) return root.filePath(name);
		}
	}
	return QString();
}

void saves_drop_empty(const QString& dir) {
	if (dir.isEmpty() || !saves_list(dir).isEmpty()) return;
	QFile::remove(dir + "/game.json");
	QDir().rmdir(dir);		// only if nothing else is in it
}

QList<xSave> saves_old(const xSaveFolder& f, int days, bool keepOne) {
	QList<xSave> res;
	QDateTime edge = QDateTime::currentDateTime().addDays(-days);
	// newest first: with keepOne the first stays, however old
	for (int i = keepOne ? 1 : 0; i < f.saves.size(); i++) {
		if (f.saves.at(i).time < edge) res.append(f.saves.at(i));
	}
	return res;
}

void saves_delete(const QList<xSave>& list) {
	for (const xSave& sv : list) {
		saves_remove(sv);
		QFileInfo fi(sv.path);
		QFile::remove(fi.path() + "/" + fi.completeBaseName() + ".json");
		if (QFileInfo(conf.saves.last) == fi) conf.saves.last.clear();
	}
}

void saves_clean_old() {
	if (!conf.saves.cleanOn) return;
	int count = 0;
	for (const xSaveFolder& f : saves_folders()) {
		QList<xSave> old = saves_old(f, conf.saves.cleanDays, conf.saves.keepOne);
		saves_delete(old);
		count += old.size();
		saves_drop_empty(f.dir);
	}
	if (count) xlog(XLG_FILE, XLL_INFO, "saves: %i older than %i days deleted", count, conf.saves.cleanDays);
}
