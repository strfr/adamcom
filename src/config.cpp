/**
 * @file config.cpp
 * @brief Configuration I/O, baud rate helpers, and CLI usage
 */

#include "adamcom.hpp"

#include <sys/stat.h>
#include <fstream>
#include <stdexcept>
#include <iostream>
#include <termios.h>

namespace adamcom {

bool file_exists(const std::string& path)
{
    struct stat st{};
    return stat(path.c_str(), &st) == 0;
}

Config read_profile(const std::string& path)
{
    Config cfg;
    std::ifstream in(path);
    if (!in) return cfg;

    std::string line;
    while (std::getline(in, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        auto pos = line.find('=');
        if (pos == std::string::npos) continue;

        std::string key = line.substr(0, pos);
        std::string value = line.substr(pos + 1);
        cfg[key] = value;
    }
    return cfg;
}

bool write_profile(const std::string& path, const Config& cfg)
{
    std::ofstream out(path, std::ofstream::trunc);
    if (!out) {
        std::cerr << "Error: Cannot write to " << path << "\n";
        return false;
    }

    out << "# ADAMCOM configuration file\n";
    for (const auto& [key, value] : cfg) {
        out << key << '=' << value << '\n';
    }
    return out.good();
}

unsigned int get_baud_numeric(const std::string& s)
{
    return static_cast<unsigned int>(std::stoul(s));
}

unsigned int get_baud_speed_t(const std::string& s)
{
    static const std::map<unsigned int, speed_t> baud_table = {
        {300,     B300},
        {1200,    B1200},
        {2400,    B2400},
        {4800,    B4800},
        {9600,    B9600},
        {19200,   B19200},
        {38400,   B38400},
        {57600,   B57600},
        {115200,  B115200},
        {230400,  B230400},
        {460800,  B460800},
        {500000,  B500000},
        {576000,  B576000},
        {921600,  B921600},
        {1000000, B1000000},
        {1152000, B1152000},
        {1500000, B1500000},
        {2000000, B2000000},
        {2500000, B2500000},
        {3000000, B3000000},
        {3500000, B3500000},
        {4000000, B4000000}
    };

    unsigned int rate = get_baud_numeric(s);
    auto it = baud_table.find(rate);
    if (it == baud_table.end()) {
        throw std::invalid_argument("Unsupported baud rate: " + s);
    }
    return static_cast<unsigned int>(it->second);
}

void usage(const char* prog)
{
    std::cerr <<
        "ADAMCOM - Serial/CAN Terminal\n"
        "\n"
        "Usage: " << prog << " [OPTIONS]\n"
        "\n"
        "Serial Options:\n"
        "  -d, --device <path>      Serial device (e.g., /dev/ttyUSB0)\n"
        "  -b, --baud <rate>        Baud rate (9600, 115200, etc.)\n"
        "  -i, --databits <5-8>     Data bits\n"
        "  -p, --parity <N|E|O>     Parity (None/Even/Odd)\n"
        "  -s, --stop <1|2>         Stop bits\n"
        "  -f, --flow <mode>        Flow control (none/hardware/software)\n"
        "\n"
        "CAN Options:\n"
        "  -c, --can <iface>        CAN interface (e.g., can0)\n"
        "  --canbitrate <rate>      CAN bitrate (125000/250000/500000/1000000)\n"
        "  --canid <id>             TX CAN ID in hex (default: 0x123)\n"
        "  --filter <id:mask>       CAN RX filter in hex (e.g., 0x100:0x7FF)\n"
        "\n"
        "Mode Options:\n"
        "  --hex                    Start in hex mode\n"
        "  --normal                 Start in normal/text mode\n"
        "  --crlf, --no-crlf        Append CRLF to lines (default: yes)\n"
        "\n"
        "Preset/Repeat:\n"
        "  --preset <n>             Send preset 1-10 once and exit\n"
        "  --repeat <n,ms>          Auto-repeat preset n every ms\n"
        "\n"
        "Other:\n"
        "  -h, --help               Show this help\n"
        "\n"
        "Interactive Commands:\n"
        "  Ctrl-T                   Open settings menu\n"
        "  Ctrl-C                   Quit\n"
        "  /p N                     Send preset N once\n"
        "  /r on|off                Toggle repeat mode\n"
        "  /ri MS                   Set repeat interval\n"
        "  /rp N                    Set repeat preset\n"
        "  /menu                    Open menu\n"
        "  /help                    Show commands\n"
        "\n"
        "Config file: ~/.adamcomrc\n";
}

} // namespace adamcom
