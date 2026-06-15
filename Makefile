CC=gcc
SLURM_INCLUDE_DIR=/usr/include

all: netns_spank.so

netns_spank.so: iproute_bind_mounts.c netns_spank.c
	$(CC) -I$(SLURM_INCLUDE_DIR) -std=gnu99 -Wall -fPIC -shared -o $@ $^

test: iproute_bind_mounts.c netns_spank.c test.c
	$(CC) -I$(SLURM_INCLUDE_DIR) -std=gnu99 -Wall -Wextra -DTEST $^ -o $@ -lslurm

clean:
	rm -f netns_spank.so test

print_example_conf:
	@echo optional ${PWD}/netns_spank.so partition=partition_name netns=netns_name

help:
	@echo "Usage: make [target]"
	@echo "Targets:"
	@echo "  all                 Build the netns_spank.so plugin"
	@echo "  test                Build test binary"
	@echo "  clean               Remove build artifacts"
	@echo "  print_example_conf  Print an example plugstack.conf line for this plugin"
	@echo "  help                Show this help message"

.PHONY: all clean print_example_conf help
