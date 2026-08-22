#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <memory>
#include <array>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <signal.h>
#include <fcntl.h>
#include <atomic>
#include <ncurses.h>

struct HostState {
    std::string ip;
    int tries = 0;
    int successes = 0;
    double min_time = 999999.0;
    double max_time = 0.0;
    double total_time = 0.0;

    double last_time = -1.0;
    double total_jitter = 0.0;
    int jitter_count = 0;

    std::string status = "WAIT";
    bool done = false;
    struct sockaddr_in addr;
};

std::mutex state_mutex;
std::vector<HostState> hosts;
std::chrono::time_point<std::chrono::steady_clock> app_start_time;
std::atomic<bool> keep_running{true};

#define C_GRN 1
#define C_YEL 2
#define C_RED 3
#define C_MAG 4
#define C_DEF 5

std::string resolve_hostname(const std::string& host) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res != nullptr) {
        char ip_str[INET_ADDRSTRLEN];
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)res->ai_addr;
        inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);
        freeaddrinfo(res);
        return std::string(ip_str);
    }
    return "";
}

unsigned short calculate_checksum(void *b, int len) {
    unsigned short *buf = (unsigned short *)b;
    unsigned int sum = 0;
    unsigned short result;
    for (sum = 0; len > 1; len -= 2) sum += *buf++;
    if (len == 1) sum += *(unsigned char *)buf;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

void print_summary() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - app_start_time).count();

    const char* c_rst = "\033[0m";
    const char* c_dim = "\033[90m";
    const char* c_grn = "\033[32m";
    const char* c_yel = "\033[33m";
    const char* c_red = "\033[31m";
    const char* c_mag = "\033[35m";

    std::cout << "\n";
    printf("%-15s | %-6s | %-5s | %-6s | %-4s | %-4s | %-4s | %-6s\n", "IP", "Status", "Tries", "Loss %", "Min", "Max", "Avg", "Jitter");
    printf("-----------------------------------------------------------------------------\n");

    int up_count = 0;
    for (const auto& h : hosts) {
        if (h.status == "-UP-") up_count++;

        int loss = (h.tries > 0) ? ((h.tries - h.successes) * 100) / h.tries : 0;
        bool is_offline = (h.tries > 0 && h.successes == 0);

        const char *c_ip = c_rst, *c_sep = c_rst, *c_tri = c_rst, *c_los = c_rst;
        const char *c_min = c_rst, *c_max = c_rst, *c_avg = c_rst, *c_jit = c_rst;
        const char *c_stat = c_rst;

        if (h.status == "-UP-") c_stat = c_grn;
        else if (h.status == "DOWN") c_stat = c_red;
        else c_stat = c_yel;

        if (is_offline) {
            c_ip = c_dim; c_sep = c_dim; c_tri = c_dim;
            c_los = c_red;
            c_min = c_dim; c_max = c_dim; c_avg = c_dim; c_jit = c_dim;
        } else {
            if (loss == 0) c_los = c_rst;
            else if (loss <= 15) c_los = c_yel;
            else if (loss < 100) c_los = c_mag;
            else c_los = c_red;

            auto lat_col = [&](double val) {
                if (val < 50.0) return c_grn;
                if (val < 150.0) return c_yel;
                return c_red;
            };

            if (h.successes > 0) {
                c_avg = lat_col(h.total_time / h.successes);
                if (h.min_time != 999999.0) {
                    c_min = lat_col(h.min_time);
                    c_max = lat_col(h.max_time);
                }

                double jit = h.jitter_count > 0 ? (h.total_jitter / h.jitter_count) : 0;
                if (jit < 5.0) c_jit = c_grn;
                else if (jit < 15.0) c_jit = c_yel;
                else c_jit = c_red;
            }
        }

        std::string avg_str = "N/a", min_str = "N/a", max_str = "N/a", jit_str = "N/a";
        if (h.successes > 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0f", h.total_time / h.successes); avg_str = buf;

            if (h.min_time != 999999.0) {
                snprintf(buf, sizeof(buf), "%.0f", h.min_time); min_str = buf;
                snprintf(buf, sizeof(buf), "%.0f", h.max_time); max_str = buf;
            } else {
                min_str = "<1"; max_str = "<1";
            }

            if (h.jitter_count > 0) {
                snprintf(buf, sizeof(buf), "%.0f", h.total_jitter / h.jitter_count); jit_str = buf;
            } else {
                jit_str = "0";
            }
        }

        printf("%s%-15s%s %s|%s %s[%-4s]%s %s|%s %s%5d%s %s|%s %s%5d%%%s %s|%s %s%4s%s %s|%s %s%4s%s %s|%s %s%4s%s %s|%s %s%6s%s\n",
               c_ip, h.ip.c_str(), c_rst, c_sep, c_rst,
               c_stat, h.status.c_str(), c_rst, c_sep, c_rst,
               c_tri, h.tries, c_rst, c_sep, c_rst,
               c_los, loss, c_rst, c_sep, c_rst,
               c_min, min_str.c_str(), c_rst, c_sep, c_rst,
               c_max, max_str.c_str(), c_rst, c_sep, c_rst,
               c_avg, avg_str.c_str(), c_rst, c_sep, c_rst,
               c_jit, jit_str.c_str(), c_rst);
    }

    std::cout << "\n--- Pingm Stopped ---\n";
    std::cout << "Total runtime: " << elapsed << " seconds\n";
    std::cout << "\033[32m" << up_count << "/" << hosts.size() << " IP(s) are UP\033[0m\n\n";
}

void handle_sigint(int sig) {
    keep_running = false;
}

std::vector<std::string> expand_cidr(const std::string& cidr_str) {
    std::vector<std::string> ips;
    auto slash_pos = cidr_str.find('/');
    if (slash_pos == std::string::npos) {
        struct in_addr sa;
        if (inet_pton(AF_INET, cidr_str.c_str(), &sa) == 1) {
            ips.push_back(cidr_str);
        } else {
            std::string resolved = resolve_hostname(cidr_str);
            if (!resolved.empty()) {
                ips.push_back(resolved);
            } else {
                std::cerr << "Warning: Could not resolve hostname '" << cidr_str << "'\n";
            }
        }
        return ips;
    }

    std::string ip = cidr_str.substr(0, slash_pos);
    int prefix = std::stoi(cidr_str.substr(slash_pos + 1));
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) return ips;

    uint32_t ip_net = ntohl(addr.s_addr);
    uint32_t mask = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF;
    uint32_t network = ip_net & mask;
    uint32_t broadcast = network | ~mask;

    for (uint32_t i = network + 1; i < broadcast; ++i) {
        struct in_addr current;
        current.s_addr = htonl(i);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &current, buf, INET_ADDRSTRLEN);
        ips.push_back(std::string(buf));
    }
    if (prefix == 32) ips.push_back(ip);
    return ips;
}

void ping_worker(int index, int max_pings, int total_hosts, uint16_t pid_id) {
    int stagger_ms = (1000 * index) / total_hosts;
    std::this_thread::sleep_for(std::chrono::milliseconds(stagger_ms));

    int sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (sockfd < 0) return;

    fcntl(sockfd, F_SETFL, O_NONBLOCK);

    uint16_t seq = 1;
    char send_packet[64];
    char recv_packet[1024];

    while (keep_running) {
        auto next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (max_pings != 0 && hosts[index].tries >= max_pings) {
                hosts[index].done = true;
                break;
            }
        }

        memset(send_packet, 0, sizeof(send_packet));
        struct icmphdr *icmp = (struct icmphdr *)send_packet;
        icmp->type = ICMP_ECHO;
        icmp->code = 0;
        icmp->un.echo.id = htons(pid_id + index);
        icmp->un.echo.sequence = htons(seq++);
        icmp->checksum = calculate_checksum(send_packet, sizeof(send_packet));

        auto send_time = std::chrono::steady_clock::now();
        bool is_up = false;
        double time_ms = 0.0;

        if (sendto(sockfd, send_packet, sizeof(send_packet), 0, (struct sockaddr*)&hosts[index].addr, sizeof(hosts[index].addr)) > 0) {
            struct sockaddr_in r_addr;
            socklen_t addr_len = sizeof(r_addr);
            auto listen_timeout = std::chrono::steady_clock::now() + std::chrono::seconds(1);

            while (std::chrono::steady_clock::now() < listen_timeout && keep_running) {
                int bytes = recvfrom(sockfd, recv_packet, sizeof(recv_packet), 0, (struct sockaddr*)&r_addr, &addr_len);

                if (bytes > 0) {
                    struct iphdr *ip_hdr = (struct iphdr *)recv_packet;
                    struct icmphdr *icmph = (struct icmphdr *)(recv_packet + (ip_hdr->ihl * 4));

                    if (icmph->type == ICMP_ECHOREPLY &&
                        icmph->un.echo.id == htons(pid_id + index) &&
                        r_addr.sin_addr.s_addr == hosts[index].addr.sin_addr.s_addr) {

                        auto recv_time = std::chrono::steady_clock::now();
                        time_ms = std::chrono::duration<double, std::milli>(recv_time - send_time).count();
                        is_up = true;
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            hosts[index].tries++;

            if (is_up) {
                hosts[index].successes++;
                hosts[index].status = "-UP-";
                hosts[index].total_time += time_ms;
                if (time_ms < hosts[index].min_time) hosts[index].min_time = time_ms;
                if (time_ms > hosts[index].max_time) hosts[index].max_time = time_ms;

                if (hosts[index].last_time != -1.0) {
                    hosts[index].total_jitter += std::abs(time_ms - hosts[index].last_time);
                    hosts[index].jitter_count++;
                }
                hosts[index].last_time = time_ms;

            } else {
                hosts[index].status = "DOWN";
            }
        }

        std::this_thread::sleep_until(next_tick);
    }
    close(sockfd);
}

int main(int argc, char* argv[]) {
    int test_sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (test_sock < 0) {
        std::cerr << "Error: Raw socket creation failed. Please run with sudo or apply setcap.\n";
        return 1;
    }
    close(test_sock);

    app_start_time = std::chrono::steady_clock::now();
    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    int max_pings = 10;
    std::vector<std::string> raw_args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-t") {
            max_pings = 0;
        } else if (arg == "-c" && i + 1 < argc) {
            max_pings = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: sudo pingm [-t] [-c count] ip1 ip2 192.168.1.0/24...\n";
            return 0;
        } else {
            raw_args.push_back(arg);
        }
    }

    if (raw_args.empty()) {
        std::cout << "Usage: sudo pingm [-t] [-c count] ip1 ip2...\n";
        return 1;
    }

    for (const auto& arg : raw_args) {
        std::vector<std::string> expanded = expand_cidr(arg);
        for (const auto& ip : expanded) {
            HostState state;
            state.ip = ip;
            memset(&state.addr, 0, sizeof(state.addr));
            state.addr.sin_family = AF_INET;
            inet_pton(AF_INET, ip.c_str(), &state.addr.sin_addr);
            hosts.push_back(state);
        }
    }

    if (hosts.empty()) {
        std::cout << "No valid IPs or hostnames provided.\n";
        return 1;
    }

    int total_hosts = hosts.size();
    uint16_t pid_id = getpid() & 0xFFFF;

    std::vector<std::thread> threads;
    for (size_t i = 0; i < hosts.size(); ++i) {
        threads.push_back(std::thread(ping_worker, i, max_pings, total_hosts, pid_id));
    }

    // Initialize ncurses
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);      // Hide cursor
    timeout(100);     // Non-blocking getch() timeout (100ms)

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_GRN, COLOR_GREEN, -1);
        init_pair(C_YEL, COLOR_YELLOW, -1);
        init_pair(C_RED, COLOR_RED, -1);
        init_pair(C_MAG, COLOR_MAGENTA, -1);
        init_pair(C_DEF, COLOR_WHITE, -1);
    }

    int scroll_y = 0;

    while (keep_running) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;

        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        int max_visible = max_y - 4; // Reserve lines for header and footer

        // Scrolling interactions
        if (ch == KEY_UP && scroll_y > 0) scroll_y--;
        if (ch == KEY_DOWN && scroll_y + max_visible < (int)hosts.size()) scroll_y++;
        if (ch == KEY_PPAGE) scroll_y = std::max(0, scroll_y - max_visible);
        if (ch == KEY_NPAGE) scroll_y = std::min((int)hosts.size() - max_visible, scroll_y + max_visible);

        erase();

        // Draw Header
        attron(A_BOLD);
        mvprintw(0, 0, "%-15s | %-6s | %-5s | %-6s | %-4s | %-4s | %-4s | %-6s", "IP", "Status", "Tries", "Loss %", "Min", "Max", "Avg", "Jitter");
        mvprintw(1, 0, "-----------------------------------------------------------------------------");
        attroff(A_BOLD);

        bool all_done = true;
        int row = 2;

        {
            std::lock_guard<std::mutex> lock(state_mutex);

            // Check if all threads completed their runs
            for (const auto& h : hosts) {
                if (!h.done) { all_done = false; break; }
            }

            // Draw Visible Window
            for (size_t i = scroll_y; i < hosts.size() && row < max_y - 2; ++i) {
                const auto& h = hosts[i];
                int loss = (h.tries > 0) ? ((h.tries - h.successes) * 100) / h.tries : 0;
                bool is_offline = (h.tries > 0 && h.successes == 0);

                int c_los = C_DEF, c_min = C_DEF, c_max = C_DEF, c_avg = C_DEF, c_jit = C_DEF;
                int c_stat = (h.status == "-UP-") ? C_GRN : ((h.status == "DOWN") ? C_RED : C_YEL);

                if (is_offline) {
                    c_los = C_RED;
                } else {
                    if (loss == 0) c_los = C_DEF;
                    else if (loss <= 15) c_los = C_YEL;
                    else if (loss < 100) c_los = C_MAG;
                    else c_los = C_RED;

                    auto lat_col = [&](double val) {
                        if (val < 50.0) return C_GRN;
                        if (val < 150.0) return C_YEL;
                        return C_RED;
                    };

                    if (h.successes > 0) {
                        c_avg = lat_col(h.total_time / h.successes);
                        if (h.min_time != 999999.0) {
                            c_min = lat_col(h.min_time);
                            c_max = lat_col(h.max_time);
                        }

                        double jit = h.jitter_count > 0 ? (h.total_jitter / h.jitter_count) : 0;
                        if (jit < 5.0) c_jit = C_GRN;
                        else if (jit < 15.0) c_jit = C_YEL;
                        else c_jit = C_RED;
                    }
                }

                std::string avg_str = "N/a", min_str = "N/a", max_str = "N/a", jit_str = "N/a";
                if (h.successes > 0) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%.0f", h.total_time / h.successes); avg_str = buf;

                    if (h.min_time != 999999.0) {
                        snprintf(buf, sizeof(buf), "%.0f", h.min_time); min_str = buf;
                        snprintf(buf, sizeof(buf), "%.0f", h.max_time); max_str = buf;
                    } else {
                        min_str = "<1"; max_str = "<1";
                    }

                    if (h.jitter_count > 0) {
                        snprintf(buf, sizeof(buf), "%.0f", h.total_jitter / h.jitter_count); jit_str = buf;
                    } else {
                        jit_str = "0";
                    }
                }

                if (is_offline) attron(A_DIM);
                
                mvprintw(row, 0, "%-15s | ", h.ip.c_str());
                
                attron(COLOR_PAIR(c_stat));
                printw("%-6s", h.status.c_str());
                attroff(COLOR_PAIR(c_stat));
                
                printw(" | %-5d | ", h.tries);
                
                attron(COLOR_PAIR(c_los));
                std::string loss_str = std::to_string(loss) + "%";
                printw("%-6s", loss_str.c_str());
                attroff(COLOR_PAIR(c_los));
                
                printw(" | ");
                
                attron(COLOR_PAIR(c_min));
                printw("%-4s", min_str.c_str());
                attroff(COLOR_PAIR(c_min));
                
                printw(" | ");
                
                attron(COLOR_PAIR(c_max));
                printw("%-4s", max_str.c_str());
                attroff(COLOR_PAIR(c_max));
                
                printw(" | ");
                
                attron(COLOR_PAIR(c_avg));
                printw("%-4s", avg_str.c_str());
                attroff(COLOR_PAIR(c_avg));
                
                printw(" | ");
                
                attron(COLOR_PAIR(c_jit));
                printw("%-6s", jit_str.c_str());
                attroff(COLOR_PAIR(c_jit));

                if (is_offline) attroff(A_DIM);

                row++;
            }
        } // state_mutex releases here

        // Draw Footer
        attron(A_BOLD);
        mvprintw(max_y - 1, 0, "[UP/DOWN/PGUP/PGDN] Scroll  |  [Q] Quit  |  Mode: %s", (max_pings == 0 ? "Continuous" : ("Max " + std::to_string(max_pings) + " Pings").c_str()));
        attroff(A_BOLD);

        refresh();

        if (max_pings != 0 && all_done) {
            break;
        }
    }

    // Stop threads and wait for them to finish
    keep_running = false;
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    // Safely exit TUI mode
    endwin();

    // Print final static results
    print_summary();

    return 0;
}
