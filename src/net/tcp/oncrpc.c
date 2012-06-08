/*
 * Copyright (C) 2007 Michael Brown <mbrown@fensystems.co.uk>.
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
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <byteswap.h>
#include <ipxe/socket.h>
#include <ipxe/tcpip.h>
#include <ipxe/in.h>
#include <ipxe/iobuf.h>
#include <ipxe/xfer.h>
#include <ipxe/open.h>
#include <ipxe/uri.h>
#include <ipxe/features.h>
#include <ipxe/oncrpc.h>

/** @file
 *
 * SUN ONC RPC protocol
 *
 */

/** Set most significant bit to 1. */
#define SET_LAST_FRAME( x ) ( (x) |= 1 << 31 )
#define GET_FRAME_SIZE( x ) ( (x) & ~( 1 << 31 ) )

#define ONCRPC_HEADER_SIZE ( 12 * sizeof ( uint32_t ) )

#define ONCRPC_CALL 0
#define ONCRPC_REPLY 1

struct oncrpc_cred oncrpc_auth_none = {
	.flavor = ONCRPC_AUTH_NONE,
	.length = 0
};

static int oncrpc_deliver ( struct oncrpc_session *session,
                            struct io_buffer *io_buf,
                            struct xfer_metadata *meta );
static void oncrpc_window_changed ( struct oncrpc_session *session );

static struct interface_operation oncrpc_intf_operations[] = {
	INTF_OP ( xfer_deliver, struct oncrpc_session *, oncrpc_deliver ),
	INTF_OP ( intf_close, struct oncrpc_session *, oncrpc_close_session ),
	INTF_OP ( xfer_window_changed, struct oncrpc_session *,
	          oncrpc_window_changed ),
};

static struct interface_descriptor oncrpc_intf_desc =
	INTF_DESC ( struct oncrpc_session, intf, oncrpc_intf_operations );

static int oncrpc_deliver ( struct oncrpc_session *session,
                            struct io_buffer *io_buf,
                            struct xfer_metadata *meta __unused ) {

	struct oncrpc_reply      reply;
	oncrpc_callback_t        callback = NULL;
	uint64_t                 fragment_size;

	fragment_size      = GET_FRAME_SIZE ( oncrpc_iob_get_int ( io_buf ) );

	reply.rpc_id       = oncrpc_iob_get_int ( io_buf );

	if ( oncrpc_iob_get_int ( io_buf ) != ONCRPC_REPLY )
		return -ENOTSUP;

	reply.reply_state  = oncrpc_iob_get_int ( io_buf );
	reply.accept_state = oncrpc_iob_get_int ( io_buf );
	reply.data         = io_buf;

	struct oncrpc_pending_reply *p;
	struct oncrpc_pending_reply *tmp;

	list_for_each_entry_safe ( p, tmp, &session->pending_reply, list ) {
		if ( reply.rpc_id != p->rpc_id )
			continue;

		callback = p->callback;
		list_del ( &p->list );
		free ( p );
	}

	if ( callback == NULL )
		return 0;

	return callback ( session, &reply );

}

static void oncrpc_window_changed ( struct oncrpc_session *session ) {
	if ( ! xfer_window ( &session->intf ) )
		return;

	struct oncrpc_pending_call *p;
	struct oncrpc_pending_call *tmp;

	list_for_each_entry_safe ( p, tmp, &session->pending_call, list ) {
		if ( ( xfer_deliver_iob ( &session->intf, p->data ) ) != 0 )
			continue;

		list_del ( &p->list );
		free ( p );
	}
}

void oncrpc_init_session ( struct oncrpc_session *session,
                           struct oncrpc_cred *credential,
                           struct oncrpc_cred *verifier, uint32_t prog_name,
                           uint32_t prog_vers ) {
	if ( ! session )
		return;

	session->credential = credential;
	session->verifier   = verifier;
	session->prog_name  = prog_name;
	session->prog_vers  = prog_vers;

	INIT_LIST_HEAD ( &session->pending_call );
	INIT_LIST_HEAD ( &session->pending_reply );
	intf_init ( &session->intf, &oncrpc_intf_desc, NULL );
}

void oncrpc_close_session ( struct oncrpc_session *session, int rc ) {
	if ( ! session )
		return;

	struct oncrpc_pending_reply *pr;
	struct oncrpc_pending_reply *tr;

	list_for_each_entry_safe ( pr, tr, &session->pending_reply, list ) {
		list_del ( &pr->list );
		//free ( pr );
	}

	struct oncrpc_pending_call *pc;
	struct oncrpc_pending_call *tc;

	list_for_each_entry_safe ( pc, tc, &session->pending_call, list ) {
		free ( pc->data );
		list_del ( &pc->list );
		free ( pc );
	}

	intf_shutdown ( &session->intf, rc );
}

size_t oncrpc_iob_add_string ( struct io_buffer *io_buf, const char *val ) {
	const char *s;

	oncrpc_iob_add_int ( io_buf, strlen ( val ) );

	for ( s = val; *s != '\0'; ++s )
		* ( char * ) iob_put ( io_buf, sizeof ( *s ) ) = *s;

	while ( ( s++ - val ) % 4 != 0 )
		* ( char * ) iob_put ( io_buf, sizeof ( *s ) ) = '\0';

	return ( ( s - val ) - 1 + sizeof ( uint32_t ) );
}

size_t oncrpc_iob_add_intarray ( struct io_buffer *io_buf, size_t size,
                                 const uint32_t *array ) {
	size_t i;

	oncrpc_iob_add_int ( io_buf, size );

	for ( i = 0; i < size; ++i )
		oncrpc_iob_add_int ( io_buf, array[i] );

	return ( ( size + 1 ) * sizeof ( uint32_t ) );
}

size_t oncrpc_iob_add_cred ( struct io_buffer *io_buf,
                             struct oncrpc_cred *cred ) {
	if ( ! io_buf || ! cred )
		return 0;

	size_t s = 0;

	s += oncrpc_iob_add_int ( io_buf, cred->flavor );
	s += oncrpc_iob_add_int ( io_buf, cred->length );

	struct oncrpc_cred_sys *syscred = ( void * ) cred;
	switch ( cred->flavor ) {
		case ONCRPC_AUTH_NONE:
			break;

		case ONCRPC_AUTH_SYS:
			s += oncrpc_iob_add_int ( io_buf, syscred->stamp );
			s += oncrpc_iob_add_string ( io_buf,
			                             syscred->hostname );
			s += oncrpc_iob_add_int ( io_buf, syscred->uid );
			s += oncrpc_iob_add_int ( io_buf, syscred->gid );
			s += oncrpc_iob_add_intarray ( io_buf,
			                               syscred->aux_gid_len,
			                               syscred->aux_gid );
			break;
	}

	return s;
}

size_t oncrpc_iob_get_cred ( struct io_buffer *io_buf,
                             struct oncrpc_cred *cred ) {
	cred->flavor = oncrpc_iob_get_int ( io_buf );
	cred->length = oncrpc_iob_get_int ( io_buf );

	return ( 2 * sizeof ( uint32_t ) );
}

int oncrpc_call_iob ( struct oncrpc_session *session, uint32_t proc_name,
                      struct io_buffer *io_buf, oncrpc_callback_t cb ) {
	if ( ! session )
		return -EINVAL;

	int rc;
	uint32_t frame_size = 0;
	struct io_buffer *call_buf;
	struct oncrpc_pending_reply *pending_reply;
	struct oncrpc_pending_call *pending_call;

	call_buf = alloc_iob ( ONCRPC_HEADER_SIZE + iob_len ( io_buf ) +
	                       session->credential->length +
	                       session->verifier->length );

	if ( ! call_buf )
		return -ENOBUFS;

	pending_reply = malloc ( sizeof ( struct oncrpc_pending_reply ) );
	if ( ! ( pending_reply ) ) {
		free_iob ( call_buf );
		return -ENOBUFS;
	}


	iob_put ( call_buf, sizeof ( frame_size ) );
	frame_size += oncrpc_iob_add_int ( call_buf, session->rpc_id++ );
	frame_size += oncrpc_iob_add_int ( call_buf, ONCRPC_CALL );
	frame_size += oncrpc_iob_add_int ( call_buf, ONCRPC_VERS );
	frame_size += oncrpc_iob_add_int ( call_buf, session->prog_name );
	frame_size += oncrpc_iob_add_int ( call_buf, session->prog_vers );
	frame_size += oncrpc_iob_add_int ( call_buf, proc_name );
	frame_size += oncrpc_iob_add_cred ( call_buf, session->credential );
	frame_size += oncrpc_iob_add_cred ( call_buf, session->verifier );

	frame_size += iob_len ( io_buf );
	SET_LAST_FRAME ( frame_size );

	* ( uint32_t * ) call_buf->data = htonl ( frame_size );

	memcpy ( call_buf->tail, io_buf->data, iob_len (io_buf));
	iob_put ( call_buf, iob_len (io_buf) );

	if (  ! xfer_window ( &session->intf ) ) {
		pending_call = malloc ( sizeof ( struct oncrpc_pending_call ) );
		if ( ! pending_call ) {
			free ( call_buf );
			free ( pending_reply );
			return -ENOBUFS;
		}

		INIT_LIST_HEAD ( &pending_call->list );
		pending_call->data = call_buf;
		list_add ( &pending_call->list, &session->pending_call );
		rc = 0;
	} else {
		rc = xfer_deliver_iob ( &session->intf, call_buf );
		if ( ! rc  ) {
			free_iob ( call_buf );
			free ( pending_reply );
			return rc;
		}
	}

	INIT_LIST_HEAD ( &pending_reply->list );
	pending_reply->callback = cb;
	pending_reply->rpc_id = session->rpc_id;
	list_add ( &pending_reply->list, &session->pending_reply );
	free_iob ( io_buf );

	return rc;
}
