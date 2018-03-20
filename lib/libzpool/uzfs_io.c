/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

#include <sys/dmu_objset.h>
#include <sys/uzfs_zvol.h>
#include <uzfs_mtree.h>
#include <uzfs_io.h>

#define	GET_NEXT_CHUNK(chunk_io, offset, len, end)		\
	do {							\
		uzfs_io_chunk_list_t *node;			\
		node = list_remove_head(chunk_io);		\
		offset = node->offset;				\
		len = node->len;				\
		end = offset + len;				\
		umem_free(node, sizeof (*node));		\
	} while (0)

#define	CHECK_FIRST_ALIGNED_BLOCK(len_in_first_aligned_block,	\
    offset, len, blocksize)	\
	do {							\
		uint64_t r_offset;				\
		r_offset = P2ALIGN_TYPED(offset, blocksize,	\
		    uint64_t);					\
		len_in_first_aligned_block = (blocksize -	\
		    (offset - r_offset));			\
		if (len_in_first_aligned_block > len)		\
			len_in_first_aligned_block = len;	\
	} while (0)

#define	WRITE_METADATA(zv, metablk, mdata, tx)			\
	do {							\
		rl_t *mrl;					\
		mrl = zfs_range_lock(&zv->zv_mrange_lock,	\
		    metablk.m_offset, metablk.m_len,		\
		    RL_WRITER);					\
		dmu_write(zv->zv_objset, ZVOL_META_OBJ,		\
		    metablk.m_offset, metablk.m_len,		\
		    mdata, tx);					\
		zfs_range_unlock(mrl);				\
	} while (0)

/* Writes data 'buf' to dataset 'zv' at 'offset' for 'len' */
int
uzfs_write_data(zvol_state_t *zv, char *buf, uint64_t offset, uint64_t len,
    blk_metadata_t *metadata, boolean_t is_rebuild)
{
	uint64_t bytes = 0, sync;
	uint64_t volsize = zv->zv_volsize;
	uint64_t blocksize = zv->zv_volblocksize;
	uint64_t end = len + offset;
	uint64_t wrote = 0;
	objset_t *os = zv->zv_objset;
	rl_t *rl;
	uint64_t mlen = 0;
	int ret = 0, error;
	metaobj_blk_offset_t metablk;
	uint64_t metadatasize = zv->zv_volmetadatasize;
	uint64_t len_in_first_aligned_block = 0;
	uint32_t count = 0;
	list_t *chunk_io = NULL;
	uint64_t orig_offset = offset;
	char *mdata = NULL, *tmdata = NULL, *tmdataend = NULL;

	sync = (dmu_objset_syncprop(os) == ZFS_SYNC_ALWAYS) ? 1 : 0;
	ASSERT3P(zv->zv_volmetablocksize, !=, 0);

	if (metadata != NULL) {
		mlen = get_metadata_len(zv, offset, len);
		VERIFY((mlen % metadatasize) == 0);
		tmdata = mdata = kmem_alloc(mlen, KM_SLEEP);
		tmdataend = mdata + mlen;
		while (tmdata < tmdataend) {
			memcpy(tmdata, metadata, metadatasize);
			tmdata += metadatasize;
		}
	}

	CHECK_FIRST_ALIGNED_BLOCK(len_in_first_aligned_block, offset, len,
	    blocksize);

	rl = zfs_range_lock(&zv->zv_range_lock, offset, len, RL_WRITER);

	if (!is_rebuild && (zv->zv_status & ZVOL_STATUS_DEGRADED))
		uzfs_add_to_incoming_io_tree(zv, offset, len);

	if (zv->zv_rebuild_status & ZVOL_REBUILDING_IN_PROGRESS) {
		if (is_rebuild) {
			count = uzfs_search_incoming_io_tree(zv, offset,
			    len, (void **)&chunk_io);
			if (!count)
				goto exit_with_error;
chunk_io:
			GET_NEXT_CHUNK(chunk_io, offset, len, end);
			wrote = offset - orig_offset;
			CHECK_FIRST_ALIGNED_BLOCK(
			    len_in_first_aligned_block, offset, len,
			    blocksize);

			zv->rebuild_data.rebuild_bytes += len;
			count--;
		}
	} else {
		VERIFY(is_rebuild == 0);
	}

	while (offset < end && offset < volsize) {
		if (len_in_first_aligned_block != 0) {
			bytes = len_in_first_aligned_block;
			len_in_first_aligned_block = 0;
		}
		else
			bytes = (len < blocksize) ? len : blocksize;

		if (bytes > (volsize - offset))
			bytes = volsize - offset;

		dmu_tx_t *tx = dmu_tx_create(os);
		dmu_tx_hold_write(tx, ZVOL_OBJ, offset, bytes);

		if (metadata != NULL) {
			/* This assumes metavolblocksize same as volblocksize */
			get_zv_metaobj_block_details(&metablk, zv, offset,
			    bytes);

			dmu_tx_hold_write(tx, ZVOL_META_OBJ, metablk.m_offset,
			    metablk.m_len);
		}

		error = dmu_tx_assign(tx, TXG_WAIT);
		if (error) {
			dmu_tx_abort(tx);
			ret = UZFS_IO_TX_ASSIGN_FAIL;
			goto exit_with_error;
		}
		dmu_write(os, ZVOL_OBJ, offset, bytes, buf + wrote, tx);

		if (metadata)
			WRITE_METADATA(zv, metablk, mdata, tx);

		zvol_log_write(zv, tx, offset, bytes, sync, metadata);

		dmu_tx_commit(tx);

		offset += bytes;
		wrote += bytes;
		len -= bytes;
	}

exit_with_error:
	if ((zv->zv_rebuild_status & ZVOL_REBUILDING_IN_PROGRESS) &&
	    is_rebuild && count && !ret)
		goto chunk_io;

	if (chunk_io) {
		list_destroy(chunk_io);
		umem_free(chunk_io, sizeof (*chunk_io));
	}

	zfs_range_unlock(rl);

	if (sync)
		zil_commit(zv->zv_zilog, ZVOL_OBJ);

	if (mdata)
		kmem_free(mdata, mlen);

	return (ret);
}

/* Reads data from volume 'zv', and fills up memory at buf */
int
uzfs_read_data(zvol_state_t *zv, char *buf, uint64_t offset, uint64_t len,
    metadata_desc_t **md_head)
{
	int error = EINVAL;	// in case we aren't able to read a single block
	uint64_t blocksize = zv->zv_volblocksize;
	uint64_t bytes = 0;
	uint64_t volsize = zv->zv_volsize;
	uint64_t end = len + offset;
	uint64_t read = 0;
	objset_t *os = zv->zv_objset;
	rl_t *rl, *mrl;
	uint64_t r_offset;
	metaobj_blk_offset_t metablk;
	uint64_t len_in_first_aligned_block = 0;
	metadata_desc_t *md_ent = NULL, *new_md;
	blk_metadata_t *metadata;
	int nmetas;

	ASSERT3P(zv->zv_volmetadatasize, ==, sizeof (blk_metadata_t));

	/* init metadata in case caller wants to receive that info */
	if (md_head != NULL) {
		*md_head = NULL;
		ASSERT3P(zv->zv_volmetablocksize, !=, 0);
	}

	r_offset = P2ALIGN_TYPED(offset, blocksize, uint64_t);

	len_in_first_aligned_block = (blocksize - (offset - r_offset));

	if (len_in_first_aligned_block > len)
		len_in_first_aligned_block = len;

	rl = zfs_range_lock(&zv->zv_range_lock, offset, len, RL_READER);

	while ((offset < end) && (offset < volsize)) {
		if (len_in_first_aligned_block != 0) {
			bytes = len_in_first_aligned_block;
			len_in_first_aligned_block = 0;
		}
		else
			bytes = (len < blocksize) ? len : blocksize;

		if (bytes > (volsize - offset))
			bytes = volsize - offset;

		error = dmu_read(os, ZVOL_OBJ, offset, bytes, buf + read, 0);
		if (error != 0)
			goto exit;

		if (md_head != NULL) {
			get_zv_metaobj_block_details(&metablk, zv, offset,
			    bytes);

			metadata = kmem_alloc(metablk.m_len, KM_SLEEP);
			mrl = zfs_range_lock(&zv->zv_mrange_lock,
			    metablk.m_offset, metablk.m_len, RL_READER);
			error = dmu_read(os, ZVOL_META_OBJ, metablk.m_offset,
			    metablk.m_len, metadata, 0);
			zfs_range_unlock(mrl);
			if (error != 0) {
				kmem_free(metadata, metablk.m_len);
				goto exit;
			}

			nmetas = metablk.m_len / sizeof (*metadata);
			ASSERT3P(zv->zv_metavolblocksize * nmetas, >=, bytes);
			ASSERT3P(zv->zv_metavolblocksize * nmetas, <,
			    bytes + blocksize);
			for (int i = 0; i < nmetas; i++) {
				new_md = NULL;
				if (md_ent != NULL) {
					/*
					 * Join adjacent metadata with the same
					 * io number into one descriptor.
					 * Otherwise create a new one.
					 */
					if (md_ent->metadata.io_num ==
					    metadata[i].io_num) {
						md_ent->len +=
						    zv->zv_metavolblocksize;
					} else {
						new_md = kmem_alloc(
						    sizeof (metadata_desc_t),
						    KM_SLEEP);
						md_ent->next = new_md;
					}
				} else {
					new_md = kmem_alloc(
					    sizeof (metadata_desc_t),
					    KM_SLEEP);
					*md_head = new_md;
				}
				if (new_md != NULL) {
					new_md->next = NULL;
					new_md->len = zv->zv_metavolblocksize;
					memcpy(&new_md->metadata, &metadata[i],
					    sizeof (*metadata));
					md_ent = new_md;
				}
			}
			kmem_free(metadata, metablk.m_len);
		}
		offset += bytes;
		read += bytes;
		len -= bytes;
	}

exit:
	zfs_range_unlock(rl);

	if (md_head != NULL && error != 0) {
		md_ent = *md_head;
		while (md_ent != NULL) {
			new_md = md_ent->next;
			kmem_free(md_ent, sizeof (metadata_desc_t));
			md_ent = new_md;
		}
		*md_head = NULL;
	}
	return (error);
}

void
uzfs_flush_data(zvol_state_t *zv)
{
	zil_commit(zv->zv_zilog, ZVOL_OBJ);
}

/*
 * Caller is responsible for locking to ensure
 * synchronization across below four functions
 */
void
uzfs_zvol_set_status(zvol_state_t *zv, zvol_status_t status)
{
	zv->zv_status = status;
}

zvol_status_t
uzfs_zvol_get_status(zvol_state_t *zv)
{
	return (zv->zv_status);
}
void
uzfs_zvol_set_rebuild_status(zvol_state_t *zv, zvol_rebuild_status_t status)
{
	zv->zv_rebuild_status = status;
}

zvol_rebuild_status_t
uzfs_zvol_get_rebuild_status(zvol_state_t *zv)
{
	return (zv->zv_rebuild_status);
}