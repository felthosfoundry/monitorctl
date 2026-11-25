# monitorctl

A command-line tool that reads and changes an external monitor's settings from an Apple Silicon Mac.

It talks to the monitor over DDC/CI, the control channel inside the display cable. This is the same channel that the monitor's own menu settings can be reached through.

## Install

```bash
git clone https://github.com/felthosfoundry/monitorctl.git
cd monitorctl
make install
```

This builds the tool and copies it to `~/.local/bin/monitorctl`. To install somewhere else, set `PREFIX`, for example `make install PREFIX=/usr/local`. It needs Xcode or the Xcode command-line tools.

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
| 1 | The monitor did not answer, or does not support the code. |
| 2 | The command line was wrong. |
| 3 | The monitor accepted a write and the value did not change. |
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

| File | What it holds |
|---|---|
| `src/ddc.c` | Building request packets and checking replies. |
| `src/controls.c` | The named controls, value names and the guard list. |
| `src/cli.c` | Turning the command line into a command. |
| `src/run.c` | Carrying out a command through a transport. |
| `src/transport_mac.c` | The only code that touches hardware. |
| `src/info_mac.c` | Reading the model, resolution and Thunderbolt firmware from macOS. |

## Licence

MIT. See [LICENSE](LICENSE). Copyright Felthos Foundry.
