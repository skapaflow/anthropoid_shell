#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "string_util.h"

bool isnumbers (char *s) {

	/* considering negative numbers */
	if ((*s == '-' || *s == '+') && isdigit(s[1]))
		return true;

	/* float numbers */
	while (*s) {
		if (!isdigit(*s) && *s != '.')
			return false;
		s++;
	}
	return true;
}

char *strsep (char **stringp, const char *delim) {

	char *rv = *stringp;
	if (rv) {
		*stringp += strcspn(*stringp, delim);
		if (**stringp)
			*(*stringp)++ = '\0';
		else
			*stringp = 0;
	}
	return rv;
}

/* removes every character listed in c */
char *strrmc (char *s, const char *c) {

	int i = 1;
	char *v = s;
	char *r = s;
	const char *t;
	do {
		t = c;
		while (*t && (i = !(*v == *t++)));
		if (i)
			*r++ = *v;
	} while (*v++);
	*--r = 0;
	return s;
}

void trim (char *str) {

	char *start = str;
	while (isspace((unsigned char)*start)) start++;  // Skips to the first non-space character
	char *end = str + strlen(str) - 1;
	while (end > start && isspace((unsigned char)*end)) end--;  // Walks back to the last non-space character
	size_t len = end - start + 1;
	memmove(str, start, len);  // Moves the useful part to the start
	str[len] = '\0';  // Adds the null terminator
}

/* removes null and space characters from the string borders */
char *strtrim (char *s1, const char *s2) {

	if (s1 != NULL && s2 != NULL) {
		while (*s2 && (*s2 > 0 && *s2 <= 32) && *s2++);
		size_t i = strlen(s2);
		strcpy(s1, s2);
		while (*s1 && (s1[i] >= 0 && s1[i] <= 32))
			s1[i--] = 0;
	}
	return s1;
}


/* normalize numbers (000040 -> 40) or (1.1000 -> 1.1) */
char *float_trim (char *str) {

	int len = strlen(str);
	if (strchr(str, '.')) {
		char *s = (str + (len - 1));
		if (len > 1)
			while (*s == '0')
				*s-- = 0;
		*s = (*s == '.' ? 0 : *s);
	}
	return str;
}

char *to_narrowchar (char *des, const wchar_t* wstr) {

	int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
	WideCharToMultiByte(CP_UTF8, 0, wstr, -1, des, size_needed, NULL, NULL);
	return des;
}

/* convert char* to wchar_t* */
wchar_t *to_widechar (wchar_t *wdsc, const char *src) {

	int size_needed = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
	MultiByteToWideChar(CP_UTF8, 0, src, -1, wdsc, size_needed);
	return wdsc;
}