#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#include "args.h"
#include "../console/console.h"
#include "../types/string_util.h"
#include "shell.h"

ANT_ARG *ant_arg = NULL;

void ant_args_to_line (int argc, const char **argv) {

	for (int i = 1; i < argc; i++) {
		if (strchr(argv[i], ' ')) {
			char buf[ANTMID] = {0};
			sprintf(buf, "\"%s\" ", argv[i]);
			strcat(char_line_buf, buf);
		} else {
			strcat(char_line_buf, argv[i]);
			strcat(char_line_buf, " ");
		}
	}
	char_line_buf[strlen(char_line_buf)-1] = 0;
}

ANT_ARG *ant_arg_insert (const char *str) {

	ANT_ARG *newnode = (ANT_ARG *) malloc(sizeof(ANT_ARG));

	if (!newnode) {
		printf("Memory allocation failed\n");
		exit(1);
	}

	newnode->string = strdup(str);
	newnode->next = NULL;
	newnode->prev = NULL;

	if (ant_arg == NULL) {
		ant_arg = newnode;
	} else {
		ANT_ARG *temp = ant_arg;
		while (temp->next) {
			temp = temp->next;
		}
		temp->next = newnode;
		newnode->prev = temp;
	}

	return newnode;
}

void ant_arg_free (void) {

	ANT_ARG *tmp;
	while (ant_arg) {
		if (ant_arg->string)
			free(ant_arg->string);
		tmp = ant_arg;
		ant_arg = ant_arg->next;
		free(tmp);
	}

	ant_arg = NULL;
}