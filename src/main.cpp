/**
 * @file main.cpp
 * @brief ADAMCOM main entry point and event loop
 */

#include "adamcom.hpp"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>

#include <readline/readline.h>
#include <readline/history.h>

#include <iostream>
#include <functional>
#include <chrono>
#include <thread>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <sstream>

#include <linux/can.h>

using namespace adamcom;

// ============================================================================
// Global State
// ============================================================================

static volatile sig_atomic_t g_keep_running = 1;
static volatile sig_atomic_t g_show_menu = 0;

static std::function<void(char*)> g_line_handler;
static std::string* g_dynamic_prompt = nullptr;
static Config* g_cfg = nullptr;
static bool* g_append_crlf = nullptr;
static int* g_fd = nullptr;
static InterfaceType* g_itype = nullptr;

// ============================================================================
// Signal Handlers
// ============================================================================

extern "C" void sigint_handler(int)
{
    g_keep_running = 0;
}

// ============================================================================
// Readline Callbacks
// ============================================================================

// Forward declarations for helper functions used in pre_input_hook
static std::string to_lower(std::string s);
static std::string extract_hex_bytes_only(const std::string& line, std::string* error = nullptr);

extern "C" void rl_trampoline(char* line)
{
    if (g_line_handler) {
        g_line_handler(line);
    }
}

extern "C" int startup_hook()
{
    if (g_dynamic_prompt) {
        rl_set_prompt(g_dynamic_prompt->c_str());
    }
    return 0;
}

extern "C" int pre_input_hook()
{
    if (!g_dynamic_prompt || !g_cfg || !g_append_crlf) {
        return 0;
    }

    const char* line = rl_line_buffer;
    std::string new_prompt;

    if ((*g_cfg)["mode"] == "hex") {
        // HEX mode: Extract only hex data portion, excluding flags
        std::string data_only = extract_hex_bytes_only(line);
        std::string hex;
        for (char c : data_only) {
            if (!std::isspace(static_cast<unsigned char>(c))) {
                hex.push_back(c);
            }
        }
        size_t bytes = hex.size() / 2;
        new_prompt = "[" + std::to_string(bytes) + "b] > ";
    } else {
        // TEXT mode: Count all characters (no flag stripping)
        size_t bytes = std::strlen(line);
        if (*g_append_crlf) bytes += 2;
        new_prompt = "[" + std::to_string(bytes) + "b] > ";
    }

    *g_dynamic_prompt = new_prompt;
    rl_set_prompt(g_dynamic_prompt->c_str());
    rl_redisplay();
    return 0;
}

extern "C" int ctrl_t_handler(int, int)
{
    g_show_menu = 1;
    return 0;
}

// Alt+1-9,0 handlers for presets 1-10
static int alt_preset_handler(int preset_num)
{
    if (!g_fd || !g_cfg || !g_itype || !g_append_crlf) {
        return 0;
    }
    
    bool ok = send_preset(*g_fd, *g_cfg, *g_itype, preset_num, *g_append_crlf);
    std::string pname = (*g_cfg)["preset" + std::to_string(preset_num) + "_name"];
    std::string msg = ok ? 
        ("TX[Preset " + std::to_string(preset_num) + " (" + pname + ")]") :
        ("TX FAILED[Preset " + std::to_string(preset_num) + "]");
    print_message_above(msg);
    return 0;
}

extern "C" int alt_1_handler(int, int) { return alt_preset_handler(1); }
extern "C" int alt_2_handler(int, int) { return alt_preset_handler(2); }
extern "C" int alt_3_handler(int, int) { return alt_preset_handler(3); }
extern "C" int alt_4_handler(int, int) { return alt_preset_handler(4); }
extern "C" int alt_5_handler(int, int) { return alt_preset_handler(5); }
extern "C" int alt_6_handler(int, int) { return alt_preset_handler(6); }
extern "C" int alt_7_handler(int, int) { return alt_preset_handler(7); }
extern "C" int alt_8_handler(int, int) { return alt_preset_handler(8); }
extern "C" int alt_9_handler(int, int) { return alt_preset_handler(9); }
extern "C" int alt_0_handler(int, int) { return alt_preset_handler(10); }

// ============================================================================
// Helper Functions
// ============================================================================

static std::string trim(const std::string& s)
{
    auto start = std::find_if_not(s.begin(), s.end(),
        [](unsigned char c) { return std::isspace(c); });
    auto end = std::find_if_not(s.rbegin(), s.rend(),
        [](unsigned char c) { return std::isspace(c); }).base();
    return (start < end) ? std::string(start, end) : "";
}

static std::pair<std::string, std::string> split_first(const std::string& s)
{
    auto pos = s.find(' ');
    if (pos == std::string::npos) {
        return {s, ""};
    }
    return {s.substr(0, pos), trim(s.substr(pos + 1))};
}

static std::string to_lower(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

/// Check if a string is a valid hex token (only 0-9, A-F, a-f)
static bool is_valid_hex_token(const std::string& token)
{
    if (token.empty()) return false;
    for (char c : token) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

/// Check if a token is a valid flag: exactly "-r", "-t", or "-id" (case insensitive)
static bool is_valid_flag(const std::string& token, const std::string& expected)
{
    if (token.size() != expected.size()) return false;
    return to_lower(token) == expected;
}

/// Check if a token is a valid CAN ID (0x prefix + hex digits)
static bool is_valid_can_id_token(const std::string& token)
{
    if (token.size() < 3) return false;
    if (token[0] != '0' || (token[1] != 'x' && token[1] != 'X')) return false;
    for (size_t i = 2; i < token.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(token[i]))) {
            return false;
        }
    }
    return true;
}

/// Check if a token is a valid positive integer
static bool is_valid_positive_int(const std::string& token)
{
    if (token.empty()) return false;
    for (char c : token) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

/// Extract only the hex bytes portion from input (HEX MODE ONLY)
/// Excludes flags like -id, -r, -t and validates them strictly
/// Returns empty string and sets error message if invalid input found
static std::string extract_hex_bytes_only(const std::string& line, std::string* error)
{
    // If it starts with /, it's a command - return empty
    if (!line.empty() && line[0] == '/') {
        return "";
    }
    
    std::istringstream iss(line);
    std::string token;
    std::string result;
    bool skip_next = false;
    std::string skip_reason;
    
    while (iss >> token) {
        if (skip_next) {
            // Validate the argument based on what we're expecting
            if (skip_reason == "-id") {
                if (!is_valid_can_id_token(token)) {
                    if (error) *error = "Invalid CAN ID format: " + token + " (expected 0xNNN)";
                    return "";
                }
            } else if (skip_reason == "-t") {
                if (!is_valid_positive_int(token)) {
                    if (error) *error = "Invalid interval: " + token + " (expected positive integer)";
                    return "";
                }
            }
            skip_next = false;
            skip_reason.clear();
            continue;
        }
        
        std::string lower_tok = to_lower(token);
        
        // Check if it's a valid flag that takes an argument
        if (is_valid_flag(token, "-id") || is_valid_flag(token, "-t")) {
            skip_next = true;
            skip_reason = lower_tok;
            continue;
        }
        
        // Check if it's a valid standalone flag
        if (is_valid_flag(token, "-r")) {
            continue;
        }
        
        // Check if it looks like a malformed flag (starts with -)
        if (!token.empty() && token[0] == '-') {
            if (error) *error = "Invalid flag: " + token + " (valid flags: -r, -t MS, -id 0xNNN)";
            return "";
        }
        
        // Must be hex data - validate it
        if (!is_valid_hex_token(token)) {
            if (error) *error = "Invalid hex: " + token + " (only 0-9, A-F allowed)";
            return "";
        }
        
        // Valid hex token
        if (!result.empty()) result += " ";
        result += token;
    }
    
    return result;
}

static void update_prompt_display(const std::string& prompt)
{
    std::printf("\r\033[K");
    rl_set_prompt(prompt.c_str());
    rl_replace_line("", 0);
    rl_forced_update_display();
}

namespace adamcom {

/// Print a message above the current readline input without interrupting typing
void print_message_above(const std::string& msg)
{
    // Save current line content and cursor position
    int saved_point = rl_point;
    char* saved_line = rl_copy_text(0, rl_end);
    
    // Move to beginning of line and clear it
    std::printf("\r\033[K");
    // Print message
    std::printf("%s\n", msg.c_str());
    // Redraw prompt and saved text
    std::printf("%s%s", g_dynamic_prompt ? g_dynamic_prompt->c_str() : "> ", saved_line);
    // Move cursor back to correct position within the line
    int prompt_len = g_dynamic_prompt ? static_cast<int>(g_dynamic_prompt->length()) : 2;
    int total_len = prompt_len + static_cast<int>(std::strlen(saved_line));
    int target_pos = prompt_len + saved_point;
    // Move cursor back from end to target position
    if (total_len > target_pos) {
        std::printf("\033[%dD", total_len - target_pos);
    }
    std::fflush(stdout);
    
    // Sync readline state
    rl_replace_line(saved_line, 0);
    rl_point = saved_point;
    
    std::free(saved_line);
}

} // namespace adamcom

// ============================================================================
// Default Configuration
// ============================================================================

static Config get_default_config()
{
    Config cfg = {
        {"type", "serial"},
        {"device", "/dev/ttyUSB0"},
        {"baud", "115200"},
        {"databits", "8"},
        {"parity", "N"},
        {"stop", "1"},
        {"flow", "none"},
        {"mode", "normal"},
        {"crlf", "yes"},
        {"can_interface", "can0"},
        {"can_bitrate", "1000000"},
        {"can_id", "0x123"},
        {"can_filter", "none"},
        {"repeat_enabled", "no"},
        {"repeat_interval", "1000"},
        {"repeat_preset", "1"}
    };

    // Initialize 10 presets
    for (int i = 1; i <= 10; ++i) {
        std::string pi = std::to_string(i);
        cfg["preset" + pi + "_name"] = "Preset " + pi;
        cfg["preset" + pi + "_format"] = "hex";
        cfg["preset" + pi + "_data"] = "";
        cfg["preset" + pi + "_can_id"] = "0x123";
    }

    return cfg;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[])
{
    // Setup signal handler
    struct sigaction sa{};
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);

    // Configuration paths
    const char* home = std::getenv("HOME");
    std::string home_dir = home ? home : ".";
    std::string cfg_path = home_dir + "/.adamcomrc";
    std::string hist_path = home_dir + "/.adamcom_history";

    // Load or create configuration
    Config cfg;
    if (!file_exists(cfg_path)) {
        cfg = get_default_config();
        write_profile(cfg_path, cfg);
    } else {
        cfg = read_profile(cfg_path);
    }

    // Ensure all required keys exist
    Config defaults = get_default_config();
    bool needs_save = false;
    for (const auto& [key, value] : defaults) {
        if (cfg.find(key) == cfg.end()) {
            cfg[key] = value;
            needs_save = true;
        }
    }
    if (needs_save) {
        write_profile(cfg_path, cfg);
    }

    // Parse state from config
    bool append_crlf = (cfg["crlf"] == "yes");
    InterfaceType itype = (cfg["type"] == "can") ? InterfaceType::CAN : InterfaceType::SERIAL;
    int start_preset_index = 0;
    int start_repeat_preset = 0;
    int start_repeat_ms = 0;

    // Parse command line arguments
    bool cli_changed = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto require_arg = [&](const char* key) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return false;
            }
            cfg[key] = argv[++i];
            cli_changed = true;
            return true;
        };

        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
        else if (arg == "-d" || arg == "-D" || arg == "--device") {
            if (!require_arg("device")) return 1;
            cfg["type"] = "serial";
            itype = InterfaceType::SERIAL;
        }
        else if (arg == "-b" || arg == "--baud") {
            if (!require_arg("baud")) return 1;
        }
        else if (arg == "-i" || arg == "--databits") {
            if (!require_arg("databits")) return 1;
        }
        else if (arg == "-p" || arg == "--parity") {
            if (!require_arg("parity")) return 1;
        }
        else if (arg == "-s" || arg == "--stop") {
            if (!require_arg("stop")) return 1;
        }
        else if (arg == "-f" || arg == "--flow") {
            if (!require_arg("flow")) return 1;
        }
        else if (arg == "-c" || arg == "--can") {
            if (!require_arg("can_interface")) return 1;
            cfg["type"] = "can";
            itype = InterfaceType::CAN;
        }
        else if (arg == "--canbitrate") {
            if (!require_arg("can_bitrate")) return 1;
        }
        else if (arg == "--canid") {
            if (!require_arg("can_id")) return 1;
        }
        else if (arg == "--filter") {
            if (!require_arg("can_filter")) return 1;
        }
        else if (arg == "--hex") {
            cfg["mode"] = "hex";
            cli_changed = true;
        }
        else if (arg == "--normal") {
            cfg["mode"] = "normal";
            cli_changed = true;
        }
        else if (arg == "--crlf") {
            append_crlf = true;
            cfg["crlf"] = "yes";
            cli_changed = true;
        }
        else if (arg == "--no-crlf") {
            append_crlf = false;
            cfg["crlf"] = "no";
            cli_changed = true;
        }
        else if (arg == "--preset") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            try {
                start_preset_index = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "Invalid preset index\n";
                return 1;
            }
        }
        else if (arg == "--repeat") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            std::string val = argv[++i];
            auto comma = val.find(',');
            if (comma == std::string::npos) {
                std::cerr << "--repeat requires format: N,MS (e.g., 1,1000)\n";
                return 1;
            }
            try {
                start_repeat_preset = std::stoi(val.substr(0, comma));
                start_repeat_ms = std::stoi(val.substr(comma + 1));
            } catch (...) {
                std::cerr << "Invalid --repeat format\n";
                return 1;
            }
        }
        else {
            std::cerr << "Unknown option: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (cli_changed) {
        write_profile(cfg_path, cfg);
    }

    // Open device
    int fd = -1;

    if (itype == InterfaceType::SERIAL) {
        fd = open(cfg["device"].c_str(), O_RDWR | O_NOCTTY);
        if (fd < 0) {
            std::perror(cfg["device"].c_str());
            return 1;
        }

        // Configure serial port
        struct termios tty{};
        if (tcgetattr(fd, &tty) != 0) {
            std::perror("tcgetattr");
            close(fd);
            return 1;
        }

        // Baud rate
        try {
            speed_t speed = static_cast<speed_t>(get_baud_speed_t(cfg["baud"]));
            cfsetispeed(&tty, speed);
            cfsetospeed(&tty, speed);
        } catch (const std::exception& e) {
            std::cerr << "Invalid baud rate: " << e.what() << "\n";
            close(fd);
            return 1;
        }

        // Data bits
        int databits = 8;
        try { databits = std::stoi(cfg["databits"]); } catch (...) {}
        tty.c_cflag &= ~CSIZE;
        switch (databits) {
            case 5: tty.c_cflag |= CS5; break;
            case 6: tty.c_cflag |= CS6; break;
            case 7: tty.c_cflag |= CS7; break;
            default: tty.c_cflag |= CS8; break;
        }

        // Parity
        char parity = 'N';
        if (!cfg["parity"].empty()) {
            parity = static_cast<char>(std::toupper(static_cast<unsigned char>(cfg["parity"][0])));
        }
        tty.c_cflag &= ~(PARENB | PARODD);
        if (parity == 'E') {
            tty.c_cflag |= PARENB;
        } else if (parity == 'O') {
            tty.c_cflag |= PARENB | PARODD;
        }

        // Stop bits
        int stopbits = 1;
        try { stopbits = std::stoi(cfg["stop"]); } catch (...) {}
        if (stopbits == 2) {
            tty.c_cflag |= CSTOPB;
        } else {
            tty.c_cflag &= ~CSTOPB;
        }

        // Flow control
        tty.c_cflag &= ~CRTSCTS;
        tty.c_iflag &= ~(IXON | IXOFF | IXANY);
        if (cfg["flow"] == "hardware") {
            tty.c_cflag |= CRTSCTS;
        } else if (cfg["flow"] == "software") {
            tty.c_iflag |= IXON | IXOFF | IXANY;
        }

        // Raw mode
        tty.c_cflag |= CREAD | CLOCAL;
        tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
        tty.c_iflag &= ~(INLCR | ICRNL | IGNCR);
        tty.c_oflag &= ~OPOST;

        if (tcsetattr(fd, TCSANOW, &tty) != 0) {
            std::perror("tcsetattr");
            close(fd);
            return 1;
        }

        fcntl(fd, F_SETFL, FNDELAY);
        std::cout << "Connected to " << cfg["device"] << " @ " << cfg["baud"]
                  << " baud (Ctrl-T: Menu, Ctrl-C: Quit)\n";

    } else {
        // CAN mode
        if (configure_can_interface(cfg["can_interface"], cfg["can_bitrate"]) < 0) {
            std::cerr << "Failed to configure CAN. Try:\n"
                      << "  sudo ip link set " << cfg["can_interface"]
                      << " type can bitrate " << cfg["can_bitrate"] << "\n"
                      << "  sudo ip link set " << cfg["can_interface"] << " up\n";
            return 1;
        }

        std::string filter = (cfg["can_filter"] == "none") ? "" : cfg["can_filter"];
        fd = setup_can(cfg["can_interface"], filter);
        if (fd < 0) {
            return 1;
        }

        std::cout << "Connected to " << cfg["can_interface"] << " @ "
                  << cfg["can_bitrate"] << " bps (Ctrl-T: Menu, Ctrl-C: Quit)\n";
    }

    // Handle one-shot preset
    if (start_preset_index > 0) {
        bool ok = send_preset(fd, cfg, itype, start_preset_index, append_crlf);
        if (!ok) {
            std::cerr << "Failed to send preset " << start_preset_index << "\n";
        }
        close(fd);
        return ok ? 0 : 1;
    }

    // Handle CLI repeat option (legacy support - sets up preset 1)
    if (start_repeat_preset > 0 && start_repeat_ms > 0) {
        size_t idx = static_cast<size_t>(start_repeat_preset - 1);
        if (idx < 10) {
            g_preset_repeats[idx].enabled = true;
            g_preset_repeats[idx].interval_ms = start_repeat_ms;
            g_preset_repeats[idx].next_fire = std::chrono::steady_clock::now() + 
                std::chrono::milliseconds(start_repeat_ms);
        }
    }

    // Clock type alias
    using Clock = std::chrono::steady_clock;

    // Setup readline
    std::string dynamic_prompt = "> ";
    g_dynamic_prompt = &dynamic_prompt;
    g_cfg = &cfg;
    g_append_crlf = &append_crlf;
    g_fd = &fd;
    g_itype = &itype;

    rl_callback_handler_install(dynamic_prompt.c_str(), rl_trampoline);
    read_history(hist_path.c_str());
    rl_startup_hook = startup_hook;
    rl_pre_input_hook = pre_input_hook;
    rl_bind_key(20, ctrl_t_handler);  // Ctrl-T
    
    // Bind Alt+1-9,0 for presets (Meta key = ESC prefix in readline)
    rl_bind_keyseq("\0331", alt_1_handler);
    rl_bind_keyseq("\0332", alt_2_handler);
    rl_bind_keyseq("\0333", alt_3_handler);
    rl_bind_keyseq("\0334", alt_4_handler);
    rl_bind_keyseq("\0335", alt_5_handler);
    rl_bind_keyseq("\0336", alt_6_handler);
    rl_bind_keyseq("\0337", alt_7_handler);
    rl_bind_keyseq("\0338", alt_8_handler);
    rl_bind_keyseq("\0339", alt_9_handler);
    rl_bind_keyseq("\0330", alt_0_handler);
    
    rl_forced_update_display();

    // Line handler callback
    g_line_handler = [&](char* buf) {
        if (!buf) {
            g_keep_running = 0;
            return;
        }

        std::string line(buf);
        std::free(buf);

        if (line.empty()) return;

        add_history(line.c_str());
        write_history(hist_path.c_str());
        std::printf("\r\033[K");

        // Handle slash commands
        if (line[0] == '/') {
            std::string body = trim(line.substr(1));
            auto [cmd, arg] = split_first(body);
            cmd = to_lower(cmd);

            if (cmd == "help" || cmd == "h") {
                std::printf("\r\n"
                    "Commands:\n"
                    "  /p N              Send preset N (1-10) once\n"
                    "  /p N -r           Start repeating preset N (default 1000ms)\n"
                    "  /p N -r -t MS     Start repeating preset N with MS interval\n"
                    "  /p N -nr          Stop repeating preset N\n"
                    "  /rs               Show repeat status for all repeats\n"
                    "  /rs stop          Stop inline repeat\n"
                    "  /ra               Stop all repeats (presets + inline)\n"
                    "  /hex XX XX        Send raw hex bytes\n"
                    "  /can ID XX XX     Send CAN frame (ID + data)\n"
                    "  /clear            Clear screen\n"
                    "  /device PATH      Change device path\n"
                    "  /baud RATE        Change baud rate\n"
                    "  /mode normal|hex  Set display mode\n"
                    "  /crlf on|off      Toggle CRLF append\n"
                    "  /status           Show current settings\n"
                    "  /menu             Open settings menu\n"
                    "  /help             Show this help\n"
                    "\n"
                    "Text Mode Repeat (use /rpt to avoid conflict with text):\n"
                    "  /rpt MS text          Repeat 'text' every MS milliseconds\n"
                    "  /rpt 500 hello        Example: repeat 'hello' every 500ms\n"
                    "  /rpt 100 -r test      Send literal '-r test' every 100ms\n"
                    "\n"
                    "Hex Mode Inline Repeat (flags parsed from input):\n"
                    "  FF FF FF -r           Repeat at 1000ms\n"
                    "  FF FF FF -r -t 100    Repeat at 100ms interval\n"
                    "  AA BB -id 0x03 -r     CAN: repeat to ID 0x03\n\n");
            }
            else if (cmd == "menu") {
                g_show_menu = 1;
            }
            else if (cmd == "clear") {
                clear_screen();
            }
            else if (cmd == "status") {
                std::printf("\r\n");
                if (itype == InterfaceType::SERIAL) {
                    std::printf("  Device: %s @ %s baud\n", cfg["device"].c_str(), cfg["baud"].c_str());
                } else {
                    std::printf("  CAN: %s @ %s bps (ID: %s)\n", cfg["can_interface"].c_str(), 
                                cfg["can_bitrate"].c_str(), cfg["can_id"].c_str());
                }
                std::printf("  Mode: %s, CRLF: %s\n", cfg["mode"].c_str(), append_crlf ? "on" : "off");
                // Show active repeats
                bool any_repeat = false;
                
                // Show inline repeat
                if (g_inline_repeat.enabled) {
                    if (!any_repeat) {
                        std::printf("  Repeating:\n");
                        any_repeat = true;
                    }
                    if (g_inline_repeat.is_hex) {
                        std::printf("    Inline: %zu bytes, every %dms\n",
                                    g_inline_repeat.data.size(),
                                    g_inline_repeat.interval_ms);
                    } else {
                        std::printf("    Inline: \"%s\", every %dms\n",
                                    g_inline_repeat.text_data.c_str(),
                                    g_inline_repeat.interval_ms);
                    }
                }
                
                // Show preset repeats
                for (int i = 0; i < 10; ++i) {
                    if (g_preset_repeats[static_cast<size_t>(i)].enabled) {
                        if (!any_repeat) {
                            std::printf("  Repeating:\n");
                            any_repeat = true;
                        }
                        std::printf("    Preset %d: every %dms\n", i + 1, 
                                    g_preset_repeats[static_cast<size_t>(i)].interval_ms);
                    }
                }
                std::printf("\n");
            }
            else if (cmd == "rs") {
                // Repeat status or stop inline repeat
                if (!arg.empty() && to_lower(arg) == "stop") {
                    // /rs stop - stop inline repeat
                    if (g_inline_repeat.enabled) {
                        g_inline_repeat.enabled = false;
                        std::printf("\r\nInline repeat stopped.\n");
                    } else {
                        std::printf("\r\nNo inline repeat is active.\n");
                    }
                } else {
                    // /rs - show repeat status
                    std::printf("\r\nRepeat Status:\n");
                    bool any = false;
                    
                    // Show inline repeat
                    if (g_inline_repeat.enabled) {
                        if (g_inline_repeat.is_can) {
                            if (g_inline_repeat.is_hex) {
                                std::printf("  Inline: CAN ID 0x%03X, %zu bytes, every %dms\n",
                                            g_inline_repeat.can_id,
                                            g_inline_repeat.data.size(),
                                            g_inline_repeat.interval_ms);
                            } else {
                                std::printf("  Inline: CAN ID 0x%03X, \"%s\", every %dms\n",
                                            g_inline_repeat.can_id,
                                            g_inline_repeat.text_data.c_str(),
                                            g_inline_repeat.interval_ms);
                            }
                        } else {
                            if (g_inline_repeat.is_hex) {
                                std::printf("  Inline: Serial, %zu bytes, every %dms\n",
                                            g_inline_repeat.data.size(),
                                            g_inline_repeat.interval_ms);
                            } else {
                                std::printf("  Inline: Serial, \"%s\", every %dms\n",
                                            g_inline_repeat.text_data.c_str(),
                                            g_inline_repeat.interval_ms);
                            }
                        }
                        any = true;
                    }
                    
                    // Show preset repeats
                    for (int i = 0; i < 10; ++i) {
                        if (g_preset_repeats[static_cast<size_t>(i)].enabled) {
                            std::string pname = cfg["preset" + std::to_string(i+1) + "_name"];
                            std::printf("  Preset %d (%s): every %dms\n", i + 1, pname.c_str(),
                                        g_preset_repeats[static_cast<size_t>(i)].interval_ms);
                            any = true;
                        }
                    }
                    if (!any) {
                        std::printf("  No repeats are active.\n");
                    }
                    std::printf("  Use /rs stop to stop inline repeat, /ra to stop all.\n\n");
                }
            }
            else if (cmd == "ra") {
                // Stop all repeats (including inline)
                g_inline_repeat.enabled = false;
                for (int i = 0; i < 10; ++i) {
                    g_preset_repeats[static_cast<size_t>(i)].enabled = false;
                }
                std::printf("\r\nAll repeats stopped.\n");
            }
            else if (cmd == "p") {
                // Parse: /p N | /p N -r | /p N -r -t MS | /p N -nr
                std::vector<std::string> tokens;
                std::istringstream iss(arg);
                std::string tok;
                while (iss >> tok) {
                    tokens.push_back(tok);
                }

                if (tokens.empty()) {
                    std::printf("\r\nUsage: /p N [-r [-t MS]] [-nr]\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }

                int idx = 0;
                try { idx = std::stoi(tokens[0]); } catch (...) {}
                if (idx < 1 || idx > 10) {
                    std::printf("\r\nUsage: /p N (1-10)\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }

                bool start_repeat = false;
                bool stop_repeat = false;
                int custom_interval = -1;

                for (size_t i = 1; i < tokens.size(); ++i) {
                    std::string t = to_lower(tokens[i]);
                    if (t == "-r") {
                        start_repeat = true;
                    } else if (t == "-nr") {
                        stop_repeat = true;
                    } else if (t == "-t" && i + 1 < tokens.size()) {
                        try { custom_interval = std::stoi(tokens[++i]); } catch (...) {}
                    }
                }

                size_t preset_idx = static_cast<size_t>(idx - 1);

                if (stop_repeat) {
                    g_preset_repeats[preset_idx].enabled = false;
                    std::printf("\r\nPreset %d repeat stopped.\n", idx);
                } else if (start_repeat) {
                    g_preset_repeats[preset_idx].enabled = true;
                    if (custom_interval > 0) {
                        g_preset_repeats[preset_idx].interval_ms = custom_interval;
                    }
                    g_preset_repeats[preset_idx].next_fire = Clock::now() + 
                        std::chrono::milliseconds(g_preset_repeats[preset_idx].interval_ms);
                    std::printf("\r\nPreset %d repeating every %dms\n", idx, 
                                g_preset_repeats[preset_idx].interval_ms);
                } else {
                    // Just send once
                    bool ok = send_preset(fd, cfg, itype, idx, append_crlf);
                    std::printf("\r\nPreset %d %s\n", idx, ok ? "sent" : "failed");
                }
            }
            else if (cmd == "hex") {
                std::vector<uint8_t> data;
                if (!parse_hex_bytes(arg, data) || data.empty()) {
                    std::printf("\r\nUsage: /hex XX XX XX ...\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }
                if (itype == InterfaceType::CAN) {
                    uint32_t canid = 0x123;
                    try { canid = std::stoul(cfg["can_id"], nullptr, 16); } catch (...) {}
                    bool ok = send_can_bytes(fd, canid, data);
                    std::printf("\r\n%s\n", ok ? "Sent" : "Failed");
                } else {
                    bool ok = send_serial_bytes(fd, data);
                    std::printf("\r\n%s\n", ok ? "Sent" : "Failed");
                }
            }
            else if (cmd == "can") {
                // /can ID XX XX ...
                auto parts = split_first(arg);
                std::string id_str = parts.first;
                std::string data_str = parts.second;

                uint32_t canid = 0;
                try { canid = std::stoul(id_str, nullptr, 16); } catch (...) {
                    std::printf("\r\nUsage: /can ID XX XX ...\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }

                std::vector<uint8_t> data;
                if (!data_str.empty() && !parse_hex_bytes(data_str, data)) {
                    std::printf("\r\nInvalid hex data\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }

                bool ok = send_can_bytes(fd, canid, data);
                std::printf("\r\n%s\n", ok ? "Sent" : "Failed");
            }
            else if (cmd == "device") {
                if (arg.empty()) {
                    std::printf("\r\nUsage: /device PATH\n");
                } else {
                    cfg["device"] = arg;
                    write_profile(cfg_path, cfg);
                    std::printf("\r\nDevice set to %s (reconnect with Ctrl-T menu)\n", arg.c_str());
                }
            }
            else if (cmd == "baud") {
                if (arg.empty()) {
                    std::printf("\r\nUsage: /baud RATE\n");
                } else {
                    cfg["baud"] = arg;
                    write_profile(cfg_path, cfg);
                    std::printf("\r\nBaud set to %s (reconnect with Ctrl-T menu)\n", arg.c_str());
                }
            }
            else if (cmd == "mode") {
                std::string a = to_lower(arg);
                if (a == "hex" || a == "normal") {
                    cfg["mode"] = a;
                    write_profile(cfg_path, cfg);
                    std::printf("\r\nMode set to %s\n", a.c_str());
                } else {
                    std::printf("\r\nUsage: /mode normal|hex\n");
                }
            }
            else if (cmd == "crlf") {
                std::string a = to_lower(arg);
                if (a == "on") {
                    append_crlf = true;
                    cfg["crlf"] = "yes";
                } else if (a == "off") {
                    append_crlf = false;
                    cfg["crlf"] = "no";
                } else {
                    std::printf("\r\nUsage: /crlf on|off\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }
                write_profile(cfg_path, cfg);
                std::printf("\r\nCRLF is now %s\n", append_crlf ? "ON" : "OFF");
            }
            else if (cmd == "rpt") {
                // /rpt MS text - repeat text every MS milliseconds
                // This is the proper way to repeat in text mode
                if (arg.empty()) {
                    std::printf("\r\nUsage: /rpt MS text\n");
                    std::printf("  Example: /rpt 500 hello world\n");
                    std::printf("  Example: /rpt 100 -r test  (sends literal '-r test')\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }
                
                auto parts = split_first(arg);
                std::string ms_str = parts.first;
                std::string text = parts.second;
                
                if (!is_valid_positive_int(ms_str)) {
                    std::printf("\r\nError: First argument must be interval in milliseconds.\n");
                    std::printf("Usage: /rpt MS text\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }
                
                if (text.empty()) {
                    std::printf("\r\nError: No text to repeat.\n");
                    std::printf("Usage: /rpt MS text\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }
                
                int interval_ms = std::stoi(ms_str);
                if (interval_ms < 10) {
                    std::printf("\r\nError: Interval must be at least 10ms.\n");
                    update_prompt_display(dynamic_prompt);
                    return;
                }
                
                // Setup inline repeat for text mode
                g_inline_repeat.enabled = true;
                g_inline_repeat.is_can = (itype == InterfaceType::CAN);
                g_inline_repeat.is_hex = false;
                g_inline_repeat.text_data = text;
                g_inline_repeat.append_crlf = append_crlf;
                g_inline_repeat.interval_ms = interval_ms;
                g_inline_repeat.next_fire = Clock::now() + 
                    std::chrono::milliseconds(interval_ms);
                
                if (itype == InterfaceType::CAN) {
                    try {
                        g_inline_repeat.can_id = std::stoul(cfg["can_id"], nullptr, 16);
                    } catch (...) {
                        g_inline_repeat.can_id = 0x123;
                    }
                    
                    // CAN text - max 8 chars
                    if (text.size() > 8) text = text.substr(0, 8);
                    g_inline_repeat.text_data = text;
                    
                    // Send first message immediately
                    struct can_frame frame{};
                    frame.can_id = g_inline_repeat.can_id;
                    frame.can_dlc = static_cast<uint8_t>(text.size());
                    std::memcpy(frame.data, text.c_str(), text.size());
                    ssize_t w = write(fd, &frame, sizeof(frame));
                    if (w < 0) {
                        std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                        g_inline_repeat.enabled = false;
                    } else {
                        std::printf("\r\nText repeat started: ID 0x%03X, \"%s\", every %dms\n",
                                    g_inline_repeat.can_id, text.c_str(), interval_ms);
                        std::printf("Use /rs stop to stop, /ra to stop all.\n");
                    }
                } else {
                    // Serial text - send first message immediately
                    std::string msg = text;
                    if (append_crlf) msg += "\r\n";
                    ssize_t w = write(fd, msg.c_str(), msg.size());
                    if (w < 0) {
                        std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                        g_inline_repeat.enabled = false;
                    } else {
                        std::printf("\r\nText repeat started: \"%s\", every %dms\n",
                                    text.c_str(), interval_ms);
                        std::printf("Use /rs stop to stop, /ra to stop all.\n");
                    }
                }
            }
            else if (cmd == "r") {
                std::string a = to_lower(arg);
                std::printf("\r\nNote: Use /p N -r to start repeat, /p N -nr to stop.\n");
                std::printf("      For text repeat, use: /rpt MS text\n");
                std::printf("      Use /rs for status, /ra to stop all.\n");
            }
            else if (cmd == "ri" || cmd == "rp") {
                std::printf("\r\nNote: Use /p N -r -t MS for interval, /rs for status.\n");
                std::printf("      For text repeat, use: /rpt MS text\n");
            }
            else {
                std::printf("\r\nUnknown command. Type /help\n");
            }

            update_prompt_display(dynamic_prompt);
            return;
        }

        // =================================================================
        // TEXT MODE: Send raw text, no flag parsing
        // Use /rpt MS text for text repeat
        // =================================================================
        if (cfg["mode"] != "hex") {
            std::string text = line;  // Send exactly what user typed
            
            if (itype == InterfaceType::CAN) {
                // CAN text mode - max 8 chars
                if (text.size() > 8) {
                    std::printf("\r\nWarning: CAN data truncated to 8 bytes.\n");
                    text = text.substr(0, 8);
                }
                
                struct can_frame frame{};
                try {
                    frame.can_id = std::stoul(cfg["can_id"], nullptr, 16);
                } catch (...) {
                    frame.can_id = 0x123;
                }
                
                frame.can_dlc = static_cast<uint8_t>(text.size());
                std::memcpy(frame.data, text.c_str(), text.size());
                
                ssize_t w = write(fd, &frame, sizeof(frame));
                if (w < 0) {
                    std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                } else {
                    std::printf("\r\nTX[ID:0x%03X DLC:%d]\n", frame.can_id, frame.can_dlc);
                }
            } else {
                // Serial text mode
                std::string msg = text;
                if (append_crlf) msg += "\r\n";
                ssize_t w = write(fd, msg.c_str(), msg.size());
                if (w < 0) {
                    std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                } else {
                    std::printf("\r\nTX[%zu bytes]\n", text.size());
                }
            }
            
            update_prompt_display("> ");
            return;
        }

        // =================================================================
        // HEX MODE: Parse inline flags with strict validation
        // Format: "FF FF FF -id 0x03 -r -t 100"
        // =================================================================
        std::string hex_part;
        uint32_t inline_can_id = 0;
        bool has_inline_id = false;
        bool start_inline_repeat = false;
        int inline_interval_ms = 1000;
        std::string parse_error;
        
        {
            std::istringstream iss(line);
            std::string token;
            std::vector<std::string> hex_tokens;
            bool skip_next = false;
            std::string skip_reason;
            
            while (iss >> token) {
                if (skip_next) {
                    // Validate argument based on what we're expecting
                    if (skip_reason == "-id") {
                        if (!is_valid_can_id_token(token)) {
                            parse_error = "Invalid CAN ID: " + token + " (expected 0xNNN)";
                            break;
                        }
                        try {
                            inline_can_id = std::stoul(token, nullptr, 16);
                            has_inline_id = true;
                        } catch (...) {
                            parse_error = "Invalid CAN ID: " + token;
                            break;
                        }
                    } else if (skip_reason == "-t") {
                        if (!is_valid_positive_int(token)) {
                            parse_error = "Invalid interval: " + token + " (expected positive integer)";
                            break;
                        }
                        inline_interval_ms = std::stoi(token);
                        if (inline_interval_ms < 10) {
                            parse_error = "Interval must be at least 10ms";
                            break;
                        }
                    }
                    skip_next = false;
                    skip_reason.clear();
                    continue;
                }
                
                // Check for valid flags (exact match only)
                if (is_valid_flag(token, "-id")) {
                    skip_next = true;
                    skip_reason = "-id";
                    continue;
                }
                if (is_valid_flag(token, "-t")) {
                    skip_next = true;
                    skip_reason = "-t";
                    continue;
                }
                if (is_valid_flag(token, "-r")) {
                    start_inline_repeat = true;
                    continue;
                }
                
                // Check if it looks like a malformed flag (starts with -)
                if (!token.empty() && token[0] == '-') {
                    parse_error = "Invalid flag: " + token + " (valid: -r, -t MS, -id 0xNNN)";
                    break;
                }
                
                // Must be hex data - validate strictly
                if (!is_valid_hex_token(token)) {
                    parse_error = "Invalid hex byte: " + token + " (only 0-9, A-F allowed)";
                    break;
                }
                
                hex_tokens.push_back(token);
            }
            
            // Check for missing argument
            if (skip_next && parse_error.empty()) {
                parse_error = "Missing argument for " + skip_reason;
            }
            
            // Rebuild hex string from valid hex tokens
            for (size_t i = 0; i < hex_tokens.size(); ++i) {
                if (i > 0) hex_part += " ";
                hex_part += hex_tokens[i];
            }
        }
        
        // Handle parse error
        if (!parse_error.empty()) {
            std::printf("\r\nError: %s\n", parse_error.c_str());
            update_prompt_display(dynamic_prompt);
            return;
        }

        // Parse and validate hex data
        std::vector<uint8_t> data;
        if (!hex_part.empty() && !parse_hex_bytes(hex_part, data)) {
            std::printf("\r\nError: Invalid hex format\n");
            update_prompt_display(dynamic_prompt);
            return;
        }
        
        if (data.empty()) {
            std::printf("\r\nError: No data to send\n");
            update_prompt_display(dynamic_prompt);
            return;
        }

        // Send data (HEX mode)
        if (itype == InterfaceType::CAN) {
            if (data.size() > 8) {
                std::printf("\r\nError: CAN data max 8 bytes (got %zu)\n", data.size());
                update_prompt_display(dynamic_prompt);
                return;
            }
            
            struct can_frame frame{};
            if (has_inline_id) {
                frame.can_id = inline_can_id;
            } else {
                try {
                    frame.can_id = std::stoul(cfg["can_id"], nullptr, 16);
                } catch (...) {
                    frame.can_id = 0x123;
                }
            }
            
            // Handle inline repeat
            if (start_inline_repeat) {
                g_inline_repeat.enabled = true;
                g_inline_repeat.is_can = true;
                g_inline_repeat.is_hex = true;
                g_inline_repeat.can_id = frame.can_id;
                g_inline_repeat.data = data;
                g_inline_repeat.interval_ms = inline_interval_ms;
                g_inline_repeat.next_fire = Clock::now() + 
                    std::chrono::milliseconds(inline_interval_ms);
                
                // Send first message immediately
                frame.can_dlc = static_cast<uint8_t>(data.size());
                std::memcpy(frame.data, data.data(), data.size());
                ssize_t w = write(fd, &frame, sizeof(frame));
                if (w < 0) {
                    std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                    g_inline_repeat.enabled = false;
                } else {
                    std::printf("\r\nInline repeat started: ID 0x%03X, %zu bytes, every %dms\n",
                                frame.can_id, data.size(), inline_interval_ms);
                    std::printf("Use /rs stop to stop, /ra to stop all.\n");
                }
            } else {
                // Send once
                frame.can_dlc = static_cast<uint8_t>(data.size());
                std::memcpy(frame.data, data.data(), data.size());
                
                ssize_t w = write(fd, &frame, sizeof(frame));
                if (w < 0) {
                    std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                } else {
                    std::printf("\r\nTX[ID:0x%03X DLC:%d]\n", frame.can_id, frame.can_dlc);
                }
            }
        } else {
            // Serial HEX mode
            if (start_inline_repeat) {
                g_inline_repeat.enabled = true;
                g_inline_repeat.is_can = false;
                g_inline_repeat.is_hex = true;
                g_inline_repeat.data = data;
                g_inline_repeat.interval_ms = inline_interval_ms;
                g_inline_repeat.next_fire = Clock::now() + 
                    std::chrono::milliseconds(inline_interval_ms);
                
                // Send first message immediately
                ssize_t w = write(fd, data.data(), data.size());
                if (w < 0) {
                    std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                    g_inline_repeat.enabled = false;
                } else {
                    std::printf("\r\nInline repeat started: %zu bytes, every %dms\n",
                                data.size(), inline_interval_ms);
                    std::printf("Use /rs stop to stop, /ra to stop all.\n");
                }
            } else {
                ssize_t w = write(fd, data.data(), data.size());
                if (w < 0) {
                    std::printf("\r\nWrite error: %s\n", std::strerror(errno));
                } else {
                    std::printf("\r\nTX[%zu bytes]\n", data.size());
                }
            }
        }

        update_prompt_display("> ");
    };

    // Main event loop
    while (g_keep_running) {
        // Handle menu request
        if (g_show_menu) {
            g_show_menu = 0;
            rl_callback_handler_remove();

            // Restore terminal to normal mode for menu interaction
            struct termios old_term{}, menu_term{};
            tcgetattr(STDIN_FILENO, &old_term);
            menu_term = old_term;
            // Enable canonical mode and echo for menu input
            menu_term.c_lflag |= (ICANON | ECHO);
            tcsetattr(STDIN_FILENO, TCSANOW, &menu_term);

            bool need_reconnect = show_settings_menu(cfg, itype, cfg_path, append_crlf);

            // Restore previous terminal state
            tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
            clear_screen();

            // Handle reconnection if settings changed
            if (need_reconnect) {
                close(fd);
                std::printf("Reconnecting...\n");
                
                if (itype == InterfaceType::CAN) {
                    fd = setup_can(cfg["can_interface"], cfg["can_filter"]);
                    if (fd < 0) {
                        std::fprintf(stderr, "Failed to reconnect to CAN interface.\n");
                        break;
                    }
                    std::printf("Connected to %s\n", cfg["can_interface"].c_str());
                } else {
                    fd = open_serial(cfg);
                    if (fd < 0) {
                        std::fprintf(stderr, "Failed to reconnect to serial port.\n");
                        break;
                    }
                    std::printf("Connected to %s @ %s baud\n", 
                                cfg["device"].c_str(), cfg["baud"].c_str());
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }

            rl_callback_handler_install(dynamic_prompt.c_str(), rl_trampoline);
            rl_bind_key(20, ctrl_t_handler);
            rl_forced_update_display();
            continue;
        }

        // Calculate poll timeout based on soonest repeat
        int timeout_ms = 100;
        auto now = Clock::now();
        
        // Check inline repeat timeout
        if (g_inline_repeat.enabled) {
            auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                g_inline_repeat.next_fire - now).count();
            if (diff <= 0) {
                timeout_ms = 0;
            } else {
                timeout_ms = static_cast<int>(std::min<long long>(timeout_ms, diff));
            }
        }
        
        // Check preset repeat timeouts
        for (size_t i = 0; i < 10; ++i) {
            if (g_preset_repeats[i].enabled) {
                auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                    g_preset_repeats[i].next_fire - now).count();
                if (diff <= 0) {
                    timeout_ms = 0;
                    break;
                } else {
                    timeout_ms = static_cast<int>(std::min<long long>(timeout_ms, diff));
                }
            }
        }

        // Poll for events
        struct pollfd fds[2] = {
            {fd, POLLIN, 0},
            {STDIN_FILENO, POLLIN, 0}
        };

        int rv = poll(fds, 2, timeout_ms);
        if (rv < 0) {
            if (errno == EINTR) continue;
            std::perror("poll");
            break;
        }

        // Handle inline repeat transmission
        now = Clock::now();
        if (g_inline_repeat.enabled && now >= g_inline_repeat.next_fire) {
            bool ok = false;
            std::string msg;
            
            if (g_inline_repeat.is_can) {
                // CAN mode
                if (g_inline_repeat.is_hex) {
                    ok = send_can_bytes(fd, g_inline_repeat.can_id, g_inline_repeat.data);
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "TX[Inline ID:0x%03X DLC:%zu]%s",
                                  g_inline_repeat.can_id, g_inline_repeat.data.size(),
                                  ok ? "" : " FAILED");
                    msg = buf;
                } else {
                    // CAN text mode
                    struct can_frame frame{};
                    frame.can_id = g_inline_repeat.can_id;
                    frame.can_dlc = static_cast<uint8_t>(std::min(g_inline_repeat.text_data.size(), size_t(8)));
                    std::memcpy(frame.data, g_inline_repeat.text_data.c_str(), frame.can_dlc);
                    ssize_t w = write(fd, &frame, sizeof(frame));
                    ok = (w == sizeof(frame));
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "TX[Inline ID:0x%03X \"%s\"]%s",
                                  g_inline_repeat.can_id, g_inline_repeat.text_data.c_str(),
                                  ok ? "" : " FAILED");
                    msg = buf;
                }
            } else {
                // Serial mode
                if (g_inline_repeat.is_hex) {
                    ssize_t w = write(fd, g_inline_repeat.data.data(), g_inline_repeat.data.size());
                    ok = (w == static_cast<ssize_t>(g_inline_repeat.data.size()));
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "TX[Inline %zu bytes]%s",
                                  g_inline_repeat.data.size(), ok ? "" : " FAILED");
                    msg = buf;
                } else {
                    // Serial text mode
                    std::string send_data = g_inline_repeat.text_data;
                    if (g_inline_repeat.append_crlf) send_data += "\r\n";
                    ssize_t w = write(fd, send_data.c_str(), send_data.size());
                    ok = (w == static_cast<ssize_t>(send_data.size()));
                    char buf[128];
                    std::snprintf(buf, sizeof(buf), "TX[Inline \"%s\"]%s",
                                  g_inline_repeat.text_data.c_str(), ok ? "" : " FAILED");
                    msg = buf;
                }
            }
            
            print_message_above(msg);
            g_inline_repeat.next_fire = now + 
                std::chrono::milliseconds(g_inline_repeat.interval_ms);
        }

        // Handle multi-preset repeat transmissions
        for (size_t i = 0; i < 10; ++i) {
            if (g_preset_repeats[i].enabled && now >= g_preset_repeats[i].next_fire) {
                int preset_num = static_cast<int>(i + 1);
                bool ok = send_preset(fd, cfg, itype, preset_num, append_crlf);
                std::string pname = cfg["preset" + std::to_string(preset_num) + "_name"];
                std::string msg = ok ? 
                    ("TX[Preset " + std::to_string(preset_num) + " (" + pname + ")]") :
                    ("TX FAILED[Preset " + std::to_string(preset_num) + "]");
                print_message_above(msg);
                g_preset_repeats[i].next_fire = now + 
                    std::chrono::milliseconds(g_preset_repeats[i].interval_ms);
            }
        }

        // Handle incoming data
        if (fds[0].revents & POLLIN) {
            if (itype == InterfaceType::CAN) {
                struct can_frame frame{};
                ssize_t n = read(fd, &frame, sizeof(frame));
                if (n >= static_cast<ssize_t>(sizeof(frame))) {
                    std::string msg = "RX[ID:0x" + std::to_string(frame.can_id) + " DLC:" + 
                                      std::to_string(frame.can_dlc) + "]: ";
                    char hex_buf[8];
                    for (int i = 0; i < frame.can_dlc; ++i) {
                        std::snprintf(hex_buf, sizeof(hex_buf), "0x%02X ", frame.data[i]);
                        msg += hex_buf;
                    }
                    print_message_above(msg);
                }
            } else {
                char buf[256];
                ssize_t n = read(fd, buf, sizeof(buf));
                if (n > 0) {
                    std::string msg = "RX[" + std::to_string(n) + " bytes]: ";
                    char hex_buf[8];
                    for (ssize_t i = 0; i < n; ++i) {
                        std::snprintf(hex_buf, sizeof(hex_buf), "0x%02X ", 
                                      static_cast<unsigned char>(buf[i]));
                        msg += hex_buf;
                    }
                    print_message_above(msg);
                }
            }
        }

        // Handle keyboard input
        if (fds[1].revents & POLLIN) {
            rl_callback_read_char();

            // Update dynamic prompt
            if (g_dynamic_prompt && g_cfg && g_append_crlf) {
                const char* line = rl_line_buffer;
                std::string new_prompt;

                if ((*g_cfg)["mode"] == "hex") {
                    // HEX mode: Extract only hex data, excluding flags
                    std::string data_only = extract_hex_bytes_only(line);
                    std::string hex;
                    for (char c : data_only) {
                        if (!std::isspace(static_cast<unsigned char>(c))) {
                            hex.push_back(c);
                        }
                    }
                    size_t bytes = hex.size() / 2;
                    new_prompt = "[" + std::to_string(bytes) + "b] > ";
                } else {
                    // TEXT mode: Count all characters (no flag stripping)
                    size_t bytes = std::strlen(line);
                    if (*g_append_crlf) bytes += 2;
                    new_prompt = "[" + std::to_string(bytes) + "b] > ";
                }

                *g_dynamic_prompt = new_prompt;
                std::printf("\r\033[K");
                rl_set_prompt(g_dynamic_prompt->c_str());
                rl_replace_line(line, 0);
                rl_forced_update_display();
            }
        }
    }

    // Cleanup
    rl_callback_handler_remove();
    close(fd);
    write_history(hist_path.c_str());
    std::cout << "Disconnected.\n";

    return 0;
}
