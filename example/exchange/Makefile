CXX			:= g++
CXXFLAGS	:= -std=c++20 -O2 -Wall -pthread -g

SP_TARGET	:= shared_ptr_example
SP_SRCS		:= shared_ptr_example.cpp

ATOM_TARGET	:= atomsnap_example
ATOM_SRCS	:= atomsnap_example.cpp

LDFLAGS	+= -L../..
LDLIBS	+= -latomsnap 

all: $(SP_TARGET) $(ATOM_TARGET)

$(SP_TARGET): $(SP_SRCS)
	$(CXX) $(CXXFLAGS) -o $(SP_TARGET) $(SP_SRCS)

$(ATOM_TARGET): $(ATOM_SRCS)
	$(CXX) $(CXXFLAGS) -o $(ATOM_TARGET) $(ATOM_SRCS) $(LDFLAGS) -static $(LDLIBS)

clean:
	rm -f $(SP_TARGET) $(ATOM_TARGET)
