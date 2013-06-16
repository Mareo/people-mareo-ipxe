/*
 * Copyright (C) 2013 Michael Brown <mbrown@fensystems.co.uk>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

FILE_LICENCE ( GPL2_OR_LATER );

/**
 * @file
 *
 * Null reboot mechanism
 *
 */

#include <stdio.h>
#include <ipxe/reboot.h>

/**
 * Reboot system
 *
 * @v warm		Perform a warm reboot
 */
static void null_reboot ( int warm __unused ) {

	printf ( "Cannot reboot; not implemented\n" );
	while ( 1 ) {}
}

PROVIDE_REBOOT ( null, reboot, null_reboot );
