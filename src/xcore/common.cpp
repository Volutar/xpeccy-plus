#include <string>
#include <vector>
#include <sstream>
#include <stdlib.h>
#include <QString>
#include <QDir>
#include <QFile>

#include "xcore.h"

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

// A file that ships inside the binary and can be replaced by one of the user's
// own: the list a user picks from is both, and the config directory wins on a
// name collision. `kind` is the folder the two share - palettes, shaders,
// styles, keymaps.

#define	XRES_ROOT	":/res/"

QString xres_dir(const char* kind) {
	return QString::fromLocal8Bit(conf.path.confDir.c_str()) + SLASH + kind;
}

QString xres_path(const char* kind, const QString& name) {
	if (name.isEmpty()) return name;
	QString own = xres_dir(kind) + SLASH + name;
	if (QFile::exists(own)) return own;
	return QString(XRES_ROOT) + kind + "/" + name;
}

QStringList xres_list(const char* kind, const QStringList& filt) {
	QStringList res = QDir(xres_dir(kind)).entryList(filt, QDir::Files, QDir::Name);
	foreach(QString nam, QDir(QString(XRES_ROOT) + kind).entryList(filt, QDir::Files, QDir::Name)) {
		if (!res.contains(nam)) res << nam;
	}
	res.sort(Qt::CaseInsensitive);
	return res;
}
