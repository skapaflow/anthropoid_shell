/*************************************************************
 * Test of the config module (src/config/config.c): sections,     *
 * values, warnings, $VAR and the conversion of the old files. *
 * No screen and no file (ant_config_parse takes the text).  *
 *************************************************************/

#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <stdbool.h>
#include <windows.h>

#include "../src/config/config.h"

static int failures = 0, total = 0;

static void report (bool ok, const char *name, const wchar_t *want, const wchar_t *got) {

	char a[4096], b[4096];

	total++;
	if (ok)
		return;
	failures++;
	WideCharToMultiByte(CP_UTF8, 0, want ? want : L"(none)", -1, a, sizeof a, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, got ? got : L"(none)", -1, b, sizeof b, NULL, NULL);
	printf("FAIL  %s\n      expected: [%s]\n      got:      [%s]\n", name, a, b);
}

static void check (const char *name, const wchar_t *want, const wchar_t *got) {

	report(want && got ? !wcscmp(want, got) : want == got, name, want, got);
}

static void check_int (const char *name, int want, int got) {

	wchar_t a[32], b[32];

	swprintf(a, 32, L"%d", want);
	swprintf(b, 32, L"%d", got);
	report(want == got, name, a, b);
}

/* expanded value of the i-th entry of the section */
static const wchar_t *value (int section, int i) {

	static wchar_t out[4096];
	return ant_config_value(section, i, out, 4096) ? out : NULL;
}

static const wchar_t *link_of (const wchar_t *name) {

	static wchar_t out[4096];
	return ant_config_link(name, out, 4096) ? out : NULL;
}

static const wchar_t *expand (const wchar_t *in) {

	static wchar_t out[4096];
	ant_config_expand(in, out, 4096);
	return out;
}

int main (void) {

	wchar_t *text;

	/* ---------- sections and values ---------- */
	ant_config_parse(
		L"\xFEFF# comment\r\n"
		L"[path]\r\n"
		L"C:\\TCC\r\n"
		L"  C:\\Program Files\\nodejs\\  \r\n"
		L"\"C:\\with space \"\r\n"
		L"\r\n"
		L"[EXPORT]\r\n"
		L"EDITOR = subl\r\n"
		L"EMPTY =\r\n"
		L"[alias]\r\n"
		L"ll = fs -l\r\n"
		L"up=cd.. && ll\r\n"
		L"q = 'quotes vanish'\r\n"
		L"[link]\r\n"
		L"doc  = C:\\Users\\user\\Documents\r\n"
		L"vid = D:\\VIDEOS\r\n"
		L"garbage = shell:RecycleBinFolder");

	check("no warnings", L"", ant_config_warnings());
	check_int("[path]: 3 pastas", 3, ant_config_count(CONFIG_PATH));
	check("[path] 1ª", L"C:\\TCC", value(CONFIG_PATH, 0));
	check("[path] no spaces at the ends", L"C:\\Program Files\\nodejs\\", value(CONFIG_PATH, 1));
	check("[path] \"quotes\" keep a space", L"C:\\with space ", value(CONFIG_PATH, 2));
	{
		wchar_t dirs[4096];
		ant_config_path(dirs, 4096);
		check("ant_config_path joins with ;", L"C:\\TCC;C:\\Program Files\\nodejs\\;C:\\with space ", dirs);
	}
	check("section is case-insensitive", L"EDITOR", ant_config_name(CONFIG_EXPORT, 0));
	check("[export] value", L"subl", value(CONFIG_EXPORT, 0));
	check("[export] empty value", L"", value(CONFIG_EXPORT, 1));
	check("[alias] ll", L"fs -l", ant_config_alias(L"ll"));
	check("[alias] case-insensitive", L"fs -l", ant_config_alias(L"LL"));
	check("[alias] no space around the =", L"cd.. && ll", ant_config_alias(L"up"));
	check("[alias] 'quotes' disappear", L"quotes vanish", ant_config_alias(L"q"));
	check("[alias] inexistente", NULL, ant_config_alias(L"missing"));
	check("[link] with spaces", L"C:\\Users\\user\\Documents", link_of(L"DOC"));
	check("[link] last line without \\n", L"shell:RecycleBinFolder", link_of(L"garbage"));
	check("[link] inexistente", NULL, link_of(L"missing"));
	check("[link] does not see aliases", NULL, link_of(L"ll"));

	/* ---------- avisos ---------- */
	ant_config_parse(
		L"stray\n"
		L"[link]\n"
		L"a = 1\n"
		L"a = 2\n"
		L"no equals sign\n"
		L"b c = 3\n"
		L"d =\n"
		L"[other]\n"
		L"x = y\n"
		L"[alias]\n"
		L"a|b = x\n");
	check("warnings with the line number",
		L"config.ant, line 1: \"stray\" outside a section\n"
		L"config.ant, line 4: a repeated; the first one wins\n"
		L"config.ant, line 5: missing '=' in \"no equals sign\"\n"
		L"config.ant, line 6: invalid name \"b c\"\n"
		L"config.ant, line 7: d has no value\n"
		L"config.ant, line 8: unknown section [other] (use [path], [export], [alias] or [link])\n"
		L"config.ant, line 11: invalid name \"a|b\"\n",
		ant_config_warnings());
	check("repeat: the first one wins", L"1", link_of(L"a"));

	/* ---------- $VAR ---------- */
	SetEnvironmentVariableW(L"ANT_T", L"C:\\x");
	SetEnvironmentVariableW(L"ANT_EMPTY", NULL);
	check("$VAR", L"C:\\x\\bin", expand(L"$ANT_T\\bin"));
	check("${VAR}", L"C:\\xy", expand(L"${ANT_T}y"));
	check("$ with no name stays", L"a$ b$", expand(L"a$ b$"));
	check("a variable that does not exist disappears", L"[]", expand(L"[$ANT_EMPTY]"));
	check("${ without closing stays", L"${ANT_T", expand(L"${ANT_T"));
	check("the name runs to the end of the letters", L"C:\\x.txt", expand(L"$ANT_T.txt"));

	ant_config_parse(
		L"[export]\n"
		L"ANT_A = $ANT_T\\a\n"
		L"[link]\n"
		L"x = $ANT_T\\link\n"
		L"y = '$ANT_T\\literal'\n"
		L"[alias]\n"
		L"z = echo $ANT_T\n"
		L"[path]\n"
		L"${ANT_T}\\bin\n");
	check("$VAR no [link]", L"C:\\x\\link", link_of(L"x"));
	check("'single quotes' do not expand", L"$ANT_T\\literal", link_of(L"y"));
	check("[alias] stays as it is", L"echo $ANT_T", ant_config_alias(L"z"));
	check("${VAR} no [path]", L"C:\\x\\bin", value(CONFIG_PATH, 0));
	check("[export] before applying: text of the file", L"$ANT_T\\a", ant_config_raw(CONFIG_EXPORT, 0));

	/* ---------- the template of a new config.ant ---------- */
	text = ant_config_template(L"C:\\Program Files\\AntShell\\bin");
	ant_config_parse(text);
	check("template: no warnings", L"", ant_config_warnings());
	check_int("template: one folder", 1, ant_config_count(CONFIG_PATH));
	check("template: the bin folder is in [path]", L"C:\\Program Files\\AntShell\\bin", value(CONFIG_PATH, 0));
	check_int("template: no shortcuts, aliases or variables", 0, ant_config_count(CONFIG_LINK) +
		ant_config_count(CONFIG_ALIAS) + ant_config_count(CONFIG_EXPORT));
	free(text);

	text = ant_config_template(NULL);
	ant_config_parse(text);
	check("template without a folder: no warnings", L"", ant_config_warnings());
	check_int("template without a folder: empty", 0, ant_config_count(CONFIG_PATH) + ant_config_count(CONFIG_LINK) +
		ant_config_count(CONFIG_ALIAS) + ant_config_count(CONFIG_EXPORT));
	free(text);

	ant_config_parse(NULL);
	check_int("empty", 0, ant_config_count(CONFIG_LINK));

	printf("config: %d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
