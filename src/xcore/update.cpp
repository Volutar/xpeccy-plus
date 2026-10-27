// The update check's logic (update.h)

#include "update.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

int up_calver_cmp(const QString& a, const QString& b) {
	QStringList pa = a.split('.');
	QStringList pb = b.split('.');
	int n = qMax(pa.size(), pb.size());
	for (int i = 0; i < n; i++) {
		int va = pa.value(i).toInt();
		int vb = pb.value(i).toInt();
		if (va != vb) return (va < vb) ? -1 : 1;
	}
	return 0;
}

int up_build_cmp(const upBuild& a, const upBuild& b) {
	int c = up_calver_cmp(a.base, b.base);
	if (c) return c;
	if (a.release != b.release) return a.release ? 1 : -1;
	if (a.release) return 0;
	if (a.date != b.date) return (a.date < b.date) ? -1 : 1;
	return 0;
}

bool up_newer(const upBuild& cand, const upBuild& self) {
	int c = up_build_cmp(cand, self);
	if (c) return c > 0;
	return !cand.release && !cand.commit.isEmpty() && !self.commit.isEmpty() && (cand.commit != self.commit);
}

bool up_same_day(const upBuild& cand, const upBuild& self) {
	return up_newer(cand, self) && (up_build_cmp(cand, self) == 0);
}

bool up_compare_ahead(const QByteArray& json) {
	QJsonDocument doc = QJsonDocument::fromJson(json);
	return doc.isObject() && (doc.object().value("status").toString() == "ahead");
}

int up_default_channel(const upBuild& self) {
	return self.release ? UP_RELEASES : UP_NIGHTLY;
}

// the name without its extension: ".zip", ".dmg", ".AppImage"
static QString up_stem(const QString& name) {
	int dot = name.lastIndexOf('.');
	return (dot > 0) ? name.left(dot) : name;
}

static upRelease up_fail(const QString& error) {
	upRelease rel;
	rel.error = error;
	return rel;
}

upRelease up_parse(const QByteArray& json, const QString& package) {
	static const QRegularExpression rxTag("^v(\\d+(?:\\.\\d+)*)$");
	static const QRegularExpression rxDev("^xpeccy-plus-(\\d+(?:\\.\\d+)*)-dev\\+(\\d{8})-");
	static const QRegularExpression rxSha("^[0-9a-f]{40}$");
	static const QRegularExpression rxDigest("^sha256:([0-9a-fA-F]{64})$");
	QJsonParseError perr;
	QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
	if (!doc.isObject()) return up_fail("GitHub's answer is not readable");
	QJsonObject obj = doc.object();
	upRelease rel;
	rel.tag = obj.value("tag_name").toString();
	rel.body = obj.value("body").toString();
	rel.page = obj.value("html_url").toString();
	if (rel.tag.isEmpty()) {
		// an error answer: {"message": "API rate limit exceeded ..."}
		QString msg = obj.value("message").toString();
		return up_fail(msg.isEmpty() ? QString("GitHub's answer has no release") : msg);
	}
	QJsonArray assets = obj.value("assets").toArray();
	if (rel.tag == "nightly") {
		rel.nightly = true;
		rel.build.release = false;
		// the version and date are the build's own, as its files are named
		foreach(QJsonValue val, assets) {
			QRegularExpressionMatch m = rxDev.match(val.toObject().value("name").toString());
			if (m.hasMatch()) {
				rel.build.base = m.captured(1);
				rel.build.date = m.captured(2).toInt();
				break;
			}
		}
		if (rel.build.base.isEmpty()) return up_fail("The nightly build has no files");
		QString sha = obj.value("target_commitish").toString().toLower();
		if (rxSha.match(sha).hasMatch()) rel.build.commit = sha;
	} else {
		QRegularExpressionMatch m = rxTag.match(rel.tag);
		if (!m.hasMatch()) return up_fail(QString("Unknown release tag %0").arg(rel.tag));
		rel.build.base = m.captured(1);
		rel.build.release = true;
	}
	if (!package.isEmpty()) {
		// named as packaging/ names them: xpeccy-plus-<version>-<package>.<ext>
		QString want = QString("xpeccy-plus-%0-%1").arg(rel.nightly
			? QString("%0-dev+%1").arg(rel.build.base).arg(rel.build.date)
			: rel.build.base, package);
		foreach(QJsonValue val, assets) {
			QJsonObject ao = val.toObject();
			QString name = ao.value("name").toString();
			if (up_stem(name) != want) continue;
			rel.asset.name = name;
			rel.asset.url = ao.value("browser_download_url").toString();
			rel.asset.size = (qint64)ao.value("size").toDouble();
			QRegularExpressionMatch dm = rxDigest.match(ao.value("digest").toString());
			if (dm.hasMatch()) rel.asset.sha256 = dm.captured(1).toLower();
			break;
		}
	}
	rel.ok = true;
	return rel;
}

int up_pick(const QList<upRelease>& list, const upBuild& self) {
	int best = -1;
	for (int i = 0; i < list.size(); i++) {
		const upRelease& rel = list[i];
		if (!rel.ok || !up_newer(rel.build, self)) continue;
		if ((best < 0) || (up_build_cmp(rel.build, list[best].build) > 0)) best = i;
	}
	return best;
}

// CHANGELOG.md, Keep a Changelog style: "## <version> - <date>" or "## Unreleased"
// sections, "### Added" groups, "- " entries with their continuation lines

struct upEntry {
	QString text;		// as written, its lines
	QString key;		// whitespace squeezed, to compare
};

struct upGroup {
	QString head;		// "### Added", or empty before any
	QList<upEntry> entries;
};

struct upSection {
	QString head;		// "## 2026.9 - 2026-11-07"
	QString version;	// "2026.9", empty for Unreleased
	QList<upGroup> groups;
};

static QList<upSection> up_sections(const QByteArray& md) {
	QList<upSection> list;
	QStringList lines = QString::fromUtf8(md).split('\n');
	upEntry* entry = NULL;
	for (QString line : lines) {
		if (line.endsWith('\r')) line.chop(1);
		if (line.startsWith("## ")) {
			upSection sec;
			sec.head = line;
			QString ver = line.mid(3).section(' ', 0, 0);
			if (ver.compare("Unreleased", Qt::CaseInsensitive)) sec.version = ver;
			list.append(sec);
			entry = NULL;
		} else if (list.isEmpty()) {
			// the preamble
		} else if (line.startsWith("### ")) {
			upGroup grp;
			grp.head = line;
			list.last().groups.append(grp);
			entry = NULL;
		} else if (line.startsWith("- ")) {
			if (list.last().groups.isEmpty()) list.last().groups.append(upGroup());
			list.last().groups.last().entries.append(upEntry());
			entry = &list.last().groups.last().entries.last();
			entry->text = line;
		} else if (entry && (line.isEmpty() || line.startsWith(' ') || line.startsWith('\t'))) {
			entry->text += "\n" + line;
		} else {
			entry = NULL;		// text that is no entry: not shown
		}
	}
	for (upSection& sec : list) {
		for (upGroup& grp : sec.groups) {
			for (upEntry& ent : grp.entries) {
				ent.text = ent.text.trimmed();
				ent.key = ent.text.simplified();
			}
		}
	}
	return list;
}

QString up_whats_new(const QByteArray& theirs, const QByteArray& mine, const upBuild& self) {
	QSet<QString> have;
	foreach(const upSection& sec, up_sections(mine)) {
		foreach(const upGroup& grp, sec.groups) {
			foreach(const upEntry& ent, grp.entries) have.insert(ent.key);
		}
	}
	QStringList out;
	foreach(const upSection& sec, up_sections(theirs)) {
		if (!sec.version.isEmpty()) {
			int c = up_calver_cmp(sec.version, self.base);
			// a dev build has some of its own version's entries, not all
			if ((c < 0) || ((c == 0) && self.release)) break;
		}
		QStringList body;
		foreach(const upGroup& grp, sec.groups) {
			QStringList ents;
			foreach(const upEntry& ent, grp.entries) {
				if (!have.contains(ent.key)) ents.append(ent.text);
			}
			if (ents.isEmpty()) continue;
			if (!grp.head.isEmpty()) body.append(grp.head);
			body.append(ents);
		}
		if (body.isEmpty()) continue;
		out.append(sec.head);
		out.append(body);
	}
	return out.join("\n\n");
}
