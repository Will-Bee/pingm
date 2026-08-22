# pingm - Multi-threaded TUI Ping Monitor

`pingm` is a fast, multi-threaded ICMP ping monitoring tool written in C++. It features a flicker-free, scrollable terminal UI (powered by `ncurses`), CIDR expansion (e.g., `192.168.1.0/24`), DNS resolution, and real-time statistics including jitter, packet loss, and latency metrics. When the application closes, it prints a final persistent summary to your terminal.

## Features

* **Scrollable TUI:** Clean, flicker-free interface using `ncurses`. Handles hundreds of IPs gracefully.
* **CIDR Support:** Automatically expands subnets like `10.0.0.0/24`.
* **Multi-threaded:** Pings all targets concurrently without blocking.
* **Smart Metrics:** Tracks Minimum, Maximum, Average latency, and Jitter.
* **Persistent Summary:** Outputs the final state to standard output upon exiting.

## Prerequisites

To build `pingm`, you need a C++ compiler (`g++`), `make`, and the **ncurses development library**.

Install the required library for your distribution:

**Debian / Ubuntu / Linux Mint:**
```bash
sudo apt update
sudo apt install g++ make libncurses-dev
```

**Fedora / RHEL / Rocky Linux:**
```bash
sudo dnf install gcc-c++ make ncurses-devel
```

**Arch Linux / Manjaro:**
```bash
sudo pacman -S gcc make ncurses
```

**Alpine Linux:**
```bash
sudo apk add g++ make ncurses-dev
```

## Building & Installation

1. Compile the program:
```bash
make
```

2. Install it system-wide (requires root):
```bash
sudo make install
```
*(Note: `make install` automatically applies `setcap cap_net_raw+ep` so you can run `pingm` without sudo in the future.)*

## Usage

You can pass individual IP addresses, hostnames, or full CIDR ranges. 

**Basic Usage:**
```bash
pingm 8.8.8.8 github.com 192.168.1.0/24
```

**Ping a specific number of times (e.g., 5 pings):**
```bash
pingm -c 5 1.1.1.1 1.0.0.1
```

**Ping indefinitely (until you press 'q'):**
```bash
pingm -t 192.168.1.0/24
```

## Controls

While the UI is running:
* **UP / DOWN Arrows:** Scroll the list up and down one row.
* **Page Up / Page Down:** Scroll the list by a full page.
* **q or Q:** Safely exit the monitor and print the final results.

## Uninstallation

To remove the binary from your system:
```bash
sudo make uninstall
```
