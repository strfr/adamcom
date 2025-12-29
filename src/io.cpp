/**
 * @file io.cpp
 * @brief Serial and CAN I/O operations
 */

#include "adamcom.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <cstring>
#include <cctype>
#include <cstdlib>
#include <iostream>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

namespace adamcom {

bool parse_hex_bytes(const std::string& input, std::vector<uint8_t>& out)
{
    out.clear();

    // Strip whitespace
    std::string hex;
    hex.reserve(input.size());
    for (char c : input) {
        if (!std::isspace(static_cast<unsigned char>(c))) {
            hex.push_back(c);
        }
    }

    // Must have even number of hex chars
    if (hex.size() % 2 != 0) {
        return false;
    }

    // Validate and parse
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        char hi = hex[i];
        char lo = hex[i + 1];

        if (!std::isxdigit(static_cast<unsigned char>(hi)) ||
            !std::isxdigit(static_cast<unsigned char>(lo))) {
            out.clear();
            return false;
        }

        int byte = std::stoi(hex.substr(i, 2), nullptr, 16);
        out.push_back(static_cast<uint8_t>(byte));
    }

    return true;
}

bool send_serial_bytes(int fd, const std::vector<uint8_t>& data)
{
    if (data.empty()) return true;
    ssize_t written = write(fd, data.data(), data.size());
    return written == static_cast<ssize_t>(data.size());
}

bool send_serial_text(int fd, const std::string& text, bool append_crlf)
{
    std::string msg = text;
    if (append_crlf) {
        msg += "\r\n";
    }
    ssize_t written = write(fd, msg.c_str(), msg.size());
    return written == static_cast<ssize_t>(msg.size());
}

int configure_can_interface(const std::string& ifname, const std::string& bitrate)
{
    // Validate interface name to prevent command injection
    for (char c : ifname) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            std::cerr << "Invalid CAN interface name\n";
            return -1;
        }
    }

    // Validate bitrate is numeric
    for (char c : bitrate) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            std::cerr << "Invalid CAN bitrate\n";
            return -1;
        }
    }

    // Bring interface down (ignore failure - might not be up)
    std::string cmd = "sudo ip link set " + ifname + " down 2>/dev/null";
    [[maybe_unused]] int rc = std::system(cmd.c_str());

    // Set bitrate
    cmd = "sudo ip link set " + ifname + " type can bitrate " + bitrate;
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "Failed to set CAN bitrate (may need sudo)\n";
        return -1;
    }

    // Bring interface up
    cmd = "sudo ip link set " + ifname + " up";
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "Failed to bring up CAN interface\n";
        return -1;
    }

    return 0;
}

int setup_can(const std::string& ifname, const std::string& filter_str)
{
    int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0) {
        perror("CAN socket");
        return -1;
    }

    struct ifreq ifr{};
    std::strncpy(ifr.ifr_name, ifname.c_str(), IFNAMSIZ - 1);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("CAN ioctl SIOCGIFINDEX");
        close(sock);
        return -1;
    }

    struct sockaddr_can addr{};
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("CAN bind");
        close(sock);
        return -1;
    }

    // Apply filter if specified
    if (!filter_str.empty() && filter_str != "none") {
        auto pos = filter_str.find(':');
        if (pos != std::string::npos) {
            try {
                struct can_filter rfilter{};
                rfilter.can_id = std::stoul(filter_str.substr(0, pos), nullptr, 16);
                rfilter.can_mask = std::stoul(filter_str.substr(pos + 1), nullptr, 16);
                setsockopt(sock, SOL_CAN_RAW, CAN_RAW_FILTER, &rfilter, sizeof(rfilter));
            } catch (const std::exception& e) {
                std::cerr << "Invalid CAN filter format: " << e.what() << "\n";
            }
        }
    }

    return sock;
}

bool send_can_bytes(int fd, uint32_t can_id, const std::vector<uint8_t>& data)
{
    struct can_frame frame{};
    frame.can_id = can_id;
    frame.can_dlc = static_cast<uint8_t>(std::min<size_t>(data.size(), 8));
    std::memcpy(frame.data, data.data(), frame.can_dlc);

    ssize_t written = write(fd, &frame, sizeof(frame));
    return written == sizeof(frame);
}

bool send_preset(int fd, const Config& cfg, InterfaceType itype,
                 int preset_index, bool append_crlf)
{
    if (preset_index < 1 || preset_index > 10) {
        return false;
    }

    std::string prefix = "preset" + std::to_string(preset_index) + "_";

    auto get_cfg = [&](const std::string& key) -> std::string {
        auto it = cfg.find(prefix + key);
        return (it != cfg.end()) ? it->second : "";
    };

    std::string format = get_cfg("format");
    std::string data_str = get_cfg("data");

    if (data_str.empty()) {
        return false;  // No data to send
    }

    if (itype == InterfaceType::CAN) {
        std::vector<uint8_t> data;
        if (!parse_hex_bytes(data_str, data)) {
            return false;
        }
        if (data.size() > 8) {
            data.resize(8);
        }

        std::string can_id_str = get_cfg("can_id");
        if (can_id_str.empty()) {
            auto it = cfg.find("can_id");
            can_id_str = (it != cfg.end()) ? it->second : "0x123";
        }

        uint32_t can_id = 0;
        try {
            can_id = std::stoul(can_id_str, nullptr, 16);
        } catch (...) {
            return false;
        }

        return send_can_bytes(fd, can_id, data);
    } else {
        // Serial mode
        if (format == "text") {
            return send_serial_text(fd, data_str, append_crlf);
        } else {
            std::vector<uint8_t> data;
            if (!parse_hex_bytes(data_str, data)) {
                return false;
            }
            return send_serial_bytes(fd, data);
        }
    }
}

int open_serial(const Config& cfg)
{
    auto get = [&](const std::string& key, const std::string& def) -> std::string {
        auto it = cfg.find(key);
        return (it != cfg.end()) ? it->second : def;
    };

    std::string device = get("device", "/dev/ttyUSB0");
    int fd = open(device.c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        std::perror(device.c_str());
        return -1;
    }

    struct termios tty{};
    if (tcgetattr(fd, &tty) != 0) {
        std::perror("tcgetattr");
        close(fd);
        return -1;
    }

    // Baud rate
    try {
        speed_t speed = static_cast<speed_t>(get_baud_speed_t(get("baud", "115200")));
        cfsetispeed(&tty, speed);
        cfsetospeed(&tty, speed);
    } catch (const std::exception& e) {
        std::cerr << "Invalid baud rate: " << e.what() << "\n";
        close(fd);
        return -1;
    }

    // Data bits
    int databits = 8;
    try { databits = std::stoi(get("databits", "8")); } catch (...) {}
    tty.c_cflag &= ~CSIZE;
    switch (databits) {
        case 5: tty.c_cflag |= CS5; break;
        case 6: tty.c_cflag |= CS6; break;
        case 7: tty.c_cflag |= CS7; break;
        default: tty.c_cflag |= CS8; break;
    }

    // Parity
    std::string parity_str = get("parity", "N");
    char parity = 'N';
    if (!parity_str.empty()) {
        parity = static_cast<char>(std::toupper(static_cast<unsigned char>(parity_str[0])));
    }
    tty.c_cflag &= ~(PARENB | PARODD);
    if (parity == 'E') {
        tty.c_cflag |= PARENB;
    } else if (parity == 'O') {
        tty.c_cflag |= PARENB | PARODD;
    }

    // Stop bits
    int stopbits = 1;
    try { stopbits = std::stoi(get("stop", "1")); } catch (...) {}
    if (stopbits == 2) {
        tty.c_cflag |= CSTOPB;
    } else {
        tty.c_cflag &= ~CSTOPB;
    }

    // Flow control
    std::string flow = get("flow", "none");
    tty.c_cflag &= ~CRTSCTS;
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);
    if (flow == "hardware") {
        tty.c_cflag |= CRTSCTS;
    } else if (flow == "software") {
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
        return -1;
    }

    fcntl(fd, F_SETFL, FNDELAY);
    return fd;
}

} // namespace adamcom
