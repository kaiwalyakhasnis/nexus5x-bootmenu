# Static aarch64 build with a stock Linux cross compiler and glibc
# (Debian/Ubuntu: apt install g++-aarch64-linux-gnu), like hike-buddy.
CROSS    ?= aarch64-linux-gnu-
OUT      ?= ../out/menu
CXX      := $(CROSS)g++

CXXFLAGS := -O2 -std=c++14 -Wall -fno-exceptions -Iminui/include -Iminui -DOVERSCAN_PERCENT=0
LDFLAGS  := -static -s

SRCS := bootmenu.cpp minui/graphics.cpp minui/graphics_fbdev.cpp minui/events.cpp
OBJS := $(SRCS:%.cpp=$(OUT)/%.o)

$(OUT)/bootmenu: $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $(OBJS)

$(OUT)/%.o: %.cpp $(wildcard minui/*.h minui/include/minui/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(OUT)
.PHONY: clean
