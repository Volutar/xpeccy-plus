#include <string>
#include <vector>
#include <sstream>
#include <stdlib.h>
#include <math.h>
#include <QString>
#include <QDir>
#include <QFile>

#include "xcore.h"
#include "sound.h"

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

// The speed scale. One control for two different things: below x1 the host
// runs emulated time slower, so the frame keeps its length in T and the rate
// drops; above it the cpu gets a multiplier and the frame rate does not move.
// Crossing x1 puts the other one back, or the two would multiply.

double xspeed_mult(int pos) {
	pos = toLimits(pos, 0, XSPD_MAX);
	return (pos < XSPD_CENTER) ? 1.0 / (1 << (XSPD_CENTER - pos))
				  : (double)(1 << (pos - XSPD_CENTER));
}

void xspeed_set(int pos) {
	pos = toLimits(pos, 0, xspeed_max());
	conf.emu.speed = (pos < XSPD_CENTER) ? xspeed_mult(pos) : 1.0;
	sndUpdateSpeed();
	if (conf.zx)
		compSetTurbo(conf.zx, (pos > XSPD_CENTER) ? xspeed_mult(pos) : 1.0);
}

int xspeed_get(void) {
	double m = (conf.emu.speed < 1.0) ? conf.emu.speed
		 : (conf.zx ? conf.zx->frqMul : 1.0);
	for (int i = 0; i <= XSPD_MAX; i++) {
		if (fabs(m - xspeed_mult(i)) < 1e-6) return i;
	}
	return XSPD_CENTER;
}

// What the slider is doing, for the options label and for the message on the
// screen. The screen font has no multiplication sign, so that one asks for ascii.
QString xspeed_name(int pos, bool ascii) {
	if (pos < XSPD_CENTER)
		return QString("Slow motion 1/%0").arg(1 << (XSPD_CENTER - pos));
	if (pos > XSPD_CENTER)
		return QString("Overclock %0%1")
			.arg(ascii ? QString("x") : QString::fromUtf8("×"))
			.arg(1 << (pos - XSPD_CENTER));
	return QString("Normal speed");
}

// How far the slider goes on this machine. The board's own turbo counts towards
// the ceiling, so a board already at x4 is not offered the last step: that one
// is where the host stops keeping up and the machine ends up slower in real time
// than it is at its base clock. Nothing below that is refused - this is a speed
// setting, not a claim about what the hardware could do.
int xspeed_max(void) {
	double hw = (conf.zx && (conf.zx->hwMul > 0.0)) ? conf.zx->hwMul : 1.0;
	int pos = XSPD_MAX;
	while ((pos > XSPD_CENTER) && (xspeed_mult(pos) * hw > XSPD_CLOCK_MAX + 1e-6))
		pos--;
	return pos;
}

// the clock the cpu is really on: the board's own turbo counts here too
double xspeed_clock(void) {
	if (!conf.zx) return 0.0;
	return conf.zx->cpuFrq * conf.zx->frqMul * conf.zx->hwMul;
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
