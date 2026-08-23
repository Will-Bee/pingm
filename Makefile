CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread
LDLIBS = -lncurses
TARGET = pingm
PREFIX ?= /usr/local/bin

.PHONY: all clean install uninstall

all: $(TARGET)

$(TARGET): main.cpp
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDLIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	@echo "Installing pingm utility"
	install -d $(DESTDIR)$(PREFIX)
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/$(TARGET)
	-sysctl -w net.ipv4.ping_group_range="0 2147483647"
	-mkdir -p /etc/sysctl.d
	-echo 'net.ipv4.ping_group_range = 0 2147483647' > /etc/sysctl.d/99-pingm.conf
	@echo "Installation done. Usage: $ pingm [-t : continuous] [-c count] [ip] [subnet/24] [example.com]"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/$(TARGET)
	rm -f /etc/sysctl.d/99-pingm.conf
	@echo "Note: ping_group_range was left as-is in the active kernel to avoid breaking other apps."
