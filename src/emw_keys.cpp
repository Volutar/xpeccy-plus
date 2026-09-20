#include "emulwin.h"
#include "filer.h"
#include "xcore/xcore.h"
#include "xcore/vscalers.h"
#include "xcore/sound.h"

#include <QMenu>
#include <QFileDialog>

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
// no grab, autorepeat: if !shortcut then get keyid, xt_release, xt_press

int ev_to_keyid(QKeyEvent* ev, bool kgrab) {
	int keyid = -1;
	if (!kgrab) {
		keyid = shortcut_check(SCG_MAIN, QKeySequence(ev->key() | xNativeMods(ev->modifiers())));
		if (keyid < 0)
			keyid = shortcut_check(SCG_MAIN, QKeySequence(ev->key()));
	}
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
			} else {
				xkey_press(keyid);
			}
		} else {
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

void MainWin::xkey_press(int xkey) {
	keyEntry kent = getKeyEntry(xkey);
//	printf("xkey_press %s\n", kent.name);
	int x;
	int y;
	int err;
	QSize wsz;
	QString path;
	Computer* comp = conf.zx;
	comp->keyb->grab = pckAct->isChecked();
	if (pckAct->isChecked()) {
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
		if (xkey == XKEY_F12) {
			compReset(comp,RES_DEFAULT);
			emit s_rzx_stop();
		}
	} else {
		switch (xkey) {
			case XCUT_FULLSCR:
				vid_set_fullscreen(!conf.vid.fullScreen);
				setMessage(conf.vid.fullScreen ? " fullscreen on " : " fullscreen off ");
				updateWindow();
				saveConfig();
				if (!conf.vid.fullScreen) {
					wsz = SCREENSIZE;
					x = (wsz.width() - width()) / 2;
					y = (wsz.height() - height()) / 2;
					move(x, y);
				}
				break;
			case XCUT_SIZEX1:
				vid_set_zoom(1);
				updateWindow();
				saveConfig();
				setMessage(" size x1 ");
				break;
			case XCUT_SIZEX2:
				vid_set_zoom(2);
				updateWindow();
				saveConfig();
				setMessage(" size x2 ");
				break;
			case XCUT_SIZEX3:
				vid_set_zoom(3);
				updateWindow();
				saveConfig();
				setMessage(" size x3 ");
				break;
			case XCUT_SIZEX4:
				vid_set_zoom(4);
				updateWindow();
				saveConfig();
				setMessage(" size x4 ");
				break;
			case XCUT_SIZEX5:
				vid_set_zoom(5);
				updateWindow();
				saveConfig();
				setMessage(" size x5 ");
				break;
			case XCUT_SIZEX6:
				vid_set_zoom(6);
				updateWindow();
				saveConfig();
				setMessage(" size x6 ");
				break;
			case XCUT_COMBOSHOT:
				scrCounter = conf.scrShot.count;
				scrInterval = 0;
				break;
			case XCUT_RES_DOS:
				compReset(comp,RES_DOS);
				emit s_rzx_stop();
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
			case XCUT_FAST:
				if (conf.emu.pause) break;
				conf.emu.fast ^= 1;
				updateHead();
				break;
			case XCUT_TURBO:
				// the board's own turbo, walked through the steps it declares.
				// A machine that sets it from a port will set it again itself
				if (comp->turboCount < 2) {
					setMessage(" model has no Turbo ");
				} else if (comp->rzx.play) {
					setMessage(" not in RZX ");
				} else {
					// an unknown step reads as -1 and so starts the list over
					int i = (xm_turbo_index(comp) + 1) % comp->turboCount;
					comp->turboStep = comp->turboTab[i];
					compSetHwTurbo(comp, comp->turboStep);
					setMessage(QString(" %0 MHz (x%1) ").arg(xspeed_clock(), 0, 'g', 6)
						.arg(comp->turboStep));
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
				pckAct->setChecked(true);
				setMessage(" grab keyboard ");
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
				fillUserMenu();
				userMenu->popup(pos() + QPoint(20,20));
				userMenu->setFocus();
				break;
			case XCUT_OPTIONS:
				pause(true, PR_OPTS);
				emit s_options();
				break;
			case XCUT_SAVE:
				pause(true,PR_FILE);
				save_file(comp, NULL, FG_ALL, -1);
				pause(false,PR_FILE);
				break;
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
#if HAVEZLIB
				if (comp->rzx.play) break;
#endif
				if (comp->cpu->type != CPU_Z80) break;
				comp_irq(IRQ_NMI, comp);
				break;
			case XCUT_TAPWIN:
				emit s_tape_show();
				break;
			case XCUT_RESET:
				compReset(comp,RES_DEFAULT);
				emit s_rzx_stop();
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
	Computer* comp = conf.zx;
//	if (relskip) {
//		relskip = 0;
//	} else {
//		qDebug() << "keyReleaseEvent" << ev->text() << ev->nativeScanCode();
		int keyid;
		if (comp->flgDBG) {
			ev->ignore();
		} else {
			keyid = -1;
			if (!pckAct->isChecked()) {
				keyid = shortcut_check(SCG_MAIN, QKeySequence(ev->key() | xNativeMods(ev->modifiers())));
				if (keyid < 0)
					keyid = shortcut_check(SCG_MAIN, QKeySequence(ev->key()));
			}
			if (keyid < 0) {	// not hotkeys
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
				// for debug
				if ((ev->modifiers() & Qt::AltModifier) && (ev->key() == Qt::Key_PageUp)) {
					comp->dif->fdc->debug ^= 1;
					xlog(XLG_DISK, XLL_DEBUG, "FDC debug: %i", comp->dif->fdc->debug);
				}
				xkey_release(keyid);
			}
		}
//	}
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
