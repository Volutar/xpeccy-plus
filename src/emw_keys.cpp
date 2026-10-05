#include "emulwin.h"
#include "filer.h"
#include "xcore/xcore.h"
#include "xcore/vscalers.h"
#include "xcore/sound.h"
#include "xcore/rewind.h"

#include <QMenu>
#include <QFileDialog>
#include <QGuiApplication>

void MainWin::kPress(QKeyEvent* ev) {
	keyPressEvent(ev);
}

void MainWin::kRelease(QKeyEvent* ev) {
	keyReleaseEvent(ev);
}

#ifdef __WIN32
static QMap<qint32, int> key_press_map;
#endif

// TODO: realtime autorepeat on xt keyboard (no runtime emulation)
// debug: no reaction
// keygrab, no autorep: get keyid, xkey_press
// keygrab, autorepeat: get keyid, xt_release, xt_press
// no grab, no autorep: get shortcut/keyid, xkey_press
// no grab, autorepeat: a shortcut only if it is a step (xcut_repeats), else the key's own repeat

// the speed mode a shortcut switches, XTM_NONE for any other
static int xcut_tmode(int keyid) {
	return (keyid == XCUT_FFWD) ? XTM_FFWD : ((keyid == XCUT_SLOWMO) ? XTM_SLOW : XTM_NONE);
}

// A held key repeats a shortcut only when the shortcut is a step: every other
// one switches, opens or fires something, and a repeat undoes it or fires it again.
static bool xcut_repeats(int keyid) {
	return (keyid == XCUT_SPEED_UP) || (keyid == XCUT_SPEED_DOWN);
}

void MainWin::releaseChord(QKeyEvent* ev, bool down) {
	int key = ev->key();
	if ((key != Qt::Key_Control) && (key != XREL_KEY2)) {
		if (down) relArmed = false;		// another key between: Ctrl+Alt+Del is not it
		return;
	}
	if (down) {
		if ((QGuiApplication::queryKeyboardModifiers() & XREL_MODS) == XREL_MODS) relArmed = true;
		return;
	}
	if (!relArmed) return;
	relArmed = false;
	if (grabMice) mouseGrabOff();
	if (pckAct->isChecked()) pckAct->setChecked(false);
}

int ev_to_keyid(QKeyEvent* ev, bool kgrab) {
	int keyid = hotkey_for(hotkey_key(ev), ev->modifiers(), kgrab);
	if (keyid < 0) {
#if defined(__linux) || defined(__BSD)
			keyid = ev->nativeScanCode();
#elif defined(__WIN32)
			keyid = ev->nativeScanCode();
#if STICKY_KEY
			if (keyid == 0) {
				keyid = ev->nativeVirtualKey();
			} else if ((ev->key() == Qt::Key_Shift) || (ev->key() == Qt::Key_Control) || (ev->key() == Qt::Key_Alt)) {
				keyid = 0;
			}
#endif
			if (key_press_map[keyid] == 0) {	// catch false press events
				key_press_map[keyid] = 1;
			} else {
				keyid = 0;
			}
#else
			keyid = qKey2id(ev->key(), ev->modifiers());
#endif
	}
	return keyid;
}

#if USE_HOST_KEYBOARD

void MainWin::keyPressEvent(QKeyEvent* ev) {
	int keyid;
	keyEntry kent;
	Computer* comp = conf.zx;
	releaseChord(ev, true);
//	qDebug() << ev->key();
	if (comp->flgDBG) {
		ev->ignore();
	} else if (pckAct->isChecked()) {
		keyid = ev_to_keyid(ev, true);
		kent = getKeyEntry(keyid);
		if (ev->isAutoRepeat()) {
			if (comp->keyb->core) {
				if (comp->keyb->core->flag & KF_AUTORPT) {
					comp->keyb->core->press(comp->keyb, &kent);
				}
			}
			//xt_release(comp->keyb, kent);
			//xt_press(comp->keyb, kent);
		} else {
			xkey_press(keyid);
		}
	} else {
//		qDebug() << ev->key();
		keyid = ev_to_keyid(ev, false);
		kent = getKeyEntry(keyid);
		if (ev->isAutoRepeat()) {
			if (keyid < 0x10000) {		// not a shortcut
				if (comp->keyb->core) {
					if (comp->keyb->core->flag & KF_AUTORPT) {
						comp->keyb->core->press(comp->keyb, &kent);
					}
				}
				//xt_release(comp->keyb, kent);
				//xt_press(comp->keyb, &kent);
			} else if (xcut_repeats(keyid)) {
				xkey_press(keyid);
			}
		} else {
			// a hotkey's own Shift or Ctrl is not the machine's Caps or Symbol Shift:
			// they are let go there before the hotkey acts, a reset or an NMI included
			if ((keyid >= 0x10000) && (ev->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
				static const int mods[] = {XKEY_LSHIFT, XKEY_RSHIFT, XKEY_LCTRL, XKEY_RCTRL};
				for (int m : mods) xkey_release(m);
			}
			xkey_press(keyid);
		}
	}
}

#else

void MainWin::keyPressEvent(QKeyEvent *ev) {
	if (ev->isAutoRepeat()) return;
//	qDebug() << "keyPressEvent" << ev->text() << ev->nativeScanCode() << ev->count();
	if (comp->debug) {
		ev->ignore();
	} else {
		int keyid = ev_to_keyid(ev, pckAct->isChecked());
//		printf("press: %i\n", keyid);
		xkey_press(keyid);
	}
}

#endif

// cmd: a hotkey asked for from a menu, which a grabbed keyboard does not take
void MainWin::xkey_press(int xkey, bool cmd) {
	keyEntry kent = getKeyEntry(xkey);
//	printf("xkey_press %s\n", kent.name);
	int x;
	int err;
	QString path;
	Computer* comp = conf.zx;
	comp->keyb->grab = pckAct->isChecked();
	if (pckAct->isChecked() && !cmd && (xkey != XCUT_GRABKBD)) {
		// xt_press(comp->keyb, &kent);
		if (comp->hw->keyp)
			comp->hw->keyp(comp, &kent);
		if (kent.joyMask) {
			if (kent.joyMask & XJ_JOYB) {
				joyPress(comp->joyb, kent.joyMask & 0xff);
			} else {
				joyPress(comp->joy, kent.joyMask & 0xff);
			}
		}
	} else {
		switch (xkey) {
			case XCUT_FULLSCR:
				vid_set_fullscreen(!conf.vid.fullScreen);
				setMessage(conf.vid.fullScreen ? " fullscreen on " : " fullscreen off ");
				updateWindow();
				saveConfig();
				if (!conf.vid.fullScreen)
					QTimer::singleShot(0, this, &MainWin::placeWindow);
				break;
			case XCUT_SIZEX1: case XCUT_SIZEX2: case XCUT_SIZEX3:
			case XCUT_SIZEX4: case XCUT_SIZEX5: case XCUT_SIZEX6:
				x = xkey - XCUT_SIZEX1 + 1;
				if (x == conf.vid.scale) break;		// the size it already is
				vid_set_zoom(x);
				updateWindow();
				saveConfig();
				setMessage(QString(" size x%0 ").arg(x));
				break;
			case XCUT_COMBOSHOT:
				scrCounter = conf.scrShot.count;
				scrInterval = 0;
				break;
			case XCUT_RES_DOS:
				resetTo(RES_DOS);
				break;
			case XCUT_RES_48:
				resetTo(RES_48);
				break;
			case XCUT_RES_128:
				resetTo(RES_128);
				break;
			case XCUT_RES_SERVICE:
				resetTo(RES_SHADOW);
				break;
			case XCUT_KEYBOARD:
				emit s_keywin_shide();
				break;
			case XCUT_SCRWIN:
				emit s_scr_show();
				break;
			case XCUT_SNDWIN:
				emit s_snd_show();
				break;
			case XCUT_MUTE:
				conf.snd.mute ^= 1;
				setMessage(conf.snd.mute ? " sound off " : " sound on ");
				syncActions();
				saveConfig();
				break;
			case XCUT_REWIND:
				rewind_want(1);
				break;
			case XCUT_FAST:
				if (conf.emu.pause) break;
				fast_key(true);
				break;
			case XCUT_TURBO:
				// the board's own turbo, walked through the steps it declares.
				// A machine that sets it from a port will set it again itself
				// an unknown step reads as -1 and so starts the list over
				setTurbo((xm_turbo_index(comp) + 1) % std::max(comp->turboCount, 1));
				break;
			case XCUT_FFWD:
			case XCUT_SLOWMO:
				if (comp->rzx.play) {
					setMessage(" not in RZX ");
				} else {
					xspeed_key(xcut_tmode(xkey), 1);
				}
				break;
			case XCUT_SPEED_UP:
			case XCUT_SPEED_DOWN:
				if (comp->rzx.play) {
					setMessage(" not in RZX ");
				} else {
					int pos = xspeed_get() + ((xkey == XCUT_SPEED_UP) ? 1 : -1);
					if ((pos < 0) || (pos > xspeed_max())) {
						setMessage(" limit reached ");
					} else {
						xspeed_set(pos);
						setMessage(QString(" %0 ").arg(xspeed_name(pos, true)));
					}
				}
				break;
			case XCUT_NOFLICK:
				if (noflic < 15)
					noflic = 25;
				else if (noflic < 35)
					noflic = 50;
				else noflic = 0;
				saveConfig();
				setMessage(QString(" noflick %0% ").arg(noflic * 2));
				break;
//			case XCUT_TVLINES:
//				scanlines = !scanlines;
//				setMessage(scanlines ? " scanlines on " : " scanlines off ");
//				saveConfig();
				break;
			case XCUT_RELOAD_SHD:
				loadShader();
				break;
			case XCUT_RELOAD:
				pause(true, PR_FILE);
				x = media_reload(comp);
				pause(false, PR_FILE);
				setMessage(x ? " reloaded " : " nothing to reload ");
				break;
			case XCUT_QUICKSAVE:
				x = quick_save(comp);
				setMessage((x == 2) ? " quick save " : (x == 1) ? " quick save, until exit " : " can't quick save ");
				break;
			case XCUT_QUICKLOAD:
				setMessage(quick_load(comp) ? " quick load " : " nothing quick saved ");
				break;
			case XCUT_QUICKUNDO:
				setMessage(quick_undo(comp) ? " quick load undone " : " nothing to undo ");
				break;
			case XCUT_TAPE_START:
				tapStateChanged(TW_REWIND, 0);
				setMessage(" tape to start ");
				break;
			case XCUT_FAVORITE:
				// adds only: taking one out stays in the menu, where it can be seen
				path = media_current();
				if (path.isEmpty()) {
					setMessage(" nothing to add ");
				} else if (findBookmark(path) >= 0) {
					setMessage(" already in Favorites ");
				} else {
					addFavorite(path);
				}
				break;
			case XCUT_RATIO:
				vid_set_ratio(!conf.vid.keepRatio);
				updateWindow();
				setMessage(conf.vid.keepRatio ? " keep aspect " : " free aspect ");
				saveConfig();
				break;
			case XCUT_MOUSE:
				if (grabMice) {
					mouseGrabOff();
				} else {
					mouseGrabOn();
				}
				break;
			case XCUT_GRABKBD:
				pckAct->setChecked(!pckAct->isChecked());	// its message comes with the switch
				break;
			case XCUT_PAUSE:
				conf.emu.pause ^= PR_PAUSE;
				pause(true,0);
				break;
			case XCUT_DEBUG:
				conf.emu.fast = 0;
				pause(true, PR_DEBUG);
				// setUpdatesEnabled(true);
				emit s_debug();
				break;
			case XCUT_MENU:
				popupUserMenu(mapToGlobal(QPoint(20,20)));
				break;
			case XCUT_OPTIONS:
				pause(true, PR_OPTS);
				emit s_options();
				break;
			case XCUT_HOTKEYS:
				pause(true, PR_OPTS);
				emit s_hotkeys();
				break;
			case XCUT_SAVE: {
				int live = !conf.emu.pause && !comp->flgDBG;
				pause(true,PR_FILE);
				save_file(comp, NULL, FG_ALL, -1, live);
				pause(false,PR_FILE);
				break;
			}
			case XCUT_LOAD:
				openMedia(QString(), FG_ALL, -1, conf.autorun);
				break;
			case XCUT_TAPLAY:
				if (tape_running(comp->tape)) {
					tapStateChanged(TW_STATE,TWS_STOP);
				} else {
					tapStateChanged(TW_STATE,TWS_PLAY);
				}
				break;
			case XCUT_TAPREC:
				if (comp->tape->on) {
					tapStateChanged(TW_STATE,TWS_STOP);
				} else {
					tapStateChanged(TW_STATE,TWS_REC);
				}
				break;
			case XCUT_VIDREC:
				videoRec();
				break;
			case XCUT_SCRSHOT:
				if (scrCounter == 0) {
					scrCounter = 1;
					scrInterval = 0;
				} else {
					scrCounter = 0;
				}
				break;
			case XCUT_RZXWIN:
				emit s_rzx_show();
				break;
			case XCUT_FASTSAVE:
				pause(true,PR_FILE);
				saveChanged();
				pause(false,PR_FILE);
				break;
			case XCUT_NMI:
				if (comp->rzx.play) break;
				if (comp->cpu->type != CPU_Z80) break;
				comp_irq(IRQ_NMI, comp);
				break;
			case XCUT_TAPWIN:
				emit s_tape_show();
				break;
			case XCUT_RESET:
				resetMachine(RES_DEFAULT);
				break;
			case XCUT_WAV_OUT:
				pause(true, PR_FILE);
				if (conf.snd.wavout) {
					snd_wav_close();
					setMessage(" stop WAV output ");
				} else {
					path = QFileDialog::getSaveFileName(this, "Sound output to WAV", "", "Wave files (*.wav)",nullptr,QFileDialog::DontUseNativeDialog);
					if (!path.isEmpty()) {
						if (!path.endsWith(".wav", Qt::CaseInsensitive))
							path.append(".wav");
						err = snd_wav_open(path.toLocal8Bit().data());
						if (err == ERR_OK) {
							setMessage(" start WAV output ");
						}
					}
				}
				pause(false, PR_FILE);
				break;
			default:
				// printf("%s %c %c\n", kent.name, kent.zxKey.key1, kent.zxKey.key2);
				//xt_press(comp->keyb, &kent);
				if (comp->hw->keyp)
					comp->hw->keyp(comp, &kent);
				if (kent.joyMask & 0xff) {
					if (kent.joyMask & XJ_JOYB) {
						joyPress(comp->joyb, kent.joyMask);
					} else {
						joyPress(comp->joy, kent.joyMask);
					}
				}
				break;
		}
	}
	emit s_keywin_upd(comp->keyb);
}

void MainWin::keyReleaseEvent(QKeyEvent *ev) {
	if (ev->isAutoRepeat()) return;
	releaseChord(ev, false);
	Computer* comp = conf.zx;
//	if (relskip) {
//		relskip = 0;
//	} else {
//		qDebug() << "keyReleaseEvent" << ev->text() << ev->nativeScanCode();
		int keyid;
		if (comp->flgDBG) {
			ev->ignore();
		} else {
			keyid = hotkeyOf(ev);
			if (keyid >= 0) {
				xcut_release(keyid);
			} else {	// not hotkeys
#if defined(__linux) || defined(__BSD)
				keyid = ev->nativeScanCode();
#elif defined(__WIN32)
				keyid = ev->nativeScanCode();
#if STICKY_KEY
				if (keyid == 0) {
					keyid = ev->nativeVirtualKey();
				} else if ((ev->key() == Qt::Key_Shift) || (ev->key() == Qt::Key_Control) || (ev->key() == Qt::Key_Alt)) {
					keyid = 0;
				}
#endif
				if (key_press_map[keyid] == 0) {
					keyid = 0;
				} else {
					key_press_map[keyid] = 0;
				}
#else
				keyid = qKey2id(ev->key());
#endif
				xkey_release(keyid);
			}
		}
//	}
}

// Fast mode's key, as its setting says it works
void MainWin::fast_key(bool down) {
	static xHoldKey key;
	if (!xhold_switches(&key, conf.emu.fastHold, conf.emu.fast, down)) return;
	conf.emu.fast ^= 1;
	updateHead();
}

// a hotkey let go: only the ones that are held answer it
void MainWin::xcut_release(int keyid) {
	if (keyid == XCUT_FAST) {
		fast_key(false);
	} else if (keyid == XCUT_REWIND) {
		rewind_want(0);
	} else if (xcut_tmode(keyid)) {
		xspeed_key(xcut_tmode(keyid), 0);
	}
}

void MainWin::xkey_release(int keyid) {
	Computer* comp = conf.zx;
	keyEntry kent = getKeyEntry(keyid);
	comp->keyb->grab = pckAct->isChecked();
	// xt_release(comp->keyb, &kent);
	if (comp->hw->keyr)
		comp->hw->keyr(comp, &kent);
	if (kent.joyMask) {
		if (kent.joyMask & XJ_JOYB) {
			joyRelease(comp->joyb, kent.joyMask & 0xff);
		} else {
			joyRelease(comp->joy, kent.joyMask & 0xff);
		}
	}
	emit s_keywin_upd(comp->keyb);
}

void MainWin::calcCoords(QMouseEvent* ev) {
	Computer* comp = conf.zx;
	if ((drawW < 1) || (drawH < 1)) return;
	int x = ((ev->xEventX - drawX) * comp->vid->vsze.x / drawW) + comp->vid->lcut.x - comp->vid->bord.x;
	int y = ((ev->xEventY - drawY) * comp->vid->vsze.y / drawH) + comp->vid->lcut.y - comp->vid->bord.y;
	if ((x >= 0) && (x < comp->vid->scrn.x) && (y >= 0) && (y < comp->vid->scrn.y)) {	// inside screen
		int adr, atr;
		vid_scr_adr(vid_scr_base(comp->vid->vidPage), x, y, &adr, &atr);
		setMessage(QString(" %0.%1 | %2 ").arg(gethexword(adr)).arg(vid_scr_bit(x)).arg(gethexword(atr)));
		emit s_scradr(x, y, adr, atr);
	}
}
