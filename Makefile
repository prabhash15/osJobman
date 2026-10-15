CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude -MMD -MP

PROGRAM_OBJS := build/main.o build/job_manager.o build/schedulers.o \
                build/proc_mgr.o build/monitor.o build/csv_logger.o

all: bin/jobman bin/worker

bin/jobman: $(PROGRAM_OBJS)
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^

bin/worker: build/worker.o
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $^

build/%.o: src/%.cpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf build bin

-include $(wildcard build/*.d)

.PHONY: all clean
