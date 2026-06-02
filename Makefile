# Makefile for Lniux Fleet Telemtery & eBPF monitoring system
CXX = g++
CXXFLAGS = -Wall -O2 -std=c++17 -pthread
CLANG = clang
BPFTOOL = bpftool

# Detect operating system
UNAME_S := $(shell uname -s)

# Target executable
TARGET = telemetry-agent
BPF_OBJ = src/disk_io.bpf.o
BPF_SKEL = src/disk_io.skel.h

# macOS Compilation Settings
ifeq ($(UNAME_S), Darwin)
    CXXFLAGS += -DDISABLE_BPF
    SOURCES = src/agent.cpp
    OBJECTS = $(SOURCES:.cpp=.o)
    
all: $(TARGET)
	@echo "[BUILD] Successfully compiled simulation-only agent on macOS."

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp src/agent.h src/disk_io.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) src/*.o

# Linux Compilation Settings (Supports real eBPF & Simulation)
else
    # Check if libbpf development libraries are installed
    LIBBPF_EXISTS := $(shell ldconfig -p | grep libbpf || echo "")
    
    # If libbpf isn't available, build in simulation-only mode
    ifeq ($(LIBBPF_EXISTS),)
        CXXFLAGS += -DDISABLE_BPF
        SOURCES = src/agent.cpp
        OBJECTS = $(SOURCES:.cpp=.o)
        
all: $(TARGET)
	@echo "[WARNING] libbpf not found. Built agent in simulation-only mode."

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp src/agent.h src/disk_io.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) src/*.o
	
    # If libbpf is available, build with full eBPF support
    else
        LDFLAGS = -lbpf -lelf -lz
        SOURCES = src/agent.cpp
        OBJECTS = $(SOURCES:.cpp=.o)

all: $(TARGET)
	@echo "[BUILD] Successfully compiled production-ready agent with eBPF support."

$(TARGET): $(BPF_SKEL) $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

# Generate eBPF Skeleton from BPF bytecode object
$(BPF_SKEL): $(BPF_OBJ)
	$(BPFTOOL) gen skeleton $< > $@

# Compile eBPF program into raw BPF bytecode
$(BPF_OBJ): src/disk_io.bpf.c src/disk_io.h
	$(CLANG) -g -O2 -target bpf -D__TARGET_ARCH_x86 -I/usr/include/x86_64-linux-gnu -c $< -o $@

%.o: %.cpp src/agent.h src/disk_io.h $(BPF_SKEL)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(TARGET) $(BPF_OBJ) $(BPF_SKEL) src/*.o
    endif
endif

.PHONY: all clean
