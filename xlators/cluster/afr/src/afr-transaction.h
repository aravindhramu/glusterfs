/*
  Copyright (c) 2008-2012 Red Hat, Inc. <http://www.redhat.com>
  This file is part of GlusterFS.

  This file is licensed to you under your choice of the GNU Lesser
  General Public License, version 3 or any later version (LGPLv3 or
  later), or the GNU General Public License, version 2 (GPLv2), in all
  cases as published by the Free Software Foundation.
*/

#ifndef __TRANSACTION_H__
#define __TRANSACTION_H__

void
afr_transaction_fop_failed (call_frame_t *frame, xlator_t *this,
			    int child_index);

int
afr_lock_server_count (afr_private_t *priv, afr_transaction_type type);

int32_t
afr_transaction (call_frame_t *frame, xlator_t *this, afr_transaction_type type);

afr_fd_ctx_t *
afr_fd_ctx_get (fd_t *fd, xlator_t *this);
int
afr_set_pending_dict (afr_private_t *priv, dict_t *xattr, int32_t **pending);
#endif /* __TRANSACTION_H__ */
