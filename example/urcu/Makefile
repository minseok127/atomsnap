CXX      := g++
CXXFLAGS := -std=c++20 -O2 -Wall -pthread
LDLIBS   := -lurcu-memb

TARGETS  := urcu_memb_exchange urcu_memb_cas

all: $(TARGETS)

urcu_memb_exchange: urcu_memb_exchange.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

urcu_memb_cas: urcu_memb_cas.cpp
	$(CXX) $(CXXFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f $(TARGETS)
