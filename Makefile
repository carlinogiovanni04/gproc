CC = clang
CFLAGS = -Wall -Wextra -Werror -g -Iinclude -I../gcui/include
LDFLAGS = -lSDL3 -lSDL3_ttf
SRC = src/main.c
OUT = bin/gproc
OUT_ASAN = bin/gproc_asan

all: $(OUT)
$(OUT): $(SRC) include/proc.h ../gcui/include/gcui.h
	mkdir -p bin/
	$(CC) $(SRC) $(CFLAGS) -o $(OUT) $(LDFLAGS)

asan: $(OUT_ASAN)
$(OUT_ASAN): $(SRC) include/proc.h ../gcui/include/gcui.h
	mkdir -p bin/
	$(CC) $(SRC) $(CFLAGS) -fsanitize=address -o $(OUT_ASAN) $(LDFLAGS)

clean:
	rm -rf bin/
