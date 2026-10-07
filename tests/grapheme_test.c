/*************************************************************
 * Test of the unicode module (src/types/unicode.c).            *
 *                                                           *
 * usage: grapheme_test.exe                                    *
 *                                                           *
 * Runs the official GraphemeBreakTest cases (Unicode 16)    *
 * forward (ucd_grapheme_next) and backward                  *
 * (ucd_grapheme_prev), and checks widths against the values    *
 * expected by the Windows Terminal tests.                   *
 *************************************************************/

#include <stdio.h>
#include <wchar.h>
#include <stdbool.h>
#include <windows.h>

#include "../src/types/unicode.h"

typedef struct {
	const wchar_t *comment;
	const wchar_t *graphemes[4];
} GRAPHEME_BREAK_TEST;

static const GRAPHEME_BREAK_TEST tests[] = {
#include "grapheme_break_data.inc"
};

static int failures = 0, total = 0;

static void report (bool ok, const wchar_t *name) {

	char buf[1024];

	total++;
	if (ok)
		return;
	failures++;
	WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof buf, NULL, NULL);
	printf("FAIL  %s\n", buf);
}

/* expected boundaries: end of each grapheme */
static int expected_bounds (const GRAPHEME_BREAK_TEST *t, wchar_t *text, int *bounds) {

	int nb = 0, pos = 0;

	text[0] = 0;
	for (int g = 0; g < 4 && t->graphemes[g]; g++) {
		wcscat(text, t->graphemes[g]);
		pos += wcslen(t->graphemes[g]);
		bounds[nb++] = pos;
	}
	return nb;
}

static bool forward_ok (const wchar_t *text, const int *bounds, int nb) {

	int len = wcslen(text), i = 0, k = 0;

	while (i < len) {
		i = ucd_grapheme_next(text, len, i, NULL);
		if (k >= nb || bounds[k] != i)
			return false;
		k++;
	}
	return k == nb;
}

static bool backward_ok (const wchar_t *text, const int *bounds, int nb) {

	int len = wcslen(text), i = len, k = nb - 1;

	while (i > 0) {
		if (k < 0 || bounds[k] != i)
			return false;
		i = ucd_grapheme_prev(text, len, i);
		k--;
	}
	return k == -1;
}

/* widths of the graphemes of 'text' must be 'widths' (terminated by -1) */
static void check_widths (const wchar_t *name, const wchar_t *text, const int *widths) {

	int len = wcslen(text), i = 0, k = 0, w;
	bool ok = true;

	while (i < len) {
		i = ucd_grapheme_next(text, len, i, &w);
		if (widths[k] < 0 || widths[k] != w)
			ok = false;
		k++;
	}
	report(ok && widths[k] < 0, name);
}

int main (void) {

	int n = sizeof tests / sizeof tests[0];
	int total_before, failures_before;

	for (int t = 0; t < n; t++) {
		wchar_t text[64];
		int bounds[4];
		int nb = expected_bounds(&tests[t], text, bounds);
		report(forward_ok(text, bounds, nb), tests[t].comment);
		report(backward_ok(text, bounds, nb), tests[t].comment);
	}
	printf("GraphemeBreakTest: %d of %d (forward and backward)\n", total - failures, total);
	total_before = total;
	failures_before = failures;

	/* larguras esperadas pelos testes do Windows Terminal (BasicGraphemes, ChunkedText, AmbiguousWidthPolicy) */
	{
		static const int w1[] = { 1, 1, 1, -1 };
		static const int w2[] = { 2, 1, -1 };
		static const int w3[] = { 1, -1 };
		check_widths(L"a+U+0363 e+U+0364x2 i+U+0365: 1 1 1", L"aͣeͤͤiͥ", w1);
		check_widths(L"rainbow flag (ZWJ) + a: 2 1", L"\U0001F3F3️‍\U0001F308a", w2);
		check_widths(L"ambiguous (→): 1", L"→", w3);
	}

	/* basic widths */
	{
		static const int wide[] = { 2, -1 };
		static const int narrow[] = { 1, -1 };
		check_widths(L"CJK 中: 2", L"中", wide);
		check_widths(L"emoji 🙂: 2", L"\U0001F642", wide);
		check_widths(L"family ZWJ: 2", L"\U0001F468‍\U0001F469‍\U0001F467", wide);
		check_widths(L"heart + VS16: 2", L"❤️", wide);
		check_widths(L"heart without VS16: 1", L"❤", narrow);
		check_widths(L"bandeira 🇧🇷: 2", L"\U0001F1E7\U0001F1F7", wide);
		check_widths(L"hangul 한: 2", L"한", wide);
		check_widths(L"fullwidth Ａ: 2", L"Ａ", wide);
		check_widths(L"e + U+0301: 1", L"é", narrow);
		check_widths(L"ç: 1", L"ç", narrow);
	}

	printf("widths: %d of %d\n", (total - total_before) - (failures - failures_before), total - total_before);
	printf("\n%d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
