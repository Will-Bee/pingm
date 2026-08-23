#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <iomanip>
#include <cstdio>
#include <memory>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <atomic>
#include <algorithm>
#include <stdexcept>
#include <ncurses.h>
#include <endian.h>

struct HostState {
    std::string ip;
    int tries = 0;
    int successes = 0;
    double min_time = 999999.0;
    double max_time = 0.0;
    double total_time = 0.0;

    double last_time = -1.0;
    double jitter = 0.0;

    bool ever_received = false;
    std::chrono::steady_clock::time_point last_recv;
    std::chrono::steady_clock::time_point last_sent; // Added to track in-flight packets
    uint16_t last_seq_received = 0;

    std::string status = "WAIT";
    struct sockaddr_in addr;
};

struct PingPayload {
    uint64_t timestamp_ns;
};

struct FullPacket {
    struct icmphdr icmp;
    PingPayload payload;
};

std::mutex state_mutex;
std::vector<HostState> hosts;
std::unordered_map<uint32_t, int> ip_to_index;
std::chrono::time_point<std::chrono::steady_clock> app_start_time;
std::atomic<bool> keep_running{true};
std::atomic<int> hosts_completed{0};

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

std::vector<std::string> expand_cidr(const std::string& cidr_str) {
    std::vector<std::string> ips;
    auto slash_pos = cidr_str.find('/');
    if (slash_pos == std::string::npos) {
        struct in_addr sa;
        if (inet_pton(AF_INET, cidr_str.c_str(), &sa) == 1) {
            ips.push_back(cidr_str);
        } else {
            std::string resolved = resolve_hostname(cidr_str);
            if (!resolved.empty()) ips.push_back(resolved);
            else std::cerr << "Warning: Could not resolve '" << cidr_str << "'\n";
        }
        return ips;
    }
    std::string ip = cidr_str.substr(0, slash_pos);
    int prefix;
    try {
        prefix = std::stoi(cidr_str.substr(slash_pos + 1));
    } catch (const std::exception& e) {
        std::cerr << "Error: Invalid CIDR prefix in '" << cidr_str << "'\n";
        return ips;
    }

    if (prefix < 0 || prefix > 32) {
        std::cerr << "Error: Invalid prefix length in '" << cidr_str << "'\n";
        return ips;
    }
    if (prefix == 0) {
        std::cerr << "Error: /0 prefix is not supported.\n";
        return ips;
    }

    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) return ips;

    uint32_t ip_net = ntohl(addr.s_addr);
    uint32_t mask = (0xFFFFFFFF << (32 - prefix)) & 0xFFFFFFFF;
    uint32_t network = ip_net & mask;
    uint32_t broadcast = network | ~mask;

    if (prefix == 32) {
        ips.push_back(ip);
    } else if (prefix == 31) {
        for (uint32_t i = network; i <= broadcast; ++i) {
            struct in_addr current;
            current.s_addr = htonl(i);
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &current, buf, INET_ADDRSTRLEN);
            ips.push_back(std::string(buf));
        }
    } else {
        for (uint32_t i = network + 1; i < broadcast; ++i) {
            struct in_addr current;
            current.s_addr = htonl(i);
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &current, buf, INET_ADDRSTRLEN);
            ips.push_back(std::string(buf));
        }
    }
    return ips;
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

        // Mask in-flight packet from loss calculation
        int effective_tries = h.tries;
        if (effective_tries > 0 && effective_tries > h.successes) {
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - h.last_sent).count() < 1000) {
                effective_tries--;
            }
        }

        int loss = (effective_tries > 0) ? ((effective_tries - h.successes) * 100) / effective_tries : 0;
        if (loss < 0) loss = 0;

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

                if (h.jitter < 5.0) c_jit = c_grn;
                else if (h.jitter < 15.0) c_jit = c_yel;
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

            if (h.successes > 1) {
                snprintf(buf, sizeof(buf), "%.0f", h.jitter); jit_str = buf;
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

void handle_sigint(int) { keep_running = false; }

void sender_thread(int sockfd, int max_pings) {
    uint16_t process_id = getpid() & 0xFFFF;
    int total_hosts = hosts.size();

    int batch_size = std::max(1, total_hosts / 1000);
    int sleep_ms = (total_hosts > 1000) ? 1 : std::max(1, 1000 / total_hosts);

    while (keep_running) {
        int sent_in_batch = 0;

        for (int i = 0; i < total_hosts; ++i) {
            if (!keep_running) break;

            bool should_send = true;
            {
                std::lock_guard<std::mutex> lock(state_mutex);
                if (max_pings != 0 && hosts[i].tries >= max_pings) {
                    should_send = false;
                }
            }

            if (!should_send) {
                sent_in_batch++;
                if (sent_in_batch >= batch_size) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    sent_in_batch = 0;
                }
                continue;
            }

            FullPacket packet;
            memset(&packet, 0, sizeof(packet));
            packet.icmp.type = ICMP_ECHO;
            packet.icmp.code = 0;
            packet.icmp.un.echo.id = htons(process_id);
            packet.icmp.un.echo.sequence = htons(hosts[i].tries + 1);
            packet.icmp.checksum = 0;

            uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            packet.payload.timestamp_ns = htobe64(ts);

            sendto(sockfd, &packet, sizeof(packet), 0, (struct sockaddr*)&hosts[i].addr, sizeof(hosts[i].addr));

            {
                std::lock_guard<std::mutex> lock(state_mutex);

                hosts[i].last_sent = std::chrono::steady_clock::now(); // Track in-flight timestamp
                hosts[i].tries++;
                if (max_pings != 0 && hosts[i].tries == max_pings) {
                    hosts_completed++;
                }

                auto now = std::chrono::steady_clock::now();
                if (hosts[i].tries > 1 && (!hosts[i].ever_received || std::chrono::duration_cast<std::chrono::seconds>(now - hosts[i].last_recv).count() >= 2)) {
                    hosts[i].status = "DOWN";
                }
            }

            sent_in_batch++;
            if (sent_in_batch >= batch_size) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                sent_in_batch = 0;
            }
        }

        if (max_pings != 0 && hosts_completed >= total_hosts) {
            break;
        }

        if (sent_in_batch > 0) {
             std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
    }
}

void receiver_thread(int sockfd) {
    struct pollfd pfd;
    pfd.fd = sockfd;
    pfd.events = POLLIN;

    alignas(struct icmphdr) char recv_packet[1024];

    while (keep_running) {
        if (poll(&pfd, 1, 100) > 0) {
            if (pfd.revents & POLLIN) {
                struct sockaddr_in r_addr;
                socklen_t addr_len = sizeof(r_addr);
                int bytes = recvfrom(sockfd, recv_packet, sizeof(recv_packet), 0, (struct sockaddr*)&r_addr, &addr_len);

                if (bytes > 0) {
                    struct icmphdr *icmph = (struct icmphdr *)recv_packet;

                    if (bytes >= (int)(sizeof(struct icmphdr) + sizeof(PingPayload)) &&
                        icmph->type == ICMP_ECHOREPLY) {

                        uint16_t incoming_seq = ntohs(icmph->un.echo.sequence);
                        PingPayload payload;
                        memcpy(&payload, recv_packet + sizeof(struct icmphdr), sizeof(payload));

                        uint64_t ts_host = be64toh(payload.timestamp_ns);
                        auto sent_time = std::chrono::time_point<std::chrono::steady_clock>(
                            std::chrono::nanoseconds(ts_host));
                        auto recv_time = std::chrono::steady_clock::now();
                        double time_ms = std::chrono::duration<double, std::milli>(recv_time - sent_time).count();

                        uint32_t sender_ip = r_addr.sin_addr.s_addr;

                        auto it = ip_to_index.find(sender_ip);
                        if (it != ip_to_index.end()) {
                            int idx = it->second;
                            std::lock_guard<std::mutex> lock(state_mutex);

                            hosts[idx].ever_received = true;
                            hosts[idx].status = "-UP-";
                            hosts[idx].last_recv = recv_time;
                            hosts[idx].total_time += time_ms;

                            hosts[idx].successes++;
                            int16_t seq_diff = (int16_t)(incoming_seq - hosts[idx].last_seq_received);
                            if (seq_diff > 0 || hosts[idx].successes == 1) {
                                hosts[idx].last_seq_received = incoming_seq;
                            }

                            if (time_ms < hosts[idx].min_time) hosts[idx].min_time = time_ms;
                            if (time_ms > hosts[idx].max_time) hosts[idx].max_time = time_ms;

                            if (hosts[idx].last_time != -1.0) {
                             // double diff = std::abs(time_ms - hosts[idx].last_time);
                                double diff = std::fabs(time_ms - hosts[idx].last_time);
                                hosts[idx].jitter += (diff - hosts[idx].jitter) / 16.0;
                            }
                            hosts[idx].last_time = time_ms;
                        }
                    }
                }
            }
        }
    }
}

int main(int argc, char* argv[]) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (sockfd < 0) {
        std::cerr << "Error: Socket creation failed.\n";
        std::cerr << "Fix: Run 'sudo sysctl -w net.ipv4.ping_group_range=\"0 2147483647\"' to enable unprivileged pings.\n";
        return 1;
    }

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
            try {
                max_pings = std::stoi(argv[++i]);
            } catch (...) {
                std::cerr << "Error: -c requires a numeric argument.\n";
                close(sockfd);
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: pingm [-t] [-c count] ip1 ip2 192.168.1.0/24...\n";
            close(sockfd);
            return 0;
        } else {
            raw_args.push_back(arg);
        }
    }

    if (raw_args.empty()) {
        std::cout << "Usage: pingm [-t] [-c count] ip1 ip2...\n";
        close(sockfd);
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

            uint32_t ip_num = state.addr.sin_addr.s_addr;
            if (ip_to_index.find(ip_num) == ip_to_index.end()) {
                ip_to_index[ip_num] = hosts.size();
                hosts.push_back(state);
            }
        }
    }

    if (hosts.empty()) {
        std::cout << "No valid IPs or hostnames provided.\n";
        close(sockfd);
        return 1;
    }

    std::thread sender(sender_thread, sockfd, max_pings);
    std::thread receiver(receiver_thread, sockfd);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);

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
    auto shutdown_start = std::chrono::steady_clock::time_point::min();

    while (keep_running) {
        int ch = getch();
        if (ch == 'q' || ch == 'Q') break;

        int max_y, max_x;
        getmaxyx(stdscr, max_y, max_x);
        int max_visible = std::max(0, max_y - 4);

        if (ch == KEY_UP && scroll_y > 0) scroll_y--;
        if (ch == KEY_DOWN) scroll_y++;
        if (ch == KEY_PPAGE) scroll_y -= max_visible;
        if (ch == KEY_NPAGE) scroll_y += max_visible;

        int max_scroll = std::max(0, (int)hosts.size() - max_visible);
        scroll_y = std::clamp(scroll_y, 0, max_scroll);

        std::vector<HostState> display_hosts;
        bool all_done = (max_pings != 0 && hosts_completed >= (int)hosts.size());

        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(state_mutex);

            size_t start_idx = scroll_y;
            size_t end_idx = std::min(hosts.size(), (size_t)(scroll_y + max_visible));

            display_hosts.reserve(end_idx - start_idx);
            for (size_t i = start_idx; i < end_idx; ++i) {
                display_hosts.push_back(hosts[i]);
            }
        }

        erase();

        attron(A_BOLD);
        mvprintw(0, 0, "%-15s | %-6s | %-5s | %-6s | %-4s | %-4s | %-4s | %-6s", "IP", "Status", "Tries", "Loss %", "Min", "Max", "Avg", "Jitter");
        mvprintw(1, 0, "-----------------------------------------------------------------------------");
        attroff(A_BOLD);

        int row = 2;

        for (size_t i = 0; i < display_hosts.size() && row < max_y - 2; ++i) {
            const auto& h = display_hosts[i];

            // Mask in-flight packet from UI loss calculation
            int effective_tries = h.tries;
            if (effective_tries > 0 && effective_tries > h.successes) {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - h.last_sent).count() < 1000) {
                    effective_tries--;
                }
            }

            int loss = (effective_tries > 0) ? ((effective_tries - h.successes) * 100) / effective_tries : 0;
            if (loss < 0) loss = 0;

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

                    if (h.jitter < 5.0) c_jit = C_GRN;
                    else if (h.jitter < 15.0) c_jit = C_YEL;
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

                if (h.successes > 1) {
                    snprintf(buf, sizeof(buf), "%.0f", h.jitter); jit_str = buf;
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

        clrtobot();

        attron(A_BOLD);
        mvprintw(max_y - 1, 0, "[UP/DOWN/PGUP/PGDN] Scroll  |  [Q] Quit  |  Mode: %s", (max_pings == 0 ? "Continuous" : ("Max " + std::to_string(max_pings) + " Pings").c_str()));
        attroff(A_BOLD);

        refresh();

        if (max_pings != 0 && all_done) {
            if (shutdown_start == std::chrono::steady_clock::time_point::min()) {
                shutdown_start = std::chrono::steady_clock::now();
            } else if (std::chrono::steady_clock::now() - shutdown_start > std::chrono::seconds(2)) {
                break;
            }
        }
    }

    keep_running = false;
    if (sender.joinable()) sender.join();
    if (receiver.joinable()) receiver.join();

    endwin();

    {
        std::lock_guard<std::mutex> lock(state_mutex);
        print_summary();
    }

    close(sockfd);

    return 0;
}
