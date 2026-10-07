# Makefile — AntShell (antshell.exe)
#
#   make          compiles src/**/*.c into obj/ (same tree as src/) and links antshell.exe
#   make run      builds and runs antshell.exe
#   make clean    removes the objects, the dependency files, antshell.exe and the test programs
#   make icon     compiles icon/recicon.rc (icon and version information) into obj/recicon.res
#   make bin      compiles the programs of bin/ (ls, cat, cp, grep, ...) through bin/Makefile: bin/<name>/<name>.c -> bin/<name>.exe
#   make dist     builds everything and packs antshell-win64.zip (antshell.exe, bin\ programs, data\help.ant...)
#   make test     grapheme (unicode), parser, expansion, config, completion and line editor tests
#                 (hidden console), plus OSC 133 marks (antshell.exe inside a ConPTY)
#
# Source layout (one directory per subject, like the Windows Terminal repository):
#   src/main.c    entry point
#   src/shell     main loop, parser, execution, command line arguments
#   src/builtins  built-in commands (cd, link, path, help, calc, clear)
#   src/editor    line editor, command history, TAB completion
#   src/config    data\config.ant
#   src/console   console output: colors, cursor, errors, logo
#   src/types     Unicode widths and graphemes, string helpers
# Headers live next to their .c files and are included by relative path,
# so no -I flag is needed.

# Windows-only project: the recipe commands (mkdir, del, cd) are cmd.exe commands.
SHELL = cmd.exe

TARGET   = antshell.exe
SRC_DIR  = src
OBJ_DIR  = obj
RES      = $(OBJ_DIR)/recicon.res

CC       = clang
WINDRES  = windres
# make WERROR=1 turns warnings into errors (the release workflow does this; it also reaches bin/)
CFLAGS   = -O2 -Wall $(if $(WERROR),-Werror)
DEPFLAGS = -MMD -MP
LDFLAGS  =
LDLIBS   =

SRCS = $(SRC_DIR)/main.c $(wildcard $(SRC_DIR)/*/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))
DEPS = $(OBJS:.o=.d)

TEST  = tests/input_test.exe
GTEST = tests/grapheme_test.exe
CTEST = tests/complete_test.exe
OTEST = tests/osc_test.exe
PTEST = tests/parse_test.exe
FTEST = tests/config_test.exe
ETEST = tests/expand_test.exe
KTEST = tests/calc_test.exe
BTEST = tests/bin_test.exe

# Windows Terminal's ConPTY for the OSC 133 test (without it, only the Windows ConPTY is tested)
OPENCONSOLE = C:\Windows Terminal\OpenConsole.exe

.PHONY: all run clean icon test bin dist

all: $(TARGET)

$(TARGET): $(OBJS) $(RES)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# antshell.exe icon and version information; windres runs inside icon/ so the .rc finds the .ico
$(RES): icon/recicon.rc icon/shell_icon.ico
	@if not exist $(subst /,\,$(@D)) mkdir $(subst /,\,$(@D))
	cd icon && $(WINDRES) -i recicon.rc --input-format=rc --target=pe-x86-64 -o ../$(RES) -O coff

icon: $(RES)

# obj/ mirrors src/: src/shell/parser.c -> obj/shell/parser.o
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@if not exist $(subst /,\,$(@D)) mkdir $(subst /,\,$(@D))
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

run: $(TARGET)
	$(TARGET)

test: $(TARGET) bin $(GTEST) $(CTEST) $(PTEST) $(FTEST) $(ETEST) $(KTEST) $(BTEST) $(TEST) $(OTEST)
	$(subst /,\,$(GTEST))
	$(subst /,\,$(PTEST))
	$(subst /,\,$(FTEST))
	$(subst /,\,$(ETEST))
	$(subst /,\,$(KTEST))
	$(subst /,\,$(BTEST)) bin
	$(subst /,\,$(CTEST))
	$(subst /,\,$(TEST)) $(TARGET)
	$(subst /,\,$(OTEST)) $(TARGET) "$(OPENCONSOLE)"

# The line editor and OSC tests drive antshell.exe from outside; the others link the module they test.
$(TEST): tests/input_test.c
	$(CC) $(CFLAGS) $< -lshell32 -o $@

$(OTEST): tests/osc_test.c
	$(CC) $(CFLAGS) -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006 $< -lntdll -lshell32 -o $@

$(PTEST): tests/parse_test.c $(SRC_DIR)/shell/parser.c $(SRC_DIR)/shell/parser.h
	$(CC) $(CFLAGS) tests/parse_test.c $(SRC_DIR)/shell/parser.c -o $@

$(FTEST): tests/config_test.c $(SRC_DIR)/config/config.c $(SRC_DIR)/config/config.h
	$(CC) $(CFLAGS) tests/config_test.c $(SRC_DIR)/config/config.c -lshell32 -o $@

$(ETEST): tests/expand_test.c $(SRC_DIR)/shell/expand.c $(SRC_DIR)/shell/expand.h $(SRC_DIR)/shell/parser.c $(SRC_DIR)/shell/parser.h
	$(CC) $(CFLAGS) tests/expand_test.c $(SRC_DIR)/shell/expand.c $(SRC_DIR)/shell/parser.c -o $@

$(KTEST): tests/calc_test.c $(SRC_DIR)/builtins/calc_eval.c $(SRC_DIR)/builtins/calc_eval.h
	$(CC) $(CFLAGS) tests/calc_test.c $(SRC_DIR)/builtins/calc_eval.c -lm -o $@

$(BTEST): tests/bin_test.c
	$(CC) $(CFLAGS) -municode $< -o $@

# the programs have their own Makefile (bin/Makefile: the list of programs and the rules)
bin:
	$(MAKE) -C bin

# The package a person downloads: antshell.zip with an antshell\ folder to unzip anywhere. This is
# the one place that says what is in it; the release workflow just calls `make dist`.
DIST     = dist/antshell
ZIP      = antshell-win64.zip
D        = $(subst /,\,$(DIST))
SYSROOT  = $(or $(SystemRoot),$(SYSTEMROOT),C:\Windows)
# the programs bin/Makefile builds, so a program added there is packaged without touching this file
BIN_EXES = $(shell $(MAKE) --no-print-directory -s -C bin list)

# the zip is made with the tar.exe of Windows 10 (bsdtar), by full path: it writes standard zip entries
# (the .NET zip of PowerShell 5.1 writes backslashes), and the PATH of a CI runner may find Git's GNU tar first
dist: all bin
	@if exist dist rmdir /s /q dist
	@mkdir $(D)\bin $(D)\data
	copy /y antshell.exe $(D) >nul
	for %%f in ($(BIN_EXES)) do copy /y bin\%%f $(D)\bin >nul
	copy /y bin\note.cfg $(D)\bin >nul
	copy /y data\help.ant $(D)\data >nul
	copy /y data\mk.ant $(D)\data >nul
	copy /y LICENSE $(D) >nul
	copy /y README.md $(D) >nul
	copy /y "ant desktop context menu.reg" $(D) >nul
	@if exist $(ZIP) del $(ZIP)
	$(SYSROOT)\System32\tar.exe -a -c -f $(ZIP) -C dist antshell

$(CTEST): tests/complete_test.c $(SRC_DIR)/editor/complete.c $(SRC_DIR)/editor/complete.h
	$(CC) $(CFLAGS) tests/complete_test.c $(SRC_DIR)/editor/complete.c -o $@

$(GTEST): tests/grapheme_test.c tests/grapheme_break_data.inc $(SRC_DIR)/types/unicode.c $(SRC_DIR)/types/unicode.h
	$(CC) $(CFLAGS) -Wno-missing-braces tests/grapheme_test.c $(SRC_DIR)/types/unicode.c -o $@

clean:
	-del /q $(subst /,\,$(OBJS) $(DEPS) $(RES) $(TARGET) $(TEST) $(GTEST) $(CTEST) $(OTEST) $(PTEST) $(FTEST) $(ETEST) $(KTEST) $(BTEST)) 2>nul
	$(MAKE) -C bin clean
	-@if exist dist rmdir /s /q dist
	-@if exist $(ZIP) del $(ZIP)

-include $(DEPS)
