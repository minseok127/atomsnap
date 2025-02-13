CXX			:= g++
CXXFLAGS	:= -std=c++20 -O2 -Wall -pthread

SP_TARGET	:= shared_ptr_eval
SP_SRCS		:= shared_ptr_eval.cpp

ATOM_TARGET	:= atomsnap_eval
ATOM_SRCS	:= atomsnap_eval.cpp

all: $(SP_TARGET) $(ATOM_TARGET)

$(SP_TARGET): $(SP_SRCS)
	$(CXX) $(CXXFLAGS) -o $(SP_TARGET) $(SP_SRCS)

$(ATOM_TARGET): $(ATOM_SRCS)
	$(CXX) $(CSSFLAGS) -o $(ATOM_TARGET) $(ATOM_SRCS) -L. -latomsnap

clean:
	rm -f $(SP_TARGET) $(ATOM_TARGET)
