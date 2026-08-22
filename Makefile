CXX = g++
CXXFLAGS = -std=c++11 -Wall -pthread -O2
TARGET = pingm
PREFIX = /usr/local/bin

all:
	$(CXX) $(CXXFLAGS) main.cpp -o $(TARGET)

clean:
	rm -f $(TARGET)

install: all
	install -m 755 $(TARGET) $(PREFIX)/$(TARGET)
	setcap cap_net_raw+ep $(PREFIX)/$(TARGET)

uninstall:
	rm -f $(PREFIX)/$(TARGET)
