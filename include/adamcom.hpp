/**
 * @file adamcom.hpp
 * @brief ADAMCOM - Serial and CAN terminal application
 *
 * A minicom-like terminal with support for both serial ports and SocketCAN
 * interfaces, featuring persistent presets, multi-preset repeat mode, and an interactive menu.
 */

#pragma once

#include <string>
#include <map>
#include <vector>
#include <array>
#include <cstdint>
#include <chrono>

namespace adamcom {

// ============================================================================
// Types
// ============================================================================

/// Interface type for communication
enum class InterfaceType { SERIAL, CAN };

/// Configuration map type alias
using Config = std::map<std::string, std::string>;

/// Per-preset repeat state (supports multiple simultaneous repeats)
struct PresetRepeatState {
    bool enabled = false;
    int interval_ms = 1000;
    std::chrono::steady_clock::time_point next_fire;
};

/// Global array holding repeat state for all 10 presets (index 0-9 for presets 1-10)
extern std::array<PresetRepeatState, 10> g_preset_repeats;

// ============================================================================
// Configuration I/O
// ============================================================================

/// Check if a file exists
bool file_exists(const std::string& path);

/// Read configuration from file
Config read_profile(const std::string& path);

/// Write configuration to file (returns true on success)
bool write_profile(const std::string& path, const Config& cfg);

// ============================================================================
// Serial Helpers
// ============================================================================

/// Parse baud rate string to numeric value
unsigned int get_baud_numeric(const std::string& s);

/// Get platform speed_t value for baud rate (throws on unsupported baud)
unsigned int get_baud_speed_t(const std::string& s);

// ============================================================================
// Data Parsing
// ============================================================================

/// Parse hex string (spaces allowed) to byte vector
bool parse_hex_bytes(const std::string& input, std::vector<uint8_t>& out);

// ============================================================================
// Serial I/O
// ============================================================================

/// Send raw bytes over serial
bool send_serial_bytes(int fd, const std::vector<uint8_t>& data);

/// Send text over serial (optionally append CRLF)
bool send_serial_text(int fd, const std::string& text, bool append_crlf);

// ============================================================================
// CAN Helpers
// ============================================================================

/// Configure CAN interface bitrate and bring up (requires sudo)
int configure_can_interface(const std::string& ifname, const std::string& bitrate);

/// Setup CAN socket and bind to interface
int setup_can(const std::string& ifname, const std::string& filter_str = "");

/// Send CAN frame (data max 8 bytes)
bool send_can_bytes(int fd, uint32_t can_id, const std::vector<uint8_t>& data);

// ============================================================================
// Presets
// ============================================================================

/// Send a preset message (1-10)
bool send_preset(int fd, const Config& cfg, InterfaceType itype,
                 int preset_index, bool append_crlf);

// ============================================================================
// Menu UI
// ============================================================================

/// Clear terminal screen
void clear_screen();

/// Show main settings menu (modifies cfg and ref params)
/// Returns true if connection settings changed and reconnect is needed
bool show_settings_menu(Config& cfg, InterfaceType& itype,
                        const std::string& cfg_path, bool& append_crlf);

/// Show presets editor submenu
void show_presets_menu(Config& cfg, InterfaceType itype);

/// Show comprehensive manual/help
void show_manual();

/// Open and configure a serial port, returns fd or -1 on error
int open_serial(const Config& cfg);

// ============================================================================
// Output Helpers
// ============================================================================

/// Print a message above the current input line without interrupting typing
void print_message_above(const std::string& msg);

// ============================================================================
// CLI
// ============================================================================

/// Print usage/help text to stderr
void usage(const char* prog);

} // namespace adamcom
