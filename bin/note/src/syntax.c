#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <stdbool.h>

#include "syntax.h"
#include "config.h"

/*************************************************************
 * Syntax highlighting.                                      *
 *                                                           *
 * Each language is one row of the table below: file        *
 * names, comments, strings, word lists and a few flags. One *
 * generic lexer reads them. HTML/XML, config files and      *
 * diffs have small lexers of their own.                     *
 *                                                           *
 * syntax_line colors one line (one S_* style per UTF-16     *
 * unit) and returns the state for the next line: inside a   *
 * block comment, inside a string that crosses lines (""",   *
 * `, [[ ]]) or inside an HTML tag.                          *
 *************************************************************/

enum {
	STATE_NORMAL,
	STATE_COMMENT,
	STATE_STRING,        /* + index of the long string */
	STATE_TAG = 8        /* HTML: between the tag name and '>' */
};

/* flags */
#define F_PREPROCESSOR  0x0001   /* # directive at the start of a line (C, C#) */
#define F_TYPE_T        0x0002   /* names ending in _t are types */
#define F_CAPS          0x0004   /* ALL_CAPS names are constants */
#define F_CAPITAL_TYPES 0x0008   /* Capitalized names are types */
#define F_FUNCTIONS     0x0010   /* a name before '(' is a function */
#define F_NOCASE        0x0020   /* words ignore case */
#define F_DOLLAR        0x0040   /* $name, ${...} and $(...) are variables */
#define F_PERCENT       0x0080   /* %name%, %1 and %%i are variables (batch) */
#define F_DECORATORS    0x0100   /* @name: decorators, annotations, at-rules */
#define F_LABELS        0x0200   /* "name:" at the start of a line: targets, keys, properties */
#define F_KEYS          0x0400   /* a string before ':' is a key (JSON) */
#define F_HEADINGS      0x0800   /* a line starting with # is a heading (Markdown) */
#define F_MACROS        0x1000   /* name! is a macro (Rust) */
#define F_SHORT_CHARS   0x2000   /* ' quotes only a short char literal ('a' vs the lifetime 'a) */
#define F_NO_OPERATORS  0x4000   /* punctuation keeps the text color */

typedef struct {
	const wchar_t *open, *close;
} PAIR;

typedef int (*LEXER) (const wchar_t *, int, int, unsigned char *);

typedef struct {
	const wchar_t *name;
	const wchar_t *files;          /* ".ext" or whole names ("Makefile"), space separated */
	const wchar_t *line_comments;  /* space separated; a word ("rem") must stand alone */
	PAIR           block_comment;
	PAIR           long_strings[2];/* strings that may cross lines */
	const wchar_t *quotes;         /* one-line string delimiters */
	const wchar_t *prefixes;       /* letters that may prefix a string: L"", f"", r"" */
	const wchar_t *keywords;       /* space separated word lists */
	const wchar_t *types;
	const wchar_t *constants;
	int            flags;
	LEXER          lexer;          /* a lexer of its own instead of the generic one */
} LANGUAGE;

static int html_line (const wchar_t *, int, int, unsigned char *);
static int config_line (const wchar_t *, int, int, unsigned char *);
static int diff_line (const wchar_t *, int, int, unsigned char *);

/*---------------------------------------------------------- word lists */

#define C_KEYWORDS \
	L"auto break case const continue default do else extern for goto if inline register " \
	L"restrict return sizeof static switch typedef volatile while _Alignas _Alignof _Atomic " \
	L"_Generic _Noreturn _Static_assert _Thread_local alignas alignof constexpr static_assert " \
	L"thread_local typeof "

#define C_TYPES \
	L"void char short int long float double signed unsigned bool _Bool _Complex struct union " \
	L"enum FILE va_list wchar_t BOOL BYTE WORD DWORD UINT ULONG LONG HANDLE HWND COORD WCHAR " \
	L"CHAR LPSTR LPCSTR LPWSTR LPCWSTR SMALL_RECT INPUT_RECORD KEY_EVENT_RECORD " \
	L"MOUSE_EVENT_RECORD CONSOLE_SCREEN_BUFFER_INFO LARGE_INTEGER "

#define CPP_KEYWORDS \
	C_KEYWORDS \
	L"class namespace new delete public private protected template typename using virtual " \
	L"override final try catch throw operator this friend explicit mutable noexcept " \
	L"static_cast dynamic_cast const_cast reinterpret_cast decltype concept requires " \
	L"co_await co_return co_yield export import module consteval constinit"

#define JS_KEYWORDS \
	L"async await break case catch class const continue debugger default delete do else " \
	L"export extends finally for from function get if import in instanceof let new of " \
	L"return set static super switch this throw try typeof var void while with yield "

#define TS_KEYWORDS \
	JS_KEYWORDS \
	L"as interface type enum implements namespace declare readonly public private " \
	L"protected abstract keyof infer is satisfies"

#define JS_TYPES L"Array Promise Map Set Object String Number Boolean Symbol Date RegExp Error"

#define TS_TYPES JS_TYPES L" number string boolean any unknown never object symbol bigint void"

/*---------------------------------------------------------- languages */

static const LANGUAGE languages[] = {
	{ L"C", L".c .h .inc",
		L"//", { L"/*", L"*/" }, { { NULL } }, L"\"'", L"LuU8",
		C_KEYWORDS, C_TYPES, L"NULL true false EOF TRUE FALSE",
		F_PREPROCESSOR | F_TYPE_T | F_CAPS | F_FUNCTIONS },

	{ L"C++", L".cpp .hpp .cc .hh .cxx .hxx .ino",
		L"//", { L"/*", L"*/" }, { { NULL } }, L"\"'", L"LuU8R",
		CPP_KEYWORDS, C_TYPES L" string vector map", L"NULL nullptr true false EOF TRUE FALSE",
		F_PREPROCESSOR | F_TYPE_T | F_CAPS | F_FUNCTIONS },

	{ L"C#", L".cs .csx",
		L"//", { L"/*", L"*/" }, { { L"\"\"\"", L"\"\"\"" } }, L"\"'", NULL,
		L"abstract as base break case catch checked class const continue default delegate do " \
		L"else enum event explicit extern finally fixed for foreach goto if implicit in " \
		L"interface internal is lock namespace new operator out override params private " \
		L"protected public readonly ref return sealed sizeof stackalloc static struct switch " \
		L"this throw try typeof unchecked unsafe using virtual volatile while async await var " \
		L"get set init record yield when where partial global nameof",
		L"bool byte char decimal double float int long object sbyte short string uint ulong " \
		L"ushort void dynamic",
		L"true false null",
		F_PREPROCESSOR | F_CAPITAL_TYPES | F_FUNCTIONS },

	{ L"Java", L".java",
		L"//", { L"/*", L"*/" }, { { L"\"\"\"", L"\"\"\"" } }, L"\"'", NULL,
		L"abstract assert break case catch class continue default do else enum extends final " \
		L"finally for if implements import instanceof interface native new package private " \
		L"protected public return static strictfp super switch synchronized this throw throws " \
		L"transient try volatile while var record yield sealed permits",
		L"boolean byte char double float int long short void",
		L"true false null",
		F_CAPS | F_CAPITAL_TYPES | F_FUNCTIONS | F_DECORATORS },

	{ L"JavaScript", L".js .mjs .cjs .jsx",
		L"//", { L"/*", L"*/" }, { { L"`", L"`" } }, L"\"'", NULL,
		JS_KEYWORDS, JS_TYPES, L"true false null undefined NaN Infinity",
		F_CAPS | F_CAPITAL_TYPES | F_FUNCTIONS | F_DECORATORS },

	{ L"TypeScript", L".ts .tsx .mts .cts",
		L"//", { L"/*", L"*/" }, { { L"`", L"`" } }, L"\"'", NULL,
		TS_KEYWORDS, TS_TYPES, L"true false null undefined NaN Infinity",
		F_CAPS | F_CAPITAL_TYPES | F_FUNCTIONS | F_DECORATORS },

	{ L"Python", L".py .pyw .pyi",
		L"#", { NULL }, { { L"\"\"\"", L"\"\"\"" }, { L"'''", L"'''" } }, L"\"'", L"rRbBfFuU",
		L"and as assert async await break class continue def del elif else except finally for " \
		L"from global if import in is lambda nonlocal not or pass raise return try while with " \
		L"yield match case",
		L"int float str bytes bool list dict set tuple object complex frozenset bytearray type range",
		L"True False None self cls",
		F_CAPS | F_CAPITAL_TYPES | F_FUNCTIONS | F_DECORATORS },

	{ L"Go", L".go",
		L"//", { L"/*", L"*/" }, { { L"`", L"`" } }, L"\"'", NULL,
		L"break case chan const continue default defer else fallthrough for func go goto if " \
		L"import interface map package range return select struct switch type var",
		L"bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune " \
		L"string uint uint8 uint16 uint32 uint64 uintptr any",
		L"true false nil iota",
		F_FUNCTIONS },

	{ L"Rust", L".rs",
		L"//", { L"/*", L"*/" }, { { NULL } }, L"\"'", L"br",
		L"as async await break const continue crate dyn else enum extern fn for if impl in let " \
		L"loop match mod move mut pub ref return self Self static struct super trait type " \
		L"unsafe use where while",
		L"i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 bool char str",
		L"true false None Some Ok Err",
		F_CAPS | F_CAPITAL_TYPES | F_FUNCTIONS | F_MACROS | F_SHORT_CHARS },

	{ L"Lua", L".lua",
		L"--", { L"--[[", L"]]" }, { { L"[[", L"]]" } }, L"\"'", NULL,
		L"and break do else elseif end for function goto if in local not or repeat return " \
		L"then until while",
		NULL, L"true false nil self",
		F_FUNCTIONS },

	{ L"Ruby", L".rb Rakefile Gemfile",
		L"#", { L"=begin", L"=end" }, { { NULL } }, L"\"'", NULL,
		L"alias and begin break case class def do else elsif end ensure for if in module next " \
		L"not or redo rescue retry return super then undef unless until when while yield",
		NULL, L"true false nil self",
		F_CAPITAL_TYPES | F_FUNCTIONS | F_DECORATORS | F_DOLLAR },

	{ L"Shell", L".sh .bash .zsh .bashrc .profile",
		L"#", { NULL }, { { NULL } }, L"\"'", NULL,
		L"if then else elif fi case esac for select while until do done in function time " \
		L"return exit break continue local export readonly declare unset shift source alias " \
		L"echo cd set",
		NULL, L"true false",
		F_DOLLAR },

	{ L"PowerShell", L".ps1 .psm1 .psd1",
		L"#", { L"<#", L"#>" }, { { NULL } }, L"\"'", NULL,
		L"begin break catch class continue data define do dynamicparam else elseif end enum " \
		L"exit filter finally for foreach from function hidden if in param process return " \
		L"static switch throw trap try until using var while",
		NULL, NULL,
		F_NOCASE | F_DOLLAR },

	{ L"Batch", L".bat .cmd",
		L"rem ::", { NULL }, { { NULL } }, L"\"", NULL,
		L"if else for in do goto call set setlocal endlocal echo exit not exist defined " \
		L"errorlevel equ neq lss leq gtr geq shift pause cd pushd popd start title cls mkdir " \
		L"rmdir del copy move type",
		NULL, L"on off nul",
		F_NOCASE | F_PERCENT },

	{ L"SQL", L".sql",
		L"--", { L"/*", L"*/" }, { { NULL } }, L"'\"", NULL,
		L"select from where and or not insert into values update set delete create table drop " \
		L"alter add column index view join inner left right outer full cross on as group by " \
		L"order having limit offset union all distinct case when then else end is like in " \
		L"between exists primary key foreign references default constraint unique begin " \
		L"commit rollback transaction if returns return declare procedure function trigger " \
		L"with asc desc",
		L"int integer bigint smallint tinyint decimal numeric float real double char varchar " \
		L"text nvarchar date time datetime timestamp boolean bit blob",
		L"true false null",
		F_NOCASE | F_FUNCTIONS },

	{ L"Makefile", L"Makefile GNUmakefile .mk .mak",
		L"#", { NULL }, { { NULL } }, L"\"'", NULL,
		L"ifeq ifneq ifdef ifndef else endif include sinclude define endef export unexport " \
		L"override vpath",
		NULL, NULL,
		F_DOLLAR | F_LABELS },

	{ L"Dockerfile", L"Dockerfile .dockerfile",
		L"#", { NULL }, { { NULL } }, L"\"'", NULL,
		L"from as run cmd label maintainer expose env add copy entrypoint volume user workdir " \
		L"arg onbuild stopsignal healthcheck shell",
		NULL, NULL,
		F_NOCASE | F_DOLLAR },

	{ L"JSON", L".json .jsonc .json5",
		L"//", { L"/*", L"*/" }, { { NULL } }, L"\"", NULL,
		NULL, NULL, L"true false null",
		F_KEYS },

	{ L"YAML", L".yml .yaml",
		L"#", { NULL }, { { NULL } }, L"\"'", NULL,
		NULL, NULL, L"true false null yes no on off",
		F_LABELS },

	{ L"CSS", L".css",
		NULL, { L"/*", L"*/" }, { { NULL } }, L"\"'", NULL,
		NULL, NULL, L"important inherit initial none auto",
		F_LABELS | F_DECORATORS | F_FUNCTIONS },

	{ L"SCSS", L".scss .less",
		L"//", { L"/*", L"*/" }, { { NULL } }, L"\"'", NULL,
		NULL, NULL, L"important inherit initial none auto",
		F_LABELS | F_DECORATORS | F_FUNCTIONS | F_DOLLAR },

	{ L"Markdown", L".md .markdown",
		NULL, { NULL }, { { L"```", L"```" } }, L"`", NULL,
		NULL, NULL, NULL,
		F_HEADINGS | F_NO_OPERATORS },

	{ L"HTML", L".html .htm .xhtml .xml .svg .xaml .xsd .xsl .plist .csproj .vcxproj .props .targets",
		NULL, { NULL }, { { NULL } }, NULL, NULL, NULL, NULL, NULL, 0, html_line },

	{ L"Config", L".cfg .ini .conf .toml .properties .editorconfig .gitconfig .gitignore .gitattributes .reg",
		NULL, { NULL }, { { NULL } }, NULL, NULL, NULL, NULL, NULL, 0, config_line },

	{ L"Diff", L".diff .patch",
		NULL, { NULL }, { { NULL } }, NULL, NULL, NULL, NULL, NULL, 0, diff_line },
};

#define LANGUAGE_COUNT ((int) (sizeof languages / sizeof languages[0]))

/*---------------------------------------------------------- helpers */

static bool ident_start (wchar_t c) {

	return c == L'_' || iswalpha(c);
}

static bool ident_char (wchar_t c) {

	return c == L'_' || iswalnum(c);
}

static void mark (unsigned char *attr, int from, int to, int style) {

	if (attr && to > from)
		memset(attr + from, style, to - from);
}

/* is s[i..) starting with 'word'? */
static bool starts (const wchar_t *s, int n, int i, const wchar_t *word, bool nocase) {

	int len = (int) wcslen(word);

	return i + len <= n && !(nocase ? _wcsnicmp(s + i, word, len) : wcsncmp(s + i, word, len));
}

/* s[0..n) is one of the space separated words of 'list' */
static bool word_in (const wchar_t *list, const wchar_t *s, int n, bool nocase) {

	const wchar_t *p = list, *e;

	if (!list)
		return false;
	while (*p) {
		for (e = p; *e && *e != L' '; e++)
			;
		if (e - p == n && !(nocase ? _wcsnicmp(p, s, n) : wcsncmp(p, s, n)))
			return true;
		p = *e ? e + 1 : e;
	}
	return false;
}

/* position just after 'close' from i, or -1 if the line ends first */
static int find_close (const wchar_t *s, int n, int i, const wchar_t *close) {

	for (; i < n; i++)
		if (starts(s, n, i, close, false))
			return i + (int) wcslen(close);
	return -1;
}

/* end of a "..." starting at s[i] (escapes skipped; stops at the end of the line) */
static int quoted_end (const wchar_t *s, int n, int i) {

	wchar_t q = s[i++];

	while (i < n && s[i] != q)
		i += s[i] == L'\\' && i + 1 < n ? 2 : 1;
	return i < n ? i + 1 : n;
}

/* a line comment of the language starts at s[i]? */
static bool line_comment_at (const LANGUAGE *lang, const wchar_t *s, int n, int i) {

	const wchar_t *p = lang->line_comments, *e;
	bool nocase = (lang->flags & F_NOCASE) != 0;

	if (!p)
		return false;
	while (*p) {
		int len;
		for (e = p; *e && *e != L' '; e++)
			;
		len = (int) (e - p);
		if (i + len <= n && !(nocase ? _wcsnicmp(s + i, p, len) : wcsncmp(s + i, p, len))) {
			/* a word must stand alone: "rem" is not the start of "remove" */
			if (!iswalpha(p[0]) || ((i == 0 || !ident_char(s[i-1])) && (i + len >= n || !ident_char(s[i+len]))))
				return true;
		}
		p = *e ? e + 1 : e;
	}
	return false;
}

/* class of the word s[i..j) given what follows it */
static int word_style (const LANGUAGE *lang, const wchar_t *s, int n, int i, int j) {

	bool nocase = (lang->flags & F_NOCASE) != 0;
	int k, upper = 0, lower = 0;

	if (word_in(lang->keywords, s + i, j - i, nocase))
		return S_KEYWORD;
	if (word_in(lang->types, s + i, j - i, nocase))
		return S_TYPE;
	if (word_in(lang->constants, s + i, j - i, nocase))
		return S_CONSTANT;
	if ((lang->flags & F_TYPE_T) && j - i > 2 && s[j-2] == L'_' && s[j-1] == L't')
		return S_TYPE;

	for (k = i; k < j; k++) {
		if (iswupper(s[k]))
			upper++;
		else if (iswlower(s[k]))
			lower++;
	}
	if ((lang->flags & F_CAPS) && upper > 0 && lower == 0 && j - i > 1)
		return S_CONSTANT;
	if ((lang->flags & F_MACROS) && j < n && s[j] == L'!' && (j + 1 >= n || s[j+1] != L'='))
		return S_FUNCTION;

	for (k = j; k < n && (s[k] == L' ' || s[k] == L'\t'); k++)
		;
	if ((lang->flags & F_FUNCTIONS) && k < n && s[k] == L'(')
		return S_FUNCTION;
	if ((lang->flags & F_CAPITAL_TYPES) && iswupper(s[i]) && lower > 0)
		return S_TYPE;
	return S_TEXT;
}

/*---------------------------------------------------------- generic lexer */

/* things that only happen at the start of a line; returns where the rest of the line starts */
static int line_start (const LANGUAGE *lang, const wchar_t *s, int n, unsigned char *attr, bool *done) {

	int i = 0, j, k;

	*done = false;
	while (i < n && (s[i] == L' ' || s[i] == L'\t'))
		i++;
	if (i >= n)
		return i;

	/* Markdown heading */
	if ((lang->flags & F_HEADINGS) && s[i] == L'#') {
		mark(attr, i, n, S_KEYWORD);
		*done = true;
		return n;
	}

	/* # directive; #include <file> */
	if ((lang->flags & F_PREPROCESSOR) && s[i] == L'#') {
		for (j = i + 1; j < n && (s[j] == L' ' || s[j] == L'\t'); j++)
			;
		for (k = j; k < n && ident_char(s[k]); k++)
			;
		mark(attr, i, k, S_PREPROCESSOR);
		if (k - j == 7 && !wcsncmp(s + j, L"include", 7)) {
			for (; k < n && (s[k] == L' ' || s[k] == L'\t'); k++)
				;
			if (k < n && s[k] == L'<') {
				for (j = k; k < n && s[k] != L'>'; k++)
					;
				if (k < n)
					k++;
				mark(attr, j, k, S_STRING);
			}
		}
		return k;
	}

	/* "name:" (make target, YAML key, CSS property); not on recipe lines, not "a := b" or "http://" */
	if ((lang->flags & F_LABELS) && s[0] != L'\t' && !line_comment_at(lang, s, n, i)) {
		if (s[i] == L'-' && i + 1 < n && s[i+1] == L' ')
			i += 2;
		for (k = i; k < n && !wcschr(L":=\"'#{};", s[k]); k++)
			;
		if (k > i && k < n && s[k] == L':' && (k + 1 >= n || wcschr(L" \t:", s[k+1]))) {
			mark(attr, i, k, S_FUNCTION);
			return k;
		}
	}
	return i;
}

static int generic_line (const LANGUAGE *lang, const wchar_t *s, int n, int state, unsigned char *attr) {

	int i = 0, j, k, f = lang->flags;
	bool done;

	mark(attr, 0, n, S_TEXT);

	/* what the line above left open */
	if (state == STATE_COMMENT) {
		j = find_close(s, n, 0, lang->block_comment.close);
		if (j < 0) {
			mark(attr, 0, n, S_COMMENT);
			return STATE_COMMENT;
		}
		mark(attr, 0, j, S_COMMENT);
		i = j;
	} else if (state >= STATE_STRING && state < STATE_STRING + 2) {
		j = find_close(s, n, 0, lang->long_strings[state - STATE_STRING].close);
		if (j < 0) {
			mark(attr, 0, n, S_STRING);
			return state;
		}
		mark(attr, 0, j, S_STRING);
		i = j;
	} else {
		i = line_start(lang, s, n, attr, &done);
		if (done)
			return STATE_NORMAL;
	}

	while (i < n) {
		wchar_t c = s[i];

		if (lang->block_comment.open && starts(s, n, i, lang->block_comment.open, false)) {
			j = find_close(s, n, i + (int) wcslen(lang->block_comment.open), lang->block_comment.close);
			if (j < 0) {
				mark(attr, i, n, S_COMMENT);
				return STATE_COMMENT;
			}
			mark(attr, i, j, S_COMMENT);
			i = j;
			continue;
		}
		for (k = 0; k < 2 && lang->long_strings[k].open; k++)
			if (starts(s, n, i, lang->long_strings[k].open, false))
				break;
		if (k < 2 && lang->long_strings[k].open) {
			j = find_close(s, n, i + (int) wcslen(lang->long_strings[k].open), lang->long_strings[k].close);
			if (j < 0) {
				mark(attr, i, n, S_STRING);
				return STATE_STRING + k;
			}
			mark(attr, i, j, S_STRING);
			i = j;
			continue;
		}
		if ((f & F_DOLLAR) && c == L'$' && i + 1 < n) {
			j = i + 1;
			if (s[j] == L'(' || s[j] == L'{') {
				wchar_t open = s[j], close = open == L'(' ? L')' : L'}';
				int depth = 0;
				for (; j < n; j++) {
					if (s[j] == open)
						depth++;
					else if (s[j] == close && --depth == 0)
						break;
				}
				j = j < n ? j + 1 : n;
			} else if (ident_char(s[j]))
				while (j < n && ident_char(s[j]))
					j++;
			else if (wcschr(L"@<^?*+#!$", s[j]) && s[j])
				j++;
			if (j > i + 1) {
				mark(attr, i, j, S_CONSTANT);
				i = j;
				continue;
			}
		}
		if (line_comment_at(lang, s, n, i)) {
			mark(attr, i, n, S_COMMENT);
			return STATE_NORMAL;
		}
		if ((f & F_PERCENT) && c == L'%' && i + 1 < n) {
			if (s[i+1] == L'%')
				for (j = i + 2; j < n && (ident_char(s[j]) || s[j] == L'~'); j++)
					;
			else if (iswdigit(s[i+1]) || s[i+1] == L'~' || s[i+1] == L'*')
				for (j = i + 1; j < n && (ident_char(s[j]) || s[j] == L'~' || s[j] == L'*'); j++)
					;
			else {
				for (j = i + 1; j < n && s[j] != L'%' && s[j] != L' '; j++)
					;
				j = j < n && s[j] == L'%' ? j + 1 : i + 1;
			}
			mark(attr, i, j, j > i + 1 ? S_CONSTANT : S_OPERATOR);
			i = j;
			continue;
		}
		if (lang->quotes && c && wcschr(lang->quotes, c)) {
			j = quoted_end(s, n, i);
			/* Rust: 'a' is a char, 'a (a lifetime) is not */
			if ((f & F_SHORT_CHARS) && c == L'\'' && !(i + 1 < n && s[i+1] == L'\\') && j - i > 4) {
				for (j = i + 1; j < n && ident_char(s[j]); j++)
					;
				mark(attr, i, j, S_TYPE);
				i = j;
				continue;
			}
			mark(attr, i, j, S_STRING);
			if (f & F_KEYS) {
				for (k = j; k < n && (s[k] == L' ' || s[k] == L'\t'); k++)
					;
				if (k < n && s[k] == L':')
					mark(attr, i, j, S_FUNCTION);
			}
			i = j;
			continue;
		}
		if (iswdigit(c) || (c == L'.' && i + 1 < n && iswdigit(s[i+1]))) {
			for (j = i + 1; j < n; j++) {
				if (ident_char(s[j]) || s[j] == L'.')
					continue;
				/* exponent sign: 1e+5, 0x1p-3 */
				if ((s[j] == L'+' || s[j] == L'-') && wcschr(L"eEpP", s[j-1]))
					continue;
				break;
			}
			mark(attr, i, j, S_NUMBER);
			i = j;
			continue;
		}
		if ((f & F_DECORATORS) && c == L'@' && i + 1 < n && ident_start(s[i+1])) {
			for (j = i + 1; j < n && (ident_char(s[j]) || s[j] == L'.' || s[j] == L'-'); j++)
				;
			mark(attr, i, j, S_PREPROCESSOR);
			i = j;
			continue;
		}
		if (ident_start(c)) {
			for (j = i + 1; j < n && ident_char(s[j]); j++)
				;
			/* a string prefix: L"...", f"...", r"..." */
			if (lang->prefixes && lang->quotes && j < n && s[j] && wcschr(lang->quotes, s[j]) && j - i <= 2) {
				for (k = i; k < j && wcschr(lang->prefixes, s[k]); k++)
					;
				if (k == j) {
					int e = quoted_end(s, n, j);
					mark(attr, i, e, S_STRING);
					i = e;
					continue;
				}
			}
			if (attr)
				mark(attr, i, j, word_style(lang, s, n, i, j));
			i = j;
			continue;
		}
		if (!(f & F_NO_OPERATORS) && c && wcschr(L"+-*/%=&|<>!~^?:", c))
			mark(attr, i, i + 1, S_OPERATOR);
		i++;
	}
	return STATE_NORMAL;
}

/*---------------------------------------------------------- own lexers */

/* HTML and XML: tags, attributes, values, entities, <!-- comments --> */
static int html_line (const wchar_t *s, int n, int state, unsigned char *attr) {

	int i = 0, j;

	mark(attr, 0, n, S_TEXT);
	while (i < n) {
		wchar_t c = s[i];

		if (state == STATE_COMMENT) {
			j = find_close(s, n, i, L"-->");
			if (j < 0) {
				mark(attr, i, n, S_COMMENT);
				return STATE_COMMENT;
			}
			mark(attr, i, j, S_COMMENT);
			i = j;
			state = STATE_NORMAL;
			continue;
		}

		if (state == STATE_TAG) {
			if (c == L'>') {
				state = STATE_NORMAL;
				i++;
			} else if (c == L'"' || c == L'\'') {
				for (j = i + 1; j < n && s[j] != c; j++)
					;
				j = j < n ? j + 1 : n;
				mark(attr, i, j, S_STRING);
				i = j;
			} else if (ident_start(c) || c == L':' || c == L'@') {
				for (j = i; j < n && (ident_char(s[j]) || wcschr(L"-:.@", s[j])) && s[j]; j++)
					;
				mark(attr, i, j, S_FUNCTION);
				i = j;
			} else {
				if (c == L'=')
					mark(attr, i, i + 1, S_OPERATOR);
				i++;
			}
			continue;
		}

		if (starts(s, n, i, L"<!--", false)) {
			state = STATE_COMMENT;
			continue;
		}
		if (c == L'<' && i + 1 < n && (iswalpha(s[i+1]) || (s[i+1] && wcschr(L"/!?", s[i+1])))) {
			int k = i + 1;
			if (wcschr(L"/!?", s[k]))
				k++;
			for (j = k; j < n && (ident_char(s[j]) || (s[j] && wcschr(L"-:.", s[j]))); j++)
				;
			mark(attr, k, j, S_KEYWORD);
			state = STATE_TAG;
			i = j;
			continue;
		}
		if (c == L'&') {
			for (j = i + 1; j < n && (ident_char(s[j]) || s[j] == L'#'); j++)
				;
			if (j < n && s[j] == L';' && j > i + 1) {
				mark(attr, i, j + 1, S_CONSTANT);
				i = j + 1;
				continue;
			}
		}
		i++;
	}
	return state;
}

/* config files (.cfg, .ini): [section], key = value, # or ; comments */
static int config_line (const wchar_t *s, int n, int state, unsigned char *attr) {

	int i = 0, j;

	(void) state;
	mark(attr, 0, n, S_TEXT);
	while (i < n && (s[i] == L' ' || s[i] == L'\t'))
		i++;
	if (i >= n)
		return STATE_NORMAL;
	if (s[i] == L'#' || s[i] == L';') {
		mark(attr, i, n, S_COMMENT);
		return STATE_NORMAL;
	}
	if (s[i] == L'[') {
		mark(attr, i, n, S_KEYWORD);
		return STATE_NORMAL;
	}

	/* key */
	for (j = i; j < n && s[j] != L'='; j++)
		;
	if (j >= n)
		return STATE_NORMAL;
	mark(attr, i, j, S_FUNCTION);
	mark(attr, j, j + 1, S_OPERATOR);

	/* value, word by word: #RRGGBB, numbers, true/false/none..., other words */
	for (i = j + 1; i < n; i = j) {
		if (s[i] == L' ' || s[i] == L'\t') {
			j = i + 1;
			continue;
		}
		for (j = i; j < n && s[j] != L' ' && s[j] != L'\t'; j++)
			;
		if (s[i] == L'#')
			mark(attr, i, j, S_STRING);
		else if (s[i] == L'"' || s[i] == L'\'')
			mark(attr, i, j, S_STRING);
		else if (iswdigit(s[i]) || ((s[i] == L'-' || s[i] == L'+') && i + 1 < j && iswdigit(s[i+1])))
			mark(attr, i, j, S_NUMBER);
		else if (word_in(L"true false yes no on off none default", s + i, j - i, true))
			mark(attr, i, j, S_CONSTANT);
		else if (word_in(L"bold italic underline", s + i, j - i, true))
			mark(attr, i, j, S_TYPE);
	}
	return STATE_NORMAL;
}

/* diffs: added, removed, hunk headers, file headers */
static int diff_line (const wchar_t *s, int n, int state, unsigned char *attr) {

	int style = S_TEXT;

	(void) state;
	if (starts(s, n, 0, L"+++", false) || starts(s, n, 0, L"---", false) ||
			starts(s, n, 0, L"diff ", false) || starts(s, n, 0, L"index ", false))
		style = S_COMMENT;
	else if (n > 0 && s[0] == L'+')
		style = S_FUNCTION;
	else if (n > 0 && s[0] == L'-')
		style = S_KEYWORD;
	else if (starts(s, n, 0, L"@@", false))
		style = S_CONSTANT;
	mark(attr, 0, n, style);
	return STATE_NORMAL;
}

/*---------------------------------------------------------- api */

/* language for a file, from its extension or its whole name */
int syntax_detect (const wchar_t *path) {

	const wchar_t *base, *dot, *p, *e;
	int i;

	if (!path)
		return LANG_NONE;
	base = path;
	for (p = path; *p; p++)
		if (*p == L'\\' || *p == L'/' || *p == L':')
			base = p + 1;
	dot = wcsrchr(base, L'.');

	for (i = 0; i < LANGUAGE_COUNT; i++) {
		for (p = languages[i].files; *p; p = *e ? e + 1 : e) {
			int len;
			for (e = p; *e && *e != L' '; e++)
				;
			len = (int) (e - p);
			if (p[0] == L'.' ? dot && (int) wcslen(dot) == len && !_wcsnicmp(dot, p, len)
					: (int) wcslen(base) == len && !_wcsnicmp(base, p, len))
				return i + 1;
		}
	}
	return LANG_NONE;
}

const wchar_t *syntax_name (int lang) {

	return lang > 0 && lang <= LANGUAGE_COUNT ? languages[lang - 1].name : L"Text";
}

/* styles of s[0..n) into attr (may be NULL); returns the state for the next line */
int syntax_line (int lang, const wchar_t *s, int n, int state, unsigned char *attr) {

	const LANGUAGE *l;

	if (lang <= 0 || lang > LANGUAGE_COUNT) {
		mark(attr, 0, n, S_TEXT);
		return STATE_NORMAL;
	}
	l = &languages[lang - 1];
	return l->lexer ? l->lexer(s, n, state, attr) : generic_line(l, s, n, state, attr);
}
