PROJECT := fixture
CFLAGS := -Wall -std=c17 -O2 -ffp-contract=off $(EXTRA_CFLAGS)
CXXFLAGS := -Wall -std=c++20 -O1 -ffp-contract=off $(EXTRA_CXXFLAGS)
LIB_CFLAGS := $(CFLAGS) -fPIC
help:
	@true
