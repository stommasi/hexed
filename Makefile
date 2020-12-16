hexed: hexed.c
	$(CC) -lcurses -o hexed hexed.c

clean :
	rm hexed
