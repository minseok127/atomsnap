CXX		:= g++
CXXFLAGS	:= -std=c++20 -O2 -Wall -pthread

SP_TARGET	:= shared_ptr_example
SP_SRCS		:= shared_ptr_example.cpp

ATOM_TARGET	:= atomsnap_example
ATOM_SRCS	:= atomsnap_example.cpp

MTX_TARGET	:= mutex_example
MTX_SRCS	:= mutex_example.cpp

SPIN_TARGET	:= spinlock_example
SPIN_SRCS	:= spinlock_example.cpp

LDFLAGS	+= -L../..
LDLIBS	+= -latomsnap 

all: $(SP_TARGET) $(ATOM_TARGET) $(MTX_TARGET) $(SPIN_TARGET)

$(SP_TARGET): $(SP_SRCS)
	$(CXX) $(CXXFLAGS) -o $(SP_TARGET) $(SP_SRCS)

$(ATOM_TARGET): $(ATOM_SRCS)
	$(CXX) $(CXXFLAGS) -o $(ATOM_TARGET) $(ATOM_SRCS) $(LDFLAGS) -static $(LDLIBS)

$(MTX_TARGET): $(MTX_SRCS)
	$(CXX) $(CXXFLAGS) -o $(MTX_TARGET) $(MTX_SRCS)

$(SPIN_TARGET): $(SPIN_SRCS)
	$(CXX) $(CXXFLAGS) -o $(SPIN_TARGET) $(SPIN_SRCS)

clean:
	rm -f $(SP_TARGET) $(ATOM_TARGET) $(MTX_TARGET) $(SPIN_TARGET)
