# Fixture for the gate self-test; not part of the library. A correct
# makefile in the shape check-stamps reads.
FLAGS_STAMP := build/.flags

.PHONY: force-flags

$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CC) $(LIB_CFLAGS) $(INCLUDE) $(LDFLAGS)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

build/%.o: src/%.c $(FLAGS_STAMP)
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -o $@

lib.so: build/a.o
	$(CC) -shared -o $@ $^ $(LDFLAGS)
