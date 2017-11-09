/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2017  Intel Corporation. All rights reserved.
 *
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/signalfd.h>

#include <readline/readline.h>
#include <readline/history.h>
#include <glib.h>

#include "src/shared/io.h"
#include "src/shared/util.h"
#include "src/shared/queue.h"
#include "src/shared/shell.h"

#define CMD_LENGTH	48
#define print_text(color, fmt, args...) \
		printf(color fmt COLOR_OFF "\n", ## args)
#define print_menu(cmd, args, desc) \
		printf(COLOR_HIGHLIGHT "%s %-*s " COLOR_OFF "%s\n", \
			cmd, (int)(CMD_LENGTH - strlen(cmd)), args, desc)

static GMainLoop *main_loop;
static gboolean option_version = FALSE;

static struct {
	struct io *input;

	bool saved_prompt;
	bt_shell_prompt_input_func saved_func;
	void *saved_user_data;

	const struct bt_shell_menu_entry *menu;
	/* TODO: Add submenus support */
} data;

static void shell_print_menu(void);

static void cmd_version(const char *arg)
{
	bt_shell_printf("Version %s\n", VERSION);
}

static void cmd_quit(const char *arg)
{
	g_main_loop_quit(main_loop);
}

static void cmd_help(const char *arg)
{
	shell_print_menu();
}

static const struct bt_shell_menu_entry default_menu[] = {
	{ "version",      NULL,       cmd_version, "Display version" },
	{ "quit",         NULL,       cmd_quit, "Quit program" },
	{ "exit",         NULL,       cmd_quit, "Quit program" },
	{ "help",         NULL,       cmd_help,
					"Display help about this program" },
	{ }
};

static void shell_print_menu(void)
{
	const struct bt_shell_menu_entry *entry;

	if (!data.menu)
		return;

	print_text(COLOR_HIGHLIGHT, "Available commands:");
	print_text(COLOR_HIGHLIGHT, "-------------------");
	for (entry = data.menu; entry->cmd; entry++) {
		print_menu(entry->cmd, entry->arg ? : "", entry->desc ? : "");
	}

	for (entry = default_menu; entry->cmd; entry++) {
		print_menu(entry->cmd, entry->arg ? : "", entry->desc ? : "");
	}
}

static void shell_exec(const char *cmd, const char *arg)
{
	const struct bt_shell_menu_entry *entry;

	if (!data.menu || !cmd)
		return;

	for (entry = data.menu; entry->cmd; entry++) {
		if (strcmp(cmd, entry->cmd))
			continue;

		if (entry->func) {
			entry->func(arg);
			return;
		}
	}

	for (entry = default_menu; entry->cmd; entry++) {
		if (strcmp(cmd, entry->cmd))
			continue;

		if (entry->func) {
			entry->func(arg);
			return;
		}
	}

	print_text(COLOR_HIGHLIGHT, "Invalid command");
}

void bt_shell_printf(const char *fmt, ...)
{
	va_list args;
	bool save_input;
	char *saved_line;
	int saved_point;

	save_input = !RL_ISSTATE(RL_STATE_DONE);

	if (save_input) {
		saved_point = rl_point;
		saved_line = rl_copy_text(0, rl_end);
		if (!data.saved_prompt) {
			rl_save_prompt();
			rl_replace_line("", 0);
			rl_redisplay();
		}
	}

	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);

	if (save_input) {
		if (!data.saved_prompt)
			rl_restore_prompt();
		rl_replace_line(saved_line, 0);
		rl_point = saved_point;
		rl_forced_update_display();
		free(saved_line);
	}
}

void bt_shell_hexdump(const unsigned char *buf, size_t len)
{
	static const char hexdigits[] = "0123456789abcdef";
	char str[68];
	size_t i;

	if (!len)
		return;

	str[0] = ' ';

	for (i = 0; i < len; i++) {
		str[((i % 16) * 3) + 1] = ' ';
		str[((i % 16) * 3) + 2] = hexdigits[buf[i] >> 4];
		str[((i % 16) * 3) + 3] = hexdigits[buf[i] & 0xf];
		str[(i % 16) + 51] = isprint(buf[i]) ? buf[i] : '.';

		if ((i + 1) % 16 == 0) {
			str[49] = ' ';
			str[50] = ' ';
			str[67] = '\0';
			bt_shell_printf("%s\n", str);
			str[0] = ' ';
		}
	}

	if (i % 16 > 0) {
		size_t j;
		for (j = (i % 16); j < 16; j++) {
			str[(j * 3) + 1] = ' ';
			str[(j * 3) + 2] = ' ';
			str[(j * 3) + 3] = ' ';
			str[j + 51] = ' ';
		}
		str[49] = ' ';
		str[50] = ' ';
		str[67] = '\0';
		bt_shell_printf("%s\n", str);
	}
}

void bt_shell_prompt_input(const char *label, const char *msg,
			bt_shell_prompt_input_func func, void *user_data)
{
	/* Normal use should not prompt for user input to the value a second
	 * time before it releases the prompt, but we take a safe action. */
	if (data.saved_prompt)
		return;

	rl_save_prompt();
	rl_message(COLOR_RED "[%s]" COLOR_OFF " %s ", label, msg);

	data.saved_prompt = true;
	data.saved_func = func;
	data.saved_user_data = user_data;
}

int bt_shell_release_prompt(const char *input)
{
	bt_shell_prompt_input_func func;
	void *user_data;

	if (!data.saved_prompt)
		return -1;

	data.saved_prompt = false;

	rl_restore_prompt();

	func = data.saved_func;
	user_data = data.saved_user_data;

	data.saved_func = NULL;
	data.saved_user_data = NULL;

	func(input, user_data);

	return 0;
}

static void rl_handler(char *input)
{
	char *cmd, *arg;

	if (!input) {
		rl_insert_text("quit");
		rl_redisplay();
		rl_crlf();
		g_main_loop_quit(main_loop);
		return;
	}

	if (!strlen(input))
		goto done;

	if (!bt_shell_release_prompt(input))
		goto done;

	if (history_search(input, -1))
		add_history(input);

	cmd = strtok_r(input, " ", &arg);
	if (!cmd)
		goto done;

	if (arg) {
		int len = strlen(arg);
		if (len > 0 && arg[len - 1] == ' ')
			arg[len - 1] = '\0';
	}

	shell_exec(cmd, arg);
done:
	free(input);
}

static char *cmd_generator(const char *text, int state)
{
	static const struct bt_shell_menu_entry *entry;
	static int index, len;
	const char *cmd;

	if (!state) {
		entry = default_menu;
		index = 0;
		len = strlen(text);
	}

	while ((cmd = entry[index].cmd)) {
		index++;

		if (!strncmp(cmd, text, len))
			return strdup(cmd);
	}

	if (state)
		return NULL;

	entry = data.menu;
	index = 0;

	return cmd_generator(text, 1);
}

static char **menu_completion(const struct bt_shell_menu_entry *entry,
				const char *text, char *input_cmd)
{
	char **matches = NULL;

	for (entry = data.menu; entry->cmd; entry++) {
		if (strcmp(entry->cmd, input_cmd))
			continue;

		if (!entry->gen)
			continue;

		rl_completion_display_matches_hook = entry->disp;
		matches = rl_completion_matches(text, entry->gen);
		break;
	}

	return matches;
}

static char **shell_completion(const char *text, int start, int end)
{
	char **matches = NULL;

	if (!data.menu)
		return NULL;

	if (start > 0) {
		char *input_cmd;

		input_cmd = strndup(rl_line_buffer, start - 1);
		matches = menu_completion(default_menu, text, input_cmd);
		if (!matches)
			matches = menu_completion(data.menu, text,
							input_cmd);

		free(input_cmd);
	} else {
		rl_completion_display_matches_hook = NULL;
		matches = rl_completion_matches(text, cmd_generator);
	}

	if (!matches)
		rl_attempted_completion_over = 1;

	return matches;
}

static bool io_hup(struct io *io, void *user_data)
{
	g_main_loop_quit(main_loop);

	return false;
}

static bool signal_read(struct io *io, void *user_data)
{
	static bool terminated = false;
	struct signalfd_siginfo si;
	ssize_t result;
	int fd;

	fd = io_get_fd(io);

	result = read(fd, &si, sizeof(si));
	if (result != sizeof(si))
		return false;

	switch (si.ssi_signo) {
	case SIGINT:
		if (data.input) {
			rl_replace_line("", 0);
			rl_crlf();
			rl_on_new_line();
			rl_redisplay();
			break;
		}

		/*
		 * If input was not yet setup up that means signal was received
		 * while daemon was not yet running. Since user is not able
		 * to terminate client by CTRL-D or typing exit treat this as
		 * exit and fall through.
		 */

		/* fall through */
	case SIGTERM:
		if (!terminated) {
			rl_replace_line("", 0);
			rl_crlf();
			g_main_loop_quit(main_loop);
		}

		terminated = true;
		break;
	}

	return false;
}

static struct io *setup_signalfd(void)
{
	struct io *io;
	sigset_t mask;
	int fd;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
		perror("Failed to set signal mask");
		return 0;
	}

	fd = signalfd(-1, &mask, 0);
	if (fd < 0) {
		perror("Failed to create signal descriptor");
		return 0;
	}

	io = io_new(fd);

	io_set_close_on_destroy(io, true);
	io_set_read_handler(io, signal_read, NULL, NULL);
	io_set_disconnect_handler(io, io_hup, NULL, NULL);

	return io;
}

static GOptionEntry options[] = {
	{ "version", 'v', 0, G_OPTION_ARG_NONE, &option_version,
				"Show version information and exit" },
	{ NULL },
};

static void rl_init(void)
{
	setlinebuf(stdout);
	rl_attempted_completion_function = shell_completion;

	rl_erase_empty_line = 1;
	rl_callback_handler_install(NULL, rl_handler);
}

void bt_shell_init(int *argc, char ***argv)
{
	GOptionContext *context;
	GError *error = NULL;

	context = g_option_context_new(NULL);
	g_option_context_add_main_entries(context, options, NULL);

	if (g_option_context_parse(context, argc, argv, &error) == FALSE) {
		if (error != NULL) {
			g_printerr("%s\n", error->message);
			g_error_free(error);
		} else
			g_printerr("An unknown error occurred\n");
		exit(1);
	}

	g_option_context_free(context);

	if (option_version == TRUE) {
		g_print("%s\n", VERSION);
		exit(EXIT_SUCCESS);
	}

	main_loop = g_main_loop_new(NULL, FALSE);

	rl_init();
}

static void rl_cleanup(void)
{
	rl_message("");
	rl_callback_handler_remove();
}

void bt_shell_run(void)
{
	struct io *signal;

	signal = setup_signalfd();

	g_main_loop_run(main_loop);

	bt_shell_release_prompt("");
	bt_shell_detach();

	io_destroy(signal);

	g_main_loop_unref(main_loop);
	main_loop = NULL;

	rl_cleanup();
}

bool bt_shell_set_menu(const struct bt_shell_menu_entry *menu)
{
	if (data.menu || !menu)
		return false;

	data.menu = menu;

	return true;
}

void bt_shell_set_prompt(const char *string)
{
	if (!main_loop)
		return;

	rl_set_prompt(string);
	printf("\r");
	rl_on_new_line();
	rl_redisplay();
}

static bool input_read(struct io *io, void *user_data)
{
	rl_callback_read_char();
	return true;
}

bool bt_shell_attach(int fd)
{
	struct io *io;

	/* TODO: Allow more than one input? */
	if (data.input)
		return false;

	io = io_new(fd);

	io_set_read_handler(io, input_read, NULL, NULL);
	io_set_disconnect_handler(io, io_hup, NULL, NULL);

	data.input = io;

	return true;
}

bool bt_shell_detach(void)
{
	if (!data.input)
		return false;

	io_destroy(data.input);
	data.input = NULL;

	return true;
}
