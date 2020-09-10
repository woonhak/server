/*****************************************************************************

Copyright (c) 1995, 2017, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2013, 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file buf/buf0dblwr.cc
Doublwrite buffer module

Created 2011/12/19
*******************************************************/

#include "buf0dblwr.h"
#include "buf0buf.h"
#include "buf0checksum.h"
#include "srv0start.h"
#include "srv0srv.h"
#include "page0zip.h"
#include "trx0sys.h"
#include "fil0crypt.h"
#include "fil0pagecompress.h"

using st_::span;

/** The doublewrite buffer */
buf_dblwr_t*	buf_dblwr = NULL;

/** Set to TRUE when the doublewrite buffer is being created */
ibool	buf_dblwr_being_created = FALSE;

#define TRX_SYS_DOUBLEWRITE_BLOCKS 2

/****************************************************************//**
Determines if a page number is located inside the doublewrite buffer.
@return TRUE if the location is inside the two blocks of the
doublewrite buffer */
ibool
buf_dblwr_page_inside(
/*==================*/
	ulint	page_no)	/*!< in: page number */
{
	if (buf_dblwr == NULL) {

		return(FALSE);
	}

	if (page_no >= buf_dblwr->block1
	    && page_no < buf_dblwr->block1
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	if (page_no >= buf_dblwr->block2
	    && page_no < buf_dblwr->block2
	    + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		return(TRUE);
	}

	return(FALSE);
}

/** @return the TRX_SYS page */
inline buf_block_t *buf_dblwr_trx_sys_get(mtr_t *mtr)
{
  buf_block_t *block= buf_page_get(page_id_t(TRX_SYS_SPACE, TRX_SYS_PAGE_NO),
                                   0, RW_X_LATCH, mtr);
  buf_block_dbg_add_level(block, SYNC_NO_ORDER_CHECK);
  return block;
}

/****************************************************************//**
Creates or initialializes the doublewrite buffer at a database start. */
static void buf_dblwr_init(const byte *doublewrite)
{
	ulint	buf_size;

	buf_dblwr = static_cast<buf_dblwr_t*>(
		ut_zalloc_nokey(sizeof(buf_dblwr_t)));

	/* There are two blocks of same size in the doublewrite
	buffer. */
	buf_size = TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;

	mutex_create(LATCH_ID_BUF_DBLWR, &buf_dblwr->mutex);

	buf_dblwr->b_event = os_event_create("dblwr_batch_event");
	buf_dblwr->first_free = 0;
	buf_dblwr->b_reserved = 0;

	buf_dblwr->block1 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK1);
	buf_dblwr->block2 = mach_read_from_4(
		doublewrite + TRX_SYS_DOUBLEWRITE_BLOCK2);

	buf_dblwr->write_buf = static_cast<byte*>(
		aligned_malloc(buf_size << srv_page_size_shift,
			       srv_page_size));

	buf_dblwr->buf_block_arr = static_cast<buf_dblwr_t::element*>(
		ut_zalloc_nokey(buf_size * sizeof(buf_dblwr_t::element)));
}

/** Create the doublewrite buffer if the doublewrite buffer header
is not present in the TRX_SYS page.
@return	whether the operation succeeded
@retval	true	if the doublewrite buffer exists or was created
@retval	false	if the creation failed (too small first data file) */
bool
buf_dblwr_create()
{
	buf_block_t*	block2;
	buf_block_t*	new_block;
	byte*	fseg_header;
	ulint	page_no;
	ulint	prev_page_no;
	ulint	i;
	mtr_t	mtr;

	if (buf_dblwr) {
		/* Already inited */
		return(true);
	}

start_again:
	mtr.start();
	buf_dblwr_being_created = TRUE;

	buf_block_t *trx_sys_block = buf_dblwr_trx_sys_get(&mtr);

	if (mach_read_from_4(TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_MAGIC
			     + trx_sys_block->frame)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has already been created:
		just read in some numbers */

		buf_dblwr_init(TRX_SYS_DOUBLEWRITE + trx_sys_block->frame);

		mtr.commit();
		buf_dblwr_being_created = FALSE;
		return(true);
	} else {
		if (UT_LIST_GET_FIRST(fil_system.sys_space->chain)->size
		    < 3 * FSP_EXTENT_SIZE) {
			goto too_small;
		}
	}

	block2 = fseg_create(fil_system.sys_space,
			     TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_FSEG,
			     &mtr, false, trx_sys_block);

	if (block2 == NULL) {
too_small:
		ib::error()
			<< "Cannot create doublewrite buffer: "
			"the first file in innodb_data_file_path"
			" must be at least "
			<< (3 * (FSP_EXTENT_SIZE
				 >> (20U - srv_page_size_shift)))
			<< "M.";
		mtr.commit();
		return(false);
	}

	ib::info() << "Doublewrite buffer not found: creating new";

	/* FIXME: After this point, the doublewrite buffer creation
	is not atomic. The doublewrite buffer should not exist in
	the InnoDB system tablespace file in the first place.
	It could be located in separate optional file(s) in a
	user-specified location. */

	/* fseg_create acquires a second latch on the page,
	therefore we must declare it: */

	buf_block_dbg_add_level(block2, SYNC_NO_ORDER_CHECK);

	fseg_header = TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_FSEG
		+ trx_sys_block->frame;
	prev_page_no = 0;

	for (i = 0; i < TRX_SYS_DOUBLEWRITE_BLOCKS * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE
		     + FSP_EXTENT_SIZE / 2; i++) {
		new_block = fseg_alloc_free_page(
			fseg_header, prev_page_no + 1, FSP_UP, &mtr);
		if (new_block == NULL) {
			ib::error() << "Cannot create doublewrite buffer: "
				" you must increase your tablespace size."
				" Cannot continue operation.";
			/* This may essentially corrupt the doublewrite
			buffer. However, usually the doublewrite buffer
			is created at database initialization, and it
			should not matter (just remove all newly created
			InnoDB files and restart). */
			mtr.commit();
			return(false);
		}

		/* We read the allocated pages to the buffer pool;
		when they are written to disk in a flush, the space
		id and page number fields are also written to the
		pages. When we at database startup read pages
		from the doublewrite buffer, we know that if the
		space id and page number in them are the same as
		the page position in the tablespace, then the page
		has not been written to in doublewrite. */

		ut_ad(rw_lock_get_x_lock_count(&new_block->lock) == 1);
		page_no = new_block->page.id().page_no();
		/* We only do this in the debug build, to ensure that
		the check in buf_flush_init_for_writing() will see a valid
		page type. The flushes of new_block are actually
		unnecessary here.  */
		ut_d(mtr.write<2>(*new_block,
				  FIL_PAGE_TYPE + new_block->frame,
				  FIL_PAGE_TYPE_SYS));

		if (i == FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == FSP_EXTENT_SIZE);
			mtr.write<4>(*trx_sys_block,
				     TRX_SYS_DOUBLEWRITE
				     + TRX_SYS_DOUBLEWRITE_BLOCK1
				     + trx_sys_block->frame,
				     page_no);
			mtr.write<4>(*trx_sys_block,
				     TRX_SYS_DOUBLEWRITE
				     + TRX_SYS_DOUBLEWRITE_REPEAT
				     + TRX_SYS_DOUBLEWRITE_BLOCK1
				     + trx_sys_block->frame,
				     page_no);

		} else if (i == FSP_EXTENT_SIZE / 2
			   + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
			ut_a(page_no == 2 * FSP_EXTENT_SIZE);
			mtr.write<4>(*trx_sys_block,
				     TRX_SYS_DOUBLEWRITE
				     + TRX_SYS_DOUBLEWRITE_BLOCK2
				     + trx_sys_block->frame,
				     page_no);
			mtr.write<4>(*trx_sys_block,
				     TRX_SYS_DOUBLEWRITE
				     + TRX_SYS_DOUBLEWRITE_REPEAT
				     + TRX_SYS_DOUBLEWRITE_BLOCK2
				     + trx_sys_block->frame,
				     page_no);
		} else if (i > FSP_EXTENT_SIZE / 2) {
			ut_a(page_no == prev_page_no + 1);
		}

		if (((i + 1) & 15) == 0) {
			/* rw_locks can only be recursively x-locked
			2048 times. (on 32 bit platforms,
			(lint) 0 - (X_LOCK_DECR * 2049)
			is no longer a negative number, and thus
			lock_word becomes like a shared lock).
			For 4k page size this loop will
			lock the fseg header too many times. Since
			this code is not done while any other threads
			are active, restart the MTR occasionally. */
			mtr.commit();
			mtr.start();
			trx_sys_block = buf_dblwr_trx_sys_get(&mtr);
			fseg_header = TRX_SYS_DOUBLEWRITE
				+ TRX_SYS_DOUBLEWRITE_FSEG
				+ trx_sys_block->frame;
		}

		prev_page_no = page_no;
	}

	mtr.write<4>(*trx_sys_block,
		     TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_MAGIC
		     + trx_sys_block->frame,
		     TRX_SYS_DOUBLEWRITE_MAGIC_N);
	mtr.write<4>(*trx_sys_block,
		     TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_MAGIC
		     + TRX_SYS_DOUBLEWRITE_REPEAT
		     + trx_sys_block->frame,
		     TRX_SYS_DOUBLEWRITE_MAGIC_N);

	mtr.write<4>(*trx_sys_block,
		     TRX_SYS_DOUBLEWRITE + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED
		     + trx_sys_block->frame,
		     TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N);
	mtr.commit();

	/* Flush the modified pages to disk and make a checkpoint */
	log_make_checkpoint();
	buf_dblwr_being_created = FALSE;

	/* Remove doublewrite pages from LRU */
	buf_pool_invalidate();

	ib::info() <<  "Doublewrite buffer created";

	goto start_again;
}

/**
At database startup initializes the doublewrite buffer memory structure if
we already have a doublewrite buffer created in the data files. If we are
upgrading to an InnoDB version which supports multiple tablespaces, then this
function performs the necessary update operations. If we are in a crash
recovery, this function loads the pages from double write buffer into memory.
@param[in]	file		File handle
@param[in]	path		Path name of file
@return DB_SUCCESS or error code */
dberr_t
buf_dblwr_init_or_load_pages(
	pfs_os_file_t	file,
	const char*	path)
{
	byte*		buf;
	byte*		page;
	ulint		block1;
	ulint		block2;
	ulint		space_id;
	byte*		read_buf;
	byte*		doublewrite;
	ibool		reset_space_ids = FALSE;
	recv_dblwr_t&	recv_dblwr = recv_sys.dblwr;

	/* We do the file i/o past the buffer pool */
	read_buf = static_cast<byte*>(
		aligned_malloc(2 * srv_page_size, srv_page_size));

	/* Read the trx sys header to check if we are using the doublewrite
	buffer */
	dberr_t		err;

	IORequest       read_request(IORequest::READ);

	err = os_file_read(
		read_request,
		file, read_buf, TRX_SYS_PAGE_NO << srv_page_size_shift,
		srv_page_size);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Failed to read the system tablespace header page";
func_exit:
		aligned_free(read_buf);
		return(err);
	}

	doublewrite = read_buf + TRX_SYS_DOUBLEWRITE;

	/* TRX_SYS_PAGE_NO is not encrypted see fil_crypt_rotate_page() */

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_MAGIC)
	    == TRX_SYS_DOUBLEWRITE_MAGIC_N) {
		/* The doublewrite buffer has been created */

		buf_dblwr_init(doublewrite);

		block1 = buf_dblwr->block1;
		block2 = buf_dblwr->block2;

		buf = buf_dblwr->write_buf;
	} else {
		err = DB_SUCCESS;
		goto func_exit;
	}

	if (mach_read_from_4(doublewrite + TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED)
	    != TRX_SYS_DOUBLEWRITE_SPACE_ID_STORED_N) {

		/* We are upgrading from a version < 4.1.x to a version where
		multiple tablespaces are supported. We must reset the space id
		field in the pages in the doublewrite buffer because starting
		from this version the space id is stored to
		FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID. */

		reset_space_ids = TRUE;

		ib::info() << "Resetting space id's in the doublewrite buffer";
	}

	/* Read the pages from the doublewrite buffer to memory */
	err = os_file_read(
		read_request,
		file, buf, block1 << srv_page_size_shift,
		TRX_SYS_DOUBLEWRITE_BLOCK_SIZE << srv_page_size_shift);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Failed to read the first double write buffer "
			"extent";
		goto func_exit;
	}

	err = os_file_read(
		read_request,
		file,
		buf + (TRX_SYS_DOUBLEWRITE_BLOCK_SIZE << srv_page_size_shift),
		block2 << srv_page_size_shift,
		TRX_SYS_DOUBLEWRITE_BLOCK_SIZE << srv_page_size_shift);

	if (err != DB_SUCCESS) {

		ib::error()
			<< "Failed to read the second double write buffer "
			"extent";
		goto func_exit;
	}

	/* Check if any of these pages is half-written in data files, in the
	intended position */

	page = buf;

	for (ulint i = 0; i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * 2; i++) {

		if (reset_space_ids) {
			ulint source_page_no;

			space_id = 0;
			mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID,
					space_id);
			/* We do not need to calculate new checksums for the
			pages because the field .._SPACE_ID does not affect
			them. Write the page back to where we read it from. */

			if (i < TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
				source_page_no = block1 + i;
			} else {
				source_page_no = block2
					+ i - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
			}

			err = os_file_write(
				IORequestWrite, path, file, page,
				source_page_no << srv_page_size_shift,
				srv_page_size);
			if (err != DB_SUCCESS) {

				ib::error()
					<< "Failed to write to the double write"
					" buffer";
				goto func_exit;
			}
		} else if (mach_read_from_8(page + FIL_PAGE_LSN)) {
			/* Each valid page header must contain
			a nonzero FIL_PAGE_LSN field. */
			recv_dblwr.add(page);
		}

		page += srv_page_size;
	}

	if (reset_space_ids) {
		os_file_flush(file);
	}

	err = DB_SUCCESS;
	goto func_exit;
}

/** Process and remove the double write buffer pages for all tablespaces. */
void
buf_dblwr_process()
{
	ut_ad(recv_sys.parse_start_lsn);

	ulint		page_no_dblwr	= 0;
	byte*		read_buf;
	recv_dblwr_t&	recv_dblwr	= recv_sys.dblwr;

	if (!buf_dblwr) {
		return;
	}

	read_buf = static_cast<byte*>(
		aligned_malloc(3 * srv_page_size, srv_page_size));
	byte* const buf = read_buf + srv_page_size;

	for (recv_dblwr_t::list::iterator i = recv_dblwr.pages.begin();
	     i != recv_dblwr.pages.end();
	     ++i, ++page_no_dblwr) {
		byte* page = *i;
		const ulint page_no = page_get_page_no(page);

		if (!page_no) {
			/* page 0 should have been recovered
			already via Datafile::restore_from_doublewrite() */
			continue;
		}

		const ulint space_id = page_get_space_id(page);
		const lsn_t lsn = mach_read_from_8(page + FIL_PAGE_LSN);

		if (recv_sys.parse_start_lsn > lsn) {
			/* Pages written before the checkpoint are
			not useful for recovery. */
			continue;
		}

		const page_id_t page_id(space_id, page_no);

		if (recv_sys.scanned_lsn < lsn) {
			ib::warn() << "Ignoring a doublewrite copy of page "
				   << page_id
				   << " with future log sequence number "
				   << lsn;
			continue;
		}

		fil_space_t* space = fil_space_acquire_for_io(space_id);

		if (!space) {
			/* Maybe we have dropped the tablespace
			and this page once belonged to it: do nothing */
			continue;
		}

		fil_space_open_if_needed(space);

		if (UNIV_UNLIKELY(page_no >= space->size)) {

			/* Do not report the warning for undo
			tablespaces, because they can be truncated in place. */
			if (!srv_is_undo_tablespace(space_id)) {
				ib::warn() << "A copy of page " << page_no
					<< " in the doublewrite buffer slot "
					<< page_no_dblwr
					<< " is beyond the end of tablespace "
					<< space->name
					<< " (" << space->size << " pages)";
			}
next_page:
			space->release_for_io();
			continue;
		}

		const ulint physical_size = space->physical_size();
		const ulint zip_size = space->zip_size();
		ut_ad(!buf_is_zeroes(span<const byte>(page, physical_size)));

		/* We want to ensure that for partial reads the
		unread portion of the page is NUL. */
		memset(read_buf, 0x0, physical_size);

		IORequest	request;

		request.dblwr_recover();

		/* Read in the actual page from the file */
		fil_io_t fio = fil_io(
			request, true,
			page_id, zip_size,
			0, physical_size, read_buf, NULL);

		if (UNIV_UNLIKELY(fio.err != DB_SUCCESS)) {
			ib::warn()
				<< "Double write buffer recovery: "
				<< page_id << " read failed with "
				<< "error: " << fio.err;
		}

		if (fio.node) {
			fio.node->space->release_for_io();
		}

		if (buf_is_zeroes(span<const byte>(read_buf, physical_size))) {
			/* We will check if the copy in the
			doublewrite buffer is valid. If not, we will
			ignore this page (there should be redo log
			records to initialize it). */
		} else if (recv_dblwr.validate_page(
				page_id, read_buf, space, buf)) {
			goto next_page;
		} else {
			/* We intentionally skip this message for
			all-zero pages. */
			ib::info()
				<< "Trying to recover page " << page_id
				<< " from the doublewrite buffer.";
		}

		page = recv_dblwr.find_page(page_id, space, buf);

		if (!page) {
			goto next_page;
		}

		/* Write the good page from the doublewrite buffer to
		the intended position. */
		fio = fil_io(IORequestWrite, true, page_id, zip_size,
			     0, physical_size, page, nullptr);

		if (fio.node) {
			ut_ad(fio.err == DB_SUCCESS);
			ib::info() << "Recovered page " << page_id
				   << " to '" << fio.node->name
				   << "' from the doublewrite buffer.";
			fio.node->space->release_for_io();
		}

		goto next_page;
	}

	recv_dblwr.pages.clear();

	fil_flush_file_spaces();
	aligned_free(read_buf);
}

/****************************************************************//**
Frees doublewrite buffer. */
void
buf_dblwr_free()
{
	/* Free the double write data structures. */
	ut_a(buf_dblwr != NULL);
	ut_ad(buf_dblwr->b_reserved == 0);

	os_event_destroy(buf_dblwr->b_event);
	aligned_free(buf_dblwr->write_buf);
	ut_free(buf_dblwr->buf_block_arr);
	mutex_free(&buf_dblwr->mutex);
	ut_free(buf_dblwr);
	buf_dblwr = NULL;
}

/** Update the doublewrite buffer on write completion. */
void buf_dblwr_update(const buf_page_t &bpage)
{
  ut_ad(srv_use_doublewrite_buf);
  ut_ad(buf_dblwr);
  ut_ad(!fsp_is_system_temporary(bpage.id().space()));
  ut_ad(!srv_read_only_mode);

  mutex_enter(&buf_dblwr->mutex);

  ut_ad(buf_dblwr->batch_running);
  ut_ad(buf_dblwr->b_reserved > 0);
  ut_ad(buf_dblwr->b_reserved <= buf_dblwr->first_free);

  if (!--buf_dblwr->b_reserved)
  {
    mutex_exit(&buf_dblwr->mutex);
    /* This will finish the batch. Sync data files to the disk. */
    fil_flush_file_spaces();
    mutex_enter(&buf_dblwr->mutex);

    /* We can now reuse the doublewrite memory buffer: */
    buf_dblwr->first_free= 0;
    buf_dblwr->batch_running= false;
    os_event_set(buf_dblwr->b_event);
  }

  mutex_exit(&buf_dblwr->mutex);
}

#ifdef UNIV_DEBUG
/** Check the LSN values on the page.
@param[in] page  page to check
@param[in] s     tablespace */
static void buf_dblwr_check_page_lsn(const page_t* page, const fil_space_t& s)
{
  /* Ignore page_compressed or encrypted pages */
  if (s.is_compressed() || buf_page_get_key_version(page, s.flags))
    return;
  const byte* lsn_start= FIL_PAGE_LSN + 4 + page;
  const byte* lsn_end= page + srv_page_size -
    (s.full_crc32()
     ? FIL_PAGE_FCRC32_END_LSN
     : FIL_PAGE_END_LSN_OLD_CHKSUM - 4);
  static_assert(FIL_PAGE_FCRC32_END_LSN % 4 == 0, "alignment");
  static_assert(FIL_PAGE_LSN % 4 == 0, "alignment");
  ut_ad(!memcmp_aligned<4>(lsn_start, lsn_end, 4));
}

static void buf_dblwr_check_page_lsn(const buf_page_t &b, const byte *page)
{
  if (fil_space_t *space= fil_space_acquire_for_io(b.id().space()))
  {
    buf_dblwr_check_page_lsn(page, *space);
    space->release_for_io();
  }
}
#endif /* UNIV_DEBUG */

/********************************************************************//**
Asserts when a corrupt block is find during writing out data to the
disk. */
static
void
buf_dblwr_assert_on_corrupt_block(
/*==============================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	buf_page_print(block->frame);

	ib::fatal() << "Apparent corruption of an index page "
		<< block->page.id()
		<< " to be written to data file. We intentionally crash"
		" the server to prevent corrupt data from ending up in"
		" data files.";
}

/********************************************************************//**
Check the LSN values on the page with which this block is associated.
Also validate the page if the option is set. */
static
void
buf_dblwr_check_block(
/*==================*/
	const buf_block_t*	block)	/*!< in: block to check */
{
	ut_ad(block->page.state() == BUF_BLOCK_FILE_PAGE);

	switch (fil_page_get_type(block->frame)) {
	case FIL_PAGE_INDEX:
	case FIL_PAGE_TYPE_INSTANT:
	case FIL_PAGE_RTREE:
		if (page_is_comp(block->frame)) {
			if (page_simple_validate_new(block->frame)) {
				return;
			}
		} else if (page_simple_validate_old(block->frame)) {
			return;
		}
		/* While it is possible that this is not an index page
		but just happens to have wrongly set FIL_PAGE_TYPE,
		such pages should never be modified to without also
		adjusting the page type during page allocation or
		buf_flush_init_for_writing() or fil_block_reset_type(). */
		break;
	case FIL_PAGE_TYPE_FSP_HDR:
	case FIL_PAGE_IBUF_BITMAP:
	case FIL_PAGE_TYPE_UNKNOWN:
		/* Do not complain again, we already reset this field. */
	case FIL_PAGE_UNDO_LOG:
	case FIL_PAGE_INODE:
	case FIL_PAGE_IBUF_FREE_LIST:
	case FIL_PAGE_TYPE_SYS:
	case FIL_PAGE_TYPE_TRX_SYS:
	case FIL_PAGE_TYPE_XDES:
	case FIL_PAGE_TYPE_BLOB:
	case FIL_PAGE_TYPE_ZBLOB:
	case FIL_PAGE_TYPE_ZBLOB2:
		/* TODO: validate also non-index pages */
		return;
	case FIL_PAGE_TYPE_ALLOCATED:
		/* empty pages should never be flushed */
		return;
	}

	buf_dblwr_assert_on_corrupt_block(block);
}

/********************************************************************//**
Writes a page that has already been written to the doublewrite buffer
to the datafile. It is the job of the caller to sync the datafile. */
static void
buf_dblwr_write_block_to_datafile(const buf_dblwr_t::element &e)
{
  buf_page_t* bpage= e.bpage;
  ut_a(bpage->in_file());
  IORequest request(IORequest::WRITE, bpage, e.lru);

  /* We request frame here to get correct buffer in case of
  encryption and/or page compression */
  void *frame = buf_page_get_frame(bpage);

  auto size= e.size;

  if (UNIV_LIKELY_NULL(bpage->zip.data))
  {
    size= bpage->zip_size();
    ut_ad(size);
  }
  else
  {
    ut_ad(bpage->state() == BUF_BLOCK_FILE_PAGE);
    ut_ad(!bpage->zip_size());
    ut_d(buf_dblwr_check_page_lsn(*bpage, static_cast<const byte*>(frame)));
  }

  fil_io(request, false, bpage->id(), bpage->zip_size(), 0, size, frame,
         bpage);
}

/********************************************************************//**
Flushes possible buffered writes from the doublewrite memory buffer to disk.
It is very important to call this function after a batch of writes has been posted,
and also when we may have to wait for a page latch! Otherwise a deadlock
of threads can occur. */
void
buf_dblwr_flush_buffered_writes()
{
	byte*		write_buf;
	ulint		first_free;
	ulint		len;

	if (!srv_use_doublewrite_buf || buf_dblwr == NULL) {
		/* Sync the writes to the disk. */
		os_aio_wait_until_no_pending_writes();
		/* Now we flush the data to disk (for example, with fsync) */
		fil_flush_file_spaces();
		return;
	}

	ut_ad(!srv_read_only_mode);

try_again:
	mutex_enter(&buf_dblwr->mutex);

	/* Write first to doublewrite buffer blocks. We use synchronous
	aio and thus know that file write has been completed when the
	control returns. */

	if (buf_dblwr->first_free == 0) {

		mutex_exit(&buf_dblwr->mutex);
		return;
	}

	if (buf_dblwr->batch_running) {
		/* Another thread is running the batch right now. Wait
		for it to finish. */
		int64_t	sig_count = os_event_reset(buf_dblwr->b_event);
		mutex_exit(&buf_dblwr->mutex);

		os_event_wait_low(buf_dblwr->b_event, sig_count);
		goto try_again;
	}

	ut_ad(buf_dblwr->first_free == buf_dblwr->b_reserved);

	/* Disallow anyone else to post to doublewrite buffer or to
	start another batch of flushing. */
	buf_dblwr->batch_running = true;
	first_free = buf_dblwr->first_free;

	/* Now safe to release the mutex. */
	mutex_exit(&buf_dblwr->mutex);

	write_buf = buf_dblwr->write_buf;

	for (ulint len2 = 0, i = 0;
	     i < buf_dblwr->first_free;
	     len2 += srv_page_size, i++) {

		buf_page_t* bpage= buf_dblwr->buf_block_arr[i].bpage;

		if (bpage->state() != BUF_BLOCK_FILE_PAGE || bpage->zip.data) {
			/* No simple validate for compressed
			pages exists. */
			continue;
		}

		/* Check that the actual page in the buffer pool is
		not corrupt and the LSN values are sane. */
		buf_dblwr_check_block(reinterpret_cast<buf_block_t*>(bpage));
		ut_d(buf_dblwr_check_page_lsn(*bpage, write_buf + len2));
	}

	/* Write out the first block of the doublewrite buffer */
	len = std::min<ulint>(TRX_SYS_DOUBLEWRITE_BLOCK_SIZE,
			      buf_dblwr->first_free) << srv_page_size_shift;

	fil_io_t fio = fil_io(IORequestWrite, true,
			      page_id_t(TRX_SYS_SPACE, buf_dblwr->block1), 0,
			      0, len, write_buf, nullptr);
	fio.node->space->release_for_io();

	if (buf_dblwr->first_free <= TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		/* No unwritten pages in the second block. */
		goto flush;
	}

	/* Write out the second block of the doublewrite buffer. */
	len = (buf_dblwr->first_free - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
	       << srv_page_size_shift;

	write_buf = buf_dblwr->write_buf
		+ (TRX_SYS_DOUBLEWRITE_BLOCK_SIZE << srv_page_size_shift);

	fio = fil_io(IORequestWrite, true,
		     page_id_t(TRX_SYS_SPACE, buf_dblwr->block2), 0,
		     0, len, write_buf, nullptr);
	fio.node->space->release_for_io();

flush:
	/* increment the doublewrite flushed pages counter */
	srv_stats.dblwr_pages_written.add(buf_dblwr->first_free);
	srv_stats.dblwr_writes.inc();

	/* Now flush the doublewrite buffer data to disk */
	fil_flush(TRX_SYS_SPACE);

	/* We know that the writes have been flushed to disk now
	and in recovery we will find them in the doublewrite buffer
	blocks. Next do the writes to the intended positions. */

	/* Up to this point first_free and buf_dblwr->first_free are
	same because we have set the buf_dblwr->batch_running flag
	disallowing any other thread to post any request but we
	can't safely access buf_dblwr->first_free in the loop below.
	This is so because it is possible that after we are done with
	the last iteration and before we terminate the loop, the batch
	gets finished in the IO helper thread and another thread posts
	a new batch setting buf_dblwr->first_free to a higher value.
	If this happens and we are using buf_dblwr->first_free in the
	loop termination condition then we'll end up dispatching
	the same block twice from two different threads. */
	ut_ad(first_free == buf_dblwr->first_free);
	for (ulint i = 0; i < first_free; i++) {
		buf_dblwr_write_block_to_datafile(buf_dblwr->buf_block_arr[i]);
	}
}

/** Schedule a page write. If the doublewrite memory buffer is full,
buf_dblwr_flush_buffered_writes() will be invoked to make space.
@param bpage      buffer pool page to be written
@param lru        true=buf_pool.LRU; false=buf_pool.flush_list
@param size       payload size in bytes */
void buf_dblwr_t::add_to_batch(buf_page_t *bpage, bool lru, size_t size)
{
  ut_ad(bpage->in_file());
try_again:
  mutex_enter(&mutex);

  if (batch_running)
  {
    /* This not nearly as bad as it looks. There is only page_cleaner
    thread which does background flushing in batches therefore it is
    unlikely to be a contention point. The only exception is when a
    user thread is forced to do a flush batch because of a sync
    checkpoint. */
    int64_t sig_count= os_event_reset(b_event);
    mutex_exit(&mutex);

    os_event_wait_low(b_event, sig_count);
    goto try_again;
  }

  if (first_free == TRX_SYS_DOUBLEWRITE_BLOCKS *
      TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
  {
    mutex_exit(&mutex);
    buf_dblwr_flush_buffered_writes();
    goto try_again;
  }

  ut_ad(first_free < TRX_SYS_DOUBLEWRITE_BLOCKS *
        TRX_SYS_DOUBLEWRITE_BLOCK_SIZE);
  byte *p= write_buf + srv_page_size * first_free;

  /* We request frame here to get correct buffer in case of
  encryption and/or page compression */
  void * frame = buf_page_get_frame(bpage);

  memcpy_aligned<OS_FILE_LOG_BLOCK_SIZE>(p, frame, size);
  ut_ad(!bpage->zip_size() || bpage->zip_size() == size);
  buf_block_arr[first_free++] = { bpage, lru, size };
  b_reserved++;

  ut_ad(!batch_running);
  ut_ad(first_free == b_reserved);
  ut_ad(b_reserved <= TRX_SYS_DOUBLEWRITE_BLOCKS *
        TRX_SYS_DOUBLEWRITE_BLOCK_SIZE);

  const bool need_flush= first_free == TRX_SYS_DOUBLEWRITE_BLOCKS *
    TRX_SYS_DOUBLEWRITE_BLOCK_SIZE;
  mutex_exit(&mutex);

  if (need_flush)
    buf_dblwr_flush_buffered_writes();
}
