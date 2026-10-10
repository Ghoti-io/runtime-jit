PROJECT := fixture
CFLAGS := -Wall -std=c17 -O2 -ffp-contract=off
CXXFLAGS := -Wall -std=c++20 -O1
LIB_CFLAGS := $(CFLAGS) -fPIC
help:
	@true
