/* upstart
 *
 * Copyright © 2009-2011 Canonical Ltd.
 * Author: Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <sys/types.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/resource.h>

#include <errno.h>
#include <stdio.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <linux/kd.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/list.h>
#include <nih/timer.h>
#include <nih/signal.h>
#include <nih/child.h>
#include <nih/option.h>
#include <nih/main.h>
#include <nih/error.h>
#include <nih/logging.h>

#include "paths.h"
#include "events.h"
#include "system.h"
#include "job_class.h"
#include "job_process.h"
#include "event.h"
#include "conf.h"
#include "control.h"
#include "state.h"


/* Prototypes for static functions */
#ifndef DEBUG
static int  logger_kmsg     (NihLogLevel priority, const char *message);
static void crash_handler   (int signum);
#endif /* DEBUG */
static void term_handler    (void *data, NihSignal *signal);
#ifndef DEBUG
static void cad_handler     (void *data, NihSignal *signal);
static void kbd_handler     (void *data, NihSignal *signal);
static void pwr_handler     (void *data, NihSignal *signal);
static void hup_handler     (void *data, NihSignal *signal);
static void usr1_handler    (void *data, NihSignal *signal);
#endif /* DEBUG */

static void handle_confdir      (void);
static void handle_logdir       (void);
static int  console_type_setter (NihOption *option, const char *arg);
static void perform_reexec      (void);
static void stateful_reexec     (void);


/**
 * args_copy:
 *
 * Copy of original argv used when re-executing to ensure same
 * command-line is used. Required since we clear the actual args for
 * ps(1) et al.
 */
char **args_copy = NULL;

/**
 * restart:
 *
 * This is set to TRUE if we're being re-exec'd by an existing init
 * process.
 **/
static int restart = FALSE;

/**
 * state_fd:
 *
 * File descriptor to read serialised state from when performing
 * stateful re-exec. If value is not -1, attempt stateful re-exec.
 **/
static int state_fd = -1;

/**
 * conf_dir:
 *
 * Full path to job configuration file directory.
 *
 **/
static char *conf_dir = NULL;

/**
 * initial_event:
 *
 * Alternate event to emit at startup (rather than STARTUP_EVENT).
 **/
static char *initial_event = NULL;

/**
 * disable_startup_event:
 *
 * If TRUE, do not emit a startup event.
 **/
static int disable_startup_event = FALSE;

extern int          disable_sessions;
extern int          disable_job_logging;
extern int          use_session_bus;
extern int          default_console;
extern char        *log_dir;


/**
 * options:
 *
 * Command-line options we accept.
 **/
static NihOption options[] = {
	{ 0, "confdir", N_("specify alternative directory to load configuration files from"),
		NULL, "DIR", &conf_dir, NULL },

	{ 0, "default-console", N_("default value for console stanza"),
		NULL, "VALUE", NULL, console_type_setter },

	{ 0, "logdir", N_("specify alternative directory to store job output logs in"),
		NULL, "DIR", &log_dir, NULL },

	{ 0, "no-log", N_("disable job logging"),
		NULL, NULL, &disable_job_logging, NULL },

	{ 0, "no-sessions", N_("disable user and chroot sessions"),
		NULL, NULL, &disable_sessions, NULL },

	{ 0, "no-startup-event", N_("do not emit any startup event (for testing)"),
		NULL, NULL, &disable_startup_event, NULL },

	/* Must be specified for both stateful and stateless re-exec */
	{ 0, "restart", N_("flag a re-exec has occurred"),
		NULL, NULL, &restart, NULL },

	/* Required for stateful re-exec */
	{ 0, "state-fd", N_("specify file descriptor to read serialisation data from"),
		NULL, "FD", &state_fd, nih_option_int },

	{ 0, "session", N_("use D-Bus session bus rather than system bus (for testing)"),
		NULL, NULL, &use_session_bus, NULL },

	{ 0, "startup-event", N_("specify an alternative initial event (for testing)"),
		NULL, "NAME", &initial_event, NULL },

	/* Ignore invalid options */
	{ '-', "--", NULL, NULL, NULL, NULL, NULL },

	NIH_OPTION_LAST
};


int
main (int   argc,
      char *argv[])
{
	char **args = NULL;
	int    ret;

	args_copy = NIH_MUST (nih_str_array_copy (NULL, NULL, argv));

	nih_main_init (args_copy[0]);

	nih_option_set_synopsis (_("Process management daemon."));
	nih_option_set_help (
		_("This daemon is normally executed by the kernel and given "
		  "process id 1 to denote its special status.  When executed "
		  "by a user process, it will actually run /sbin/telinit."));

	args = nih_option_parser (NULL, argc, argv, options, FALSE);
	if (! args)
		exit (1);

	handle_confdir ();
	handle_logdir ();

	if (disable_job_logging)
		nih_debug ("Job logging disabled");

	control_handle_bus_type ();

#ifndef DEBUG
	if (use_session_bus == FALSE) {

		/* Check we're root */
		if (getuid ()) {
			nih_fatal (_("Need to be root"));
			exit (1);
		}

		/* Check we're process #1 */
		if (getpid () > 1) {
			execv (TELINIT, argv);
			/* Ignore failure, probably just that telinit doesn't exist */

			nih_fatal (_("Not being executed as init"));
			exit (1);
		}

		/* Clear our arguments from the command-line, so that we show up in
		 * ps or top output as /sbin/init, with no extra flags.
		 *
		 * This is a very Linux-specific trick; by deleting the NULL
		 * terminator at the end of the last argument, we fool the kernel
		 * into believing we used a setproctitle()-a-like to extend the
		 * argument space into the environment space, and thus make it use
		 * strlen() instead of its own assumed length.  In fact, we've done
		 * the exact opposite, and shrunk the command line length to just that
		 * of whatever is in argv[0].
		 *
		 * If we don't do this, and just write \0 over the rest of argv, for
		 * example; the command-line length still includes those \0s, and ps
		 * will show whitespace in their place.
		 */
		if (argc > 1) {
			char *arg_end;

			arg_end = argv[argc-1] + strlen (argv[argc-1]);
			*arg_end = ' ';
		}


		/* Become the leader of a new session and process group, shedding
		 * any controlling tty (which we shouldn't have had anyway - but
		 * you never know what initramfs did).
		 */
		setsid ();

		/* Set the standard file descriptors to the ordinary console device,
		 * resetting it to sane defaults unless we're inheriting from another
		 * init process which we know left it in a sane state.
		 */
		if (system_setup_console (CONSOLE_OUTPUT, (! restart)) < 0) {
			NihError *err;

			err = nih_error_get ();

			nih_warn ("%s: %s", _("Unable to initialize console, will try /dev/null"),
				  err->message);
			nih_free (err);
	
			if (system_setup_console (CONSOLE_NONE, FALSE) < 0) {
				err = nih_error_get ();
				nih_fatal ("%s: %s", _("Unable to initialize console as /dev/null"),
					   err->message);
				nih_free (err);
	
				exit (1);
			}
		}

		/* Set the PATH environment variable */
		setenv ("PATH", PATH, TRUE);

		/* Switch to the root directory in case we were started from some
		 * strange place, or worse, some directory in the initramfs that's
		 * going to go away soon.
		 */
		if (chdir ("/"))
			nih_warn ("%s: %s", _("Unable to set root directory"),
				strerror (errno));

		/* Mount the /proc and /sys filesystems, which are pretty much
		 * essential for any Linux system; not to mention used by
		 * ourselves.
		 */
		if (system_mount ("proc", "/proc") < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_warn ("%s: %s", _("Unable to mount /proc filesystem"),
				err->message);
			nih_free (err);
		}

		if (system_mount ("sysfs", "/sys") < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_warn ("%s: %s", _("Unable to mount /sys filesystem"),
				err->message);
			nih_free (err);
		}
	} else {
		nih_log_set_priority (NIH_LOG_DEBUG);
		nih_debug ("Running with UID %d as PID %d (PPID %d)",
				(int)getuid (), (int)getpid (), (int)getppid ());
	}

#else /* DEBUG */
	nih_log_set_priority (NIH_LOG_DEBUG);
	nih_debug ("Running with UID %d as PID %d (PPID %d)",
		(int)getuid (), (int)getpid (), (int)getppid ());
#endif /* DEBUG */


	/* Reset the signal state and install the signal handler for those
	 * signals we actually want to catch; this also sets those that
	 * can be sent to us, because we're special
	 */
	if (! restart)
		nih_signal_reset ();

#ifndef DEBUG
	if (use_session_bus == FALSE) {
		/* Catch fatal errors immediately rather than waiting for a new
		 * iteration through the main loop.
		 */
		nih_signal_set_handler (SIGSEGV, crash_handler);
		nih_signal_set_handler (SIGABRT, crash_handler);
	}
#endif /* DEBUG */

	/* Don't ignore SIGCHLD or SIGALRM, but don't respond to them
	 * directly; it's enough that they interrupt the main loop and
	 * get dealt with during it.
	 */
	nih_signal_set_handler (SIGCHLD, nih_signal_handler);
	nih_signal_set_handler (SIGALRM, nih_signal_handler);

#ifndef DEBUG
	if (use_session_bus == FALSE) {
		/* Ask the kernel to send us SIGINT when control-alt-delete is
		 * pressed; generate an event with the same name.
		 */
		reboot (RB_DISABLE_CAD);
		nih_signal_set_handler (SIGINT, nih_signal_handler);
		NIH_MUST (nih_signal_add_handler (NULL, SIGINT, cad_handler, NULL));

		/* Ask the kernel to send us SIGWINCH when alt-uparrow is pressed;
		 * generate a keyboard-request event.
		 */
		if (ioctl (0, KDSIGACCEPT, SIGWINCH) == 0) {
			nih_signal_set_handler (SIGWINCH, nih_signal_handler);
			NIH_MUST (nih_signal_add_handler (NULL, SIGWINCH,
						kbd_handler, NULL));
		}

		/* powstatd sends us SIGPWR when it changes /etc/powerstatus */
		nih_signal_set_handler (SIGPWR, nih_signal_handler);
		NIH_MUST (nih_signal_add_handler (NULL, SIGPWR, pwr_handler, NULL));

		/* SIGHUP instructs us to re-load our configuration */
		nih_signal_set_handler (SIGHUP, nih_signal_handler);
		NIH_MUST (nih_signal_add_handler (NULL, SIGHUP, hup_handler, NULL));

		/* SIGUSR1 instructs us to reconnect to D-Bus */
		nih_signal_set_handler (SIGUSR1, nih_signal_handler);
		NIH_MUST (nih_signal_add_handler (NULL, SIGUSR1, usr1_handler, NULL));

	}

	/* SIGTERM instructs us to re-exec ourselves; this should be the
	 * last in the list to ensure that all other signals are handled
	 * before a SIGTERM.
	 */
	nih_signal_set_handler (SIGTERM, nih_signal_handler);
	NIH_MUST (nih_signal_add_handler (NULL, SIGTERM, term_handler, NULL));

#endif /* DEBUG */


	/* Watch children for events */
	NIH_MUST (nih_child_add_watch (NULL, -1, NIH_CHILD_ALL,
				       job_process_handler, NULL));

	/* Process the event queue each time through the main loop */
	NIH_MUST (nih_main_loop_add_func (NULL, (NihMainLoopCb)event_poll,
					  NULL));


	/* Adjust our OOM priority to the default, which will be inherited
	 * by all jobs.
	 */
	if (JOB_DEFAULT_OOM_SCORE_ADJ) {
		char  filename[PATH_MAX];
		int   oom_value;
		FILE *fd;

		snprintf (filename, sizeof (filename),
			  "/proc/%d/oom_score_adj", getpid ());
		oom_value = JOB_DEFAULT_OOM_SCORE_ADJ;
		fd = fopen (filename, "w");
		if ((! fd) && (errno == ENOENT)) {
			snprintf (filename, sizeof (filename),
				  "/proc/%d/oom_adj", getpid ());
			oom_value = (JOB_DEFAULT_OOM_SCORE_ADJ
				     * ((JOB_DEFAULT_OOM_SCORE_ADJ < 0) ? 17 : 15)) / 1000;
			fd = fopen (filename, "w");
		}
		if (! fd) {
			nih_warn ("%s: %s", _("Unable to set default oom score"),
				  strerror (errno));
		} else {
			fprintf (fd, "%d\n", oom_value);

			if (fclose (fd))
				nih_warn ("%s: %s", _("Unable to set default oom score"),
					  strerror (errno));
		}
	}


	if (restart) {
		if (state_fd == -1) {
			nih_warn ("%s",
				_("Stateful re-exec supported but stateless re-exec requested"));
		} else if (state_read (state_fd) < 0) {
			nih_local char *arg = NULL;

			/* Stateful re-exec has failed so try once more by
			 * degrading to stateless re-exec, which even in
			 * the case of low-memory scenarios will work.
			 */

			/* Inform the child we've given up on stateful
			 * re-exec.
			 */
			close (state_fd);

			nih_error ("%s - %s",
				_("Failed to read serialisation data"),
				_("reverting to stateless re-exec"));

			/* Remove any existing state fd args which will effectively
			 * disable stateful re-exec.
			 */
			for (int i = 1; args_copy[i]; i++) {
				if (! strcmp (args_copy[i], "--state-fd")) {

					/* Remove existing entry and fd value */
					nih_free (args_copy[i]);
					nih_free (args_copy[i+1]);

					/* shuffle up the remaining args */
					for (int j = i+2; args_copy[j]; i++, j++)
						args_copy[i] = args_copy[j];
					args_copy[i] = NULL;
					break;
				}
			}

			/* Attempt stateless re-exec */
			perform_reexec ();

			nih_error ("%s",
				_("Both stateful and stateless re-execs failed"));

			/* Out of options */
			nih_assert_not_reached ();
		} else {
			close (state_fd);

			nih_info ("Stateful re-exec completed");
		}
	}

	/* Read configuration */
	NIH_MUST (conf_source_new (NULL, CONFFILE, CONF_FILE));
	NIH_MUST (conf_source_new (NULL, conf_dir, CONF_JOB_DIR));

	conf_reload (restart);

	/* Create a listening server for private connections. */
	if (use_session_bus == FALSE) {
		while (control_server_open () < 0) {
			NihError *err;

			err = nih_error_get ();
			if (err->number != ENOMEM) {
				nih_warn ("%s: %s", _("Unable to listen for private connections"),
					err->message);
				nih_free (err);
				break;
			}
			nih_free (err);
		}
	}

	/* Open connection to the appropriate D-Bus bus; we normally expect this to
	 * fail (since dbus-daemon probably isn't running yet) and will try again
	 * later - don't let ENOMEM stop us though.
	 */
	while (control_bus_open (restart) < 0) {
		NihError *err;
		int       number;

		err = nih_error_get ();
		number = err->number;
		nih_free (err);

		if (number != ENOMEM)
			break;
	}

#ifndef DEBUG
	if (use_session_bus == FALSE) {
		/* Now that the startup is complete, send all further logging output
		 * to kmsg instead of to the console.
		 */
		if (system_setup_console (CONSOLE_NONE, FALSE) < 0) {
			NihError *err;
			
			err = nih_error_get ();
			nih_fatal ("%s: %s", _("Unable to setup standard file descriptors"),
				   err->message);
			nih_free (err);
	
			exit (1);
		}

		nih_log_set_logger (logger_kmsg);
	}
#endif /* DEBUG */


	/* Generate and run the startup event or read the state from the
	 * init daemon that exec'd us
	 */
	if (! restart) {
		if (disable_startup_event) {
			nih_debug ("Startup event disabled");
		} else {
			NIH_MUST (event_new (NULL,
				initial_event
				? initial_event
				: STARTUP_EVENT,
				NULL));
		}

	} else {
		sigset_t        mask;

		/* We have been re-exec'd. Don't emit an initial event
		 * as only Upstart is restarting - we don't want to restart
		 * the system (another reason being that we don't yet support
		 * upstart-in-initramfs to upstart-in-root-filesystem
		 * state-passing transitions).
		 */

		/* We're ok to receive signals again so restore signals
		 * disabled by the term_handler */
		sigemptyset (&mask);
		sigprocmask (SIG_SETMASK, &mask, NULL);
	}

	if (disable_sessions)
		nih_debug ("Sessions disabled");

	/* Run through the loop at least once to deal with signals that were
	 * delivered to the previous process while the mask was set or to
	 * process the startup event we emitted.
	 */
	nih_main_loop_interrupt ();
	ret = nih_main_loop ();

	return ret;
}


#ifndef DEBUG
/**
 * logger_kmsg:
 * @priority: priority of message being logged,
 * @message: message to log.
 *
 * Outputs the @message to the kernel log message socket prefixed with an
 * appropriate tag based on @priority, the program name and terminated with
 * a new line.
 *
 * Returns: zero on success, negative value on error.
 **/
static int
logger_kmsg (NihLogLevel priority,
	     const char *message)
{
	int   tag;
	FILE *kmsg;

	nih_assert (message != NULL);

	switch (priority) {
	case NIH_LOG_DEBUG:
		tag = '7';
		break;
	case NIH_LOG_INFO:
		tag = '6';
		break;
	case NIH_LOG_MESSAGE:
		tag = '5';
		break;
	case NIH_LOG_WARN:
		tag = '4';
		break;
	case NIH_LOG_ERROR:
		tag = '3';
		break;
	case NIH_LOG_FATAL:
		tag = '2';
		break;
	default:
		tag = 'd';
	}

	kmsg = fopen ("/dev/kmsg", "w");
	if (! kmsg)
		return -1;

	if (fprintf (kmsg, "<%c>%s: %s\n", tag, program_name, message) < 0) {
		int saved_errno = errno;
		fclose (kmsg);
		errno = saved_errno;
		return -1;
	}

	if (fclose (kmsg) < 0)
		return -1;

	return 0;
}


/**
 * crash_handler:
 * @signum: signal number received.
 *
 * Handle receiving the SEGV or ABRT signal, usually caused by one of
 * our own mistakes.  We deal with it by dumping core in a child process
 * and then killing the parent.
 *
 * Sadly there's no real alternative to the ensuing kernel panic.  Our
 * state is likely in tatters, so we can't sigjmp() anywhere "safe" or
 * re-exec since the system will be suddenly lobotomised.  We definitely
 * don't want to start a root shell or anything like that.  Best thing is
 * to just stop the whole thing and hope that bug report comes quickly.
 **/
static void
crash_handler (int signum)
{
	pid_t pid;

	nih_assert (args_copy[0] != NULL);

	pid = fork ();
	if (pid == 0) {
		struct sigaction act;
		struct rlimit    limit;
		sigset_t         mask;

		/* Mask out all signals */
		sigfillset (&mask);
		sigprocmask (SIG_SETMASK, &mask, NULL);

		/* Set the handler to the default so core is dumped */
		act.sa_handler = SIG_DFL;
		act.sa_flags = 0;
		sigemptyset (&act.sa_mask);
		sigaction (signum, &act, NULL);

		/* Don't limit the core dump size */
		limit.rlim_cur = RLIM_INFINITY;
		limit.rlim_max = RLIM_INFINITY;
		setrlimit (RLIMIT_CORE, &limit);

		/* Dump in the root directory */
		if (chdir ("/"))
			nih_warn ("%s: %s", _("Unable to set root directory"),
				  strerror (errno));

		/* Raise the signal again */
		raise (signum);

		/* Unmask so that we receive it */
		sigdelset (&mask, signum);
		sigprocmask (SIG_SETMASK, &mask, NULL);

		/* Wait for death */
		pause ();
		exit (0);
	} else if (pid > 0) {
		/* Wait for the core to be generated */
		waitpid (pid, NULL, 0);

		nih_fatal (_("Caught %s, core dumped"),
			   (signum == SIGSEGV
			    ? "segmentation fault" : "abort"));
	} else {
		nih_fatal (_("Caught %s, unable to dump core"),
			   (signum == SIGSEGV
			    ? "segmentation fault" : "abort"));
	}

	/* Goodbye, cruel world. */
	exit (signum);
}
#endif

/**
 * term_handler:
 * @data: unused,
 * @signal: signal caught.
 *
 * This is called when we receive the TERM signal, which instructs us
 * to reexec ourselves.
 **/
static void
term_handler (void      *data,
	      NihSignal *signal)
{
	sigset_t    mask, oldmask;

	nih_assert (args_copy[0] != NULL);
	nih_assert (signal != NULL);

	nih_warn (_("Re-executing %s"), args_copy[0]);

	/* Block signals while we work.  We're the last signal handler
	 * installed so this should mean that they're all handled now.
	 *
	 * The child must make sure that it unblocks these again when
	 * it's ready.
	 */
	sigfillset (&mask);
	sigprocmask (SIG_BLOCK, &mask, &oldmask);

	stateful_reexec ();

	/* We should never end up here since it likely indicates the
	 * new init binary is damaged.
	 *
	 * All we can do is restore the signal handler and drop back into
	 * the main loop.
	 */

	/* Restore */
	sigprocmask (SIG_SETMASK, &oldmask, NULL);
}


#ifndef DEBUG
/**
 * cad_handler:
 * @data: unused,
 * @signal: signal that called this handler.
 *
 * Handle having recieved the SIGINT signal, sent to us when somebody
 * presses Ctrl-Alt-Delete on the console.  We just generate a
 * ctrlaltdel event.
 **/
static void
cad_handler (void      *data,
	     NihSignal *signal)
{
	NIH_MUST (event_new (NULL, CTRLALTDEL_EVENT, NULL));
}

/**
 * kbd_handler:
 * @data: unused,
 * @signal: signal that called this handler.
 *
 * Handle having recieved the SIGWINCH signal, sent to us when somebody
 * presses Alt-UpArrow on the console.  We just generate a
 * kbdrequest event.
 **/
static void
kbd_handler (void      *data,
	     NihSignal *signal)
{
	NIH_MUST (event_new (NULL, KBDREQUEST_EVENT, NULL));
}

/**
 * pwr_handler:
 * @data: unused,
 * @signal: signal that called this handler.
 *
 * Handle having recieved the SIGPWR signal, sent to us when powstatd
 * changes the /etc/powerstatus file.  We just generate a
 * power-status-changed event and jobs read the file.
 **/
static void
pwr_handler (void      *data,
	     NihSignal *signal)
{
	NIH_MUST (event_new (NULL, PWRSTATUS_EVENT, NULL));
}

/**
 * hup_handler:
 * @data: unused,
 * @signal: signal that called this handler.
 *
 * Handle having recieved the SIGHUP signal, which we use to instruct us to
 * reload our configuration.
 **/
static void
hup_handler (void      *data,
	     NihSignal *signal)
{
	nih_info (_("Reloading configuration"));
	conf_reload (restart);
}

/**
 * usr1_handler:
 * @data: unused,
 * @signal: signal that called this handler.
 *
 * Handle having recieved the SIGUSR signal, which we use to instruct us to
 * reconnect to D-Bus.
 **/
static void
usr1_handler (void      *data,
	      NihSignal *signal)
{
	if (! control_bus) {
		nih_info (_("Reconnecting to system bus"));

		if (control_bus_open (restart) < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_warn ("%s: %s", _("Unable to connect to the system bus"),
				  err->message);
			nih_free (err);
		}
	}
}
#endif /* DEBUG */

/**
 * handle_confdir:
 *
 * Determine where system configuration files should be loaded from.
 **/
static void
handle_confdir (void)
{
	char *dir;

	/* user has already specified directory on command-line */
	if (conf_dir)
		goto out;

	conf_dir = CONFDIR;

	dir = getenv (CONFDIR_ENV);
	if (! dir)
		return;

	conf_dir = dir;

out:
	nih_debug ("Using alternate configuration directory %s",
			conf_dir);
}

/**
 * handle_logdir:
 *
 * Determine directory where job log files should be written to.
 **/
static void
handle_logdir (void)
{
	char *dir;

	/* user has already specified directory on command-line */
	if (log_dir)
		goto out;

	log_dir = JOB_LOGDIR;

	dir = getenv (LOGDIR_ENV);
	if (! dir)
		return;

	log_dir = dir;

out:
	nih_debug ("Using alternate log directory %s",
			log_dir);
}

/**  
 * NihOption setter function to handle selection of default console
 * type.
 *
 * Returns: 0 on success, -1 on invalid console type.
 **/
static int
console_type_setter (NihOption *option, const char *arg)
{
	 nih_assert (option);

	 default_console = (int)job_class_console_type (arg);

	 if (default_console == -1) {
		 nih_fatal ("%s: %s", _("invalid console type specified"), arg);
		 return -1;
	 }

	 return 0;
}


/**
 * perform_reexec:
 *
 * Perform a bare re-exec.
 *
 * Note that unless the appropriate command-line option has
 * _already_ been specified in @args_copy, all internal state will be lost.
 **/
static void
perform_reexec (void)
{
	NihError    *err;
	const char  *loglevel = NULL;

	/* Although we have a copy of the original arguments (which may
	 * have included an option to modify the log level), we need to
	 * handle the case where the log priority has been changed at
	 * runtime which potentially invalidates the original command-line
	 * option value.
	 *
	 * Fortuitously, this can be handled easily: NIH option parsing
	 * semantics allow any option to be specified multiple times -
	 * the last value seen is used. Therefore, we just append the
	 * current log-level option and ignore any existing (earlier)
	 * log level options.
	 *
	 * Note that should Upstart be re-exec'ed too many times,
	 * eventually an unexpected log level may result if the
	 * command-line becomes too large (and thus truncates).
	 *
	 * The correct way to handle this would be to prune now invalid
	 * options from the command-line to ensure it does not continue
	 * to increase. That said, if we hit the limit, worse things
	 * are probably going on so for now we'll settle for the
	 * simplistic approach.
	 */
	if (nih_log_priority <= NIH_LOG_DEBUG) {
		loglevel = "--debug";
	} else if (nih_log_priority <= NIH_LOG_INFO) {
		loglevel = "--verbose";
	} else if (nih_log_priority >= NIH_LOG_ERROR) {
		loglevel = "--error";
	} else {
		/* User has not modified default log level of
		 * NIH_LOG_MESSAGE.
		 */
		loglevel = NULL;
	}

	if (loglevel)
		NIH_MUST (nih_str_array_add (&args_copy, NULL, NULL, loglevel));

	/* if the currently running instance wasn't invoked as
	 * part of a re-exec, ensure that the next instance is (since
	 * otherwise, why would this function be being called!? :)
	 */
	if (! restart)
		NIH_MUST (nih_str_array_add (&args_copy, NULL, NULL, "--restart"));

	execv (args_copy[0], args_copy);
	nih_error_raise_system ();

	err = nih_error_get ();
	nih_error (_("Failed to re-execute %s: %s"), args_copy[0], err->message);
	nih_free (err);
}


/**
 * stateful_reexec:
 *
 * Perform re-exec with state-passing. UPSTART must be capable of
 * stateful re-exec for this routine to be called. Any failures
 * result in a basic re-exec being performed where all state
 * will be lost.
 *
 * The process involves the initial Upstart instance (PID 1) creating a
 * pipe and then forking. The child then writes its serialised state
 * over the pipe back to PID 1 which has now re-exec'd itself.
 *
 * Once the state has been passed, the child can exit.
 **/
static void
stateful_reexec (void)
{
	int    fds[2] = { -1 };
	pid_t  pid;

	if (pipe (fds) < 0)
		goto reexec;

	nih_info (_("Performing stateful re-exec"));

	/* retain the D-Bus connection across the re-exec */
	control_prepare_reexec ();

	pid = fork ();

	if (pid < 0)
		goto reexec;
	else if (pid > 0) {
		nih_local char *arg = NULL;

		/* Parent */
		close (fds[1]);

		/* Tell the new instance where to read the
		 * serialisation data from.
		 *
		 * Note that if the "new" instance is actually an older
		 * version of Upstart (that does not understand stateful
		 * re-exec), due to the way NIH handles command-line
		 * paring, this option will be ignored and the new instance
		 * will therefore not be able to read the state and overall
		 * a stateless re-exec will therefore be performed.
		 */
		arg = NIH_MUST (nih_strdup (NULL, "--state-fd"));
		NIH_MUST (nih_str_array_add (&args_copy, NULL, NULL, arg));

		arg = NIH_MUST (nih_sprintf (NULL, "%d", fds[0]));
		NIH_MUST (nih_str_array_add (&args_copy, NULL, NULL, arg));
	} else {
		/* Child */
		close (fds[0]);

		nih_info (_("Passing state from PID %d to parent"), (int)getpid ());

		/* D-Bus name must be relinquished now to allow parent
		 * from acquiring it.
		 */
		if (control_bus_release_name () < 0) {
			NihError *err;

			err = nih_error_get ();
			nih_error (_("Failed to release D-Bus name: %s"),
					err->message);
			nih_free (err);
		}

		if (state_write (fds[1]) < 0) {
			nih_error ("%s",
				_("Failed to write serialisation data"));
			exit (1);
		}

		/* The baton has now been passed */
		exit (0);
	}

reexec:
	/* Attempt stateful re-exec */
	perform_reexec ();
}
