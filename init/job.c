/* upstart
 *
 * job.c - core state machine of tasks and services
 *
 * Copyright © 2008 Canonical Ltd.
 * Author: Scott James Remnant <scott@netsplit.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif /* HAVE_CONFIG_H */


#include <sys/types.h>

#include <string.h>

#include <nih/macros.h>
#include <nih/alloc.h>
#include <nih/string.h>
#include <nih/list.h>
#include <nih/signal.h>
#include <nih/logging.h>

#include "events.h"
#include "environ.h"
#include "process.h"
#include "job_class.h"
#include "job.h"
#include "job_process.h"
#include "event.h"
#include "event_operator.h"
#include "control.h"


/* Prototypes for static functions */


/**
 * job_new:
 * @class: class of job,
 * @name: name for new instance.
 *
 * Allocates and returns a new Job structure for the @class given,
 * appending it to the list of instances for @class.  The returned job
 * will also be an nih_alloc() child of @class.
 *
 * @name is used to uniquely identify the instance, and must be given if
 * the @class instance member is not NULL.
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
	nih_assert ((! class->instance) || (name != NULL));

	job = nih_new (class, Job);
	if (! job)
		return NULL;

	nih_list_init (&job->entry);

	nih_alloc_set_destructor (job, (NihDestructor)nih_list_destroy);

	job->class = class;

	job->name = NULL;
	if (name) {
		job->name = nih_strdup (job, name);
		if (! job->name)
			goto error;
	}

	job->path = nih_dbus_path (job, CONTROL_ROOT, "jobs",
				   class->name, name ? name : "active", NULL);
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

	job->pid = nih_alloc (job, sizeof (pid_t) * PROCESS_LAST);
	if (! job->pid)
		goto error;

	for (i = 0; i < PROCESS_LAST; i++)
		job->pid[i] = 0;

	job->blocked = NULL;
	job->blocking = NULL;

	job->kill_timer = NULL;

	job->failed = FALSE;
	job->failed_process = -1;
	job->exit_status = 0;

	job->respawn_time = 0;
	job->respawn_count = 0;

	job->trace_forks = 0;
	job->trace_state = TRACE_NONE;

	nih_list_add (&class->instances, &job->entry);

	return job;

error:
	nih_free (job);
	return NULL;
}

/**
 * job_instance:
 * @class: job class,
 * @name: name of instance to find.
 *
 * This function is used to find a particular instance of @class.
 *
 * For singleton jobs, this will always be that instance if active or NULL
 * if not, so a new one will be created.  For instance jobs, @name must
 * not be NULL and will be looked up in the list of active instances.
 *
 * Returns: existing instance or NULL if a new one should be created.
 **/
Job *
job_instance (JobClass   *class,
	      const char *name)
{
	nih_assert (class != NULL);

	/* There aren't any instances in the list, always return NULL */
	if (NIH_LIST_EMPTY (&class->instances))
		return NULL;

	/* Not an instance job, always return the first instance */
	if (! class->instance)
		return (Job *)class->instances.next;

	nih_assert (name != NULL);

	/* Lookup an instance with the name given */
	NIH_LIST_FOREACH (&class->instances, iter) {
		Job *job = (Job *)iter;

		nih_assert (job->name != NULL);

		if (! strcmp (job->name, name))
			return job;
	}

	return NULL;
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
 * WARNING: On return from this function, @job may no longer be valid
 * since it will be freed once it becomes fully stopped; it may only be
 * called without unexpected side-effects if you are not in the WAITING
 * or RUNNING state and changing the goal to START or STOP respectively.
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

		nih_assert (job->blocked == NULL);

		nih_info (_("%s state changed from %s to %s"), job_name (job),
			  job_state_name (job->state), job_state_name (state));

		old_state = job->state;
		job->state = state;

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
					nih_free (job->env);

				job->env = job->start_env;
				job->start_env = NULL;
			}

			/* Throw away the stop environment */
			if (job->stop_env) {
				nih_free (job->stop_env);
				job->stop_env = NULL;
			}

			/* Clear any old failed information */
			job->failed = FALSE;
			job->failed_process = -1;
			job->exit_status = 0;

			job->blocked = job_emit_event (job);

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
					nih_free (job->stop_env);
					job->stop_env = NULL;
				}

				/* Cancel the stop attempt */
				job_unblock (job, FALSE);
			} else {
				job_emit_event (job);

				/* If we're not a task, our goal is to be
				 * running.
				 */
				if (! job->class->task)
					job_unblock (job, FALSE);
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
			nih_assert ((old_state == JOB_PRE_START)
				    || (old_state == JOB_SPAWNED)
				    || (old_state == JOB_POST_START)
				    || (old_state == JOB_RUNNING)
				    || (old_state == JOB_PRE_STOP));

			job->blocked = job_emit_event (job);

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

			job_unblock (job, FALSE);

			/* Remove the job from the list of instances and
			 * then allow a better class to replace us
			 * in the hash table if we have no other instances
			 * and there is one.
			 */
			nih_list_remove (&job->entry);
			job_class_reconsider (job->class);

			/* If the class is due to be deleted, free it
			 * taking the job with it; otherwise free the
			 * job.
			 */
			if (job->class->deleted) {
				nih_free (job->class);
			} else {
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
		}
	case JOB_STARTING:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_PRE_START;
		}
	case JOB_PRE_START:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_SPAWNED;
		}
	case JOB_SPAWNED:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_POST_START;
		}
	case JOB_POST_START:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_RUNNING;
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
		}
	case JOB_PRE_STOP:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_STOPPING;
		case JOB_START:
			return JOB_RUNNING;
		}
	case JOB_STOPPING:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_KILLED;
		case JOB_START:
			return JOB_KILLED;
		}
	case JOB_KILLED:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_POST_STOP;
		case JOB_START:
			return JOB_POST_STOP;
		}
	case JOB_POST_STOP:
		switch (job->goal) {
		case JOB_STOP:
			return JOB_WAITING;
		case JOB_START:
			return JOB_STARTING;
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

	job_unblock (job, TRUE);
}

/**
 * job_unblock:
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
job_unblock (Job *job,
	     int  failed)
{
	nih_assert (job != NULL);

	if (job->blocking) {
		NIH_LIST_FOREACH (job->blocking, iter) {
			NihListEntry *entry = (NihListEntry *)iter;
			Event        *event = (Event *)entry->data;

			nih_assert (event != NULL);

			if (failed)
				event->failed = TRUE;

			event_unblock (event);
		}

		nih_free (job->blocking);
		job->blocking = NULL;
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
	Event       *event;
	const char  *name;
	int          stop = FALSE;
	char       **env = NULL, **e;
	size_t       len;

	nih_assert (job != NULL);

	switch (job->state) {
	case JOB_STARTING:
		name = JOB_STARTING_EVENT;
		break;
	case JOB_RUNNING:
		name = JOB_STARTED_EVENT;
		break;
	case JOB_STOPPING:
		name = JOB_STOPPING_EVENT;
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
	NIH_MUST (env = nih_str_array_new (NULL));

	/* Add the job and instance name */
	NIH_MUST (environ_set (&env, NULL, &len, TRUE,
			       "JOB=%s", job->class->name));
	if (job->name)
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
		if ((job->failed_process != -1) && (job->exit_status != -1)) {
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
		} else if (job->failed_process != -1) {
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

	event = event_new (NULL, name, env);

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
		nih_free (name);

	if (job->name) {
		NIH_MUST (name = nih_sprintf (NULL, "%s (%s)",
					      job->class->name, job->name));
	} else {
		NIH_MUST (name = nih_strdup (NULL, job->class->name));
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
