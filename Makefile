# Shadertoy SDL2 — fullscreen shader viewer
#   make          -> build/shadertoy.exe (Windows) or build/shadertoy (Linux)
#   make DEBUG=1  -> ASan debug build
#   make clean
#
# On aarch64/arm64, automatically uses OpenGL ES via glad_es
# and probes ES 3.2 → 3.1 → 3.0 at runtime.
#
# SDL2 is located via pkg-config; if that yields nothing, sdl2-config
# --prefix is used (only if its header actually exists — some Git-Bash
# wrappers rewrite the path at runtime), then common MSYS2 prefixes are
# probed. Override on the command line if needed:
#   make SDL_PREFIX=/path/to/prefix

CC        ?= gcc
ifeq ($(DEBUG),1)
CFLAGS    ?= -O0 -g -Wall -Wextra -std=c99 -fsanitize=address -fno-omit-frame-pointer -DDEBUG
LDFLAGS   ?= -fsanitize=address
else
CFLAGS    ?= -O2 -DNDEBUG -Wall -Wextra -std=c99
LDFLAGS   ?=
endif

# ------------------------------------------------------------------
# Platform detection
# ------------------------------------------------------------------
# MSYS2/Git-Bash shells often don't export OS=Windows_NT to make, so also
# check uname: MINGW*/MSYS*/CYGWIN* all indicate a Windows host.
UNAME_S := $(shell uname -s 2>/dev/null)
ifeq ($(OS),Windows_NT)
  IS_WINDOWS := 1
else ifneq ($(findstring MINGW,$(UNAME_S)),)
  IS_WINDOWS := 1
else ifneq ($(findstring MSYS,$(UNAME_S)),)
  IS_WINDOWS := 1
else ifneq ($(findstring CYGWIN,$(UNAME_S)),)
  IS_WINDOWS := 1
else
  IS_WINDOWS :=
endif

# ------------------------------------------------------------------
# SDL2 flags
# ------------------------------------------------------------------
# 1) pkg-config (2>/dev/null: absent -> empty, no noise)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

ifeq ($(SDL_LIBS),)
  # 2) sdl2-config --prefix, but only trusted if its header really exists
  #    (Git-Bash's wrapper substitutes its own root into the path at
  #    runtime, producing "C:/Program Files/Git/mingw64" with no SDL2 in it)
  ifeq ($(origin SDL_PREFIX),undefined)
    SDL_PREFIX := $(shell sdl2-config --prefix 2>/dev/null)
    ifeq ($(wildcard $(SDL_PREFIX)/include/SDL2/SDL.h),)
      SDL_PREFIX :=
    endif
  endif
  # 3) probe known MSYS2 prefixes
  ifeq ($(SDL_PREFIX),)
    SDL_PREFIXES := /mingw64 /ucrt64 /clang64 /c/msys64/mingw64 /c/msys64/ucrt64 /usr
    SDL_PREFIX := $(firstword $(foreach p,$(SDL_PREFIXES),$(if $(wildcard $(p)/include/SDL2/SDL.h),$(p),)))
  endif
  ifneq ($(SDL_PREFIX),)
    SDL_CFLAGS := -I$(SDL_PREFIX)/include/SDL2 -Dmain=SDL_main
    SDL_LIBS   := -L$(SDL_PREFIX)/lib -lmingw32 -lSDL2main -lSDL2
    ifeq ($(IS_WINDOWS),1)
      SDL_LIBS += -mwindows
    endif
  else
    $(warning SDL2 not found via pkg-config, sdl2-config, or common MSYS2 prefixes.)
    $(warning Install it, e.g. MSYS2:  pacman -S mingw-w64-x86_64-SDL2)
    $(warning or point make at an existing install:  make SDL_PREFIX=/path/to/prefix)
  endif
endif

# ------------------------------------------------------------------
# OpenGL loading: glad for desktop GL, glad_es for ARM (OpenGL ES)
# ------------------------------------------------------------------
ARCH      := $(shell $(CC) -dumpmachine 2>/dev/null)
IS_ARM    := $(findstring aarch64,$(ARCH))$(findstring arm64,$(ARCH))

ifneq ($(IS_ARM),)
  # On ARM: use OpenGL ES via glad_es, probe 3.2 → 3.1 → 3.0 at runtime
  GLAD_SRC   := glad_es/glad.c
  GLAD_INC   := -Iglad_es
  GL_CFLAGS  := -DUSE_GLES
else
  GLAD_SRC   := glad/glad.c
  GLAD_INC   := -Iglad
  GL_CFLAGS  :=
endif

ifeq ($(IS_WINDOWS),1)
  BINEXT    := .exe
  PLAT_LIBS := -lopengl32 -lgdi32
else
  BINEXT    :=
  PLAT_LIBS := -lm -ldl
ifneq ($(IS_ARM),)
  # ARM Linux (Raspberry Pi, etc.) — ES via EGL
  PLAT_LIBS += -lEGL
else
  # Desktop Linux needs EGL for GLAD
  PLAT_LIBS += -lGL -lEGL
endif
endif

OUTDIR     := build
ifeq ($(DEBUG),1)
TARGET     := $(OUTDIR)/shadertoy-debug$(BINEXT)
else
TARGET     := $(OUTDIR)/shadertoy$(BINEXT)
endif

SRCS       := src/main.c $(GLAD_SRC)
OBJS       := $(patsubst %.c,%.o,$(SRCS))

all: $(TARGET)

$(OUTDIR):
	mkdir -p $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(GL_CFLAGS) $(GLAD_INC) -I. -c -o $@ $<

glad/%.o: glad/%.c
	$(CC) $(CFLAGS) -Iglad -c -o $@ $<

glad_es/%.o: glad_es/%.c
	$(CC) $(CFLAGS) -Iglad_es -c -o $@ $<

$(TARGET): $(OBJS) | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SDL_LIBS) $(PLAT_LIBS) $(LDFLAGS)
ifndef DEBUG
	strip -s $@
endif

clean:
	-rm -rf build src/*.o glad/*.o glad_es/*.o
