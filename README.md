# pingm

Fast, multi-threaded TUI ICMP ping monitor written in C++.

## Requirements

* C++17 compiler (`g++`)
* `make`
* `ncurses` development headers (`libncurses-dev` on Debian/Ubuntu, `ncurses-devel` on RHEL/Fedora, `ncurses` on Arch)

## Installation

Build and install system-wide:

```
make
sudo make install
```

*(Note: `sudo make install` automatically configures `net.ipv4.ping_group_range` via sysctl so non-root users can run pingm).*

Uninstall:

```
sudo make uninstall
```

## Usage

Usage: 

```
$ pingm [-t : continuous] [-c count] [ip] [subnet/24] [example.com]
```

**Examples:**

```
# Ping multiple targets with default count (10)
pingm 8.8.8.8 1.1.1.1 192.168.1.0/24 github.com

# Ping 5 times and exit
pingm -c 5 192.168.1.1

# Continuous pinging
pingm -t 10.0.0.0/24
```

### Controls

* **Q**: Exit and display terminal summary
* **Up / Down / PgUp / PgDn**: Scroll target list
