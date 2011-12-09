/* upstart
 *
 * job.c - core state machine of tasks and services
 *
 * Copyright © 2010,2011 Canonical Ltd.
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

#include <errno.h>
#include <string.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/list.h>
#include <nih/hash.h>
#include <nih/signal.h>
#include <nih/logging.h>

#include <nih-dbus/dbus_error.h>
#include <nih-dbus/dbus_message.h>
#include <nih-dbus/dbus_object.h>
#include <nih-dbus/dbus_util.h>

#include "dbus/upstart.h"

#include "events.h"
#include "environ.h"
#include "process.h"
#include "session.h"
#include "job_class.h"
#include "job.h"
#include "job_process.h"
#include "event.h"
#include "event_operator.h"
#include "blocked.h"
#include "control.h"

#include "com.ubuntu.Upstart.Job.h"
#include "com.ubuntu.Upstart.Instance.h"


/**
 * job_new:
 * @class: class of job,
 * @name: name for new instance.
 *
 * Allocates and returns a new Job structure for the @class given,
 * appending it to the list of instances for @class.  The returned job
 * will also be an nih_alloc() child of @class.
 *
 * @name is used to uniquely identify the instance and is normally
 * generated by expanding the @class's instance member.
 *
 * Returns: newly allocated job structure or NULL if insufficient memory.
 **/
Job *
job_new (JobClass   *class,
	 const char *name)
{
	Job *job;
	int  i;

	nih_assert (class != NULL);
	nih_assert (name != NULL);

	control_init ();

	job = nih_new (class, Job);
	if (! job)
		return NULL;

	nih_list_init (&job->entry);

	nih_alloc_set_destructor (job, nih_list_destroy);

	job->name = nih_strdup (job, name);
	if (! job->name)
		goto error;

	job->class = class;

	if (job->class->session && job->class->session->chroot) {
		/* JobClass already contains a valid D-Bus path prefix for the job */
		job->path = nih_dbus_path (job, class->path, job->name, NULL);
	} else {
		job->path = nih_dbus_path (job, DBUS_PATH_UPSTART, "jobs",
				class->name, job->name, NULL);
	}

	if (! job->path)
		goto error;

	job->goal = JOB_STOP;
	job->state = JOB_WAITING;
	job->env = NULL;

	job->start_env = NULL;
	job->stop_env = NULL;

	job->stop_on = NULL;
	if (class->stop_on) {
		job->stop_on = event_operator_copy (job, class->stop_on);
		if (! job->stop_on)
			goto error;
	}

	job->fds = NULL;
	job->num_fds = 0;

	job->pid = nih_alloc (job, sizeof (pid_t) * PROCESS_LAST);
	if (! job->pid)
		goto error;

	for (i = 0; i < PROCESS_LAST; i++)
		job->pid[i] = 0;

	job->blocker = NULL;
	nih_list_init (&job->blocking);

	job->kill_timer = NULL;
	job->kill_process = -1;

	job->failed = FALSE;
	job->failed_process = -1;
	job->exit_status = 0;

	job->respawn_time = 0;
	job->respawn_count = 0;

	job->trace_forks = 0;
	job->trace_state = TRACE_NONE;

	job->log = NULL;

	nih_hash_add (class->instances, &job->entry);

	NIH_LIST_FOREACH (control_conns, iter) {
		NihListEntry   *entry = (NihListEntry *)iter;
		DBusConnection *conn = (DBusConnection *)entry->data;

		job_register (job, conn, TRUE);
	}

	return job;

error:
	nih_free (job);
	return NULL;
}

/**
 * job_register:
 * @job: job to register,
 * @conn: connection to register for,
 * @signal: emit the InstanceAdded signal.
 *
 * Register the @job instance with the D-Bus connection @conn, using
 * the path set when the job was created.
 **/
void
job_register (Job            *job,
	      DBusConnection *conn,
	      int             signal)
{
	nih_assert (job != NULL);
	nih_assert (conn != NULL);

	NIH_MUST (nih_dbus_object_new (job, conn, job->path,
				       job_interfaces, job));

	nih_debug ("Registered instance %s", job->path);

	if (signal)
		NIH_ZERO (job_class_emit_instance_added (conn, job->class->path,
							 job->path));
}


/**
 * job_change_goal:
 * @job: job to change goal of,
 * @goal: goal to change to.
 *
 * This function changes the current goal of a @job to the new @goal given,
 * performing any necessary state changes or actions (such as killing
 * the running process) to correctly enter the new goal.
 *
 * If the job is not in a rest state (WAITING or RUNNING), this has no
 * other effect than changing the goal; since the job is waiting on some
 * other event.  The goal change will cause it to take action to head
 * towards stopped.
 *
 * If the job is in the WAITING state and @goal is START, the job will
 * begin to be started and will block in the STARTING state for an event
 * to finish.
 *
 * If the job is in the RUNNING state and @goal is STOP, the job will
 * begin to be stopped and will either block in the PRE-STOP state for
 * the pre-stop script or the STOPPING state for an event to finish.
 *
 * Thus in all circumstances, @job is safe to use once this function
 * returns.  Though further calls to job_change_state may change that as
 * noted.
 **/
void
job_change_goal (Job     *job,
		 JobGoal  goal)
{
	nih_assert (job != NULL);

	if (job->goal == goal)
		return;

	nih_info (_("%s goal changed from %s to %s"), job_name (job),
		  job_goal_name (job->goal), job_goal_name (goal));

	job->goal = goal;

	NIH_LIST_FOREACH (control_conns, iter) {
		NihListEntry   *entry = (NihListEntry *)iter;
		DBusConnection *conn = (DBusConnection *)entry->data;

		NIH_ZERO (job_emit_goal_changed (
				conn, job->path,
				job_goal_name (job->goal)));
	}


	/* Normally whatever process or event is associated with the state
	 * will finish naturally, so all we need do is change the goal and
	 * we'll change direction through the state machine at that point.
	 *
	 * The exceptions are the natural rest sates of waiting and a
	 * running process; these need induction to get them moving.
	 */
	switch (goal) {
	case JOB_START:
		if (job->state == JOB_WAITING)
			job_change_state (job, job_next_state (job));

		break;
	case JOB_STOP:
		if (job->state == JOB_RUNNING)
			job_change_state (job, job_next_state (job));

		break;
	case JOB_RESPAWN:
		break;
	default:
		nih_assert_not_reached ();
	}
}

/**
 * job_change_state:
 * @job: job to change state of,
 * @state: state to change to.
 *
 * This function changes the current state of a @job to the new @state
 * given, performing any actions to correctly enter the new state (such
 * as spawning scripts or processes).
 *
 * The associated event is also queued by this function.
 *
 * Some state transitions are not be permitted and will result in an
 * assertion failure.  Also some state transitions may result in further
 * transitions, so the state when this function returns may not be the
 * state requested.
 *
 * WARNING: On return from this function, @job may no longer be valid
 * since it will be freed once it becomes fully stopped.
 **/
void
job_change_state (Job      *job,
		  JobState  state)
{
	nih_assert (job != NULL);

	while (job->state != state) {
		JobState old_state;
		int      unused;

		nih_assert (job->blocker == NULL);

		nih_info (_("%s state changed from %s to %s"), job_name (job),
			  job_state_name (job->state), job_state_name (state));

		old_state = job->state;
		job->state = state;

		NIH_LIST_FOREACH (control_conns, iter) {
			NihListEntry   *entry = (NihListEntry *)iter;
			DBusConnection *conn = (DBusConnection *)entry->data;

			NIH_ZERO (job_emit_state_changed (
					conn, job->path,
					job_state_name (job->state)));
		}

		/* Perform whatever action is necessary to enter the new
		 * state, such as executing a process or emitting an event.
		 */
		switch (job->state) {
		case JOB_STARTING:
			nih_assert (job->goal == JOB_START);
			nih_assert ((old_state == JOB_WAITING)
				    || (old_state == JOB_POST_STOP));

			/* Throw away our old environment and use the newly
			 * set environment from now on; unless that's NULL
			 * in which case we just keep our old environment.
			 */
			if (job->start_env) {
				if (job->env)
					nih_unref (job->env, job);

				job->env = job->start_env;
				job->start_env = NULL;
			}

			/* Throw away the stop environment */
			if (job->stop_env) {
				nih_unref (job->stop_env, job);
				job->stop_env = NULL;
			}

			/* Clear any old failed information */
			job->failed = FALSE;
			job->failed_process = -1;
			job->exit_status = 0;

			job->blocker = job_emit_event (job);

			break;
		case JOB_PRE_START:
			nih_assert (job->goal == JOB_START);
			nih_assert (old_state == JOB_STARTING);

			if (job->class->process[PROCESS_PRE_START]) {
				if (job_process_run (job, PROCESS_PRE_START) < 0) {
					job_failed (job, PROCESS_PRE_START, -1);
					job_change_goal (job, JOB_STOP);
					state = job_next_state (job);
				}
			} else {
				state = job_next_state (job);
			}

			break;
		case JOB_SPAWNED:
			nih_assert (job->goal == JOB_START);
			nih_assert (old_state == JOB_PRE_START);

			if (job->class->process[PROCESS_MAIN]) {
				if (job_process_run (job, PROCESS_MAIN) < 0) {
					job_failed (job, PROCESS_MAIN, -1);
					job_change_goal (job, JOB_STOP);
					state = job_next_state (job);
				} else if (job->class->expect == EXPECT_NONE)
					state = job_next_state (job);
			} else {
				state = job_next_state (job);
			}

			break;
		case JOB_POST_START:
			nih_assert (job->goal == JOB_START);
			nih_assert (old_state == JOB_SPAWNED);

			if (job->class->process[PROCESS_POST_START]) {
				if (job_process_run (job, PROCESS_POST_START) < 0)
					state = job_next_state (job);
			} else {
				state = job_next_state (job);
			}

			break;
		case JOB_RUNNING:
			nih_assert (job->goal == JOB_START);
			nih_assert ((old_state == JOB_POST_START)
				    || (old_state == JOB_PRE_STOP));

			if (old_state == JOB_PRE_STOP) {
				/* Throw away the stop environment */
				if (job->stop_env) {
					nih_unref (job->stop_env, job);
					job->stop_env = NULL;
				}

				/* Cancel the stop attempt */
				job_finished (job, FALSE);
			} else {
				job_emit_event (job);

				/* If we're not a task, our goal is to be
				 * running.
				 */
				if (! job->class->task)
					job_finished (job, FALSE);
			}

			break;
		case JOB_PRE_STOP:
			nih_assert (job->goal == JOB_STOP);
			nih_assert (old_state == JOB_RUNNING);

			if (job->class->process[PROCESS_PRE_STOP]) {
				if (job_process_run (job, PROCESS_PRE_STOP) < 0)
					state = job_next_state (job);
			} else {
				state = job_next_state (job);
			}

			break;
		case JOB_STOPPING:
			nih_assert ((old_state == JOB_STARTING)
				    || (old_state == JOB_PRE_START)
				    || (old_state == JOB_SPAWNED)
				    || (old_state == JOB_POST_START)
				    || (old_state == JOB_RUNNING)
				    || (old_state == JOB_PRE_STOP));

			job->blocker = job_emit_event (job);

			break;
		case JOB_KILLED:
			nih_assert (old_state == JOB_STOPPING);

			if (job->class->process[PROCESS_MAIN]
			    && (job->pid[PROCESS_MAIN] > 0)) {
				job_process_kill (job, PROCESS_MAIN);
			} else {
				state = job_next_state (job);
			}

			break;
		case JOB_POST_STOP:
			nih_assert (old_state == JOB_KILLED);

			if (job->class->process[PROCESS_POST_STOP]) {
				if (job_process_run (job, PROCESS_POST_STOP) < 0) {
					job_failed (job, PROCESS_POST_STOP, -1);
					job_change_goal (job, JOB_STOP);
					state = job_next_state (job);
				}
			} else {
				state = job_next_state (job);
			}

			break;
		case JOB_WAITING:
			nih_assert (job->goal == JOB_STOP);
			nih_assert ((old_state == JOB_POST_STOP)
				    || (old_state == JOB_STARTING));

			job_emit_event (job);

			job_finished (job, FALSE);

			/* Remove the job from the list of instances and
			 * then allow a better class to replace us
			 * in the hash table if we have no other instances
			 * and there is one.
			 */
			nih_list_remove (&job->entry);
			unused = job_class_reconsider (job->class);

			/* If the class is due to be deleted, free it
			 * taking the job with it; otherwise free the
			 * job.
			 */
			if (job->class->deleted && unused) {
				nih_debug ("Destroyed unused job %s",
					   job->class->name);
				nih_free (job->class);
			} else {
				nih_debug ("Destroyed inactive instance %s",
					   job_name (job));

				NIH_LIST_FOREACH (control_conns, iter) {
					NihListEntry   *entry = (NihListEntry *)iter;
					DBusConnection *conn = (DBusConnection *)entry->data;

					NIH_ZERO (job_class_emit_instance_removed (
							  conn,
							  job->class->path,
							  job->path));
				}

				nih_free (job);
			}

			return;
		}
	}
}

/**
 * job_next_state:
 * @job: job undergoing state change.
 *
 * The next state a job needs to change into is not always obvious as it
 * depends both on the current state and the ultimate goal of the job, ie.
 * whether we're moving towards stop or start.
 *
 * This function contains the logic to decide the next state the job should
 * be in based on the current state and goal.
 *
 * It is up to the caller to ensure the goal is set appropriately before
 * calling this function, for example setting it to JOB_STOP if something
 * failed.  It is also up to the caller to actually set the new state as
 * this simply returns the suggested one.
 *
 * Returns: suggested state to change to.
 **/
JobState
job_next_state (Job *job)
{
	nih_assert (job != NULL);

	switch (job->state) {
	case JOB_WAITING:
		switch (job->goal) {
		case JOB_STOP:
			nih_assert_not_reached ();
		case JOB_START:
			return JOB_STARTING;
		default:
			nih_assert_not_reached ();
		}
	case JOB_STARTING:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_PRE_START;
		default:
			nih_assert_not_reached ();
		}
	case JOB_PRE_START:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_SPAWNED;
		default:
			nih_assert_not_reached ();
		}
	case JOB_SPAWNED:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_POST_START;
		default:
			nih_assert_not_reached ();
		}
	case JOB_POST_START:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_RUNNING;
		case JOB_RESPAWN:
			job_change_goal (job, JOB_START);
			return JOB_STOPPING;
		default:
			nih_assert_not_reached ();
		}
	case JOB_RUNNING:
		switch (job->goal) {
		case JOB_STOP:
			if (job->class->process[PROCESS_MAIN]
			    && (job->pid[PROCESS_MAIN] > 0)) {
				return JOB_PRE_STOP;
			} else {
				return JOB_STOPPING;
			}
		case JOB_START:
			return JOB_STOPPING;
		default:
			nih_assert_not_reached ();
		}
	case JOB_PRE_STOP:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_RUNNING;
		case JOB_RESPAWN:
			job_change_goal (job, JOB_START);
			return JOB_STOPPING;
		default:
			nih_assert_not_reached ();
		}
	case JOB_STOPPING:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_KILLED;
		case JOB_START:
			return JOB_KILLED;
		default:
			nih_assert_not_reached ();
		}
	case JOB_KILLED:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_POST_STOP;
		case JOB_START:
			return JOB_POST_STOP;
		default:
			nih_assert_not_reached ();
		}
	case JOB_POST_STOP:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_WAITING;
		case JOB_START:
			return JOB_STARTING;
		default:
			nih_assert_not_reached ();
		}
	default:
		nih_assert_not_reached ();
	}
}


/**
 * job_failed:
 * @job: job that has failed,
 * @process: process that failed,
 * @status: status of @process at failure.
 *
 * Mark @job as having failed, unless it already has been marked so, storing
 * @process and @status so that they may show up as arguments and environment
 * to the stop and stopped events generated for the job.
 *
 * Additionally this marks the start and stop events as failed as well; this
 * is reported to the emitter of the event, and will also cause a failed event
 * to be generated after the event completes.
 *
 * @process may be -1 to indicate a failure to respawn, and @exit_status
 * may be -1 to indicate a spawn failure.
 **/
void
job_failed (Job         *job,
	    ProcessType  process,
	    int          status)
{
	nih_assert (job != NULL);

	if (job->failed)
		return;

	job->failed = TRUE;
	job->failed_process = process;
	job->exit_status = status;

	NIH_LIST_FOREACH (control_conns, iter) {
		NihListEntry   *entry = (NihListEntry *)iter;
		DBusConnection *conn = (DBusConnection *)entry->data;

		NIH_ZERO (job_emit_failed (conn, job->path, status));
	}

	job_finished (job, TRUE);
}

/**
 * job_finished:
 * @job: job that is blocking,
 * @failed: mark events as failed.
 *
 * This function unblocks any events blocking on @job; it is called when the
 * job reaches a rest state (waiting for all, running for services), when a
 * new command is received or when the job fails.
 *
 * If @failed is TRUE then the events that are blocking will be marked as
 * failed.
 **/
void
job_finished (Job *job,
	      int  failed)
{
	nih_assert (job != NULL);

	NIH_LIST_FOREACH_SAFE (&job->blocking, iter) {
		Blocked *blocked = (Blocked *)iter;

		switch (blocked->type) {
		case BLOCKED_EVENT:
			if (failed)
				blocked->event->failed = TRUE;

			event_unblock (blocked->event);

			break;
		case BLOCKED_JOB_START_METHOD:
			if (failed) {
				NIH_ZERO (nih_dbus_message_error (
						  blocked->message,
						  DBUS_INTERFACE_UPSTART ".Error.JobFailed",
						  _("Job failed to start")));
			} else {
				NIH_ZERO (job_class_start_reply (
						  blocked->message,
						  job->path));
			}

			break;
		case BLOCKED_JOB_STOP_METHOD:
			if (failed) {
				NIH_ZERO (nih_dbus_message_error (
						  blocked->message,
						  DBUS_INTERFACE_UPSTART ".Error.JobFailed",
						  _("Job failed while stopping")));
			} else {
				NIH_ZERO (job_class_stop_reply (
						  blocked->message));
			}

			break;
		case BLOCKED_JOB_RESTART_METHOD:
			if (failed) {
				NIH_ZERO (nih_dbus_message_error (
						  blocked->message,
						  DBUS_INTERFACE_UPSTART ".Error.JobFailed",
						  _("Job failed to restart")));
			} else {
				NIH_ZERO (job_class_restart_reply (
						  blocked->message,
						  job->path));
			}

			break;
		case BLOCKED_INSTANCE_START_METHOD:
			if (failed) {
				NIH_ZERO (nih_dbus_message_error (
						  blocked->message,
						  DBUS_INTERFACE_UPSTART ".Error.JobFailed",
						  _("Job failed to start")));
			} else {
				NIH_ZERO (job_start_reply (blocked->message));
			}

			break;
		case BLOCKED_INSTANCE_STOP_METHOD:
			if (failed) {
				NIH_ZERO (nih_dbus_message_error (
						  blocked->message,
						  DBUS_INTERFACE_UPSTART ".Error.JobFailed",
						  _("Job failed while stopping")));
			} else {
				NIH_ZERO (job_stop_reply (blocked->message));
			}

			break;
		case BLOCKED_INSTANCE_RESTART_METHOD:
			if (failed) {
				NIH_ZERO (nih_dbus_message_error (
						  blocked->message,
						  DBUS_INTERFACE_UPSTART ".Error.JobFailed",
						  _("Job failed to restart")));
			} else {
				NIH_ZERO (job_restart_reply (blocked->message));
			}

			break;
		default:
			nih_assert_not_reached ();
		}

		nih_free (blocked);
	}
}


/**
 * job_emit_event:
 * @job: job generating the event.
 *
 * Called from a state change because it believes an event should be
 * emitted.  Constructs the event with the right arguments and environment
 * and adds it to the pending queue.
 *
 * The starting and stopping events will record the job as blocking on
 * the event, and will change the job's state when they finish.
 *
 * The stopping and stopped events have an extra argument that is "ok" if
 * the job terminated successfully, or "failed" if it terminated with an
 * error.  If failed, a further argument indicates which process it was
 * that caused the failure and either an EXIT_STATUS or EXIT_SIGNAL
 * environment variable detailing it.
 *
 * Returns: new Event in the queue.
 **/
Event *
job_emit_event (Job *job)
{
	Event           *event;
	const char      *name;
	int              block = FALSE, stop = FALSE;
	nih_local char **env = NULL;
	char           **e;
	size_t           len;

	nih_assert (job != NULL);

	switch (job->state) {
	case JOB_STARTING:
		name = JOB_STARTING_EVENT;
		block = TRUE;
		break;
	case JOB_RUNNING:
		name = JOB_STARTED_EVENT;
		break;
	case JOB_STOPPING:
		name = JOB_STOPPING_EVENT;
		block = TRUE;
		stop = TRUE;
		break;
	case JOB_WAITING:
		name = JOB_STOPPED_EVENT;
		stop = TRUE;
		break;
	default:
		nih_assert_not_reached ();
	}

	len = 0;
	env = NIH_MUST (nih_str_array_new (NULL));

	/* Add the job and instance name */
	NIH_MUST (environ_set (&env, NULL, &len, TRUE,
			       "JOB=%s", job->class->name));
	NIH_MUST (environ_set (&env, NULL, &len, TRUE,
			       "INSTANCE=%s", job->name));

	/* Stop events include a "failed" argument if a process failed,
	 * otherwise stop events have an "ok" argument.
	 */
	if (stop && job->failed) {
		NIH_MUST (environ_add (&env, NULL, &len, TRUE,
				       "RESULT=failed"));

		/* Include information about the process that failed, and
		 * the signal/exit information.  If it was the spawn itself
		 * that failed, we don't include signal/exit information and
		 * if it was a respawn failure, we use the special "respawn"
		 * argument instead of the process name,
		 */
		if ((job->failed_process != (ProcessType)-1)
		    && (job->exit_status != -1)) {
			NIH_MUST (environ_set (&env, NULL, &len, TRUE,
					       "PROCESS=%s",
					       process_name (job->failed_process)));

			/* If the job was terminated by a signal, that
			 * will be stored in the higher byte and we
			 * set EXIT_SIGNAL instead of EXIT_STATUS.
			 */
			if (job->exit_status & ~0xff) {
				const char *sig;

				sig = nih_signal_to_name (job->exit_status >> 8);
				if (sig) {
					NIH_MUST (environ_set (&env, NULL, &len, TRUE,
							       "EXIT_SIGNAL=%s", sig));
				} else {
					NIH_MUST (environ_set (&env, NULL, &len, TRUE,
							       "EXIT_SIGNAL=%d", job->exit_status >> 8));
				}
			} else {
				NIH_MUST (environ_set (&env, NULL, &len, TRUE,
						       "EXIT_STATUS=%d", job->exit_status));
			}
		} else if (job->failed_process != (ProcessType)-1) {
			NIH_MUST (environ_set (&env, NULL, &len, TRUE,
					       "PROCESS=%s",
					       process_name (job->failed_process)));
		} else {
			NIH_MUST (environ_add (&env, NULL, &len, TRUE,
					       "PROCESS=respawn"));
		}
	} else if (stop) {
		NIH_MUST (environ_add (&env, NULL, &len, TRUE, "RESULT=ok"));
	}

	/* Add any exported variables from the job environment */
	for (e = job->class->export; e && *e; e++) {
		char * const *str;

		str = environ_lookup (job->env, *e, strlen (*e));
		if (str)
			NIH_MUST (environ_add (&env, NULL, &len, FALSE, *str));
	}

	event = NIH_MUST (event_new (NULL, name, env));
	event->session = job->class->session;

	if (block) {
		Blocked *blocked;

		blocked = NIH_MUST (blocked_new (event, BLOCKED_JOB, job));
		nih_list_add (&event->blocking, &blocked->entry);
	}

	return event;
}


/**
 * job_name:
 * @job: job to return name of.
 *
 * Returns a string used in messages that contains the job name; this
 * always begins with the name from the class, and then if set,
 * has the name of the instance appended in brackets.
 *
 * Returns: internal copy of the string.
 **/
const char *
job_name (Job *job)
{
	static char *name = NULL;

	nih_assert (job != NULL);

	if (name)
		nih_discard (name);

	if (*job->name) {
		name = NIH_MUST (nih_sprintf (NULL, "%s (%s)",
					      job->class->name, job->name));
	} else {
		name = NIH_MUST (nih_strdup (NULL, job->class->name));
	}

	return name;
}


/**
 * job_goal_name:
 * @goal: goal to convert.
 *
 * Converts an enumerated job goal into the string used for the status
 * and for logging purposes.
 *
 * Returns: static string or NULL if goal not known.
 **/
const char *
job_goal_name (JobGoal goal)
{
	switch (goal) {
	case JOB_STOP:
		return N_("stop");
	case JOB_START:
		return N_("start");
	case JOB_RESPAWN:
		return N_("respawn");
	default:
		return NULL;
	}
}

/**
 * job_goal_from_name:
 * @goal: goal to convert.
 *
 * Converts a job goal string into the enumeration.
 *
 * Returns: enumerated goal or -1 if not known.
 **/
JobGoal
job_goal_from_name (const char *goal)
{
	nih_assert (goal != NULL);

	if (! strcmp (goal, "stop")) {
		return JOB_STOP;
	} else if (! strcmp (goal, "start")) {
		return JOB_START;
	} else if (! strcmp (goal, "respawn")) {
		return JOB_RESPAWN;
	} else {
		return -1;
	}
}


/**
 * job_state_name:
 * @state: state to convert.
 *
 * Converts an enumerated job state into the string used for the status
 * and for logging purposes.
 *
 * Returns: static string or NULL if state not known.
 **/
const char *
job_state_name (JobState state)
{
	switch (state) {
	case JOB_WAITING:
		return N_("waiting");
	case JOB_STARTING:
		return N_("starting");
	case JOB_PRE_START:
		return N_("pre-start");
	case JOB_SPAWNED:
		return N_("spawned");
	case JOB_POST_START:
		return N_("post-start");
	case JOB_RUNNING:
		return N_("running");
	case JOB_PRE_STOP:
		return N_("pre-stop");
	case JOB_STOPPING:
		return N_("stopping");
	case JOB_KILLED:
		return N_("killed");
	case JOB_POST_STOP:
		return N_("post-stop");
	default:
		return NULL;
	}
}

/**
 * job_state_from_name:
 * @state: state to convert.
 *
 * Converts a job state string into the enumeration.
 *
 * Returns: enumerated state or -1 if not known.
 **/
JobState
job_state_from_name (const char *state)
{
	nih_assert (state != NULL);

	if (! strcmp (state, "waiting")) {
		return JOB_WAITING;
	} else if (! strcmp (state, "starting")) {
		return JOB_STARTING;
	} else if (! strcmp (state, "pre-start")) {
		return JOB_PRE_START;
	} else if (! strcmp (state, "spawned")) {
		return JOB_SPAWNED;
	} else if (! strcmp (state, "post-start")) {
		return JOB_POST_START;
	} else if (! strcmp (state, "running")) {
		return JOB_RUNNING;
	} else if (! strcmp (state, "pre-stop")) {
		return JOB_PRE_STOP;
	} else if (! strcmp (state, "stopping")) {
		return JOB_STOPPING;
	} else if (! strcmp (state, "killed")) {
		return JOB_KILLED;
	} else if (! strcmp (state, "post-stop")) {
		return JOB_POST_STOP;
	} else {
		return -1;
	}
}


/**
 * job_start:
 * @job: job to be started,
 * @message: D-Bus connection and message received,
 * @wait: whether to wait for command to finish before returning.
 *
 * Implements the top half of the Start method of the
 * com.ubuntu.Upstart.Instance interface, the bottom half may be found in
 * job_finished().
 *
 * Called on a stopping instance @job to cause it to be restarted.  If the
 * instance goal is already start, the com.ubuntu.Upstart.Error.AlreadyStarted
 * D_Bus error will be returned immediately.  If the instance fails to
 * start again, the com.ubuntu.Upstart.Error.JobFailed D-Bus error will
 * be returned when the problem occurs.
 *
 * When @wait is TRUE the method call will not return until the job has
 * finished starting (running for tasks); when @wait is FALSE, the method
 * call returns once the command has been processed and the goal changed.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
job_start (Job             *job,
	   NihDBusMessage  *message,
	   int              wait)
{
	Session *session;
	Blocked *blocked = NULL;

	nih_assert (job != NULL);
	nih_assert (message != NULL);

	/* Don't permit out-of-session modification */
	session = session_from_dbus (NULL, message);
	if (session != job->class->session) {
		nih_dbus_error_raise_printf (
			DBUS_INTERFACE_UPSTART ".Error.PermissionDenied",
			_("You do not have permission to modify job: %s"),
			job_name (job));
		return -1;
	}

	if (job->goal == JOB_START) {
		nih_dbus_error_raise_printf (
			DBUS_INTERFACE_UPSTART ".Error.AlreadyStarted",
			_("Job is already running: %s"),
			job_name (job));

		return -1;
	}

	if (wait) {
		blocked = blocked_new (job, BLOCKED_INSTANCE_START_METHOD,
				       message);
		if (! blocked)
			nih_return_system_error (-1);
	}

	if (job->start_env)
		nih_unref (job->start_env, job);
	job->start_env = NULL;

	job_finished (job, FALSE);
	if (blocked)
		nih_list_add (&job->blocking, &blocked->entry);

	job_change_goal (job, JOB_START);

	if (! wait)
		NIH_ZERO (job_start_reply (message));

	return 0;
}

/**
 * job_stop:
 * @job: job to be stopped,
 * @message: D-Bus connection and message received,
 * @wait: whether to wait for command to finish before returning.
 *
 * Implements the top half of the Stop method of the
 * com.ubuntu.Upstart.Instance interface, the bottom half may be found in
 * job_finished().
 *
 * Called on a running instance @job to cause it to be stopped.  If the
 * instance goal is already stop, the com.ubuntu.Upstart.Error.AlreadyStopped
 * D_Bus error will be returned immediately.  If the instance fails while
 * stopping, the com.ubuntu.Upstart.Error.JobFailed D-Bus error will
 * be returned when the problem occurs.
 *
 * When @wait is TRUE the method call will not return until the job has
 * finished stopping; when @wait is FALSE, the method call returns once
 * the command has been processed and the goal changed.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
job_stop (Job            *job,
	  NihDBusMessage *message,
	  int             wait)
{
	Session *session;
	Blocked *blocked = NULL;

	nih_assert (job != NULL);
	nih_assert (message != NULL);

	/* Don't permit out-of-session modification */
	session = session_from_dbus (NULL, message);
	if (session != job->class->session) {
		nih_dbus_error_raise_printf (
			DBUS_INTERFACE_UPSTART ".Error.PermissionDenied",
			_("You do not have permission to modify job: %s"),
			job_name (job));
		return -1;
	}

	if (job->goal == JOB_STOP) {
		nih_dbus_error_raise_printf (
			DBUS_INTERFACE_UPSTART ".Error.AlreadyStopped",
			_("Job has already been stopped: %s"),
			job_name (job));

		return -1;
	}

	if (wait) {
		blocked = blocked_new (job, BLOCKED_INSTANCE_STOP_METHOD,
				       message);
		if (! blocked)
			nih_return_system_error (-1);
	}

	if (job->stop_env)
		nih_unref (job->stop_env, job);
	job->stop_env = NULL;

	job_finished (job, FALSE);
	if (blocked)
		nih_list_add (&job->blocking, &blocked->entry);

	job_change_goal (job, JOB_STOP);

	if (! wait)
		NIH_ZERO (job_stop_reply (message));

	return 0;
}

/**
 * job_restart:
 * @job: job to be restarted,
 * @message: D-Bus connection and message received,
 * @wait: whether to wait for command to finish before returning.
 *
 * Implements the top half of the Restart method of the
 * com.ubuntu.Upstart.Instance interface, the bottom half may be found in
 * job_finished().
 *
 * Called on a running instance @job to cause it to be restarted.  If the
 * instance goal is already stop, the com.ubuntu.Upstart.Error.AlreadyStopped
 * D-Bus error will be returned immediately.  If the instance fails to
 * restart, the com.ubuntu.Upstart.Error.JobFailed D-Bus error will
 * be returned when the problem occurs.
 *
 * When @wait is TRUE the method call will not return until the job has
 * finished starting again (running for tasks); when @wait is FALSE, the
 * method call returns once the command has been processed and the goal
 * changed.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
job_restart (Job            *job,
	     NihDBusMessage *message,
	     int             wait)
{
	Session *session;
	Blocked *blocked = NULL;

	nih_assert (job != NULL);
	nih_assert (message != NULL);

	/* Don't permit out-of-session modification */
	session = session_from_dbus (NULL, message);
	if (session != job->class->session) {
		nih_dbus_error_raise_printf (
			DBUS_INTERFACE_UPSTART ".Error.PermissionDenied",
			_("You do not have permission to modify job: %s"),
			job_name (job));
		return -1;
	}

	if (job->goal == JOB_STOP) {
		nih_dbus_error_raise_printf (
			DBUS_INTERFACE_UPSTART ".Error.AlreadyStopped",
			_("Job has already been stopped: %s"),
			job_name (job));

		return -1;
	}

	if (wait) {
		blocked = blocked_new (job, BLOCKED_INSTANCE_RESTART_METHOD,
				       message);
		if (! blocked)
			nih_return_system_error (-1);
	}

	if (job->start_env)
		nih_unref (job->start_env, job);
	job->start_env = NULL;

	if (job->stop_env)
		nih_unref (job->stop_env, job);
	job->stop_env = NULL;

	job_finished (job, FALSE);
	if (blocked)
		nih_list_add (&job->blocking, &blocked->entry);

	job_change_goal (job, JOB_STOP);
	job_change_goal (job, JOB_START);

	if (! wait)
		NIH_ZERO (job_restart_reply (message));

	return 0;
}


/**
 * job_get_name:
 * @job: job to obtain name from,
 * @message: D-Bus connection and message received,
 * @name: pointer for reply string.
 *
 * Implements the get method for the name property of the
 * com.ubuntu.Upstart.Instance interface.
 *
 * Called to obtain the instance name of the given @job, which will be stored
 * in @name.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
job_get_name (Job             *job,
	      NihDBusMessage  *message,
	      char           **name)
{
	nih_assert (job != NULL);
	nih_assert (message != NULL);
	nih_assert (name != NULL);

	*name = job->name;
	nih_ref (*name, message);

	return 0;
}

/**
 * job_get_goal:
 * @job: job to obtain goal from,
 * @message: D-Bus connection and message received,
 * @goal: pointer for reply string.
 *
 * Implements the get method for the goal property of the
 * com.ubuntu.Upstart.Instance interface.
 *
 * Called to obtain the goal of the given @job as a string, which will be
 * stored in @goal.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
job_get_goal (Job             *job,
	      NihDBusMessage  *message,
	      char           **goal)
{
	nih_assert (job != NULL);
	nih_assert (message != NULL);
	nih_assert (goal != NULL);

	*goal = nih_strdup (message, job_goal_name (job->goal));
	if (! *goal)
		nih_return_no_memory_error (-1);

	return 0;
}

/**
 * job_get_state:
 * @job: job to obtain state from,
 * @message: D-Bus connection and message received,
 * @state: pointer for reply string.
 *
 * Implements the get method for the state property of the
 * com.ubuntu.Upstart.Instance interface.
 *
 * Called to obtain the state of the given @job as a string, which will be
 * stored in @state.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
job_get_state (Job             *job,
	      NihDBusMessage  *message,
	      char           **state)
{
	nih_assert (job != NULL);
	nih_assert (message != NULL);
	nih_assert (state != NULL);

	*state = nih_strdup (message, job_state_name (job->state));
	if (! *state)
		nih_return_no_memory_error (-1);

	return 0;
}


/**
 * job_get_processes:
 * @job: job to obtain state from,
 * @message: D-Bus connection and message received,
 * @processes: pointer for reply array.
 *
 * Implements the get method for the processes property of the
 * com.ubuntu.Upstart.Instance interface.
 *
 * Called to obtain the current set of processes for the given @job as an
 * array of process names and pids, which will be stored in @processes.
 *
 * Returns: zero on success, negative value on raised error.
 **/
int
job_get_processes (Job *                  job,
		   NihDBusMessage *       message,
		   JobProcessesElement ***processes)
{
	size_t num_processes;

	nih_assert (job != NULL);
	nih_assert (message != NULL);
	nih_assert (processes != NULL);

	*processes = nih_alloc (message, sizeof (JobProcessesElement *) * 1);
	if (! *processes)
		nih_return_no_memory_error (-1);

	num_processes = 0;
	(*processes)[num_processes] = NULL;

	for (int i = 0; i < PROCESS_LAST; i++) {
		JobProcessesElement * process;
		JobProcessesElement **tmp;

		if (job->pid[i] <= 0)
			continue;

		process = nih_new (*processes, JobProcessesElement);
		if (! process) {
			nih_error_raise_no_memory ();
			nih_free (*processes);
			return -1;
		}

		process->item0 = nih_strdup (process, process_name (i));
		if (! process->item0) {
			nih_error_raise_no_memory ();
			nih_free (*processes);
			return -1;
		}

		process->item1 = job->pid[i];

		tmp = nih_realloc (*processes, message,
				   (sizeof (JobProcessesElement *)
				    * (num_processes + 2)));
		if (! tmp) {
			nih_error_raise_no_memory ();
			nih_free (*processes);
			return -1;
		}

		*processes = tmp;
		(*processes)[num_processes++] = process;
		(*processes)[num_processes] = NULL;
	}

	return 0;
}
