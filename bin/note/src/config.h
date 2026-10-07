#ifndef NOTE_CONFIG_H
#define NOTE_CONFIG_H

#include <wchar.h>
#include <stdbool.h>

/* styles; the first ones (S_TEXT..S_OPERATOR) double as syntax highlight classes */
enum {
	S_TEXT,
	S_COMMENT,
	S_KEYWORD,
	S_TYPE,
	S_FUNCTION,
	S_STRING,
	S_NUMBER,
	S_CONSTANT,
	S_PREPROCESSOR,
	S_OPERATOR,
	S_CONTROL,
	S_CURRENT_LINE,
	S_SELECTION,
	S_BRACKET,
	S_LINE_NUMBER,
	S_LINE_NUMBER_CURRENT,
	S_MENU_BAR,
	S_MENU_HOTKEY,
	S_MENU,
	S_MENU_SELECTED,
	S_MENU_SHORTCUT,
	S_STATUS_BAR,
	S_STATUS_MESSAGE,
	S_STATUS_ERROR,
	S_DIALOG,
	S_DIALOG_TITLE,
	S_BUTTON,
	S_BUTTON_SELECTED,
	S_BUTTON_HOTKEY,
	S_FIELD,
	S_SHADOW,
	S_COUNT
};

typedef struct {
	int  fg, bg;                  /* 0xRRGGBB; -1: inherit */
	bool bold, italic, underline;
} STYLE;

typedef struct {
	int  tab_size;
	bool line_numbers;
	bool syntax;
	bool auto_indent;
	bool highlight_line;
	bool mouse;
	bool expand_tabs;
	bool word_wrap;
} SETTINGS;

extern STYLE    theme[S_COUNT];
extern SETTINGS settings;

extern bool config_load (wchar_t *, int);

#endif
