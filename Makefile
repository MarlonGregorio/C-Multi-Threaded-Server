all: httpserver

httpserver: httpserver.o
	gcc -g -Wall -Wextra -Wpedantic -Wshadow -pthread -o httpserver httpserver.o

httpserver.o: httpserver.c
	gcc -g -Wall -Wextra -Wpedantic -Wshadow -pthread -c httpserver.c

clean:
	rm httpserver.o httpserver