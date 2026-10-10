PROJECT := fixture
CFLAGS := -Wall -std=c17 -O2 -ffp-contract=off
CXXFLAGS := -Wall -std=c++20 -O1 -ffp-contract=off
LIB_CFLAGS := $(CFLAGS) -fPIC
TSAN_CXXFLAGS := -Wall -std=c++20 -fsanitize=thread
help:
	@true
