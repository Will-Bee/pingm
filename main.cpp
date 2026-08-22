#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <chrono>
#include <regex>
#include <iomanip>
#include <cstdio>
#include <memory>
#include <array>
#include <arpa/inet.h>
#include <signal.h>
#include <cmath> 

struct HostState {
    std::string ip;
    int tries = 0;
    int successes = 0;
    double min_time = 999999.0;
    double max_time = 0.0;
    double total_time = 0.0;
    
    // Jitter variables
    double last_time = -1.0;   
    double total_jitter = 0.0; 
    int jitter_count = 0;      
    
    std::string status = "\033[33m[WAIT]\033[0m";
    bool done = false;
};

std::mutex state_mutex;
std::vector<HostState> hosts;
std::chrono::time_point<std::chrono::steady_clock> app_start_time;

// Print final summary stats
void print_summary() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - app_start_time).count();
    
    int up_count = 0;
    for (const auto& h : hosts) {
        if (h.status.find("-UP-") != std::string::npos) {
            up_count++;
        }
    }
    
    std::cout << "\033[?25h\n"; // Show cursor
    std::cout << "\n--- Pingm Stopped ---\n";
    std::cout << "Total runtime: " << elapsed << " seconds\n";
    std::cout << "\033[32m" << up_count << "/" << hosts.size() << " IP are UP\033[0m\n\n";
}

// Restores terminal cursor and prints runtime on exit (Ctrl + C)
void handle_sigint(int sig) {
    print_summary();
    exit(0);
}

// Executes a shell command and returns output
std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) return "";
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

// Expands CIDR (e.g. 192.168.1.0/24) into individual IPs
std::vector<std::string> expand_cidr(const std::string& cidr_str) {
    std::vector<std::string> ips;
    auto slash_pos = cidr_str.find('/');
    if (slash_pos == std::string::npos) {
        ips.push_back(cidr_str);
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

// Background worker thread for each IP
void ping_worker(int index, int max_pings, int total_hosts) {
    int stagger_ms = (1000 * index) / total_hosts;
    std::this_thread::sleep_for(std::chrono::milliseconds(stagger_ms));

    while (true) {
        auto next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (max_pings != 0 && hosts[index].tries >= max_pings) {
                hosts[index].done = true;
                break;
            }
        }

        std::string cmd = "ping -c 1 -W 1 " + hosts[index].ip + " 2>&1";
        std::string out = exec(cmd.c_str());

        bool is_up = false;
        double time_ms = 0.0;

        size_t pos = out.find("time=");
        if (pos != std::string::npos) {
            is_up = true;
            try {
                time_ms = std::stod(out.substr(pos + 5));
            } catch (...) {
                is_up = false; 
            }
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            hosts[index].tries++;

            if (is_up) {
                hosts[index].successes++;
                hosts[index].status = "\033[32m[-UP-]\033[0m";
                hosts[index].total_time += time_ms;
                if (time_ms < hosts[index].min_time) hosts[index].min_time = time_ms;
                if (time_ms > hosts[index].max_time) hosts[index].max_time = time_ms;

                if (hosts[index].last_time != -1.0) {
                    hosts[index].total_jitter += std::abs(time_ms - hosts[index].last_time);
                    hosts[index].jitter_count++;
                }
                hosts[index].last_time = time_ms;

            } else {
                hosts[index].status = "\033[31m[DOWN]\033[0m";
            }
        }

        std::this_thread::sleep_until(next_tick);
    }
}

int main(int argc, char* argv[]) {
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
            std::cout << "Usage: pingm [-t] [-c count] ip1 ip2 192.168.1.0/24...\n";
            return 0;
        } else {
            raw_args.push_back(arg);
        }
    }

    if (raw_args.empty()) {
        std::cout << "Usage: pingm [-t] [-c count] ip1 ip2...\n";
        return 1;
    }

    for (const auto& arg : raw_args) {
        std::vector<std::string> expanded = expand_cidr(arg);
        for (const auto& ip : expanded) {
            HostState state;
            state.ip = ip;
            hosts.push_back(state);
        }
    }

    if (hosts.empty()) {
        std::cout << "No valid IPs provided.\n";
        return 1;
    }

    int total_hosts = hosts.size();
    std::vector<std::thread> threads;
    for (size_t i = 0; i < hosts.size(); ++i) {
        threads.push_back(std::thread(ping_worker, i, max_pings, total_hosts));
    }

    std::cout << "\033[?25l"; // Hide cursor
    std::cout << "\033[2J";   // Clear screen

    // ANSI Colors
    const char* c_rst = "\033[0m";
    const char* c_dim = "\033[90m";
    const char* c_grn = "\033[32m";
    const char* c_yel = "\033[33m";
    const char* c_red = "\033[31m";
    const char* c_mag = "\033[35m";

    // Foreground UI Loop
    while (true) {
        std::cout << "\033[H"; // Move cursor to top left

        printf("%-15s | %-6s | %-5s | %-6s | %-4s | %-4s | %-4s | %-6s\n", "IP", "Status", "Tries", "Loss %", "Min", "Max", "Avg", "Jitter");
        printf("-----------------------------------------------------------------------------\n");

        bool all_done = true;

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            for (const auto& h : hosts) {
                if (!h.done) all_done = false;

                int loss = (h.tries > 0) ? ((h.tries - h.successes) * 100) / h.tries : 0;
                bool is_offline = (h.tries > 0 && h.successes == 0);

                // Default Colors
                const char *c_ip = c_rst, *c_sep = c_rst, *c_tri = c_rst, *c_los = c_rst;
                const char *c_min = c_rst, *c_max = c_rst, *c_avg = c_rst, *c_jit = c_rst;

                // Color Logic
                if (is_offline) {
                    c_ip = c_dim; c_sep = c_dim; c_tri = c_dim;
                    c_los = c_red; // Keep loss red to highlight the 100% failure
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
                    snprintf(buf, sizeof(buf), "%.0f", h.total_time / h.successes);
                    avg_str = buf;

                    if (h.min_time != 999999.0) {
                        snprintf(buf, sizeof(buf), "%.0f", h.min_time); min_str = buf;
                        snprintf(buf, sizeof(buf), "%.0f", h.max_time); max_str = buf;
                    } else {
                        min_str = "<1"; max_str = "<1";
                    }

                    if (h.jitter_count > 0) {
                        snprintf(buf, sizeof(buf), "%.0f", h.total_jitter / h.jitter_count);
                        jit_str = buf;
                    } else {
                        jit_str = "0"; 
                    }
                }

                // Complex print with inline color blocks
                printf("%s%-15s%s %s|%s %s %s|%s %s%5d%s %s|%s %s%5d%%%s %s|%s %s%4s%s %s|%s %s%4s%s %s|%s %s%4s%s %s|%s %s%6s%s\033[K\n",
                       c_ip, h.ip.c_str(), c_rst,
                       c_sep, c_rst,
                       h.status.c_str(),
                       c_sep, c_rst,
                       c_tri, h.tries, c_rst,
                       c_sep, c_rst,
                       c_los, loss, c_rst,
                       c_sep, c_rst,
                       c_min, min_str.c_str(), c_rst,
                       c_sep, c_rst,
                       c_max, max_str.c_str(), c_rst,
                       c_sep, c_rst,
                       c_avg, avg_str.c_str(), c_rst,
                       c_sep, c_rst,
                       c_jit, jit_str.c_str(), c_rst);
            }
        }

        if (all_done) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 10 FPS refresh
    }

    for (auto& t : threads) {
        t.join();
    }

    print_summary();
    return 0;
}
