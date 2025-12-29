# ADAMCOM

A minicom-like terminal for Serial and CAN bus communication with support for persistent presets, multi-preset repeat mode, and an interactive menu.

## Features

- **Serial & CAN Support**: Connect to serial ports or SocketCAN interfaces
- **10 Persistent Presets**: Store and send frequently used messages (Alt+1-9,0)
- **Multi-Repeat Mode**: Repeat multiple presets simultaneously with independent intervals
- **Interactive Menu**: Configure settings on-the-fly with Ctrl-T
- **Non-Interrupting Output**: RX/TX messages appear above your input line
- **Slash Commands**: Quick access to all features via `/command` syntax
- **Hex & Text Modes**: Send and receive data in hex or text format

## Installation

```bash
bash <(curl -sL https://raw.githubusercontent.com/strfr/adamcom/main/install.sh)
```

## Quick Start

```bash
# Connect to serial port
adamcom -d /dev/ttyUSB0 -b 115200

# Connect to CAN interface
adamcom -c can0 --canbitrate 500000
```

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Ctrl-C | Exit program |
| Ctrl-T | Open settings menu |
| Alt+1-9,0 | Send preset 1-10 |

## Slash Commands

| Command | Description |
|---------|-------------|
| `/p N` | Send preset N immediately |
| `/p N -r` | Start repeating preset N (default 1000ms) |
| `/p N -r -t MS` | Start repeating preset N with MS interval |
| `/p N -nr` | Stop repeating preset N |
| `/rs` | Show repeat status for all repeats |
| `/rs stop` | Stop inline repeat |
| `/ra` | Stop ALL repeats (presets + inline) |
| `/hex XX XX` | Send raw hex bytes |
| `/can ID XX XX` | Send CAN frame (ID in hex, up to 8 data bytes) |
| `/clear` | Clear screen |
| `/device PATH` | Switch serial device |
| `/baud RATE` | Change baud rate |
| `/mode normal\|hex` | Set display mode |
| `/crlf on\|off` | Toggle CRLF append |
| `/status` | Show current settings |
| `/menu` | Open settings menu |
| `/help` | Show available commands |

## Multi-Repeat Mode

Send multiple presets simultaneously with independent intervals:

```
/p 1 -r -t 250    # Start preset 1 every 250ms
/p 2 -r -t 1000   # Start preset 2 every 1000ms (runs alongside preset 1)
/rs               # Check which presets are running
/p 1 -nr          # Stop preset 1 only
/ra               # Stop all repeating presets
```

## Inline Repeat Mode

Send arbitrary data with inline flags (works in all modes):

```
# Hex mode examples:
FF FF FF FF -r                    # Repeat every 1000ms (default)
AA BB CC DD -r -t 100             # Repeat every 100ms
AA BB CC DD -id 0x123 -r          # CAN: Repeat to specific ID

# Text mode examples:
hello world -r -t 500             # Repeat text every 500ms
ATZ -r -t 1000                    # Repeat AT command every second

# Control:
/rs stop                          # Stop inline repeat
/ra                               # Stop all repeats
```

Flags (`-r`, `-t`, `-id`) can appear anywhere; only data is sent.

## Configuration

Settings are saved to `~/.adamcomrc` automatically. Presets are stored as:
- `preset1_name`, `preset1_data`, `preset1_format`
- Per-preset CAN IDs with `preset1_can_id`

## Building from Source

```bash
make
sudo make install
```

## Dependencies

- GNU Readline
- Linux with SocketCAN support (for CAN mode)
