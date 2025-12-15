BUILD_DIR := ../build
TARGETS = \
	$(BUILD_DIR)/vadd.so \
	$(BUILD_DIR)/lz4.so \
	$(BUILD_DIR)/grep.so \
	$(BUILD_DIR)/sql.so \
	$(BUILD_DIR)/knn.so \
	$(BUILD_DIR)/vadd.bpf.o

all: $(BUILD_DIR) $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)
	chmod 777 $(BUILD_DIR)

CC = gcc
CXX = g++
AR = ar
CFLAGS = -Wall -Wextra -O2 -march=native -std=c11
CXXFLAGS = -Wall -Wextra -O2 -march=native -std=c++11

$(BUILD_DIR)/vadd.so: vadd.c
	$(CC) $(CFLAGS) -shared -fPIC -o $@ $<

$(BUILD_DIR)/lz4.so: lz4.cc
	$(CXX) $(CXXFLAGS) -shared -fPIC -llz4 -o $@ $< /usr/lib/x86_64-linux-gnu/liblz4.a

$(BUILD_DIR)/grep.so: grep.cc
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ $<

$(BUILD_DIR)/sql.so: sql.cc
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ $<

$(BUILD_DIR)/knn.so: knn.cc
	$(CXX) $(CXXFLAGS) -shared -fPIC -o $@ $<

$(BUILD_DIR)/vadd.bpf.o: vadd.bpf.c
	clang -target bpf -static -O2 -g -c -o $@ $<

clean:
	rm -rf $(TARGETS)
