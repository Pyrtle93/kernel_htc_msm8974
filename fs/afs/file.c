/* AFS filesystem file handling
 *
 * Copyright (C) 2002, 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/gfp.h>
#include "internal.h"

static int afs_readpage(struct file *file, struct page *page);
static void afs_invalidatepage(struct page *page, unsigned long offset);
static int afs_releasepage(struct page *page, gfp_t gfp_flags);
static int afs_launder_page(struct page *page);

static int afs_readpages(struct file *filp, struct address_space *mapping,
			 struct list_head *pages, unsigned nr_pages);

const struct file_operations afs_file_operations = {
	.open		= afs_open,
	.release	= afs_release,
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.write		= do_sync_write,
	.aio_read	= generic_file_aio_read,
	.aio_write	= afs_file_write,
	.mmap		= generic_file_readonly_mmap,
	.splice_read	= generic_file_splice_read,
	.fsync		= afs_fsync,
	.lock		= afs_lock,
	.flock		= afs_flock,
};

const struct inode_operations afs_file_inode_operations = {
	.getattr	= afs_getattr,
	.setattr	= afs_setattr,
	.permission	= afs_permission,
};

const struct address_space_operations afs_fs_aops = {
	.readpage	= afs_readpage,
	.readpages	= afs_readpages,
	.set_page_dirty	= afs_set_page_dirty,
	.launder_page	= afs_launder_page,
	.releasepage	= afs_releasepage,
	.invalidatepage	= afs_invalidatepage,
	.write_begin	= afs_write_begin,
	.write_end	= afs_write_end,
	.writepage	= afs_writepage,
	.writepages	= afs_writepages,
};

int afs_open(struct inode *inode, struct file *file)
{
	struct afs_vnode *vnode = AFS_FS_I(inode);
	struct key *key;
	int ret;

	_enter("{%x:%u},", vnode->fid.vid, vnode->fid.vnode);

	key = afs_request_key(vnode->volume->cell);
	if (IS_ERR(key)) {
		_leave(" = %ld [key]", PTR_ERR(key));
		return PTR_ERR(key);
	}

	ret = afs_validate(vnode, key);
	if (ret < 0) {
		_leave(" = %d [val]", ret);
		return ret;
	}

	file->private_data = key;
	_leave(" = 0");
	return 0;
}

int afs_release(struct inode *inode, struct file *file)
{
	struct afs_vnode *vnode = AFS_FS_I(inode);

	_enter("{%x:%u},", vnode->fid.vid, vnode->fid.vnode);

	key_put(file->private_data);
	_leave(" = 0");
	return 0;
}

#ifdef CONFIG_AFS_FSCACHE
static void afs_file_readpage_read_complete(struct page *page,
					    void *data,
					    int error)
{
	_enter("%p,%p,%d", page, data, error);

	if (!error)
		SetPageUptodate(page);
	unlock_page(page);
}
#endif

int afs_page_filler(void *data, struct page *page)
{
	struct inode *inode = page->mapping->host;
	struct afs_vnode *vnode = AFS_FS_I(inode);
	struct key *key = data;
	size_t len;
	off_t offset;
	int ret;

	_enter("{%x},{%lu},{%lu}", key_serial(key), inode->i_ino, page->index);

	BUG_ON(!PageLocked(page));

	ret = -ESTALE;
	if (test_bit(AFS_VNODE_DELETED, &vnode->flags))
		goto error;

	
#ifdef CONFIG_AFS_FSCACHE
	ret = fscache_read_or_alloc_page(vnode->cache,
					 page,
					 afs_file_readpage_read_complete,
					 NULL,
					 GFP_KERNEL);
#else
	ret = -ENOBUFS;
#endif
	switch (ret) {
		
	case 0:
		break;

		
	case -ENODATA:
		_debug("cache said ENODATA");
		goto go_on;

		
	case -ENOBUFS:
		_debug("cache said ENOBUFS");
	default:
	go_on:
		offset = page->index << PAGE_CACHE_SHIFT;
		len = min_t(size_t, i_size_read(inode) - offset, PAGE_SIZE);

		ret = afs_vnode_fetch_data(vnode, key, offset, len, page);
		if (ret < 0) {
			if (ret == -ENOENT) {
				_debug("got NOENT from server"
				       " - marking file deleted and stale");
				set_bit(AFS_VNODE_DELETED, &vnode->flags);
				ret = -ESTALE;
			}

#ifdef CONFIG_AFS_FSCACHE
			fscache_uncache_page(vnode->cache, page);
#endif
			BUG_ON(PageFsCache(page));
			goto error;
		}

		SetPageUptodate(page);

		
#ifdef CONFIG_AFS_FSCACHE
		if (PageFsCache(page) &&
		    fscache_write_page(vnode->cache, page, GFP_KERNEL) != 0) {
			fscache_uncache_page(vnode->cache, page);
			BUG_ON(PageFsCache(page));
		}
#endif
		unlock_page(page);
	}

	_leave(" = 0");
	return 0;

error:
	SetPageError(page);
	unlock_page(page);
	_leave(" = %d", ret);
	return ret;
}

static int afs_readpage(struct file *file, struct page *page)
{
	struct key *key;
	int ret;

	if (file) {
		key = file->private_data;
		ASSERT(key != NULL);
		ret = afs_page_filler(key, page);
	} else {
		struct inode *inode = page->mapping->host;
		key = afs_request_key(AFS_FS_S(inode->i_sb)->volume->cell);
		if (IS_ERR(key)) {
			ret = PTR_ERR(key);
		} else {
			ret = afs_page_filler(key, page);
			key_put(key);
		}
	}
	return ret;
}

static int afs_readpages(struct file *file, struct address_space *mapping,
			 struct list_head *pages, unsigned nr_pages)
{
	struct key *key = file->private_data;
	struct afs_vnode *vnode;
	int ret = 0;

	_enter("{%d},{%lu},,%d",
	       key_serial(key), mapping->host->i_ino, nr_pages);

	ASSERT(key != NULL);

	vnode = AFS_FS_I(mapping->host);
	if (test_bit(AFS_VNODE_DELETED, &vnode->flags)) {
		_leave(" = -ESTALE");
		return -ESTALE;
	}

	
#ifdef CONFIG_AFS_FSCACHE
	ret = fscache_read_or_alloc_pages(vnode->cache,
					  mapping,
					  pages,
					  &nr_pages,
					  afs_file_readpage_read_complete,
					  NULL,
					  mapping_gfp_mask(mapping));
#else
	ret = -ENOBUFS;
#endif

	switch (ret) {
		
	case 0:
		BUG_ON(!list_empty(pages));
		BUG_ON(nr_pages != 0);
		_leave(" = 0 [reading all]");
		return 0;

		
	case -ENODATA:
	case -ENOBUFS:
		break;

		
	default:
		_leave(" = %d", ret);
		return ret;
	}

	
	ret = read_cache_pages(mapping, pages, afs_page_filler, key);

	_leave(" = %d [netting]", ret);
	return ret;
}

static int afs_launder_page(struct page *page)
{
	_enter("{%lu}", page->index);

	return 0;
}

static void afs_invalidatepage(struct page *page, unsigned long offset)
{
	struct afs_writeback *wb = (struct afs_writeback *) page_private(page);

	_enter("{%lu},%lu", page->index, offset);

	BUG_ON(!PageLocked(page));

	
	if (offset == 0) {
#ifdef CONFIG_AFS_FSCACHE
		if (PageFsCache(page)) {
			struct afs_vnode *vnode = AFS_FS_I(page->mapping->host);
			fscache_wait_on_page_write(vnode->cache, page);
			fscache_uncache_page(vnode->cache, page);
		}
#endif

		if (PagePrivate(page)) {
			if (wb && !PageWriteback(page)) {
				set_page_private(page, 0);
				afs_put_writeback(wb);
			}

			if (!page_private(page))
				ClearPagePrivate(page);
		}
	}

	_leave("");
}

static int afs_releasepage(struct page *page, gfp_t gfp_flags)
{
	struct afs_writeback *wb = (struct afs_writeback *) page_private(page);
	struct afs_vnode *vnode = AFS_FS_I(page->mapping->host);

	_enter("{{%x:%u}[%lu],%lx},%x",
	       vnode->fid.vid, vnode->fid.vnode, page->index, page->flags,
	       gfp_flags);

	/* deny if page is being written to the cache and the caller hasn't
	 * elected to wait */
#ifdef CONFIG_AFS_FSCACHE
	if (!fscache_maybe_release_page(vnode->cache, page, gfp_flags)) {
		_leave(" = F [cache busy]");
		return 0;
	}
#endif

	if (PagePrivate(page)) {
		if (wb) {
			set_page_private(page, 0);
			afs_put_writeback(wb);
		}
		ClearPagePrivate(page);
	}

	
	_leave(" = T");
	return 1;
}
