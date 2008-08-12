/* plymouth.c - updates boot status
 *
 * Copyright (C) 2007 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include "config.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "ply-boot-client.h"
#include "ply-command-parser.h"
#include "ply-event-loop.h"
#include "ply-logger.h"
#include "ply-utils.h"

typedef struct
{
  ply_event_loop_t     *loop;
  ply_boot_client_t    *client;
  ply_command_parser_t *command_parser;
} state_t;

typedef struct
{
  state_t *state;
  char    *command;
} answer_state_t;

static char **
split_string (const char *command,
              const char  delimiter)
{
  const char *p, *q;
  int i, number_of_delimiters;
  char **args;

  number_of_delimiters = 0;
  for (p = command; *p != '\0'; p++)
    {
      if (*p == delimiter &&
          *(p + 1) != delimiter)
      number_of_delimiters++;
    }

  /* there is one more arg that delimiters between args
   * and a trailing NULL arg
   */
  args = calloc (number_of_delimiters + 2, sizeof (char *));
  q = command;
  i = 0;
  for (p = command; *p != '\0'; p++)
    {
      if (*p == delimiter)
        {
          args[i++] = strndup (q, p - q);

	  while (*p == delimiter)
	    p++;

	  q = p;
	}
      
      assert (*q != delimiter);
      assert (i <= number_of_delimiters);
    }

  args[i++] = strndup (q, p - q);

  return args;
}

static bool
answer_via_command (answer_state_t *answer_state,
                    const char     *answer,
                    int            *exit_status)
{
  bool gave_answer;
  pid_t pid;
  int command_input_sender_fd, command_input_receiver_fd;

  gave_answer = false;
  if (!ply_open_unidirectional_pipe (&command_input_sender_fd,
                                     &command_input_receiver_fd))  return false;

  pid = fork (); 

  if (pid < 0)
    return false;

  if (pid == 0)
    {
      char **args;
      close (command_input_sender_fd);
      args = split_string (answer_state->command, ' ');
      dup2 (command_input_receiver_fd, STDIN_FILENO);
      execvp (args[0], args); 
      ply_trace ("could not run command: %m");
      _exit (127);
    }
  close (command_input_receiver_fd);

  if (write (command_input_sender_fd, answer, strlen (answer)) < 0)
    goto out;

  gave_answer = true;
out:
  close (command_input_sender_fd);
  waitpid (pid, exit_status, 0); 

  return gave_answer;
}

static void
on_answer_failure (answer_state_t *answer_state)
{
  ply_event_loop_exit (answer_state->state->loop, 1);
}

static void
on_answer (answer_state_t   *answer_state,
           const char       *answer)
{
  int exit_status;

  exit_status = 0;
  if (answer_state->command != NULL)
    {
      bool command_started = false;

      exit_status = 127;
      command_started = answer_via_command (answer_state, answer,
                                            &exit_status);

      if (command_started && (!WIFEXITED (exit_status) ||
          WEXITSTATUS (exit_status) != 0))
        {
          ply_boot_client_ask_daemon_for_password (answer_state->state->client,
                                                   (ply_boot_client_answer_handler_t)
                                                   on_answer,
                                                   (ply_boot_client_response_handler_t)
                                                   on_answer_failure, answer_state);
          return;
        }
    }
  else
    write (STDOUT_FILENO, answer, strlen (answer));

  if (WIFSIGNALED (exit_status))
    raise (WTERMSIG (exit_status));

  ply_event_loop_exit (answer_state->state->loop, WEXITSTATUS (exit_status));
}

static void
on_failure (state_t *state)
{
  ply_event_loop_exit (state->loop, 1);
}

static void
on_success (state_t *state)
{
  ply_event_loop_exit (state->loop, 0);
}

static void
on_disconnect (state_t *state)
{
  bool wait;
  int status = 0;

  wait = false;
  ply_command_parser_get_options (state->command_parser,
                                   "wait", &wait,
                                   NULL
                                  );

  if (! wait) {
      ply_error ("error: unexpectedly disconnected from boot status daemon");
      status = 2;
  }

  ply_event_loop_exit (state->loop, status);
}

static void
on_password_request (state_t    *state,
                     const char *command)
{
  char *prompt;
  char *program;
  answer_state_t *answer_state;

  prompt = NULL;
  program = NULL;
  ply_command_parser_get_command_options (state->command_parser,
                                          command,
                                          "command", &program, NULL);

  answer_state = calloc (1, sizeof (answer_state_t));
  answer_state->state = state;
  answer_state->command = program != NULL? strdup (program): NULL;

  ply_boot_client_ask_daemon_for_password (state->client,
                                           (ply_boot_client_answer_handler_t)
                                           on_answer,
                                           (ply_boot_client_response_handler_t)
                                           on_answer_failure, answer_state);

  free (program);
}

int
main (int    argc,
      char **argv)
{
  state_t state = { 0 };
  bool should_help, should_quit, should_ping, should_sysinit, should_ask_for_password, should_show_splash, should_hide_splash, should_wait, should_be_verbose, report_error;
  char *status, *chroot_dir;
  int exit_code;

  exit_code = 0;

  signal (SIGPIPE, SIG_IGN);

  state.loop = ply_event_loop_new ();
  state.client = ply_boot_client_new ();
  state.command_parser = ply_command_parser_new ("plymouth", "Boot splash control client");

  ply_command_parser_add_options (state.command_parser,
                                  "help", "This help message", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "debug", "Enable verbose debug logging", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "newroot", "Tell boot daemon that new root filesystem is mounted", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "quit", "Tell boot daemon to quit", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "ping", "Check of boot daemon is running", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "sysinit", "Tell boot daemon root filesystem is mounted read-write", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "show-splash", "Show splash screen", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "hide-splash", "Hide splash screen", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "ask-for-password", "Ask user for password", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "update", "Tell boot daemon an update about boot progress", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "details", "Tell boot daemon there were errors during boot", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "wait", "Wait for boot daemon to quit", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  NULL);

  ply_command_parser_add_command (state.command_parser,
                                  "ask-for-password", "Ask user for passowrd",
                                  (ply_command_handler_t)
                                  on_password_request, &state,
                                  "command", "Command to send password to via standard input",
                                  PLY_COMMAND_OPTION_TYPE_STRING, NULL);

  if (!ply_command_parser_parse_arguments (state.command_parser, state.loop, argv, argc))
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      ply_error ("%s", help_string);

      free (help_string);
      return 1;
    }

  ply_command_parser_get_options (state.command_parser,
                                  "help", &should_help,
                                  "debug", &should_be_verbose,
                                  "newroot", &chroot_dir,
                                  "quit", &should_quit,
                                  "ping", &should_ping,
                                  "sysinit", &should_sysinit,
                                  "show-splash", &should_show_splash,
                                  "hide-splash", &should_hide_splash,
                                  "ask-for-password", &should_ask_for_password,
                                  "update", &status,
                                  "wait", &should_wait,
                                  "details", &report_error,
                                  NULL);

  if (should_help || argc < 2)
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      if (argc < 2)
        fprintf (stderr, "%s", help_string);
      else
        printf ("%s", help_string);

      free (help_string);
      return 0;
    }

  if (should_be_verbose && !ply_is_tracing ())
    ply_toggle_tracing ();

  if (!ply_boot_client_connect (state.client,
                                (ply_boot_client_disconnect_handler_t)
                                on_disconnect, &state))
    {
      if (should_ping)
         return 1;

#if 0
      ply_save_errno ();

      if (errno == ECONNREFUSED)
        ply_error ("error: boot status daemon not running "
                   "(use --ping to check ahead of time)");
      else
        ply_error ("could not connect to boot status daemon: %m");
      ply_restore_errno ();
#endif
      return errno;
    }

  ply_boot_client_attach_to_event_loop (state.client, state.loop);

  if (should_show_splash)
    ply_boot_client_tell_daemon_to_show_splash (state.client,
                                               (ply_boot_client_response_handler_t)
                                               on_success,
                                               (ply_boot_client_response_handler_t)
                                               on_failure, &state);
  else if (should_hide_splash)
    ply_boot_client_tell_daemon_to_hide_splash (state.client,
                                               (ply_boot_client_response_handler_t)
                                               on_success,
                                               (ply_boot_client_response_handler_t)
                                               on_failure, &state);
  else if (should_quit)
    ply_boot_client_tell_daemon_to_quit (state.client,
                                         (ply_boot_client_response_handler_t)
                                         on_success,
                                         (ply_boot_client_response_handler_t)
                                         on_failure, &state);
  else if (should_ping)
    ply_boot_client_ping_daemon (state.client,
                                 (ply_boot_client_response_handler_t)
                                 on_success, 
                                 (ply_boot_client_response_handler_t)
                                 on_failure, &state);
  else if (status != NULL)
    ply_boot_client_update_daemon (state.client, status,
                                   (ply_boot_client_response_handler_t)
                                   on_success, 
                                   (ply_boot_client_response_handler_t)
                                   on_failure, &state);
  else if (should_ask_for_password)
    {
      answer_state_t answer_state = { 0 };

      answer_state.state = &state;
      ply_boot_client_ask_daemon_for_password (state.client,
                                               (ply_boot_client_answer_handler_t)
                                                on_answer,
                                               (ply_boot_client_response_handler_t)
                                               on_answer_failure, &answer_state);
    }
  else if (should_sysinit)
    ply_boot_client_tell_daemon_system_is_initialized (state.client,
                                   (ply_boot_client_response_handler_t)
                                   on_success, 
                                   (ply_boot_client_response_handler_t)
                                   on_failure, &state);
  else if (chroot_dir)
    ply_boot_client_tell_daemon_to_change_root (state.client, chroot_dir,
                                   (ply_boot_client_response_handler_t)
                                   on_success,
                                   (ply_boot_client_response_handler_t)
                                   on_failure, &state);

  else if (should_wait)
    {} // Do nothing
  else if (report_error)
    ply_boot_client_tell_daemon_about_error (state.client,
                                             (ply_boot_client_response_handler_t)
                                             on_success,
                                             (ply_boot_client_response_handler_t)
                                             on_failure, &state);

  exit_code = ply_event_loop_run (state.loop);

  ply_boot_client_free (state.client);

  return exit_code;
}
/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
