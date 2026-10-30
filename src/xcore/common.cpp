#include <string>
#include <vector>
#include <sstream>
#include <stdlib.h>
#include <math.h>
#include <QString>
#include <QDir>
#include <QFile>
#include <QMutex>

#include "xcore.h"
#include "sound.h"
#include "pacing.h"
#include "autostart.h"
#include "fastload.h"
#include "cheats.h"

static const char* hexhalf = "0123456789ABCDEF";
// static char hexbuf[5] = {'0','0','0','0',0x00};

// unsigned, or a negative number never shifts down to zero and this spins
// until the string eats the memory
QString formbufword(int num) {
	QString res;
	unsigned int val = (unsigned int)num;
	while (val) {
		res.prepend(hexhalf[val & 0x0f]);
		val >>= 4;
	}
	return res;
}

QString gethexint(int num) {
	QString res = formbufword(num);
	return res.rightJustified(8, '0');
}

QString gethex6(int num) {
	QString res = formbufword(num & 0xffffff);
	return res.rightJustified(6, '0');
}

// Resets the user asked for, counted for whoever wants to know one happened.
// Kept out of the machine: rewind and run-ahead put that back, and the count
// would go back with it.
std::atomic<int> xUserResets(0);

// Every write to the machine's keyboard from the gui goes under the emulation's
// lock: the emulation thread runs the ps/2 repeat and reads the scancode log
// meanwhile, and run-ahead puts the whole keyboard back as it was.
void x_key(Computer* comp, keyEntry* ent, int press) {
	cbHwKey cb = press ? comp->hw->keyp : comp->hw->keyr;
	if (!cb) return;
	x_input_bump();
	emu_lock();
	cb(comp, ent);
	emu_unlock();
}

void x_kbd_release_all(Keyboard* kbd) {
	emu_lock();
	kbdReleaseAll(kbd);
	emu_unlock();
}

void x_user_reset(Computer* comp, int res) {
	xUserResets.fetch_add(1);
	x_state_replaced();
	compUserReset(comp, res);
}

// What was kept for the machine as it stood goes when it is reset, switched or
// loaded over: the fast-loading wind back would bring the old state back,
// autostart would go on typing into whatever came instead, and the cheats that
// were on are gone with the memory they were in. autostart_arm()
// resets through x_user_reset() too, before it arms, so it does not cancel itself.
void x_state_replaced() {
	x_input_bump();
	snd_drop();		// the old machine's sound
	autostart_cancel();
	fastload_forget();
	cheats_forget();
}

QString gethexword(int num) {
	QString res = formbufword(num & 0xffff);
	return res.rightJustified(4, '0');
}

QString gethexbyte(int num) {
	QString res = formbufword(num & 0xff);
	return res.rightJustified(2, '0');
}

// A watched port written with four digits is the address on the bus as it is;
// two digits mean a byte port, compared by its low byte alone. It is the number
// of digits that says which, so the width travels in the mask and the two
// round-trip through the profile file

QString getPortString(int port, int mask) {
	return (mask > 0xff) ? gethexword(port) : gethexbyte(port);
}

bool parsePort(const QString& str, int* port, int* mask) {
	QString txt = str.trimmed();
	bool ok = false;
	int prt = txt.toInt(&ok, 16);
	if (!ok || (prt <= 0) || (txt.size() > 4)) return false;
	*port = prt;
	*mask = (txt.size() > 2) ? 0xffff : 0xff;
	return true;
}

QString gethexshift(char shft) {
	QString str = (shft < 0) ? "-" : "+";
	if (shft < 0)
		shft = 256 - shft;
	str.append(gethexbyte(shft & 0x7f));
	return str;
}

QString getdecshift(char shft) {
	QString str = (shft < 0) ? "-" : "+";
	if (shft < 0)
		shft = 256 - shft;
	str.append(QString::number(shft & 0x7f));
	return str;
}

QString getbinbyte(uchar num) {
	return QString::number(num+0x100,2).right(8).toUpper();
}

std::string int2str(int num) {
	std::stringstream str;
	str<<num;
	return str.str();
}

std::string float2str(float num) {
	std::stringstream str;
	str<<num;
	return str.str();
}

int toPower(int src) {
	int dst = 1;
	while (dst < src)
		dst <<= 1;
	return dst;
}

int toLimits(int src, int min, int max) {
	if (src < min) return min;
	if (src > max) return max;
	return src;
}

int getRanged(const char* str, int min, int max) {
	int res = atoi(str);
	return toLimits(res, min, max);
}

double absd(double v) {
	return (v < 0) ? -v : v;
}

std::string getTimeString(int tsec) {
	int tmin = tsec / 60;
	tsec -= tmin * 60;
	std::string res(int2str(tmin));
	res += ":";
	if (tsec < 10) res += "0";
	res += int2str(tsec);
	return res;
}

void setFlagBit(bool cond, int* val, int mask) {
	if (cond) {
		*val |= mask;
	} else {
		*val &= ~mask;
	}
}

// The folder is kept as UTF-8. One that is gone - or a driveless one an older
// build wrote, run from another drive - is not a reason to lose the shot.
QString shot_dir() {
	QString dir = QString::fromStdString(conf.scrShot.dir);
	if (!dir.isEmpty() && QDir(dir).exists()) return dir;
	// once per folder: a screenshot breakpoint may hit every frame, and it
	// comes from the emulation thread
	static QMutex mx;
	static bool told = false;
	static std::string toldDir;
	QMutexLocker lock(&mx);
	if (!told || (toldDir != conf.scrShot.dir)) {
		told = true;
		toldDir = conf.scrShot.dir;
		xlog(XLG_APP, XLL_WARN, "no screenshot folder '%s', saving to home", conf.scrShot.dir.c_str());
	}
	return QDir::homePath();
}

// a number typed by the user: #, $ and 0x are hex, everything else decimal -
// a leading zero is not octal
int str_num(QString str, bool* ok) {
	str = str.trimmed();
	if (str.startsWith("#") || str.startsWith("$"))
		return str.mid(1).toInt(ok, 16);
	if (str.startsWith("0x", Qt::CaseInsensitive))
		return str.mid(2).toInt(ok, 16);
	return str.toInt(ok, 10);
}

bool str2bool(std::string v) {
	return !(v=="n" || v=="N" || v=="0" || v=="no" || v=="NO" || v=="false" || v=="FALSE" || v=="off" || v=="OFF");
}

std::vector<std::string> splitstr(std::string str,const char* spl) {
	size_t pos;
	std::vector<std::string> res;
	pos = str.find_first_of(spl);
	while (pos != std::string::npos) {
		res.push_back(str.substr(0,pos));
		str = str.substr(pos+1);
		pos = str.find_first_of(spl);
	}
	res.push_back(str);
	return res;

}

void ltrim(std::string& str) {
	size_t pos;
	pos = str.find_first_not_of(' ');
	str.erase(0,pos);
}

void rtrim(std::string& str) {
	size_t pos;
	pos = str.find_last_not_of(' ');
	if (pos == std::string::npos) return;
	str.erase(pos+1,std::string::npos);
}

void trim(std::string& str) {
	ltrim(str);
	rtrim(str);
}

std::pair<std::string,std::string> splitline(std::string line, char delim) {
	size_t pos;
	std::pair<std::string,std::string> res;
	do {pos = line.find("\r"); if (pos!=std::string::npos) line.erase(pos);} while (pos!=std::string::npos);
	do {pos = line.find("\n"); if (pos!=std::string::npos) line.erase(pos);} while (pos!=std::string::npos);
	res.first = "";
	res.second = "";
	pos = line.find(delim);
	if (pos!=std::string::npos) {
		res.first = std::string(line,0,pos);
		res.second = std::string(line,pos+1);
	} else {
		res.first = line;
		res.second = "";
	}
	trim(res.first);
	trim(res.second);
	return res;
}

// A file that ships inside the binary with one of the user's own beside it.
// `kind` is the folder the two share - palettes, shaders, styles, keymaps,
// machines. The list a user picks from is both, and for everything read through
// xres_path() the config directory wins outright on a name collision; machines
// read the two halves apart (xres_dir/xres_root) so that the user's file can
// patch the built-in one instead of hiding it.

#define	XRES_ROOT	":/res/"

QString xres_dir(const char* kind) {
	return QString::fromLocal8Bit(conf.path.confDir.c_str()) + SLASH + kind;
}

// The overclock scale: the cpu gets a multiplier and the frame rate does not move.
// Setting it puts emulated time back to normal, or the two would multiply.

double xspeed_mult(int pos) {
	return (double)(1 << toLimits(pos, 0, XSPD_MAX));
}

void xspeed_set(int pos) {
	pos = toLimits(pos, 0, xspeed_max());
	conf.emu.tmode = XTM_NONE;
	conf.emu.speed = 1.0;
	sndUpdateSpeed();
	if (conf.zx)
		compSetTurbo(conf.zx, xspeed_mult(pos));
}

// Slow motion and fast forward are emulated time run slower or faster than the
// host's. They start from normal speed: the overclock goes back to x1 first, and
// moving it again ends them.
void xspeed_toggle(int mode) {
	int was = conf.emu.tmode;
	xspeed_set(XSPD_NORMAL);
	if (was == mode) return;
	conf.emu.tmode = mode;
	conf.emu.speed = (mode == XTM_SLOW) ? 1.0 / conf.emu.slowDiv : (double)conf.emu.ffMul;
	sndUpdateSpeed();
}

const int xtime_slow_steps[] = {2, 4, 8, 0};
const int xtime_ffwd_steps[] = {2, 4, 8, 16, 0};

int xtime_index(const int* list, int f) {
	for (int i = 0; list[i]; i++) {
		if (list[i] == f) return i;
	}
	return -1;
}

int xtime_fit(const int* list, int f) {
	int best = list[0];
	for (int i = 0; list[i]; i++) {
		if (abs(list[i] - f) < abs(best - f)) best = list[i];
	}
	return best;
}

// The Faster/Slower keys: one place along 1/8 .. x1 .. x16 from where emulated time
// stands. The factors set for slow motion and fast forward stay as the user set them.
int xtime_step(int dir) {
	std::vector<double> scale;
	for (int i = 0; xtime_slow_steps[i]; i++)
		scale.insert(scale.begin(), 1.0 / xtime_slow_steps[i]);
	scale.push_back(1.0);
	for (int i = 0; xtime_ffwd_steps[i]; i++)
		scale.push_back(xtime_ffwd_steps[i]);
	double now = conf.emu.speed;
	double f = 0.0;
	if (dir > 0) {
		for (double s : scale) {
			if (s > now + 1e-6) { f = s; break; }
		}
	} else {
		for (double s : scale) {
			if (s < now - 1e-6) f = s;
		}
	}
	if (f == 0.0) return 0;
	xspeed_set(XSPD_NORMAL);	// overclock and either mode off: one speed at a time
	if (f > 1.0)
		conf.emu.tmode = XTM_FFWD;
	else if (f < 1.0)
		conf.emu.tmode = XTM_SLOW;
	conf.emu.speed = f;
	sndUpdateSpeed();
	return 1;
}

// A tap shorter than this switches a hybrid key's mode; a longer press only holds it.
#define XHOLD_TAP_NS	300000000LL

bool xhold_switches(xHoldKey* key, int hold, bool on, bool down) {
	if (down) {
		key->held = true;
		if ((hold == XHOLD_TOGGLE) || !on) {
			key->downAt = paceClockNs();
			return true;
		}
		if (hold != XHOLD_HYBRID) return false;
		key->held = false;		// a tap switched it on, this press off
		return true;
	}
	if (!key->held) return false;
	key->held = false;
	if (!on) return false;
	return (hold == XHOLD_HOLD) || ((hold == XHOLD_HYBRID) && (paceClockNs() - key->downAt >= XHOLD_TAP_NS));
}

void xspeed_key(int mode, int down) {
	static xHoldKey keys[2];
	int hold = (mode == XTM_SLOW) ? conf.emu.slowHold : conf.emu.ffHold;
	if (xhold_switches(&keys[mode == XTM_SLOW], hold, conf.emu.tmode == mode, down))
		xspeed_toggle(mode);
}

// Another tape, disk or machine is another game: it starts at normal speed.
void xspeed_modes_off() {
	if (conf.emu.tmode) xspeed_set(XSPD_NORMAL);
}

int xspeed_get(void) {
	double m = conf.zx ? conf.zx->frqMul : 1.0;
	for (int i = 0; i <= XSPD_MAX; i++) {
		if (fabs(m - xspeed_mult(i)) < 1e-6) return i;
	}
	return XSPD_NORMAL;
}

// What the slider is doing, for the options label and for the message on the
// screen. The screen font has no multiplication sign, so that one asks for ascii.
QString xspeed_name(int pos, bool ascii) {
	pos = toLimits(pos, 0, XSPD_MAX);
	if (pos == XSPD_NORMAL)
		return QString("No overclock");
	return QString("Overclock %0%1")
		.arg(ascii ? QString("x") : QString::fromUtf8("×"))
		.arg(1 << pos);
}

// How far the slider goes on this machine. The board's own turbo counts towards
// the ceiling, so a board already at x4 is not offered the last step: that one
// is where the host stops keeping up and the machine ends up slower in real time
// than it is at its base clock. Nothing below that is refused - this is a speed
// setting, not a claim about what the hardware could do.
int xspeed_max(void) {
	double hw = (conf.zx && (conf.zx->hwMul > 0.0)) ? conf.zx->hwMul : 1.0;
	int pos = XSPD_MAX;
	while ((pos > XSPD_NORMAL) && (xspeed_mult(pos) * hw > XSPD_CLOCK_MAX + 1e-6))
		pos--;
	return pos;
}

// the clock the cpu is really on: the board's own turbo counts here too
double xspeed_clock(void) {
	if (!conf.zx) return 0.0;
	return conf.zx->cpuFrq * conf.zx->frqMul * conf.zx->hwMul;
}

// Is the tape under way? Playing, or standing still with the automatics free to
// take it - flash loading reads block after block without ever turning the motor
// on, so ->on alone left the Stop button greyed out over a tape that was very
// much running. Stop latches tape->userStop and every automatic honours it.
// The tape options are kept in conf and read from there by the trap; these two
// are the copies the tape itself carries. Both windows that can change an option
// go through here.
void tape_apply_options(Tape* tap) {
	if (!tap) return;
	tap->autorew = conf.tape.rewind;
	tap->detectOn = conf.tape.autostart;
	tap->flash = tape_flash();
}

int tape_running(Tape* tap) {
	if (!tap || (tap->blkCount < 1)) return 0;
	if (tap->on) return 1;
	return !tap->userStop && (conf.tape.autostart || tape_flash());
}

// "3.5469 MHz", or anything a person types into that box. Out of range or
// unreadable keeps what the machine already had rather than inventing a clock.
double xcpu_frq_parse(const QString& txt, double def) {
	bool ok = false;
	double v = QString(txt).remove("MHz", Qt::CaseInsensitive).trimmed().toDouble(&ok);
	if (!ok || (v < 0.1) || (v > 28.0)) return def;
	return v;
}

QString xres_root(const char* kind) {
	return QString(XRES_ROOT) + kind;
}

QString xres_path(const char* kind, const QString& name) {
	if (name.isEmpty()) return name;
	QString own = xres_dir(kind) + SLASH + name;
	if (QFile::exists(own)) return own;
	return xres_root(kind) + "/" + name;
}

QStringList xres_list(const char* kind, const QStringList& filt) {
	QStringList res = QDir(xres_dir(kind)).entryList(filt, QDir::Files, QDir::Name);
	foreach(QString nam, QDir(xres_root(kind)).entryList(filt, QDir::Files, QDir::Name)) {
		if (!res.contains(nam)) res << nam;
	}
	res.sort(Qt::CaseInsensitive);
	return res;
}
