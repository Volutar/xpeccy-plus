#include "displaylook.h"

#include "../xcore/xcore.h"
#include "../xcore/log.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <map>

#define	LOOK_KIND	"shaders"
#define	LOOK_STORE	"values.conf"
#define	LOOK_TUBE	"crt.txt"

// The tube's looks that took another name: the old one is no user preset's to take
static const struct {const char* was; const char* now;} lookRenamed[] = {
	{"PVM", "Sony PVM"},
};

// ---------------------------------------------------------------- the files

namespace {

struct FileEntry {
	QString path;
	QDateTime stamp;
	bool ok = false;
	xShdPreset head;
};

// a map: an entry stays where it is while others come
std::map<QString, FileEntry> fileCache;

struct UserPreset {
	QString base;		// its file's name, no extension
	QString title;
	QString shader;
	xShdValues values;
};

// key = value lines; # starts a comment line
QList<QPair<QString, QString> > read_pairs(const QString& path) {
	QList<QPair<QString, QString> > res;
	QFile file(path);
	if (!file.open(QFile::ReadOnly)) return res;
	while (!file.atEnd()) {
		const QString lin = QString::fromUtf8(file.readLine()).trimmed();
		if (lin.isEmpty() || lin.startsWith('#')) continue;
		if (lin.startsWith('[')) {
			res << qMakePair(QString(), lin.mid(1, lin.lastIndexOf(']') - 1));
			continue;
		}
		const int eq = lin.indexOf('=');
		if (eq < 1) continue;
		res << qMakePair(lin.left(eq).trimmed(), lin.mid(eq + 1).trimmed());
	}
	return res;
}

QString num_text(float val) {
	return QString::number(val, 'g', 7);
}

// The user's presets as the folder has them. Read again by every listing (a menu, a
// window opening) and after one is saved or deleted - not at every step of a slider.
QList<UserPreset> userCache;
bool userRead = false;

QList<UserPreset> read_user_presets() {
	QList<UserPreset> res;
	const QDir dir(xres_dir(LOOK_KIND));
	for (const QString& nam : dir.entryList(QStringList() << "*.preset", QDir::Files, QDir::Name)) {
		UserPreset up;
		up.base = QFileInfo(nam).completeBaseName();
		up.title = up.base;
		for (const auto& kv : read_pairs(dir.filePath(nam))) {
			if (kv.first == "shader") {
				up.shader = kv.second;
			} else if (kv.first == "name") {
				up.title = kv.second;
			} else if (!kv.first.isEmpty()) {
				bool ok = false;
				const float val = kv.second.toFloat(&ok);
				if (ok) up.values.append({kv.first, val});
			}
		}
		if (!up.shader.isEmpty()) res << up;
	}
	return res;
}

const QList<UserPreset>& user_presets(bool fresh = false) {
	if (fresh || !userRead) {
		userCache = read_user_presets();
		userRead = true;
	}
	return userCache;
}

// the user's preset a new one of that name would be written over: the same title or
// the same file name, in any case of letters
const UserPreset* user_clash(const QString& title, const QString& base) {
	for (const UserPreset& up : user_presets()) {
		if (!up.title.compare(title, Qt::CaseInsensitive) || !up.base.compare(base, Qt::CaseInsensitive))
			return &up;
	}
	return nullptr;
}

// a preset's file name: its title less what a file name or a section name cannot hold
QString preset_base(const QString& title) {
	QString base = title;
	base.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|#=\[\]])")), "_");
	return base;
}

// ------------------------------------------------------------ the kept values

xLookStore store;
bool storeRead = false;
bool storeDirty = false;

QString look_id(const xLook& lk) {
	return lk.file + "#" + lk.preset;
}

QString store_path() {
	return xres_dir(LOOK_KIND) + "/" + LOOK_STORE;
}

void store_read() {
	if (storeRead) return;
	storeRead = true;
	QString sect;
	for (const auto& kv : read_pairs(store_path())) {
		if (kv.first.isEmpty()) {
			sect = kv.second;
			continue;
		}
		bool ok = false;
		const float val = kv.second.toFloat(&ok);
		if (ok && !sect.isEmpty()) store[sect][kv.first] = val;
	}
}

const xShdParam* file_param(const xLook& lk, const QString& name) {
	const xShdPreset* head = look_file(lk.file);
	if (!head) return nullptr;
	for (const xShdParam& p : head->params)
		if (p.name == name) return &p;
	return nullptr;
}

// "NAME value, NAME value"
xShdValues parse_values(const char* text) {
	xShdValues res;
	for (const QString& item : QString::fromLatin1(text).split(',', Qt::SkipEmptyParts)) {
		const QStringList t = item.trimmed().split(' ', Qt::SkipEmptyParts);
		if (t.size() == 2) res.append({t[0], t[1].toFloat()});
	}
	return res;
}

} // anonymous namespace

const xShdPreset* look_file(const QString& file) {
	if (file.isEmpty()) return nullptr;
	const QString path = xres_path(LOOK_KIND, file);
	const QDateTime stamp = QFileInfo(path).lastModified();
	FileEntry& e = fileCache[file];
	if ((e.path != path) || (e.stamp != stamp)) {
		QString err;
		e.path = path;
		e.stamp = stamp;
		e.ok = xshd_parse(path, e.head, &err);
	}
	return e.ok ? &e.head : nullptr;
}

// ---------------------------------------------------------------- the list

// The shader files the tube came instead of. They stay in the build, listed apart under
// Legacy, until the tube does all they did (its side fill and bezel are to come); then
// they go, and look_old_names() takes a config that names one over to the tube.
static const char* const legacyFiles[] = {
	"composite-crt-flat.txt", "composite-crt.txt", "grille-nocurve-crisp.txt", "grille-nocurve-light.txt",
	"grille2-nocurve-crisp.txt", "grille2-nocurve-light.txt", "monitor-1084-flat.txt", "monitor-1084.txt",
	"pvm-sharp.txt", "tridot-curve-crisp.txt", "tridot-curve-light.txt", "tridot-nocurve-crisp.txt",
	"tridot-nocurve-light.txt", "tvline-curve-light.txt", "tvline-nocurve-light.txt", "widetridot-curve-crisp.txt",
	"widetridot-curve-light.txt", "widetridot-nocurve-crisp.txt", "widetridot-nocurve-light.txt", "zx-rf-artifact.txt",
};

static bool legacy_file(const QString& file) {
	for (const char* name : legacyFiles)
		if (file == name) return true;
	return false;
}

// ...and is in this build
static bool legacy_kept(const QString& file) {
	return legacy_file(file) && QFile::exists(xres_root(LOOK_KIND) + "/" + file);
}

QString look_legacy_tip() {
	return QCoreApplication::translate("Display", "Deprecated: to be removed in a coming release. The CRT looks replace it.");
}

QString look_flat_tip() {
	return QCoreApplication::translate("Display", "Every look without curvature, corners or vignette, unless set in its settings");
}

QList<xLook> look_list() {
	QList<xLook> parts[LOOK_LEGACY + 1];
	xLook none;
	none.title = "None";
	parts[LOOK_BASIC] << none;
	if (conf.vid.shd_support) {
		const QStringList builtin = QDir(xres_root(LOOK_KIND)).entryList(QStringList() << "*.txt", QDir::Files);
		for (const QString& file : xres_list(LOOK_KIND, QStringList() << "*.txt")) {
			const xShdPreset* head = look_file(file);
			xLook lk;
			lk.file = file;
			lk.title = (head && !head->title.isEmpty()) ? head->title : QFileInfo(file).completeBaseName();
			// (a file of an old look's name in the user's folder is the user's own)
			if (legacy_kept(file) && !QFile::exists(xres_dir(LOOK_KIND) + "/" + file)) {
				lk.part = LOOK_LEGACY;
				parts[LOOK_LEGACY] << lk;
			} else if (head && !head->sets.isEmpty()) {
				// a file with presets is listed by them
				lk.part = LOOK_PRESET;
				for (const xShdSet& set : head->sets) {
					lk.preset = set.name;
					lk.title = set.name;
					parts[LOOK_PRESET] << lk;
				}
			} else {
				const bool basic = head && !head->title.isEmpty() && !head->params.isEmpty() && builtin.contains(file);
				lk.part = basic ? LOOK_BASIC : LOOK_FILE;
				parts[lk.part] << lk;
			}
		}
		for (const UserPreset& up : user_presets(true)) {
			if (!look_file(up.shader)) continue;
			xLook lk;
			lk.part = LOOK_USER;
			lk.file = up.shader;
			lk.preset = up.base;
			lk.title = up.title;
			lk.user = true;
			parts[LOOK_USER] << lk;
		}
		auto byTitle = [](const xLook& a, const xLook& b) {return a.title.compare(b.title, Qt::CaseInsensitive) < 0;};
		std::sort(parts[LOOK_USER].begin(), parts[LOOK_USER].end(), byTitle);
		std::sort(parts[LOOK_FILE].begin(), parts[LOOK_FILE].end(), byTitle);
		std::sort(parts[LOOK_LEGACY].begin(), parts[LOOK_LEGACY].end(), byTitle);
	}
	QList<xLook> res;
	for (const QList<xLook>& p : parts) res << p;
	return res;
}

xLook look_current() {
	xLook lk;
	lk.file = QString::fromLocal8Bit(conf.vid.shader.c_str());
	lk.preset = QString::fromUtf8(conf.vid.shdPreset.c_str());
	if (lk.file.isEmpty()) {
		lk.preset.clear();
		lk.title = "None";
		return lk;
	}
	const QList<xLook> list = look_list();
	for (const xLook& o : list)
		if (o == lk) return o;
	// a preset that is gone, or none named where the file has them: the file's first
	for (const xLook& o : list)
		if ((o.file == lk.file) && !o.user) return o;
	// a file that is not there stays chosen: its error is shown
	lk.preset.clear();
	lk.part = LOOK_FILE;
	lk.title = QFileInfo(lk.file).completeBaseName();
	return lk;
}

void look_choose(const xLook& lk) {
	conf.vid.shader = lk.file.toLocal8Bit().constData();
	conf.vid.shdPreset = lk.preset.toUtf8().constData();	// a name the user typed: any letters
}

bool look_has_knobs(const xLook& lk) {
	const xShdPreset* head = conf.vid.shd_support ? look_file(lk.file) : nullptr;
	return head && !head->params.isEmpty();
}

// ---------------------------------------------------------------- the values

static xShdValues preset_values(const xLook& lk) {
	if (lk.user) {
		for (const UserPreset& up : user_presets())
			if ((up.base == lk.preset) && (up.shader == lk.file)) return up.values;
		return xShdValues();
	}
	const xShdPreset* head = look_file(lk.file);
	if (head) {
		for (const xShdSet& set : head->sets)
			if (set.name == lk.preset) return set.values;
	}
	return xShdValues();
}

// Flat screen: the file's flat knobs at 0 (or their nearest) over the preset's
static xShdValues base_values(const xLook& lk, bool flat) {
	xShdValues res = preset_values(lk);
	const xShdPreset* head = flat ? look_file(lk.file) : nullptr;
	if (head) {
		for (const xShdParam& p : head->params) {
			if (!p.flat) continue;
			const float off = qBound(p.min, 0.0f, p.max);
			bool had = false;
			for (xShdValue& v : res) {
				if (v.name != p.name) continue;
				v.value = off;
				had = true;
			}
			if (!had) res.append({p.name, off});
		}
	}
	return res;
}

xShdValues look_base(const xLook& lk) {
	return base_values(lk, conf.vid.flatScreen);
}

xShdValues look_values(const xLook& lk) {
	xShdValues res = look_base(lk);
	store_read();
	const QMap<QString, float> kept = store.value(look_id(lk));
	for (auto it = kept.constBegin(); it != kept.constEnd(); ++it)
		res.append({it.key(), it.value()});
	return res;
}

static float own_value(const xLook& lk, const QString& name, bool flat) {
	float val = 0;
	if (const xShdParam* p = file_param(lk, name)) val = p->def;
	for (const xShdValue& v : base_values(lk, flat))
		if (v.name == name) val = v.value;
	return val;
}

static float own_value(const xLook& lk, const QString& name) {
	return own_value(lk, name, conf.vid.flatScreen);
}

float look_value(const xLook& lk, const QString& name) {
	store_read();
	const QMap<QString, float> kept = store.value(look_id(lk));
	if (kept.contains(name)) return kept.value(name);
	return own_value(lk, name);
}

// the look's own value with or without Flat screen, as near as a step's thousandth
static bool is_own(const xLook& lk, const QString& name, float value, bool flat) {
	const xShdParam* p = file_param(lk, name);
	const float slack = (p ? std::fabs(p->step) : 0.0f) * 0.001f + 1e-7f;
	return std::fabs(value - own_value(lk, name, flat)) <= slack;
}

static void forget(const xLook& lk, const QString& name) {
	const QString id = look_id(lk);
	if (!store.contains(id) || !store[id].contains(name)) return;
	store[id].remove(name);
	if (store[id].isEmpty()) store.remove(id);
	storeDirty = true;
}

// a flat knob is let go only at the look's own value with Flat screen and without
void look_keep(const xLook& lk, const QString& name, float value) {
	store_read();
	const xShdParam* p = file_param(lk, name);
	const bool flat = p && p->flat;
	if (is_own(lk, name, value, conf.vid.flatScreen) && (!flat || is_own(lk, name, value, !conf.vid.flatScreen))) {
		forget(lk, name);
		return;
	}
	store[look_id(lk)][name] = value;
	storeDirty = true;
}

void look_back(const xLook& lk, const QString& name) {
	store_read();
	forget(lk, name);
}

void look_reset(const xLook& lk) {
	store_read();
	if (store.remove(look_id(lk))) storeDirty = true;
}

bool look_turned(const xLook& lk, const QString& name) {
	store_read();
	return store.value(look_id(lk)).contains(name);
}

bool look_changed(const xLook& lk) {
	store_read();
	return store.contains(look_id(lk));
}

float look_own(const xLook& lk, const QString& name) {
	return own_value(lk, name);
}

// the configuration is read again: what the files have now, not what was turned
void look_store_reload() {
	store.clear();
	storeRead = false;
	storeDirty = false;
	userRead = false;
}

QString look_preset_taken(const QString& name) {
	const QString title = name.trimmed();
	const UserPreset* old = user_clash(title, preset_base(title));
	return old ? old->title : QString();
}

xLookStore look_store_take() {
	store_read();
	return store;
}

void look_store_put(const xLookStore& was) {
	store_read();
	if (store == was) return;
	store = was;
	storeDirty = true;
}

void look_store_save() {
	if (!storeDirty) return;
	QFile file(store_path());
	if (store.isEmpty()) {
		storeDirty = file.exists() && !file.remove();
		return;
	}
	// not written: still owed, the next save tries again
	if (!file.open(QFile::WriteOnly | QFile::Truncate)) {
		xlog(XLG_CONF, XLL_WARN, "can't write %s", store_path().toLocal8Bit().constData());
		return;
	}
	storeDirty = false;
	QByteArray out("# The Display knobs turned, by look: shader file # preset\n");
	for (auto st = store.constBegin(); st != store.constEnd(); ++st) {
		out += "\n[" + st.key().toUtf8() + "]\n";
		for (auto it = st.value().constBegin(); it != st.value().constEnd(); ++it)
			out += it.key().toUtf8() + " = " + num_text(it.value()).toUtf8() + "\n";
	}
	file.write(out);
}

// ---------------------------------------------------------------- the user's presets

bool look_save_preset(const xLook& lk, const QString& name, QString* err) {
	const QString title = name.trimmed();
	const xShdPreset* head = look_file(lk.file);
	if (title.isEmpty() || !head) {
		*err = title.isEmpty() ? "A preset needs a name" : "This look has no settings to save";
		return false;
	}
	QString base = preset_base(title);
	if (const UserPreset* old = user_clash(title, base)) base = old->base;
	for (const xLook& o : look_list()) {
		if (o.user || (o.part == LOOK_LEGACY)) continue;
		if (!o.title.compare(title, Qt::CaseInsensitive) || !o.preset.compare(base, Qt::CaseInsensitive)) {
			*err = "A built-in look has this name";
			return false;
		}
	}
	for (const auto& row : lookRenamed) {
		if (!title.compare(row.was, Qt::CaseInsensitive) || !base.compare(row.was, Qt::CaseInsensitive)) {
			*err = "A built-in look had this name";
			return false;
		}
	}
	QByteArray out("shader = " + lk.file.toUtf8() + "\nname = " + title.toUtf8() + "\n");
	// Flat screen is no part of a preset: a flat knob not turned saves the look's own value
	store_read();
	const QMap<QString, float> kept = store.value(look_id(lk));
	for (const xShdParam& p : head->params) {
		const float val = kept.contains(p.name) ? kept.value(p.name) : own_value(lk, p.name, false);
		if (std::fabs(val - p.def) > std::fabs(p.step) * 0.001f + 1e-7f)
			out += p.name.toUtf8() + " = " + num_text(val).toUtf8() + "\n";
	}
	QDir().mkpath(xres_dir(LOOK_KIND));
	QFile file(xres_dir(LOOK_KIND) + "/" + base + ".preset");
	if (!file.open(QFile::WriteOnly | QFile::Truncate)) {
		*err = "The preset's file cannot be written";
		return false;
	}
	file.write(out);
	file.close();
	user_presets(true);
	xLook made;
	made.part = LOOK_USER;
	made.file = lk.file;
	made.preset = base;
	made.title = title;
	made.user = true;
	// the knobs turned are the preset's own now: the look they were turned on is as it was
	look_reset(lk);
	look_reset(made);
	look_choose(made);
	return true;
}

bool look_delete_preset(const xLook& lk) {
	if (!lk.user) return false;
	const bool chosen = (look_current() == lk);
	if (!QFile::remove(xres_dir(LOOK_KIND) + "/" + lk.preset + ".preset")) return false;
	user_presets(true);
	look_reset(lk);
	if (chosen) look_choose(look_current());	// to the file's first look
	return true;
}

// ---------------------------------------------------------------- old names

// The looks that were files of their own - the aperture grille and shadow mask shaders,
// the TV lines, the first monitor and signal looks - are the tube: an old name whose file
// is not there (see legacyFiles: they are, for now) opens the tube's look nearest to it
// with the knobs turned to stand for it (measured beside the old file
// on a test card: the lines' depth on bright and on plain white, the dot's sharpness,
// the light over all, the bend). The knobs are turned only on a look not turned yet.
#define	OLD_CRISP	"CURVE 0, CURVEX 1, SPOT 0, MASKS 0.6, SCAN 1.0, WIDTH 0.8, FOCUS 0.9, GROW 0, GLOW 0.1, BRIGHT 0.55, DIVERGE 0"
#define	OLD_SOFT	"CURVE 0, CURVEX 1, SPOT 0, MASKS 0.45, SCAN 0.6, WIDTH 0.8, FOCUS 0.65, GROW 0, GLOW 0.1, BRIGHT 0.7, DIVERGE 0"
#define	OLD_GRILLE	"MASK 1, PITCH 1.0, "
#define	OLD_FINE	"MASK 1, PITCH 2.0, "
// (Tridot's own ghost, grain, glow and the like back to none: the old files had none)
#define	OLD_TRIDOT	"KEEP 1, PITCH 1.5, MBEND 0, SHIMMER 0.15, GHOST 0, GHOSTX 1.6, WIGGLE 0, NOISE 0, CORNER 0, BLOOM 0, CONTRAST 0, CURVEY 1, GLOWR 1.5, AFTER 0, "
#define	OLD_SHADOW	"MASK 3, MSIZE 2, " OLD_TRIDOT
#define	OLD_WIDE	"MASK 3, MSIZE 3, " OLD_TRIDOT
#define	OLD_CURVE	", CURVE 0.02"
#define	OLD_TVLINE	"CURVE 0, CURVEX 1, SPOT 0, MASK 1, MASKS 0.3, PITCH 2.5, WIDTH 0.8, FOCUS 0.85, GLOW 0, DIVERGE 0, "

// a look of the tube moves to another name: the config that has it and its kept knobs
static void look_move(const QString& was, const QString& now) {
	const QString from = QString(LOOK_TUBE) + "#" + was;	// look_id() of the tube's look
	const QString to = QString(LOOK_TUBE) + "#" + now;
	if (store.contains(from)) {
		// both kept: what the new name has stays, the old one's other knobs join it
		const QMap<QString, float> old = store.take(from);
		for (auto it = old.constBegin(); it != old.constEnd(); ++it)
			if (!store[to].contains(it.key())) store[to][it.key()] = it.value();
		storeDirty = true;
	}
	if ((QString::fromLocal8Bit(conf.vid.shader.c_str()) == LOOK_TUBE) && (QString::fromUtf8(conf.vid.shdPreset.c_str()) == was))
		conf.vid.shdPreset = now.toUtf8().constData();
}

// the user's preset of the tube by that file name, if any
static const UserPreset* user_tube_preset(const QString& base) {
	for (const UserPreset& up : user_presets())
		if ((up.shader == LOOK_TUBE) && !up.base.compare(base, Qt::CaseInsensitive)) return &up;
	return nullptr;
}

// The tube's looks renamed: a config that has the old name, and the knobs kept for it,
// go over to the new one. A preset of the user's that has the new name (saved before it
// was the tube's) is renamed first, its knobs with it; one that has the old name keeps it
static void look_renamed() {
	store_read();
	user_presets(true);		// the folder as this config has it, not as read before it was loaded
	for (const auto& row : lookRenamed) {
		if (const UserPreset* up = user_tube_preset(row.now)) {
			const QString dir = xres_dir(LOOK_KIND) + "/";
			QString base = up->base + " (user)";
			for (int n = 2; QFile::exists(dir + base + ".preset"); n++) base = up->base + QString(" (user %1)").arg(n);
			QFile file(dir + up->base + ".preset");
			if (file.open(QFile::ReadOnly)) {
				QByteArray text = file.readAll();
				file.close();
				text.replace("\nname = " + up->title.toUtf8() + "\n", "\nname = " + base.toUtf8() + "\n");
				QFile out(dir + base + ".preset");
				if (out.open(QFile::WriteOnly) && (out.write(text) == text.size())) {
					out.close();
					file.remove();
					look_move(up->base, base);
					xlog(XLG_CONF, XLL_INFO, "the preset %s is now %s: a built-in look has its name",
						up->base.toUtf8().constData(), base.toUtf8().constData());
				}
			}
			user_presets(true);
		}
		if (!user_tube_preset(row.was)) look_move(row.was, row.now);
	}
}

void look_old_names() {
	look_renamed();
	// old: the file's name, and for the file of round (a) its preset too
	static const struct {const char* old; const char* was; const char* preset; const char* extra;} names[] = {
		{"grille-nocurve-crisp.txt", "", "Sony PVM", OLD_GRILLE OLD_CRISP},
		{"grille-nocurve-light.txt", "", "Sony PVM", OLD_GRILLE OLD_SOFT},
		{"grille2-nocurve-crisp.txt", "", "Sony PVM", OLD_FINE OLD_CRISP},
		{"grille2-nocurve-light.txt", "", "Sony PVM", OLD_FINE OLD_SOFT},
		{"tridot-nocurve-crisp.txt", "", "Tridot", OLD_SHADOW OLD_CRISP},
		{"tridot-nocurve-light.txt", "", "Tridot", OLD_SHADOW OLD_SOFT},
		{"tridot-curve-crisp.txt", "", "Tridot", OLD_SHADOW OLD_CRISP OLD_CURVE},
		{"tridot-curve-light.txt", "", "Tridot", OLD_SHADOW OLD_SOFT OLD_CURVE},
		{"widetridot-nocurve-crisp.txt", "", "Tridot", OLD_WIDE OLD_CRISP},
		{"widetridot-nocurve-light.txt", "", "Tridot", OLD_WIDE OLD_SOFT},
		{"widetridot-curve-crisp.txt", "", "Tridot", OLD_WIDE OLD_CRISP OLD_CURVE},
		{"widetridot-curve-light.txt", "", "Tridot", OLD_WIDE OLD_SOFT OLD_CURVE},
		{"crt-lottes.txt", "Aperture grille", "Sony PVM", OLD_GRILLE OLD_CRISP},
		{"crt-lottes.txt", "Aperture grille, soft", "Sony PVM", OLD_GRILLE OLD_SOFT},
		{"crt-lottes.txt", "Fine grille", "Sony PVM", OLD_FINE OLD_CRISP},
		{"crt-lottes.txt", "Fine grille, soft", "Sony PVM", OLD_FINE OLD_SOFT},
		{"crt-lottes.txt", "Shadow mask", "Tridot", OLD_SHADOW OLD_CRISP},
		{"crt-lottes.txt", "Shadow mask, soft", "Tridot", OLD_SHADOW OLD_SOFT},
		{"crt-lottes.txt", "Wide shadow mask", "Tridot", OLD_WIDE OLD_CRISP},
		{"crt-lottes.txt", "Wide shadow mask, soft", "Tridot", OLD_WIDE OLD_SOFT},
		{"crt-lottes.txt", "", "Sony PVM", OLD_GRILLE OLD_CRISP},	// no preset, or one not its own
		{"tvline-nocurve-light.txt", "", "Sony PVM", OLD_TVLINE "SCAN 1.0, GROW 0.85, BRIGHT 0.7"},
		{"tvline-curve-light.txt", "", "Sony PVM", OLD_TVLINE "SCAN 0.9, GROW 1.0, BRIGHT 0.95, CURVE 0.02"},
		{"monitor-1084.txt", "", "RGB monitor", ""},
		{"monitor-1084-flat.txt", "", "RGB monitor", "CURVE 0, CORNER 0, VIGN 0"},
		{"pvm-sharp.txt", "", "Sony PVM", ""},
		{"composite-crt.txt", "", "Home TV (composite)", ""},
		{"composite-crt-flat.txt", "", "Home TV (composite)", "CURVE 0, VIGN 0"},
		{"zx-rf-artifact.txt", "", "Home TV (composite)", "INPUT 2"},
	};
	const QString old = QString::fromLocal8Bit(conf.vid.shader.c_str());
	const QString was = QString::fromUtf8(conf.vid.shdPreset.c_str());
	for (const auto& row : names) {
		if ((old != row.old) || (row.was[0] && (was != row.was))) continue;
		// a file of that name is the shader, readable or not: the user's own, or the
		// build's old look while it is kept
		if (QFile::exists(xres_dir(LOOK_KIND) + "/" + old) || legacy_kept(old)) return;
		xLook lk;
		lk.part = LOOK_PRESET;
		lk.file = LOOK_TUBE;
		lk.preset = row.preset;
		look_choose(lk);
		store_read();
		if (row.extra[0] && !store.contains(look_id(lk))) {
			for (const xShdValue& v : parse_values(row.extra))
				store[look_id(lk)][v.name] = v.value;
			storeDirty = true;
		}
		return;
	}
}
