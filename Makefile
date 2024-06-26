
all: hdimanip hdiprint hdifuse hdifdisk

CXXFLAGS ?= -Wall -Wextra -O2 -flto -std=gnu++17

hdimanip: hdimanip.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

hdiprint: hdiprint.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

hdifuse: hdifuse.cpp fat12.cpp file.cpp util.cpp codepage.cpp ms932.cpp
	$(CXX) $(CXXFLAGS) -pthread -lfuse3 -I/usr/include/fuse3  $^ -o $@

hdifdisk: hdifdisk.cpp fat12.cpp util.cpp codepage.cpp ms932.cpp file.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

clean:
	rm -f hdifdisk hdifuse hdimanip hdiprint
