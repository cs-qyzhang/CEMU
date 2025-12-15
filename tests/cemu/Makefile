BUILD_DIR := build
TARGETS = \
	$(BUILD_DIR)/libcemu.a \
	$(BUILD_DIR)/cemu_benchmark \
	$(BUILD_DIR)/test_fdmfs \
	$(BUILD_DIR)/vadd_example \
	$(BUILD_DIR)/test_indirect \
	$(BUILD_DIR)/test_p2p \
	$(BUILD_DIR)/test_iouring \
	$(BUILD_DIR)/test_sync_breakdown

CC = gcc
CXX = g++
CFLAGS = -Wall -Wextra -Wno-unused-function -pedantic -g
CXXFLAGS = $(CFLAGS)

.PHONY: all kernel

all: $(BUILD_DIR) $(TARGETS)

kernel:
	$(MAKE) -C kernel all

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	chmod 777 $(BUILD_DIR)

$(BUILD_DIR)/libcemu.a: util.c util.h cemu_ioctl.h
	$(CC) $(CFLAGS) -c -o $(BUILD_DIR)/util.o $<
	ar rcs $@ $(BUILD_DIR)/util.o

$(BUILD_DIR)/test_fdmfs: test_fdmfs.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CFLAGS) -o $@ $^

$(BUILD_DIR)/vadd_example: vadd_example.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CXXFLAGS) -o $@ $^ -luring

$(BUILD_DIR)/cemu_benchmark: cemu_benchmark.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CXXFLAGS) -o $@ $^ -luring

$(BUILD_DIR)/test_iouring: test_iouring.c $(BUILD_DIR)/libcemu.a
	$(CC) $(CFLAGS) -o $@ $^ -luring

$(BUILD_DIR)/test_indirect: test_indirect.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CFLAGS) -o $@ $^ -luring

$(BUILD_DIR)/test_parallel_chunks_threads: test_parallel_chunks_threads.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CFLAGS) -o $@ $^ -luring

$(BUILD_DIR)/test_sync_breakdown: test_sync_breakdown.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CXXFLAGS) -o $@ $^ -luring

$(BUILD_DIR)/test_p2p: test_p2p.cpp $(BUILD_DIR)/libcemu.a
	$(CXX) $(CXXFLAGS) -o $@ $^ -luring

clean:
	rm -rf $(BUILD_DIR)
