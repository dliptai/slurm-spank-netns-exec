SLURMINC=/usr/include

all: netns_isolate.so

netns_isolate.so: netns_isolate.c
	gcc -I$(SLURMINC) -std=gnu99 -Wall -o netns_isolate.o -fPIC -c netns_isolate.c
	gcc -shared -o netns_isolate.so netns_isolate.o

clean:
	rm -f netns_isolate.o netns_isolate.so

print_example_conf:
	@echo optional ${PWD}/netns_isolate.so partition=other netns=isolate
