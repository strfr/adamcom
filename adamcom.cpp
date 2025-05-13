// adamcom.cpp
// Copyright (c) 2025 Huseyin B <aawifoa@gmail.com>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>

static volatile bool keep_running = true;
void sigint_handler(int) { keep_running = false; }

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -d, --device   <path>      serial device (default from profile)\n"
              << "  -b, --baud     <rate>      baud rate (default from profile)\n"
              << "  -D, --data     <5|6|7|8>   data bits (default from profile)\n"
              << "  -p, --parity   <N|E|O>     parity (default from profile)\n"
              << "  -s, --stop     <1|2>       stop bits (default from profile)\n"
              << "  -f, --flow     <none|hardware|software>\n"
              << "                             flow control (default from profile)\n"
              << "  -h, --help                 show this help and exit\n"
              << "\n"
              << "Device not found? Use -d to specify correct serial port.\n";
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::map<std::string, std::string> read_profile(const std::string& path) {
    std::map<std::string, std::string> cfg;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        cfg[line.substr(0,pos)] = line.substr(pos+1);
    }
    return cfg;
}

void write_profile(const std::string& path, const std::map<std::string,std::string>& cfg) {
    std::ofstream out(path, std::ofstream::trunc);
    for (auto& kv : cfg) {
        out << kv.first << "=" << kv.second << "\n";
    }
}

int get_baud(const std::string& s) {
    static const std::map<int, speed_t> m = {
        { 9600,   B9600   },{19200,   B19200  },{38400,   B38400 },
        {57600,   B57600  },{115200,  B115200 },{230400,  B230400},
        {460800,  B460800},{500000,  B500000 },{576000,  B576000},
        {921600,  B921600}
    };
    int r = std::stoi(s);
    auto it = m.find(r);
    if (it == m.end()) throw std::invalid_argument("unsupported baud");
    return it->second;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, sigint_handler);

    // determine config file path
    const char* home = getenv("HOME");
    std::string cfg_path = std::string(home ? home : ".") + "/.adamcomrc";
    // ensure profile exists
    if (!file_exists(cfg_path)) {
        std::map<std::string,std::string> def = {
            {"device", "/dev/tnt1"},
            {"baud",   "115200"},
            {"data",   "8"},
            {"parity", "N"},
            {"stop",   "1"},
            {"flow",   "none"}
        };
        write_profile(cfg_path, def);
    }

    // load profile defaults
    auto cfg = read_profile(cfg_path);
    bool changed = false;

    // parse argv
    std::vector<std::string> args(argv+1, argv+argc);
    for (size_t i = 0; i < args.size(); ++i) {
        auto& a = args[i];
        if ((a=="-h")||(a=="--help")) { usage(argv[0]); return 0; }
        if ((a=="-d"||a=="--device") && i+1<args.size()) {
            cfg["device"] = args[++i]; changed = true; continue;
        }
        if ((a=="-b"||a=="--baud") && i+1<args.size()) {
            cfg["baud"] = args[++i]; changed = true; continue;
        }
        if ((a=="-D"||a=="--data") && i+1<args.size()) {
            cfg["data"] = args[++i]; changed = true; continue;
        }
        if ((a=="-p"||a=="--parity") && i+1<args.size()) {
            cfg["parity"] = args[++i]; changed = true; continue;
        }
        if ((a=="-s"||a=="--stop") && i+1<args.size()) {
            cfg["stop"] = args[++i]; changed = true; continue;
        }
        if ((a=="-f"||a=="--flow") && i+1<args.size()) {
            cfg["flow"] = args[++i]; changed = true; continue;
        }
        std::cerr << "Unknown option: " << a << "\n";
        usage(argv[0]);
        return 1;
    }

    // save updated profile
    if (changed) write_profile(cfg_path, cfg);

    // open device
    int fd = open(cfg["device"].c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0) {
        if (errno == ENOENT) {
            std::cerr << "Error: Cannot find device "
                      << cfg["device"]
                      << ". Use -d,--device to specify.\n";
        } else {
            std::cerr << "Error opening " << cfg["device"]
                      << ": " << strerror(errno) << "\n";
        }
        return 1;
    }

    // configure port
    termios tty;
    if (tcgetattr(fd, &tty)) {
        std::cerr << "tcgetattr error\n"; return 1;
    }
    cfsetispeed(&tty, get_baud(cfg["baud"]));
    cfsetospeed(&tty, get_baud(cfg["baud"]));

    tty.c_cflag &= ~CSIZE;
    switch (std::stoi(cfg["data"])) {
      case 5: tty.c_cflag |= CS5; break;
      case 6: tty.c_cflag |= CS6; break;
      case 7: tty.c_cflag |= CS7; break;
      case 8: tty.c_cflag |= CS8; break;
      default: std::cerr<<"Invalid data bits\n"; return 1;
    }
    char p = toupper(cfg["parity"][0]);
    if (p=='E') tty.c_cflag |= PARENB;
    else if (p=='O') tty.c_cflag |= PARENB|PARODD;
    else tty.c_cflag &= ~PARENB;

    tty.c_cflag &= ~(CSTOPB);
    if (cfg["stop"] == "2") tty.c_cflag |= CSTOPB;

    if (cfg["flow"]=="none") {
        tty.c_cflag &= ~CRTSCTS;
        tty.c_iflag &= ~(IXON|IXOFF|IXANY);
    } else if (cfg["flow"]=="hardware") {
        tty.c_cflag |= CRTSCTS;
        tty.c_iflag &= ~(IXON|IXOFF|IXANY);
    } else if (cfg["flow"]=="software") {
        tty.c_cflag &= ~CRTSCTS;
        tty.c_iflag |= (IXON|IXOFF|IXANY);
    } else {
        std::cerr<<"Invalid flow control\n"; return 1;
    }

    tty.c_cflag |= CREAD|CLOCAL;
    tty.c_lflag &= ~(ICANON|ECHO|ECHOE|ISIG);
    tty.c_iflag &= ~(INLCR|ICRNL|IGNCR);
    tty.c_oflag &= ~OPOST;

    if (tcsetattr(fd, TCSANOW, &tty)) {
        std::cerr<<"tcsetattr error\n"; return 1;
    }

    // set non-blocking read
    fcntl(fd, F_SETFL, FNDELAY);

    // main loop: read device and stdin
    std::cout << "Connected to " << cfg["device"] << ". Ctrl-C to exit.\n";
    while (keep_running) {
        // read from serial
        char buf[256];
        int n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            std::cout << "RX["<<n<<" bytes]: ";
            for (int i = 0; i < n; ++i)
                std::cout << std::hex << std::uppercase
                          << (buf[i]&0xFF) << " ";
            std::cout << std::dec << "\n";
        }
        // read from stdin
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        timeval tv = {0,100000}; // 100ms
        if (select(STDIN_FILENO+1, &rfds, NULL, NULL, &tv) > 0) {
            std::string line;
            if (std::getline(std::cin, line)) {
                int to_write = write(fd, line.data(), line.size());
                std::cout << "TX["<< to_write <<" bytes]\n";
            }
        }
    }

    close(fd);
    std::cout << "Disconnected.\n";
    return 0;
}
