# pingm

A blazing-fast, multi-threaded ping utility written in C++ that brings an `mtr`-like real-time dashboard to your terminal. 

## Features

* **Asynchronous & Fast:** Pings multiple hosts simultaneously using native C++ threads.
* **Real-Time Dashboard:** 10 FPS UI refresh rate with a MTR like output
* **CIDR & Hostname Support:** Easily scan entire subnets (e.g., 192.168.1.0/24) and resolve domains (e.g., google.com).
* **Smart Color-Coding:** Instantly spot network degradation with threshold-based colors for latency, packet loss, and jitter.
* **Decluttered UI:** Offline and dead hosts are automatically dimmed to push them into the background.

---

## Installation

Ensure you have `g++` and `make` installed on your system. Keep `main.cpp` and the `Makefile` in the same directory, then run:

```
make
sudo make install
```

---

## Usage

```
pingm [-t] [-c count] ip1 ip2 192.168.1.0/24 example.com...
```

### Options
* `-t` : Ping forever until interrupted (Ctrl+C).
* `-c [count]` : Stop after sending exactly `count` pings (default is 10).

### Examples

Ping multiple specific targets using the default 10 tries:
```
pingm 8.8.8.8 1.1.1.1 example.com
```

Ping a full subnet continuously:
```
pingm -t 192.168.1.0/24
```
