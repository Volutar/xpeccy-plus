// Getting FFmpeg for the video recorder. Windows: the newest release of BtbN's
// static build into the config folder, checked against its sha256, with the
// curl and tar every Windows 10 has. Linux: the distribution's package through
// pkexec. macOS: Homebrew.

#include <QApplication>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QMap>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <functional>

#include "xgui.h"
#include "../xcore/xcore.h"
#include "../xcore/vidrec.h"

#define FG_BASE		"https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/"
#define FG_MASTER	"ffmpeg-master-latest-win64-gpl.zip"

class xFfGet : public QDialog {
	public:
		xFfGet(QWidget* p);
		bool installed;
		void reject() override;
	private:
		QLabel* lab;
		QLabel* labLine;	// what the program says, as it goes
		QProgressBar* bar;
		QDialogButtonBox* box;
		QPushButton* btnGo;
		QPushButton* btnLatest;	// Windows: the newest release, btnGo the one before
		bool latest;
		QProcess* prc;
		bool running;
		QByteArray out;		// the step's stdout, for the one step that reads it
		QString tail;		// the program's last line, what a failure shows
		QString offer();
		void go();
		void run(const QString& prog, const QStringList& args, std::function<void()> next);
		void output(const QByteArray& chunk);
		void finish(const QString& html);
		void fail(const QString& msg);
		void ready(const QString& prog);
#ifdef _WIN32
		QString tmp;		// where the download goes
		QString name;
		QString sha;
		void getList();
		void check();
		void install();
#else
		QString pm;
		QStringList pmArgs;
#endif
};

#ifdef _WIN32
// the system's own, not one a shell put first on PATH
static QString fg_system_tool(const QString& exe) {
	QString path = qEnvironmentVariable("SystemRoot") + "/System32/" + exe;
	return QFileInfo(path).isFile() ? path : QStandardPaths::findExecutable(exe);
}
#else
// what installs FFmpeg here, and how it is told to without asking
static bool fg_manager(QString* pm, QStringList* args) {
#ifdef __APPLE__
	*pm = vrec_find_tool("brew");
	*args = QStringList() << "install" << "ffmpeg";
	return !pm->isEmpty();
#else
	static const struct {const char* pm; const char* args;} tab[] = {
		{"apt-get", "install -y ffmpeg"},
		{"dnf", "install -y ffmpeg"},
		{"pacman", "-S --noconfirm --needed ffmpeg"},
		{"zypper", "--non-interactive install ffmpeg"},
	};
	QString pkexec = QStandardPaths::findExecutable("pkexec");
	for (const auto& row : tab) {
		QString path = QStandardPaths::findExecutable(row.pm);
		if (path.isEmpty()) continue;
		// pkexec clears the environment, so apt's quiet mode goes through env
		*pm = pkexec;
		*args = QStringList() << "env" << "DEBIAN_FRONTEND=noninteractive" << path << QString(row.args).split(' ');
		return !pkexec.isEmpty();
	}
	return false;
#endif
}
#endif

xFfGet::xFfGet(QWidget* p):QDialog(p) {
	setWindowTitle(tr("Get FFmpeg"));
	installed = false;
	running = false;
	prc = NULL;
	latest = false;
	btnLatest = NULL;
	QVBoxLayout* lay = new QVBoxLayout(this);
	lab = new QLabel;
	lab->setWordWrap(true);
	lab->setTextInteractionFlags(Qt::TextBrowserInteraction);
	lab->setOpenExternalLinks(true);
	lab->setMinimumWidth(420);
	lay->addWidget(lab);
	bar = new QProgressBar;
	bar->setTextVisible(false);
	bar->hide();
	lay->addWidget(bar);
	labLine = new QLabel;
	QFont fnt = labLine->font();
	fnt.setItalic(true);
	labLine->setFont(fnt);
	labLine->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	labLine->hide();
	lay->addWidget(labLine);
	box = new QDialogButtonBox(QDialogButtonBox::Cancel);
#ifdef _WIN32
	btnGo = box->addButton(tr("Get previous"), QDialogButtonBox::AcceptRole);
	btnLatest = box->addButton(tr("Get latest"), QDialogButtonBox::AcceptRole);
	connect(btnLatest, &QPushButton::clicked, this, [this]() {
		latest = true;
		go();
	});
#else
	btnGo = box->addButton(tr("Get it"), QDialogButtonBox::AcceptRole);
#endif
	btnGo->setDefault(true);
	// the icons Options and the Drives menu give theirs
	foreach(QPushButton* btn, QList<QPushButton*>() << btnGo << btnLatest)
		if (btn) btn->setIcon(QIcon(":/images/arrow-down.png"));
	box->button(QDialogButtonBox::Cancel)->setIcon(QIcon(":/images/cancel.png"));
	lay->addWidget(box);
	connect(box, &QDialogButtonBox::rejected, this, &xFfGet::reject);
	connect(btnGo, &QPushButton::clicked, this, &xFfGet::go);
	lab->setText(offer());
}

// what will be done, asked before it is
QString xFfGet::offer() {
	QString head = tr("Video recording runs FFmpeg, which is not part of Xpeccy+.") + "<br><br>";
#ifdef _WIN32
	return head + tr("Download a release of the build at "
		"<a href=\"https://github.com/BtbN/FFmpeg-Builds\">github.com/BtbN/FFmpeg-Builds</a> "
		"(about 190&nbsp;MB, GPL) into the config folder?") + "<br><br>"
		+ tr("The latest release is built for the newest graphics drivers: with an older one, the "
		"card encoders (NVENC, AMF, QSV) may not run. The previous release works with older drivers "
		"and records just as well.");
#else
	if (fg_manager(&pm, &pmArgs)) {
#ifdef __APPLE__
		return head + tr("Install it with Homebrew?");
#else
		return head + tr("Install it with %0? The system asks for your password.").arg(QFileInfo(pmArgs.at(2)).fileName());
#endif
	}
	btnGo->hide();
#ifdef __APPLE__
	return head + tr("It comes from Homebrew: install that from <a href=\"https://brew.sh\">brew.sh</a>, "
		"then run <b>brew install ffmpeg</b>.");
#else
	return head + tr("Install the <b>ffmpeg</b> package with your package manager.");
#endif
#endif
}

void xFfGet::run(const QString& prog, const QStringList& args, std::function<void()> next) {
	if (prog.isEmpty()) {
		fail(tr("A tool this needs is missing"));
		return;
	}
	// the step before may be the one finishing now
	if (prc) prc->deleteLater();
	prc = new QProcess(this);
	tail.clear();
	out.clear();
	connect(prc, &QProcess::readyReadStandardOutput, this, [this]() {
		QByteArray chunk = prc->readAllStandardOutput();
		out.append(chunk);
		output(chunk);
	});
	connect(prc, &QProcess::readyReadStandardError, this, [this]() {
		output(prc->readAllStandardError());
	});
	connect(prc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, next](int code, QProcess::ExitStatus st) {
		if (!running) return;		// cancelled
		if ((st != QProcess::NormalExit) || (code != 0)) {
			fail(tail.isEmpty() ? tr("It stopped with code %0").arg(code) : tail);
		} else {
			next();
		}
	});
	connect(prc, &QProcess::errorOccurred, this, [this, prog](QProcess::ProcessError err) {
		if (running && (err == QProcess::FailedToStart))
			fail(tr("%0 does not start").arg(QFileInfo(prog).fileName()));
	});
	prc->start(prog, args);
}

// a percentage moves the bar, any other line is kept for a failure to show
void xFfGet::output(const QByteArray& chunk) {
	static QRegularExpression pct("(\\d+(?:\\.\\d+)?)%\\s*$");
	foreach(QString line, QString::fromLocal8Bit(chunk).split(QRegularExpression("[\r\n]"), X_SkipEmptyParts)) {
		line = line.trimmed();
		if (line.isEmpty()) continue;
		QRegularExpressionMatch m = pct.match(line);
		if (m.hasMatch()) {
			bar->setValue(int(m.captured(1).toDouble() * 10));
		} else {
			tail = line;
			labLine->setText(line);
		}
	}
}

void xFfGet::go() {
	running = true;
	btnGo->hide();
	if (btnLatest) btnLatest->hide();
	bar->setRange(0, 0);
	bar->show();
#ifdef _WIN32
	tmp = QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/ffmpeg.part/";
	QDir(tmp).removeRecursively();
	if (!QDir().mkpath(tmp)) {
		fail(tr("Cannot write into the config folder"));
		return;
	}
	lab->setText(tr("Looking up the latest release"));
	run(fg_system_tool("curl.exe"), QStringList() << "-fsSL" << FG_BASE "checksums.sha256", [this]() { getList(); });
#else
	// a package manager has the system's lock: it is left to finish, not stopped midway
	box->button(QDialogButtonBox::Cancel)->setEnabled(false);
	labLine->show();
	lab->setText(tr("Installing FFmpeg"));
	run(pm, pmArgs, [this]() {
		QString path = vrec_ffmpeg_auto();
		if (path.isEmpty()) {
			fail(tr("It was installed, but no ffmpeg is on the PATH"));
		} else {
			ready(path);
		}
	});
#endif
}

#ifdef _WIN32
// The newest release in the list, or the newest of the major version before
// it; the development build if the names ever change. BtbN builds the newest
// major version against the newest NVENC, which wants a driver most people
// do not have yet.
void xFfGet::getList() {
	static QRegularExpression rx("^([0-9a-f]{64})\\s+(ffmpeg-n(\\d+)\\.(\\d+)-latest-win64-gpl-[\\d.]+\\.zip)$");
	QMap<int, QPair<QString, QString> > rel;	// major * 1000 + minor: sha, name
	QString master;
	foreach(QString line, QString::fromLatin1(out).split('\n')) {
		line = line.trimmed();
		if (line.endsWith(" " FG_MASTER)) master = line.section(' ', 0, 0);
		QRegularExpressionMatch m = rx.match(line);
		if (m.hasMatch())
			rel[m.captured(3).toInt() * 1000 + m.captured(4).toInt()] = qMakePair(m.captured(1), m.captured(2));
	}
	if (!rel.isEmpty()) {
		int pick = rel.lastKey();
		// the keys go up: the one just below the latest major version is the newest before it
		QMap<int, QPair<QString, QString> >::iterator it = rel.lowerBound(pick / 1000 * 1000);
		if (!latest && (it != rel.begin())) pick = (--it).key();
		sha = rel[pick].first;
		name = rel[pick].second;
	}
	if (name.isEmpty() && (master.length() == 64)) {
		sha = master;
		name = FG_MASTER;
	}
	if (name.isEmpty()) {
		fail(tr("No Windows build in the release list"));
		return;
	}
	bar->setRange(0, 1000);
	bar->setValue(0);
	lab->setText(tr("Downloading %0").arg(name));
	run(fg_system_tool("curl.exe"), QStringList() << "-fL" << "--progress-bar" << "-o" << QDir::toNativeSeparators(tmp + name) << FG_BASE + name,
		[this]() { check(); });
}

void xFfGet::check() {
	lab->setText(tr("Checking the download"));
	bar->setRange(0, 0);
	QFile file(tmp + name);
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (file.open(QFile::ReadOnly)) {
		while (!file.atEnd()) {
			hash.addData(file.read(1 << 20));
			QApplication::processEvents();
			if (!running) return;		// cancelled meanwhile
		}
		file.close();
	}
	if (hash.result().toHex() != sha.toLatin1()) {
		fail(tr("The download is damaged, its checksum does not match"));
		return;
	}
	lab->setText(tr("Unpacking"));
	QDir().mkpath(tmp + "x");
	run(fg_system_tool("tar.exe"), QStringList() << "-xf" << QDir::toNativeSeparators(tmp + name)
		<< "-C" << QDir::toNativeSeparators(tmp + "x") << "--strip-components" << "1" << "*/bin/ffmpeg.exe" << "*/LICENSE.txt",
		[this]() { install(); });
}

// into <config>/ffmpeg/bin/, where vrec_ffmpeg_auto() looks
void xFfGet::install() {
	QString dir = vrec_ffmpeg_dir();
	QString exe = dir + "bin/ffmpeg.exe";
	QDir().mkpath(dir + "bin");
	QFile::remove(exe);
	QFile::remove(dir + "LICENSE.txt");
	if (!QFile::rename(tmp + "x/bin/ffmpeg.exe", exe)) {
		fail(tr("Cannot put ffmpeg.exe into %0").arg(QDir::toNativeSeparators(dir + "bin")));
		return;
	}
	QFile::rename(tmp + "x/LICENSE.txt", dir + "LICENSE.txt");
	QDir(tmp).removeRecursively();
	ready(QDir::cleanPath(exe));
}
#endif

// Asked what it can do before the dialog says it is ready, so Options has the
// answer at once: the file is new, and a copy in the same place is new too
void xFfGet::ready(const QString& prog) {
	lab->setText(tr("Checking what it runs here"));
	bar->setRange(0, 0);
	QApplication::processEvents();
	vrecProbe res = vrec_probe(prog, conf.rec);
	if (res.version.isEmpty()) {
		fail(tr("The program installed does not run"));
		return;
	}
	vrec_probe_keep(prog, res);
	installed = true;
	finish(tr("FFmpeg %0 is ready.").arg(vrec_ffmpeg_release(res.version)).toHtmlEscaped());
}

// the end either way: what came of it, and Cancel is only a way out now
void xFfGet::finish(const QString& html) {
	running = false;
	bar->hide();
	labLine->hide();
	lab->setText(html);
	QPushButton* btn = box->button(QDialogButtonBox::Cancel);
	btn->setEnabled(true);
	btn->setText(tr("Close"));
}

void xFfGet::fail(const QString& msg) {
#ifdef _WIN32
	if (!tmp.isEmpty()) QDir(tmp).removeRecursively();
#endif
	finish(tr("FFmpeg was not installed.") + "<br><br>" + msg.toHtmlEscaped());
}

// cancel: the download is stopped and what it left is taken away
void xFfGet::reject() {
#ifndef _WIN32
	if (running) return;		// a package manager is left to finish
#endif
	running = false;
	if (prc && (prc->state() != QProcess::NotRunning)) {
		prc->kill();
		prc->waitForFinished(2000);
	}
#ifdef _WIN32
	if (!tmp.isEmpty()) QDir(tmp).removeRecursively();
#endif
	QDialog::reject();
}

// true when an FFmpeg is there to be used afterwards; the settings go back to
// Auto, which finds it, so a path that no longer runs does not stand in its way
bool ffmpeg_get(QWidget* parent) {
	xFfGet dlg(parent);
	dlg.exec();
	if (dlg.installed) conf.rec.ffmpeg.clear();
	return dlg.installed;
}
