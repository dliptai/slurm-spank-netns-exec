CC=gcc
SLURM_INCLUDE_DIR=/usr/include

all: netns_spank.so

netns_spank.so: netns_spank.c
	$(CC) -I$(SLURM_INCLUDE_DIR) -std=gnu99 -Wall -fPIC -shared -o netns_spank.so netns_spank.c

tests: netns_spank.c tests.c
	$(CC) -I$(SLURM_INCLUDE_DIR) -std=gnu99 -Wall -DDEBUG netns_spank.c tests.c -o tests -lslurm

test: tests
	@./tests

clean:
	rm -f netns_spank.o netns_spank.so tests

print_example_conf:
	@echo optional ${PWD}/netns_spank.so partition=partition_name netns=/var/run/netns/netns_name

help:
	@echo "Usage: make [target]"
	@echo "Targets:"
	@echo "  all                 Build the netns_spank.so plugin"
	@echo "  test                Run unit tests"
	@echo "  clean               Remove build artifacts"
	@echo "  print_example_conf  Print an example plugstack.conf line for this plugin"
	@echo "  help                Show this help message"

.PHONY: all clean test print_example_conf help
