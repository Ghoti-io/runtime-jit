PROJECT := fixture
CFLAGS := -Wall -std=c17 -O2 -ffp-contract=off $(EXTRA_CFLAGS) -ffp-contract=fast
CXXFLAGS := -Wall -std=c++20 -O1 -ffp-contract=off
LIB_CFLAGS := $(CFLAGS) -fPIC
help:
	@true
