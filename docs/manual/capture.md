# Capture menu

```{index} Capture menu
```

The **Capture** menu saves what the machine shows and plays: still pictures, video, sound,
and RZX recordings of a game as it is played. Where the files go and what they look like is
set in the **Screenshots** and **Recording** groups of the [Video page](options-video.md).

## Screenshots

```{index} Screenshot, Screenshot Series
```

```{index} see: screen capture; Screenshot
```

**Capture > Screenshot** (F7 in both hotkey presets) saves the picture as a file. The message
**screenshot saved** appears in the corner of the picture, or **can't save screenshot** when
the file cannot be written.

**Capture > Screenshot Series** takes several screenshots in a row, a set number of frames
apart. Use
**Capture > Screenshot** while a series runs to stop it.

The files go into the screenshots folder, named after the time they were taken:
`xpeccy_153012_250.png` is 15:30:12 and 250 ms. The folder, the format and the number of
pictures in a series are set in [Screenshots](options-video.md#screenshots) on the
**Video** page.

## Record Video

```{index} Record Video, video
```

**Capture > Record Video** starts a video, and the same command again stops it. While it
records, a blinking sign in the top right corner of the picture says so; the sign is not in
the video. When the recording stops, the message **video saved:** names the file. Its
size, codec and name are set in [Recording](options-video.md#recording) on the **Video**
page, which can also start and stop a video by itself
([Auto recording](options-video.md#auto-recording)).

The video has the picture and the sound together, in an MP4 file by default. It is made by
FFmpeg, a free program that is not part of Xpeccy+.

### Getting FFmpeg

```{index} FFmpeg, Get FFmpeg
```

Xpeccy+ looks for FFmpeg in the `ffmpeg` folder inside the
[settings folder](getting-started.md#where-the-settings-are), then wherever your system
finds programs (and on macOS in Homebrew's folders). To use another copy, give its path in
the **FFmpeg** field of the [Recording](options-video.md#recording) group; leave the field empty to let Xpeccy+ find it.

When FFmpeg is not found, **Record Video** opens the **Get FFmpeg** window instead (the
{icon}`arrow-down` arrow button beside the **FFmpeg** field opens it too):

- **Windows** - downloads a ready-made FFmpeg (about 185 MB) into the settings folder and
  checks it. {icon}`arrow-down` **Get latest** downloads the newest release,
  {icon}`arrow-down` **Get previous** downloads the one before it. Take the previous one if
  your graphics driver is not new: with an old driver the newest release cannot use the
  graphics card to encode. A download that was cancelled or cut off continues where it
  stopped the next time.
- **Linux** - installs the distribution's `ffmpeg` package; the system asks for your
  password.
- **macOS** - installs it with Homebrew. When Homebrew is not installed, the window says
  where to get Homebrew.

When the window says **FFmpeg ... is ready**, close it and use **Record Video** again.

## Record Sound to WAV

```{index} Record Sound to WAV, WAV; recording sound, sound; recording
```

**Capture > Record Sound to WAV...** asks for a file and writes everything the machine
plays into it, until you choose the command again. The file is 16-bit stereo, at the
**Rate** set on the **Sound** page of **Options**. The messages **start WAV output** and
**stop WAV output** confirm it. `--wav-out <file>` on the command line records from the
start.

## RZX

```{index} RZX
```

An RZX file records a game as it is played: the machine's state at the start and every key
press and joystick move after it. Played back, it repeats the game exactly, frame for frame,
on any program that plays RZX. It is much smaller than a video. Players use it to show a
long game played to the end, or how a hard part of a game is done.

### Playing an RZX

```{index} RZX Player, RZX; playing
```

![The RZX Player](images/rzx-player.png)

Open an `.rzx` file with **File > Open...**, or with the {icon}`fileopen` button in the
**RZX Player** (**Capture > RZX > RZX Player**). It plays on the machine it was recorded on.
While it plays, a sign in the top right corner of the picture says so: a green triangle blinking
next to **RZX**. A pause or a speed sign takes its place while it is on, and a video of the window
leaves it out, as it does the recording sign.

The **RZX Player** shows:

- **Recorded with**, **Machine**, **Length** and **Snapshots** - what the file says about
  itself. For files made by Xpeccy+, **Recorded with** also tells how many rollbacks and how
  much slow motion were used while recording.
- the list of the file's blocks, with **Start**, **Length**, **Block** and **Info**; the
  part being played is marked. Double-click a row to go there.
- the progress bar and the time; click the bar to go to that point.
- {icon}`tape-rec` **Record**, {icon}`tape-play` **Play / Pause**, {icon}`tape-stop` **Stop**,
  and {icon}`rzx-bookmark` **Add a bookmark to roll back to**.

### Recording an RZX

```{index} RZX; recording, Add Bookmark, Rollback to Bookmark, Stop and Finalize, bookmark; RZX
```

![Capture > RZX](images/menu-rzx.png)

**Capture > RZX > Record...** asks for a file and starts recording from the machine's
current state. A blinking sign in the corner of the picture shows that recording is on. The
file is saved every 30 seconds, so a crash does not lose the game.

{icon}`tape-rec` **Record** in the **RZX Player** during playback starts recording from
that point instead: the new recording keeps everything played so far and goes on from the
current frame.

While you record:

- **Add Bookmark** - remember this point. A bookmark added while the machine is paused is
  set when the machine runs again.
- **Rollback to Bookmark** - go back to the last bookmark, or to the start when there is
  none, and play on from there; what was recorded after that point is thrown away.
  [Rewind](time.md#rewind) takes the recording back the same way.
- **Stop and Finalize** - end the recording and save it without the bookmarks: this is the
  file to share.

```{index} Open and Continue, Finalize File
```

Ending the recording any other way - **Record...** again, {icon}`tape-stop` **Stop** in the
player, opening another RZX file, switching machines or quitting Xpeccy+ - saves it with its bookmarks, so
you can go on with it later. **Open and Continue...** is how you go on:
it opens a recording, runs it to its end at full speed, and records on from there; you are
asked for the file to record into, with the same file offered.

**Finalize File...** removes the bookmarks from a recording saved earlier, without playing
it.
