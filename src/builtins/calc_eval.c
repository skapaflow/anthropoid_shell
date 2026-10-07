/*************************************************************
 * Calculator engine: no screen, tested by tests/calc_test.c *
 *                                                           *
 *   infix   2+3, (1+2)*3, 2^3^2, -(2+3), 2*-3, .5+1, 5=5    *
 *           10%3, sqrt(16), abs(-5), 2*pi, e^2              *
 *   RPN     2 3 +, 3 4 + 2 *, pi 2 *  (tried when the infix *
 *           reading fails and the text has blanks)          *
 *                                                           *
 * Precedence, lowest first: =  then + -  then * / %  then   *
 * unary - +  then ^ (right to left, so -2^2 is -4 and       *
 * 2^3^2 is 512).                                            *
 * Functions (with the parenthesis right after the name):    *
 * sqrt abs round log (base 10) ln. Constants: pi e.         *
 *************************************************************/

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "calc_eval.h"

#define MAX_DEPTH 200

typedef struct {
	const char *s;     /* cursor */
	char msg[96];      /* first error, empty while all is well */
	int code;          /* exit code of the error: 2 syntax, 1 arithmetic */
	int depth;
} CALC;

typedef struct {
	const char *name;
	double (*fn) (double);
} FUNC;

static double fn_ln (double x) { return log(x); }
static double fn_log (double x) { return log10(x); }

static const FUNC funcs[] = {
	{ "sqrt", sqrt }, { "abs", fabs }, { "round", round }, { "log", fn_log }, { "ln", fn_ln },
};

static double parse_cmp (CALC *c);
static double parse_unary (CALC *c);

static const FUNC *find_func (const char *w, size_t len) {

	for (size_t i = 0; i < sizeof funcs / sizeof funcs[0]; i++)
		if (strlen(funcs[i].name) == len && !strncmp(funcs[i].name, w, len))
			return &funcs[i];
	return NULL;
}

static bool find_const (const char *w, size_t len, double *value) {

	if (len == 2 && !strncmp(w, "pi", 2)) {
		*value = 3.14159265358979323846;
		return true;
	}
	if (len == 1 && *w == 'e') {
		*value = 2.71828182845904523536;
		return true;
	}
	return false;
}

/*
 * A calculator expression: only digits, blanks and . + - * / ^ % ( ) =, plus the names
 * of the functions (followed by '(') and of the constants (pi, e), with a digit or an
 * operator somewhere. So "pi", "e" and "sqrt 9" stay commands, and "2 + x" too.
 */
bool ant_calc_is_expression (const char *str) {

	bool calc = false;
	double unused;

	if (!str)
		return false;
	for (const char *s = str; *s; ) {
		if (isalpha((unsigned char) *s)) {
			const char *w = s;

			while (isalnum((unsigned char) *s) || *s == '_')
				s++;
			if (find_func(w, s - w)) {
				if (*s != '(')
					return false;
			} else if (!find_const(w, s - w, &unused))
				return false;
			continue;
		}
		if (isdigit((unsigned char) *s) || strchr("(+-*/^)%", *s))
			calc = true;
		else if (!strchr(" \t\n\r.=", *s))
			return false;
		s++;
	}
	return calc;
}

static void fail (CALC *c, int code, const char *fmt, ...) {

	va_list ap;

	if (c->msg[0])
		return;
	va_start(ap, fmt);
	vsnprintf(c->msg, sizeof c->msg, fmt, ap);
	va_end(ap);
	c->code = code;
}

static void skip_blanks (CALC *c) {

	while (*c->s == ' ' || *c->s == '\t' || *c->s == '\n' || *c->s == '\r')
		c->s++;
}

/* an operand or a character that cannot be there */
static void syntax (CALC *c) {

	if (*c->s)
		fail(c, 2, "syntax error near '%c'", *c->s);
	else
		fail(c, 2, "unexpected end of the expression");
}

static bool nest (CALC *c) {

	if (++c->depth > MAX_DEPTH) {
		fail(c, 2, "expression too deeply nested");
		return false;
	}
	return true;
}

/* '(' expr ')' with the cursor on the '(' */
static double parse_group (CALC *c) {

	double v;

	if (!nest(c))
		return 0;
	c->s++;
	v = parse_cmp(c);
	skip_blanks(c);
	if (c->msg[0])
		return 0;
	if (*c->s != ')') {
		if (*c->s)
			fail(c, 2, "syntax error near '%c'", *c->s);
		else
			fail(c, 2, "missing ')'");
		return 0;
	}
	c->s++;
	c->depth--;
	return v;
}

static double parse_number (CALC *c) {

	char *end;
	double v = strtod(c->s, &end);

	c->s = end;
	/* strtod takes "1." and "1.2"; a second '.' (1.2.3) is an error */
	if (*c->s == '.') {
		fail(c, 2, "syntax error near '%c'", *c->s);
		return 0;
	}
	return v;
}

/* a name: constant, or function with its argument */
static double parse_name (CALC *c) {

	const char *w = c->s;
	size_t len;
	double v;
	const FUNC *f;

	while (isalnum((unsigned char) *c->s) || *c->s == '_')
		c->s++;
	len = c->s - w;
	if ((f = find_func(w, len))) {
		if (*c->s != '(') {
			fail(c, 2, "missing '(' after '%s'", f->name);
			return 0;
		}
		v = parse_group(c);
		return c->msg[0] ? 0 : f->fn(v);
	}
	if (find_const(w, len, &v))
		return v;
	fail(c, 2, "unknown name '%.*s'", (int) (len > 30 ? 30 : len), w);
	return 0;
}

static double parse_primary (CALC *c) {

	skip_blanks(c);
	if (*c->s == '(')
		return parse_group(c);
	if (isdigit((unsigned char) *c->s) || (*c->s == '.' && isdigit((unsigned char) c->s[1])))
		return parse_number(c);
	if (isalpha((unsigned char) *c->s))
		return parse_name(c);
	syntax(c);
	return 0;
}

/* primary [^ unary]: right to left because the exponent is a unary, which comes back here */
static double parse_power (CALC *c) {

	double base = parse_primary(c);

	skip_blanks(c);
	if (!c->msg[0] && *c->s == '^') {
		double e;

		c->s++;
		if (!nest(c))
			return 0;
		e = parse_unary(c);
		c->depth--;
		if (c->msg[0])
			return 0;
		if (base == 0 && e < 0) {
			fail(c, 1, "division by zero");
			return 0;
		}
		return pow(base, e);
	}
	return base;
}

static double parse_unary (CALC *c) {

	skip_blanks(c);
	if (*c->s == '-' || *c->s == '+') {
		char op = *c->s++;
		double v;

		if (!nest(c))
			return 0;
		v = parse_unary(c);
		c->depth--;
		return op == '-' ? -v : v;
	}
	return parse_power(c);
}

static double parse_term (CALC *c) {

	double v = parse_unary(c);

	for (;;) {
		char op;
		double r;

		skip_blanks(c);
		if (c->msg[0] || (*c->s != '*' && *c->s != '/' && *c->s != '%'))
			return v;
		op = *c->s++;
		r = parse_unary(c);
		if (c->msg[0])
			return 0;
		if (op == '*')
			v *= r;
		else if (r == 0) {
			fail(c, 1, "division by zero");
			return 0;
		} else if (op == '/')
			v /= r;
		else
			v = fmod(v, r);
	}
}

static double parse_sum (CALC *c) {

	double v = parse_term(c);

	for (;;) {
		char op;
		double r;

		skip_blanks(c);
		if (c->msg[0] || (*c->s != '+' && *c->s != '-'))
			return v;
		op = *c->s++;
		r = parse_term(c);
		if (c->msg[0])
			return 0;
		v = op == '+' ? v + r : v - r;
	}
}

static double parse_cmp (CALC *c) {

	double v = parse_sum(c);

	skip_blanks(c);
	if (!c->msg[0] && *c->s == '=') {
		double r;

		c->s++;
		r = parse_sum(c);
		if (c->msg[0])
			return 0;
		return v == r;
	}
	return v;
}

/* the message goes in 'err' (fmt has one %s for 'arg'); frees the stack */
static bool rpn_fail (double *st, char *err, size_t errsz, int *code, int c, const char *fmt, const char *arg) {

	snprintf(err, errsz, fmt, arg);
	*code = c;
	free(st);
	return false;
}

/* RPN: blank-separated numbers, constants and one-character operators */
static bool eval_rpn (const char *str, double *value, char *err, size_t errsz, int *code) {

	double *st = malloc((strlen(str) / 2 + 2) * sizeof(double));
	int n = 0;
	const char *s = str;

	if (!st)
		return rpn_fail(st, err, errsz, code, 1, "%s", "out of memory");
	while (*s) {
		char tok[64];
		size_t len = 0;
		double k;

		while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
			s++;
		if (!*s)
			break;
		while (s[len] && s[len] != ' ' && s[len] != '\t' && s[len] != '\n' && s[len] != '\r')
			len++;
		if (len >= sizeof tok)
			return rpn_fail(st, err, errsz, code, 2, "%s", "token too long");
		memcpy(tok, s, len);
		tok[len] = 0;
		s += len;

		if (len == 1 && strchr("+-*/%^=", tok[0])) {
			double b, a, r = 0;

			if (n < 2)
				return rpn_fail(st, err, errsz, code, 2, "too few operands for '%s'", tok);
			b = st[--n];
			a = st[--n];
			if (((tok[0] == '/' || tok[0] == '%') && b == 0) || (tok[0] == '^' && a == 0 && b < 0))
				return rpn_fail(st, err, errsz, code, 1, "%s", "division by zero");
			switch (tok[0]) {
				case '+': r = a + b; break;
				case '-': r = a - b; break;
				case '*': r = a * b; break;
				case '/': r = a / b; break;
				case '%': r = fmod(a, b); break;
				case '=': r = a == b; break;
				case '^': r = pow(a, b); break;
			}
			st[n++] = r;
		} else if (find_const(tok, len, &k)) {
			st[n++] = k;
		} else {
			char *end;
			double v = strtod(tok, &end);

			if (end == tok || *end)
				return rpn_fail(st, err, errsz, code, 2, isalpha((unsigned char) tok[0]) ? "unknown name '%s'" : "syntax error near '%s'", tok);
			st[n++] = v;
		}
	}
	if (n != 1)
		return rpn_fail(st, err, errsz, code, 2, "%s", n ? "too many values left on the stack" : "unexpected end of the expression");
	*value = st[0];
	free(st);
	return true;
}

/*
 * The value of 'expr': infix, or RPN when that fails and there are blanks.
 * false: err has the message (UTF-8, no prefix) and *code the exit code (2 syntax, 1 arithmetic).
 */
bool ant_calc_eval (const char *expr, double *value, char *err, size_t errsz, int *code) {

	CALC c = { expr, "", 0, 0 };
	double v = parse_cmp(&c);

	skip_blanks(&c);
	if (!c.msg[0] && *c.s)
		fail(&c, 2, "syntax error near '%c'", *c.s);
	if (c.msg[0]) {
		bool blanks = strpbrk(expr, " \t") != NULL;

		if (blanks && eval_rpn(expr, &v, err, errsz, code))
			c.msg[0] = 0;
		else if (blanks && *code == 1)
			return false; /* it read as RPN and failed on arithmetic (1 0 /): that is the error */
		else {
			snprintf(err, errsz, "%s", c.msg);
			*code = c.code;
			return false;
		}
	}
	if (!isfinite(v)) {
		snprintf(err, errsz, isnan(v) ? "result is not a number" : "result out of range");
		*code = 1;
		return false;
	}
	*value = v;
	return true;
}

/* the value as text: integers in full, the rest with up to 10 decimals without trailing zeros */
void ant_calc_format (double v, char *buf, size_t size) {

	if (v == 0)
		v = 0; /* no "-0" */
	if (v == floor(v)) {
		snprintf(buf, size, "%.0f", v);
		return;
	}
	snprintf(buf, size, "%.10f", v);
	if (strchr(buf, '.')) {
		char *e = buf + strlen(buf) - 1;

		while (e > buf && *e == '0')
			*e-- = 0;
		if (*e == '.')
			*e = 0;
	}
	/* too small for 10 decimals: scientific notation, so it does not look like 0 */
	if (!strcmp(buf, "0") || !strcmp(buf, "-0"))
		snprintf(buf, size, "%.6g", v);
}
