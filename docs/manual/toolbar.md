# Toolbar

```{index} toolbar, toolbar; customizing, Restore Default Buttons, Icon Size, Lock Toolbar
```

By default the toolbar has {icon}`fileopen` **Open**, {icon}`floppy` **Save**,
{icon}`shutdown` **Reset**, {icon}`model-zx48` **Model**, {icon}`tape` **Tape Player**,
{icon}`fdd_disk` **Disk Manager**, {icon}`fullscreen-16` **Fullscreen**,
{icon}`vkeys` **Virtual Keyboard**, {icon}`gamepad` **Gamepads**, {icon}`bug` **Debugger**,
{icon}`other` **Options** and {icon}`speaker` **Mute**, and at the right end
{icon}`time-rewind` **Rewind**, {icon}`time-slow` **Slow Motion**,
{icon}`time-pause` **Pause**, {icon}`time-ffwd` **Fast Forward** and
{icon}`time-fast` **Fast Mode**.

Right-click the toolbar to change it:

- **Buttons** - every command that can be a button, by group; tick the ones you want.
- **Remove** - removes the button you right-clicked.
- **Add Separator Here**, **Add Space Here** - a line or a gap between groups.
- **Icon Size** - **Auto**, the default: 24 x 24 while every button fits the window's
  width, 16 x 16 when it does not. Or always 16 x 16 or 24 x 24.
- **Restore Default Buttons** - back to the set above.
- **Lock Toolbar** - on by default, so a slip of the mouse moves no button.
- **Hide Toolbar** - **View > Toolbar** brings it back.

To move a button, untick **Lock Toolbar** and drag the button.

```{index} Mute, volume
```

The arrow beside **Mute** opens a volume slider. Alt with the mouse wheel over the picture
changes the volume too.

## The buttons

```{index} toolbar; buttons
```

**Buttons** in the toolbar's right-click menu lists every command that can be a button,
in groups. Each one is the command of the same name in a menu, described there:

| Described in | Buttons |
|---|---|
| [File menu](files.md), [Favorites menu](favorites.md) | **Open...**, **Favorites**, **Reload**, **Save...**, **Save Changed Disks**, **Options...** |
| [Capture menu](capture.md) | **Screenshot**, **Record Video**, **Record Sound to WAV...**, **Record...** (RZX), **RZX Player** |
| [Machine menu](machines.md) | **Model**, **Reset**, **NMI**, **Turbo Mode** |
| [Speed and time](time.md) | **Pause**, **Fast Mode**, **Fast Forward**, **Slow Motion**, **Rewind** |
| [Media menu](media.md) | **Tape Play/Stop**, **Tape Record/Stop**, **Tape Player**, **Disk Manager**, **Cartridge**, **divMMC**, **SD Card**, **Drives** |
| [View menu](view.md) | **Fullscreen**, **Keep Aspect Ratio**, **Size**, **Shaders**, **ZX Palette**, **Virtual Keyboard** |
| [Input menu](input.md) | **Grab Keyboard**, **Grab Mouse**, **Keymap**, **Gamepads** |
| [Tools menu](tools.md) | **Cheats...**, **Debugger**, **Screen**, **Sound Chips** |
| [Mute](machines.md#mute) | **Mute** |

A button for a menu - **Model**, **Turbo Mode**, **Size** and the like - opens that menu
when clicked. A button with an arrow beside it does its command when clicked, and the arrow
opens its menu: **Reset** lists the ROMs, **Disk Manager** the drives, **divMMC** the cards,
**Gamepads** the joysticks, **Debugger** its windows, **Mute** the volume slider.

**Rewind** has no menu item: it works while the button or its key is held down. The time
buttons work as their keys do; see [How the keys work](time.md#how-the-keys-work).
