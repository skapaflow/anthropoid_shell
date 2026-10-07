#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

#include "console.h"

int get_console_width (void) {

	/* get terminal size */
	CONSOLE_SCREEN_BUFFER_INFO w;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &w);
	return w.dwMaximumWindowSize.X;
}

void set_cursor_type (bool insert, bool visible) {

	CONSOLE_CURSOR_INFO cci;
	cci.dwSize = insert ? 10 : 99;
	cci.bVisible = visible;
	SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &cci);
}

int print (int color, const char *fmt, ...) {

	CONSOLE_SCREEN_BUFFER_INFO info;
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info);
	va_list ap;
	va_start(ap, fmt);
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), color);
	int n = vprintf(fmt, ap);
	va_end(ap);
	SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), info.wAttributes);
	return n;
}

#define ASCII_LEN 96
#define ASCII_SIZE 36

/* "char pixel size" = 6x6 */
const char font_char[ASCII_LEN][ASCII_SIZE] = {
	/* NULL PART */
	/* {,} */
	/* !"#$%&'()*+,-./ */
	{"000000000000000000000000000000000000"},/*   */ /* 32 */
	{"001100001100001100001100000000001100"},/* ! */
	{"010100010100000000000000000000000000"},/* " */
	{"010100111110010100010100111110010100"},/* # */
	{"001000011110101000011100001010111100"},/* $ */
	{"011001101010110100001011010101100110"},/* % */
	{"011000100100011001100101100010011101"},/* & */
	{"001000001000000000000000000000000000"},/* ' */
	{"000100001000001000001000001000000100"},/* ( */
	{"001000000100000100000100000100001000"},/* ) */
	{"101010011100111110011100101010000000"},/* * */
	{"000000001000001000111110001000001000"},/* + */
	{"000000000000000000001100001100011000"},/* , */
	{"000000000000000000111110000000000000"},/* - */
	{"000000000000000000000000001100001100"},/* . */
	{"000000000010000100001000010000100000"},/* / */ /* 47 */
	/* 0123456789 */
	{"011100100110101010110010100010011100"},/* 0 */ /* 48 */
	{"001000011000101000001000001000111110"},/* 1 */
	{"011100100010000100001000010000111110"},/* 2 */
	{"011100100010001100000010100010011100"},/* 3 */
	{"000100001100010100111110000100000100"},/* 4 */
	{"111110100000111100000010100010011100"},/* 5 */
	{"011100100000111100100010100010011100"},/* 6 */
	{"111110000010000100001000010000100000"},/* 7 */
	{"011100100010011100100010100010011100"},/* 8 */
	{"011100100010100010011110000010011100"},/* 9 */ /* 57 */
	/* :;<=>?@ */
	{"001100001100000000000000001100001100"},/* : */ /* 58 */
	{"001100001100000000001100001100011000"},/* ; */
	{"000000000100001000010000001000000100"},/* < */
	{"000000000000011110000000011110000000"},/* = */
	{"000000001000000100000010000100001000"},/* > */
	{"011100100010001100001000000000001000"},/* ? */
	{"011100100010101110101010101100011110"},/* @ */ /* 64 */
	/* ABCDEFGHIJKLMNOPQRSTUVWXYZ */
	{"001000001000010100010100111110100010"},/* A */ /* 65 */
	{"111100100010111100100010100010111100"},/* B */
	{"011100100010100000100000100010011100"},/* C */
	{"111100100010100010100010100010111100"},/* D */
	{"111110100000111100100000100000111110"},/* E */
	{"111110100000111100100000100000100000"},/* F */
	{"011100100010100000101110100010011100"},/* G */
	{"100010100010111110100010100010100010"},/* H */
	{"011100001000001000001000001000011100"},/* I */
	{"111110000010000010100010100010011100"},/* J */
	{"100100101000110000101000100100100010"},/* K */
	{"100000100000100000100000100000111110"},/* L */
	{"100010110110101010100010100010100010"},/* M */
	{"100010110010101010100110100010100010"},/* N */
	{"011100100010100010100010100010011100"},/* O */
	{"111100100010100010111100100000100000"},/* P */
	{"011100100010100010100010100110011111"},/* Q */
	{"111100100010100010111100100010100010"},/* R */
	{"011100100010010000001100100010011100"},/* S */
	{"111110001000001000001000001000001000"},/* T */
	{"100010100010100010100010100010011100"},/* U */
	{"100010100010010100010100001000001000"},/* V */
	{"100010101010101010110110110110100010"},/* W */
	{"000000100010010100001000010100100010"},/* X */
	{"100010010100001000001000001000001000"},/* Y */
	{"111110000100001000010000100000111110"},/* Z */ /* 90 */
	/* [\]^_` */
	{"001110001000001000001000001000001110"},/* [ */ /* 91 */
	{"000000100000010000001000000100000010"},/* \ */
	{"011100000100000100000100000100011100"},/* ] */
	{"001000010100100010000000000000000000"},/* ^ */
	{"000000000000000000000000000000111110"},/* _ */
	{"110000011000000000000000000000000000"},/* ` */ /* 96 */
	/* abcdefghijklmnopqrstuvwxyz */
	{"000000011100000010011110100110011010"},/* a */ /* 97 */
	{"100000100000111100100010110010101100"},/* b */
	{"000000000000011100100000100000011100"},/* c */
	{"000010000010011110100010100110011010"},/* d */
	{"000000011100100010111110100000011100"},/* e */
	{"001110010000111100010000010000010000"},/* f */
	{"011010100110100010011010000010011100"},/* g */
	{"000000010000010000011100010010010010"},/* h */
	{"001000000000011000001000001000011100"},/* i */
	{"000100000000011100000100000100011000"},/* j */
	{"000000010000010010010100011000010110"},/* k */
	{"000000011000001000001000001000011100"},/* l */
	{"000000010100101010101010101010101010"},/* m */
	{"000000101100110010100010100010100010"},/* n */
	{"000000011100100010100010100010011100"},/* o */
	{"000000101100110010100010111100100000"},/* p */
	{"000000011010100110100010011110000010"},/* q */
	{"000000101100110010100000100000100000"},/* r */
	{"000000001110010000001100000010011100"},/* s */
	{"010000111100010000010000010000001100"},/* t */
	{"000000100010100010100010100110011010"},/* u */
	{"000000000000100010100010010100001000"},/* v */
	{"000000000000100010101010101010010100"},/* w */
	{"000000000000110110001000001000110110"},/* x */
	{"000000000000100010010100001000110000"},/* y */
	{"000000000000111100001000010000111100"},/* z */ /* 122 */
	/* {|}~█ */
	{"000110001000001000110000001000001110"},/* { */ /* 123 */
	{"001000001000001000001000001000001000"},/* | */
	{"011000000100000100000011000100011100"},/* } */
	{"000000000000011010101100000000000000"},/* ~ */
	{"011110011110011110011110011110011110"} /* █ */ /* 127 */
};



/* the drawing characters, in UTF-8 (the shell's code page), so they look the same whatever the console's original one */
#define BLOCK_FULL   "\xe2\x96\x88" /* █ */
#define SHADE_LIGHT  "\xe2\x96\x91" /* ░ */
#define SHADE_MEDIUM "\xe2\x96\x92" /* ▒ */
#define SHADE_DARK   "\xe2\x96\x93" /* ▓ */
#define BOX_H        "\xe2\x94\x80" /* ─ */
#define BOX_V        "\xe2\x94\x82" /* │ */
#define BOX_TL       "\xe2\x94\x8c" /* ┌ */
#define BOX_TR       "\xe2\x94\x90" /* ┐ */
#define BOX_BL       "\xe2\x94\x94" /* └ */
#define BOX_BR       "\xe2\x94\x98" /* ┘ */

/* str in the 6x6 font: 'cell' (UTF-8) in the color where the font has a 1; a run of cells goes in one write */
void print_big_text (const char *str, const char *cell, int color) {

	const int lot = 6;
	const int begin = 32;
	int len = strlen(str), cell_len = strlen(cell);
	char *run = malloc(len * lot * cell_len + 1);

	if (!run)
		return;
	for (int y = 0; y < lot; y++) {
		int n = 0;

		printf(" ");
		for (int i = 0; i < len; i++) {
			int c = (unsigned char) str[i] - begin;

			if (c < 0 || c >= ASCII_LEN)
				c = 0;
			for (int x = 0; x < lot; x++) {
				if (font_char[c][x + (y * lot)] == '1') {
					memcpy(run + n, cell, cell_len);
					n += cell_len;
					continue;
				}
				if (n) {
					run[n] = 0;
					print(color, "%s", run);
					n = 0;
				}
				printf(" ");
			}
		}
		if (n) {
			run[n] = 0;
			print(color, "%s", run);
		}
		printf("\n");
	}
	free(run);
}

void ant_logo (int width) {

	static const char skull[] =
	               "00000000124442100000000"
	               "00000014444444441000000"
	               "00000144444444444100000"
	               "00001444444444444410000"
	               "00003444444444444430000"
	               "00012344444444444321000"
	               "00011244444444444211000"
	               "00011244444444444211000"
	               "00011431112321113411000"
	               "00012410001410001421000"
	               "14312400001410000421341"
	               "44413400014241000431444"
	               "44413411242124211431444"
	               "13442444241014244424431"
	               "00131442141214124413100"
	               "00000011124442111000000"
	               "00000000244444200000000"
	               "00001313031413031310000"
	               "00014414200000241441000"
	               "13444414414141441444431"
	               "44444431444444413444444"
	               "44431000144444100013444"
	               "14300000012321000000341";

	/* each digit of the skull is a shade, drawn twice (a cell is twice as tall as it is wide) */
	static const char *shade[] = { " ", SHADE_LIGHT, SHADE_MEDIUM, SHADE_DARK, BLOCK_FULL };
	enum { SKULL_W = 23, BAR_W = 39 };
	char bar[BAR_W * 3 + 1], row[SKULL_W * 2 * 3 + 1], pad[64];
	int n;

	if (width < 0)
		width = 0;
	if (width > (int) sizeof pad - 3)
		width = (int) sizeof pad - 3;

	for (n = 0; n < BAR_W; n++)
		memcpy(bar + n * 3, BOX_H, 3);
	bar[BAR_W * 3] = 0;
	memset(pad, ' ', width + 2);
	pad[width + 2] = 0;
	printf("\n%s" BOX_TL "%s" BOX_TR "\n%s" BOX_V "        ANTHROPOID SHELL [v2025]       " BOX_V "\n%s" BOX_BL "%s" BOX_BR "\n",
		pad, bar, pad, pad, bar);

	pad[width] = 0;
	for (int j = 0; skull[j]; ) {
		n = 0;
		for (int i = 0; i < SKULL_W; i++, j++) {
			const char *s = shade[skull[j] - '0'];
			int k = strlen(s);
			memcpy(row + n, s, k);
			memcpy(row + n + k, s, k);
			n += 2 * k;
		}
		row[n] = 0;
		printf("%s%s\n", pad, row);
	}
	printf("\n");
}

/* error message on the error output (red in the console), so "2>" can redirect it */
void ant_error (const wchar_t *fmt, ...) {

	static wchar_t buf[2048];
	static char u8[2048 * 3];
	HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
	CONSOLE_SCREEN_BUFFER_INFO info;
	bool console = GetConsoleScreenBufferInfo(err, &info);
	DWORD n;
	va_list ap;

	va_start(ap, fmt);
	vswprintf(buf, 2048, fmt, ap);
	va_end(ap);
	wcscat(buf, L"\r\n");

	fflush(stdout);
	if (console) {
		SetConsoleTextAttribute(err, CLEAR|RED);
		WriteConsoleW(err, buf, (DWORD) wcslen(buf), &n, NULL);
		SetConsoleTextAttribute(err, info.wAttributes);
	} else {
		int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof u8, NULL, NULL);
		if (len > 1)
			WriteFile(err, u8, len - 1, &n, NULL);
	}
}

/* wide text on standard output in UTF-8, the shell's code page (wprintf would turn accents into '?') */
void ant_write (const wchar_t *text) {

	int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
	char *u8 = (n > 1) ? malloc(n) : NULL;

	if (u8 && WideCharToMultiByte(CP_UTF8, 0, text, -1, u8, n, NULL, NULL))
		fputs(u8, stdout);
	free(u8);
}
