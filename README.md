# monitorctl

A command-line tool that reads and changes an external monitor's settings from an Apple Silicon Mac.

It talks to the monitor over DDC/CI, the control channel inside the display cable. This is the same channel that the monitor's own menu settings can be reached through.

The project page and the monitor database are at [felthosfoundry.com/monitorctl](https://felthosfoundry.com/monitorctl/).

## Install

```bash
curl -fsSL https://felthosfoundry.com/monitorctl/install.sh | sh
```

This runs [install.sh](install.sh) from this repository. The script downloads the source from GitHub, builds it on your Mac, and copies the program to `~/.local/bin/monitorctl`. It needs the Xcode command-line tools, and it tells you how to get them when they are missing.

The script never asks for a password and never edits your shell's startup files. When `~/.local/bin` is not on your `PATH`, it prints the one line that adds it. To install somewhere else, set `PREFIX`:

```bash
curl -fsSL https://felthosfoundry.com/monitorctl/install.sh | PREFIX="$HOME/tools" sh
```

Running the command again installs the newest version.

To build it yourself instead:

```bash
git clone https://github.com/felthosfoundry/monitorctl.git
cd monitorctl
make install
```

## Use

```bash
monitorctl list
```

`list` shows every named control and its current value:

```text
brightness     100 of 100
contrast       75 of 100
volume         50 of 100
mute           off (2)
red            70 of 100
green          70 of 100
blue           70 of 100
color-preset   5000k (4)
picture-mode   2 of 3
input          4
power          on (1)
```

The other commands are:

| Command | What it does | Example |
|---|---|---|
| `get <control>` | Prints the value as a plain number. | `monitorctl get brightness` |
| `set <control> <value>` | Sets an exact value. | `monitorctl set brightness 80` |
| `chg <control> <change>` | Raises or lowers the value, and prints the new one. | `monitorctl chg volume -5` |
| `scan` | Reads every code from 0x00 to 0xFF and lists the ones the monitor supports. | `monitorctl scan` |
| `caps` | Prints the monitor's own capability string. | `monitorctl caps` |
| `info` | Shows the model, resolution, Thunderbolt firmware and capability string. | `monitorctl info` |
| `timing` | Shows the signal the monitor says it is receiving. | `monitorctl timing` |
| `setup` | Tests what the monitor supports and saves the result as a profile. | `monitorctl setup` |
| `help` | Shows the usage text. | `monitorctl help` |

A control is one of the names from `list`, or a raw code such as `0x10`. A value is a number. Some controls also take names:

- `mute`: `on`, `off`
- `power`: `on`, `off`
- `color-preset`: `srgb`, `native`, `4000k`, `5000k`, `6500k`, `7500k`, `8200k`, `9300k`, `10000k`, `11500k`, `user1`, `user2`, `user3`
- `input`: `dp1`, `dp2`, `hdmi1`, `hdmi2`, `usbc`

`chg` keeps the result between 0 and the maximum the monitor reports.

## Facts about the display

```bash
monitorctl info
```

`info` shows four facts about the display itself:

```text
model          C34J79x
resolution     3440 x 1440 at 100 Hz
thunderbolt    firmware 61.2
capabilities   (prot(monitor)type(lcd)model(FALCON)...)
```

The first three lines come from macOS, so they print even when the monitor does not answer. The last line comes from the monitor, and it reads `the monitor did not answer` when the monitor is silent.

- **model** is the product name the monitor reports to macOS.
- **resolution** is the picture macOS is sending right now. It is the only sign of picture-by-picture on a monitor that has no code for it: the Samsung C34J791 shows `1720 x 1440 at 60 Hz` with picture-by-picture on.
- **thunderbolt** is the firmware version of the monitor's Thunderbolt controller, as System Information shows it. A monitor on a DisplayPort or HDMI cable has no such controller, and the line reads `not connected over Thunderbolt`.
- **capabilities** is the same string that `caps` prints.

## The signal the monitor sees

```bash
monitorctl timing
```

`timing` asks the monitor what signal it is receiving:

```text
line rate      150.9 kHz
refresh rate   100.4 Hz
sync           horizontal positive, vertical positive
signal         stable and in range
```

It is a read, so nothing on the monitor changes. `info` reports the resolution macOS is sending, and `timing` reports what arrives at the monitor.

- **line rate** is how many lines the monitor draws each second.
- **refresh rate** is how many pictures it draws each second.
- **sync** is the polarity of the two timing signals in the cable.
- **signal** reads `unstable` when the monitor's own measurement of the signal is not steady, and `out of range` when the signal is outside what the monitor accepts.

The two rates are the same numbers as codes `0xAC` and `0xAE`. No settings code reports the last line.

## What your monitor supports

```bash
monitorctl setup
```

`setup` finds out what the connected monitor supports and saves the answer as a profile. It takes about half a minute:

```text
monitor        C34J79x (SAM0F1E)
database       1 report for this monitor: https://felthosfoundry.com/monitorctl/SAM0F1E
codes          35 can be read
brightness     writes work
contrast       writes work
volume         writes work
mute           writes work
red            writes work
green          writes work
blue           writes work
color-preset   writes not tested
picture-mode   writes not tested
input          writes not tested here, and are ignored in the database
power          writes not tested
profile        /Users/you/.config/monitorctl/SAM0F1E.txt
```

It works in five steps:

1. It asks the monitor database what other people found on the same monitor.
2. It reads every code from 0x00 to 0xFF and records the ones the monitor supports.
3. It tests writes on brightness, contrast, volume, mute and the three colour gains. Each one moves by one step, is read back, and is put back. The picture may flicker, and a muted monitor may sound for a moment.
4. It checks the colour preset and the picture mode, and puts them back if a test changed them as a side effect.
5. It saves the profile, and then asks whether to send it to the monitor database.

`setup` writes to nothing else. It never writes to `input` or `power`, because a monitor that changes input or switches off stops answering, and the old value could then not be put back. It never writes to a factory-reset code or a manufacturer-private code either. Those codes appear in the profile as `untested`.

When a setting cannot be put back, `setup` says which one, still saves the profile, and exits with code 3.

### The monitor database

The [monitor database](https://felthosfoundry.com/monitorctl/) holds what other people's monitors reported. `setup` asks it about your monitor and adds what it learns to the summary in two cases:

- **A control that `setup` does not test.** The line for `input` above says that the database found input writes are ignored on this monitor.
- **A disagreement.** When your monitor and the database differ, the line says so, for example `writes work here, but are ignored in the database`.

The lookup goes to `felthosfoundry.com` and carries the monitor's identifier, such as `SAM0F1E`, and nothing else. To skip it, add `--offline`:

```bash
monitorctl setup --offline
```

A database that cannot be reached is not an error. `setup` says so and carries on.

### Sending your profile

After the profile is saved, `setup` asks whether to send it to the database:

```text
Send this profile to the monitor database at felthosfoundry.com, so the next person
with this monitor knows what works? It is the file above, and it holds no serial number. [y/N]
```

The default answer is no, and nothing is sent without a yes. On a yes, `setup` sends the saved file, byte for byte. You can read the file first, because its path is printed just above the question.

- **What the site keeps.** It keeps the profile, the time it arrived, and a four-digit bucket derived from your network address. Tens of thousands of addresses share each bucket, so it tells senders apart well enough to count reports and cannot be turned back into an address. The address itself is not stored.
- **Review.** A person reads every report before it appears on the site.
- **Scripts.** When `setup` is not running in a terminal it does not ask, and it prints the command to use. `monitorctl setup --upload` sends without asking.
- **Never.** `monitorctl setup --offline` does not look the monitor up, does not ask, and does not send.

A send that fails is reported, and `setup` exits with code 1. The profile is saved all the same.

The database only describes a monitor. Nothing it returns can make `monitorctl` write to a code, and it cannot unlock a write that needs `--force`. It is also kept out of your profile, so the profile stays a record of what your own monitor did.

### The profile

The monitor's identifier is the three manufacturer letters and the product number from its EDID, such as `SAM0F1E`. The profile is a plain text file named after it, with one fact per line:

```text
monitorctl-profile 1
id SAM0F1E
model C34J79x
built 2022 week 44
thunderbolt 61.2
caps (prot(monitor)type(lcd)model(FALCON)...)
code 0x10 max 100 write works
code 0x60 max 3 write untested
```

Each `code` line gives a code the monitor supports, the maximum the monitor reports for it, and what happened when `setup` tested a write: `works`, `ignored` or `untested`. A line for a fact that is not known is left out.

The profile holds no serial number and none of your current settings. `setup` saves it under `~/.config/monitorctl/`, or under `$XDG_CONFIG_HOME/monitorctl/` when that is set. The profile is sent only when you say yes to the question above, or pass `--upload`. The other commands do not read the profile yet.

### When the monitor does not answer

Most monitors answer a single request after a 60 ms wait. Some answer with a null message, which is a valid DDC/CI reply that holds nothing, until they are given more time or the request a second time. So while no request has been answered, each retry uses another method: the request sent twice, a 250 ms wait, and both. The first method that gets an answer is used for the rest of the command, and `setup` names it in a `requests` line when it is not the usual one. A monitor that answers the usual method is never asked in any other way.

Some monitors do not answer at all. `setup` then tries each method once more on the brightness, reading only, and says where the request was lost:

```text
The monitor did not answer.
The Mac sent the request and could not read a reply (macOS error 0xE00002D6).
It may be off, asleep, or connected through a dock or adapter that does not carry DDC/CI.

problem        /Users/you/.config/monitorctl/SAM74A3-problem.txt
```

It saves what it saw as a problem report next to the profiles, and asks whether to send it to the database. The rules are the same as for a profile: nothing is sent without a yes, `--upload` sends without asking, and `--offline` neither asks nor sends. `setup` exits with code 1 either way.

```text
monitorctl-problem 1
id SAM74A3
model S34C65xT
built 2023 week 40
computer Mac16,7
system 27.2.1
displays 1
channels 1
device SAMSUNG S34C65T firmware 41.1
step scan
cause not-read
try 1 wait 60 write 0x00000000 read 0xE00002D6
try 2 wait 60 twice write 0x00000000 read 0xE00002D6
try 3 wait 250 write 0x00000000 read 0xE00002D6
try 4 wait 250 twice write 0x00000000 read 0xE00002D6
```

| Line | What it holds |
|---|---|
| `id`, `model`, `built`, `thunderbolt` | The same facts as in a profile. |
| `computer`, `system` | The Mac's model and the macOS version. |
| `displays`, `channels` | How many external displays the Mac sees, and for how many of them macOS offers a DDC/CI channel. |
| `device` | A Thunderbolt device plugged into the Mac: its maker, its model and its firmware. This can include a dock or a drive. |
| `cause` | Where the request was lost: `not-sent`, `not-read`, `empty`, `invalid`, or `null` when the monitor sent only null messages. |
| `try` | One attempt: how long it waited for the reply, whether it sent the request twice, the code macOS returned for the write and for the read, and the reply's bytes when there was one. |

The report holds no serial number, no user name, no computer name and none of your settings.

## Writes are checked

A monitor replies "accepted" to a write even when it then ignores it. So after every write, `monitorctl` reads the value back. When the value did not change, it says so and exits with code 3:

```text
$ monitorctl set input dp1
The monitor accepted the command, but input is still 4.
```

## Writes that need `--force`

Three kinds of write are refused unless you add `--force`:

- **Switching the monitor off** (`set power off`). A monitor that is off stops listening for commands. Only the button or joystick on the monitor can switch it on again.
- **Factory resets** (codes `0x04`, `0x05`, `0x06`, `0x08` and `0x0A`). They discard the monitor's current settings.
- **Manufacturer-private codes** (`0xE0` to `0xFF`). Their effect differs from one monitor to the next and is usually undocumented.

To turn the screen off in a way that a key press undoes, sleep the Mac's display instead:

```bash
pmset displaysleepnow
```

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Done. |
| 1 | The monitor did not answer, or does not support the code. For `setup`, a profile that was to be sent could not be. |
| 2 | The command line was wrong. |
| 3 | The monitor accepted a write and the value did not change, or `setup` could not put a setting back. |
| 4 | The write is guarded and `--force` was not given. |

## Tested monitor

Every command was tested on a Samsung C34J791 connected by Thunderbolt 3. Brightness, contrast, volume, mute, red gain, colour preset and picture mode all read and write. Three things are specific to that monitor:

- It ignores writes to `input`, with picture-by-picture on and off.
- It has no code for picture-by-picture.
- Changing the volume unmutes it.

The meaning of each `picture-mode` number is the monitor's own and is not documented, so that control takes numbers only.

## Limits

- It works on Apple Silicon Macs only. It uses a private macOS interface, IOAVService, which is the same one that `m1ddc`, MonitorControl and BetterDisplay use. Apple could change it in a macOS update.
- It controls the first external display it finds. There is no option yet to choose between several.
- A monitor answers only on the input it is currently showing.
- `info` reports the resolution macOS draws at. On a display running a scaled HiDPI mode, that is larger than the signal the monitor receives.

## Develop

```bash
make test
```

The tests need no monitor. They run every command against a pretend monitor that answers DDC/CI packets, including one that ignores writes and one that stops answering after it is switched off.

They also run `install.sh` against a tarball of the working tree, with an empty folder as the home folder. That part needs no network.

| File | What it holds |
|---|---|
| `src/ddc.c` | Building request packets and checking replies. |
| `src/controls.c` | The named controls, value names and the guard list. |
| `src/cli.c` | Turning the command line into a command. |
| `src/run.c` | Carrying out a command through a transport. |
| `src/profile.c` | Building a monitor's identifier, and writing, reading and saving a profile. |
| `src/problem.c` | Judging where a request was lost when a monitor does not answer, and writing a problem report. |
| `src/transport_mac.c` | The only code that touches hardware. |
| `src/info_mac.c` | Reading the model, resolution and Thunderbolt devices from macOS, and the facts that help when a monitor does not answer. |
| `src/database_curl.c` | Asking the monitor database for a record and sending it a profile, by running the curl that ships with macOS. |
| `src/ask_tty.c` | Asking a yes-or-no question in the terminal. |
| `install.sh` | The install command: downloading the source, building it and copying the program into place. |

## Licence

MIT. See [LICENSE](LICENSE). Copyright Felthos Foundry.
