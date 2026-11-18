# Build with: make LIBRETRO_INCLUDE=../engine9000-src/ami9000/libretro-common/include
LIBRETRO_INCLUDE ?= ../engine9000-src/ami9000/libretro-common/include
BUILD ?= ../build/ami9000_host
CFLAGS ?= -O2 -g -Wall -Wextra -std=gnu99
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)
LUA_CFLAGS := $(shell pkg-config --cflags lua5.4 2>/dev/null)
LUA_LIBS := $(or $(shell pkg-config --libs lua5.4 2>/dev/null),-llua)

$(BUILD): ami9000_host.c lua_script.c lua_script.h loader_api.h $(wildcard lua/*.lua)
	mkdir -p $(dir $(BUILD))
	$(CC) $(CFLAGS) -I$(LIBRETRO_INCLUDE) $(SDL_CFLAGS) $(LUA_CFLAGS) -DAMI_LUA_DIR='"$(abspath lua)"' -o $@ ami9000_host.c lua_script.c $(SDL_LIBS) $(LUA_LIBS) -ldl -lm

clean:
	rm -f $(BUILD)
.PHONY: clean
