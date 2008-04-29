/* upstart
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

#ifndef INIT_CONTROL_H
#define INIT_CONTROL_H

#include <nih/macros.h>

#include <nih/dbus.h>


/**
 * CONTROL_BUS_NAME:
 *
 * Well-known name that we register on the system bus so that clients may
 * contact us.
 **/
#define CONTROL_BUS_NAME "com.ubuntu.Upstart"

/**
 * CONTROL_ROOT:
 *
 * Well-known object name that we register for the manager object, and that
 * we use as the root path for all of our other objects.
 **/
#define CONTROL_ROOT "/com/ubuntu/Upstart"


NIH_BEGIN_EXTERN

DBusConnection *control_bus;


int   control_bus_open        (void);
void  control_bus_close       (void);

NIH_END_EXTERN

#endif /* INIT_CONTROL_H */
