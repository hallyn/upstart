/* upstart
 *
 * test_job.c - test suite for init/job.c
 *
 * Copyright © 2006 Canonical Ltd.
 * Author: Scott James Remnant <scott@ubuntu.com>.
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

#include <config.h>

#include <stdio.h>
#include <string.h>

#include <nih/alloc.h>

#include "job.h"


int
test_new (void)
{
	Job *job;
	int  ret = 0, i;

	printf ("Testing job_new()\n");
	job = job_new (NULL, "test");

	/* Name should be set */
	if (strcmp (job->name, "test")) {
		printf ("BAD: job name set incorrectly.\n");
		ret = 1;
	}

	/* Name should be a copy attached to the job */
	if (nih_alloc_parent (job->name) != job) {
		printf ("BAD: nih_alloc was not used for job name.\n");
		ret = 1;
	}

	/* Goal should be to stop the process */
	if (job->goal != JOB_STOP) {
		printf ("BAD: job goal set incorrectly.\n");
		ret = 1;
	}

	/* State should be waiting for event */
	if (job->state != JOB_WAITING) {
		printf ("BAD: job state set incorrectly.\n");
		ret = 1;
	}

	/* There should be no process */
	if (job->process_state != PROCESS_NONE) {
		printf ("BAD: job process state set incorrectly.\n");
		ret = 1;
	}

	/* Kill timeout should be the default */
	if (job->kill_timeout != JOB_DEFAULT_KILL_TIMEOUT) {
		printf ("BAD: job kill timeout set incorrectly.\n");
		ret = 1;
	}

	/* PID timeout should be the default */
	if (job->pid_timeout != JOB_DEFAULT_PID_TIMEOUT) {
		printf ("BAD: job pid timeout set incorrectly.\n");
		ret = 1;
	}

	/* The console should be logged */
	if (job->console != CONSOLE_LOGGED) {
		printf ("BAD: job console type set incorrectly.\n");
		ret = 1;
	}

	/* Umask should be the default */
	if (job->umask != JOB_DEFAULT_UMASK) {
		printf ("BAD: job umask set incorrectly.\n");
		ret = 1;
	}

	/* Limits should be all NULL (unset) */
	for (i = 0; i < RLIMIT_NLIMITS; i++) {
		if (job->limits[i] != NULL) {
			printf ("BAD: job limits set incorrectly.\n");
			ret = 1;
			break;
		}
	}

	/* Should be in jobs list */
	if (NIH_LIST_EMPTY (&job->entry)) {
		printf ("BAD: not placed into jobs list.\n");
		ret = 1;
	}

	/* Should have been allocated using nih_alloc */
	if (nih_alloc_size (job) != sizeof (Job)) {
		printf ("BAD: nih_alloc was not used for job.\n");
		ret = 1;
	}

	return ret;
}


int
main (int   argc,
      char *argv[])
{
	int ret = 0;

	ret |= test_new ();

	return ret;
}
