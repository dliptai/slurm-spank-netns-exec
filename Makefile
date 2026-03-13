SLURM_INCLUDE_DIR=/usr/include

all: netns_spank.so

netns_spank.so: netns_spank.c
	gcc -I$(SLURM_INCLUDE_DIR) -std=gnu99 -Wall -o netns_spank.o -fPIC -c netns_spank.c
	gcc -shared -o netns_spank.so netns_spank.o

clean:
	rm -f netns_spank.o netns_spank.so

print_example_conf:
	@echo optional ${PWD}/netns_spank.so partition=other netns=isolate nodes=c1

help:
	@echo "Usage: make [target]"
	@echo "Targets:"
	@echo "  all               Build the netns_spank.so plugin"
	@echo "  clean             Remove build artifacts"
	@echo "  print_example_conf Print an example plugstack.conf line for this plugin"
	@echo "  help              Show this help message"
