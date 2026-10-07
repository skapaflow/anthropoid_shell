# Makefile — programs of bin/ (the only Makefile in this tree)
#
#   make            compiles every program that is out of date (bin/<name>/<name>.c -> bin/<name>.exe)
#   make -j4        the same, four at a time
#   make ls.exe     only one program
#   make note-test  builds and runs the test suite of note (note/tests/note_test.c)
#   make clean      removes the .exe files built from here
#   make list       shows what is built from here
#
# Run from bin/ (or from the root: make bin / make -C bin). Other compiler: make CC=gcc

SHELL = cmd.exe

CC     = clang
CFLAGS = -O2 -Wall

# unix-style programs: wmain, shared bin/common.h
UNIX_PROGS = ls cat mkdir rm cp mv touch head tail wc sort grep kill pwd sleep tee uniq du find ps pkill xargs core color show
# stand-alone tools and games: one .c each, no common.h
TOOL_PROGS = cube pong brick dump

UNIX_EXES = $(addsuffix .exe,$(UNIX_PROGS))
TOOL_EXES = $(addsuffix .exe,$(TOOL_PROGS)) ascii.exe vplay.exe note.exe
EXES      = $(UNIX_EXES) $(TOOL_EXES)

NOTE_SRCS = $(wildcard note/src/*.c)
NOTE_HDRS = $(wildcard note/src/*.h)
NOTE_TEST = note/tests/note_test.exe

.PHONY: all clean list note-test

all: $(EXES)

list:
	@echo $(EXES)

# one rule per program (make only replaces the first % of a prerequisite, so %/%.c is not possible)
define UNIX_RULE
$(1).exe: $(1)/$(1).c common.h
	$$(CC) $$(CFLAGS) -municode $$< -o $$@
endef
define TOOL_RULE
$(1).exe: $(1)/$(1).c
	$$(CC) $$(CFLAGS) -s $$< -o $$@
endef
$(foreach p,$(UNIX_PROGS),$(eval $(call UNIX_RULE,$(p))))
$(foreach p,$(TOOL_PROGS),$(eval $(call TOOL_RULE,$(p))))

# asc/ascii.c builds ascii.exe
ascii.exe: asc/ascii.c
	$(CC) $(CFLAGS) -s $< -o $@

# vplay: no console (-mwindows); winmm = MCI, dwmapi = composition, gdi32 = the time bar
vplay.exe: vplay/vplay.c
	$(CC) $(CFLAGS) -s -mwindows $< -lwinmm -ldwmapi -lgdi32 -o $@

# note: several sources; note.cfg is read from the folder of note.exe
note.exe: $(NOTE_SRCS) $(NOTE_HDRS)
	$(CC) $(CFLAGS) -s $(NOTE_SRCS) -lshell32 -o $@

$(NOTE_TEST): note/tests/note_test.c
	$(CC) $(CFLAGS) $< -lshell32 -o $@

# opens note in a hidden console, types and checks the screen and the files
note-test: note.exe $(NOTE_TEST)
	$(subst /,\,$(NOTE_TEST)) note.exe

clean:
	-del /q $(EXES) $(subst /,\,$(NOTE_TEST)) 2>nul
