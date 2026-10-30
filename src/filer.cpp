#include "filer.h"
#include "xcore/rzxrecord.h"
#include "xcore/xcore.h"
#include "xcore/autostart.h"
#include "xcore/vfat_scan.h"
#include "xcore/filemachine.h"
#include "xcore/cheats.h"
#include "xcore/sound.h"
#include "libxpeccy/cpu/Z80/z80.h"
#include "libxpeccy/filetypes/szx.h"
#include "libxpeccy/xstate.h"
#include "xgui/xgui.h"

#include <QDebug>
#include <QDialog>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QTreeView>
#include <QVBoxLayout>

static QFileDialog* fileDlg = nullptr;
static QWidget* fileDlgParent = nullptr;

// Built on first use, in the folder it is about to show: a Qt dialog built
// without one lists and watches the cwd on the GUI thread, and started from a
// 37k-file folder the window froze for seconds
static QFileDialog* file_dialog() {
	if (fileDlg) return fileDlg;
	QString dir = QString::fromLocal8Bit(conf.lastDir.c_str());
	if (dir.isEmpty() || !QFileInfo(dir).isDir())
		dir = QString::fromLocal8Bit(conf.path.confDir.c_str());
	fileDlg = new QFileDialog(fileDlgParent, QString(), dir);
	fileDlg->setWindowIcon(QIcon(":/images/logo.png"));
	fileDlg->setWindowModality(Qt::ApplicationModal);
	fileDlg->setOption(QFileDialog::HideNameFilterDetails, false);
	fileDlg->setOption(QFileDialog::DontConfirmOverwrite, false);
	fileDlg->setOption(QFileDialog::DontUseNativeDialog, true);
	fileDlg->resize(700, 480);
	return fileDlg;
}

// new

typedef struct {
	int id;
	int ch;
	const char* ext;
	const char* filt;
	int(*load)(Computer*, const char*, int);
	int(*save)(Computer*, const char*, int);
	const char* name;
} xFileTypeInfo;

typedef struct {
	int id;
	const char* defext;
	int drv;
	const char* name;
	xFileTypeInfo* fti;
	int child[32];
} xFileGroupInfo;

typedef struct {
	int id;
	int child[32];
} xFileHWInfo;

// Save takes the settings the export dialog holds, so the two roads to a wav
// do not write different files
static int save_wav(Computer* comp, const char* name, int drv) {
	return saveWAVopt(comp, name, &conf.tape.exp);
}

// An .szx links to the tape and the disks it was taken with, by their names on
// this host, the way Spectaculator writes them.
// the file in memory, with the images the machine has in linked by name
static int build_szx(Computer* comp, szxBuf* b) {
	QByteArray tape;
	QByteArray disk[4];
	const char* dptr[4];
	if (comp->tape->path && comp->tape->blkCount)
		tape = QDir::toNativeSeparators(QString::fromLocal8Bit(comp->tape->path)).toLocal8Bit();
	for (int i = 0; i < 4; i++) {
		Floppy* flp = comp->dif->flp[i];
		if (flp->insert && flp->path)
			disk[i] = QDir::toNativeSeparators(QString::fromLocal8Bit(flp->path)).toLocal8Bit();
		dptr[i] = disk[i].isEmpty() ? NULL : disk[i].constData();
	}
	szx_set_links(tape.isEmpty() ? NULL : tape.constData(), dptr);
	szx_set_machine(conf.macId.c_str());
	int res = szx_build(comp, b);
	szx_set_links(NULL, NULL);
	return res;
}

static int save_szx(Computer* comp, const char* name, int drv) {
	szxBuf b;
	int res = build_szx(comp, &b);
	if (res == ERR_OK)
		res = szx_write(&b, name);
	return res;
}

// A .nex or .snx is started as on the machine: NextZXOS boots from the card, and on its
// command line .cd goes into the file's folder and NEXLOAD or SPECTRUM starts it - the
// program finds its own files, and an open file handle, where it expects them. A file
// on the card itself starts where it is, so what it saves stays there; one from
// elsewhere has its folder put onto the card as a folder of the root.
// a node of a folder card by its name, without case as the guest sees it; -1 if none
static int vf_child(vFat* vf, int dir, const char* name) {
	for (int i = (dir >= 0) ? vf->node[dir].child : -1; i >= 0; i = vf->node[i].next) {
		if (!QString::compare(QString::fromUtf8(vf->node[i].name), QString::fromUtf8(name), Qt::CaseInsensitive)) return i;
	}
	return -1;
}

// started from dir (a node of the card, cd the path NextZXOS goes there by): by its
// name, else by the card's 8.3 name for it, which any name has and the keyboard can type
static int nex_arm(Computer* comp, int dir, const QByteArray& cd, const QFileInfo& inf, int welcome, int snx, int powered) {
	vFat* vf = comp->sdc->vfat;
	if (autostart_arm_nex(comp, cd.constData(), inf.fileName().toLocal8Bit().constData(), welcome, snx, powered)) return ERR_OK;
	QByteArray want = inf.fileName().toUtf8();
	for (int i = (dir >= 0) ? vf->node[dir].child : -1; i >= 0; i = vf->node[i].next) {
		if (vf->node[i].isdir || (want != vf->node[i].name)) continue;
		QString sn = QString::fromLatin1(vf->node[i].sname, 8).trimmed();
		QString ext = QString::fromLatin1(vf->node[i].sname + 8, 3).trimmed();
		if (!ext.isEmpty()) sn += "." + ext;
		return autostart_arm_nex(comp, cd.constData(), sn.toLatin1().constData(), welcome, snx, powered) ? ERR_OK : ERR_NEX_NAME;
	}
	return ERR_NEX_NAME;
}

static int load_next(Computer* comp, const char* name, int snx) {
	if (comp->hw->id != HW_ZXNEXT) return ERR_NEX_HW;
	if (!comp->sdc->vfat) return ERR_NEX_CARD;
	QFileInfo inf(QString::fromLocal8Bit(name));
	// a card nobody has booted yet runs its welcome pages until they are turned off
	// with D, which a card served read only cannot keep
	auto welcome = [comp]() {
		vFat* vf = comp->sdc->vfat;
		return (vf_child(vf, vf_child(vf, 0, "nextzxos"), "autoexec.1st") >= 0) ? 1 : 0;
	};
	// on the card: its folder there, found by name down from the root
	QString card = QDir(QString::fromLocal8Bit(comp->sdc->image)).canonicalPath();
	QString folder = inf.absoluteDir().canonicalPath();
	if (!card.isEmpty() && ((folder == card) || folder.startsWith(card + "/"))) {
		QString rel = QDir(card).relativeFilePath(folder);
		if (rel == ".") rel.clear();
		int node = 0;
		foreach(QString part, rel.split('/', X_SkipEmptyParts))
			node = vf_child(comp->sdc->vfat, node, part.toUtf8().constData());
		if ((node >= 0) && (rel.toLatin1() == rel.toUtf8())) {
			int res = nex_arm(comp, node, ("/" + rel).toLatin1(), inf, welcome(), snx, 0);
			if (res == ERR_OK) return res;
		}
		// a path the keyboard cannot type: as a file from elsewhere
	}
	// the folder's name, as the keyboard types it, and one the card's root has not got
	QString base;
	foreach(QChar c, inf.absoluteDir().dirName().toLower()) {
		if (((c >= 'a') && (c <= 'z')) || ((c >= '0') && (c <= '9')) || (c == '_') || (c == '-')) base += c;
	}
	base = base.left(20);
	if (base.isEmpty()) base = "nex";
	QStringList taken = QDir(QString::fromLocal8Bit(comp->sdc->image)).entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
	QString dir = base;
	for (int i = 2; taken.contains(dir, Qt::CaseInsensitive); i++)
		dir = base + QString::number(i);
	// The card changes under the machine, which a soft reset does not tell: NextZXOS's
	// disk layer keeps in memory what it read of the old one, and the folder put on is
	// not found ("No such file or dir"). So off and on first, the card read again (as
	// Read-only or not, as the user said), then the folder put on.
	xm_power_cycle();
	if (!sdc_mount_over(comp->sdc, inf.absolutePath(), dir)) return ERR_NEX_CARD;
	return nex_arm(comp, vf_child(comp->sdc->vfat, 0, dir.toUtf8().constData()), dir.toLatin1(), inf, welcome(), snx, 1);
}

static int load_nex(Computer* comp, const char* name, int drv) {
	return load_next(comp, name, 0);
}

// a .snx is a .sna that NextZXOS loads with the Next left as it is (no 48K/128K
// modes) and the file left open for the program
static int load_snx(Computer* comp, const char* name, int drv) {
	return load_next(comp, name, 1);
}

// a .sna or .z80 on the Next goes the same way: its power-on boots the firmware over
// anything put into memory, and SPECTRUM is how NextZXOS opens one
static int load_sna(Computer* comp, const char* name, int drv) {
	return (comp->hw->id == HW_ZXNEXT) ? load_next(comp, name, 1) : loadSNA(comp, name, drv);
}

static int load_z80(Computer* comp, const char* name, int drv) {
	return (comp->hw->id == HW_ZXNEXT) ? load_next(comp, name, 1) : loadZ80(comp, name, drv);
}

static xFileTypeInfo ft_tab[] = {
	{FL_SNA, 0, ".sna", "*.sna", load_sna, saveSNA, "SNA snapshot"},
	{FL_Z80, 0, ".z80", "*.z80", load_z80, saveZ80, "Z80 snapshot"},
	{FL_SZX, 0, ".szx", "*.szx", loadSZX, save_szx, "SZX snapshot"},
	{FL_SPG, 0, ".spg", "*.spg", loadSPG, NULL, "SPG snapshot"},
	{FL_NEX, 0, ".nex", "*.nex", load_nex, NULL, "NEX program"},
	{FL_SNX, 0, ".snx", "*.snx", load_snx, NULL, "SNX snapshot"},
	{FL_TAP, 0, ".tap", "*.tap", loadTAP, saveTAP, "TAP tape image"},
	{FL_TZX, 0, ".tzx", "*.tzx", loadTZX, NULL, "TZX tape image"},
	{FL_WAV, 0, ".wav", "*.wav", loadWAV, save_wav, "WAV recording"},
	{FL_SCL, 1, ".scl", "*.scl", loadSCL, saveSCL, "SCL disk image"},
	{FL_TRD, 1, ".trd", "*.trd", loadTRD, saveTRD, "TRD disk image"},
	{FL_TD0, 1, ".td0", "*.td0", loadTD0, NULL, "TD0 disk image"},
	{FL_FDI, 1, ".fdi", "*.fdi", loadFDI, NULL, "FDI disk image"},
	{FL_UDI, 1, ".udi", "*.udi", loadUDI, saveUDI, "UDI disk image"},
	{FL_DSK, 1, ".dsk", "*.dsk", loadDSK, saveDSK, "DSK disk image"},
	{FL_HOBETA, 0, ".$", "*.$?", loadHobeta, NULL, "Hobeta file"},
	{FL_SLT_ROM, 0, ".rom", "*.rom", loadSlot, NULL, "Cartridge image"},
	{FL_SLT_BIN, 0, ".bin", "*.bin", loadSlot, NULL, "Cartridge image"},
	{FL_IMA, 1, ".ima", "*.ima", load_ima, NULL, "1.44 FDD image"},
	{FL_PCIMG, 1, ".img", "*.img", load_ima, NULL, "1.44 FDD image"},
	{FL_RZX, 0, ".rzx", "*.rzx", loadRZX, NULL, "RZX playback"},
	{FL_POK, 0, ".pok", "*.pok", NULL, NULL, "Cheats"},
	{FL_RAW, 0, NULL, "*", loadRaw, NULL, "RAW file to TR-DOS disk"},			// * for all files; *.* for all files that have extension
	{0, 0, NULL, NULL, NULL, NULL, NULL}
};

static xFileTypeInfo ft_sltraw = {FL_SLT_BIN, 0, NULL, NULL, loadSlot, NULL, "RAW cartridge image"};
static xFileTypeInfo ft_raw = {FL_RAW, 0, NULL, NULL, loadRaw, NULL, "RAW file to disk A"};
static xFileTypeInfo ft_dum = {FL_NONE, 0, NULL, NULL, NULL, NULL, "Dummy entry"};

// 3rd parameter
// < 0 : allways on
// 0..3 : disk (must be inserted for save)
// 4 : tape (block count > 0)
static xFileGroupInfo fg_tab[] = {
	{FG_SNAPSHOT, ".szx", -1, "Snapshot", NULL, {FL_SZX, FL_SNA, FL_Z80, FL_SPG, FL_NEX, FL_SNX, 0}},
	{FG_TAPE, ".tap", 4, "Tape", NULL, {FL_TAP, FL_TZX, FL_WAV, 0}},
	{FG_DISK_A, ".trd", 0,"Disk A", NULL, {FL_SCL, FL_TRD, FL_TD0, FL_FDI, FL_UDI, FL_DSK, FL_IMA, FL_PCIMG, FL_HOBETA, 0}},
	{FG_DISK_B, ".trd", 1, "Disk B", NULL, {FL_SCL, FL_TRD, FL_TD0, FL_FDI, FL_UDI, FL_DSK, FL_IMA, FL_PCIMG, FL_HOBETA, 0}},
	{FG_DISK_C, ".trd", 2, "Disk C", NULL, {FL_SCL, FL_TRD, FL_TD0, FL_FDI, FL_UDI, FL_DSK, FL_IMA, FL_PCIMG, FL_HOBETA, 0}},
	{FG_DISK_D, ".trd", 3, "Disk D", NULL, {FL_SCL, FL_TRD, FL_TD0, FL_FDI, FL_UDI, FL_DSK, FL_IMA, FL_PCIMG, FL_HOBETA, 0}},
	{FG_RAW, "", 0, "RAW file to disk", &ft_raw, {FL_RAW, 0}},
	{FG_IF2_ROM, "", -1, "Cartridge image", &ft_sltraw, {FL_SLT_ROM, FL_SLT_BIN, 0}},
	{FG_RZX, "", -1, "RZX playback", NULL, {FL_RZX, 0}},
	{FG_POK, "", -1, "Cheats", NULL, {FL_POK, 0}},
	{0, "", -1, NULL, NULL, {0}}
};

static xFileGroupInfo fg_dum = {0, "", -1, NULL, NULL, {0}};

// a wav is a recording of the tape, not its image: saved only as an export
static bool ft_export(xFileTypeInfo* inf) {
	return inf && (inf->id == FL_WAV);
}

static xFileHWInfo fh_tab[] = {
	{FH_SPECTRUM, {FG_SNAPSHOT, FG_TAPE, FG_DISK_A, FG_DISK_B, FG_DISK_C, FG_DISK_D, FG_RAW, FG_RZX, FG_IF2_ROM, FG_POK, 0}},
	{FH_ALF, {FG_IF2_ROM, FG_SNAPSHOT, 0}},
	{FH_SLOTS, {FG_IF2_ROM, 0}},
	{FH_DRIVE_A, {FG_DISK_A, FG_RAW, 0}},
	{FH_DRIVE_B, {FG_DISK_B, FG_RAW, 0}},
	{FH_DRIVE_C, {FG_DISK_C, FG_RAW, 0}},
	{FH_DRIVE_D, {FG_DISK_D, FG_RAW, 0}},
	{0, {0}}
};

static xFileHWInfo hw_tab[] = {
	{FH_SPECTRUM, {HW_ATM1, HW_ATM2, HW_P1024, HW_PENT, HW_PENTEVO, HW_PHOENIX, HW_PLUS2A, HW_PLUS3, HW_PROFI, HW_PROFI3, HW_SCORP, HW_SCORPTP, HW_TSLAB, HW_ZX48, HW_ZX128, HW_ZXNEXT, 0}},
	{FH_ALF, {HW_ALF, 0}},
	{0, {0}}
};

int detect_hw_id(int hwid) {
	int i = 0;
	int x;
	int id = 0;
	while (hw_tab[i].id > 0) {
		x = 0;
		while (hw_tab[i].child[x] > 0) {
			if (hw_tab[i].child[x] == hwid)
				id = hw_tab[i].id;
			x++;
		}
		i++;
	}
	return id;
}

xFileHWInfo* file_find_hw(int id) {
	int i = 0;
	while ((fh_tab[i].id > 0) && (fh_tab[i].id != id))
		i++;
	return &fh_tab[i];
}

xFileGroupInfo* file_find_group(int id) {
	int i = 0;
	while ((fg_tab[i].id > 0) && (fg_tab[i].id != id))
		i++;
	return &fg_tab[i];
}

xFileGroupInfo* file_detect_grp(QString flt) {
	int i = 0;
	xFileGroupInfo* inf = &fg_dum;
	while (fg_tab[i].id > 0) {
		if (flt.startsWith(fg_tab[i].name, Qt::CaseInsensitive))
			inf = &fg_tab[i];
		i++;
	}
	return inf;
}

xFileTypeInfo* file_find_type(int id) {
	int i = 0;
	while ((ft_tab[i].id > 0) && (ft_tab[i].id != id))
		i++;
	return &ft_tab[i];
}

xFileTypeInfo* file_detect_type(QString flt) {
	int i = 0;
	xFileTypeInfo* inf = &ft_dum;
	while (ft_tab[i].id > 0) {
		if (flt.startsWith(ft_tab[i].name, Qt::CaseInsensitive)) {
			inf = &ft_tab[i];
		}
		i++;
	}
	return inf;
}

xFileTypeInfo* file_find_hw_ext(int hw, QString path) {
	xFileTypeInfo* inf = NULL;
	xFileHWInfo* hwinf;
	xFileGroupInfo* fginf;
	xFileTypeInfo* ftinf;
	QString ext = path.split(".").last();
	int ig;
	int iz;
	hw = detect_hw_id(hw);
	if (hw != 0) {
		hwinf = file_find_hw(hw);
		ig = 0;
		while ((hwinf->child[ig] != 0) && (inf == NULL)) {
			fginf = file_find_group(hwinf->child[ig]);
			iz = 0;
			while ((fginf->child[iz] != 0) && (inf == NULL)) {
				ftinf = file_find_type(fginf->child[iz]);
				if (ftinf->id != 0) {
					if (ftinf->id == FL_HOBETA) {
						if (ext.startsWith("$"))
							inf = ftinf;
					} else if (ftinf->ext == NULL) {

					} else if (path.endsWith(ftinf->ext, Qt::CaseInsensitive)) {
						inf = ftinf;
					}
				}
				iz++;
			}
			ig++;
		}
	}
	return inf;
}

// what a file opened as id is: the group's own type when it has one (a raw
// file, a cartridge), otherwise whatever the extension says
static xFileTypeInfo* file_type_of(Computer* comp, int id, const QString& path) {
	xFileGroupInfo* grp = file_find_group(id);	// not a group: the empty one
	return grp->fti ? grp->fti : file_find_hw_ext(comp->hw->id, path);
}

static QString allfilt;

QString file_get_type_filter(int id, int sv) {
	QString flt;
	xFileTypeInfo* inf = file_find_type(id);
	if (inf->filt && ((!sv && inf->load) || (sv && inf->save))) {
		flt = QString(inf->filt);
		if (!allfilt.contains(inf->filt) && inf->ext)
			allfilt.append(inf->filt).append(" ");
	}
	return flt;
}

QString file_get_group_filter(Computer* comp, int id, int sv) {
	QStringList flt;
	QStringList exf;
	QString ftf;
	QString ofl;
	int i;
	xFileGroupInfo* inf = file_find_group(id);
	if (inf->id > 0) {
		if (!sv || (inf->drv < 0) || (sv && !(inf->drv & ~3) && comp->dif->flp[inf->drv & 3]->insert) || ((inf->drv == 4) && comp->tape->blkCount > 0)) {
			i = 0;
			while(inf->child[i] > 0) {
				ofl = file_get_type_filter(inf->child[i], sv);
				xFileTypeInfo* tin = file_find_type(inf->child[i]);
				if (!ofl.isEmpty() && sv && ft_export(tin))
					exf.append(QString("%0, export (%1)").arg(tin->name, ofl));
				else if (!ofl.isEmpty())
					ftf.append(ofl).append(" ");
				i++;
			}
			ftf = ftf.trimmed();
			if (!ftf.isEmpty())
				flt.append(QString("%0 (%1)").arg(inf->name, ftf));
			flt.append(exf);
		}
	}
	return flt.join(";;");
}

QString file_get_hw_filter(Computer* comp, int id, int sv) {
	QStringList flt;
	QString gfl;
	allfilt.clear();
	int i;
	xFileHWInfo* inf = file_find_hw(id);
	if (inf->id > 0) {
		i = 0;
		while (inf->child[i] > 0) {
			gfl = file_get_group_filter(comp, inf->child[i], sv);
			if (!gfl.isEmpty())
				flt.append(gfl);
			i++;
		}
	}
	if ((flt.size() > 1) && !sv)
		flt.prepend(QString("All files (%0)").arg(allfilt));
	return flt.join(";;");
}

typedef struct {
	int err;
	const char* text;
} xFilerError;

static xFilerError err_tab[] = {
	{ERR_CANT_OPEN, "Can't open file"},
	{ERR_NO_DRIVE, "The machine has no such drive"},
	{ERR_RZX_SIGN, "Wrong RZX signature"},
	{ERR_RZX_CRYPT, "Encrypted RZX is not supported"},
	{ERR_RZX_UNPACK, "RZX unpack error"},
	{ERR_RZX_REC, "This machine cannot be recorded"},
	{ERR_TZX_SIGN, "Wrong TZX signature"},
	{ERR_TZX_UNKNOWN, "Unknown TZX block"},
	{ERR_TRD_LEN, "Incorrect TRD size"},
	{ERR_TRD_SIGN, "Not a TR-DOS disk"},
	{ERR_UDI_SIGN, "Wrong UDI signature"},
	{ERR_FDI_SIGN, "Wrong FDI signature"},
	{ERR_FDI_HEAD, "Wrong FDI heads count"},
	{ERR_HOB_CANT, "Can't create file at disk"},
	{ERR_SCL_SIGN, "Wrong SCL signature"},
	{ERR_SCL_MANY, "Too many files in SCL"},
	{ERR_RAW_LONG, "File is too big"},
	{ERR_DSK_SIGN, "Wrong DSK signature"},
	{ERR_TD0_SIGN, "Wrong TD0 signature"},
	{ERR_TD0_TYPE, "Unsupported TD0"},
	{ERR_TD0_VERSION, "Unsupported TD0 version"},
	{ERR_WAV_HEAD, "Wrong WAV header"},
	{ERR_WAV_FORMAT, "Unsupported WAV format"},
	{ERR_Z80_HW, "Z80 snapshot: unsupported hardware"},
	{ERR_TRD_SNF, "Wrong disk structure for TRD file"},
	{ERR_SPG_SIGN, "Wrong SPG signature"},
	{ERR_SPG_VERSION, "Unsupported SPG version"},
	{ERR_SZX_SIGN, "Wrong SZX signature"},
	{ERR_SZX_HW, "SZX snapshot: a machine this emulator does not have"},
	{ERR_SZX_DATA, "SZX snapshot: no CPU or memory in it"},
	{ERR_TAP_DATA, "This tape has blocks TAP cannot hold: export it to WAV"},
	{ERR_TAP_EMPTY, "The tape is empty"},
	{ERR_NOTRD, "Not a TR-DOS disk"},
	{ERR_CANT_SAVE, "Can't save in this format"},
	{ERR_FILE_TYPE, "Unknown file type"},
	{ERR_SNAP_SHORT, "The snapshot is cut short"},
	{ERR_NEX_HW, "NEX and SNX files need the ZX Spectrum Next"},
	{ERR_NEX_CARD, "NEX and SNX files start from NextZXOS: put the System/Next folder in the SD card slot"},
	{ERR_NEX_NAME, "The file's name cannot be typed in NextZXOS"},
	{ERR_SNAP_NEXT, "Snapshots of the ZX Spectrum Next cannot be saved yet"},
	{ERR_OK, ""}
};

// a cancel was the user's own answer, so it is not reported
void file_errors(int err) {
	int i = 0;
	if ((err == ERR_OK) || (err == ERR_CANCEL)) return;
	while ((err_tab[i].err != ERR_OK) && (err_tab[i].err != err))
		i++;
	if (err_tab[i].err != ERR_OK) {
		shitHappens(err_tab[i].text);
	} else {
		shitHappens(QString("Error #%0").arg(err).toLocal8Bit().constData());
	}
}

static int disk_id[] = {FG_DISK_A, FG_DISK_B, FG_DISK_C, FG_DISK_D};
static int boot_ft[] = {FL_SCL, FL_TRD, FL_TD0, FL_FDI, FL_UDI, FL_HOBETA, 0};

// what the last loaded file would need to start on its own, and the drive it
// went to. The caller decides whether to act on it
static int last_as_kind = AS_NONE;
static int last_as_drv = 0;
// and a snapshot loaded since the window last asked
static QString last_snapshot;
// the recording the RZX player shows
static QString rzx_path;

QString rzx_current() {
	return rzx_path;
}

int file_autostart_kind() {
	return last_as_kind;
}

QString file_take_snapshot() {
	QString res = last_snapshot;
	last_snapshot.clear();
	return res;
}

// the image named in the window title, which is what Favorites take
static QString media_now;

QString media_current() {
	return media_now;
}

// what a video of the machine is named after: the image in use, else what is
// in the deck or drive A - a tape about to be started - else the machine
// the image in use, as the title has it, without its folder and extension;
// empty when nothing is: an image left mounted is not what the machine runs
QString media_image_name() {
	return QFileInfo(media_now).completeBaseName();
}

void media_set_current(const QString& path) {
	media_now = path;
}

void media_autorun_forget() {
	last_as_kind = AS_NONE;
	last_as_drv = 0;
}

static char as_no_tape[] = "This machine cannot start a tape";
static char as_no_disk[] = "This machine cannot start that disk";
static char as_not_a[] = "Only drive A can be started";

// a run with no window (--bench) has nowhere to put a question or a notice
static int as_headless = 0;

void media_autorun_headless(int on) {
	as_headless = on;
}

// A disk cannot be started while a divMMC is on: its drives are out. Said in a
// window with the machine paused: the message line is gone before it can be read.
// The gui asks to switch the board off before it gets here (drivesBack); this is
// the command line's.
static void as_board_refused() {
	if (as_headless) return;		// the log has the refusal
	conf.emu.pause |= PR_EXTRA;
	QMessageBox::information(nullptr, "Cannot start the disk",
		"The floppy drives are off while the divMMC is on: the disk is in its drive, "
		"but cannot be started.\n\n"
		"Switch the divMMC off with Media > divMMC, or start a disk image from the "
		"card in the esxDOS browser (Machine > NMI).");
	conf.emu.pause &= ~PR_EXTRA;
}

// Media just opened from the gui: mounting is not enough when the user wants it
// started, so do what the command line does - reset and press the keys. A
// machine that cannot take this kind of media keeps running, the image just
// sits in its drive.
// booting is drive A business
static bool as_from_drive(int kind, int drv) {
	return (kind == AS_TAPE) || (drv <= 0);
}

// a tape, or a disk going into drive A
static bool as_runnable(int kind, int drv) {
	return (kind != AS_NONE) && as_from_drive(kind, drv);
}

void media_autorun(Computer* comp, int run) {
	int kind = file_autostart_kind();
	if (!comp || !run || (kind == AS_NONE)) return;
	if (!as_from_drive(kind, last_as_drv)) {
		comp->msg = as_not_a;
		xlog(XLG_FILE, XLL_INFO, "autostart: only drive A can be started");
		return;
	}
	emu_lock();		// arming resets the machine
	int ok = autostart_arm(comp, kind);
	emu_unlock();
	xlog(XLG_FILE, XLL_INFO, "autostart %s: %s", ok ? "armed" : "refused",
		comp->hw->name);
	if (ok) return;
	if (autostart_board_blocks(comp, kind) || ((kind != AS_TAPE) && comp->dif->out))
		as_board_refused();
	else
		comp->msg = (kind == AS_TAPE) ? as_no_tape : as_no_disk;
}

static int as_kind_of(int ftype) {
	switch (ftype) {
		case FL_TAP:
		case FL_TZX:
		case FL_WAV:
			return AS_TAPE;
		case FL_SCL:
		case FL_TRD:
		case FL_TD0:
		case FL_FDI:
		case FL_UDI:
			return AS_DISK;
		case FL_DSK:
			return AS_DISK3;
	}
	return AS_NONE;
}

// ERR_OK once the disk or tape the file would replace is safe to lose
static int replace_ok(Computer* comp, xFileTypeInfo* inf, int drv) {
	if (inf->ch) return saveChangedDisk(comp, drv);
	if (as_kind_of(inf->id) == AS_TAPE) return saveChangedTape(comp);
	return ERR_OK;
}

// Ask: the machines that can take the file, Auto's pick first. false is
// Cancel - the file is not opened at all
static bool media_ask_machine(const QString& path, const std::vector<std::string>& ids, std::string* mac) {
	const xMachine* cur = xm_find(conf.macId);
	QString curName = QString::fromLocal8Bit(cur ? cur->name.c_str() : conf.macId.c_str());
	QDialog dlg;
	dlg.setWindowTitle("Choose a Machine");
	QVBoxLayout* lay = new QVBoxLayout(&dlg);
	QLabel* lab = new QLabel(QString("<b>%1</b> does not run on %2.<br>Run it on:")
		.arg(QFileInfo(path).fileName().toHtmlEscaped(), curName.toHtmlEscaped()));
	lab->setWordWrap(true);
	lay->addWidget(lab);
	QListWidget* list = new QListWidget;
	for (const std::string& id : ids) {
		const xMachine* m = xm_find(id);
		QListWidgetItem* it = new QListWidgetItem(QString::fromLocal8Bit(m ? m->name.c_str() : id.c_str()));
		it->setData(Qt::UserRole, QString::fromLocal8Bit(id.c_str()));
		list->addItem(it);
	}
	list->setCurrentRow(0);
	// the whole list in view: it is never longer than the machines there are
	list->setMinimumHeight(list->sizeHintForRow(0) * list->count() + 2 * list->frameWidth());
	lay->addWidget(list);
	QHBoxLayout* btns = new QHBoxLayout;
	QPushButton* run = new QPushButton("Switch");
	QPushButton* keep = new QPushButton(QString("Keep %1").arg(curName));
	QPushButton* cancel = new QPushButton("Cancel");
	run->setDefault(true);
	btns->addStretch();
	btns->addWidget(run);
	btns->addWidget(keep);
	btns->addWidget(cancel);
	lay->addLayout(btns);
	enum {ASK_CANCEL = 0, ASK_SWITCH, ASK_KEEP};
	QObject::connect(run, &QPushButton::clicked, &dlg, [&dlg]() {dlg.done(ASK_SWITCH);});
	QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, [&dlg]() {dlg.done(ASK_SWITCH);});
	QObject::connect(keep, &QPushButton::clicked, &dlg, [&dlg]() {dlg.done(ASK_KEEP);});
	QObject::connect(cancel, &QPushButton::clicked, &dlg, [&dlg]() {dlg.done(ASK_CANCEL);});
	int res = dlg.exec();
	if ((res == ASK_SWITCH) && list->currentItem())
		*mac = list->currentItem()->data(Qt::UserRole).toString().toLocal8Bit().data();
	return res != ASK_CANCEL;
}

// The machine a file is to be opened on, asked before it is opened: *mac comes
// back empty to keep the running one. Only what is about to run gets a say - a
// snapshot always, a tape or a disk only when it is started, and a disk only
// from drive A, the one that boots. false: the user backed out of the question.
bool media_machine(Computer* comp, const QString& path, int id, int drv, int run, std::string* mac) {
	mac->clear();
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (!inf) return true;
	int kind = as_kind_of(inf->id);		// AS_NONE: a snapshot, or nothing that runs
	if ((kind != AS_NONE) && (!run || !as_runnable(kind, drv))) return true;
	xFileMacPick pick = fm_pick(inf->id, path.toLocal8Bit().data());
	if (!pick.ask.empty())
		return media_ask_machine(path, pick.ask, mac);
	*mac = pick.target;
	return true;
}

bool media_runnable(Computer* comp, const QString& path, int id, int drv) {
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	return inf && as_runnable(as_kind_of(inf->id), drv);
}

// a file that goes into a floppy drive
bool media_to_drive(Computer* comp, const QString& path, int id) {
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (!inf) return false;
	switch (inf->id) {
		case FL_SCL: case FL_TRD: case FL_TD0: case FL_FDI: case FL_UDI: case FL_DSK:
		case FL_IMA: case FL_PCIMG: case FL_HOBETA: case FL_RAW:
			return true;
	}
	return false;
}

void disk_boot(Computer* comp, int drv, int id) {
	if (!conf.boot) return;
	int idx = 0;
	while (boot_ft[idx] && (boot_ft[idx] != id))
		idx++;
	if (boot_ft[idx])
		loadBoot(comp, conf.path.boot.c_str(), drv);
}

// The dialog is shared with the saves: the name a save left in it goes, and the
// mode is set before the filter, which in save mode puts its own suffix on that
// name - an RZX just recorded came back offered as an .szx.
static QFileDialog* file_dialog_open_mode() {
	QFileDialog* filer = file_dialog();
	filer->setAcceptMode(QFileDialog::AcceptOpen);
	filer->selectFile(QString());
	return filer;
}

// The open dialog, for the kind of file id asks for. What was picked in it
// comes back through id and drv: the filter chosen is the group the file is
// opened as, and a drive of its own when drv did not name one.
QString file_ask_open(Computer* comp, int* id, int* drv) {
	int fid = *id;
	if (fid == FG_DISK)
		fid = disk_id[*drv & 3];
	if (fid == FG_ALL)
		fid = detect_hw_id(comp->hw->id);
	QString path;
	QString flt = file_get_hw_filter(comp, fid, 0);
	if (flt.isEmpty()) {
		flt = file_get_group_filter(comp, fid, 0);
		if (flt.isEmpty())
			flt = file_get_type_filter(fid, 0);
	}
	if (flt.isEmpty()) return path;
	QFileDialog* filer = file_dialog_open_mode();
	filer->setWindowTitle("Open File");
	filer->setNameFilter(flt);
	filer->setDirectory(conf.lastDir.c_str());
	filer->setHistory(QStringList());
	if (filer->exec()) {
		path = filer->selectedFiles().first();
		xFileGroupInfo* grp = file_detect_grp(filer->selectedNameFilter());
		if (grp->id > 0) *id = grp->id;
		if (*drv < 0) *drv = grp->drv;
		conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
	}
	return path;
}

// The tape and the disks an .szx links to or carries. A link that does not
// resolve is looked for beside the snapshot, as Spectaculator writes plain names
// too; a carried image is written out to the temp folder first. One already in
// its place is left in, so a disk written to since is not thrown away.
static void szx_mount(Computer* comp, const QString& snap) {
	const szxMedia* med;
	int count = szx_media(&med);
	for (int i = 0; i < count; i++) {
		const szxMedia* m = &med[i];
		QString path;
		if (m->data) {
			QString name = QFileInfo(snap).completeBaseName();
			if (m->kind != SZX_MED_TAPE) name += QString("-%1").arg(QChar('a' + m->drive));
			path = QDir::temp().filePath(name + "." + QString::fromLocal8Bit(m->ext).toLower());
			QFile file(path);
			if (!file.open(QIODevice::WriteOnly) || (file.write((const char*)m->data, m->size) != (qint64)m->size)) {
				xlog(XLG_FILE, XLL_WARN, "szx: cannot write the carried image to %s", path.toLocal8Bit().constData());
				continue;
			}
		} else {
			QString link = QDir::fromNativeSeparators(QString::fromLocal8Bit(m->path));
			path = link;
			if (!QFileInfo(path).isFile())
				path = QFileInfo(snap).dir().filePath(QFileInfo(link).fileName());
			if (!QFileInfo(path).isFile()) {
				xlog(XLG_FILE, XLL_WARN, "szx: %s not found", m->path);
				continue;
			}
		}
		path = QFileInfo(path).canonicalFilePath();
		xFileTypeInfo* inf = file_find_hw_ext(comp->hw->id, path);
		int kind = inf ? as_kind_of(inf->id) : AS_NONE;
		if ((m->kind == SZX_MED_TAPE) ? (kind != AS_TAPE) : ((kind != AS_DISK) && (kind != AS_DISK3))) {
			xlog(XLG_FILE, XLL_WARN, "szx: %s is no image of the kind it is linked as", path.toLocal8Bit().constData());
			continue;
		}
		if (m->kind == SZX_MED_TAPE) {
			const char* now = comp->tape->path;
			if (!now || (QFileInfo(QString::fromLocal8Bit(now)).canonicalFilePath() != path)) {
				if (replace_ok(comp, inf, 0) != ERR_OK) continue;
				if (inf->load(comp, path.toLocal8Bit().data(), 0) != ERR_OK) continue;
			}
			if (m->block < comp->tape->blkCount) {
				tapRewind(comp->tape, m->block);
			} else if ((m->block == comp->tape->blkCount) && (m->block > 0)) {
				// taken with the tape run out: it stays run out, for
				// "Rewind at end" to wind back to the block a load asks for
				tapRewind(comp->tape, m->block - 1);
				tapNextBlock(comp->tape);
			}
		} else {
			int drv = m->drive & 3;
			Floppy* flp = comp->dif->flp[drv];
			if (!flp->fitted) continue;
			if (!flp->insert || !flp->path || (QFileInfo(QString::fromLocal8Bit(flp->path)).canonicalFilePath() != path)) {
				if (replace_ok(comp, inf, drv) != ERR_OK) continue;
				if (inf->load(comp, path.toLocal8Bit().data(), drv) != ERR_OK) continue;
				disk_boot(comp, drv, inf->id);		// as opening it does
			}
			flp->protect = m->protect ? 1 : 0;
			flp->trk = m->cylinder;
		}
		xlog(XLG_FILE, XLL_INFO, "szx: %s %s", (m->kind == SZX_MED_TAPE) ? "tape" : "disk", path.toLocal8Bit().constData());
	}
	szx_ext_media(comp);
}

// What the snapshot was taken with and this machine does not have. Nothing of
// the machine is changed for it: the user is told, and decides.
static void szx_warn() {
	static const struct {
		int note;
		const char* text;
	} tab[] = {
		{SZN_RELATIVE, "a model this emulator does not have, loaded on its nearest relative"},
		{SZN_TIMINGS, "the other ULA timings (early/late)"},
		{SZN_ISSUE, "another board issue (2/3), so the EAR bit reads otherwise"},
		{SZN_ULAPLUS, "ULA+ in use"},
		{SZN_AY, "an AY"},
		{SZN_BETA, "a Beta 128 disk interface"},
		{SZN_COVOX, "a Covox"},
		{SZN_DEVICE, "a device not emulated here (Interface 1, Multiface, SpecDrum...)"},
		{0, NULL}
	};
	int notes = szx_notes();
	if (!notes || !conf.running) return;
	QString text = QObject::tr("The snapshot was taken with something this machine does not have:");
	text += "\n";
	for (int i = 0; tab[i].text; i++) {
		if (notes & tab[i].note)
			text += QString("\n- ") + tab[i].text;
	}
	text += "\n\n";
	text += QObject::tr("It may not run as it did. The machine's settings are left as they are.");
	QMessageBox mbx(QMessageBox::Warning, "SZX snapshot", text, QMessageBox::Ok);
	mbx.exec();
}

int load_file(Computer* comp, const char* name, int id, int drv) {
	last_as_kind = AS_NONE;
	xspeed_modes_off();
	QString path = QFileInfo(QString::fromLocal8Bit(name)).canonicalFilePath();
	if (path.isEmpty()) return ERR_CANT_OPEN;
	xFileTypeInfo* inf = file_type_of(comp, id, path);
	if (inf && (inf->id == FL_POK)) inf = NULL;	// not a medium: see file_is_pok
	int err = inf ? ERR_OK : ERR_FILE_TYPE;
	if (drv < 0) drv = 0;
	if (inf && inf->ch && !comp->dif->flp[drv & 3]->fitted) {
		err = ERR_NO_DRIVE;
	} else if (inf && inf->load) {
		if (replace_ok(comp, inf, drv) != ERR_OK) {		// a failed save has been shown
			err = ERR_CANCEL;
		} else {
			if (inf->id == FL_RZX)
				rzxr_stop(comp);		// playing one ends the one being made
			else if (comp->rzx.play)
				rzxStop(comp);			// the recording would go on feeding what was opened
			err = inf->load(comp, path.toLocal8Bit().data(), drv);
			if ((err == ERR_OK) && (inf->id == FL_SZX)) {
				szx_mount(comp, path);
				szx_warn();
			}
			disk_boot(comp, drv, inf->id);
			if (err == ERR_OK) {
				last_as_kind = as_kind_of(inf->id);
				last_as_drv = drv;
				switch (inf->id) {
					case FL_SNA: case FL_Z80: case FL_SZX: case FL_SPG: case FL_NEX: case FL_SNX:
						rzx_rec_touch();
						last_snapshot = path;
						break;
					case FL_RZX:
						last_snapshot = path;
						break;
				}
				if (inf->id == FL_RZX)
					rzx_path = path;
			}
		}
	}
	if (err == ERR_OK) {
		// the drive only means anything for a disk (inf->ch marks one)
		if (inf && inf->ch) {
			xlog(XLG_FILE, XLL_INFO, "open %s into drive %c: %s", inf->name,
				'A' + (drv & 3), path.toLocal8Bit().constData());
		} else {
			xlog(XLG_FILE, XLL_INFO, "open %s: %s", inf ? inf->name : "?",
				path.toLocal8Bit().constData());
		}
	} else {
		xlog(XLG_FILE, XLL_WARN, "open %s failed, error %i",
			path.toLocal8Bit().constData(), err);
	}
	file_errors(err);
	return err;
}

bool file_is_pok(const QString& path) {
	return path.endsWith(".pok", Qt::CaseInsensitive);
}

static xMediaOpen media_last = {QString(), FG_ALL, 0, 0, false};

void media_opened(const QString& path, int id, int drv, int run, bool pinned) {
	media_last = {path, id, drv, run, pinned};
	if (drv < 1) labels_image(path);	// a disk in drive B is not the program
}

xMediaOpen media_last_open() {
	return media_last;
}

// Run the machine on to the end of the next INT acknowledge, where loading puts
// a .sna with interrupts off back, a frame and a bit at most. Returns 0 if it
// never got there: interrupts kept off, or a breakpoint.
static int snap_align(Computer* comp) {
	CPU* cpu = comp->cpu;
	int flen = comp_frame_ticks(comp);
	int t0 = comp->tickCount;
	int done = 0;
	emu_lock();
	while ((comp->tickCount - t0 < flen + flen / 4) && !done && !comp->flgBRK) {
		int iff = cpu->flgIFF1;
		int pc = cpu->regPC;
		int sp = cpu->regSP;
		snd_scope_step(comp, compExec(comp));
		// taken: interrupts went off and the pc was pushed - past a HALT if the
		// cpu sat in one - where a DI pushes nothing
		if (iff && !cpu->flgIFF1 && (cpu->regSP == ((sp - 2) & 0xffff))) {
			int ret = memRd(comp->mem, cpu->regSP) | (memRd(comp->mem, (cpu->regSP + 1) & 0xffff) << 8);
			done = (ret == pc) || (ret == ((pc + 1) & 0xffff));
		}
	}
	emu_unlock();
	return done;
}

int save_file(Computer* comp, const char* name, int id, int drv, int live) {
	QString path = QString::fromLocal8Bit(name);
	QString flt;
	QString ext;
	xFileTypeInfo* inf = NULL;
	xFileTypeInfo* tin;
	xFileGroupInfo* grp;
	int i;
	int flg;
	if (id == FG_DISK)
		id = disk_id[drv & 3];
	if (id == FG_ALL)
		id = detect_hw_id(comp->hw->id);
	int err = ERR_OK;
	if (path.isEmpty()) {
		flt = file_get_hw_filter(comp, id, 1);
		if (flt.isEmpty()) {
			flt = file_get_group_filter(comp, id, 1);
			if (flt.isEmpty())
				flt = file_get_type_filter(id, 1);
		}
		if (!flt.isEmpty()) {
			QFileDialog* filer = file_dialog();
			filer->setWindowTitle("Save File");
			filer->setNameFilter(flt);
			filer->setAcceptMode(QFileDialog::AcceptSave);
			filer->setDirectory(conf.lastDir.c_str());
			filer->setHistory(QStringList());
			if (filer->exec()) {
				path = filer->selectedFiles().first();
				flt = filer->selectedNameFilter();
				grp = file_detect_grp(flt);
				if (grp->id != FL_NONE) {
					drv = grp->drv;
					i = 0;
					flg = 1;
					// scan group file types and check if path extension is the same
					while ((grp->child[i] != FL_NONE) && flg) {
						tin = file_find_type(grp->child[i]);
						if (tin) {
							if (path.endsWith(tin->ext, Qt::CaseInsensitive)) {
								flg = 0;
								inf = tin;
							}
						}
						i++;
					}
					// if no filetypes found, add default extension
					if (flg)
						path.append(grp->defext);
				} else {
					tin = file_detect_type(flt);
					if (tin->id != FL_NONE) {
						if (tin->ext && !path.endsWith(tin->ext, Qt::CaseInsensitive))
							path.append(tin->ext);
						inf = tin;
					}
				}
				conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
			}
		}
	}
	if (path.isEmpty()) {
		// no dialog: nothing of this kind can be written
		err = flt.isEmpty() ? ERR_CANT_SAVE : ERR_CANCEL;
		file_errors(err);
		return err;
	}
	if (drv < 0)
		drv = 0;
	if (!inf)
		// inf = file_ext_type(path);
		inf = file_find_hw_ext(comp->hw->id, path);
	if (inf) {
		xlog(XLG_FILE, XLL_INFO, "filetype: %s", inf->name);
		if (inf->save) {
			// a .sna has no frame position: a running machine is saved where
			// loading puts it back, a held one stays put and is told so
			if (inf->id == FL_SNA) {
				int aligned = live && !comp->rzx.play && snap_align(comp);
				if (!aligned)
					comp->msg = (char*)" SNA: no frame position, use SZX ";
			}
			err = inf->save(comp, path.toLocal8Bit().data(), drv);
		} else {
			err = ERR_CANT_SAVE;
		}
	} else {
		err = ERR_FILE_TYPE;
	}
	if (err != ERR_OK)
		xlog(XLG_FILE, XLL_WARN, "save %s failed, error %i", path.toLocal8Bit().constData(), err);
	file_errors(err);
	return err;
}

static bool file_can_save(Computer* comp, const char* path) {
	if (!path || !*path) return false;
	xFileTypeInfo* inf = file_find_hw_ext(comp->hw->id, QString::fromLocal8Bit(path));
	return inf && inf->save && !ft_export(inf);
}

int save_back(Computer* comp, const char* path, int id, int drv) {
	return save_file(comp, file_can_save(comp, path) ? path : "", id, drv);
}


// quick save: the machine as it is, in memory - an xstate is not a file, it
// holds the live machine's own pointers - and as an .szx
static xState* quickState = NULL;
static std::string quickMac;		// the machine the one in memory is of

static QString quick_path(const char* ext = ".szx") {
	QString dir = QString::fromLocal8Bit(conf.path.confDir.c_str()) + "/quick";
	QDir().mkpath(dir);
	return dir + "/" + QString::fromLocal8Bit(conf.macId.c_str()) + ext;
}

// The file is only built while the machine is held: packing several megabytes
// of an Evo's memory would hold it for frames.
int quick_save(Computer* comp) {
	szxBuf b;
	emu_lock();
	if (!quickState) quickState = xstate_create();
	int res = (quickState && xstate_save(quickState, comp)) ? 1 : 0;
	quickMac = res ? conf.macId : std::string();
	int built = res && szxCanSave(comp) && (build_szx(comp, &b) == ERR_OK);
	emu_unlock();
	if (built) {
		QString path = quick_path();
		if (szx_write(&b, path.toLocal8Bit().data()) == ERR_OK) {
			res = 2;
			QFile::remove(quick_path(".z80"));	// what builds before .szx left there
		}
	}
	return res;
}

// what a quick load replaced, to take it back with
static xState* undoState = NULL;
static std::string undoMac;

// A quick save may be from before what a recording has logged since, so the
// recording joins there instead of going back with the machine.
static int quick_xstate_load(xState* st, Computer* comp) {
	rzxRecPos rec = comp->rzx.rec;
	int res = xstate_load(st, comp);
	if (res && rec.on) {
		comp->rzx.rec = rec;
		rzx_rec_touch();
	}
	return res;
}

int quick_load(Computer* comp) {
	int res = 0;
	emu_lock();
	if (!undoState) undoState = xstate_create();
	undoMac = (undoState && xstate_save(undoState, comp)) ? conf.macId : std::string();
	if (quickState && (quickMac == conf.macId))
		res = quick_xstate_load(quickState, comp);
	if (!res) {
		QString path = quick_path();
		if (!QFileInfo::exists(path))
			path = quick_path(".z80");
		if (QFileInfo::exists(path))
			res = (load_file(comp, path.toLocal8Bit().data(), FG_ALL, 0) == ERR_OK);
	}
	if (res) cheats_forget();
	emu_unlock();
	return res;
}

// back to before the last quick load; again, and that load is back
int quick_undo(Computer* comp) {
	if (!undoState || (undoMac != conf.macId)) return 0;
	emu_lock();
	xState* was = xstate_create();
	bool kept = was && xstate_save(was, comp);
	int res = quick_xstate_load(undoState, comp);
	if (res && kept) {
		xstate_destroy(undoState);
		undoState = was;
	} else if (was) {
		xstate_destroy(was);
	}
	if (res) cheats_forget();
	emu_unlock();
	return res;
}

// The save dialog on its own, for an export that is not one of the file types
// the tables above know. Same dialog as every other open and save in the app.
// own: a file that is written over without asking - the one a recording goes on in
QString file_ask_save(const char* title, const char* filter, const char* ext, const QString& suggest, const QString& own) {
	QFileDialog* filer = file_dialog();
	filer->setWindowTitle(title);
	filer->setNameFilter(filter);
	filer->setAcceptMode(QFileDialog::AcceptSave);
	if (suggest.isEmpty()) {
		filer->setDirectory(conf.lastDir.c_str());
	} else {
		filer->setDirectory(QFileInfo(suggest).absolutePath());
		filer->selectFile(QFileInfo(suggest).fileName());
	}
	filer->setHistory(QStringList());
	filer->setOption(QFileDialog::DontConfirmOverwrite, !own.isEmpty());
	int ok = filer->exec();
	filer->setOption(QFileDialog::DontConfirmOverwrite, false);
	if (!ok) return QString();
	QString path = filer->selectedFiles().first();
	if (!path.endsWith(ext, Qt::CaseInsensitive))
		path.append(ext);
	if (!own.isEmpty() && QFileInfo::exists(path) && (QFileInfo(path) != QFileInfo(own))) {
		QString q = QString("%1 already exists.\nDo you want to replace it?").arg(QFileInfo(path).fileName());
		if (QMessageBox::warning(filer->parentWidget(), title, q, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
			return QString();
	}
	conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
	return path;
}

// one file of one kind, to open
QString file_ask_load(const char* title, const char* filter) {
	QFileDialog* filer = file_dialog_open_mode();
	filer->setWindowTitle(title);
	filer->setNameFilter(filter);
	filer->setDirectory(conf.lastDir.c_str());
	filer->setHistory(QStringList());
	if (!filer->exec()) return QString();
	QString path = filer->selectedFiles().first();
	conf.lastDir = std::string(QFileInfo(path).dir().absolutePath().toLocal8Bit().data());
	return path;
}

// old

void initFileDialog(QWidget* par) {
	fileDlgParent = par;
}

// Every Qt file dialog restores the column widths any Qt program last left in
// the shared QtProject settings, and a long image name gets cut in them. Give
// the name all the room the other columns do not need, whatever was stored.
// Called on Show, after that restore; a native dialog has no tree and is skipped
void fitFileDialog(QWidget* w) {
	QFileDialog* dlg = qobject_cast<QFileDialog*>(w);
	QTreeView* tree = dlg ? dlg->findChild<QTreeView*>("treeView") : nullptr;
	if (!tree) return;
	QHeaderView* hdr = tree->header();
	hdr->setSectionResizeMode(QHeaderView::ResizeToContents);
	hdr->setSectionResizeMode(0, QHeaderView::Stretch);
	hdr->setStretchLastSection(false);
	// the places list on the left: same shared settings, stored narrower than its names
	QSplitter* split = dlg->findChild<QSplitter*>("splitter");
	QListView* side = dlg->findChild<QListView*>("sidebar");
	if (!split || !side || (split->indexOf(side) != 0) || (split->count() != 2)) return;
	int need = side->sizeHintForColumn(0) + 2 * side->frameWidth() + side->verticalScrollBar()->sizeHint().width();
	QList<int> sz = split->sizes();
	if (sz[0] >= need) return;
	split->setSizes({need, qMax(sz[0] + sz[1] - need, 0)});
}

// ERR_OK once the changed medium is safe to lose: saved, or the user said No
static int ask_save_changed(Computer* comp, QString what, const char* path, int id, int drv) {
	if (path && *path)
		what += ": " + QFileInfo(QString::fromLocal8Bit(path)).fileName();
	QMessageBox mbox;
	mbox.setText(QString("<b>%0 has been changed</b>").arg(what.toHtmlEscaped()));
	mbox.setInformativeText("Do you want to save it?");
	mbox.setStandardButtons(QMessageBox::Yes|QMessageBox::No|QMessageBox::Cancel);
	mbox.setIcon(QMessageBox::Warning);
	switch (mbox.exec()) {
		case QMessageBox::Yes: return save_back(comp, path, id, drv);
		case QMessageBox::No: return ERR_OK;
	}
	return ERR_CANCEL;
}

int saveChangedDisk(Computer* comp,int id) {
	id &= 3;
	Floppy* flp = comp->dif->flp[id];
	if (!flp->changed) return ERR_OK;
	return ask_save_changed(comp, QString("Disk %0").arg(QChar('A' + id)), flp->path, FG_DISK, id);
}

int saveChangedTape(Computer* comp) {
	Tape* tape = comp->tape;
	if (!tape->changed || (tape->blkCount < 1)) return ERR_OK;
	return ask_save_changed(comp, "Tape", tape->path, FG_TAPE, -1);
}

int testSlotOn(Computer* comp) {
	int res = 0;
	for (int i = 0; i < 256; i++) {
		if (comp->mem->map[i].type == MEM_SLOT) {
			res = 1;
			i = 256;
		}
	}
	return res;
}
