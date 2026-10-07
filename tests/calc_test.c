/*************************************************************
 * Test of the calculator engine (src/builtins/calc_eval.c). *
 *                                                           *
 * Each case checks the result text (ant_calc_format)        *
 * or "error: message [code]".                               *
 *************************************************************/

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "../src/builtins/calc_eval.h"

static int failures = 0, total = 0;

static void check (const char *expr, const char *want) {

	char got[300], err[200], num[100];
	double v;
	int code = 0;

	if (ant_calc_eval(expr, &v, err, sizeof err, &code)) {
		ant_calc_format(v, num, sizeof num);
		snprintf(got, sizeof got, "%s", num);
	} else
		snprintf(got, sizeof got, "error: %s [%d]", err, code);
	total++;
	if (strcmp(got, want)) {
		failures++;
		printf("FAIL  %-28s wants '%s', got '%s'\n", expr, want, got);
	}
}

static void check_is (const char *s, bool want) {

	total++;
	if (ant_calc_is_expression(s) != want) {
		failures++;
		printf("FAIL  is_expression('%s') should be %d\n", s, want);
	}
}

int main (void) {

	/* basico e precedencia */
	check("2+3", "5");
	check("7-3-2", "2");
	check("8/2/2", "2");
	check("2+3*4-5", "9");
	check("(2+3)*4", "20");
	check("2*(3+4)*5", "70");
	check("10/4", "2.5");
	check("0.1+0.2", "0.3");
	check("1.5*2", "3");
	check("  2 +  3 ", "5");
	check("1000000*1000000", "1000000000000");

	/* power: right to left, stronger than * and than the sign */
	check("2^3", "8");
	check("2*3^2", "18");
	check("2^3^2", "512");
	check("-2^2", "-4");
	check("(-2)^2", "4");
	check("2^-1", "0.5");
	check("2^0.5*2^0.5", "2");

	/* sinal unario */
	check("-(2+3)", "-5");
	check("2*-3", "-6");
	check("-3+5", "2");
	check("--3", "3");
	check("+4", "4");
	check("5 - -3", "8");

	/* decimais */
	check(".5+1", "1.5");
	check("1.+1", "2");
	check("0.000001", "0.000001");
	check("1/3", "0.3333333333");
	check("0.00000000001", "1e-11");

	/* igualdade */
	check("5=5", "1");
	check("2+3=5", "1");
	check("2=3", "0");

	/* RPN */
	check("2 3 +", "5");
	check("3 4 + 2 *", "14");
	check("2 3 ^", "8");
	check("10 4 /", "2.5");
	check("2 3 -", "-1");
	check("2 3 *", "6");
	check("5 5 =", "1");
	check("5 1 2 + 4 * + 3 -", "14");

	/* erros */
	check("1/0", "error: division by zero [1]");
	check("0^-1", "error: division by zero [1]");
	check("1 0 /", "error: division by zero [1]");
	check("(2+3", "error: missing ')' [2]");
	check("2+3)", "error: syntax error near ')' [2]");
	check("()", "error: syntax error near ')' [2]");
	check("2+", "error: unexpected end of the expression [2]");
	check("+", "error: unexpected end of the expression [2]");
	check("*3", "error: syntax error near '*' [2]");
	check("2 3", "error: syntax error near '3' [2]");
	check("2..3", "error: syntax error near '.' [2]");
	check("1.2.3", "error: syntax error near '.' [2]");
	check("2 +", "error: unexpected end of the expression [2]");
	check("(-8)^0.5", "error: result is not a number [1]");
	check("10^400", "error: result out of range [1]");

	/* modulo */
	check("10%3", "1");
	check("10 % 3", "1");
	check("7+10%3", "8");
	check("5.5%2", "1.5");
	check("-7%3", "-1");
	check("2*7%4", "2");
	check("10 3 %", "1");
	check("5%0", "error: division by zero [1]");
	check("5 0 %", "error: division by zero [1]");

	/* funcoes */
	check("sqrt(16)", "4");
	check("sqrt(2)^2", "2");
	check("sqrt(abs(-16))", "4");
	check("sqrt(9)+sqrt(16)", "7");
	check("-sqrt(4)", "-2");
	check("2^sqrt(4)", "4");
	check("abs(-5)", "5");
	check("abs(3-10)*pi", "21.9911485751");
	check("abs(-2)^2", "4");
	check("round(2.5)", "3");
	check("round(-2.5)", "-3");
	check("round(3.14159)", "3");
	check("log(1000)", "3");
	check("log(100)", "2");
	check("ln(e)", "1");
	check("ln(1)", "0");
	check("sqrt(-1)", "error: result is not a number [1]");
	check("log(0)", "error: result out of range [1]");
	check("sqrt 9", "error: missing '(' after 'sqrt' [2]");
	check("sqrt(", "error: unexpected end of the expression [2]");
	check("sqrt(4", "error: missing ')' [2]");
	check("foo(2)", "error: unknown name 'foo' [2]");

	/* constantes */
	check("pi+0", "3.1415926536");
	check("2*pi", "6.2831853072");
	check("e^2", "7.3890560989");
	check("pi 2 *", "6.2831853072");
	check("e 1 +", "3.7182818285");
	check("x 2 +", "error: unknown name 'x' [2]");

	/* long expression: no fixed buffer */
	{
		char big[8000] = "1";

		for (int i = 0; i < 1500; i++)
			strcat(big, "+1");
		check(big, "1501");
	}
	/* nesting too deep: an error, not a stack overflow */
	{
		char deep[2100];

		memset(deep, '(', 1000);
		strcpy(deep + 1000, "1");
		memset(deep + 1001, ')', 1000);
		deep[2001] = 0;
		check(deep, "error: expression too deeply nested [2]");
	}

	/* when it is a shell expression */
	check_is("2+3", true);
	check_is("2 3 +", true);
	check_is("(1+2)*3", true);
	check_is(".5+1", true);
	check_is("5", true);
	check_is("10 % 3", true);
	check_is("5%3", true);
	check_is("2*pi", true);
	check_is("e^2", true);
	check_is("pi 2 *", true);
	check_is("sqrt(9)", true);
	check_is("abs(-5)*2", true);
	check_is("round(2.5)", true);
	check_is("pi", false);
	check_is("e", false);
	check_is("sqrt 9", false);
	check_is("sqrt", false);
	check_is("foo(2)", false);
	check_is("7z", false);
	check_is("e2", false);
	check_is("echo 5", false);
	check_is("a=1", false);
	check_is("C:\\x", false);
	check_is("ls", false);
	check_is("ls -l", false);
	check_is("2 + x", false);
	check_is(".", false);
	check_is("", false);

	printf("calc: %d of %d cases passed\n", total - failures, total);
	return failures ? 1 : 0;
}
