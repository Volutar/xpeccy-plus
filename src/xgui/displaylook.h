#pragma once

// A look is what the Display list holds: a shader file, one of its presets, and the
// knobs turned on top. conf.vid.shader and conf.vid.shdPreset name the chosen one;
// the knobs turned are kept by look in shaders/values.conf of the config folder.

#include "shaderchain.h"

#include <QList>
#include <QMap>
#include <QString>

// the list's parts, in its order; a line stands between two
enum {
	LOOK_BASIC = 0,		// None, and the built-in files with knobs and no presets
	LOOK_PRESET,		// a file's own presets
	LOOK_USER,		// the user's presets
	LOOK_FILE		// the other shader files
};

struct xLook {
	int part = LOOK_BASIC;
	QString file;		// the shader's file; empty: None
	QString preset;		// a preset of the file, or a user's .preset by its file name; empty: the file as it is
	QString title;		// what the lists show
	bool user = false;	// the preset is a file of the user's
	bool operator==(const xLook& o) const {return (file == o.file) && (preset == o.preset);}
};

QList<xLook> look_list();
xLook look_current();
void look_choose(const xLook&);
bool look_has_knobs(const xLook&);		// its file has parameters

// the file's parameters and presets, read once and again when the file changes;
// NULL: no such file, or one that cannot be read
const xShdPreset* look_file(const QString& file);

// The look's own values: its preset's over the file's defaults...
xShdValues look_base(const xLook&);
// ...and with the knobs turned on top: what the chain is loaded with
xShdValues look_values(const xLook&);
float look_value(const xLook&, const QString& name);

// a knob turned: kept, unless it is back at the look's own value
void look_keep(const xLook&, const QString& name, float value);
void look_reset(const xLook&);			// every knob back
bool look_changed(const xLook&);		// a knob is turned

// the kept values as a whole, for a window that can be cancelled
typedef QMap<QString, QMap<QString, float> > xLookStore;
xLookStore look_store_take();
void look_store_put(const xLookStore&);
void look_store_save();				// to values.conf, if anything changed

// The look as it stands, as a preset of the user's: <name>.preset in the config's
// shaders folder; it becomes the chosen look. false: err says why
bool look_save_preset(const xLook&, const QString& name, QString* err);
bool look_delete_preset(const xLook&);

// a shader file name of an older version in the config: the look it is now
void look_old_names();
