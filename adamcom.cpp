// adamcom.cpp  –  always-listening serial console
// Copyright (c) 2025 Huseyin B <aawifoa@gmail.com>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <poll.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <functional>

#include <readline/readline.h>
#include <readline/history.h>
#include <readline/keymaps.h>

static volatile bool keep_running = true;
void sigint_handler(int) { keep_running = false; }

/* ---------- small helpers ---------- */
void usage(const char* prog)
{
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  -d, -D, --device   <path>          serial device\n"
              << "  -b, --baud         <rate>          baud rate\n"
              << "  -i, --databits     <5|6|7|8>\n"
              << "  -p, --parity       <N|E|O>\n"
              << "  -s, --stop         <1|2>\n"
              << "  -f, --flow         <none|hardware|software>\n"
              << "  --hex, --normal    start mode\n"
              << "  --crlf, --no-crlf  append CRLF to sent lines (default: yes)\n"
              << "  -h, --help\n";
}

bool file_exists(const std::string& p)
{
    struct stat st{};
    return stat(p.c_str(), &st) == 0;
}

std::map<std::string,std::string> read_profile(const std::string& path)
{
    std::map<std::string,std::string> cfg;
    std::ifstream in(path);
    std::string l;
    while (std::getline(in,l)) {
        auto pos = l.find('=');
        if (pos==std::string::npos) continue;
        cfg[l.substr(0,pos)] = l.substr(pos+1);
    }
    return cfg;
}

void write_profile(const std::string& path,
                   const std::map<std::string,std::string>& cfg)
{
    std::ofstream out(path,std::ofstream::trunc);
    for (auto& kv: cfg) out<<kv.first<<'='<<kv.second<<'\n';
}

speed_t get_baud(const std::string& s)
{
    static const std::map<int,speed_t> tbl = {
        {9600,B9600},{19200,B19200},{38400,B38400},{57600,B57600},
        {115200,B115200},{230400,B230400},{460800,B460800},
        {500000,B500000},{576000,B576000},{921600,B921600}
    };
    int r = std::stoi(s);
    auto it = tbl.find(r);
    if (it==tbl.end()) throw std::invalid_argument("baud");
    return it->second;
}

/* ---------- readline callback bridge ---------- */
static std::function<void(char*)> g_line_handler;

extern "C" void rl_trampoline(char* line)
{
    if (g_line_handler) g_line_handler(line);
}

// Global variables for readline hooks
static std::string* g_dynamic_prompt = nullptr;
static std::map<std::string,std::string>* g_cfg = nullptr;
static bool* g_append_crlf = nullptr;

extern "C" int startup_hook(void) {
    if (g_dynamic_prompt) {
        rl_set_prompt(g_dynamic_prompt->c_str());
    }
    return 0;
}

extern "C" int pre_input_hook(void) {
    if (!g_dynamic_prompt || !g_cfg || !g_append_crlf) return 0;

    const char* line = rl_line_buffer;
    if ((*g_cfg)["mode"] == "hex") {
        std::string s;
        for(char c: std::string(line)) if(!std::isspace(c)) s.push_back(c);
        size_t bytes = s.size() / 2;  // Each byte is 2 hex chars
        *g_dynamic_prompt = std::string("[") + std::to_string(bytes) + "b] > ";
    } else {
        size_t bytes = strlen(line);
        if (*g_append_crlf) bytes += 2;  // Add 2 for \r\n if enabled
        *g_dynamic_prompt = std::string("[") + std::to_string(bytes) + "b] > ";
    }
    rl_set_prompt(g_dynamic_prompt->c_str());
    rl_redisplay();
    return 0;
}

/* ---------- main ---------- */
int main(int argc,char* argv[])
{
    signal(SIGINT,sigint_handler);

    const char* home = getenv("HOME");
    std::string cfg_path = std::string(home?home:".") + "/.adamcomrc";

    /* varsayılan profil oluştur */
    if (!file_exists(cfg_path)) {
        write_profile(cfg_path,{
            {"device","/dev/tnt1"},{"baud","115200"},
            {"databits","8"},{"parity","N"},{"stop","1"},
            {"flow","none"},{"mode","normal"},
            {"crlf","yes"}
        });
    }
    auto cfg = read_profile(cfg_path);

    /* eksik anahtarlar */
    bool fixed=false;
    std::vector<std::pair<std::string,std::string>> def = {
        {"device","/dev/tnt1"},{"baud","115200"}, {"databits","8"},
        {"parity","N"},{"stop","1"},{"flow","none"},{"mode","normal"},
        {"crlf","yes"}
    };
    for (auto& p:def)
        if (cfg.find(p.first)==cfg.end()) { cfg[p.first]=p.second; fixed=true; }
    if (fixed) write_profile(cfg_path,cfg);

    /* argümanlar */
    bool changed=false;
    bool append_crlf = (cfg["crlf"]=="yes"); // default from config
    for (int i=1;i<argc;++i) {
        std::string a = argv[i];
        auto need = [&](const char* key){
            if (i+1>=argc){usage(argv[0]); exit(1);}
            cfg[key]=argv[++i]; changed=true;
        };
        if (a=="-h"||a=="--help"){usage(argv[0]); return 0;}
        else if (a=="-d"||a=="-D"||a=="--device") need("device");
        else if (a=="-b"||a=="--baud")            need("baud");
        else if (a=="-i"||a=="--databits")        need("databits");
        else if (a=="-p"||a=="--parity")          need("parity");
        else if (a=="-s"||a=="--stop")            need("stop");
        else if (a=="-f"||a=="--flow")            need("flow");
        else if (a=="--hex")   { cfg["mode"]="hex";    changed=true; }
        else if (a=="--normal"){ cfg["mode"]="normal"; changed=true; }
        else if (a=="--crlf")  { append_crlf = true; cfg["crlf"]="yes"; changed=true; }
        else if (a=="--no-crlf") { append_crlf = false; cfg["crlf"]="no"; changed=true; }
        else { std::cerr<<"Unknown option "<<a<<"\n"; usage(argv[0]); return 1; }
    }
    if (changed) write_profile(cfg_path,cfg);

    /* seri port aç/ayarla */
    int fd = open(cfg["device"].c_str(), O_RDWR|O_NOCTTY);
    if (fd<0){ perror(cfg["device"].c_str()); return 1; }

    termios tty{};
    if (tcgetattr(fd,&tty)){ perror("tcgetattr"); return 1; }

    try{ speed_t sp = get_baud(cfg["baud"]); cfsetispeed(&tty,sp); cfsetospeed(&tty,sp); }
    catch(...){ std::cerr<<"Invalid baud\n"; return 1; }

    try{
        int db = std::stoi(cfg["databits"]);
        tty.c_cflag &= ~CSIZE;
        tty.c_cflag |= (db==5?CS5: db==6?CS6: db==7?CS7: CS8);
    }
    catch(...){ std::cerr<<"Invalid data bits\n"; return 1; }

    char parity = std::toupper(cfg["parity"][0]);
    tty.c_cflag &= ~PARENB;
    if (parity=='E') tty.c_cflag |= PARENB;
    else if (parity=='O') tty.c_cflag |= PARENB|PARODD;

    try{
        int sb = std::stoi(cfg["stop"]);
        if (sb==2) tty.c_cflag |= CSTOPB;
        else if (sb!=1) throw 0;
    }
    catch(...){ std::cerr<<"Invalid stop bits\n"; return 1; }

    tty.c_cflag &= ~CRTSCTS; tty.c_iflag &= ~(IXON|IXOFF|IXANY);
    if (cfg["flow"]=="hardware")       tty.c_cflag |= CRTSCTS;
    else if (cfg["flow"]=="software")  tty.c_iflag |= (IXON|IXOFF|IXANY);
    else if (cfg["flow"]!="none"){ std::cerr<<"Invalid flow\n"; return 1; }

    tty.c_cflag |= CREAD|CLOCAL;
    tty.c_lflag &= ~(ICANON|ECHO|ECHOE|ISIG);
    tty.c_iflag &= ~(INLCR|ICRNL|IGNCR);
    tty.c_oflag &= ~OPOST;

    if (tcsetattr(fd,TCSANOW,&tty)){ perror("tcsetattr"); return 1; }
    fcntl(fd,F_SETFL,FNDELAY);   // non-blocking

    std::cout<<"Connected to "<<cfg["device"]<<"  (Ctrl-C to quit)\n";

    /* ---------- readline callback kurulumu ---------- */
    const char* base_prompt = "> ";
    std::string dynamic_prompt = base_prompt;
    
    // Set global variables for hooks
    g_dynamic_prompt = &dynamic_prompt;
    g_cfg = &cfg;
    g_append_crlf = &append_crlf;

    // Install readline callback
    rl_callback_handler_install(dynamic_prompt.c_str(), rl_trampoline);

    // Setup history file
    std::string histfile = std::string(home ? home : ".") + "/.adamcom_history";
    read_history(histfile.c_str());

    // Add readline callback for prompt updates
    rl_startup_hook = startup_hook;
    rl_pre_input_hook = pre_input_hook;

    // Force initial display update
    rl_forced_update_display();

    g_line_handler = [&](char* buf)
    {
        if (!buf) { keep_running=false; return; }         // Ctrl-D
        std::string line(buf); free(buf);
        if (line.empty()) return;

        // Add non-empty lines to history
        if (!line.empty()) {
            add_history(line.c_str());
            write_history(histfile.c_str());
        }

        printf("\r\033[K");  // Clear current line

        if (cfg["mode"]=="hex") {
            std::string s;
            for(char c:line) if(!std::isspace(c)) s.push_back(c);
            if (s.size()%2){ std::cerr<<"Hex length must be even\n"; return; }

            std::vector<uint8_t> data;
            try{
                for (size_t i=0;i<s.size();i+=2){
                    int b=std::stoi(s.substr(i,2),nullptr,16);
                    data.push_back(uint8_t(b));
                }
            }catch(...){ std::cerr<<"Invalid hex\n"; return; }

            ssize_t w = write(fd,data.data(),data.size());
            if (w < 0) {
                printf("\r\nWrite error: %s\r\n", strerror(errno));
            } else {
                printf("\r\nTX[%zu bytes]\r\n", data.size());
            }
        }
        else {
            std::string to_send = line;
            if (append_crlf) to_send += "\r\n";
            ssize_t w = write(fd,to_send.c_str(),to_send.size());
            if (w < 0) {
                printf("\r\nWrite error: %s\r\n", strerror(errno));
            } else {
                printf("\r\nTX[%zu bytes]\r\n", line.size());
            }
        }
        
        // Reset prompt to base prompt and force update
        dynamic_prompt = base_prompt;
        rl_set_prompt(dynamic_prompt.c_str());
        rl_replace_line("", 0);
        rl_forced_update_display();
    };

    /* ---------- ana döngü ---------- */
    while (keep_running) {
        struct pollfd fds[2] = {
            { fd,           POLLIN, 0 },
            { STDIN_FILENO, POLLIN, 0 }
        };
        int rv = poll(fds,2,-1);
        if (rv<0){ if(errno==EINTR) continue; perror("poll"); break; }

        if (fds[0].revents & POLLIN) {          // seri portta veri
            char buf[256];
            int n = read(fd,buf,sizeof(buf));
            if (n>0){
                printf("\r\033[K\r\nRX[%d bytes]: ", n);
                for(int i=0;i<n;++i)
                    printf("0x%02X ", static_cast<uint8_t>(buf[i]));
                printf("\r\n");
                rl_set_prompt(dynamic_prompt.c_str());
                rl_replace_line("", 0);
                rl_forced_update_display();
            }
        }
        if (fds[1].revents & POLLIN) {          // klavye
            rl_callback_read_char();
            // Update prompt after each character
            if (g_dynamic_prompt && g_cfg && g_append_crlf) {
                const char* line = rl_line_buffer;
                std::string new_prompt;
                if ((*g_cfg)["mode"] == "hex") {
                    std::string s;
                    for(char c: std::string(line)) if(!std::isspace(c)) s.push_back(c);
                    size_t bytes = s.size() / 2;  // Each byte is 2 hex chars
                    new_prompt = std::string("[") + std::to_string(bytes) + "b] > ";
                } else {
                    size_t bytes = strlen(line);
                    if (*g_append_crlf) bytes += 2;  // Add 2 for \r\n if enabled
                    new_prompt = std::string("[") + std::to_string(bytes) + "b] > ";
                }
                *g_dynamic_prompt = new_prompt;
                printf("\r\033[K"); // Clear the current line
                rl_set_prompt(g_dynamic_prompt->c_str());
                rl_replace_line(line, 0);
                rl_forced_update_display();
            }
        }
    }

    /* temizlik */
    rl_callback_handler_remove();
    close(fd);
    write_history(histfile.c_str());  // Program kapanırken history'i kaydet
    std::cout<<"Disconnected.\n";
    return 0;
}
