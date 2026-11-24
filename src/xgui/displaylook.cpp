#include "displaylook.h"

#include "../xcore/xcore.h"
#include "../xcore/log.h"

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
#define	LOOK_LOTTES	"crt-lottes.txt"

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

QList<xLook> look_list() {
	QList<xLook> parts[LOOK_FILE + 1];
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
			if (head && !head->sets.isEmpty()) {
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

xShdValues look_base(const xLook& lk) {
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

xShdValues look_values(const xLook& lk) {
	xShdValues res = look_base(lk);
	store_read();
	const QMap<QString, float> kept = store.value(look_id(lk));
	for (auto it = kept.constBegin(); it != kept.constEnd(); ++it)
		res.append({it.key(), it.value()});
	return res;
}

static float own_value(const xLook& lk, const QString& name) {
	float val = 0;
	if (const xShdParam* p = file_param(lk, name)) val = p->def;
	for (const xShdValue& v : look_base(lk))
		if (v.name == name) val = v.value;
	return val;
}

float look_value(const xLook& lk, const QString& name) {
	store_read();
	const QMap<QString, float> kept = store.value(look_id(lk));
	if (kept.contains(name)) return kept.value(name);
	return own_value(lk, name);
}

void look_keep(const xLook& lk, const QString& name, float value) {
	store_read();
	const QString id = look_id(lk);
	const xShdParam* p = file_param(lk, name);
	const float slack = (p ? std::fabs(p->step) : 0.0f) * 0.001f + 1e-7f;
	if (std::fabs(value - own_value(lk, name)) <= slack) {
		if (!store.contains(id)) return;
		store[id].remove(name);
		if (store[id].isEmpty()) store.remove(id);
	} else {
		store[id][name] = value;
	}
	storeDirty = true;
}

void look_reset(const xLook& lk) {
	store_read();
	if (store.remove(look_id(lk))) storeDirty = true;
}

bool look_changed(const xLook& lk) {
	store_read();
	return store.contains(look_id(lk));
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
		if (o.user) continue;
		if (!o.title.compare(title, Qt::CaseInsensitive) || !o.preset.compare(base, Qt::CaseInsensitive)) {
			*err = "A built-in look has this name";
			return false;
		}
	}
	QByteArray out("shader = " + lk.file.toUtf8() + "\nname = " + title.toUtf8() + "\n");
	for (const xShdParam& p : head->params) {
		const float val = look_value(lk, p.name);
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

// The aperture grille and shadow mask shaders were a file a look; each is a preset of
// one file now, the curved ones with what they had on top.
void look_old_names() {
	static const struct {const char* old; const char* preset; const char* extra;} names[] = {
		{"grille-nocurve-crisp.txt", "Aperture grille", ""},
		{"grille-nocurve-light.txt", "Aperture grille, soft", ""},
		{"grille2-nocurve-crisp.txt", "Fine grille", ""},
		{"grille2-nocurve-light.txt", "Fine grille, soft", ""},
		{"tridot-nocurve-crisp.txt", "Shadow mask", ""},
		{"tridot-nocurve-light.txt", "Shadow mask, soft", ""},
		{"tridot-curve-crisp.txt", "Shadow mask", "curve 0.025, curveY 0, brightBoost 1"},
		{"tridot-curve-light.txt", "Shadow mask, soft", "curve 0.025"},
		{"widetridot-nocurve-crisp.txt", "Wide shadow mask", ""},
		{"widetridot-nocurve-light.txt", "Wide shadow mask, soft", ""},
		{"widetridot-curve-crisp.txt", "Wide shadow mask", "curve 0.025"},
		{"widetridot-curve-light.txt", "Wide shadow mask, soft", "curve 0.025, brightBoost 1.1"},
	};
	const QString old = QString::fromLocal8Bit(conf.vid.shader.c_str());
	for (const auto& row : names) {
		if (old != row.old) continue;
		// a file of that name in the user's folder is the user's shader
		if (QFile::exists(xres_dir(LOOK_KIND) + "/" + old)) return;
		xLook lk;
		lk.part = LOOK_PRESET;
		lk.file = LOOK_LOTTES;
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
