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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <libgen.h>
#include <byteswap.h>
#include <ipxe/time.h>
#include <ipxe/socket.h>
#include <ipxe/tcpip.h>
#include <ipxe/in.h>
#include <ipxe/iobuf.h>
#include <ipxe/xfer.h>
#include <ipxe/open.h>
#include <ipxe/uri.h>
#include <ipxe/features.h>
#include <ipxe/nfs.h>
#include <ipxe/nfs_open.h>
#include <ipxe/oncrpc.h>
#include <ipxe/oncrpc_iob.h>
#include <ipxe/portmap.h>
#include <ipxe/mount.h>
#include <ipxe/settings.h>

/** @file
 *
 * Network File System protocol
 *
 */

FEATURE ( FEATURE_PROTOCOL, "NFS", DHCP_EB_FEATURE_NFS, 1 );

#define NFS_RSIZE 1300

enum nfs_pm_state {
	NFS_PORTMAP_NONE = 0,
	NFS_PORTMAP_MOUNTPORT,
	NFS_PORTMAP_NFSPORT,
	MFS_PORTMAP_CLOSED,
};

enum nfs_mount_state {
	NFS_MOUNT_NONE = 0,
	NFS_MOUNT_MNT,
	NFS_MOUNT_UMNT,
	NFS_MOUNT_CLOSED,
};

enum nfs_state {
	NFS_NONE = 0,
	NFS_LOOKUP,
	NFS_LOOKUP_SENT,
	NFS_READ,
	NFS_READ_SENT,
	NFS_CLOSED,
};

/**
 * A NFS request
 *
 */
struct nfs_request {
	/** Reference counter */
	struct refcnt           refcnt;
	/** Data transfer interface */
	struct interface        xfer;

	struct interface        pm_intf;
	struct interface        mount_intf;
	struct interface        nfs_intf;

	enum nfs_pm_state       pm_state;
	enum nfs_mount_state    mount_state;
	enum nfs_state          nfs_state;

	struct oncrpc_session   pm_session;
	struct oncrpc_session   mount_session;
	struct oncrpc_session   nfs_session;

	struct oncrpc_cred_sys  auth_sys;

	char *                  mountpoint;
	char *                  filename;
	struct nfs_fh           current_fh;
	uint64_t                file_offset;

	/** URI being fetched */
	struct uri              *uri;
};

static void nfs_step ( struct nfs_request *nfs );

/**
 * Free NFS request
 *
 * @v refcnt		Reference counter
 */
static void nfs_free ( struct refcnt *refcnt ) {
	struct nfs_request      *nfs;

	nfs = container_of ( refcnt, struct nfs_request, refcnt );
	DBGC ( nfs, "NFS_OPEN %p freed\n", nfs );

	free ( nfs->mountpoint );
	free ( nfs->auth_sys.hostname );
	uri_put ( nfs->uri );

	/* Causes segfault for a reason yet to be discovered. */
	free ( nfs );
}

/**
 * Mark NFS operation as complete
 *
 * @v nfs		NFS request
 * @v rc		Return status code
 */
static void nfs_done ( struct nfs_request *nfs, int rc ) {
	if ( rc == 0 && nfs->nfs_state != NFS_CLOSED )
		rc = -ECONNRESET;

	DBGC ( nfs, "NFS_OPEN %p completed (%s)\n", nfs, strerror ( rc ) );

	intf_shutdown ( &nfs->xfer, rc );
	intf_shutdown ( &nfs->pm_intf, rc );
	intf_shutdown ( &nfs->mount_intf, rc );
	intf_shutdown ( &nfs->nfs_intf, rc );
}

static int nfs_connect ( struct interface *intf, uint16_t port,
                         const char *hostname ) {
	struct sockaddr_tcpip   peer;
	struct sockaddr_tcpip   local;

	if ( ! intf || ! hostname || ! port )
		return -EINVAL;

	memset ( &peer, 0, sizeof ( peer ) );
	memset ( &peer, 0, sizeof ( local ) );
	peer.st_port = htons ( port );

	/* Use a local port < 1024 to avoid using the 'insecure' option in
	 * /etc/exports file. */
	local.st_port = htons ( 1 + ( rand() % 1023 ) );

	return xfer_open_named_socket ( intf, SOCK_STREAM,
	                                ( struct sockaddr * ) &peer, hostname,
                                        ( struct sockaddr * ) &local );
}

static void nfs_pm_step ( struct nfs_request *nfs ) {
	int     rc;

	if ( ! xfer_window ( &nfs->pm_intf ) )
		return;

	if ( nfs->pm_state == NFS_PORTMAP_NONE ) {
		DBGC ( nfs, "NFS_OPEN %p GETPORT call (mount)\n", nfs );

		rc = portmap_getport ( &nfs->pm_intf, &nfs->pm_session,
		                       ONCRPC_MOUNT, MOUNT_VERS,
		                       PORTMAP_PROTO_TCP );
		if ( rc == -EAGAIN )
			return;

		if ( rc != 0 )
			goto err;

		nfs->pm_state++;
		return;
	}

	if ( nfs->pm_state == NFS_PORTMAP_NFSPORT ) {
		DBGC ( nfs, "NFS_OPEN %p GETPORT call (nfs)\n", nfs );

		rc = portmap_getport ( &nfs->pm_intf, &nfs->pm_session,
		                       ONCRPC_NFS, NFS_VERS,
		                       PORTMAP_PROTO_TCP );
		if ( rc == -EAGAIN )
			return;

		if ( rc != 0 )
			goto err;

		return;
	}

	return;
err:
	nfs_done ( nfs, rc );
}

static int nfs_pm_deliver ( struct nfs_request *nfs,
                            struct io_buffer *io_buf,
                            struct xfer_metadata *meta __unused ) {
	int                             rc;
	struct oncrpc_reply             reply;
	struct portmap_getport_reply    getport_reply;

	oncrpc_get_reply ( &nfs->pm_session, &reply, io_buf );
	if ( reply.accept_state != 0 )
	{
		rc = -EPROTO;
		goto err;
	}

	if ( nfs->pm_state == NFS_PORTMAP_MOUNTPORT ) {
		DBGC ( nfs, "NFS_OPEN %p got GETPORT reply (mount)\n", nfs );

		rc = portmap_get_getport_reply ( &getport_reply, &reply );
		if ( rc != 0 )
			goto err;

		rc = nfs_connect ( &nfs->mount_intf, getport_reply.port,
	                           nfs->uri->host );
		if ( rc != 0 )
			goto err;

		nfs->pm_state++;
		nfs_pm_step ( nfs );

		goto done;
	}

	if ( nfs->pm_state == NFS_PORTMAP_NFSPORT ) {
		DBGC ( nfs, "NFS_OPEN %p got GETPORT reply (nfs)\n", nfs );

		rc = portmap_get_getport_reply ( &getport_reply, &reply );
		if ( rc != 0 )
			goto err;

		rc = nfs_connect ( &nfs->nfs_intf, getport_reply.port,
	                           nfs->uri->host );
		if ( rc != 0 )
			goto err;

		intf_shutdown ( &nfs->pm_intf, 0 );
		nfs->pm_state++;

		goto done;
	}

	rc = -EPROTO;
err:
	nfs_done ( nfs, rc );
done:
	free_iob ( io_buf );
	return 0;
}

static void nfs_mount_step ( struct nfs_request *nfs ) {
	int     rc;

	if ( ! xfer_window ( &nfs->mount_intf ) )
		return;

	if ( nfs->mount_state == NFS_MOUNT_NONE ) {
		DBGC ( nfs, "NFS_OPEN %p MNT call\n", nfs );

		rc = mount_mnt ( &nfs->mount_intf, &nfs->mount_session,
		                 nfs->mountpoint );
		if ( rc == -EAGAIN )
			return;
		if ( rc != 0 )
			goto err;

		nfs->mount_state++;
		return;
	}

	if ( nfs->mount_state == NFS_MOUNT_UMNT ) {
		DBGC ( nfs, "NFS_OPEN %p UMNT call\n", nfs );

		rc = mount_umnt ( &nfs->mount_intf, &nfs->mount_session,
		                  nfs->mountpoint );
		if ( rc != 0 )
			goto err;
	}

	return;
err:
	nfs_done ( nfs, rc );
}

static int nfs_mount_deliver ( struct nfs_request *nfs,
                               struct io_buffer *io_buf,
                               struct xfer_metadata *meta __unused ) {
	int                     rc;
	struct oncrpc_reply     reply;
	struct mount_mnt_reply  mnt_reply;

	oncrpc_get_reply ( &nfs->mount_session, &reply, io_buf );
	if ( reply.accept_state != 0 )
	{
		rc = -EPROTO;
		goto err;
	}

	if ( nfs->mount_state == NFS_MOUNT_MNT ) {
		DBGC ( nfs, "NFS_OPEN %p got MNT reply\n", nfs );
		rc = mount_get_mnt_reply ( &mnt_reply, &reply );
		if ( rc != 0 )
			goto err;

		nfs->current_fh = mnt_reply.fh;
		nfs->nfs_state = NFS_LOOKUP;
		nfs_step ( nfs );

		goto done;
	}

	if ( nfs->mount_state == NFS_MOUNT_UMNT ) {
		DBGC ( nfs, "NFS_OPEN %p got UMNT reply\n", nfs );
		nfs_done ( nfs, 0 );

		goto done;
	}

	rc = -EPROTO;
err:
	nfs_done ( nfs, rc );
done:
	free_iob ( io_buf );
	return 0;
}

static void nfs_step ( struct nfs_request *nfs ) {
	int     rc;

	if ( ! xfer_window ( &nfs->nfs_intf ) )
		return;

	if ( nfs->nfs_state == NFS_LOOKUP ) {
		DBGC ( nfs, "NFS_OPEN %p LOOKUP call\n", nfs );

		rc = nfs_lookup ( &nfs->nfs_intf, &nfs->nfs_session,
		                  &nfs->current_fh, nfs->filename );
		if ( rc == -EAGAIN )
			return;
		if ( rc != 0 )
			goto err;

		nfs->nfs_state++;
		return;
	}

	if ( nfs->nfs_state == NFS_READ ) {
		DBGC ( nfs, "NFS_OPEN %p READ call\n", nfs );

		rc = nfs_read ( &nfs->nfs_intf, &nfs->nfs_session,
		                &nfs->current_fh, nfs->file_offset,
		                NFS_RSIZE );
		if ( rc == -EAGAIN )
			return;
		if ( rc != 0 )
			goto err;

		nfs->nfs_state++;
		return;
	}

	return;
err:
	nfs_done ( nfs, rc );
}

static int nfs_deliver ( struct nfs_request *nfs,
                         struct io_buffer *io_buf,
                         struct xfer_metadata *meta __unused ) {
	int                     rc;
	struct oncrpc_reply     reply;
	struct nfs_read_reply   read_reply;
	struct nfs_lookup_reply lookup_reply;

	oncrpc_get_reply ( &nfs->nfs_session, &reply, io_buf );
	if ( reply.accept_state != 0 )
	{
		rc = -EPROTO;
		goto err;
	}

	if ( nfs->nfs_state == NFS_LOOKUP_SENT ) {
		DBGC ( nfs, "NFS_OPEN %p got LOOKUP reply\n", nfs );

		rc = nfs_get_lookup_reply ( &lookup_reply, &reply );
		if ( rc != 0 )
			goto err;

		nfs->current_fh = lookup_reply.fh;
		nfs->nfs_state++;
		nfs_step ( nfs );

		goto done;
	}

	if ( nfs->nfs_state == NFS_READ_SENT ) {
		DBGC ( nfs, "NFS_OPEN %p got READ reply\n", nfs );

		rc = nfs_get_read_reply ( &read_reply, &reply );
		if ( rc != 0 )
			goto err;

		if ( nfs->file_offset == 0 )
		{
			xfer_seek ( &nfs->xfer, read_reply.filesize );
			xfer_seek ( &nfs->xfer, 0 );
		}

		nfs->file_offset += read_reply.count;

		rc = xfer_deliver_raw ( &nfs->xfer, read_reply.data,
		                        read_reply.data_len );
		if ( rc != 0 )
			goto err;

		if ( ! read_reply.eof ) {
			nfs->nfs_state--;
			nfs_step ( nfs );
		} else {
			intf_shutdown ( &nfs->nfs_intf, 0 );
			nfs->nfs_state++;
			nfs->mount_state++;
			nfs_mount_step ( nfs );
		}

		goto done;
	}

	rc = -EPROTO;
err:
	nfs_done ( nfs, rc );
done:
	free_iob ( io_buf );
	return 0;
}

/*****************************************************************************
 * Interfaces
 *
 */

static struct interface_operation nfs_xfer_operations[] = {
	INTF_OP ( intf_close, struct nfs_request *, nfs_done ),
};

/** NFS data transfer interface descriptor */
static struct interface_descriptor nfs_xfer_desc =
	INTF_DESC ( struct nfs_request, xfer, nfs_xfer_operations );

static struct interface_operation nfs_pm_operations[] = {
	INTF_OP ( intf_close, struct nfs_request *, nfs_done ),
	INTF_OP ( xfer_deliver, struct nfs_request *, nfs_pm_deliver ),
	INTF_OP ( xfer_window_changed, struct nfs_request *, nfs_pm_step ),
};

static struct interface_descriptor nfs_pm_desc =
	INTF_DESC ( struct nfs_request, pm_intf, nfs_pm_operations );

static struct interface_operation nfs_mount_operations[] = {
	INTF_OP ( intf_close, struct nfs_request *, nfs_done ),
	INTF_OP ( xfer_deliver, struct nfs_request *, nfs_mount_deliver ),
	INTF_OP ( xfer_window_changed, struct nfs_request *, nfs_mount_step ),
};

static struct interface_descriptor nfs_mount_desc =
	INTF_DESC ( struct nfs_request, mount_intf, nfs_mount_operations );

static struct interface_operation nfs_operations[] = {
	INTF_OP ( intf_close, struct nfs_request *, nfs_done ),
	INTF_OP ( xfer_deliver, struct nfs_request *, nfs_deliver ),
	INTF_OP ( xfer_window_changed, struct nfs_request *, nfs_step ),
};

static struct interface_descriptor nfs_desc =
	INTF_DESC_PASSTHRU ( struct nfs_request, nfs_intf, nfs_operations,
	                     xfer );

/*****************************************************************************
 *
 * URI opener
 *
 */


/**
 * Initiate a NFS connection
 *
 * @v xfer		Data transfer interface
 * @v uri		Uniform Resource Identifier
 * @ret rc		Return status code
 */
static int nfs_open ( struct interface *xfer, struct uri *uri ) {
	int                     rc;
	char                    *hostname;
	struct nfs_request      *nfs;

	/* Sanity checks */
	if ( ! uri->path || ! uri->host )
		return -EINVAL;

	nfs = zalloc ( sizeof ( *nfs ) );
	if ( ! nfs )
		return -ENOMEM;

	fetch_string_setting_copy ( NULL, &hostname_setting,
	                            &hostname );
	if ( ! hostname )
		hostname = strdup ( "iPXE" );

	if ( ! hostname )
	{
		rc = -ENOMEM;
		goto err_hostname;
	}

	oncrpc_init_cred_sys ( &nfs->auth_sys, 0, 0, hostname );

	nfs->mountpoint = strdup ( uri->path );
	if ( ! nfs->mountpoint )
	{
		rc = -ENOMEM;
		goto err_mountpoint;
	}

	ref_init ( &nfs->refcnt, nfs_free );
	intf_init ( &nfs->xfer, &nfs_xfer_desc, &nfs->refcnt );
	intf_init ( &nfs->pm_intf, &nfs_pm_desc, &nfs->refcnt );
	intf_init ( &nfs->mount_intf, &nfs_mount_desc, &nfs->refcnt );
	intf_init ( &nfs->nfs_intf, &nfs_desc, &nfs->refcnt );
	nfs->uri = uri_get ( uri );

	portmap_init_session ( &nfs->pm_session, &nfs->auth_sys.credential );
	mount_init_session ( &nfs->mount_session, &nfs->auth_sys.credential );
	nfs_init_session ( &nfs->nfs_session, &nfs->auth_sys.credential );

	nfs->filename   = basename ( nfs->mountpoint );
	nfs->mountpoint = dirname ( nfs->mountpoint );

	rc = nfs_connect ( &nfs->pm_intf, uri_port ( uri, PORTMAP_PORT ),
	                   uri->host );
	if ( rc != 0 )
		goto err_connect;

	/* Attach to parent interface, mortalise self, and return */
	intf_plug_plug ( &nfs->xfer, xfer );
	ref_put ( &nfs->refcnt );

	return 0;

err_connect:
err_mountpoint:
	free ( hostname );
err_hostname:
	free ( nfs );
	return rc;
}

/** NFS URI opener */
struct uri_opener nfs_uri_opener __uri_opener = {
	.scheme	= "nfs",
	.open	= nfs_open,
};
