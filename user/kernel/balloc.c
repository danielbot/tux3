/*
 * Block allocation
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 2
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3.h"

#ifndef trace
#define trace trace_on
#endif

#ifndef __KERNEL__
block_t count_range(struct inode *inode, block_t start, block_t count)
{
	unsigned char ones[256];

	assert(!(start & 7));

	for (int i = 0; i < sizeof(ones); i++)
		ones[i] = bytebits(i);

	struct sb *sb = tux_sb(inode->i_sb);
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	block_t limit = start + count;
	block_t blocks = (limit + mapmask) >> mapshift;
	block_t tail = (count + 7) >> 3, total = 0;
	unsigned offset = (start & mapmask) >> 3;

	for (block_t block = start >> mapshift; block < blocks; block++) {
		//trace("count block %x/%x", block, blocks);
		struct buffer_head *buffer = blockread(mapping(inode), block);
		if (!buffer)
			return -1;
		unsigned bytes = sb->blocksize - offset;
		if (bytes > tail)
			bytes = tail;
		unsigned char *p = bufdata(buffer) + offset, *top = p + bytes;
		while (p < top)
			total += ones[*p++];
		blockput(buffer);
		tail -= bytes;
		offset = 0;
	}
	return total;
}

block_t bitmap_dump(struct inode *inode, block_t start, block_t count)
{
	struct sb *sb = tux_sb(inode->i_sb);
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapmask = (1 << mapshift) - 1;
	block_t limit = start + count;
	block_t blocks = (limit + mapmask) >> mapshift, active = 0;
	unsigned offset = (start & mapmask) >> 3;
	unsigned startbit = start & 7;
	block_t tail = (count + startbit + 7) >> 3, begin = -1;

	__tux3_dbg("%Ld bitmap blocks:\n", blocks);
	for (block_t block = start >> mapshift; block < blocks; block++) {
		int ended = 0, any = 0;
		struct buffer_head *buffer = blockread(mapping(inode), block);
		if (!buffer)
			return -1;
		unsigned bytes = sb->blocksize - offset;
		if (bytes > tail)
			bytes = tail;
		unsigned char *p = bufdata(buffer) + offset, *top = p + bytes;
		for (; p < top; p++, startbit = 0) {
			unsigned c = *p;
			if (!any && c)
				__tux3_dbg("[%Lx] ", block);
			any |= c;
			if ((!c && begin < 0) || (c == 0xff && begin >= 0))
				continue;
			for (int i = startbit, mask = 1 << startbit; i < 8; i++, mask <<= 1) {
				if (!(c & mask) == (begin < 0))
					continue;
				block_t found = i + (((void *)p - bufdata(buffer)) << 3) + (block << mapshift);
				if (begin < 0)
					begin = found;
				else {
					if ((begin >> mapshift) != block)
						__tux3_dbg("-%Lx ", found - 1);
					else if (begin == found - 1)
						__tux3_dbg("%Lx ", begin);
					else
						__tux3_dbg("%Lx-%Lx ", begin, found - 1);
					begin = -1;
					ended++;
				}
			}
		}
		active += !!any;
		blockput(buffer);
		tail -= bytes;
		offset = 0;
		if (begin >= 0)
			__tux3_dbg("%Lx-", begin);
		if (any)
			__tux3_dbg("\n");
	}
	__tux3_dbg("(%Ld active)\n", active);
	return -1;
}
#endif

/*
 * Modify bits on one block, then adjust ->freeblocks.
 */
static int bitmap_modify_bits(struct sb *sb, struct buffer_head *buffer,
			      unsigned offset, unsigned blocks, int set)
{
	struct buffer_head *clone;
	void (*modify)(u8 *, unsigned, unsigned) = set ? set_bits : clear_bits;

	assert(blocks > 0);
	assert(offset + blocks <= sb->blocksize << 3);

	/*
	 * The bitmap is modified only by backend.
	 * blockdirty() should never return -EAGAIN.
	 */
	clone = blockdirty(buffer, sb->rollup);
	if (IS_ERR(clone)) {
		int err = PTR_ERR(clone);
		assert(err != -EAGAIN);
		return err;
	}

	modify(bufdata(clone), offset, blocks);

	mark_buffer_dirty_non(clone);
	blockput(clone);

	if (set)
		sb->freeblocks -= blocks;
	else
		sb->freeblocks += blocks;

	return 0;
}

/*
 * Modify bits on multiple blocks.  Caller may want to check range is
 * excepted state.
 *
 * FIXME: If error happened on middle of blocks, modified bits and
 * ->freeblocks are not restored to original. What to do?
 */
static int bitmap_modify(struct sb *sb, block_t start, unsigned blocks, int set)
{
	struct inode *bitmap = sb->bitmap;
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapsize = 1 << mapshift;
	unsigned mapmask = mapsize - 1;
	unsigned mapoffset = start & mapmask;
	block_t mapblock, mapblocks = (start + blocks + mapmask) >> mapshift;

	assert(blocks > 0);
	assert(start + blocks <= sb->volblocks);

	for (mapblock = start >> mapshift; mapblock < mapblocks; mapblock++) {
		struct buffer_head *buffer;
		unsigned len;
		int err;

		buffer = blockread(mapping(bitmap), mapblock);
		if (!buffer) {
			tux3_err(sb, "block read failed");
			// !!! error return sucks here
			return -EIO;
		}

		len = min(mapsize - mapoffset, blocks);
		err = bitmap_modify_bits(sb, buffer, mapoffset, len, set);
		if (err) {
			blockput(buffer);
			/* FIXME: error handling */
			return err;
		}

		mapoffset = 0;
		blocks -= len;
	}

	return 0;
}

/*
 * If bits on multiple blocks is excepted state, modify bits.
 *
 * FIXME: If error happened on middle of blocks, modified bits and
 * ->freeblocks are not restored to original. What to do?
 */
static int bitmap_test_and_modify(struct sb *sb, block_t start, unsigned blocks,
				  int set)
{
	struct inode *bitmap = sb->bitmap;
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapsize = 1 << mapshift;
	unsigned mapmask = mapsize - 1;
	unsigned mapoffset = start & mapmask;
	block_t mapblock, mapblocks = (start + blocks + mapmask) >> mapshift;
	int (*test)(u8 *, unsigned, unsigned) = set ? all_clear : all_set;

	assert(blocks > 0);
	assert(start + blocks <= sb->volblocks);

	for (mapblock = start >> mapshift; mapblock < mapblocks; mapblock++) {
		struct buffer_head *buffer;
		unsigned len;
		int err;

		buffer = blockread(mapping(bitmap), mapblock);
		if (!buffer) {
			tux3_err(sb, "block read failed");
			// !!! error return sucks here
			return -EIO;
		}

		len = min(mapsize - mapoffset, blocks);
		if (!test(bufdata(buffer), mapoffset, len)) {
			blockput(buffer);

			tux3_fs_error(sb, "%s: start 0x%Lx, count %x",
				      set ? "already allocated" : "double free",
				      start, blocks);

			return -EIO;	/* FIXME: error code? */
		}

		err = bitmap_modify_bits(sb, buffer, mapoffset, len, set);
		if (err) {
			blockput(buffer);
			/* FIXME: error handling */
			return err;
		}

		mapoffset = 0;
		blocks -= len;
	}

	return 0;
}

/* userland only */
block_t balloc_from_range(struct sb *sb, block_t start, block_t count,
			  unsigned blocks)
{
	struct inode *bitmap = sb->bitmap;
	unsigned mapshift = sb->blockbits + 3;
	unsigned mapsize = 1 << mapshift;
	unsigned mapmask = mapsize - 1;
	unsigned mapoffset = start & mapmask;
	block_t limit = start + count;
	block_t mapblock, mapblocks = (limit + mapmask) >> mapshift;
	struct buffer_head *buffer;
	block_t need, found;

	trace("balloc find %i blocks, range [%Lx, %Lx]", blocks, start, count);
	assert(blocks > 0);
	assert(start + count <= sb->volblocks);
	assert(tux3_under_backend(sb));

	need = blocks;
	for (mapblock = start >> mapshift; mapblock < mapblocks; mapblock++) {
		unsigned idx, maplimit;
		void *p;

		buffer = blockread(mapping(bitmap), mapblock);
		if (!buffer) {
			tux3_err(sb, "block read failed");
			// !!! error return sucks here
			return -EIO;
		}

		/* If last block of range, apply limit */
		if (mapblock == mapblocks - 1) {
			if (limit & mapmask)
				mapsize = limit & mapmask;
		}

		p = bufdata(buffer);
		while (1) {
			maplimit = min_t(block_t, mapoffset + need, mapsize);

			/* Check if there is no non-zero bits */
			idx = find_next_bit_le(p, maplimit, mapoffset);
			if (idx == maplimit) {
				need -= idx - mapoffset;
				if (need)
					break;	/* Need more blocks */

				/* Found requested free blocks */
				found = (mapblock << mapshift) + idx - blocks;
				goto found_range;
			}

			/* Reset needed blocks */
			need = blocks;

			/* Skip non-zero bit */
			mapoffset = find_next_zero_bit_le(p, mapsize, idx + 1);
			if (mapoffset == mapsize)
				break;	/* Search next blocks */
		}

		blockput(buffer);
		mapoffset = 0;
	}

	return -ENOSPC;

found_range:
	/* Found free blocks within one block? */
	if ((found >> mapshift) == mapblock) {
		unsigned foundoffset = found & mapmask;
		int err;

		err = bitmap_modify_bits(sb, buffer, foundoffset, blocks, 1);
		if (err) {
			blockput(buffer);
			/* FIXME: error handling */
			return err;
		}
	} else {
		int err;

		blockput(buffer);
		err = bitmap_modify(sb, found, blocks, 1);
		if (err)
			return err;
	}

	sb->nextalloc = found + blocks;
	//set_sb_dirty(sb);

	trace("balloc extent [block %Lx, count %x]", found, blocks);

	return found;
}

int balloc(struct sb *sb, unsigned blocks, block_t *block)
{
	block_t ret, goal = sb->nextalloc, total = sb->volblocks;

	ret = balloc_from_range(sb, goal, total - goal, blocks);
	if (goal && ret == -ENOSPC)
		ret = balloc_from_range(sb, 0, goal, blocks);

	if (ret < 0) {
		if (ret == -ENOSPC) {
			/* FIXME: This is for debugging. Remove this */
			tux3_warn(sb, "couldn't balloc: blocks %u", blocks);
		}
		return ret;
	}

	/* Set allocated block */
	*block = ret;

	return 0;
}

int bfree(struct sb *sb, block_t start, unsigned blocks)
{
	assert(tux3_under_backend(sb));
	trace("bfree extent [block %Lx, count %x], ", start, blocks);
	return bitmap_test_and_modify(sb, start, blocks, 0);
}

int replay_update_bitmap(struct replay *rp, block_t start, unsigned blocks,
			 int set)
{
	return bitmap_test_and_modify(rp->sb, start, blocks, set);
}
