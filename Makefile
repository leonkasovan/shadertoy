# Shadertoy — fullscreen shader viewer
#   make                -> SDL2 backend, build/shadertoy.exe (Windows) or
#                          build/shadertoy (Linux)
#   make BACKEND=glfw   -> GLFW backend, build/shadertoy-glfw(.exe)
#   make console        -> console-subsystem viewer (keeps stdout/stderr)
#   make glfw           -> alias for BACKEND=glfw
#   make glfw-console   -> console-subsystem GLFW build
#   make DEBUG=1        -> ASan debug build
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
#
# The GLFW backend still links SDL2: mcp.c uses SDL for its HTTP thread,
# mutexes and condition variables.

CC        ?= gcc
ifeq ($(DEBUG),1)
CFLAGS    ?= -O0 -g -Wall -Wextra -std=c99 -DDEBUG
LDFLAGS   ?=
else
CFLAGS    ?= -O2 -DNDEBUG -Wall -Wextra -std=c99
LDFLAGS   ?=
endif

# AddressSanitizer is opt-in (SAN=1) because some toolchains (notably
# w64devkit on Windows) ship no libasan and would fail at link time.
ifeq ($(SAN),1)
CFLAGS  += -fsanitize=address -fno-omit-frame-pointer
LDFLAGS += -fsanitize=address
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
  PLAT_LIBS := -lopengl32 -lgdi32 -lws2_32
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

# ------------------------------------------------------------------
# Viewer backend: SDL2 (default) or GLFW
# ------------------------------------------------------------------
BACKEND ?= sdl2
ifeq ($(BACKEND),sdl2)
  MAIN_SRC    := src/main-sdl2.c
  TARGET_TAG  :=
  # pkg-config/sdl2-config also hand us -Dmain=SDL_main, which is required by
  # SDL2main on Windows but must not leak into the GLFW main file.
  BACKEND_CFLAGS := $(SDL_CFLAGS)
  BACKEND_LIBS   := $(SDL_LIBS)
else ifeq ($(BACKEND),glfw)
  MAIN_SRC    := src/main-glfw.c
  TARGET_TAG  := -glfw
  GLFW_CFLAGS := $(shell pkg-config --cflags glfw3 2>/dev/null)
  GLFW_LIBS   := $(shell pkg-config --libs glfw3 2>/dev/null)
  ifeq ($(GLFW_LIBS),)
    $(error GLFW not found via pkg-config; install it (e.g. MSYS2: pacman -S mingw-w64-x86_64-glfw) or build the default SDL2 backend)
  endif
  BACKEND_CFLAGS := $(filter-out -Dmain=SDL_main,$(SDL_CFLAGS)) $(GLFW_CFLAGS)
  BACKEND_LIBS   := $(SDL_LIBS) $(GLFW_LIBS)
else
  $(error Unknown BACKEND '$(BACKEND)'; use BACKEND=sdl2 or BACKEND=glfw)
endif

OUTDIR     := build
ifeq ($(DEBUG),1)
TARGET     := $(OUTDIR)/shadertoy$(TARGET_TAG)-debug$(BINEXT)
CONSOLE    := $(OUTDIR)/shadertoy$(TARGET_TAG)-debug-console$(BINEXT)
else
TARGET     := $(OUTDIR)/shadertoy$(TARGET_TAG)$(BINEXT)
CONSOLE    := $(OUTDIR)/shadertoy$(TARGET_TAG)-console$(BINEXT)
endif

SRCS       := $(MAIN_SRC) src/shader_common.c src/shader_list.c $(GLAD_SRC)

# MCP (Model Context Protocol) support via libjq
JQ_CFLAGS  := $(shell pkg-config --cflags libjq 2>/dev/null)
JQ_LIBS    := $(shell pkg-config --libs libjq 2>/dev/null)
ifeq ($(JQ_LIBS),)
  JQ_ENABLED := 0
  # Compile-time flag so the viewer can stub the MCP entry points instead of
  # failing at link time.
  JQ_CFLAGS  := -DMCP_UNAVAILABLE
  $(warning libjq not found via pkg-config: building WITHOUT MCP support.)
  $(warning Install it, e.g. MSYS2: pacman -S mingw-w64-x86_64-jq)
else
  JQ_ENABLED := 1
  SRCS += src/mcp.c
endif
OBJS       := $(patsubst %.c,%.o,$(SRCS))

# Console builds must not link the Windows GUI subsystem, otherwise stdout
# is detached (useful when running the viewer from a terminal and reading its
# log).
ifeq ($(IS_WINDOWS),1)
CONSOLE_BACKEND_LIBS := $(filter-out -mwindows,$(BACKEND_LIBS))
else
CONSOLE_BACKEND_LIBS := $(BACKEND_LIBS)
endif

all: $(TARGET)

# Convenience alias: make glfw (command-line variables such as DEBUG/SAN are
# forwarded automatically through MAKEFLAGS).
glfw:
	$(MAKE) BACKEND=glfw

glfw-console:
	$(MAKE) BACKEND=glfw console

# Console build: same sources, Windows console subsystem, so the viewer keeps
# its stdout/stderr when launched from a terminal.
console: $(CONSOLE)

$(OUTDIR):
	mkdir -p $@

src/%.o: src/%.c
	$(CC) $(CFLAGS) $(BACKEND_CFLAGS) $(GL_CFLAGS) $(GLAD_INC) $(JQ_CFLAGS) -I. -c -o $@ $<

glad/%.o: glad/%.c
	$(CC) $(CFLAGS) -Iglad -c -o $@ $<

glad_es/%.o: glad_es/%.c
	$(CC) $(CFLAGS) -Iglad_es -c -o $@ $<

$(TARGET): $(OBJS) | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(BACKEND_LIBS) $(PLAT_LIBS) $(JQ_LIBS) $(LDFLAGS)
ifndef DEBUG
	strip -s $@
endif

$(CONSOLE): $(OBJS) | $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(CONSOLE_BACKEND_LIBS) $(PLAT_LIBS) $(JQ_LIBS) $(LDFLAGS)
ifndef DEBUG
	strip -s $@
endif

clean:
	-rm -rf build src/*.o glad/*.o glad_es/*.o

.PHONY: all clean console glfw glfw-console
