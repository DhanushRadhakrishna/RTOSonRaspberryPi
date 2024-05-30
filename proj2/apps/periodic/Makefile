ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
CC=gcc

NAME=periodic

all: 
	$(CROSS_COMPILE)$(CC) -O2 -o $(NAME) $(NAME).c

clean:
	rm -f $(NAME) 2> /dev/null

