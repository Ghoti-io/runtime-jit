PROJECT := fixture
CFLAGS := -Wall -std=c17 -O2 -ffp-contract=off
CXXFLAGS := -Wall -std=c++20 -O1 -ffp-contract=off
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -fPIC
ASAN_CFLAGS := $(CFLAGS) -fsanitize=address
CUTIL_CFLAGS := -I/somewhere/include
help:
	@true
