/**
 * @file menu.cpp
 * @brief Interactive settings menu, preset editor, and manual
 */

#include "adamcom.hpp"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <unistd.h>
#include <iostream>
#include <string>

namespace adamcom {

// Define the global preset repeats array
std::array<PresetRepeatState, 10> g_preset_repeats{};

// Define the global inline repeat state
InlineRepeatState g_inline_repeat{};

void clear_screen()
{
    std::printf("\033[2J\033[H");
    std::fflush(stdout);
}

static std::string read_line()
{
    std::string line;
    std::getline(std::cin, line);
    return line;
}

static void flush_stdin()
{
    int c;
    while ((c = std::getchar()) != '\n' && c != EOF) {}
}

void show_manual()
{
    clear_screen();
    std::printf("\n");
    std::printf("╔══════════════════════════════════════════════════════════════════════════════╗\n");
    std::printf("║                           ADAMCOM USER MANUAL                                ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ ADAMCOM is a minicom-like terminal for Serial and CAN communication.        ║\n");
    std::printf("║ Supports 10 persistent presets, multi-repeat mode, and live configuration.  ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ KEYBOARD SHORTCUTS                                                          ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ Ctrl-C    Exit program                                                      ║\n");
    std::printf("║ Ctrl-T    Open settings menu                                                ║\n");
    std::printf("║ Alt+1-9,0 Send preset 1-10                                                  ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ SLASH COMMANDS                                                              ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ /p N                Send preset N immediately                               ║\n");
    std::printf("║ /p N -r             Start repeating preset N (default 1000ms)               ║\n");
    std::printf("║ /p N -r -t MS       Start repeating preset N with MS milliseconds interval  ║\n");
    std::printf("║ /p N -nr            Stop repeating preset N                                 ║\n");
    std::printf("║ /rs                 Show repeat status for all repeats                      ║\n");
    std::printf("║ /rs stop            Stop inline repeat                                      ║\n");
    std::printf("║ /ra                 Stop ALL repeats (presets + inline)                     ║\n");
    std::printf("║ /hex XX XX ...      Send raw hex bytes                                      ║\n");
    std::printf("║ /can ID XX XX       Send CAN frame (ID in hex, up to 8 data bytes)          ║\n");
    std::printf("║ /clear              Clear screen                                            ║\n");
    std::printf("║ /device PATH        Switch serial device (e.g., /device /dev/ttyUSB1)       ║\n");
    std::printf("║ /baud RATE          Change baud rate (e.g., /baud 115200)                   ║\n");
    std::printf("║ /mode MODE          Set mode (normal/hex)                                   ║\n");
    std::printf("║ /crlf on|off        Toggle CRLF append                                      ║\n");
    std::printf("║ /status             Show current connection settings                        ║\n");
    std::printf("║ /help               Show available slash commands                           ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ INLINE REPEAT MODE (works in all modes: Serial/CAN, Hex/Text)               ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ Add flags to any input line to enable repeat:                               ║\n");
    std::printf("║   -r         Start repeating (default 1000ms)                               ║\n");
    std::printf("║   -t MS      Set repeat interval in milliseconds                            ║\n");
    std::printf("║   -id 0xNNN  CAN only: send to specific CAN ID                              ║\n");
    std::printf("║                                                                             ║\n");
    std::printf("║ Examples:                                                                   ║\n");
    std::printf("║   FF FF FF -r              Hex mode: repeat every 1000ms                    ║\n");
    std::printf("║   AA BB -r -t 100          Hex mode: repeat every 100ms                     ║\n");
    std::printf("║   hello -r -t 500          Text mode: repeat every 500ms                    ║\n");
    std::printf("║   AA BB -id 0x03 -r        CAN: repeat to specific ID                       ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ MULTI-REPEAT MODE (Presets)                                                 ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ You can repeat multiple presets simultaneously with independent intervals:  ║\n");
    std::printf("║   /p 1 -r -t 250    Start preset 1 every 250ms                              ║\n");
    std::printf("║   /p 2 -r -t 1000   Start preset 2 every 1000ms (runs alongside preset 1)   ║\n");
    std::printf("║   /rs               Check which presets are running                         ║\n");
    std::printf("║   /p 1 -nr          Stop preset 1 only                                      ║\n");
    std::printf("║   /ra               Stop all repeating presets                              ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ CONFIGURATION FILE                                                          ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ Settings are saved to ~/.adamcomrc automatically.                           ║\n");
    std::printf("║ Presets are stored as preset1_name, preset1_data, preset1_format, etc.      ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ MODES                                                                       ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════════════════╣\n");
    std::printf("║ normal - Send text directly, receive and display as ASCII                   ║\n");
    std::printf("║ hex    - Input hex bytes, display received data as hex values               ║\n");
    std::printf("╚══════════════════════════════════════════════════════════════════════════════╝\n");
    std::printf("\nPress Enter to return...");
    std::fflush(stdout);
    flush_stdin();
}

void show_presets_menu(Config& cfg, InterfaceType itype)
{
    while (true) {
        clear_screen();
        std::printf("\n");
        std::printf("┌─────────────────────────────────────────────────────────────┐\n");
        std::printf("│                    Preset Editor                            │\n");
        std::printf("├─────────────────────────────────────────────────────────────┤\n");

        for (int i = 1; i <= 10; ++i) {
            std::string pi = std::to_string(i);
            std::string name = cfg["preset" + pi + "_name"];
            std::string data = cfg["preset" + pi + "_data"];
            std::string canid = cfg["preset" + pi + "_can_id"];

            if (canid.empty()) {
                canid = cfg["can_id"];
            }

            if (name.length() > 14) name = name.substr(0, 14);
            if (data.length() > 20) data = data.substr(0, 17) + "...";

            std::printf("│ %2d) %-14s  ", i, name.c_str());
            if (itype == InterfaceType::CAN) {
                std::printf("ID:%-8s  ", canid.c_str());
            } else {
                std::printf("            ");
            }
            std::printf("%-20s │\n", data.c_str());
        }

        std::printf("├─────────────────────────────────────────────────────────────┤\n");
        std::printf("│ Select [1-10] to edit, or Q to return                       │\n");
        std::printf("└─────────────────────────────────────────────────────────────┘\n");
        std::printf("\nChoice: ");
        std::fflush(stdout);

        char ch;
        if (std::scanf(" %c", &ch) != 1) continue;
        flush_stdin();

        if (ch == 'q' || ch == 'Q') return;

        int idx = -1;
        if (ch == '0') {
            idx = 10;
        } else if (ch >= '1' && ch <= '9') {
            idx = ch - '0';
        }

        if (idx < 1 || idx > 10) continue;

        std::string pi = std::to_string(idx);
        std::string key_name = "preset" + pi + "_name";
        std::string key_fmt = "preset" + pi + "_format";
        std::string key_data = "preset" + pi + "_data";
        std::string key_canid = "preset" + pi + "_can_id";

        clear_screen();
        std::printf("\n");
        std::printf("┌─────────────────────────────────────────────┐\n");
        std::printf("│           Preset %-2d Options                 │\n", idx);
        std::printf("├─────────────────────────────────────────────┤\n");
        std::printf("│ Current values:                             │\n");
        std::printf("│   Name: %-35s │\n", cfg[key_name].c_str());
        std::printf("│   Format: %-33s │\n", cfg[key_fmt].c_str());
        if (itype == InterfaceType::CAN) {
            std::printf("│   CAN ID: %-33s │\n", cfg[key_canid].c_str());
        }
        std::printf("│   Data: %-35s │\n", cfg[key_data].c_str());
        std::printf("├─────────────────────────────────────────────┤\n");
        std::printf("│ E) Edit    D) Delete/Clear    Q) Back       │\n");
        std::printf("└─────────────────────────────────────────────┘\n");
        std::printf("\nChoice: ");
        std::fflush(stdout);

        char action;
        if (std::scanf(" %c", &action) != 1) continue;
        flush_stdin();

        if (action == 'q' || action == 'Q') continue;

        if (action == 'd' || action == 'D') {
            // Delete/Clear preset
            cfg[key_name] = "Preset " + pi;
            cfg[key_fmt] = "hex";
            cfg[key_data] = "";
            cfg[key_canid] = "0x123";
            std::printf("\n✓ Preset %d cleared to defaults.\n", idx);
            sleep(1);
            continue;
        }

        if (action != 'e' && action != 'E') continue;

        // Edit mode
        clear_screen();
        std::printf("\n");
        std::printf("┌─────────────────────────────────────────────┐\n");
        std::printf("│           Editing Preset %-2d                 │\n", idx);
        std::printf("│  (Press Enter to keep current, - to clear)  │\n");
        std::printf("└─────────────────────────────────────────────┘\n\n");

        std::printf("Name [%s]: ", cfg[key_name].c_str());
        std::fflush(stdout);
        std::string line = read_line();
        if (line == "-") {
            cfg[key_name] = "Preset " + pi;
        } else if (!line.empty()) {
            cfg[key_name] = line;
        }

        std::printf("Format (hex/text) [%s]: ", cfg[key_fmt].c_str());
        std::fflush(stdout);
        line = read_line();
        if (line == "-") {
            cfg[key_fmt] = "hex";
        } else if (!line.empty()) {
            cfg[key_fmt] = line;
        }

        if (itype == InterfaceType::CAN) {
            std::printf("CAN ID (hex, e.g. 0x123) [%s]: ", cfg[key_canid].c_str());
            std::fflush(stdout);
            line = read_line();
            if (line == "-") {
                cfg[key_canid] = "0x123";
            } else if (!line.empty()) {
                cfg[key_canid] = line;
            }
        }

        std::printf("Data (%s) [%s]: ", cfg[key_fmt].c_str(), cfg[key_data].c_str());
        std::fflush(stdout);
        line = read_line();
        if (line == "-") {
            cfg[key_data] = "";
        } else if (!line.empty()) {
            cfg[key_data] = line;
        }

        std::printf("\n✓ Preset %d updated.\n", idx);
        sleep(1);
    }
}

bool show_settings_menu(Config& cfg, InterfaceType& itype,
                        const std::string& cfg_path, bool& append_crlf)
{
    bool need_reconnect = false;

    while (true) {
        clear_screen();
        std::printf("\n");
        std::printf("┌─────────────────────────────────────────────────────────────────┐\n");
        std::printf("│                    ADAMCOM Settings Menu                        │\n");
        std::printf("├─────────────────────────────────────────────────────────────────┤\n");
        std::printf("│ T - Interface type: %-42s │\n", 
                    itype == InterfaceType::SERIAL ? "SERIAL" : "CAN");
        std::printf("├─────────────────────────────────────────────────────────────────┤\n");

        if (itype == InterfaceType::SERIAL) {
            std::printf("│ Serial Configuration                                            │\n");
            std::printf("├─────────────────────────────────────────────────────────────────┤\n");
            std::printf("│ A - Device        : %-42s │\n", cfg["device"].c_str());
            std::printf("│ B - Baud rate     : %-42s │\n", cfg["baud"].c_str());
            std::printf("│ C - Data bits     : %-42s │\n", cfg["databits"].c_str());
            std::printf("│ D - Parity        : %-42s │\n", cfg["parity"].c_str());
            std::printf("│ E - Stop bits     : %-42s │\n", cfg["stop"].c_str());
            std::printf("│ F - Flow control  : %-42s │\n", cfg["flow"].c_str());
        } else {
            std::printf("│ CAN Configuration                                               │\n");
            std::printf("├─────────────────────────────────────────────────────────────────┤\n");
            std::printf("│ A - Interface     : %-42s │\n", cfg["can_interface"].c_str());
            std::printf("│ B - Bitrate       : %-42s │\n", cfg["can_bitrate"].c_str());
            std::printf("│ C - TX CAN ID     : %-42s │\n", cfg["can_id"].c_str());
            std::printf("│ D - Filter        : %-42s │\n", cfg["can_filter"].c_str());
        }

        std::printf("├─────────────────────────────────────────────────────────────────┤\n");
        std::printf("│ Common Settings                                                 │\n");
        std::printf("├─────────────────────────────────────────────────────────────────┤\n");
        std::printf("│ M - Mode          : %-42s │\n", cfg["mode"].c_str());
        std::printf("│ L - CRLF          : %-42s │\n", append_crlf ? "yes" : "no");
        std::printf("├─────────────────────────────────────────────────────────────────┤\n");
        std::printf("│ Presets (Alt+1-9,0 to send, /p N to use)                        │\n");
        std::printf("├─────────────────────────────────────────────────────────────────┤\n");

        // Show all 10 presets with repeat status
        for (int i = 1; i <= 10; ++i) {
            std::string pi = std::to_string(i);
            std::string name = cfg["preset" + pi + "_name"];
            std::string data = cfg["preset" + pi + "_data"];
            bool repeating = g_preset_repeats[static_cast<size_t>(i - 1)].enabled;
            int ms = g_preset_repeats[static_cast<size_t>(i - 1)].interval_ms;

            if (name.empty()) name = "(empty)";
            if (name.length() > 10) name = name.substr(0, 10);
            if (data.length() > 25) data = data.substr(0, 22) + "...";

            std::string status = repeating ? ("R:" + std::to_string(ms) + "ms") : "";
            std::printf("│ %2d) %-10s %-25s %12s │\n", i, name.c_str(), data.c_str(), status.c_str());
        }

        std::printf("├─────────────────────────────────────────────────────────────────┤\n");
        std::printf("│ P - Edit presets   H - Show manual   S - Save   Q - Exit        │\n");
        std::printf("├─────────────────────────────────────────────────────────────────┤\n");
        std::printf("│ COMMANDS: /p N -r start repeat | /p N -nr stop | /rs status     │\n");
        std::printf("│          /ra stop all | /hex XX | /can ID XX | /help            │\n");
        std::printf("└─────────────────────────────────────────────────────────────────┘\n");
        std::printf("\nSelect option: ");
        std::fflush(stdout);

        char choice;
        if (std::scanf(" %c", &choice) != 1) continue;
        flush_stdin();
        choice = static_cast<char>(std::toupper(static_cast<unsigned char>(choice)));

        char buffer[256];

        switch (choice) {
            case 'T':
                if (itype == InterfaceType::SERIAL) {
                    itype = InterfaceType::CAN;
                    std::printf("Switched to CAN mode\n");
                } else {
                    itype = InterfaceType::SERIAL;
                    std::printf("Switched to SERIAL mode\n");
                }
                need_reconnect = true;
                sleep(1);
                break;

            case 'A':
                if (itype == InterfaceType::SERIAL) {
                    std::printf("Enter device path (e.g. /dev/ttyUSB0): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) {
                        cfg["device"] = buffer;
                        need_reconnect = true;
                    }
                    flush_stdin();
                } else {
                    std::printf("Enter CAN interface (e.g. can0, vcan0): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) {
                        cfg["can_interface"] = buffer;
                        need_reconnect = true;
                    }
                    flush_stdin();
                }
                break;

            case 'B':
                if (itype == InterfaceType::SERIAL) {
                    std::printf("Enter baud rate (e.g. 9600, 115200): ");
                } else {
                    std::printf("Enter CAN bitrate (125000/250000/500000/1000000): ");
                }
                std::fflush(stdout);
                if (std::scanf("%255s", buffer) == 1) {
                    if (itype == InterfaceType::SERIAL) {
                        cfg["baud"] = buffer;
                    } else {
                        cfg["can_bitrate"] = buffer;
                    }
                    need_reconnect = true;
                }
                flush_stdin();
                break;

            case 'C':
                if (itype == InterfaceType::SERIAL) {
                    std::printf("Enter data bits (5-8): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) cfg["databits"] = buffer;
                } else {
                    std::printf("Enter CAN ID (hex, e.g. 0x123): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) cfg["can_id"] = buffer;
                }
                flush_stdin();
                break;

            case 'D':
                if (itype == InterfaceType::SERIAL) {
                    std::printf("Enter parity (N/E/O): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) cfg["parity"] = buffer;
                } else {
                    std::printf("Enter filter (id:mask in hex, or 'none'): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) cfg["can_filter"] = buffer;
                }
                flush_stdin();
                break;

            case 'E':
                if (itype == InterfaceType::SERIAL) {
                    std::printf("Enter stop bits (1/2): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) cfg["stop"] = buffer;
                    flush_stdin();
                }
                break;

            case 'F':
                if (itype == InterfaceType::SERIAL) {
                    std::printf("Enter flow control (none/hardware/software): ");
                    std::fflush(stdout);
                    if (std::scanf("%255s", buffer) == 1) cfg["flow"] = buffer;
                    flush_stdin();
                }
                break;

            case 'M':
                std::printf("Enter mode (normal/hex): ");
                std::fflush(stdout);
                if (std::scanf("%255s", buffer) == 1) cfg["mode"] = buffer;
                flush_stdin();
                break;

            case 'L':
                append_crlf = !append_crlf;
                cfg["crlf"] = append_crlf ? "yes" : "no";
                std::printf("CRLF is now %s\n", append_crlf ? "ON" : "OFF");
                sleep(1);
                break;

            case 'P':
                show_presets_menu(cfg, itype);
                break;

            case 'H':
                show_manual();
                break;

            case 'S':
                cfg["crlf"] = append_crlf ? "yes" : "no";
                if (write_profile(cfg_path, cfg)) {
                    std::printf("\n✓ Settings saved to %s\n", cfg_path.c_str());
                    if (need_reconnect) {
                        std::printf("  Reconnecting with new settings...\n");
                    }
                } else {
                    std::printf("\n✗ Failed to save settings!\n");
                }
                sleep(1);
                return need_reconnect;

            case 'Q':
                return need_reconnect;

            default:
                break;
        }
    }
}

} // namespace adamcom
