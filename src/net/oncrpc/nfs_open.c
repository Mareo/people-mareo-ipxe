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

/**
 * A NFS request
 *
 */
struct nfs_request {
	/** Reference counter */
	struct refcnt           refcnt;
	/** Data transfer interface */
	struct interface        xfer;

	struct oncrpc_cred_sys  auth_sys;

	struct oncrpc_session   pm_session;
	struct oncrpc_session   mount_session;
	struct oncrpc_session   nfs_session;

	char *                  mountpoint;
	char *                  filename;
	struct nfs_fh           file_fh;
	uint64_t                file_offset;

	/** URI being fetched */
	struct uri              *uri;
};

static int getport_nfs_cb ( struct oncrpc_session *session,
                            struct oncrpc_reply *reply);

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
	// free ( nfs );
}

/**
 * Mark NFS operation as complete
 *
 * @v nfs		NFS request
 * @v rc		Return status code
 */
static void nfs_done ( struct nfs_request *nfs, int rc ) {
	DBGC ( nfs, "NFS_OPEN %p completed (%s)\n", nfs, strerror ( rc ) );

	oncrpc_close_session ( &nfs->pm_session, rc );
	oncrpc_close_session ( &nfs->nfs_session, rc );
	oncrpc_close_session ( &nfs->mount_session, rc );
	intf_shutdown ( &nfs->xfer, rc );
}

static int umnt_cb ( struct oncrpc_session *session,
                     struct oncrpc_reply *reply __unused) {
	struct nfs_request      *nfs;

	nfs = container_of ( session, struct nfs_request, mount_session );
	DBGC ( nfs, "NFS_OPEN %p got UMNT reply\n", nfs );

	nfs_done ( nfs, 0 );

	return 0;
}

static int read_cb ( struct oncrpc_session *session,
                     struct oncrpc_reply *reply) {
	int                     rc;
	struct nfs_request      *nfs;
	struct nfs_read_reply   read_reply;

	nfs = container_of ( session, struct nfs_request, nfs_session );
	DBGC ( nfs, "NFS_OPEN %p got READ reply\n", nfs );

	rc = nfs_get_read_reply ( &read_reply, reply );
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

	if ( ! read_reply.eof )
	{
		rc = nfs_read ( session, &nfs->file_fh, nfs->file_offset,
		                NFS_RSIZE, read_cb );
		if ( rc != 0 )
			goto err;
	}

	if ( read_reply.eof )
	{
		rc = mount_umnt ( &nfs->mount_session, nfs->mountpoint,
		                  umnt_cb );
		if ( rc != 0  )
			goto err;
	}

	return 0;

err:
	nfs_done ( nfs, rc );
	return rc;
}

static int lookup_cb ( struct oncrpc_session *session,
                       struct oncrpc_reply *reply) {
	int                     rc;
	struct nfs_request      *nfs;
	struct nfs_lookup_reply lookup_reply;

	nfs = container_of ( session, struct nfs_request, nfs_session );
	DBGC ( nfs, "NFS_OPEN %p got LOOKUP reply\n", nfs );

	rc = nfs_get_lookup_reply ( &lookup_reply, reply );
	if ( rc != 0 )
		goto err;

	nfs->file_fh = lookup_reply.fh;
	rc = nfs_read ( session, &lookup_reply.fh, 0, NFS_RSIZE, read_cb );
	if ( rc != 0 )
		goto err;

	return 0;

err:
	nfs_done ( nfs, rc );
	return rc;
}

static int mnt_cb ( struct oncrpc_session *session,
                    struct oncrpc_reply *reply) {
	int                     rc;
	struct nfs_request      *nfs;
	struct mount_mnt_reply  mnt_reply;

	nfs = container_of ( session, struct nfs_request, mount_session );
	DBGC ( nfs, "NFS_OPEN %p got MNT reply\n", nfs );

	rc = mount_get_mnt_reply ( &mnt_reply, reply );
	if ( rc != 0 )
		goto err;

	rc = nfs_lookup ( &nfs->nfs_session, &mnt_reply.fh, nfs->filename,
                          lookup_cb );
	if ( rc != 0 )
		goto err;

	return 0;

err:
	nfs_done ( nfs, rc );
	return rc;
}

static int getport_mount_cb ( struct oncrpc_session *session,
                              struct oncrpc_reply *reply) {
	int                             rc;
	struct nfs_request              *nfs;
	struct portmap_getport_reply    getport_reply;

	nfs = container_of ( session, struct nfs_request, pm_session );
	DBGC ( nfs, "NFS_OPEN %p got GETPORT reply\n", nfs );

	if ( reply->accept_state != 0 )
	{
		rc = -ENOTSUP;
		goto err;
	}

	portmap_get_getport_reply ( &getport_reply, reply );
	rc = mount_init_session ( &nfs->mount_session, getport_reply.port,
	                          nfs->uri->host );
	if ( rc != 0 )
		goto err;

	rc = portmap_getport ( &nfs->pm_session, ONCRPC_NFS, NFS_VERS,
	                       PORTMAP_PROT_TCP, &getport_nfs_cb );
	if ( rc != 0 )
		goto err;

	rc = mount_mnt ( &nfs->mount_session, nfs->mountpoint, mnt_cb );
	if ( rc != 0 )
		goto err;

	return 0;

err:
	nfs_done ( nfs, rc );
	return rc;
}

static int getport_nfs_cb ( struct oncrpc_session *session,
                            struct oncrpc_reply *reply) {
	int                             rc;
	struct nfs_request              *nfs;
	struct portmap_getport_reply    getport_reply;

	nfs = container_of ( session, struct nfs_request, pm_session );
	DBGC ( nfs, "NFS_OPEN %p got GETPORT reply\n", nfs );

	if ( reply->accept_state != 0 )
	{
		rc = -ENOTSUP;
		goto err;
	}

	portmap_get_getport_reply ( &getport_reply, reply );
	rc = nfs_init_session ( &nfs->nfs_session, getport_reply.port,
	                        nfs->uri->host );
	if ( rc != 0 )
		goto err;

	nfs->nfs_session.credential = &nfs->auth_sys.credential;

	return 0;

err:
	nfs_done ( nfs, rc );
	return rc;
}

static struct interface_operation nfs_xfer_operations[] = {
	INTF_OP ( intf_close, struct nfs_request *, nfs_done ),
};

/** NFS data transfer interface descriptor */
static struct interface_descriptor nfs_xfer_desc =
	INTF_DESC ( struct nfs_request, xfer, nfs_xfer_operations );

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
	nfs->uri = uri_get ( uri );

	portmap_init_session ( &nfs->pm_session, uri_port ( uri, 0 ),
	                       uri->host );

	nfs->filename   = basename ( nfs->mountpoint );
	nfs->mountpoint = dirname ( nfs->mountpoint );

	portmap_getport ( &nfs->pm_session, ONCRPC_MOUNT, MOUNT_VERS,
	                  PORTMAP_PROT_TCP, &getport_mount_cb );

	/* Attach to parent interface, mortalise self, and return */
	intf_plug_plug ( &nfs->xfer, xfer );
	ref_put ( &nfs->refcnt );

	return 0;

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
