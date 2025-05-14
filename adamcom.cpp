// adamcom.cpp
// Copyright (c) 2025 Huseyin B <aawifoa@gmail.com>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/select.h>

static volatile bool keep_running = true;
void sigint_handler(int) { keep_running = false; }

void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  -d, -D, --device   <path>          serial device\n"
              << "  -b, --baud         <rate>          baud rate\n"
              << "  -i, --databits     <5|6|7|8>       data bits\n"
              << "  -p, --parity       <N|E|O>         parity\n"
              << "  -s, --stop         <1|2>           stop bits\n"
              << "  -f, --flow         <none|hardware|software>  flow control\n"
              << "  -h, --help                          show this help\n\n"
              << "If device not found, use -d/--device to specify.\n";
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

std::map<std::string,std::string> read_profile(const std::string& path) {
    std::map<std::string,std::string> cfg;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        cfg[line.substr(0,pos)] = line.substr(pos+1);
    }
    return cfg;
}

void write_profile(const std::string& path,
                   const std::map<std::string,std::string>& cfg) {
    std::ofstream out(path, std::ofstream::trunc);
    for (auto& kv : cfg) out << kv.first << "=" << kv.second << "\n";
}

speed_t get_baud(const std::string& s) {
    static const std::map<int, speed_t> m = {
        {9600,B9600},{19200,B19200},{38400,B38400},
        {57600,B57600},{115200,B115200},{230400,B230400},
        {460800,B460800},{500000,B500000},{576000,B576000},
        {921600,B921600}
    };
    int r = std::stoi(s);
    auto it = m.find(r);
    if (it == m.end()) throw std::invalid_argument("unsupported baud");
    return it->second;
}

// Dynamic prompt refresh using readline event hook
static char prompt_buf[64];
int refresh_prompt() {
    size_t len = strlen(rl_line_buffer);
    snprintf(prompt_buf, sizeof(prompt_buf), "> (%zu bytes) ", len);
    rl_set_prompt(prompt_buf);
    rl_redisplay();
    return 0;
}

std::string read_input_line() {
    rl_event_hook = refresh_prompt;
    snprintf(prompt_buf, sizeof(prompt_buf), "> (0 bytes) ");
    char* buf = readline(prompt_buf);
    rl_event_hook = nullptr;
    if (!buf) return {};
    std::string line(buf);
    if (!line.empty()) add_history(buf);
    free(buf);
    return line;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, sigint_handler);
    const char* home = getenv("HOME");
    std::string cfg_path = std::string(home?home:".") + "/.adamcomrc";

    if (!file_exists(cfg_path)) {
        write_profile(cfg_path, {{"device","/dev/tnt1"},{"baud","115200"},
                                 {"databits","8"},{"parity","N"},
                                 {"stop","1"},{"flow","none"}});
    }
    auto cfg = read_profile(cfg_path);
    bool fixed = false;
    for (auto &d : std::vector<std::pair<std::string,std::string>>{
           {"device","/dev/tnt1"},{"baud","115200"},
           {"databits","8"},{"parity","N"},
           {"stop","1"},{"flow","none"}}) {
        if (cfg[d.first].empty()) { cfg[d.first] = d.second; fixed = true; }
    }
    if (fixed) write_profile(cfg_path, cfg);

    bool changed = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a=="-h"||a=="--help") { usage(argv[0]); return 0; }
        if ((a=="-d"||a=="-D"||a=="--device")&&i+1<argc) {
            cfg["device"] = argv[++i]; changed = true; continue;
        }
        if ((a=="-b"||a=="--baud")&&i+1<argc) {
            cfg["baud"] = argv[++i]; changed = true; continue;
        }
        if ((a=="-i"||a=="--databits")&&i+1<argc) {
            cfg["databits"] = argv[++i]; changed = true; continue;
        }
        if ((a=="-p"||a=="--parity")&&i+1<argc) {
            cfg["parity"] = argv[++i]; changed = true; continue;
        }
        if ((a=="-s"||a=="--stop")&&i+1<argc) {
            cfg["stop"] = argv[++i]; changed = true; continue;
        }
        if ((a=="-f"||a=="--flow")&&i+1<argc) {
            cfg["flow"] = argv[++i]; changed = true; continue;
        }
        std::cerr << "Unknown option: " << a << "\n";
        usage(argv[0]); return 1;
    }
    if (changed) write_profile(cfg_path, cfg);

    int fd = open(cfg["device"].c_str(), O_RDWR|O_NOCTTY);
    if (fd < 0) {
        std::cerr << "Error opening " << cfg["device"]
                  << ": " << strerror(errno) << "\n";
        return 1;
    }
    termios tty;
    if (tcgetattr(fd, &tty)) { std::cerr<<"tcgetattr error\n"; return 1; }
    try {
        speed_t sp = get_baud(cfg["baud"]);
        cfsetispeed(&tty, sp); cfsetospeed(&tty, sp);
    } catch (...) {
        std::cerr<<"Error: invalid baud '"<<cfg["baud"]<<"'.\n"; return 1;
    }
    try {
        int db = std::stoi(cfg["databits"]);
        tty.c_cflag &= ~CSIZE;
        switch(db) { case 5:tty.c_cflag|=CS5;break; case 6:tty.c_cflag|=CS6;break;\
case 7:tty.c_cflag|=CS7;break; case 8:tty.c_cflag|=CS8;break; default:throw 0;}\
    } catch (...) {
        std::cerr<<"Error: invalid data-bits.\n"; return 1;
    }
    char p = std::toupper(cfg["parity"][0]); tty.c_cflag&=~PARENB;
    if (p=='E') tty.c_cflag |= PARENB; else if (p=='O') tty.c_cflag |= PARENB|PARODD;
    try { int sb = std::stoi(cfg["stop"]); tty.c_cflag &= ~CSTOPB; if(sb==2)tty.c_cflag|=CSTOPB; else if(sb!=1)throw 0; } catch(...) { std::cerr<<"Error: invalid stop-bits.\n"; return 1; }
    tty.c_cflag &= ~CRTSCTS; tty.c_iflag &= ~(IXON|IXOFF|IXANY);
    if      (cfg["flow"]=="hardware") tty.c_cflag |= CRTSCTS;
    else if (cfg["flow"]=="software") tty.c_iflag |= (IXON|IXOFF|IXANY);
    else if (cfg["flow"]!="none")      { std::cerr<<"Error: invalid flow.\n"; return 1;}
    tty.c_cflag |= CREAD|CLOCAL;
    tty.c_lflag &= ~(ICANON|ECHO|ECHOE|ISIG);
    tty.c_iflag &= ~(INLCR|ICRNL|IGNCR);
    tty.c_oflag &= ~OPOST;
    if (tcsetattr(fd, TCSANOW, &tty)) { std::cerr<<"tcsetattr error\n"; return 1; }
    fcntl(fd, F_SETFL, FNDELAY);

    std::cout<<"Connected to "<<cfg["device"]<<". Ctrl-C to exit.\n";

    while (keep_running) {
        char buf[256]; int n = read(fd, buf, sizeof(buf));
        if (n>0) {
            std::cout<<"\nRX["<<n<<" bytes]: ";
            for (int i=0;i<n;i++) std::cout<<std::hex<<std::uppercase<<(buf[i]&0xFF)<<" ";
            std::cout<<std::dec<<"\n";
        }
        std::string line = read_input_line();
        if (!line.empty()) {
            int w = write(fd, line.c_str(), line.size());
            std::cout<<"TX["<<w<<" bytes]\n";
        }
    }

    close(fd);
    std::cout<<"Disconnected.\n";
    return 0;
}
