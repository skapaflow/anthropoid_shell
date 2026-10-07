#ifndef ANT_SHELL_PARSER_H
#define ANT_SHELL_PARSER_H

#include <wchar.h>
#include <stdbool.h>

/* redirections, in the order they appear (the order matters: "> f 2>&1" != "2>&1 > f") */
enum {
	REDIR_IN,         /* < file   */
	REDIR_OUT,        /* > file   */
	REDIR_APPEND,     /* >> file  */
	REDIR_ERR,        /* 2> file  */
	REDIR_ERR_APPEND, /* 2>> file */
	REDIR_ERR_TO_OUT  /* 2>&1    */
};

/* how a pipeline connects to the next one */
enum {
	OP_END, /* last */
	OP_SEQ, /* ;  */
	OP_AND, /* && */
	OP_OR,  /* || */
	OP_BG   /* &  */
};

/* how each character of a word was written: decides what expand.c may expand */
enum {
	QUOTE_NONE,   /* bare: $, ~, * and ? are expanded */
	QUOTE_DOUBLE, /* "...": only $ is expanded */
	QUOTE_SINGLE  /* '...': nothing is expanded */
};

typedef struct {
	int kind;
	wchar_t *path; /* NULL for 2>&1 */
	char *lit;     /* quoting of each character of path (see ANT_CMD), NULL for 2>&1 */
} ANT_REDIR;

typedef struct {
	int argc;
	wchar_t **argv; /* words already without quotes */
	char **lit;     /* per word, one byte per character: QUOTE_NONE, QUOTE_DOUBLE or QUOTE_SINGLE */
	int nredir;
	ANT_REDIR *redir;
} ANT_CMD;

typedef struct {
	int ncmd;
	ANT_CMD *cmd; /* a | b | c */
	int op;
} ANT_PIPELINE;

typedef struct {
	int n;
	ANT_PIPELINE *p;
	wchar_t error[128]; /* syntax error message */
} ANT_LIST;

extern bool ant_parse (const wchar_t *, ANT_LIST *);
extern void ant_parse_free (ANT_LIST *);
extern void ant_parse_set_alias (const wchar_t *(*) (const wchar_t *));

#endif
