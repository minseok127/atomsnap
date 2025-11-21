CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -pthread
LDLIBS   := -lurcu -lurcu-qsbr

TARGETS  := urcu_mb_exchange urcu_mb_cas urcu_qsbr_exchange urcu_qsbr_cas

all: $(TARGETS)

urcu_mb_exchange: urcu_mb_exchange.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lurcu

urcu_mb_cas: urcu_mb_cas.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lurcu

urcu_qsbr_exchange: urcu_qsbr_exchange.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lurcu-qsbr

urcu_qsbr_cas: urcu_qsbr_cas.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< -lurcu-qsbr

clean:
	rm -f $(TARGETS)
