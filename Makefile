SLURMINC=/usr/include

all: netns_isolate.so

netns_isolate.so: netns_isolate.c
	gcc -I$(SLURMINC) -std=gnu99 -Wall -o netns_isolate.o -fPIC -c netns_isolate.c
	gcc -shared -o netns_isolate.so netns_isolate.o

clean:
	rm -f netns_isolate.o netns_isolate.so

print_example_conf:
	@echo optional ${PWD}/netns_isolate.so partition=other netns=isolate nodes=c1

help:
	@echo "Usage: make [target]"
	@echo "Targets:"
	@echo "  all               Build the netns_isolate.so plugin"
	@echo "  clean             Remove build artifacts"
	@echo "  print_example_conf Print an example plugstack.conf line for this plugin"
	@echo "  help              Show this help message"
