CC = gcc
CFLAGS = -Wall -Werror -std=c99 -pthread

all: hangman_server hangman_client

hangman_server: hangman_server.c
	$(CC) $(CFLAGS) -o hangman_server hangman_server.c

hangman_client: hangman_client.c
	$(CC) $(CFLAGS) -o hangman_client hangman_client.c

clean:
	rm -f hangman_server hangman_client