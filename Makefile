
all: hdimanip hdiprint hdifuse

CXXFLAGS ?= -Wall -Wextra -O2 -flto -std=gnu++17

hdimanip: hdimanip.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

hdiprint: hdiprint.cpp
	$(CXX) $(CXXFLAGS) $^ -o $@

hdifuse: hdifuse.cpp fat12.cpp file.cpp util.cpp codepage.cpp ms932.cpp
	$(CXX) $(CXXFLAGS) -pthread -lfuse3 -I/usr/include/fuse3  $^ -o $@

clean:
	rm -f hdiimgmanip hdifat12print hdifuse
