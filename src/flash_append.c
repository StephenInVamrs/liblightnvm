/*
 * flash_append - user-space append-only file system for flash memories
 *
 * Copyright (C) 2015 Javier Gonzalez <javier@cnexlabs.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 *
 */
#define _GNU_SOURCE

#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <liblightnvm.h>

#include "../util/likely.h"
#include "flash_beam.h"

static struct atomic_cnt beam_guid = {
	.cnt = 0,
};

static struct atomic_cnt fd_guid = {
	.cnt = 0,
};

static struct lnvm_device *devt = NULL;
static struct lnvm_target *tgtt = NULL;
static struct lnvm_target_map *tgtmt = NULL;
static struct beam *beamt = NULL;

static void beam_buf_free(struct w_buffer *buf)
{
	if (buf->buf)
		free(buf->buf);

	buf->buf = NULL;
	buf->mem = NULL;
	buf->sync = NULL;
}

static void beam_put_blocks(struct beam *beam)
{
	int i;

	for (i = 0; i < beam->nvblocks; i++) {
		put_block(beam->tgt->tgt_id, &beam->vblocks[i]);
	}
}

static void beam_free(struct beam *beam)
{
	beam_buf_free(&beam->w_buffer);
	beam_put_blocks(beam);
	free(beam);
}

/* XXX: All block functions assume that block allocation is thread safe */
/* TODO: Allocate blocks dynamically */
static int switch_block(struct beam **beam)
{
	size_t buf_size;
	int dev_page_size = (*beam)->tgt->tgt->dev->dev_page_size;
	int ret;

	/* Write buffer for small writes */
	buf_size = get_npages_block(&(*beam)->vblocks[(*beam)->nvblocks] - 1) *
								dev_page_size;
	if (buf_size != (*beam)->w_buffer.buf_limit) {
		LNVM_DEBUG("Allocating new write buffer of size %lu\n",
								buf_size);
		free((*beam)->w_buffer.buf);
		ret = posix_memalign(&(*beam)->w_buffer.buf, dev_page_size,
								buf_size);
		if (ret) {
			LNVM_DEBUG("Cannot allocate memory "
					" (%lu bytes - align. %d)\n",
				buf_size, dev_page_size);
			return ret;
		}
		(*beam)->w_buffer.buf_limit = buf_size;
	}

	(*beam)->w_buffer.mem = (*beam)->w_buffer.buf;
	(*beam)->w_buffer.sync = (*beam)->w_buffer.buf;
	(*beam)->w_buffer.cursize = 0;
	(*beam)->w_buffer.cursync = 0;

	(*beam)->current_w_vblock = &(*beam)->vblocks[(*beam)->nvblocks - 1];

	LNVM_DEBUG("Block switched. Beam: %d, id: %llu. Total blocks: %d\n",
			(*beam)->gid,
			(*beam)->current_w_vblock->id,
			(*beam)->nvblocks);

	return 0;
}

static int preallocate_block(struct beam *beam)
{
	VBLOCK *vblock = &beam->vblocks[beam->nvblocks];
	int ret = 0;

	ret = get_block(beam->tgt->tgt_id, beam->lun, vblock);
	if (ret) {
		LNVM_DEBUG("Could not allocate a new block for beam %d\n",
				beam->gid);
		goto out;
	}

	LNVM_DEBUG("Block preallocated (pos:%d). Beam: %d, id: %llu, "
			" bppa: %llu\n",
			beam->nvblocks,
			beam->gid,
			beam->vblocks[beam->nvblocks].id,
			beam->vblocks[beam->nvblocks].bppa);

	beam->nvblocks++;
out:
	return ret;
}

static int allocate_block(struct beam *beam)
{
	int ret;

	// TODO: Retry if preallocation fails - when saving metadata
	ret = preallocate_block(beam);
	if (ret)
		goto out;
	ret = switch_block(&beam);

out:
	return ret;
}

static int beam_init(struct beam *beam, int lun, int tgt)
{
	atomic_assign_inc(&beam_guid, &beam->gid);
	beam->lun = lun;
	beam->nvblocks = 0;
	beam->bytes = 0;

	HASH_FIND_INT(tgtmt, &tgt, beam->tgt);
	if (!beam->tgt) {
		LNVM_DEBUG("Initializing beam on uninitialized target\n");
		return -EINVAL;
	}

	beam->w_buffer.buf_limit = 0;
	beam->w_buffer.buf = NULL;

	return allocate_block(beam);
}

static int beam_sync(struct beam *beam, int flags)
{
	size_t sync_len = beam->w_buffer.cursize - beam->w_buffer.cursync;
	size_t synced_bytes;
	int write_page_size = beam->tgt->tgt->dev->write_page_size;
	size_t disaligned_data = sync_len % write_page_size;
	size_t ppa_off = calculate_ppa_off(beam->w_buffer.cursync, write_page_size);
	int dev_page_size = beam->tgt->tgt->dev->dev_page_size;
	int max_pages_write = beam->tgt->tgt->dev->max_io_size;
	int npages = sync_len / dev_page_size;
	int synced_pages;

	if (((flags & OPTIONAL_SYNC) && (sync_len < write_page_size)) ||
		(sync_len == 0))
		return 0;

	if (flags & FORCE_SYNC) {
		if (beam->w_buffer.cursync + sync_len ==
						beam->w_buffer.buf_limit) {
			/* TODO: Metadata */
		}

		if (disaligned_data > 0) {
			/* Add padding to current page */
			int padding = write_page_size - disaligned_data;
			memset(beam->w_buffer.mem, '\0', padding);
			beam->w_buffer.cursize += padding;
			beam->w_buffer.mem += padding;

			npages++;
		}
	} else {
		sync_len -= disaligned_data;
	}

	/* write data to media */
	synced_pages = flash_write(beam->tgt->tgt_id, beam->current_w_vblock,
					beam->w_buffer.sync, ppa_off, npages,
					max_pages_write, write_page_size);
	if (synced_pages != npages) {
		LNVM_DEBUG("Error syncing data\n");
		return -1;
	}

	/* We might need to take a lock here */
	synced_bytes = synced_pages * dev_page_size;
	beam->bytes += synced_bytes;
	beam->w_buffer.cursync += synced_bytes;
	beam->w_buffer.sync += synced_bytes;

	LNVM_DEBUG("Synced bytes: %lu (%d pages)\n", synced_bytes, synced_pages);

	/* TODO: Access times */
	return 0;
}

static void device_clean(struct lnvm_device *dev)
{
	HASH_DEL(devt, dev);
	free(dev);
}

static void target_clean(struct lnvm_target *tgt)
{
	struct lnvm_device *dev = tgt->dev;

	HASH_DEL(tgtt, tgt);
	if (atomic_dec_and_test(&dev->ref_cnt))
		device_clean(dev);
	free(tgt);
}

static void target_ins_clean(int tgt)
{
	struct lnvm_target *t;
	struct lnvm_target_map *tm;

	HASH_FIND_INT(tgtmt, &tgt, tm);
	if (tm) {
		t = tm->tgt;
		if (atomic_dec_and_test(&t->ref_cnt))
			target_clean(t);
		HASH_DEL(tgtmt, tm);
		free(tm);
	}
}

static inline int get_tgt_info(char *tgt, char *dev,
						struct lnvm_target *target)
{
	struct nvm_ioctl_tgt_info tgt_info;
	int ret = 0;

	/* Initialize ioctl values */
	tgt_info.version[0] = 0;
	tgt_info.version[1] = 0;
	tgt_info.version[2] = 0;
	tgt_info.reserved = 0;
	memset(tgt_info.target.dev, 0, DISK_NAME_LEN);
	memset(tgt_info.target.tgttype, 0, NVM_TTYPE_MAX);

	strncpy(tgt_info.target.tgtname, tgt, DISK_NAME_LEN);
	ret = nvm_get_target_info(&tgt_info);
	if (ret)
		goto out;

	target->version[0] = tgt_info.version[0];
	target->version[1] = tgt_info.version[1];
	target->version[2] = tgt_info.version[2];
	target->reserved = tgt_info.reserved;
	strncpy(target->tgtname, tgt_info.target.tgtname, DISK_NAME_LEN);
	strncpy(target->tgttype, tgt_info.target.tgttype, NVM_TTYPE_NAME_MAX);
	strncpy(dev, tgt_info.target.dev, DISK_NAME_LEN);

	LNVM_DEBUG("Target cached: %s(t:%s, d:%s)\t(%u,%u,%u)\n",
			target->tgtname, target->tgttype, dev,
			target->version[0], target->version[1],
			target->version[2]);
out:
	return ret;
}

static inline int get_dev_info(char *dev, struct lnvm_device *device)
{
	struct nvm_ioctl_dev_info dev_info;
	int ret = 0;
	int i;

	/* Initialize ioctl values */
	memset(dev_info.bmname, 0, NVM_TTYPE_MAX);
	for (i = 0; i < 3; i++)
		dev_info.bmversion[i] = 0;
	for (i = 0; i < 8; i++)
		dev_info.reserved[i] = 0;
	dev_info.prop.page_size = 0;
	dev_info.prop.max_io_size = 0;

	dev_info.flags = 0;

	strncpy(dev_info.dev, dev, DISK_NAME_LEN);
	ret = nvm_get_device_info(&dev_info);
	if (ret)
		goto out;

	strncpy(device->dev, dev_info.dev, DISK_NAME_LEN);
	device->dev_page_size = dev_info.prop.page_size;
	device->write_page_size =
			dev_info.prop.page_size * dev_info.prop.nr_planes;
	device->nr_luns = dev_info.prop.nr_luns;
	device->max_io_size = dev_info.prop.max_io_size;

	LNVM_DEBUG("Device cached: %s(page_size:%u-%u, max_io_size:%u)\n",
			device->dev, device->dev_page_size,
			device->write_page_size, device->max_io_size);
out:
	return ret;
}

/*TODO: Add which application opens tgt? */
static int nvm_target_open(const char *tgt, int flags)
{
	struct lnvm_device *dev_entry;
	struct lnvm_target *tgt_entry;
	struct lnvm_target_map *tgtm_entry;
	char tgt_loc[NVM_TGT_NAME_MAX] = "/dev/";
	char tgt_eol[DISK_NAME_LEN];
	char dev[DISK_NAME_LEN];
	int tgt_id;
	int new_dev = 0, new_tgt = 0;
	int ret = 0;

	strcpy(tgt_eol, tgt);
	tgt_eol[DISK_NAME_LEN - 1] = '\0';

	HASH_FIND_STR(tgtt, tgt_eol, tgt_entry);
	if (!tgt_entry) {
		tgt_entry = malloc(sizeof(struct lnvm_target));
		if (!tgt_entry)
			return -ENOMEM;

		ret = get_tgt_info(tgt_eol, dev, tgt_entry);
		if (ret) {
			LNVM_DEBUG("Could not get target information\n");
			free(tgt_entry);
			return ret;
		}

		dev[DISK_NAME_LEN - 1] = '\0';
		HASH_FIND_STR(devt, dev, dev_entry);
		if (!dev_entry) {
			dev_entry = malloc(sizeof(struct lnvm_device));
			if (!dev_entry) {
				free(tgt_entry);
				return -ENOMEM;
			}

			ret = get_dev_info(dev, dev_entry);
			if (ret) {
				LNVM_DEBUG("Could not get device information\n");
				free(dev_entry);
				free(tgt_entry);
				return ret;
			}

			strncpy(dev_entry->dev, dev, DISK_NAME_LEN);
			HASH_ADD_STR(devt, dev, dev_entry);

			atomic_init(&dev_entry->ref_cnt);
			atomic_set(&dev_entry->ref_cnt, 0);
			new_dev = 1;
		}

		tgt_entry->dev = dev_entry;
		strncpy(tgt_entry->tgtname, tgt_eol, DISK_NAME_LEN);
		HASH_ADD_STR(tgtt, tgtname, tgt_entry);
		atomic_inc(&dev_entry->ref_cnt);

		atomic_init(&tgt_entry->ref_cnt);
		atomic_set(&tgt_entry->ref_cnt, 0);
		new_tgt = 1;
	}

	strcat(tgt_loc, tgt);
	tgt_id = open(tgt_loc, O_RDWR | O_DIRECT);
	if (tgt_id < 0) {
		LNVM_DEBUG("Failed to open LightNVM target %s (%d)\n",
				tgt_loc, tgt_id);
		goto error;
	}

	tgtm_entry = malloc(sizeof(struct lnvm_target_map));
	if (!tgtm_entry) {
		ret = -ENOMEM;
		goto error;
	}

	tgtm_entry->tgt_id = tgt_id;
	tgtm_entry->tgt = tgt_entry;
	HASH_ADD_INT(tgtmt, tgt_id, tgtm_entry);
	atomic_inc(&tgt_entry->ref_cnt);

	return tgt_id;

error:
	if (new_dev)
		free(dev_entry);
	if (new_tgt)
		free(tgt_entry);

	return ret;
}

static void nvm_target_close(int tgt)
{
	close(tgt);
	target_ins_clean(tgt);
}

/*
 * liblightnvm flash API
 */

int nvm_beam_create(const char *tgt, int lun, int flags)
{
	struct beam *beam;
	int tgt_fd;
	int ret;

	tgt_fd = nvm_target_open(tgt, flags);
	if (tgt_fd < 0)
		return tgt_fd;

	beam = malloc(sizeof(struct beam));
	if (!beam)
		return -ENOMEM;

	ret = beam_init(beam, lun, tgt_fd);
	if (ret)
		goto error;

	HASH_ADD_INT(beamt, gid, beam);
	LNVM_DEBUG("Created flash beam (p:%p, id:%d). Target: %d\n",
			beam, beam->gid, tgt_fd);

	return beam->gid;

error:
	free(beam);
	return ret;
}

void nvm_beam_destroy(int beam, int flags)
{
	struct beam *b;
	int tm;

	LNVM_DEBUG("Deleting beam with id %d\n", beam);

	HASH_FIND_INT(beamt, &beam, b);
	HASH_DEL(beamt, b);

	tm = b->tgt->tgt_id;
	nvm_target_close(tm);

	beam_free(b);
}


/* TODO: Implement a pool of available bloks to support double buffering */
/*
 * TODO: Flush pages in a different thread as write buffer gets filled up,
 * instead of flushing the whole block at a time
 */
ssize_t nvm_beam_append(int beam, const void *buf, size_t count)
{
	struct beam *b;
	size_t offset = 0;
	size_t left = count;
	int ret;

	HASH_FIND_INT(beamt, &beam, b);
	if (!b) {
		LNVM_DEBUG("Beam %d does not exits\n", beam);
		return -EINVAL;
	}

	LNVM_DEBUG("Append %lu bytes to beam %d (p:%p, b:%d). Csize:%lu, "
			"Csync:%lu, BL:%lu\n",
				count,
				b->gid, b,
				beam,
				b->w_buffer.cursize,
				b->w_buffer.cursync,
				b->w_buffer.buf_limit);

	while (b->w_buffer.cursize + left > b->w_buffer.buf_limit) {
		LNVM_DEBUG("Block overflow. Csize:%lu, Csync:%lu, BL:%lu "
				"left:%lu\n",
				b->w_buffer.cursize,
				b->w_buffer.cursync,
				b->w_buffer.buf_limit,
				left);
		size_t fits_buf = b->w_buffer.buf_limit - b->w_buffer.cursize;
		ret = preallocate_block(b);
		if (ret)
			return ret;
		memcpy(b->w_buffer.mem, buf, fits_buf);
		b->w_buffer.mem += fits_buf;
		b->w_buffer.cursize += fits_buf;
		if (beam_sync(b, FORCE_SYNC)) {
			LNVM_DEBUG("Cannot force sync for beam %d\n", b->gid);
			return -ENOSPC;
		}
		switch_block(&b);

		left -= fits_buf;
		offset += fits_buf;
	}

	memcpy(b->w_buffer.mem, buf + offset, left);
	b->w_buffer.mem += left;
	b->w_buffer.cursize += left;

	return count;
}

int nvm_beam_sync(int beam, int flags)
{
	struct beam *b;

	HASH_FIND_INT(beamt, &beam, b);
	if (!b) {
		LNVM_DEBUG("Beam %d does not exits\n", beam);
		return -EINVAL;
	}

	// TODO: Expose flags to application
	return beam_sync(b, FORCE_SYNC);
}

/*
 * Flag for aligned buffer
 */
ssize_t nvm_beam_read(int beam, void *buf, size_t count, off_t offset, int flags)
{
	struct beam *b;
	VBLOCK *current_r_vblock;
	size_t block_off, ppa_off, page_off;
	size_t ppa_count;
	size_t nppas;
	size_t left_bytes = count;
	/* TODO: Improve this calculations */
	size_t valid_bytes;
	size_t left_pages;
	size_t pages_to_read, bytes_to_read;
	void *read_buf;
	char *reader;
	char *writer = buf;
	/* char *cache; // Used when trying write cache for reads*/
	int read_pages;
	/* We can read at device page size granurality */
	int dev_page_size;
	int max_pages_read;
	int ret;

	HASH_FIND_INT(beamt, &beam, b);
	if (!b) {
		LNVM_DEBUG("Beam %d does not exits\n", beam);
		return -EINVAL;
	}

	dev_page_size = b->tgt->tgt->dev->dev_page_size;
	max_pages_read = b->tgt->tgt->dev->max_io_size;
	left_pages = count / dev_page_size +
		((((count) % dev_page_size) > 0) ? 1 : 0) +
		(((offset / dev_page_size) != (count + offset) / dev_page_size)
		? 1 : 0);

	LNVM_DEBUG("Read %lu bytes (pgs:%lu) from beam %d (p:%p, b:%d). Off:%lu\n",
			count, left_pages,
			b->gid, b,
			beam,
			offset);

	/* Assume that all blocks forming the beam have same size */
	nppas = get_npages_block(&b->vblocks[0]);

	ppa_count = offset / dev_page_size;
	block_off = ppa_count/ nppas;
	ppa_off = ppa_count % nppas;
	page_off = offset % dev_page_size;

	current_r_vblock = &b->vblocks[block_off];

	pages_to_read = (nppas > left_pages) ? left_pages : nppas;
	ret = posix_memalign(&read_buf, dev_page_size,
						pages_to_read * dev_page_size);
	if (ret) {
		LNVM_DEBUG("Cannot allocate memory (%lu bytes - align. %d )\n",
				left_pages * dev_page_size, dev_page_size);
		return ret;
	}
	reader = read_buf;

	while (left_bytes) {
		bytes_to_read = pages_to_read * dev_page_size;
		valid_bytes = (left_bytes > bytes_to_read) ?
						bytes_to_read : left_bytes;

		if (UNLIKELY(pages_to_read + ppa_off > nppas)) {
			while (pages_to_read + ppa_off > nppas)
				pages_to_read--;

			valid_bytes = (nppas * dev_page_size) -
					(ppa_off * dev_page_size) - page_off;
		}

		assert(left_bytes <= left_pages * dev_page_size);

		read_pages = flash_read(b->tgt->tgt_id, current_r_vblock, reader,
						ppa_off, pages_to_read,
						max_pages_read, dev_page_size);
		if (read_pages != pages_to_read)
			return -1;

		/* TODO: Optional - Flag for aligned memory */
		memcpy(writer, reader + page_off, valid_bytes);
		writer += valid_bytes;

		reader = read_buf;

		block_off++;
		ppa_off = 0;
		page_off = 0;
		current_r_vblock = &b->vblocks[block_off];

		left_pages -= read_pages;
		left_bytes -= valid_bytes;

		pages_to_read = (nppas > left_pages) ? left_pages : nppas;
	}

	/* TODO: Optional - Flag for aligned memory */
	free(read_buf);
	return count;
}

int nvm_init()
{
	pthread_spin_init(&fd_guid.lock, PTHREAD_PROCESS_SHARED);
	pthread_spin_init(&beam_guid.lock, PTHREAD_PROCESS_SHARED);

	/* TODO: Recover state */
	return 0;
}

void nvm_exit()
{
	pthread_spin_destroy(&fd_guid.lock);
	pthread_spin_destroy(&beam_guid.lock);

	/* TODO: save state */
}