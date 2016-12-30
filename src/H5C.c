/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the files COPYING and Copyright.html.  COPYING can be found at the root   *
 * of the source code distribution tree; Copyright.html can be found at the  *
 * root level of an installed copy of the electronic HDF5 document set and   *
 * is linked from the top-level documents page.  It can also be found at     *
 * http://hdfgroup.org/HDF5/doc/Copyright.html.  If you do not have          *
 * access to either file, you may request a copy from help@hdfgroup.org.     *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*-------------------------------------------------------------------------
 *
 * Created:     H5C.c
 *              June 1 2004
 *              John Mainzer
 *
 * Purpose:     Functions in this file implement a generic cache for
 *              things which exist on disk, and which may be
 *	 	unambiguously referenced by their disk addresses.
 *
 *              The code in this module was initially written in
 *		support of a complete re-write of the metadata cache
 *		in H5AC.c  However, other uses for the cache code
 *		suggested themselves, and thus this file was created
 *		in an attempt to support re-use.
 *
 *		For a detailed overview of the cache, please see the
 *		header comment for H5C_t in H5Cpkg.h.
 *
 *-------------------------------------------------------------------------
 */

/**************************************************************************
 *
 *				To Do:
 *
 *	Code Changes:
 *
 *	 - Remove extra functionality in H5C__flush_single_entry()?
 *
 *	 - Change protect/unprotect to lock/unlock.
 *
 *	 - Flush entries in increasing address order in
 *	   H5C_make_space_in_cache().
 *
 *	 - Also in H5C_make_space_in_cache(), use high and low water marks
 *	   to reduce the number of I/O calls.
 *
 *	 - When flushing, attempt to combine contiguous entries to reduce
 *	   I/O overhead.  Can't do this just yet as some entries are not
 *	   contiguous.  Do this in parallel only or in serial as well?
 *
 *	 - Create MPI type for dirty objects when flushing in parallel.
 *
 *	 - Now that TBBT routines aren't used, fix nodes in memory to
 *         point directly to the skip list node from the LRU list, eliminating
 *         skip list lookups when evicting objects from the cache.
 *
 *	Tests:
 *
 *	 - Trim execution time.  (This is no longer a major issue with the
 *	   shift from the TBBT to a hash table for indexing.)
 *
 *	 - Add random tests.
 *
 **************************************************************************/

/****************/
/* Module Setup */
/****************/

#include "H5Cmodule.h"          /* This source code file is part of the H5C module */
#define H5F_FRIEND		/*suppress error about including H5Fpkg	  */


/***********/
/* Headers */
/***********/
#include "H5private.h"		/* Generic Functions			*/
#include "H5Cpkg.h"		/* Cache				*/
#include "H5Eprivate.h"		/* Error handling		  	*/
#include "H5Fpkg.h"		/* Files				*/
#include "H5FLprivate.h"	/* Free Lists                           */
#include "H5Iprivate.h"		/* IDs			  		*/
#include "H5MFprivate.h"	/* File memory management		*/
#include "H5MMprivate.h"	/* Memory management			*/
#include "H5Pprivate.h"         /* Property lists                       */


/****************/
/* Local Macros */
/****************/
#if H5C_DO_MEMORY_SANITY_CHECKS
#define H5C_IMAGE_EXTRA_SPACE 8
#define H5C_IMAGE_SANITY_VALUE "DeadBeef"
#else /* H5C_DO_MEMORY_SANITY_CHECKS */
#define H5C_IMAGE_EXTRA_SPACE 0
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */


/******************/
/* Local Typedefs */
/******************/


/********************/
/* Local Prototypes */
/********************/

static herr_t H5C__auto_adjust_cache_size(H5F_t * f,
                                          hid_t dxpl_id,
                                          hbool_t write_permitted);

static herr_t H5C__autoadjust__ageout(H5F_t * f,
                                      hid_t dxpl_id,
                                      double hit_rate,
                                      enum H5C_resize_status * status_ptr,
                                      size_t * new_max_cache_size_ptr,
                                      hbool_t write_permitted);

static herr_t H5C__autoadjust__ageout__cycle_epoch_marker(H5C_t * cache_ptr);

static herr_t H5C__autoadjust__ageout__evict_aged_out_entries(H5F_t * f,
                                                       hid_t dxpl_id,
                                                       hbool_t write_permitted);

static herr_t H5C__autoadjust__ageout__insert_new_marker(H5C_t * cache_ptr);

static herr_t H5C__autoadjust__ageout__remove_all_markers(H5C_t * cache_ptr);

static herr_t H5C__autoadjust__ageout__remove_excess_markers(H5C_t * cache_ptr);

static herr_t H5C__flash_increase_cache_size(H5C_t * cache_ptr,
    size_t old_entry_size, size_t new_entry_size);

static herr_t H5C_flush_invalidate_cache(const H5F_t *  f,
                                          hid_t    dxpl_id,
			                  unsigned flags);

static herr_t H5C_flush_invalidate_ring(const H5F_t * f, hid_t dxpl_id,
    H5C_ring_t ring, unsigned flags);

static herr_t H5C_flush_ring(H5F_t *f, hid_t dxpl_id, H5C_ring_t ring,
    unsigned flags);

static void * H5C_load_entry(H5F_t *             f,
                             hid_t               dxpl_id,
#ifdef H5_HAVE_PARALLEL
                             hbool_t             coll_access,
#endif /* H5_HAVE_PARALLEL */
                             const H5C_class_t * type,
                             haddr_t             addr,
                             void *              udata);

static herr_t H5C__mark_flush_dep_dirty(H5C_cache_entry_t * entry);

static herr_t H5C__mark_flush_dep_clean(H5C_cache_entry_t * entry);

static herr_t H5C__verify_len_eoa(H5F_t *f, const H5C_class_t * type,
    haddr_t addr, size_t *len, hbool_t actual);

static herr_t H5C__generate_image(const H5F_t *f, H5C_t * cache_ptr, H5C_cache_entry_t *entry_ptr, 
                                  hid_t dxpl_id);

#if H5C_DO_SLIST_SANITY_CHECKS
static hbool_t H5C_entry_in_skip_list(H5C_t * cache_ptr, 
                                      H5C_cache_entry_t *target_ptr);
#endif /* H5C_DO_SLIST_SANITY_CHECKS */

#if H5C_DO_EXTREME_SANITY_CHECKS
static herr_t H5C_validate_lru_list(H5C_t * cache_ptr);
static herr_t H5C_validate_pinned_entry_list(H5C_t * cache_ptr);
static herr_t H5C_validate_protected_entry_list(H5C_t * cache_ptr);
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

#ifndef NDEBUG
static void H5C__assert_flush_dep_nocycle(const H5C_cache_entry_t * entry,
                                          const H5C_cache_entry_t * base_entry);
#endif /* NDEBUG */


/*********************/
/* Package Variables */
/*********************/

/* Package initialization variable */
hbool_t H5_PKG_INIT_VAR = FALSE;

/* The H5C__class_mem_types array exists to allow lookups from 
 * class ID to the associated mem type.  This is needed 
 * to support writes of dirty prefetched entries with the 
 * correct mem type.
 *				JRM -- 8/14/15
 */
const H5FD_mem_t H5C__class_mem_types[H5C__MAX_NUM_TYPE_IDS + 1] = 
{
   /*  0 H5AC_BT_ID               */ H5FD_MEM_BTREE,
   /*  1 H5AC_SNODE_ID            */ H5FD_MEM_BTREE,
   /*  2 H5AC_LHEAP_PRFX_ID       */ H5FD_MEM_LHEAP,
   /*  3 H5AC_LHEAP_DBLK_ID       */ H5FD_MEM_LHEAP,
   /*  4 H5AC_GHEAP_ID            */ H5FD_MEM_GHEAP,
   /*  5 H5AC_OHDR_ID             */ H5FD_MEM_OHDR,
   /*  6 H5AC_OHDR_CHK_ID         */ H5FD_MEM_OHDR,
   /*  7 H5AC_BT2_HDR_ID          */ H5FD_MEM_BTREE,
   /*  8 H5AC_BT2_INT_ID          */ H5FD_MEM_BTREE,
   /*  9 H5AC_BT2_LEAF_ID         */ H5FD_MEM_BTREE,
   /* 10 H5AC_FHEAP_HDR_ID        */ H5FD_MEM_FHEAP_HDR,
   /* 11 H5AC_FHEAP_DBLOCK_ID     */ H5FD_MEM_FHEAP_DBLOCK,
   /* 12 H5AC_FHEAP_IBLOCK_ID     */ H5FD_MEM_FHEAP_IBLOCK,
   /* 13 H5AC_FSPACE_HDR_ID       */ H5FD_MEM_FSPACE_HDR,
   /* 14 H5AC_FSPACE_SINFO_ID     */ H5FD_MEM_FSPACE_SINFO,
   /* 15 H5AC_SOHM_TABLE_ID       */ H5FD_MEM_SOHM_TABLE,
   /* 16 H5AC_SOHM_LIST_ID        */ H5FD_MEM_SOHM_TABLE,
   /* 17 H5AC_EARRAY_HDR_ID       */ H5FD_MEM_EARRAY_HDR,
   /* 18 H5AC_EARRAY_IBLOCK_ID    */ H5FD_MEM_EARRAY_IBLOCK,
   /* 19 H5AC_EARRAY_SBLOCK_ID    */ H5FD_MEM_EARRAY_SBLOCK,
   /* 20 H5AC_EARRAY_DBLOCK_ID    */ H5FD_MEM_EARRAY_DBLOCK,
   /* 21 H5AC_EARRAY_DBLK_PAGE_ID */ H5FD_MEM_EARRAY_DBLK_PAGE,
   /* 22 H5AC_FARRAY_HDR_ID       */ H5FD_MEM_FARRAY_HDR,
   /* 23 H5AC_FARRAY_DBLOCK_ID    */ H5FD_MEM_FARRAY_DBLOCK,
   /* 24 H5AC_FARRAY_DBLK_PAGE_ID */ H5FD_MEM_FARRAY_DBLK_PAGE,
   /* 25 H5AC_SUPERBLOCK_ID       */ H5FD_MEM_SUPER,
   /* 26 H5AC_DRVRINFO_ID         */ H5FD_MEM_SUPER,
   /* 27 H5AC_EPOCH_MARKER_ID     */ H5FD_MEM_DEFAULT,
   /* 28 H5AC_PROXY_ENTRY_ID      */ H5FD_MEM_SUPER,
   /* 29 H5AC_PREFETCHED_ENTRY_ID */ H5FD_MEM_DEFAULT,
   /* 30 H5AC_TEST_ID             */ H5FD_MEM_DEFAULT,
   /* 31 H5AC_NTYPES              */ H5FD_MEM_DEFAULT
};

/* Declare a free list to manage the tag info struct */
H5FL_DEFINE(H5C_tag_info_t);


/*****************************/
/* Library Private Variables */
/*****************************/


/*******************/
/* Local Variables */
/*******************/

/* Declare a free list to manage the H5C_t struct */
H5FL_DEFINE_STATIC(H5C_t);

/* Declare a free list to manage flush dependency arrays */
H5FL_BLK_DEFINE_STATIC(parent);



/*-------------------------------------------------------------------------
 * Function:    H5C_create
 *
 * Purpose:     Allocate, initialize, and return the address of a new
 *		instance of H5C_t.
 *
 *		In general, the max_cache_size parameter must be positive,
 *		and the min_clean_size parameter must lie in the closed
 *		interval [0, max_cache_size].
 *
 *		The check_write_permitted parameter must either be NULL,
 *		or point to a function of type H5C_write_permitted_func_t.
 *		If it is NULL, the cache will use the write_permitted
 *		flag to determine whether writes are permitted.
 *
 * Return:      Success:        Pointer to the new instance.
 *
 *              Failure:        NULL
 *
 * Programmer:  John Mainzer
 *              6/2/04
 *
 *-------------------------------------------------------------------------
 */
H5C_t *
H5C_create(size_t		      max_cache_size,
           size_t		      min_clean_size,
           int			      max_type_id,
           const char *		      (* type_name_table_ptr),
           H5C_write_permitted_func_t check_write_permitted,
           hbool_t		      write_permitted,
           H5C_log_flush_func_t       log_flush,
           void *                     aux_ptr)
{
    int i;
    H5C_t * cache_ptr = NULL;
    H5C_t * ret_value = NULL;      /* Return value */

    FUNC_ENTER_NOAPI(NULL)

    HDassert( max_cache_size >= H5C__MIN_MAX_CACHE_SIZE );
    HDassert( max_cache_size <= H5C__MAX_MAX_CACHE_SIZE );
    HDassert( min_clean_size <= max_cache_size );

    HDassert( max_type_id >= 0 );
    HDassert( max_type_id < H5C__MAX_NUM_TYPE_IDS );
    HDassert( type_name_table_ptr );

    for ( i = 0; i <= max_type_id; i++ ) {
        HDassert( (type_name_table_ptr)[i] );
        HDassert( HDstrlen(( type_name_table_ptr)[i]) > 0 );
    } /* end for */

    if(NULL == (cache_ptr = H5FL_CALLOC(H5C_t)))
	HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, NULL, "memory allocation failed")

    if(NULL == (cache_ptr->slist_ptr = H5SL_create(H5SL_TYPE_HADDR, NULL)))
        HGOTO_ERROR(H5E_CACHE, H5E_CANTCREATE, NULL, "can't create skip list.")

    if(NULL == (cache_ptr->tag_list = H5SL_create(H5SL_TYPE_HADDR, NULL)))
        HGOTO_ERROR(H5E_CACHE, H5E_CANTCREATE, NULL, "can't create skip list for tagged entry addresses.")

    /* If we get this far, we should succeed.  Go ahead and initialize all
     * the fields.
     */

    cache_ptr->magic 				= H5C__H5C_T_MAGIC;

    cache_ptr->flush_in_progress		= FALSE;

    cache_ptr->logging_enabled                  = FALSE;

    cache_ptr->currently_logging                = FALSE;

    cache_ptr->log_file_ptr			= NULL;

    cache_ptr->trace_file_ptr			= NULL;

    cache_ptr->aux_ptr				= aux_ptr;

    cache_ptr->max_type_id			= max_type_id;

    cache_ptr->type_name_table_ptr		= type_name_table_ptr;

    cache_ptr->max_cache_size			= max_cache_size;
    cache_ptr->min_clean_size			= min_clean_size;

    cache_ptr->check_write_permitted		= check_write_permitted;
    cache_ptr->write_permitted			= write_permitted;

    cache_ptr->log_flush			= log_flush;

    cache_ptr->evictions_enabled		= TRUE;

    cache_ptr->index_len			= 0;
    cache_ptr->index_size			= (size_t)0;
    cache_ptr->clean_index_size			= (size_t)0;
    cache_ptr->dirty_index_size			= (size_t)0;

    for(i = 0; i < H5C_RING_NTYPES; i++) {
	cache_ptr->index_ring_len[i]		= 0;
	cache_ptr->index_ring_size[i]		= (size_t)0;
	cache_ptr->clean_index_ring_size[i]	= (size_t)0;
	cache_ptr->dirty_index_ring_size[i]	= (size_t)0;

	cache_ptr->slist_ring_len[i]		= 0;
	cache_ptr->slist_ring_size[i]		= (size_t)0;
    } /* end for */

    for(i = 0; i < H5C__HASH_TABLE_LEN; i++)
        (cache_ptr->index)[i] = NULL;

    cache_ptr->il_len				= 0;
    cache_ptr->il_size				= (size_t)0;
    cache_ptr->il_head				= NULL;
    cache_ptr->il_tail				= NULL;

    /* Tagging Field Initializations */
    cache_ptr->ignore_tags                      = FALSE;

    cache_ptr->slist_changed			= FALSE;
    cache_ptr->slist_len			= 0;
    cache_ptr->slist_size			= (size_t)0;

#if H5C_DO_SANITY_CHECKS
    cache_ptr->slist_len_increase		= 0;
    cache_ptr->slist_size_increase		= 0;
#endif /* H5C_DO_SANITY_CHECKS */

    cache_ptr->entries_removed_counter		= 0;
    cache_ptr->last_entry_removed_ptr		= NULL;
    cache_ptr->entry_watched_for_removal        = NULL;

    cache_ptr->pl_len				= 0;
    cache_ptr->pl_size				= (size_t)0;
    cache_ptr->pl_head_ptr			= NULL;
    cache_ptr->pl_tail_ptr			= NULL;

    cache_ptr->pel_len				= 0;
    cache_ptr->pel_size				= (size_t)0;
    cache_ptr->pel_head_ptr			= NULL;
    cache_ptr->pel_tail_ptr			= NULL;

    cache_ptr->LRU_list_len			= 0;
    cache_ptr->LRU_list_size			= (size_t)0;
    cache_ptr->LRU_head_ptr			= NULL;
    cache_ptr->LRU_tail_ptr			= NULL;

#ifdef H5_HAVE_PARALLEL
    cache_ptr->coll_list_len			= 0;
    cache_ptr->coll_list_size			= (size_t)0;
    cache_ptr->coll_head_ptr			= NULL;
    cache_ptr->coll_tail_ptr			= NULL;
    cache_ptr->coll_write_list			= NULL;
#endif /* H5_HAVE_PARALLEL */

    cache_ptr->cLRU_list_len			= 0;
    cache_ptr->cLRU_list_size			= (size_t)0;
    cache_ptr->cLRU_head_ptr			= NULL;
    cache_ptr->cLRU_tail_ptr			= NULL;

    cache_ptr->dLRU_list_len			= 0;
    cache_ptr->dLRU_list_size			= (size_t)0;
    cache_ptr->dLRU_head_ptr			= NULL;
    cache_ptr->dLRU_tail_ptr			= NULL;

    cache_ptr->size_increase_possible		= FALSE;
    cache_ptr->flash_size_increase_possible     = FALSE;
    cache_ptr->flash_size_increase_threshold    = 0;
    cache_ptr->size_decrease_possible		= FALSE;
    cache_ptr->resize_enabled			= FALSE;
    cache_ptr->cache_full			= FALSE;
    cache_ptr->size_decreased			= FALSE;
    cache_ptr->resize_in_progress		= FALSE;

    (cache_ptr->resize_ctl).version		= H5C__CURR_AUTO_SIZE_CTL_VER;
    (cache_ptr->resize_ctl).rpt_fcn		= NULL;
    (cache_ptr->resize_ctl).set_initial_size	= FALSE;
    (cache_ptr->resize_ctl).initial_size	= H5C__DEF_AR_INIT_SIZE;
    (cache_ptr->resize_ctl).min_clean_fraction	= H5C__DEF_AR_MIN_CLEAN_FRAC;
    (cache_ptr->resize_ctl).max_size		= H5C__DEF_AR_MAX_SIZE;
    (cache_ptr->resize_ctl).min_size		= H5C__DEF_AR_MIN_SIZE;
    (cache_ptr->resize_ctl).epoch_length	= H5C__DEF_AR_EPOCH_LENGTH;

    (cache_ptr->resize_ctl).incr_mode		= H5C_incr__off;
    (cache_ptr->resize_ctl).lower_hr_threshold	= H5C__DEF_AR_LOWER_THRESHHOLD;
    (cache_ptr->resize_ctl).increment	        = H5C__DEF_AR_INCREMENT;
    (cache_ptr->resize_ctl).apply_max_increment	= TRUE;
    (cache_ptr->resize_ctl).max_increment	= H5C__DEF_AR_MAX_INCREMENT;

    (cache_ptr->resize_ctl).flash_incr_mode     = H5C_flash_incr__off;
    (cache_ptr->resize_ctl).flash_multiple      = 1.0f;
    (cache_ptr->resize_ctl).flash_threshold     = 0.25f;

    (cache_ptr->resize_ctl).decr_mode		= H5C_decr__off;
    (cache_ptr->resize_ctl).upper_hr_threshold	= H5C__DEF_AR_UPPER_THRESHHOLD;
    (cache_ptr->resize_ctl).decrement	        = H5C__DEF_AR_DECREMENT;
    (cache_ptr->resize_ctl).apply_max_decrement	= TRUE;
    (cache_ptr->resize_ctl).max_decrement	= H5C__DEF_AR_MAX_DECREMENT;
    (cache_ptr->resize_ctl).epochs_before_eviction = H5C__DEF_AR_EPCHS_B4_EVICT;
    (cache_ptr->resize_ctl).apply_empty_reserve = TRUE;
    (cache_ptr->resize_ctl).empty_reserve	= H5C__DEF_AR_EMPTY_RESERVE;

    cache_ptr->epoch_markers_active		= 0;

    /* no need to initialize the ring buffer itself */
    cache_ptr->epoch_marker_ringbuf_first	= 1;
    cache_ptr->epoch_marker_ringbuf_last	= 0;
    cache_ptr->epoch_marker_ringbuf_size	= 0;

    /* Initialize all epoch marker entries' fields to zero/FALSE/NULL */
    HDmemset(cache_ptr->epoch_markers, 0, sizeof(cache_ptr->epoch_markers));

    /* Set non-zero/FALSE/NULL fields for epoch markers */
    for ( i = 0; i < H5C__MAX_EPOCH_MARKERS; i++ )
    {
        ((cache_ptr->epoch_markers)[i]).magic		 =
					       H5C__H5C_CACHE_ENTRY_T_MAGIC;
        ((cache_ptr->epoch_markers)[i]).addr		 = (haddr_t)i;
        ((cache_ptr->epoch_markers)[i]).type		 = &H5C__epoch_marker_class;
    }

    /* Initialize cache image generation on file close related fields.
     * Initial value of image_ctl must match H5C__DEFAULT_CACHE_IMAGE_CTL
     * in H5Cprivate.h.
     */
    cache_ptr->image_ctl.version            = H5C__CURR_CACHE_IMAGE_CTL_VER;
    cache_ptr->image_ctl.generate_image     = FALSE;
    cache_ptr->image_ctl.save_resize_status = FALSE;
    cache_ptr->image_ctl.entry_ageout       = -1;
    cache_ptr->image_ctl.flags              = H5C_CI__ALL_FLAGS;

    cache_ptr->serialization_in_progress= FALSE;
    cache_ptr->close_warning_received   = FALSE;
    cache_ptr->load_image		= FALSE;
    cache_ptr->image_loaded             = FALSE;
    cache_ptr->delete_image		= FALSE;
    cache_ptr->image_addr		= HADDR_UNDEF;
    cache_ptr->image_len		= 0;
    cache_ptr->image_data_len		= 0;

    cache_ptr->entries_loaded_counter		= 0;
    cache_ptr->entries_inserted_counter		= 0;
    cache_ptr->entries_relocated_counter	= 0;
    cache_ptr->entry_fd_height_change_counter	= 0;

    cache_ptr->num_entries_in_image	= 0;
    cache_ptr->image_entries		= NULL;
    cache_ptr->image_buffer		= NULL;

    /* initialize free space manager related fields: */
    cache_ptr->rdfsm_settled		= FALSE;
    cache_ptr->mdfsm_settled		= FALSE;

    if ( H5C_reset_cache_hit_rate_stats(cache_ptr) != SUCCEED ) {

        /* this should be impossible... */
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, NULL, \
                    "H5C_reset_cache_hit_rate_stats failed.")
    }

    H5C_stats__reset(cache_ptr);

    cache_ptr->prefix[0]			= '\0';  /* empty string */

#ifndef NDEBUG
    cache_ptr->get_entry_ptr_from_addr_counter  = 0;
#endif /* NDEBUG */

    /* Set return value */
    ret_value = cache_ptr;

done:
    if(NULL == ret_value) {
        if(cache_ptr != NULL) {
            if(cache_ptr->slist_ptr != NULL)
                H5SL_close(cache_ptr->slist_ptr);

            if(cache_ptr->tag_list != NULL)
                H5SL_close(cache_ptr->tag_list);

            cache_ptr->magic = 0;
            cache_ptr = H5FL_FREE(H5C_t, cache_ptr);
        } /* end if */
    } /* end if */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_create() */


/*-------------------------------------------------------------------------
 * Function:    H5C_def_auto_resize_rpt_fcn
 *
 * Purpose:     Print results of a automatic cache resize.
 *
 *		This function should only be used where HDprintf() behaves
 *		well -- i.e. not on Windows.
 *
 * Return:      void
 *
 * Programmer:  John Mainzer
 *		10/27/04
 *
 *-------------------------------------------------------------------------
 */
void
H5C_def_auto_resize_rpt_fcn(H5C_t * cache_ptr,
#ifndef NDEBUG
                            int32_t version,
#else /* NDEBUG */
                            int32_t H5_ATTR_UNUSED version,
#endif /* NDEBUG */
                            double hit_rate,
                            enum H5C_resize_status status,
                            size_t old_max_cache_size,
                            size_t new_max_cache_size,
                            size_t old_min_clean_size,
                            size_t new_min_clean_size)
{
    HDassert( cache_ptr != NULL );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( version == H5C__CURR_AUTO_RESIZE_RPT_FCN_VER );

    switch ( status )
    {
        case in_spec:
            HDfprintf(stdout,
                      "%sAuto cache resize -- no change. (hit rate = %lf)\n",
                      cache_ptr->prefix, hit_rate);
            break;

        case increase:
            HDassert( hit_rate < (cache_ptr->resize_ctl).lower_hr_threshold );
            HDassert( old_max_cache_size < new_max_cache_size );

            HDfprintf(stdout,
                      "%sAuto cache resize -- hit rate (%lf) out of bounds low (%6.5lf).\n",
                      cache_ptr->prefix, hit_rate,
                      (cache_ptr->resize_ctl).lower_hr_threshold);

            HDfprintf(stdout,
                    "%s	cache size increased from (%Zu/%Zu) to (%Zu/%Zu).\n",
                    cache_ptr->prefix,
                    old_max_cache_size,
                    old_min_clean_size,
                    new_max_cache_size,
                    new_min_clean_size);
            break;

        case flash_increase:
            HDassert( old_max_cache_size < new_max_cache_size );

            HDfprintf(stdout,
                    "%sflash cache resize(%d) -- size threshold = %Zu.\n",
                    cache_ptr->prefix,
                    (int)((cache_ptr->resize_ctl).flash_incr_mode),
                    cache_ptr->flash_size_increase_threshold);

            HDfprintf(stdout,
                  "%s cache size increased from (%Zu/%Zu) to (%Zu/%Zu).\n",
                   cache_ptr->prefix,
                   old_max_cache_size,
                   old_min_clean_size,
                   new_max_cache_size,
                   new_min_clean_size);
                break;

        case decrease:
            HDassert( old_max_cache_size > new_max_cache_size );

            switch ( (cache_ptr->resize_ctl).decr_mode )
            {
                case H5C_decr__off:
                    HDfprintf(stdout,
                              "%sAuto cache resize -- decrease off.  HR = %lf\n",
                              cache_ptr->prefix, hit_rate);
                    break;

                case H5C_decr__threshold:
                    HDassert( hit_rate >
                              (cache_ptr->resize_ctl).upper_hr_threshold );

                    HDfprintf(stdout,
                              "%sAuto cache resize -- decrease by threshold.  HR = %lf > %6.5lf\n",
                              cache_ptr->prefix, hit_rate,
                              (cache_ptr->resize_ctl).upper_hr_threshold);

                    HDfprintf(stdout, "%sout of bounds high (%6.5lf).\n",
                              cache_ptr->prefix,
                              (cache_ptr->resize_ctl).upper_hr_threshold);
                    break;

                case H5C_decr__age_out:
                    HDfprintf(stdout,
                              "%sAuto cache resize -- decrease by ageout.  HR = %lf\n",
                              cache_ptr->prefix, hit_rate);
                    break;

                case H5C_decr__age_out_with_threshold:
                    HDassert( hit_rate >
                              (cache_ptr->resize_ctl).upper_hr_threshold );

                    HDfprintf(stdout,
                              "%sAuto cache resize -- decrease by ageout with threshold. HR = %lf > %6.5lf\n",
                              cache_ptr->prefix, hit_rate,
                              (cache_ptr->resize_ctl).upper_hr_threshold);
                    break;

                default:
                    HDfprintf(stdout,
                              "%sAuto cache resize -- decrease by unknown mode.  HR = %lf\n",
                              cache_ptr->prefix, hit_rate);
            }

            HDfprintf(stdout,
                      "%s	cache size decreased from (%Zu/%Zu) to (%Zu/%Zu).\n",
                      cache_ptr->prefix,
                      old_max_cache_size,
                      old_min_clean_size,
                      new_max_cache_size,
                      new_min_clean_size);
            break;

        case at_max_size:
            HDfprintf(stdout,
                      "%sAuto cache resize -- hit rate (%lf) out of bounds low (%6.5lf).\n",
                      cache_ptr->prefix, hit_rate,
                      (cache_ptr->resize_ctl).lower_hr_threshold);
            HDfprintf(stdout,
                      "%s	cache already at maximum size so no change.\n",
                      cache_ptr->prefix);
            break;

        case at_min_size:
            HDfprintf(stdout,
                      "%sAuto cache resize -- hit rate (%lf) -- can't decrease.\n",
                      cache_ptr->prefix, hit_rate);
            HDfprintf(stdout, "%s	cache already at minimum size.\n",
                      cache_ptr->prefix);
            break;

        case increase_disabled:
            HDfprintf(stdout,
                      "%sAuto cache resize -- increase disabled -- HR = %lf.",
                      cache_ptr->prefix, hit_rate);
            break;

        case decrease_disabled:
            HDfprintf(stdout,
                      "%sAuto cache resize -- decrease disabled -- HR = %lf.\n",
                      cache_ptr->prefix, hit_rate);
            break;

        case not_full:
            HDassert( hit_rate < (cache_ptr->resize_ctl).lower_hr_threshold );

            HDfprintf(stdout,
                      "%sAuto cache resize -- hit rate (%lf) out of bounds low (%6.5lf).\n",
                      cache_ptr->prefix, hit_rate,
                      (cache_ptr->resize_ctl).lower_hr_threshold);
            HDfprintf(stdout,
                      "%s	cache not full so no increase in size.\n",
                      cache_ptr->prefix);
            break;

        default:
            HDfprintf(stdout, "%sAuto cache resize -- unknown status code.\n",
                      cache_ptr->prefix);
            break;
    }

    return;

} /* H5C_def_auto_resize_rpt_fcn() */


/*-------------------------------------------------------------------------
 * Function:    H5C_free_tag_list_cb
 *
 * Purpose:     Callback function to free tag nodes from the skip list.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *		January 2014
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C_free_tag_list_cb(void *_item, void H5_ATTR_UNUSED *key, void H5_ATTR_UNUSED *op_data)
{
    H5C_tag_info_t *tag_info = (H5C_tag_info_t *)_item;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    HDassert(tag_info);

    /* Release the item */
    tag_info = H5FL_FREE(H5C_tag_info_t, tag_info);

    FUNC_LEAVE_NOAPI(0)
}  /* H5C_free_tag_list_cb() */


/*-------------------------------------------------------------------------
 * Function:    H5C_dest
 *
 * Purpose:     Flush all data to disk and destroy the cache.
 *
 *              This function fails if any object are protected since the
 *              resulting file might not be consistent.
 *
 *		The primary_dxpl_id and secondary_dxpl_id parameters
 *		specify the dxpl_ids used on the first write occasioned
 *		by the destroy (primary_dxpl_id), and on all subsequent
 *		writes (secondary_dxpl_id).  This is useful in the metadata
 *		cache, but may not be needed elsewhere.  If so, just use the
 *		same dxpl_id for both parameters.
 *
 *		Note that *cache_ptr has been freed upon successful return.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *		6/2/04
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_dest(H5F_t * f, hid_t dxpl_id)
{
    H5C_t * cache_ptr = f->shared->cache;
    herr_t ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity check */
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(cache_ptr->close_warning_received);

#if H5AC_DUMP_IMAGE_STATS_ON_CLOSE
    if(H5C_image_stats(cache_ptr, TRUE) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Can't display cache image stats")
#endif /* H5AC_DUMP_IMAGE_STATS_ON_CLOSE */

    /* Flush and invalidate all cache entries */
    if(H5C_flush_invalidate_cache(f, dxpl_id, H5C__NO_FLAGS_SET) < 0 )
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to flush cache")

    if(cache_ptr->close_warning_received && cache_ptr->image_ctl.generate_image) {
	/* construct cache image */
        if(H5C_construct_cache_image_buffer(f, cache_ptr) < 0)
	    HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Can't create metadata cache image")

	/* free image entries array */
	if(H5C_free_image_entries_array(cache_ptr) < 0)
	    HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Can't free image entries array")

	/* write cache image block if so configured */
	if(cache_ptr->image_ctl.flags & H5C_CI__GEN_MDC_IMAGE_BLK) {
	    if(H5AC_write_cache_image(f, dxpl_id, cache_ptr->image_addr, cache_ptr->image_len, cache_ptr->image_buffer) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Can't write metadata cache image block to file")

	    H5C__UPDATE_STATS_FOR_CACHE_IMAGE_CREATE(cache_ptr);
	} /* end if */

	/* free cache image buffer */
        HDassert(cache_ptr->image_buffer);
        cache_ptr->image_buffer = H5MM_xfree(cache_ptr->image_buffer);
    } /* end if */

    if(cache_ptr->slist_ptr != NULL) {
        H5SL_close(cache_ptr->slist_ptr);
        cache_ptr->slist_ptr = NULL;
    } /* end if */

    if(cache_ptr->tag_list != NULL) {
        H5SL_destroy(cache_ptr->tag_list, H5C_free_tag_list_cb, NULL);
        cache_ptr->tag_list = NULL;
    } /* end if */

#ifndef NDEBUG
#if H5C_DO_SANITY_CHECKS
    if(cache_ptr->get_entry_ptr_from_addr_counter > 0)
        HDfprintf(stdout, "*** %ld calls to H5C_get_entry_ptr_from_add(). ***\n",
                cache_ptr->get_entry_ptr_from_addr_counter);
#endif /* H5C_DO_SANITY_CHECKS */

    cache_ptr->magic = 0;
#endif /* NDEBUG */

    cache_ptr = H5FL_FREE(H5C_t, cache_ptr);

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_dest() */


/*-------------------------------------------------------------------------
 * Function:    H5C_evict
 *
 * Purpose:     Evict all except pinned entries in the cache
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Vailin Choi
 *		Dec 2013
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_evict(H5F_t * f, hid_t dxpl_id)
{
    H5C_t *cache_ptr = f->shared->cache;
    herr_t ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity check */
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);

    /* Flush and invalidate all cache entries except the pinned entries */
    if(H5C_flush_invalidate_cache(f, dxpl_id, H5C__EVICT_ALLOW_LAST_PINS_FLAG) < 0 )
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to evict entries in the cache")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_evict() */


/*-------------------------------------------------------------------------
 * Function:    H5C_expunge_entry
 *
 * Purpose:     Use this function to tell the cache to expunge an entry
 * 		from the cache without writing it to disk even if it is
 * 		dirty.  The entry may not be either pinned or protected.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              6/29/06
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_expunge_entry(H5F_t *f, hid_t dxpl_id, const H5C_class_t *type,
    haddr_t addr, unsigned flags)
{
    H5C_t *		cache_ptr;
    H5C_cache_entry_t *	entry_ptr = NULL;
    unsigned            flush_flags = (H5C__FLUSH_INVALIDATE_FLAG | H5C__FLUSH_CLEAR_ONLY_FLAG);
    herr_t		ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f);
    HDassert(f->shared);
    cache_ptr = f->shared->cache;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(type);
    HDassert(H5F_addr_defined(addr));

#if H5C_DO_EXTREME_SANITY_CHECKS
    if(H5C_validate_lru_list(cache_ptr) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "LRU extreme sanity check failed on entry.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    /* Look for entry in cache */
    H5C__SEARCH_INDEX(cache_ptr, addr, entry_ptr, FAIL)
    if((entry_ptr == NULL) || (entry_ptr->type != type))
        /* the target doesn't exist in the cache, so we are done. */
        HGOTO_DONE(SUCCEED)

    HDassert(entry_ptr->addr == addr);
    HDassert(entry_ptr->type == type);

    /* Check for entry being pinned or protected */
    if(entry_ptr->is_protected)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTEXPUNGE, FAIL, "Target entry is protected.")
    if(entry_ptr->is_pinned)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTEXPUNGE, FAIL, "Target entry is pinned.")
#ifdef H5_HAVE_PARALLEL
    if(entry_ptr->coll_access) {
        entry_ptr->coll_access = FALSE;
        H5C__REMOVE_FROM_COLL_LIST(cache_ptr, entry_ptr, FAIL)
    } /* end if */
#endif /* H5_HAVE_PARALLEL */

    /* If we get this far, call H5C__flush_single_entry() with the
     * H5C__FLUSH_INVALIDATE_FLAG and the H5C__FLUSH_CLEAR_ONLY_FLAG.
     * This will clear the entry, and then delete it from the cache.
     */

    /* Pass along 'free file space' flag */
    flush_flags |= (flags & H5C__FREE_FILE_SPACE_FLAG);

    /* Delete the entry from the skip list on destroy */
    flush_flags |= H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG;

    if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, flush_flags) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTEXPUNGE, FAIL, "can't flush entry")

done:
#if H5C_DO_EXTREME_SANITY_CHECKS
    if(H5C_validate_lru_list(cache_ptr) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "LRU extreme sanity check failed on exit.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_expunge_entry() */


/*-------------------------------------------------------------------------
 * Function:    H5C_flush_cache
 *
 * Purpose:	Flush (and possibly destroy) the entries contained in the
 *		specified cache.
 *
 *		If the cache contains protected entries, the function will
 *		fail, as protected entries cannot be flushed.  However
 *		all unprotected entries should be flushed before the
 *		function returns failure.
 *
 * Return:      Non-negative on success/Negative on failure or if there was
 *		a request to flush all items and something was protected.
 *
 * Programmer:  John Mainzer
 *		6/2/04
 *
 * Changes:	Modified function to test for slist chamges in 
 *		pre_serialize and serialize callbacks, and re-start
 *		scans through the slist when such changes occur.
 *
 *		This has been a potential problem for some time,
 *		and there has been code in this function to deal 
 *		with elements of this issue.  However the shift 
 *		to the V3 cache in combination with the activities
 *		of some of the cache clients (in particular the 
 *		free space manager and the fractal heap) have
 *		made this re-work necessary.
 *
 *						JRM -- 12/13/14
 *
 *		Modified function to support rings.  Basic idea is that 
 *		every entry in the cache is assigned to a ring.  Entries
 *		in the outermost ring are flushed first, followed by 
 *		those in the next outermost ring, and so on until the 
 *		innermost ring is flushed.  See header comment on 
 *		H5C_ring_t in H5Cprivate.h for a more detailed 
 *		discussion.
 *
 *						JRM -- 8/30/15
 *
 *		Modified function to call the free space manager 
 *		settling functions.
 *						JRM -- 6/9/16
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_flush_cache(H5F_t *f, hid_t dxpl_id, unsigned flags)
{
#if H5C_DO_SANITY_CHECKS
    int			i;
    int32_t		index_len = 0;
    size_t		index_size = (size_t)0;
    size_t		clean_index_size = (size_t)0;
    size_t		dirty_index_size = (size_t)0;
    size_t		slist_size = (size_t)0;
    int32_t		slist_len = 0;
#endif /* H5C_DO_SANITY_CHECKS */
    H5C_ring_t		ring;
    H5C_t             * cache_ptr;
    hbool_t             destroy;
    hbool_t		ignore_protected;
    herr_t		ret_value = SUCCEED;

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f);
    HDassert(f->shared);
    cache_ptr = f->shared->cache;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(cache_ptr->slist_ptr);

#if H5C_DO_SANITY_CHECKS
    HDassert(cache_ptr->index_ring_len[H5C_RING_UNDEFINED] == 0);
    HDassert(cache_ptr->index_ring_size[H5C_RING_UNDEFINED] == (size_t)0);
    HDassert(cache_ptr->clean_index_ring_size[H5C_RING_UNDEFINED] == (size_t)0);
    HDassert(cache_ptr->dirty_index_ring_size[H5C_RING_UNDEFINED] == (size_t)0);
    HDassert(cache_ptr->slist_ring_len[H5C_RING_UNDEFINED] == 0);
    HDassert(cache_ptr->slist_ring_size[H5C_RING_UNDEFINED] == (size_t)0);

    for(i = H5C_RING_USER; i < H5C_RING_NTYPES; i++) {
        index_len += cache_ptr->index_ring_len[i];
        index_size += cache_ptr->index_ring_size[i];
        clean_index_size += cache_ptr->clean_index_ring_size[i];
        dirty_index_size += cache_ptr->dirty_index_ring_size[i];

	slist_len += cache_ptr->slist_ring_len[i];
        slist_size += cache_ptr->slist_ring_size[i];
    } /* end for */

    HDassert(cache_ptr->index_len == index_len);
    HDassert(cache_ptr->index_size == index_size);
    HDassert(cache_ptr->clean_index_size == clean_index_size);
    HDassert(cache_ptr->dirty_index_size == dirty_index_size);
    HDassert(cache_ptr->slist_len == slist_len);
    HDassert(cache_ptr->slist_size == slist_size);
#endif /* H5C_DO_SANITY_CHECKS */

#if H5C_DO_EXTREME_SANITY_CHECKS
    if((H5C_validate_protected_entry_list(cache_ptr) < 0) ||
            (H5C_validate_pinned_entry_list(cache_ptr) < 0) ||
            (H5C_validate_lru_list(cache_ptr) < 0))
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "an extreme sanity check failed on entry.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    ignore_protected = ( (flags & H5C__FLUSH_IGNORE_PROTECTED_FLAG) != 0 );
    destroy = ( (flags & H5C__FLUSH_INVALIDATE_FLAG) != 0 );
    HDassert( ! ( destroy && ignore_protected ) );
    HDassert( ! ( cache_ptr->flush_in_progress ) );

    cache_ptr->flush_in_progress = TRUE;

    if(destroy) {
        if(H5C_flush_invalidate_cache(f, dxpl_id, flags) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "flush invalidate failed.")
    } /* end if */
    else {
	/* flush each ring, starting from the outermost ring and 
         * working inward.
         */
        ring = H5C_RING_USER;
	while(ring < H5C_RING_NTYPES) {
	    /* only call the free space manager settle routines when close
             * warning has been received, and then only when the index is 
             * non-empty for that ring.
             *
             * Observe that the FSM rings should only be populated if 
             * persistant free space managers are enabled.
             */
	    if(cache_ptr->close_warning_received) {
		switch(ring) {
		    case H5C_RING_USER:
			break;

		    case H5C_RING_RDFSM:
			/* if the cache is clean through the FSM rings,
                         * then the FSMs are clean and can't change -- 
                         * no need to run the settle routine.
                         */
			if((f->shared->fs_strategy == H5F_FILE_SPACE_ALL_PERSIST)
                                && (!cache_ptr->rdfsm_settled)) {
			    if(H5MF_settle_raw_data_fsm(f, dxpl_id) < 0)
                                HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "RD FSM settle failed")

			    cache_ptr->rdfsm_settled = TRUE;
                        } /* end if */
			break;

		    case H5C_RING_MDFSM:
			/* if the cache is clean through the FSM rings,
                         * then the FSMs are clean and can't change -- 
                         * no need to run the settle routine.
                         */
			if((f->shared->fs_strategy == H5F_FILE_SPACE_ALL_PERSIST)
                                && (!cache_ptr->mdfsm_settled)) {
			    if(H5MF_settle_meta_data_fsm(f, dxpl_id) < 0)
                                HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "MD FSM settle failed")

			    cache_ptr->mdfsm_settled = TRUE;
                        } /* end if */
			break;

		    case H5C_RING_SBE:
			break;

		    case H5C_RING_SB:
			break;

		    default:
                        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Unknown ring?!?!")
			break;
		} /* end switch */
            } /* end if */

	    if(H5C_flush_ring(f, dxpl_id, ring, flags) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "flush ring failed.")
            ring++;
        } /* end while */
    } /* end else */

done:
    cache_ptr->flush_in_progress = FALSE;

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_flush_cache() */


/*-------------------------------------------------------------------------
 * Function:    H5C_flush_to_min_clean
 *
 * Purpose:	Flush dirty entries until the caches min clean size is
 *		attained.
 *
 *		This function is used in the implementation of the
 *		metadata cache in PHDF5.  To avoid "messages from the
 *		future", the cache on process 0 can't be allowed to
 *		flush entries until the other processes have reached
 *		the same point in the calculation.  If this constraint
 *		is not met, it is possible that the other processes will
 *		read metadata generated at a future point in the
 *		computation.
 *
 *
 * Return:      Non-negative on success/Negative on failure or if
 *		write is not permitted.
 *
 * Programmer:  John Mainzer
 *		9/16/05
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_flush_to_min_clean(H5F_t * f,
		       hid_t    dxpl_id)
{
    H5C_t *             cache_ptr;
    herr_t      	result;
    hbool_t		write_permitted;
#if 0 /* modified code -- commented out for now */ /* JRM */
    int			i;
    int			flushed_entries_count = 0;
    size_t		flushed_entries_size = 0;
    size_t		space_needed = 0;
    haddr_t	      * flushed_entries_list = NULL;
    H5C_cache_entry_t *	entry_ptr = NULL;
#endif /* JRM */
    herr_t		ret_value = SUCCEED;

    FUNC_ENTER_NOAPI(FAIL)

    HDassert( f );
    HDassert( f->shared );

    cache_ptr = f->shared->cache;

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    if ( cache_ptr->check_write_permitted != NULL ) {

        result = (cache_ptr->check_write_permitted)(f, &write_permitted);

        if ( result < 0 ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                        "Can't get write_permitted")
        }
    } else {

        write_permitted = cache_ptr->write_permitted;
    }

    if ( ! write_permitted ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "cache write is not permitted!?!\n");
    }
#if 1 /* original code */
    result = H5C_make_space_in_cache(f,
                                     dxpl_id,
                                     (size_t)0,
                                     write_permitted);

    if ( result < 0 ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "H5C_make_space_in_cache failed.")
    }
#else /* modified code -- commented out for now */
    if ( cache_ptr->max_cache_size > cache_ptr->index_size ) {

        if ( ((cache_ptr->max_cache_size - cache_ptr->index_size) +
               cache_ptr->cLRU_list_size) >= cache_ptr->min_clean_size ) {

            space_needed = 0;

        } else {

            space_needed = cache_ptr->min_clean_size -
                ((cache_ptr->max_cache_size - cache_ptr->index_size) +
                 cache_ptr->cLRU_list_size);
        }
    } else {

        if ( cache_ptr->min_clean_size <= cache_ptr->cLRU_list_size ) {

           space_needed = 0;

        } else {

            space_needed = cache_ptr->min_clean_size -
                           cache_ptr->cLRU_list_size;
        }
    }

    if ( space_needed > 0 ) { /* we have work to do */

        HDassert( cache_ptr->slist_len > 0 );

        /* allocate an array to keep a list of the entries that we
         * mark for flush.  We need this list to touch up the LRU
         * list after the flush.
         */
        flushed_entries_list = (haddr_t *)H5MM_malloc(sizeof(haddr_t) *
                                              (size_t)(cache_ptr->slist_len));

        if ( flushed_entries_list == NULL ) {

            HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, \
                        "memory allocation failed for flushed entries list")
        }

        /* Scan the dirty LRU list from tail forward and mark sufficient
         * entries to free up the necessary space.  Keep a list of the
         * entries marked in the order in which they are encountered.
         */
        entry_ptr = cache_ptr->dLRU_tail_ptr;

        while ( ( flushed_entries_size < space_needed ) &&
                ( flushed_entries_count < cache_ptr->slist_len ) &&
                ( entry_ptr != NULL ) )
        {
            HDassert( ! (entry_ptr->is_protected) );
            HDassert( ! (entry_ptr->is_read_only) );
            HDassert( entry_ptr->ro_ref_count == 0 );
            HDassert( entry_ptr->is_dirty );
            HDassert( entry_ptr->in_slist );

            entry_ptr->flush_marker = TRUE;
            flushed_entries_size += entry_ptr->size;
            flushed_entries_list[flushed_entries_count] = entry_ptr->addr;
            flushed_entries_count++;
            entry_ptr = entry_ptr->aux_prev;
        }

        HDassert( flushed_entries_count <= cache_ptr->slist_len );
        HDassert( flushed_entries_size >= space_needed );


        /* Flush the marked entries */
	result = H5C_flush_cache(f, primary_dxpl_id, secondary_dxpl_id,
                                 H5C__FLUSH_MARKED_ENTRIES_FLAG | H5C__FLUSH_IGNORE_PROTECTED_FLAG);

        if ( result < 0 ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "H5C_flush_cache failed.")
        }

        /* Now touch up the LRU list so as to place the flushed entries in
         * the order they they would be in if we had flushed them in the
         * order we encountered them in.
         */

        i = 0;
        while ( i < flushed_entries_count )
        {
            H5C__SEARCH_INDEX_NO_STATS(cache_ptr, flushed_entries_list[i], \
                                       entry_ptr, FAIL)

	    /* At present, the above search must always succeed.  However,
             * that may change.  Write the code so we need only remove the
             * following assert in that event.
             */
            HDassert( entry_ptr != NULL );
            H5C__FAKE_RP_FOR_MOST_RECENT_ACCESS(cache_ptr, entry_ptr, FAIL)
            i++;
        }
    } /* if ( space_needed > 0 ) */
#endif /* end modified code -- commented out for now */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_flush_to_min_clean() */


/*-------------------------------------------------------------------------
 * Function:    H5C_insert_entry
 *
 * Purpose:     Adds the specified thing to the cache.  The thing need not
 *              exist on disk yet, but it must have an address and disk
 *              space reserved.
 *
 *		The primary_dxpl_id and secondary_dxpl_id parameters
 *		specify the dxpl_ids used on the first write occasioned
 *		by the insertion (primary_dxpl_id), and on all subsequent
 *		writes (secondary_dxpl_id).  This is useful in the
 *		metadata cache, but may not be needed elsewhere.  If so,
 *		just use the same dxpl_id for both parameters.
 *
 *		The primary_dxpl_id is the dxpl_id passed to the
 *		check_write_permitted function if such a function has been
 *		provided.
 *
 *		Observe that this function cannot occasion a read.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *		6/2/04
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_insert_entry(H5F_t *             f,
                 hid_t		     dxpl_id,
                 const H5C_class_t * type,
                 haddr_t 	     addr,
                 void *		     thing,
                 unsigned int        flags)
{
    H5C_t               *cache_ptr;
    H5P_genplist_t      *dxpl;
    H5AC_ring_t         ring = H5C_RING_UNDEFINED;
    hbool_t		insert_pinned;
    hbool_t             flush_last;
#ifdef H5_HAVE_PARALLEL
    hbool_t             coll_access = FALSE; /* whether access to the cache entry is done collectively */
#endif /* H5_HAVE_PARALLEL */
    hbool_t             set_flush_marker;
    hbool_t		write_permitted = TRUE;
    size_t		empty_space;
    H5C_cache_entry_t  *entry_ptr;
    H5C_cache_entry_t  *test_entry_ptr;
    herr_t		ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert( f );
    HDassert( f->shared );

    cache_ptr = f->shared->cache;

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( type );
    HDassert( ( type->flags & H5C__CLASS_SKIP_MEM_TYPE_CHECKS ) ||
              ( type->mem_type == H5C__class_mem_types[type->id] ) );
    HDassert( type->image_len );
    HDassert( H5F_addr_defined(addr) );
    HDassert( thing );

#if H5C_DO_EXTREME_SANITY_CHECKS
    /* no need to verify that entry is not already in the index as */
    /* we already make that check below.                           */

    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on entry.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    set_flush_marker   = ( (flags & H5C__SET_FLUSH_MARKER_FLAG) != 0 );
    insert_pinned      = ( (flags & H5C__PIN_ENTRY_FLAG) != 0 );
    flush_last         = ( (flags & H5C__FLUSH_LAST_FLAG) != 0 );

    /* Get the dataset transfer property list */
    if(NULL == (dxpl = (H5P_genplist_t *)H5I_object_verify(dxpl_id, H5I_GENPROP_LST)))
        HGOTO_ERROR(H5E_CACHE, H5E_BADTYPE, FAIL, "not a property list")

    /* Get the ring type from the DXPL */
    if((H5P_get(dxpl, H5AC_RING_NAME, &ring)) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTGET, FAIL, "unable to query ring value")

    entry_ptr = (H5C_cache_entry_t *)thing;

    /* verify that the new entry isn't already in the hash table -- scream
     * and die if it is.
     */

    H5C__SEARCH_INDEX(cache_ptr, addr, test_entry_ptr, FAIL)

    if(test_entry_ptr != NULL) {
        if(test_entry_ptr == entry_ptr)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTINS, FAIL, "entry already in cache.")
        else
            HGOTO_ERROR(H5E_CACHE, H5E_CANTINS, FAIL, "duplicate entry in cache.")
    } /* end if */

    entry_ptr->magic = H5C__H5C_CACHE_ENTRY_T_MAGIC;
    entry_ptr->cache_ptr = cache_ptr;
    entry_ptr->addr  = addr;
    entry_ptr->type  = type;

    entry_ptr->image_ptr = NULL;
    entry_ptr->image_up_to_date = FALSE;

    entry_ptr->is_protected = FALSE;
    entry_ptr->is_read_only = FALSE;
    entry_ptr->ro_ref_count = 0;

    entry_ptr->is_pinned = insert_pinned;
    entry_ptr->pinned_from_client = insert_pinned;
    entry_ptr->pinned_from_cache = FALSE;
    entry_ptr->flush_me_last = flush_last;

    /* newly inserted entries are assumed to be dirty */
    entry_ptr->is_dirty = TRUE;

    /* not protected, so can't be dirtied */
    entry_ptr->dirtied  = FALSE;

    /* Retrieve the size of the thing */
    if((type->image_len)(thing, &(entry_ptr->size)) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTGETSIZE, FAIL, "can't get size of thing")
    HDassert(entry_ptr->size > 0 &&  entry_ptr->size < H5C_MAX_ENTRY_SIZE);

    entry_ptr->in_slist = FALSE;

#ifdef H5_HAVE_PARALLEL
    entry_ptr->clear_on_unprotect = FALSE;
    entry_ptr->flush_immediately = FALSE;
#endif /* H5_HAVE_PARALLEL */

    entry_ptr->flush_in_progress = FALSE;
    entry_ptr->destroy_in_progress = FALSE;

    entry_ptr->ring = ring;

    /* Initialize flush dependency fields */
    entry_ptr->flush_dep_parent             = NULL;
    entry_ptr->flush_dep_nparents           = 0;
    entry_ptr->flush_dep_parent_nalloc      = 0;
    entry_ptr->flush_dep_nchildren          = 0;
    entry_ptr->flush_dep_ndirty_children    = 0;
    entry_ptr->flush_dep_nunser_children    = 0;

    entry_ptr->ht_next = NULL;
    entry_ptr->ht_prev = NULL;
    entry_ptr->il_next = NULL;
    entry_ptr->il_prev = NULL;

    entry_ptr->next = NULL;
    entry_ptr->prev = NULL;

    entry_ptr->aux_next = NULL;
    entry_ptr->aux_prev = NULL;

    /* initialize cache image related fields */
    entry_ptr->include_in_image 		= FALSE;
    entry_ptr->lru_rank         		= 0;
    entry_ptr->image_index			= -1;
    entry_ptr->image_dirty			= FALSE;
    entry_ptr->fd_parent_count			= 0;
    entry_ptr->fd_parent_addrs			= NULL;
    entry_ptr->fd_child_count			= 0;
    entry_ptr->fd_dirty_child_count		= 0;
    entry_ptr->image_fd_height			= 0;
    entry_ptr->prefetched			= FALSE;
    entry_ptr->prefetch_type_id			= 0;
    entry_ptr->age				= 0;
#ifndef NDEBUG  /* debugging field */
    entry_ptr->serialization_count		= 0;
#endif /* NDEBUG */

#ifdef H5_HAVE_PARALLEL
    entry_ptr->coll_next = NULL;
    entry_ptr->coll_prev = NULL;
#endif /* H5_HAVE_PARALLEL */

    /* Apply tag to newly inserted entry */
    if(H5C__tag_entry(cache_ptr, entry_ptr, dxpl_id) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTTAG, FAIL, "Cannot tag metadata entry")

    H5C__RESET_CACHE_ENTRY_STATS(entry_ptr)

    if(cache_ptr->flash_size_increase_possible &&
            (entry_ptr->size > cache_ptr->flash_size_increase_threshold))
        if(H5C__flash_increase_cache_size(cache_ptr, 0, entry_ptr->size) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTINS, FAIL, "H5C__flash_increase_cache_size failed.")

    if(cache_ptr->index_size >= cache_ptr->max_cache_size)
        empty_space = 0;
    else
        empty_space = cache_ptr->max_cache_size - cache_ptr->index_size;

    if(cache_ptr->evictions_enabled &&
         (((cache_ptr->index_size + entry_ptr->size) > cache_ptr->max_cache_size)
	   ||
	   (((empty_space + cache_ptr->clean_index_size) < cache_ptr->min_clean_size)))) {
        size_t space_needed;

	if(empty_space <= entry_ptr->size)
            cache_ptr->cache_full = TRUE;

        if(cache_ptr->check_write_permitted != NULL) {
            if((cache_ptr->check_write_permitted)(f, &write_permitted) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTINS, FAIL, "Can't get write_permitted")
        } /* end if */
        else
            write_permitted = cache_ptr->write_permitted;

        HDassert(entry_ptr->size <= H5C_MAX_ENTRY_SIZE);
        space_needed = entry_ptr->size;
        if(space_needed > cache_ptr->max_cache_size)
            space_needed = cache_ptr->max_cache_size;

        /* Note that space_needed is just the amount of space that
         * needed to insert the new entry without exceeding the cache
         * size limit.  The subsequent call to H5C_make_space_in_cache()
         * may evict the entries required to free more or less space
         * depending on conditions.  It MAY be less if the cache is
         * currently undersized, or more if the cache is oversized.
         *
         * The cache can exceed its maximum size limit via the following
         * mechanisms:
         *
         * First, it is possible for the cache to grow without
         * bound as long as entries are protected and not unprotected.
         *
         * Second, when writes are not permitted it is also possible
         * for the cache to grow without bound.
         *
         * Finally, we usually don't check to see if the cache is
         * oversized at the end of an unprotect.  As a result, it is
         * possible to have a vastly oversized cache with no protected
         * entries as long as all the protects preceed the unprotects.
         *
         * Since items 1 and 2 are not changing any time soon, I see
         * no point in worrying about the third.
         */

        if(H5C_make_space_in_cache(f, dxpl_id, space_needed, write_permitted) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTINS, FAIL, "H5C_make_space_in_cache failed.")
    }

    H5C__INSERT_IN_INDEX(cache_ptr, entry_ptr, FAIL)

    /* New entries are presumed to be dirty */
    HDassert(entry_ptr->is_dirty);
    entry_ptr->flush_marker = set_flush_marker;
    H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, FAIL)
    H5C__UPDATE_RP_FOR_INSERTION(cache_ptr, entry_ptr, FAIL)

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) )
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "an extreme sanity check failed just before done.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    /* If the entry's type has a 'notify' callback send a 'after insertion'
     * notice now that the entry is fully integrated into the cache.
     */
    if(entry_ptr->type->notify &&
            (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_AFTER_INSERT, entry_ptr) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry inserted into cache")

    H5C__UPDATE_STATS_FOR_INSERTION(cache_ptr, entry_ptr)

    cache_ptr->entries_inserted_counter++;

#ifdef H5_HAVE_PARALLEL
    /* Get the dataset transfer property list */
    if(NULL == (dxpl = (H5P_genplist_t *)H5I_object(dxpl_id)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "not a property list")

    if(H5F_HAS_FEATURE(f, H5FD_FEAT_HAS_MPI)) {
        coll_access = (H5P_USER_TRUE == f->coll_md_read ? TRUE : FALSE);

        if(!coll_access && H5P_FORCE_FALSE != f->coll_md_read) {
            H5P_coll_md_read_flag_t prop_value;

            /* Get the property value */
            if(H5P_get(dxpl, H5_COLL_MD_READ_FLAG_NAME, &prop_value) < 0)
                HGOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "Can't get collective metadata access flag")
            coll_access = (H5P_USER_TRUE == prop_value ? TRUE : FALSE);
        } /* end if */
    } /* end if */

    entry_ptr->coll_access = coll_access;
    if(coll_access) {
        H5C__INSERT_IN_COLL_LIST(cache_ptr, entry_ptr, FAIL)

        /* Make sure the size of the collective entries in the cache remain in check */
        if(H5P_USER_TRUE == f->coll_md_read) {
            if(cache_ptr->max_cache_size * 80 < cache_ptr->coll_list_size * 100) {
                if(H5C_clear_coll_entries(cache_ptr, TRUE) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "can't clear collective metadata entries")
            } /* end if */
        } /* end if */
        else {
            if(cache_ptr->max_cache_size * 40 < cache_ptr->coll_list_size * 100) {
                if(H5C_clear_coll_entries(cache_ptr, TRUE) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "can't clear collective metadata entries")
            } /* end if */
        } /* end else */
    } /* end if */
#endif

done:
#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) )
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "an extreme sanity check failed on exit.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_insert_entry() */


/*-------------------------------------------------------------------------
 * Function:    H5C_mark_entry_dirty
 *
 * Purpose:	Mark a pinned or protected entry as dirty.  The target entry
 * 		MUST be either pinned or protected, and MAY be both.
 *
 * 		In the protected case, this call is the functional
 * 		equivalent of setting the H5C__DIRTIED_FLAG on an unprotect
 * 		call.
 *
 * 		In the pinned but not protected case, if the entry is not
 * 		already dirty, the function places function marks the entry
 * 		dirty and places it on the skip list.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              5/15/06
 *
 * 		JRM -- 11/5/08
 * 		Added call to H5C__UPDATE_INDEX_FOR_ENTRY_DIRTY() to
 * 		update the new clean_index_size and dirty_index_size
 * 		fields of H5C_t in the case that the entry was clean
 * 		prior to this call, and is pinned and not protected.
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_mark_entry_dirty(void *thing)
{
    H5C_t *             cache_ptr;
    H5C_cache_entry_t * entry_ptr = (H5C_cache_entry_t *)thing;
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(entry_ptr);
    HDassert(H5F_addr_defined(entry_ptr->addr));
    cache_ptr = entry_ptr->cache_ptr;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);

    if ( entry_ptr->is_protected ) {
	HDassert( ! ((entry_ptr)->is_read_only) );

        /* set the dirtied flag */
        entry_ptr->dirtied = TRUE;

        /* reset image_up_to_date */
        if(entry_ptr->image_up_to_date) {
            entry_ptr->image_up_to_date = FALSE;

            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_unserialized(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "Can't propagate serialization status to fd parents")
        }/* end if */
    } /* end if */
    else if ( entry_ptr->is_pinned ) {
        hbool_t		was_clean;      /* Whether the entry was previously clean */
        hbool_t		image_was_up_to_date;

        /* Remember previous dirty status */
	was_clean = !entry_ptr->is_dirty;

        /* Check if image is up to date */
        image_was_up_to_date = entry_ptr->image_up_to_date;

        /* Mark the entry as dirty if it isn't already */
        entry_ptr->is_dirty = TRUE;
	entry_ptr->image_up_to_date = FALSE;

        /* Modify cache data structures */
        if(was_clean)
            H5C__UPDATE_INDEX_FOR_ENTRY_DIRTY(cache_ptr, entry_ptr)
        if(!entry_ptr->in_slist)
            H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, FAIL)

        /* Update stats for entry being marked dirty */
        H5C__UPDATE_STATS_FOR_DIRTY_PIN(cache_ptr, entry_ptr)

        /* Check for entry changing status and do notifications, etc. */
        if(was_clean) {
            /* If the entry's type has a 'notify' callback send a 'entry dirtied'
             * notice now that the entry is fully integrated into the cache.
             */
            if(entry_ptr->type->notify &&
                    (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_DIRTIED, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag set")

            /* Propagate the dirty flag up the flush dependency chain if appropriate */
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_dirty(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKDIRTY, FAIL, "Can't propagate flush dep dirty flag")
        } /* end if */

        if(image_was_up_to_date)
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_unserialized(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "Can't propagate serialization status to fd parents")

        if(!entry_ptr->in_slist)
            H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, FAIL)


        H5C__UPDATE_STATS_FOR_DIRTY_PIN(cache_ptr, entry_ptr)
    } /* end if */
    else
        HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKDIRTY, FAIL, "Entry is neither pinned nor protected??")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_mark_entry_dirty() */


/*-------------------------------------------------------------------------
 * Function:    H5C_mark_entry_clean
 *
 * Purpose:	Mark a pinned entry as clean.  The target entry MUST be pinned.
 *
 * 		If the entry is not
 * 		already clean, the function places function marks the entry
 * 		clean and removes it from the skip list.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              7/23/16
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_mark_entry_clean(void *_thing)
{
    H5C_t *             cache_ptr;
    H5C_cache_entry_t * entry_ptr = (H5C_cache_entry_t *)_thing;
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(entry_ptr);
    HDassert(H5F_addr_defined(entry_ptr->addr));
    cache_ptr = entry_ptr->cache_ptr;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);

    /* Operate on pinned entry */
    if(entry_ptr->is_protected)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKCLEAN, FAIL, "entry is protected")
    else if(entry_ptr->is_pinned) {
        hbool_t		was_dirty;      /* Whether the entry was previously dirty */

        /* Remember previous dirty status */
        was_dirty = entry_ptr->is_dirty;

        /* Mark the entry as clean if it isn't already */
        entry_ptr->is_dirty = FALSE;

        /* Also reset the 'flush_marker' flag, since the entry shouldn't be flushed now */
        entry_ptr->flush_marker = FALSE;

        /* Modify cache data structures */
        if(was_dirty)
            H5C__UPDATE_INDEX_FOR_ENTRY_CLEAN(cache_ptr, entry_ptr)
        if(entry_ptr->in_slist)
            H5C__REMOVE_ENTRY_FROM_SLIST(cache_ptr, entry_ptr, FALSE)

        /* Update stats for entry being marked clean */
        H5C__UPDATE_STATS_FOR_CLEAR(cache_ptr, entry_ptr)

        /* Check for entry changing status and do notifications, etc. */
        if(was_dirty) {
            /* If the entry's type has a 'notify' callback send a 'entry cleaned'
             * notice now that the entry is fully integrated into the cache.
             */
            if(entry_ptr->type->notify &&
                    (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_CLEANED, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag cleared")

            /* Propagate the clean up the flush dependency chain, if appropriate */
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_clean(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKCLEAN, FAIL, "Can't propagate flush dep clean")
        } /* end if */
    } /* end if */
    else
        HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKCLEAN, FAIL, "Entry is not pinned??")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_mark_entry_clean() */


/*-------------------------------------------------------------------------
 * Function:    H5C_mark_entry_unserialized
 *
 * Purpose:	Mark a pinned or protected entry as unserialized.  The target
 *		entry MUST be either pinned or protected, and MAY be both.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              12/23/16
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_mark_entry_unserialized(void *thing)
{
    H5C_cache_entry_t  *entry = (H5C_cache_entry_t *)thing;
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(entry);
    HDassert(H5F_addr_defined(entry->addr));

    if(entry->is_protected || entry->is_pinned) {
        HDassert(!entry->is_read_only);

        /* Reset image_up_to_date */
        if(entry->image_up_to_date) {
	    entry->image_up_to_date = FALSE;

            if(entry->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_unserialized(entry) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTSET, FAIL, "Can't propagate serialization status to fd parents")
        }/* end if */
    } /* end if */
    else
        HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKUNSERIALIZED, FAIL, "Entry to unserialize is neither pinned nor protected??")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_mark_entry_unserialized() */


/*-------------------------------------------------------------------------
 * Function:    H5C_mark_entry_serialized
 *
 * Purpose:	Mark a pinned entry as serialized.  The target entry MUST be
 *		pinned.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              12/23/16
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_mark_entry_serialized(void *_thing)
{
    H5C_cache_entry_t  *entry = (H5C_cache_entry_t *)_thing;
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(entry);
    HDassert(H5F_addr_defined(entry->addr));

    /* Operate on pinned entry */
    if(entry->is_protected)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKSERIALIZED, FAIL, "entry is protected")
    else if(entry->is_pinned) {
        /* Check for entry changing status and do notifications, etc. */
        if(!entry->image_up_to_date) {
	    /* Set the image_up_to_date flag */
            entry->image_up_to_date = TRUE;

            /* Propagate the serialize up the flush dependency chain, if appropriate */
            if(entry->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_serialized(entry) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKSERIALIZED, FAIL, "Can't propagate flush dep serialize")
        } /* end if */
    } /* end if */
    else
        HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKSERIALIZED, FAIL, "Entry is not pinned??")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_mark_entry_serialized() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_move_entry
 *
 * Purpose:     Use this function to notify the cache that an entry's
 *              file address changed.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              6/2/04
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_move_entry(H5C_t *	     cache_ptr,
                 const H5C_class_t * type,
                 haddr_t 	     old_addr,
	         haddr_t 	     new_addr)
{
    H5C_cache_entry_t *	entry_ptr = NULL;
    H5C_cache_entry_t *	test_entry_ptr = NULL;
    herr_t			ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(type);
    HDassert(H5F_addr_defined(old_addr));
    HDassert(H5F_addr_defined(new_addr));
    HDassert(H5F_addr_ne(old_addr, new_addr));

#if H5C_DO_EXTREME_SANITY_CHECKS
    if((H5C_validate_protected_entry_list(cache_ptr) < 0) ||
             (H5C_validate_pinned_entry_list(cache_ptr) < 0) ||
             (H5C_validate_lru_list(cache_ptr) < 0))
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "an extreme sanity check failed on entry.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    H5C__SEARCH_INDEX(cache_ptr, old_addr, entry_ptr, FAIL)

    if(entry_ptr == NULL || entry_ptr->type != type)
        /* the old item doesn't exist in the cache, so we are done. */
        HGOTO_DONE(SUCCEED)

    HDassert(entry_ptr->addr == old_addr);
    HDassert(entry_ptr->type == type);

    /* Check for R/W status, otherwise error */
    /* (Moving a R/O entry would mark it dirty, which shouldn't
     *  happen. QAK - 2016/12/02)
     */
    if(entry_ptr->is_read_only)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTMOVE, FAIL, "can't move R/O entry")

    H5C__SEARCH_INDEX(cache_ptr, new_addr, test_entry_ptr, FAIL)

    if(test_entry_ptr != NULL) { /* we are hosed */
        if(test_entry_ptr->type == type)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTMOVE, FAIL, "target already moved & reinserted???")
        else
            HGOTO_ERROR(H5E_CACHE, H5E_CANTMOVE, FAIL, "new address already in use?")
    } /* end if */

    /* If we get this far we have work to do.  Remove *entry_ptr from
     * the hash table (and skip list if necessary), change its address to the
     * new address, mark it as dirty (if it isn't already) and then re-insert.
     *
     * Update the replacement policy for a hit to avoid an eviction before
     * the moved entry is touched.  Update stats for a move.
     *
     * Note that we do not check the size of the cache, or evict anything.
     * Since this is a simple re-name, cache size should be unaffected.
     *
     * Check to see if the target entry is in the process of being destroyed
     * before we delete from the index, etc.  If it is, all we do is
     * change the addr.  If the entry is only in the process of being flushed,
     * don't mark it as dirty either, lest we confuse the flush call back.
     */
    if(!entry_ptr->destroy_in_progress) {
        H5C__DELETE_FROM_INDEX(cache_ptr, entry_ptr, FAIL)

        if(entry_ptr->in_slist) {
            HDassert(cache_ptr->slist_ptr);
            H5C__REMOVE_ENTRY_FROM_SLIST(cache_ptr, entry_ptr, FALSE)
        } /* end if */
    } /* end if */

    entry_ptr->addr = new_addr;

    if(!entry_ptr->destroy_in_progress) {
        hbool_t		was_dirty;      /* Whether the entry was previously dirty */

        /* Remember previous dirty status */
        was_dirty = entry_ptr->is_dirty;

        /* Mark the entry as dirty if it isn't already */
	entry_ptr->is_dirty = TRUE;

	/* This shouldn't be needed, but it keeps the test code happy */
        if(entry_ptr->image_up_to_date) {
            entry_ptr->image_up_to_date = FALSE;
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_unserialized(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "Can't propagate serialization status to fd parents")
        } /* end if */

        /* Modify cache data structures */
        H5C__INSERT_IN_INDEX(cache_ptr, entry_ptr, FAIL)
        H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, FAIL)

        /* Skip some actions if we're in the middle of flushing the entry */
	if(!entry_ptr->flush_in_progress) {
            /* Update the replacement policy for the entry */
            H5C__UPDATE_RP_FOR_MOVE(cache_ptr, entry_ptr, was_dirty, FAIL)

            /* Check for entry changing status and do notifications, etc. */
            if(!was_dirty) {
                /* If the entry's type has a 'notify' callback send a 'entry dirtied'
                 * notice now that the entry is fully integrated into the cache.
                 */
                if(entry_ptr->type->notify &&
                        (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_DIRTIED, entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag set")

                /* Propagate the dirty flag up the flush dependency chain if appropriate */
                if(entry_ptr->flush_dep_nparents > 0)
                    if(H5C__mark_flush_dep_dirty(entry_ptr) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKDIRTY, FAIL, "Can't propagate flush dep dirty flag")
            } /* end if */
        } /* end if */
    } /* end if */

    H5C__UPDATE_STATS_FOR_MOVE(cache_ptr, entry_ptr)

    cache_ptr->entries_relocated_counter++;

done:
#if H5C_DO_EXTREME_SANITY_CHECKS
    if((H5C_validate_protected_entry_list(cache_ptr) < 0) ||
             (H5C_validate_pinned_entry_list(cache_ptr) < 0) ||
             (H5C_validate_lru_list(cache_ptr) < 0))
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "an extreme sanity check failed on exit.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_move_entry() */


/*-------------------------------------------------------------------------
 * Function:    H5C_resize_entry
 *
 * Purpose:	Resize a pinned or protected entry.
 *
 * 		Resizing an entry dirties it, so if the entry is not
 * 		already dirty, the function places the entry on the
 * 		skip list.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              7/5/06
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_resize_entry(void *thing, size_t new_size)
{
    H5C_t             * cache_ptr;
    H5C_cache_entry_t * entry_ptr = (H5C_cache_entry_t *)thing;
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(entry_ptr);
    HDassert(H5F_addr_defined(entry_ptr->addr));
    cache_ptr = entry_ptr->cache_ptr;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);

    /* Check for usage errors */
    if(new_size <= 0)
        HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "New size is non-positive.")
    if(!(entry_ptr->is_pinned || entry_ptr->is_protected))
        HGOTO_ERROR(H5E_CACHE, H5E_BADTYPE, FAIL, "Entry isn't pinned or protected??")

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on entry.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    /* update for change in entry size if necessary */
    if ( entry_ptr->size != new_size ) {
        hbool_t		was_clean;

        /* make note of whether the entry was clean to begin with */
        was_clean = !entry_ptr->is_dirty;

        /* mark the entry as dirty if it isn't already */
        entry_ptr->is_dirty = TRUE;

        /* Reset the image up-to-date status */
        if(entry_ptr->image_up_to_date) {
            entry_ptr->image_up_to_date = FALSE;
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_unserialized(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "Can't propagate serialization status to fd parents")
        } /* end if */

        /* Release the current image */
        if(entry_ptr->image_ptr)
            entry_ptr->image_ptr = H5MM_xfree(entry_ptr->image_ptr);

        /* do a flash cache size increase if appropriate */
        if ( cache_ptr->flash_size_increase_possible ) {

            if ( new_size > entry_ptr->size ) {
                size_t             	size_increase;

                size_increase = new_size - entry_ptr->size;

                if(size_increase >= cache_ptr->flash_size_increase_threshold) {
                    if(H5C__flash_increase_cache_size(cache_ptr, entry_ptr->size, new_size) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTRESIZE, FAIL, "flash cache increase failed")
                }
            }
        }

        /* update the pinned and/or protected entry list */
        if(entry_ptr->is_pinned) {
            H5C__DLL_UPDATE_FOR_SIZE_CHANGE((cache_ptr->pel_len), \
                                            (cache_ptr->pel_size), \
                                            (entry_ptr->size), (new_size))
        } /* end if */
        if(entry_ptr->is_protected) {
            H5C__DLL_UPDATE_FOR_SIZE_CHANGE((cache_ptr->pl_len), \
                                            (cache_ptr->pl_size), \
                                            (entry_ptr->size), (new_size))
        } /* end if */

#ifdef H5_HAVE_PARALLEL
        if(entry_ptr->coll_access) {
            H5C__DLL_UPDATE_FOR_SIZE_CHANGE((cache_ptr->coll_list_len), \
                                            (cache_ptr->coll_list_size), \
                                            (entry_ptr->size), (new_size))
        } /* end if */
#endif /* H5_HAVE_PARALLEL */

        /* update statistics just before changing the entry size */
	H5C__UPDATE_STATS_FOR_ENTRY_SIZE_CHANGE(cache_ptr, entry_ptr, new_size);

        /* update the hash table */
	H5C__UPDATE_INDEX_FOR_SIZE_CHANGE(cache_ptr, entry_ptr->size, \
                                          new_size, entry_ptr, was_clean);

        /* if the entry is in the skip list, update that too */
        if(entry_ptr->in_slist)
	    H5C__UPDATE_SLIST_FOR_SIZE_CHANGE(cache_ptr, entry_ptr->size, new_size);

	/* finally, update the entry size proper */
	entry_ptr->size = new_size;

        if(!entry_ptr->in_slist)
            H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, FAIL)

        if(entry_ptr->is_pinned)
            H5C__UPDATE_STATS_FOR_DIRTY_PIN(cache_ptr, entry_ptr)

        /* Check for entry changing status and do notifications, etc. */
        if(was_clean) {
            /* If the entry's type has a 'notify' callback send a 'entry dirtied'
             * notice now that the entry is fully integrated into the cache.
             */
            if(entry_ptr->type->notify &&
                    (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_DIRTIED, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag set")

            /* Propagate the dirty flag up the flush dependency chain if appropriate */
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_dirty(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKDIRTY, FAIL, "Can't propagate flush dep dirty flag")
        } /* end if */
    } /* end if */

done:

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on exit.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_resize_entry() */


/*-------------------------------------------------------------------------
 * Function:    H5C_pin_entry_from_client()
 *
 * Purpose:	Internal routine to pin a cache entry from a client action.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              3/26/09
 *
 * Changes:	Added sanity checks to clarify the circumstances under
 *		which an entry can be pinned.   JRM -- 4/27/14 
 *
 *-------------------------------------------------------------------------
 */
#ifndef NDEBUG
static herr_t
H5C_pin_entry_from_client(H5C_t *	          cache_ptr,
                        H5C_cache_entry_t * entry_ptr)
#else
static herr_t
H5C_pin_entry_from_client(H5C_t H5_ATTR_UNUSED *	cache_ptr,
                        H5C_cache_entry_t * entry_ptr)
#endif
{
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    /* Sanity checks */
    HDassert( cache_ptr );
    HDassert( entry_ptr );
    HDassert( entry_ptr->is_protected );

    /* Check if the entry is already pinned */
    if(entry_ptr->is_pinned) {
        /* Check if the entry was pinned through an explicit pin from a client */
        if(entry_ptr->pinned_from_client)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTPIN, FAIL, "Entry is already pinned")
    } /* end if */
    else {
        entry_ptr->is_pinned = TRUE;

        H5C__UPDATE_STATS_FOR_PIN(cache_ptr, entry_ptr)
    } /* end else */

    /* Mark that the entry was pinned through an explicit pin from a client */
    entry_ptr->pinned_from_client = TRUE;

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_pin_entry_from_client() */


/*-------------------------------------------------------------------------
 * Function:    H5C_pin_protected_entry()
 *
 * Purpose:	Pin a protected cache entry.  The entry must be protected
 * 		at the time of call, and must be unpinned.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              4/26/06
 *
 * Changes:	Added extreme sanity checks on entry and exit.
 *                                          JRM -- 4/26/14
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_pin_protected_entry(void *thing)
{
    H5C_t             * cache_ptr;
    H5C_cache_entry_t * entry_ptr = (H5C_cache_entry_t *)thing; /* Pointer to entry to pin */
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(entry_ptr);
    HDassert(H5F_addr_defined(entry_ptr->addr));
    cache_ptr = entry_ptr->cache_ptr;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on entry.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */


    /* Only protected entries can be pinned */
    if(!entry_ptr->is_protected)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTPIN, FAIL, "Entry isn't protected")

    /* Pin the entry from a client */
    if(H5C_pin_entry_from_client(cache_ptr, entry_ptr) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTPIN, FAIL, "Can't pin entry by client")

done:

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on exit.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_pin_protected_entry() */


/*-------------------------------------------------------------------------
 * Function:    H5C_protect
 *
 * Purpose:     If the target entry is not in the cache, load it.  If
 *		necessary, attempt to evict one or more entries to keep
 *		the cache within its maximum size.
 *
 *		Mark the target entry as protected, and return its address
 *		to the caller.  The caller must call H5C_unprotect() when
 *		finished with the entry.
 *
 *		While it is protected, the entry may not be either evicted
 *		or flushed -- nor may it be accessed by another call to
 *		H5C_protect.  Any attempt to do so will result in a failure.
 *
 * Return:      Success:        Ptr to the desired entry
 *              Failure:        NULL
 *
 * Programmer:  John Mainzer -  6/2/04
 *
 * 		JRM -- 11/13/08
 * 		Modified function to call H5C_make_space_in_cache() when
 * 		the min_clean_size is violated, not just when there isn't
 * 		enough space for and entry that has just been loaded.
 *
 *              The purpose of this modification is to avoid "metadata
 *              blizzards" in the write only case.  In such instances,
 *              the cache was allowed to fill with dirty metadata.  When
 *              we finally needed to evict an entry to make space, we had
 *              to flush out a whole cache full of metadata -- which has
 *              interesting performance effects.  We hope to avoid (or
 *              perhaps more accurately hide) this effect by maintaining
 *              the min_clean_size, which should force us to start flushing
 *              entries long before we actually have to evict something
 *              to make space.
 *
 *		JRM -- 9/1/14
 *		Replace the old rw parameter with the flags parameter.
 *		This allows H5C_protect to accept flags other than 
 *		H5C__READ_ONLY_FLAG.  
 *
 *		Added support for the H5C__FLUSH_LAST_FLAG.
 *		At present, this flag is only applied if the entry is 
 *              not in cache, and is loaded into the cache as a result of 
 *              this call.
 *
 *		JRM -- 7/8/15
 *		Added code to call H5C_load_cache_image() if 
 *		cache_ptr->load_image is TRUE.
 *
 *		JRM -- 8/13/15
 *		Added code to manage prefetched entries.
 *
 *-------------------------------------------------------------------------
 */
void *
H5C_protect(H5F_t *		f,
            hid_t	        dxpl_id,
            const H5C_class_t * type,
            haddr_t 	        addr,
            void *              udata,
	    unsigned		flags)
{
    H5C_t *		cache_ptr;
    H5AC_ring_t         ring = H5C_RING_UNDEFINED;
    hbool_t		hit;
    hbool_t		have_write_permitted = FALSE;
    hbool_t		read_only = FALSE;
    hbool_t             flush_last;
#ifdef H5_HAVE_PARALLEL
    hbool_t             coll_access = FALSE; /* whether access to the cache entry is done collectively */
#endif /* H5_HAVE_PARALLEL */
    hbool_t		write_permitted;
    hbool_t             was_loaded = FALSE;     /* Whether the entry was loaded as a result of the protect */
    size_t		empty_space;
    void *		thing;
    H5C_cache_entry_t *	entry_ptr;
    H5P_genplist_t    * dxpl;    /* dataset transfer property list */
    void *		ret_value = NULL;       /* Return value */

    FUNC_ENTER_NOAPI(NULL)

    /* check args */
    HDassert( f );
    HDassert( f->shared );

    cache_ptr = f->shared->cache;

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( type );
    HDassert( ( type->flags & H5C__CLASS_SKIP_MEM_TYPE_CHECKS ) ||
              ( type->mem_type == H5C__class_mem_types[type->id] ) );
    HDassert( H5F_addr_defined(addr) );

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, NULL, "an extreme sanity check failed on entry.\n")
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    /* Load the cache image, if requested */
    if(cache_ptr->load_image) {
        cache_ptr->load_image = FALSE;
        if(H5C_load_cache_image(f, dxpl_id) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTLOAD, NULL, "Can't load cache image")
    } /* end if */

    read_only          = ( (flags & H5C__READ_ONLY_FLAG) != 0 );
    flush_last         = ( (flags & H5C__FLUSH_LAST_FLAG) != 0 );

    /* Get the dataset transfer property list */
    if(NULL == (dxpl = (H5P_genplist_t *)H5I_object_verify(dxpl_id, H5I_GENPROP_LST)))
        HGOTO_ERROR(H5E_CACHE, H5E_BADTYPE, NULL, "not a property list")

    /* Get the ring type from the DXPL */
    if((H5P_get(dxpl, H5AC_RING_NAME, &ring)) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTGET, NULL, "unable to query ring value")

#ifdef H5_HAVE_PARALLEL
    if(H5F_HAS_FEATURE(f, H5FD_FEAT_HAS_MPI)) {
        coll_access = (H5P_USER_TRUE == f->coll_md_read ? TRUE : FALSE);

        if(!coll_access && H5P_FORCE_FALSE != f->coll_md_read) {
            H5P_coll_md_read_flag_t prop_value;

            /* get the property value */
            if(H5P_get(dxpl, H5_COLL_MD_READ_FLAG_NAME, &prop_value) < 0)
                HGOTO_ERROR(H5E_PLIST, H5E_CANTGET, NULL, "Can't get collective metadata access flag")
            coll_access = (H5P_USER_TRUE == prop_value ? TRUE : FALSE);
        } /* end if */
    } /* end if */
#endif /* H5_HAVE_PARALLEL */

    /* first check to see if the target is in cache */
    H5C__SEARCH_INDEX(cache_ptr, addr, entry_ptr, NULL)

    if ( entry_ptr != NULL ) {
        if(entry_ptr->ring != ring)
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, NULL, "ring type mismatch occured for cache entry\n")

        HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);

        if(entry_ptr->prefetched) {
            /* This call removes the prefetched entry from the cache,
             * and replaces it with an entry deserialized from the 
             * image of the prefetched entry.
             */
            if(H5C_deserialize_prefetched_entry(f, dxpl_id, cache_ptr, &entry_ptr, type, addr, udata) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTLOAD, NULL, "can't deserialize prefetched entry.")

            HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
            HDassert(!entry_ptr->prefetched);
            HDassert(entry_ptr->addr == addr);
        } /* end if */

        /* Check for trying to load the wrong type of entry from an address */
        if(entry_ptr->type != type)
            HGOTO_ERROR(H5E_CACHE, H5E_BADTYPE, NULL, "incorrect cache entry type")

        /* if this is a collective metadata read, the entry is not
           marked as collective, and is clean, it is possible that
           other processes will not have it in its cache and will
           expect a bcast of the entry from process 0. So process 0
           will bcast the entry to all other ranks. Ranks that do have
           the entry in their cache still have to participate in the
           bcast. */
#ifdef H5_HAVE_PARALLEL
        if(H5F_HAS_FEATURE(f, H5FD_FEAT_HAS_MPI) && coll_access) {
            if(!(entry_ptr->is_dirty) && !(entry_ptr->coll_access)) {
                MPI_Comm  comm;           /* File MPI Communicator */
                int       mpi_code;       /* MPI error code */
                int       buf_size;

                if(MPI_COMM_NULL == (comm = H5F_mpi_get_comm(f)))
                    HGOTO_ERROR(H5E_FILE, H5E_CANTGET, NULL, "get_comm request failed")

                if(entry_ptr->image_ptr == NULL) {
                    int mpi_rank;

                    if((mpi_rank = H5F_mpi_get_rank(f)) < 0)
                        HGOTO_ERROR(H5E_FILE, H5E_CANTGET, NULL, "Can't get MPI rank")

                    if(NULL == (entry_ptr->image_ptr = H5MM_malloc(entry_ptr->size + H5C_IMAGE_EXTRA_SPACE)))
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTALLOC, NULL, "memory allocation failed for on disk image buffer")
#if H5C_DO_MEMORY_SANITY_CHECKS
                    HDmemcpy(((uint8_t *)entry_ptr->image_ptr) + entry_ptr->size, H5C_IMAGE_SANITY_VALUE, H5C_IMAGE_EXTRA_SPACE);
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */
                    if(0 == mpi_rank)
                        if(H5C__generate_image(f, cache_ptr, entry_ptr, dxpl_id) < 0)
                            HGOTO_ERROR(H5E_CACHE, H5E_CANTGET, NULL, "can't generate entry's image")
                } /* end if */
                HDassert(entry_ptr->image_ptr);

                H5_CHECKED_ASSIGN(buf_size, int, entry_ptr->size, size_t);
                if(MPI_SUCCESS != (mpi_code = MPI_Bcast(entry_ptr->image_ptr, buf_size, MPI_BYTE, 0, comm)))
                    HMPI_GOTO_ERROR(NULL, "MPI_Bcast failed", mpi_code)

                /* Mark the entry as collective and insert into the collective list */
                entry_ptr->coll_access = TRUE;
                H5C__INSERT_IN_COLL_LIST(cache_ptr, entry_ptr, NULL)
            } /* end if */
            else if(entry_ptr->coll_access) {
                H5C__MOVE_TO_TOP_IN_COLL_LIST(cache_ptr, entry_ptr, NULL)
            } /* end else-if */
        } /* end if */
#endif /* H5_HAVE_PARALLEL */

#if H5C_DO_TAGGING_SANITY_CHECKS
{
        /* Verify tag value */
        if(cache_ptr->ignore_tags != TRUE) {
            haddr_t tag;              /* Tag value */

            /* The entry is already in the cache, but make sure that the tag value 
               being passed in via dxpl is still legal. This will ensure that had
               the entry NOT been in the cache, tagging was still set up correctly
               and it would have received a legal tag value after getting loaded
               from disk. */

            /* Get the tag from the DXPL */
            if((H5P_get(dxpl, H5AC_TAG_NAME, &tag)) < 0)
                HGOTO_ERROR(H5E_PLIST, H5E_CANTGET, NULL, "unable to query property value")
    
            if(H5C_verify_tag(entry_ptr->type->id, tag) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTGET, NULL, "tag verification failed")
        } /* end if */
}
#endif

        hit = TRUE;
        thing = (void *)entry_ptr;

    } else {

        /* must try to load the entry from disk. */

        hit = FALSE;

        if(NULL == (thing = H5C_load_entry(f, dxpl_id, 
#ifdef H5_HAVE_PARALLEL
                                           coll_access, 
#endif /* H5_HAVE_PARALLEL */
                                           type, addr, udata)))
            HGOTO_ERROR(H5E_CACHE, H5E_CANTLOAD, NULL, "can't load entry")

        entry_ptr = (H5C_cache_entry_t *)thing;
        cache_ptr->entries_loaded_counter++;

        entry_ptr->ring  = ring;
#ifdef H5_HAVE_PARALLEL
        if(H5F_HAS_FEATURE(f, H5FD_FEAT_HAS_MPI) && entry_ptr->coll_access)
            H5C__INSERT_IN_COLL_LIST(cache_ptr, entry_ptr, NULL)
#endif /* H5_HAVE_PARALLEL */

        /* Apply tag to newly protected entry */
        if(H5C__tag_entry(cache_ptr, entry_ptr, dxpl_id) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTTAG, NULL, "Cannot tag metadata entry")

        /* If the entry is very large, and we are configured to allow it,
         * we may wish to perform a flash cache size increase.
         */
        if ( ( cache_ptr->flash_size_increase_possible ) &&
             ( entry_ptr->size > cache_ptr->flash_size_increase_threshold ) ) {

            if(H5C__flash_increase_cache_size(cache_ptr, 0, entry_ptr->size) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, NULL, "H5C__flash_increase_cache_size failed.")
        }

        if(cache_ptr->index_size >= cache_ptr->max_cache_size)
           empty_space = 0;
        else
           empty_space = cache_ptr->max_cache_size - cache_ptr->index_size;

	/* try to free up if necceary and if evictions are permitted.  Note
	 * that if evictions are enabled, we will call H5C_make_space_in_cache()
	 * regardless if the min_free_space requirement is not met.
	 */
        if ( ( cache_ptr->evictions_enabled ) &&
             ( ( (cache_ptr->index_size + entry_ptr->size) >
	         cache_ptr->max_cache_size)
	       ||
	       ( ( empty_space + cache_ptr->clean_index_size ) <
	         cache_ptr->min_clean_size )
	     )
           ) {

            size_t space_needed;

	    if(empty_space <= entry_ptr->size)
                cache_ptr->cache_full = TRUE;

            if(cache_ptr->check_write_permitted != NULL) {
                if((cache_ptr->check_write_permitted)(f, &write_permitted) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, NULL, "Can't get write_permitted 1")
                else
                    have_write_permitted = TRUE;
            } /* end if */
            else {
                write_permitted = cache_ptr->write_permitted;
                have_write_permitted = TRUE;
            } /* end else */

            HDassert(entry_ptr->size <= H5C_MAX_ENTRY_SIZE);
            space_needed = entry_ptr->size;
            if(space_needed > cache_ptr->max_cache_size)
                space_needed = cache_ptr->max_cache_size;

            /* Note that space_needed is just the amount of space that
             * needed to insert the new entry without exceeding the cache
             * size limit.  The subsequent call to H5C_make_space_in_cache()
             * may evict the entries required to free more or less space
             * depending on conditions.  It MAY be less if the cache is
             * currently undersized, or more if the cache is oversized.
             *
             * The cache can exceed its maximum size limit via the following
             * mechanisms:
             *
             * First, it is possible for the cache to grow without
             * bound as long as entries are protected and not unprotected.
             *
             * Second, when writes are not permitted it is also possible
             * for the cache to grow without bound.
	     *
	     * Third, the user may choose to disable evictions -- causing
	     * the cache to grow without bound until evictions are
	     * re-enabled.
             *
             * Finally, we usually don't check to see if the cache is
             * oversized at the end of an unprotect.  As a result, it is
             * possible to have a vastly oversized cache with no protected
             * entries as long as all the protects preceed the unprotects.
             *
             * Since items 1, 2, and 3 are not changing any time soon, I
             * see no point in worrying about the fourth.
             */

            if(H5C_make_space_in_cache(f, dxpl_id, space_needed, write_permitted) < 0 )
                HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, NULL, "H5C_make_space_in_cache failed 1.")
        }

        /* Insert the entry in the hash table.  It can't be dirty yet, so
         * we don't even check to see if it should go in the skip list.
         *
         * This is no longer true -- due to a bug fix, we may modify
         * data on load to repair a file.
         *
         *   *******************************************
         *
         * Set the flush_last field
 	 * of the newly loaded entry before inserting it into the 
         * index.  Must do this, as the index tracked the number of 
         * entries with the flush_last field set, but assumes that 
         * the field will not change after insertion into the index.
         *
         * Note that this means that the H5C__FLUSH_LAST_FLAG flag 
         * is ignored if the entry is already in cache.
         */
        entry_ptr->flush_me_last = flush_last;

        H5C__INSERT_IN_INDEX(cache_ptr, entry_ptr, NULL)

        if ( ( entry_ptr->is_dirty ) && ( ! (entry_ptr->in_slist) ) ) {

            H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, NULL)
        }

        /* insert the entry in the data structures used by the replacement
         * policy.  We are just going to take it out again when we update
         * the replacement policy for a protect, but this simplifies the
         * code.  If we do this often enough, we may want to optimize this.
         */
        H5C__UPDATE_RP_FOR_INSERTION(cache_ptr, entry_ptr, NULL)

        /* Record that the entry was loaded, to trigger a notify callback later */
        /* (After the entry is fully added to the cache) */
        was_loaded = TRUE;
    }

    HDassert( entry_ptr->addr == addr );
    HDassert( entry_ptr->type == type );

    if ( entry_ptr->is_protected ) {

	if ( ( read_only ) && ( entry_ptr->is_read_only ) ) {

	    HDassert( entry_ptr->ro_ref_count > 0 );

	    (entry_ptr->ro_ref_count)++;

	} else {

            HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, NULL, \
                        "Target already protected & not read only?!?.")
	}
    } else {

    	H5C__UPDATE_RP_FOR_PROTECT(cache_ptr, entry_ptr, NULL)

    	entry_ptr->is_protected = TRUE;

	if ( read_only ) {

	    entry_ptr->is_read_only = TRUE;
	    entry_ptr->ro_ref_count = 1;
	}

    	entry_ptr->dirtied = FALSE;
    }

    H5C__UPDATE_CACHE_HIT_RATE_STATS(cache_ptr, hit)

    H5C__UPDATE_STATS_FOR_PROTECT(cache_ptr, entry_ptr, hit)

    ret_value = thing;

    if ( ( cache_ptr->evictions_enabled ) &&
         ( ( cache_ptr->size_decreased ) ||
           ( ( cache_ptr->resize_enabled ) &&
             ( cache_ptr->cache_accesses >=
               (cache_ptr->resize_ctl).epoch_length ) ) ) ) {

        if ( ! have_write_permitted ) {

            if ( cache_ptr->check_write_permitted != NULL ) {
                if((cache_ptr->check_write_permitted)(f, &write_permitted) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, NULL, "Can't get write_permitted 2")
                else
                    have_write_permitted = TRUE;
            } else {

                write_permitted = cache_ptr->write_permitted;

                have_write_permitted = TRUE;

            }
        }

        if ( ( cache_ptr->resize_enabled ) &&
             ( cache_ptr->cache_accesses >=
               (cache_ptr->resize_ctl).epoch_length ) ) {

            if(H5C__auto_adjust_cache_size(f, dxpl_id, write_permitted) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, NULL, "Cache auto-resize failed.")
        }

        if ( cache_ptr->size_decreased  ) {

            cache_ptr->size_decreased = FALSE;

            /* check to see if the cache is now oversized due to the cache
             * size reduction.  If it is, try to evict enough entries to
             * bring the cache size down to the current maximum cache size.
	     *
	     * Also, if the min_clean_size requirement is not met, we
	     * should also call H5C_make_space_in_cache() to bring us
	     * into complience.
             */

            if(cache_ptr->index_size >= cache_ptr->max_cache_size)
               empty_space = 0;
            else
               empty_space = cache_ptr->max_cache_size - cache_ptr->index_size;

            if ( ( cache_ptr->index_size > cache_ptr->max_cache_size )
	         ||
	         ( ( empty_space + cache_ptr->clean_index_size ) <
	           cache_ptr->min_clean_size) ) {

		if(cache_ptr->index_size > cache_ptr->max_cache_size)
                    cache_ptr->cache_full = TRUE;

                if(H5C_make_space_in_cache(f, dxpl_id, (size_t)0, write_permitted) < 0 )
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTPROTECT, NULL, "H5C_make_space_in_cache failed 2.")
            }
        }
    }

    /* If we loaded the entry and the entry's type has a 'notify' callback, send
     * an 'after load' notice now that the entry is fully integrated into
     * the cache and protected.  We must wait until it is protected so it is not
     * evicted during the notify callback.
     */
    if(was_loaded) {
        /* If the entry's type has a 'notify' callback send a 'after load'
         * notice now that the entry is fully integrated into the cache.
         */
        if(entry_ptr->type->notify &&
                (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_AFTER_LOAD, entry_ptr) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, NULL, "can't notify client about entry inserted into cache")
    } /* end if */

#ifdef H5_HAVE_PARALLEL
    if(H5F_HAS_FEATURE(f, H5FD_FEAT_HAS_MPI)) {
        /* Make sure the size of the collective entries in the cache remain in check */
        if(coll_access) {
            if(H5P_USER_TRUE == f->coll_md_read) {
                if(cache_ptr->max_cache_size * 80 < cache_ptr->coll_list_size * 100)
                    if(H5C_clear_coll_entries(cache_ptr, TRUE) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, NULL, "can't clear collective metadata entries")
            } /* end if */
            else {
                if(cache_ptr->max_cache_size * 40 < cache_ptr->coll_list_size * 100)
                    if(H5C_clear_coll_entries(cache_ptr, TRUE) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, NULL, "can't clear collective metadata entries")
            } /* end else */
        } /* end if */
    } /* end if */
#endif /* H5_HAVE_PARALLEL */

done:
#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) )
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, NULL, "an extreme sanity check failed on exit.\n")
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_protect() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_reset_cache_hit_rate_stats()
 *
 * Purpose:     Reset the cache hit rate computation fields.
 *
 * Return:      SUCCEED on success, and FAIL on failure.
 *
 * Programmer:  John Mainzer, 10/5/04
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_reset_cache_hit_rate_stats(H5C_t * cache_ptr)
{
    herr_t	ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    if((cache_ptr == NULL) || (cache_ptr->magic != H5C__H5C_T_MAGIC))
        HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "bad cache_ptr on entry.")

    cache_ptr->cache_hits		= 0;
    cache_ptr->cache_accesses		= 0;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_reset_cache_hit_rate_stats() */


/*-------------------------------------------------------------------------
 * Function:    H5C_set_cache_auto_resize_config
 *
 * Purpose:	Set the cache automatic resize configuration to the
 *		provided values if they are in range, and fail if they
 *		are not.
 *
 *		If the new configuration enables automatic cache resizing,
 *		coerce the cache max size and min clean size into agreement
 *		with the new policy and re-set the full cache hit rate
 *		stats.
 *
 * Return:      SUCCEED on success, and FAIL on failure.
 *
 * Programmer:  John Mainzer
 *		10/8/04
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_set_cache_auto_resize_config(H5C_t *cache_ptr,
                                 H5C_auto_size_ctl_t *config_ptr)
{
    size_t      new_max_cache_size;
    size_t      new_min_clean_size;
    herr_t	ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    if((cache_ptr == NULL) || (cache_ptr->magic != H5C__H5C_T_MAGIC))
        HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "bad cache_ptr on entry.")
    if(config_ptr == NULL)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "NULL config_ptr on entry.")
    if(config_ptr->version != H5C__CURR_AUTO_SIZE_CTL_VER)
        HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "unknown config version.")

    /* check general configuration section of the config: */
    if(H5C_validate_resize_config(config_ptr, H5C_RESIZE_CFG__VALIDATE_GENERAL) < 0)
        HGOTO_ERROR(H5E_ARGS, H5E_BADRANGE, FAIL, "error in general configuration fields of new config.")

    /* check size increase control fields of the config: */
    if(H5C_validate_resize_config(config_ptr, H5C_RESIZE_CFG__VALIDATE_INCREMENT) < 0)
        HGOTO_ERROR(H5E_ARGS, H5E_BADRANGE, FAIL, "error in the size increase control fields of new config.")

    /* check size decrease control fields of the config: */
    if(H5C_validate_resize_config(config_ptr, H5C_RESIZE_CFG__VALIDATE_DECREMENT) < 0)
        HGOTO_ERROR(H5E_ARGS, H5E_BADRANGE, FAIL, "error in the size decrease control fields of new config.")

    /* check for conflicts between size increase and size decrease controls: */
    if(H5C_validate_resize_config(config_ptr, H5C_RESIZE_CFG__VALIDATE_INTERACTIONS) < 0)
        HGOTO_ERROR(H5E_ARGS, H5E_BADRANGE, FAIL, "conflicting threshold fields in new config.")

    /* will set the increase possible fields to FALSE later if needed */
    cache_ptr->size_increase_possible       = TRUE;
    cache_ptr->flash_size_increase_possible = TRUE;
    cache_ptr->size_decrease_possible       = TRUE;

    switch(config_ptr->incr_mode) {
        case H5C_incr__off:
            cache_ptr->size_increase_possible = FALSE;
            break;

        case H5C_incr__threshold:
            if((config_ptr->lower_hr_threshold <= (double)0.0f) ||
                     (config_ptr->increment <= (double)1.0f) ||
                     ((config_ptr->apply_max_increment) && (config_ptr->max_increment <= 0)))
                 cache_ptr->size_increase_possible = FALSE;
            break;

        default: /* should be unreachable */
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Unknown incr_mode?!?!?.")
    } /* end switch */

    /* logically, this is were configuration for flash cache size increases
     * should go.  However, this configuration depends on max_cache_size, so
     * we wait until the end of the function, when this field is set.
     */

    switch(config_ptr->decr_mode) {
        case H5C_decr__off:
            cache_ptr->size_decrease_possible = FALSE;
            break;

        case H5C_decr__threshold:
            if((config_ptr->upper_hr_threshold >= (double)1.0f) ||
                     (config_ptr->decrement >= (double)1.0f) ||
                     ((config_ptr->apply_max_decrement) && (config_ptr->max_decrement <= 0)))
                cache_ptr->size_decrease_possible = FALSE;
            break;

        case H5C_decr__age_out:
            if(((config_ptr->apply_empty_reserve) && (config_ptr->empty_reserve >= (double)1.0f)) ||
                    ((config_ptr->apply_max_decrement) && (config_ptr->max_decrement <= 0)))
                cache_ptr->size_decrease_possible = FALSE;
            break;

        case H5C_decr__age_out_with_threshold:
            if(((config_ptr->apply_empty_reserve) && (config_ptr->empty_reserve >= (double)1.0f)) ||
                    ((config_ptr->apply_max_decrement) && (config_ptr->max_decrement <= 0)) ||
                    (config_ptr->upper_hr_threshold >= (double)1.0f))
                cache_ptr->size_decrease_possible = FALSE;
            break;

        default: /* should be unreachable */
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Unknown decr_mode?!?!?.")
    } /* end switch */

    if(config_ptr->max_size == config_ptr->min_size) {
        cache_ptr->size_increase_possible = FALSE;
	cache_ptr->flash_size_increase_possible = FALSE;
        cache_ptr->size_decrease_possible = FALSE;
    } /* end if */

    /* flash_size_increase_possible is intentionally omitted from the
     * following:
     */
    cache_ptr->resize_enabled = cache_ptr->size_increase_possible ||
                                cache_ptr->size_decrease_possible;

    cache_ptr->resize_ctl = *config_ptr;

    /* Resize the cache to the supplied initial value if requested, or as
     * necessary to force it within the bounds of the current automatic
     * cache resizing configuration.
     *
     * Note that the min_clean_fraction may have changed, so we
     * go through the exercise even if the current size is within
     * range and an initial size has not been provided.
     */
    if(cache_ptr->resize_ctl.set_initial_size)
        new_max_cache_size = cache_ptr->resize_ctl.initial_size;
    else if(cache_ptr->max_cache_size > cache_ptr->resize_ctl.max_size)
        new_max_cache_size = cache_ptr->resize_ctl.max_size;
    else if(cache_ptr->max_cache_size < cache_ptr->resize_ctl.min_size)
        new_max_cache_size = cache_ptr->resize_ctl.min_size;
    else
        new_max_cache_size = cache_ptr->max_cache_size;

    new_min_clean_size = (size_t)((double)new_max_cache_size *
                          ((cache_ptr->resize_ctl).min_clean_fraction));


    /* since new_min_clean_size is of type size_t, we have
     *
     * 	( 0 <= new_min_clean_size )
     *
     * by definition.
     */
    HDassert(new_min_clean_size <= new_max_cache_size);
    HDassert(cache_ptr->resize_ctl.min_size <= new_max_cache_size);
    HDassert(new_max_cache_size <= cache_ptr->resize_ctl.max_size);

    if(new_max_cache_size < cache_ptr->max_cache_size)
        cache_ptr->size_decreased = TRUE;

    cache_ptr->max_cache_size = new_max_cache_size;
    cache_ptr->min_clean_size = new_min_clean_size;

    if(H5C_reset_cache_hit_rate_stats(cache_ptr) < 0)
        /* this should be impossible... */
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "H5C_reset_cache_hit_rate_stats failed.")

    /* remove excess epoch markers if any */
    if((config_ptr->decr_mode == H5C_decr__age_out_with_threshold) ||
            (config_ptr->decr_mode == H5C_decr__age_out)) {
        if(cache_ptr->epoch_markers_active > cache_ptr->resize_ctl.epochs_before_eviction)
            if(H5C__autoadjust__ageout__remove_excess_markers(cache_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "can't remove excess epoch markers.")
    } /* end if */
    else if(cache_ptr->epoch_markers_active > 0) {
        if(H5C__autoadjust__ageout__remove_all_markers(cache_ptr) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "error removing all epoch markers.")
    }

    /* configure flash size increase facility.  We wait until the
     * end of the function, as we need the max_cache_size set before
     * we start to keep things simple.
     *
     * If we haven't already ruled out flash cache size increases above,
     * go ahead and configure it.
     */

    if(cache_ptr->flash_size_increase_possible) {
        switch(config_ptr->flash_incr_mode) {
            case H5C_flash_incr__off:
                cache_ptr->flash_size_increase_possible = FALSE;
                break;

            case H5C_flash_incr__add_space:
                cache_ptr->flash_size_increase_possible = TRUE;
                cache_ptr->flash_size_increase_threshold = (size_t)(((double)(cache_ptr->max_cache_size)) *
                     ((cache_ptr->resize_ctl).flash_threshold));
                break;

            default: /* should be unreachable */
                 HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Unknown flash_incr_mode?!?!?.")
                 break;
        } /* end switch */
    } /* end if */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_set_cache_auto_resize_config() */


/*-------------------------------------------------------------------------
 * Function:    H5C_set_evictions_enabled()
 *
 * Purpose:     Set cache_ptr->evictions_enabled to the value of the
 *              evictions enabled parameter.
 *
 * Return:      SUCCEED on success, and FAIL on failure.
 *
 * Programmer:  John Mainzer
 *              7/27/07
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_set_evictions_enabled(H5C_t *cache_ptr, hbool_t evictions_enabled)
{
    herr_t ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    if((cache_ptr == NULL) || (cache_ptr->magic != H5C__H5C_T_MAGIC))
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Bad cache_ptr on entry.")

    /* There is no fundamental reason why we should not permit
     * evictions to be disabled while automatic resize is enabled.
     * However, I can't think of any good reason why one would
     * want to, and allowing it would greatly complicate testing
     * the feature.  Hence the following:
     */
    if((evictions_enabled != TRUE) &&
         ((cache_ptr->resize_ctl.incr_mode != H5C_incr__off) ||
	   (cache_ptr->resize_ctl.decr_mode != H5C_decr__off)))
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Can't disable evictions when auto resize enabled.")

    cache_ptr->evictions_enabled = evictions_enabled;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_set_evictions_enabled() */


/*-------------------------------------------------------------------------
 * Function:    H5C_unpin_entry_from_client()
 *
 * Purpose:	Internal routine to unpin a cache entry from a client action.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              3/24/09
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C_unpin_entry_from_client(H5C_t *		  cache_ptr,
                H5C_cache_entry_t *	  entry_ptr,
                hbool_t update_rp)
{
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    /* Sanity checking */
    HDassert( cache_ptr );
    HDassert( entry_ptr );

    /* Error checking (should be sanity checks?) */
    if(!entry_ptr->is_pinned)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPIN, FAIL, "Entry isn't pinned")
    if(!entry_ptr->pinned_from_client)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPIN, FAIL, "Entry wasn't pinned by cache client")

    /* Check if the entry is not pinned from a flush dependency */
    if(!entry_ptr->pinned_from_cache) {
        /* If requested, update the replacement policy if the entry is not protected */
        if(update_rp && !entry_ptr->is_protected)
            H5C__UPDATE_RP_FOR_UNPIN(cache_ptr, entry_ptr, FAIL)

        /* Unpin the entry now */
        entry_ptr->is_pinned = FALSE;

        /* Update the stats for an unpin operation */
        H5C__UPDATE_STATS_FOR_UNPIN(cache_ptr, entry_ptr)
    } /* end if */

    /* Mark the entry as explicitly unpinned by the client */
    entry_ptr->pinned_from_client = FALSE;

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_unpin_entry_from_client() */


/*-------------------------------------------------------------------------
 * Function:    H5C_unpin_entry()
 *
 * Purpose:	Unpin a cache entry.  The entry can be either protected or
 * 		unprotected at the time of call, but must be pinned.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              3/22/06
 *
 * Changes:	Added extreme sanity checks on entry and exit.
                				JRM -- 4/26/14 
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_unpin_entry(void *_entry_ptr)
{
    H5C_t             * cache_ptr;
    H5C_cache_entry_t * entry_ptr = (H5C_cache_entry_t *)_entry_ptr; /* Pointer to entry to unpin */
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity check */
    HDassert(entry_ptr);
    cache_ptr = entry_ptr->cache_ptr;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on entry.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */


    /* Unpin the entry */
    if(H5C_unpin_entry_from_client(cache_ptr, entry_ptr, TRUE) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPIN, FAIL, "Can't unpin entry from client")

done:

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on exit.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_unpin_entry() */


/*-------------------------------------------------------------------------
 * Function:    H5C_unprotect
 *
 * Purpose:	Undo an H5C_protect() call -- specifically, mark the
 *		entry as unprotected, remove it from the protected list,
 *		and give it back to the replacement policy.
 *
 *		The TYPE and ADDR arguments must be the same as those in
 *		the corresponding call to H5C_protect() and the THING
 *		argument must be the value returned by that call to
 *		H5C_protect().
 *
 * Return:      Non-negative on success/Negative on failure
 *
 *		If the deleted flag is TRUE, simply remove the target entry
 *		from the cache, clear it, and free it without writing it to
 *		disk.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              6/2/04
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_unprotect(H5F_t *		  f,
              hid_t		  dxpl_id,
              haddr_t		  addr,
              void *		  thing,
              unsigned int        flags)
{
    H5C_t *             cache_ptr;
    hbool_t		deleted;
    hbool_t		dirtied;
    hbool_t             set_flush_marker;
    hbool_t		pin_entry;
    hbool_t		unpin_entry;
    hbool_t		free_file_space;
    hbool_t		take_ownership;
    hbool_t 		was_clean;
#ifdef H5_HAVE_PARALLEL
    hbool_t		clear_entry = FALSE;
#endif /* H5_HAVE_PARALLEL */
    H5C_cache_entry_t *	entry_ptr;
    H5C_cache_entry_t *	test_entry_ptr;
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    deleted                = ((flags & H5C__DELETED_FLAG) != 0);
    dirtied                = ((flags & H5C__DIRTIED_FLAG) != 0);
    set_flush_marker       = ((flags & H5C__SET_FLUSH_MARKER_FLAG) != 0);
    pin_entry              = ((flags & H5C__PIN_ENTRY_FLAG) != 0);
    unpin_entry            = ((flags & H5C__UNPIN_ENTRY_FLAG) != 0);
    free_file_space        = ((flags & H5C__FREE_FILE_SPACE_FLAG) != 0);
    take_ownership         = ((flags & H5C__TAKE_OWNERSHIP_FLAG) != 0);

    HDassert( f );
    HDassert( f->shared );

    cache_ptr = f->shared->cache;

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( H5F_addr_defined(addr) );
    HDassert( thing );
    HDassert( ! ( pin_entry && unpin_entry ) );
    HDassert( ( ! free_file_space ) || ( deleted ) );   /* deleted flag must accompany free_file_space */
    HDassert( ( ! take_ownership ) || ( deleted ) );    /* deleted flag must accompany take_ownership */
    HDassert( ! ( free_file_space && take_ownership ) );    /* can't have both free_file_space & take_ownership */

    entry_ptr = (H5C_cache_entry_t *)thing;

    HDassert( entry_ptr->addr == addr );

    /* also set the dirtied variable if the dirtied field is set in
     * the entry.
     */
    dirtied |= entry_ptr->dirtied;
    was_clean = ! ( entry_ptr->is_dirty );

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on entry.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    /* if the entry has multiple read only protects, just decrement
     * the ro_ref_counter.  Don't actually unprotect until the ref count
     * drops to zero.
     */
    if(entry_ptr->ro_ref_count > 1) {
        /* Sanity check */
	HDassert(entry_ptr->is_protected);
        HDassert(entry_ptr->is_read_only);

	if(dirtied)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "Read only entry modified??")

        /* Reduce the RO ref count */
	(entry_ptr->ro_ref_count)--;

        /* Pin or unpin the entry as requested. */
        if(pin_entry) {
            /* Pin the entry from a client */
            if(H5C_pin_entry_from_client(cache_ptr, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTPIN, FAIL, "Can't pin entry by client")
        } else if(unpin_entry) {
            /* Unpin the entry from a client */
            if(H5C_unpin_entry_from_client(cache_ptr, entry_ptr, FALSE) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPIN, FAIL, "Can't unpin entry by client")
        } /* end if */
    } else {
	if(entry_ptr->is_read_only) {
            /* Sanity check */
	    HDassert(entry_ptr->ro_ref_count == 1);

	    if(dirtied)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "Read only entry modified??")

	    entry_ptr->is_read_only = FALSE;
	    entry_ptr->ro_ref_count = 0;
	} /* end if */

#ifdef H5_HAVE_PARALLEL
        /* When the H5C code is used to implement the metadata cache in the
         * PHDF5 case, only the cache on process 0 is allowed to write to file.
         * All the other metadata caches must hold dirty entries until they
         * are told that the entries are clean.
         *
         * The clear_on_unprotect flag in the H5C_cache_entry_t structure
         * exists to deal with the case in which an entry is protected when
         * its cache receives word that the entry is now clean.  In this case,
         * the clear_on_unprotect flag is set, and the entry is flushed with
         * the H5C__FLUSH_CLEAR_ONLY_FLAG.
         *
         * All this is a bit awkward, but until the metadata cache entries
         * are contiguous, with only one dirty flag, we have to let the supplied
         * functions deal with the reseting the is_dirty flag.
         */
        if(entry_ptr->clear_on_unprotect) {
            /* Sanity check */
            HDassert(entry_ptr->is_dirty);

            entry_ptr->clear_on_unprotect = FALSE;
            if(!dirtied)
                clear_entry = TRUE;
        } /* end if */
#endif /* H5_HAVE_PARALLEL */

        if(!entry_ptr->is_protected)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "Entry already unprotected??")

        /* Mark the entry as dirty if appropriate */
        entry_ptr->is_dirty = (entry_ptr->is_dirty || dirtied);

	if(dirtied)
	    if(entry_ptr->image_up_to_date) {
	        entry_ptr->image_up_to_date = FALSE;
	        if(entry_ptr->flush_dep_nparents > 0)
		    if(H5C__mark_flush_dep_unserialized(entry_ptr) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "Can't propagate serialization status to fd parents")
            } /* end if */

        /* Check for newly dirtied entry */
        if(was_clean && entry_ptr->is_dirty) {
            /* Update index for newly dirtied entry */
            H5C__UPDATE_INDEX_FOR_ENTRY_DIRTY(cache_ptr, entry_ptr)

            /* If the entry's type has a 'notify' callback send a 'entry dirtied'
             * notice now that the entry is fully integrated into the cache.
             */
            if(entry_ptr->type->notify &&
                    (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_DIRTIED, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag set")

            /* Propagate the flush dep dirty flag up the flush dependency chain
             * if appropriate */
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_dirty(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKDIRTY, FAIL, "Can't propagate flush dep dirty flag")
        } /* end if */ 
        /* Check for newly clean entry */
        else if(!was_clean && !entry_ptr->is_dirty) {
            /* If the entry's type has a 'notify' callback send a 'entry cleaned'
             * notice now that the entry is fully integrated into the cache.
             */
            if(entry_ptr->type->notify &&
                    (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_CLEANED, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag cleared")

            /* Propagate the flush dep clean flag up the flush dependency chain
             * if appropriate */
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_clean(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKDIRTY, FAIL, "Can't propagate flush dep dirty flag")
        } /* end else-if */

        /* Pin or unpin the entry as requested. */
        if(pin_entry) {
            /* Pin the entry from a client */
            if(H5C_pin_entry_from_client(cache_ptr, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTPIN, FAIL, "Can't pin entry by client")
        } else if(unpin_entry) {
            /* Unpin the entry from a client */
            if(H5C_unpin_entry_from_client(cache_ptr, entry_ptr, FALSE) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPIN, FAIL, "Can't unpin entry by client")
        } /* end if */

        /* H5C__UPDATE_RP_FOR_UNPROTECT will place the unprotected entry on
         * the pinned entry list if entry_ptr->is_pinned is TRUE.
         */
        H5C__UPDATE_RP_FOR_UNPROTECT(cache_ptr, entry_ptr, FAIL)

        entry_ptr->is_protected = FALSE;

        /* if the entry is dirty, 'or' its flush_marker with the set flush flag,
         * and then add it to the skip list if it isn't there already.
         */
        if(entry_ptr->is_dirty) {
            entry_ptr->flush_marker |= set_flush_marker;
            if(!entry_ptr->in_slist)
                H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, FAIL)
        } /* end if */

        /* this implementation of the "deleted" option is a bit inefficient, as
         * we re-insert the entry to be deleted into the replacement policy
         * data structures, only to remove them again.  Depending on how often
         * we do this, we may want to optimize a bit.
         *
         * On the other hand, this implementation is reasonably clean, and
         * makes good use of existing code.
         *                                             JRM - 5/19/04
         */
        if(deleted) {
            unsigned    flush_flags = (H5C__FLUSH_CLEAR_ONLY_FLAG |
                                         H5C__FLUSH_INVALIDATE_FLAG);

            /* verify that the target entry is in the cache. */
            H5C__SEARCH_INDEX(cache_ptr, addr, test_entry_ptr, FAIL)
            if(test_entry_ptr == NULL)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "entry not in hash table?!?.")
            else if(test_entry_ptr != entry_ptr)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "hash table contains multiple entries for addr?!?.")

            /* Set the 'free file space' flag for the flush, if needed */
            if(free_file_space)
                flush_flags |= H5C__FREE_FILE_SPACE_FLAG;

            /* Set the "take ownership" flag for the flush, if needed */
            if(take_ownership)
                flush_flags |= H5C__TAKE_OWNERSHIP_FLAG;

            /* Delete the entry from the skip list on destroy */
            flush_flags |= H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG;

            HDassert(((!was_clean) || dirtied) == entry_ptr->in_slist);
            if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, flush_flags) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "Can't flush entry")

#if H5C_DO_SANITY_CHECKS
            if((take_ownership) && ((!was_clean) || (dirtied))) {
                /* we have just removed an entry from the skip list.  Thus 
                 * we must touch up cache_ptr->slist_len_increase and
                 * cache_ptr->slist_size_increase to keep from skewing
                 * the sanity checks on flushes.
                 */
                cache_ptr->slist_len_increase -= 1;
                cache_ptr->slist_size_increase -= (int64_t)(entry_ptr->size);
            } /* end if */
#endif /* H5C_DO_SANITY_CHECKS */
        } /* end if */
#ifdef H5_HAVE_PARALLEL
        else if(clear_entry) {

            /* verify that the target entry is in the cache. */
            H5C__SEARCH_INDEX(cache_ptr, addr, test_entry_ptr, FAIL)
            if(test_entry_ptr == NULL)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "entry not in hash table?!?.")
            else if(test_entry_ptr != entry_ptr)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "hash table contains multiple entries for addr?!?.")

            if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__FLUSH_CLEAR_ONLY_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNPROTECT, FAIL, "Can't clear entry")
        } /* end else if */
#endif /* H5_HAVE_PARALLEL */
    }

    H5C__UPDATE_STATS_FOR_UNPROTECT(cache_ptr)

done:

#if H5C_DO_EXTREME_SANITY_CHECKS
    if ( ( H5C_validate_protected_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_pinned_entry_list(cache_ptr) < 0 ) ||
         ( H5C_validate_lru_list(cache_ptr) < 0 ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "an extreme sanity check failed on exit.\n");
    }
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_unprotect() */


/*-------------------------------------------------------------------------
 * Function:    H5C_unsettle_ring()
 *
 * Purpose:	Advise the metadata cache that the specified free space 
 *		manager ring is no longer settled (if it was on entry).
 *
 *		If the target free space manager ring is already 
 *		unsettled, do nothing, and return SUCCEED.
 *
 *		If the target free space manager ring is settled, and
 *		we are not in the process of a file shutdown, mark 
 *		the ring as unsettled, and return SUCCEED.
 *
 *		If the target free space manager is settled, and we 
 *		are in the process of a file shutdown, post an error
 *		message, and return FAIL.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              10/15/16
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_unsettle_ring(H5F_t *f, H5C_ring_t ring) 
{
    H5C_t *		cache_ptr;
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->cache);
    cache_ptr = f->shared->cache;
    HDassert(H5C__H5C_T_MAGIC == cache_ptr->magic);
    HDassert((H5C_RING_RDFSM == ring) || (H5C_RING_MDFSM == ring));

    switch(ring) {
	case H5C_RING_RDFSM:
	    if(cache_ptr->rdfsm_settled) {
		if(cache_ptr->flush_in_progress || cache_ptr->close_warning_received)
		    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "unexpected rdfsm ring unsettle")
		cache_ptr->rdfsm_settled = FALSE;
	    } /* end if */
	    break;

	case H5C_RING_MDFSM:
	    if(cache_ptr->mdfsm_settled) {
		if(cache_ptr->flush_in_progress || cache_ptr->close_warning_received)
		    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "unexpected mdfsm ring unsettle")
		cache_ptr->mdfsm_settled = FALSE;
	    } /* end if */
	    break;

	default:
	    HDassert(FALSE); /* this should be un-reachable */
	    break;
    } /* end switch */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_unsettle_ring() */


/*-------------------------------------------------------------------------
 * Function:    H5C_validate_resize_config()
 *
 * Purpose:	Run a sanity check on the specified sections of the
 *		provided instance of struct H5C_auto_size_ctl_t.
 *
 *		Do nothing and return SUCCEED if no errors are detected,
 *		and flag an error and return FAIL otherwise.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              3/23/05
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_validate_resize_config(H5C_auto_size_ctl_t * config_ptr,
                           unsigned int tests)
{
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    if ( config_ptr == NULL ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "NULL config_ptr on entry.")
    }

    if ( config_ptr->version != H5C__CURR_AUTO_SIZE_CTL_VER ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Unknown config version.")
    }


    if ( (tests & H5C_RESIZE_CFG__VALIDATE_GENERAL) != 0 ) {

        if(config_ptr->max_size > H5C__MAX_MAX_CACHE_SIZE)
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "max_size too big")

        if(config_ptr->min_size < H5C__MIN_MAX_CACHE_SIZE)
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "min_size too small")

        if(config_ptr->min_size > config_ptr->max_size)
            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "min_size > max_size")

        if ( ( config_ptr->set_initial_size ) &&
             ( ( config_ptr->initial_size < config_ptr->min_size ) ||
               ( config_ptr->initial_size > config_ptr->max_size ) ) ) {

            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                  "initial_size must be in the interval [min_size, max_size]");
        }

        if ( ( config_ptr->min_clean_fraction < (double)0.0f ) ||
             ( config_ptr->min_clean_fraction > (double)1.0f ) ) {

            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                  "min_clean_fraction must be in the interval [0.0, 1.0]");
        }

        if ( config_ptr->epoch_length < H5C__MIN_AR_EPOCH_LENGTH ) {

            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "epoch_length too small")
        }

        if ( config_ptr->epoch_length > H5C__MAX_AR_EPOCH_LENGTH ) {

            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "epoch_length too big")
        }
    } /* H5C_RESIZE_CFG__VALIDATE_GENERAL */


    if ( (tests & H5C_RESIZE_CFG__VALIDATE_INCREMENT) != 0 ) {

        if ( ( config_ptr->incr_mode != H5C_incr__off ) &&
             ( config_ptr->incr_mode != H5C_incr__threshold ) ) {

            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "Invalid incr_mode")
        }

        if ( config_ptr->incr_mode == H5C_incr__threshold ) {

            if((config_ptr->lower_hr_threshold < (double)0.0f) ||
                    (config_ptr->lower_hr_threshold > (double)1.0f))
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "lower_hr_threshold must be in the range [0.0, 1.0]")

            if(config_ptr->increment < (double)1.0f)
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "increment must be greater than or equal to 1.0")

            /* no need to check max_increment, as it is a size_t,
             * and thus must be non-negative.
             */
        } /* H5C_incr__threshold */

        switch ( config_ptr->flash_incr_mode )
        {
            case H5C_flash_incr__off:
                /* nothing to do here */
                break;

            case H5C_flash_incr__add_space:
                if ( ( config_ptr->flash_multiple < (double)0.1f ) ||
                     ( config_ptr->flash_multiple > (double)10.0f ) ) {

                    HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                        "flash_multiple must be in the range [0.1, 10.0]");
                }

                if ( ( config_ptr->flash_threshold < (double)0.1f ) ||
                     ( config_ptr->flash_threshold > (double)1.0f ) ) {

                    HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                           "flash_threshold must be in the range [0.1, 1.0]");
                }
                break;

            default:
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                            "Invalid flash_incr_mode");
                break;
        }
    } /* H5C_RESIZE_CFG__VALIDATE_INCREMENT */


    if ( (tests & H5C_RESIZE_CFG__VALIDATE_DECREMENT) != 0 ) {

        if ( ( config_ptr->decr_mode != H5C_decr__off ) &&
             ( config_ptr->decr_mode != H5C_decr__threshold ) &&
             ( config_ptr->decr_mode != H5C_decr__age_out ) &&
             ( config_ptr->decr_mode != H5C_decr__age_out_with_threshold )
           ) {

            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "Invalid decr_mode")
        }

        if ( config_ptr->decr_mode == H5C_decr__threshold ) {

            if ( config_ptr->upper_hr_threshold > (double)1.0f ) {

                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                            "upper_hr_threshold must be <= 1.0");
            }

            if ( ( config_ptr->decrement > (double)1.0f ) ||
                 ( config_ptr->decrement < (double)0.0f ) ) {

                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                            "decrement must be in the interval [0.0, 1.0]");
            }

            /* no need to check max_decrement as it is a size_t
             * and thus must be non-negative.
             */
        } /* H5C_decr__threshold */

        if ( ( config_ptr->decr_mode == H5C_decr__age_out ) ||
             ( config_ptr->decr_mode == H5C_decr__age_out_with_threshold )
           ) {

            if ( config_ptr->epochs_before_eviction < 1 ) {

                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                            "epochs_before_eviction must be positive");
            }

            if(config_ptr->epochs_before_eviction > H5C__MAX_EPOCH_MARKERS)
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "epochs_before_eviction too big")

            if((config_ptr->apply_empty_reserve) &&
                    ((config_ptr->empty_reserve > (double)1.0f) ||
                        (config_ptr->empty_reserve < (double)0.0f)))
                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "empty_reserve must be in the interval [0.0, 1.0]")

            /* no need to check max_decrement as it is a size_t
             * and thus must be non-negative.
             */
        } /* H5C_decr__age_out || H5C_decr__age_out_with_threshold */

        if ( config_ptr->decr_mode == H5C_decr__age_out_with_threshold ) {

            if ( ( config_ptr->upper_hr_threshold > (double)1.0f ) ||
                 ( config_ptr->upper_hr_threshold < (double)0.0f ) ) {

                HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                       "upper_hr_threshold must be in the interval [0.0, 1.0]");
            }
        } /* H5C_decr__age_out_with_threshold */

    } /* H5C_RESIZE_CFG__VALIDATE_DECREMENT */


    if ( (tests & H5C_RESIZE_CFG__VALIDATE_INTERACTIONS) != 0 ) {

        if ( ( config_ptr->incr_mode == H5C_incr__threshold )
             &&
             ( ( config_ptr->decr_mode == H5C_decr__threshold )
               ||
               ( config_ptr->decr_mode == H5C_decr__age_out_with_threshold )
             )
             &&
             ( config_ptr->lower_hr_threshold
               >=
               config_ptr->upper_hr_threshold
             )
           ) {

            HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, \
                        "conflicting threshold fields in config.")
        }
    } /* H5C_RESIZE_CFG__VALIDATE_INTERACTIONS */

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_validate_resize_config() */


/*-------------------------------------------------------------------------
 * Function:    H5C_create_flush_dependency()
 *
 * Purpose:	Initiates a parent<->child entry flush dependency.  The parent
 *              entry must be pinned or protected at the time of call, and must
 *              have all dependencies removed before the cache can shut down.
 *
 * Note:	Flush dependencies in the cache indicate that a child entry
 *              must be flushed to the file before its parent.  (This is
 *              currently used to implement Single-Writer/Multiple-Reader (SWMR)
 *              I/O access for data structures in the file).
 *
 *              Creating a flush dependency between two entries will also pin
 *              the parent entry.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              3/05/09
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_create_flush_dependency(void * parent_thing, void * child_thing)
{
    H5C_t             * cache_ptr;
    H5C_cache_entry_t *	parent_entry = (H5C_cache_entry_t *)parent_thing;   /* Ptr to parent thing's entry */
    H5C_cache_entry_t * child_entry = (H5C_cache_entry_t *)child_thing;    /* Ptr to child thing's entry */
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(parent_entry);
    HDassert(parent_entry->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
    HDassert(H5F_addr_defined(parent_entry->addr));
    HDassert(child_entry);
    HDassert(child_entry->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
    HDassert(H5F_addr_defined(child_entry->addr));
    cache_ptr = parent_entry->cache_ptr;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(cache_ptr == child_entry->cache_ptr);
#ifndef NDEBUG
    /* Make sure the parent is not already a parent */
    {
        unsigned u;

        for(u = 0; u < child_entry->flush_dep_nparents; u++)
            HDassert(child_entry->flush_dep_parent[u] != parent_entry);
    } /* end block */
#endif /* NDEBUG */

#ifndef NDEBUG
    /* Make sure the parent is not already a parent */
    {
        unsigned u;

        for(u = 0; u < child_entry->flush_dep_nparents; u++)
            HDassert(child_entry->flush_dep_parent[u] != parent_entry);
    } /* end block */
#endif /* NDEBUG */

    /* More sanity checks */
    if(child_entry == parent_entry)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTDEPEND, FAIL, "Child entry flush dependency parent can't be itself")
    if(!(parent_entry->is_protected || parent_entry->is_pinned))
        HGOTO_ERROR(H5E_CACHE, H5E_CANTDEPEND, FAIL, "Parent entry isn't pinned or protected")

    /* Check for parent not pinned */
    if(!parent_entry->is_pinned) {
        /* Sanity check */
        HDassert(parent_entry->flush_dep_nchildren == 0);
        HDassert(!parent_entry->pinned_from_client);
        HDassert(!parent_entry->pinned_from_cache);

        /* Pin the parent entry */
        parent_entry->is_pinned = TRUE;
        H5C__UPDATE_STATS_FOR_PIN(cache_ptr, parent_entry)
    } /* end else */

    /* Mark the entry as pinned from the cache's action (possibly redundantly) */
    parent_entry->pinned_from_cache = TRUE;

    /* Check if we need to resize the child's parent array */
    if(child_entry->flush_dep_nparents >= child_entry->flush_dep_parent_nalloc) {
        if(child_entry->flush_dep_parent_nalloc == 0) {
            /* Array does not exist yet, allocate it */
            HDassert(!child_entry->flush_dep_parent);

            if(NULL == (child_entry->flush_dep_parent = (H5C_cache_entry_t **)H5FL_BLK_MALLOC(parent, H5C_FLUSH_DEP_PARENT_INIT * sizeof(H5C_cache_entry_t *))))
                HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed for flush dependency parent list")
            child_entry->flush_dep_parent_nalloc = H5C_FLUSH_DEP_PARENT_INIT;
        } /* end if */
        else {
            /* Resize existing array */
            HDassert(child_entry->flush_dep_parent);

            if(NULL == (child_entry->flush_dep_parent = (H5C_cache_entry_t **)H5FL_BLK_REALLOC(parent, child_entry->flush_dep_parent, 2 * child_entry->flush_dep_parent_nalloc * sizeof(H5C_cache_entry_t *))))
                HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed for flush dependency parent list")
            child_entry->flush_dep_parent_nalloc *= 2;
        } /* end else */
        cache_ptr->entry_fd_height_change_counter++;
    } /* end if */

    /* Add the dependency to the child's parent array */
    child_entry->flush_dep_parent[child_entry->flush_dep_nparents] = parent_entry;
    child_entry->flush_dep_nparents++;

    /* Increment parent's number of children */
    parent_entry->flush_dep_nchildren++;

    /* Adjust the number of dirty children */
    if(child_entry->is_dirty) {
        /* Sanity check */
        HDassert(parent_entry->flush_dep_ndirty_children < parent_entry->flush_dep_nchildren);

        parent_entry->flush_dep_ndirty_children++;

        /* If the parent has a 'notify' callback, send a 'child entry dirtied' notice */
        if(parent_entry->type->notify &&
                (parent_entry->type->notify)(H5C_NOTIFY_ACTION_CHILD_DIRTIED, parent_entry) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry dirty flag set")
    } /* end if */

    /* adjust the parent's number of unserialized children.  Note
     * that it is possible for and entry to be clean and unserialized.
     */
    if(!child_entry->image_up_to_date) {
        HDassert(parent_entry->flush_dep_nunser_children < parent_entry->flush_dep_nchildren);

        parent_entry->flush_dep_nunser_children++;

        /* If the parent has a 'notify' callback, send a 'child entry unserialized' notice */
        if(parent_entry->type->notify &&
                (parent_entry->type->notify)(H5C_NOTIFY_ACTION_CHILD_UNSERIALIZED, parent_entry) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry serialized flag reset")
    } /* end if */

    /* Post-conditions, for successful operation */
    HDassert(parent_entry->is_pinned);
    HDassert(parent_entry->flush_dep_nchildren > 0);
    HDassert(child_entry->flush_dep_parent);
    HDassert(child_entry->flush_dep_nparents > 0);
    HDassert(child_entry->flush_dep_parent_nalloc > 0);
#ifndef NDEBUG
    H5C__assert_flush_dep_nocycle(parent_entry, child_entry);
#endif /* NDEBUG */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_create_flush_dependency() */


/*-------------------------------------------------------------------------
 * Function:    H5C_destroy_flush_dependency()
 *
 * Purpose:	Terminates a parent<-> child entry flush dependency.  The
 *              parent entry must be pinned.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              3/05/09
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_destroy_flush_dependency(void *parent_thing, void * child_thing)
{
    H5C_t             * cache_ptr;
    H5C_cache_entry_t *	parent_entry = (H5C_cache_entry_t *)parent_thing; /* Ptr to parent entry */
    H5C_cache_entry_t *	child_entry = (H5C_cache_entry_t *)child_thing; /* Ptr to child entry */
    unsigned            u;                      /* Local index variable */
    herr_t              ret_value = SUCCEED;    /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(parent_entry);
    HDassert(parent_entry->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
    HDassert(H5F_addr_defined(parent_entry->addr));
    HDassert(child_entry);
    HDassert(child_entry->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
    HDassert(H5F_addr_defined(child_entry->addr));
    cache_ptr = parent_entry->cache_ptr;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(cache_ptr == child_entry->cache_ptr);

    /* Usage checks */
    if(!parent_entry->is_pinned)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTUNDEPEND, FAIL, "Parent entry isn't pinned")
    if(NULL == child_entry->flush_dep_parent)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTUNDEPEND, FAIL, "Child entry doesn't have a flush dependency parent array")
    if(0 == parent_entry->flush_dep_nchildren)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTUNDEPEND, FAIL, "Parent entry flush dependency ref. count has no child dependencies")

    /* Search for parent in child's parent array.  This is a linear search
     * because we do not expect large numbers of parents.  If this changes, we
     * may wish to change the parent array to a skip list */
    for(u = 0; u < child_entry->flush_dep_nparents; u++)
        if(child_entry->flush_dep_parent[u] == parent_entry)
            break;
    if(u == child_entry->flush_dep_nparents)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTUNDEPEND, FAIL, "Parent entry isn't a flush dependency parent for child entry")

    /* Remove parent entry from child's parent array */
    if(u < (child_entry->flush_dep_nparents - 1))
        HDmemmove(&child_entry->flush_dep_parent[u],
                &child_entry->flush_dep_parent[u + 1],
                (child_entry->flush_dep_nparents - u - 1) * sizeof(child_entry->flush_dep_parent[0]));
    child_entry->flush_dep_nparents--;

    /* Adjust parent entry's nchildren and unpin parent if it goes to zero */
    parent_entry->flush_dep_nchildren--;
    if(0 == parent_entry->flush_dep_nchildren) {
        /* Sanity check */
        HDassert(parent_entry->pinned_from_cache);

        /* Check if we should unpin parent entry now */
        if(!parent_entry->pinned_from_client) {
            /* Update the replacement policy if the entry is not protected */
            if(!parent_entry->is_protected)
                H5C__UPDATE_RP_FOR_UNPIN(cache_ptr, parent_entry, FAIL)

            /* Unpin the entry now */
            parent_entry->is_pinned = FALSE;

            /* Update the stats for an unpin operation */
            H5C__UPDATE_STATS_FOR_UNPIN(cache_ptr, parent_entry)
        } /* end if */

        /* Mark the entry as unpinned from the cache's action */
        parent_entry->pinned_from_cache = FALSE;
    } /* end if */

    /* Adjust parent entry's ndirty_children */
    if(child_entry->is_dirty) {
        /* Sanity check */
        HDassert(parent_entry->flush_dep_ndirty_children > 0);

        parent_entry->flush_dep_ndirty_children--;

        /* If the parent has a 'notify' callback, send a 'child entry cleaned' notice */
        if(parent_entry->type->notify &&
                (parent_entry->type->notify)(H5C_NOTIFY_ACTION_CHILD_CLEANED, parent_entry) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry dirty flag reset")
    } /* end if */

    /* adjust parent entry's number of unserialized children */
    if(!child_entry->image_up_to_date) {
        HDassert(parent_entry->flush_dep_nunser_children > 0);

        parent_entry->flush_dep_nunser_children--;

        /* If the parent has a 'notify' callback, send a 'child entry serialized' notice */
        if(parent_entry->type->notify &&
                (parent_entry->type->notify)(H5C_NOTIFY_ACTION_CHILD_SERIALIZED, parent_entry) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry serialized flag set")
    } /* end if */

    /* Shrink or free the parent array if apporpriate */
    if(child_entry->flush_dep_nparents == 0) {
        child_entry->flush_dep_parent = (H5C_cache_entry_t **)H5FL_BLK_FREE(parent, child_entry->flush_dep_parent);
        child_entry->flush_dep_parent_nalloc = 0;
    } /* end if */
    else if(child_entry->flush_dep_parent_nalloc > H5C_FLUSH_DEP_PARENT_INIT
            && child_entry->flush_dep_nparents
            <= (child_entry->flush_dep_parent_nalloc / 4)) {
        if(NULL == (child_entry->flush_dep_parent = (H5C_cache_entry_t **)H5FL_BLK_REALLOC(parent, child_entry->flush_dep_parent, (child_entry->flush_dep_parent_nalloc / 4) * sizeof(H5C_cache_entry_t *))))
            HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "memory allocation failed for flush dependency parent list")
        child_entry->flush_dep_parent_nalloc /= 4;
    } /* end if */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_destroy_flush_dependency() */


/*************************************************************************/
/**************************** Private Functions: *************************/
/*************************************************************************/

/*-------------------------------------------------------------------------
 *
 * Function:	H5C__auto_adjust_cache_size
 *
 * Purpose:    	Obtain the current full cache hit rate, and compare it
 *		with the hit rate thresholds for modifying cache size.
 *		If one of the thresholds has been crossed, adjusts the
 *		size of the cache accordingly.
 *
 *		The function then resets the full cache hit rate
 *		statistics, and exits.
 *
 * Return:      Non-negative on success/Negative on failure or if there was
 *		an attempt to flush a protected item.
 *
 *
 * Programmer:  John Mainzer, 10/7/04
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__auto_adjust_cache_size(H5F_t * f,
                            hid_t dxpl_id,
                            hbool_t write_permitted)
{
    H5C_t *			cache_ptr = f->shared->cache;
    herr_t			result;
    hbool_t			reentrant_call = FALSE;
    hbool_t			inserted_epoch_marker = FALSE;
    size_t			new_max_cache_size = 0;
    size_t			old_max_cache_size = 0;
    size_t			new_min_clean_size = 0;
    size_t			old_min_clean_size = 0;
    double			hit_rate;
    enum H5C_resize_status	status = in_spec; /* will change if needed */
    herr_t			ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( f );
    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( cache_ptr->cache_accesses >=
              (cache_ptr->resize_ctl).epoch_length );
    HDassert( (double)0.0f <= (cache_ptr->resize_ctl).min_clean_fraction );
    HDassert( (cache_ptr->resize_ctl).min_clean_fraction <= (double)100.0f );

    /* check to see if cache_ptr->resize_in_progress is TRUE.  If it, this
     * is a re-entrant call via a client callback called in the resize 
     * process.  To avoid an infinite recursion, set reentrant_call to 
     * TRUE, and goto done.
     */
    if(cache_ptr->resize_in_progress) {
        reentrant_call = TRUE;
        HGOTO_DONE(SUCCEED);
    } /* end if */

    cache_ptr->resize_in_progress = TRUE;

    if ( !cache_ptr->resize_enabled ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Auto cache resize disabled.")
    }

    HDassert( ( (cache_ptr->resize_ctl).incr_mode != H5C_incr__off ) || \
              ( (cache_ptr->resize_ctl).decr_mode != H5C_decr__off ) );

    if ( H5C_get_cache_hit_rate(cache_ptr, &hit_rate) != SUCCEED ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Can't get hit rate.")
    }

    HDassert( ( (double)0.0f <= hit_rate ) && ( hit_rate <= (double)1.0f ) );

    switch ( (cache_ptr->resize_ctl).incr_mode )
    {
        case H5C_incr__off:
            if ( cache_ptr->size_increase_possible ) {

                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                           "size_increase_possible but H5C_incr__off?!?!?")
            }
            break;

        case H5C_incr__threshold:
            if ( hit_rate < (cache_ptr->resize_ctl).lower_hr_threshold ) {

                if ( ! cache_ptr->size_increase_possible ) {

                    status = increase_disabled;

                } else if ( cache_ptr->max_cache_size >=
                            (cache_ptr->resize_ctl).max_size ) {

                    HDassert( cache_ptr->max_cache_size == \
                              (cache_ptr->resize_ctl).max_size );
                    status = at_max_size;

                } else if ( ! cache_ptr->cache_full ) {

                    status = not_full;

                } else {

                    new_max_cache_size = (size_t)
                                     (((double)(cache_ptr->max_cache_size)) *
                                      (cache_ptr->resize_ctl).increment);

                    /* clip to max size if necessary */
                    if ( new_max_cache_size >
                         (cache_ptr->resize_ctl).max_size ) {

                        new_max_cache_size = (cache_ptr->resize_ctl).max_size;
                    }

                    /* clip to max increment if necessary */
                    if ( ( (cache_ptr->resize_ctl).apply_max_increment ) &&
                         ( (cache_ptr->max_cache_size +
                            (cache_ptr->resize_ctl).max_increment) <
                           new_max_cache_size ) ) {

                        new_max_cache_size = cache_ptr->max_cache_size +
                                         (cache_ptr->resize_ctl).max_increment;
                    }

                    status = increase;
                }
            }
            break;

        default:
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "unknown incr_mode.")
    }

    /* If the decr_mode is either age out or age out with threshold, we
     * must run the marker maintenance code, whether we run the size
     * reduction code or not.  We do this in two places -- here we
     * insert a new marker if the number of active epoch markers is
     * is less than the the current epochs before eviction, and after
     * the ageout call, we cycle the markers.
     *
     * However, we can't call the ageout code or cycle the markers
     * unless there was a full complement of markers in place on
     * entry.  The inserted_epoch_marker flag is used to track this.
     */

    if ( ( ( (cache_ptr->resize_ctl).decr_mode == H5C_decr__age_out )
           ||
           ( (cache_ptr->resize_ctl).decr_mode ==
              H5C_decr__age_out_with_threshold
           )
         )
         &&
         ( cache_ptr->epoch_markers_active <
           (cache_ptr->resize_ctl).epochs_before_eviction
         )
       ) {

        result = H5C__autoadjust__ageout__insert_new_marker(cache_ptr);

        if ( result != SUCCEED ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                        "can't insert new epoch marker.")

        } else {

            inserted_epoch_marker = TRUE;
        }
    }

    /* don't run the cache size decrease code unless the cache size
     * increase code is disabled, or the size increase code sees no need
     * for action.  In either case, status == in_spec at this point.
     */

    if ( status == in_spec ) {

        switch ( (cache_ptr->resize_ctl).decr_mode )
        {
            case H5C_decr__off:
                break;

            case H5C_decr__threshold:
                if ( hit_rate > (cache_ptr->resize_ctl).upper_hr_threshold ) {

                    if ( ! cache_ptr->size_decrease_possible ) {

                        status = decrease_disabled;

                    } else if ( cache_ptr->max_cache_size <=
                                (cache_ptr->resize_ctl).min_size ) {

                        HDassert( cache_ptr->max_cache_size ==
                                  (cache_ptr->resize_ctl).min_size );
                        status = at_min_size;

                    } else {

                        new_max_cache_size = (size_t)
                                 (((double)(cache_ptr->max_cache_size)) *
                                  (cache_ptr->resize_ctl).decrement);

                        /* clip to min size if necessary */
                        if ( new_max_cache_size <
                             (cache_ptr->resize_ctl).min_size ) {

                            new_max_cache_size =
                                (cache_ptr->resize_ctl).min_size;
                        }

                        /* clip to max decrement if necessary */
                        if ( ( (cache_ptr->resize_ctl).apply_max_decrement ) &&
                             ( ((cache_ptr->resize_ctl).max_decrement +
                                new_max_cache_size) <
                               cache_ptr->max_cache_size ) ) {

                            new_max_cache_size = cache_ptr->max_cache_size -
                                         (cache_ptr->resize_ctl).max_decrement;
                        }

                        status = decrease;
                    }
                }
                break;

            case H5C_decr__age_out_with_threshold:
            case H5C_decr__age_out:
                if ( ! inserted_epoch_marker ) {

                    if ( ! cache_ptr->size_decrease_possible ) {

                        status = decrease_disabled;

                    } else {

                        result = H5C__autoadjust__ageout(f,
                                                         dxpl_id,
                                                         hit_rate,
                                                         &status,
                                                         &new_max_cache_size,
                                                         write_permitted);

                        if ( result != SUCCEED ) {

                            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                                        "ageout code failed.")
                        }
                    }
                }
                break;

            default:
                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "unknown incr_mode.")
        }
    }

    /* cycle the epoch markers here if appropriate */
    if ( ( ( (cache_ptr->resize_ctl).decr_mode == H5C_decr__age_out )
           ||
           ( (cache_ptr->resize_ctl).decr_mode ==
              H5C_decr__age_out_with_threshold
           )
         )
         &&
         ( ! inserted_epoch_marker )
       ) {

        /* move last epoch marker to the head of the LRU list */
        result = H5C__autoadjust__ageout__cycle_epoch_marker(cache_ptr);

        if ( result != SUCCEED ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                        "error cycling epoch marker.")
        }
    }

    if ( ( status == increase ) || ( status == decrease ) ) {

        old_max_cache_size = cache_ptr->max_cache_size;
        old_min_clean_size = cache_ptr->min_clean_size;

        new_min_clean_size = (size_t)
                             ((double)new_max_cache_size *
                              ((cache_ptr->resize_ctl).min_clean_fraction));

        /* new_min_clean_size is of size_t, and thus must be non-negative.
         * Hence we have
         *
         * 	( 0 <= new_min_clean_size ).
         *
 	 * by definition.
         */
        HDassert( new_min_clean_size <= new_max_cache_size );
        HDassert( (cache_ptr->resize_ctl).min_size <= new_max_cache_size );
        HDassert( new_max_cache_size <= (cache_ptr->resize_ctl).max_size );

        cache_ptr->max_cache_size = new_max_cache_size;
        cache_ptr->min_clean_size = new_min_clean_size;

        if ( status == increase ) {

            cache_ptr->cache_full = FALSE;

        } else if ( status == decrease ) {

            cache_ptr->size_decreased = TRUE;
        }

	/* update flash cache size increase fields as appropriate */
	if ( cache_ptr->flash_size_increase_possible ) {

            switch ( (cache_ptr->resize_ctl).flash_incr_mode )
            {
                case H5C_flash_incr__off:

                    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                     "flash_size_increase_possible but H5C_flash_incr__off?!")
                    break;

                case H5C_flash_incr__add_space:
                    cache_ptr->flash_size_increase_threshold =
                        (size_t)
                        (((double)(cache_ptr->max_cache_size)) *
                         ((cache_ptr->resize_ctl).flash_threshold));
                     break;

                default: /* should be unreachable */
                    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                                "Unknown flash_incr_mode?!?!?.")
                    break;
            }
        }
    }

    if ( (cache_ptr->resize_ctl).rpt_fcn != NULL ) {

        (*((cache_ptr->resize_ctl).rpt_fcn))
            (cache_ptr,
             H5C__CURR_AUTO_RESIZE_RPT_FCN_VER,
             hit_rate,
             status,
             old_max_cache_size,
             new_max_cache_size,
             old_min_clean_size,
             new_min_clean_size);
    }

    if ( H5C_reset_cache_hit_rate_stats(cache_ptr) != SUCCEED ) {

        /* this should be impossible... */
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "H5C_reset_cache_hit_rate_stats failed.")
    }

done:
    /* Sanity checks */
    HDassert(cache_ptr->resize_in_progress);
    if(!reentrant_call)
        cache_ptr->resize_in_progress = FALSE;
    HDassert((!reentrant_call) || (cache_ptr->resize_in_progress));

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__auto_adjust_cache_size() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__autoadjust__ageout
 *
 * Purpose:     Implement the ageout automatic cache size decrement
 *		algorithm.  Note that while this code evicts aged out
 *		entries, the code does not change the maximum cache size.
 *		Instead, the function simply computes the new value (if
 *		any change is indicated) and reports this value in
 *		*new_max_cache_size_ptr.
 *
 * Return:      Non-negative on success/Negative on failure or if there was
 *              an attempt to flush a protected item.
 *
 *
 * Programmer:  John Mainzer, 11/18/04
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__autoadjust__ageout(H5F_t * f,
                        hid_t dxpl_id,
                        double hit_rate,
                        enum H5C_resize_status * status_ptr,
                        size_t * new_max_cache_size_ptr,
                        hbool_t write_permitted)
{
    H5C_t *     cache_ptr = f->shared->cache;
    herr_t	result;
    size_t	test_size;
    herr_t	ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( f );
    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( ( status_ptr ) && ( *status_ptr == in_spec ) );
    HDassert( ( new_max_cache_size_ptr ) && ( *new_max_cache_size_ptr == 0 ) );

    /* remove excess epoch markers if any */
    if ( cache_ptr->epoch_markers_active >
         (cache_ptr->resize_ctl).epochs_before_eviction ) {

        result = H5C__autoadjust__ageout__remove_excess_markers(cache_ptr);

        if ( result != SUCCEED ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                        "can't remove excess epoch markers.")
        }
    }

    if ( ( (cache_ptr->resize_ctl).decr_mode == H5C_decr__age_out )
         ||
         ( ( (cache_ptr->resize_ctl).decr_mode ==
              H5C_decr__age_out_with_threshold
               )
           &&
           ( hit_rate >= (cache_ptr->resize_ctl).upper_hr_threshold )
         )
       ) {

        if ( cache_ptr->max_cache_size > (cache_ptr->resize_ctl).min_size ){

            /* evict aged out cache entries if appropriate... */
            if(H5C__autoadjust__ageout__evict_aged_out_entries(f, dxpl_id, write_permitted) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "error flushing aged out entries.")

            /* ... and then reduce cache size if appropriate */
            if ( cache_ptr->index_size < cache_ptr->max_cache_size ) {

                if ( (cache_ptr->resize_ctl).apply_empty_reserve ) {

                    test_size = (size_t)(((double)cache_ptr->index_size) /
                                (1 - (cache_ptr->resize_ctl).empty_reserve));

                    if ( test_size < cache_ptr->max_cache_size ) {

                        *status_ptr = decrease;
                        *new_max_cache_size_ptr = test_size;
                    }
                } else {

                    *status_ptr = decrease;
                    *new_max_cache_size_ptr = cache_ptr->index_size;
                }

                if ( *status_ptr == decrease ) {

                    /* clip to min size if necessary */
                    if ( *new_max_cache_size_ptr <
                         (cache_ptr->resize_ctl).min_size ) {

                        *new_max_cache_size_ptr =
                                (cache_ptr->resize_ctl).min_size;
                    }

                    /* clip to max decrement if necessary */
                    if ( ( (cache_ptr->resize_ctl).apply_max_decrement ) &&
                         ( ((cache_ptr->resize_ctl).max_decrement +
                            *new_max_cache_size_ptr) <
                           cache_ptr->max_cache_size ) ) {

                        *new_max_cache_size_ptr = cache_ptr->max_cache_size -
                                         (cache_ptr->resize_ctl).max_decrement;
                    }
                }
            }
        } else {

            *status_ptr = at_min_size;
        }
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C__autoadjust__ageout() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__autoadjust__ageout__cycle_epoch_marker
 *
 * Purpose:     Remove the oldest epoch marker from the LRU list,
 *		and reinsert it at the head of the LRU list.  Also
 *		remove the epoch marker's index from the head of the
 *		ring buffer, and re-insert it at the tail of the ring
 *		buffer.
 *
 * Return:      SUCCEED on success/FAIL on failure.
 *
 * Programmer:  John Mainzer, 11/22/04
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__autoadjust__ageout__cycle_epoch_marker(H5C_t * cache_ptr)
{
    herr_t                      ret_value = SUCCEED;      /* Return value */
    int i;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    if ( cache_ptr->epoch_markers_active <= 0 ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "No active epoch markers on entry?!?!?.")
    }

    /* remove the last marker from both the ring buffer and the LRU list */

    i = cache_ptr->epoch_marker_ringbuf[cache_ptr->epoch_marker_ringbuf_first];

    cache_ptr->epoch_marker_ringbuf_first =
            (cache_ptr->epoch_marker_ringbuf_first + 1) %
            (H5C__MAX_EPOCH_MARKERS + 1);

    cache_ptr->epoch_marker_ringbuf_size -= 1;

    if ( cache_ptr->epoch_marker_ringbuf_size < 0 ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "ring buffer underflow.")
    }

    if ( (cache_ptr->epoch_marker_active)[i] != TRUE ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "unused marker in LRU?!?")
    }

    H5C__DLL_REMOVE((&((cache_ptr->epoch_markers)[i])), \
                    (cache_ptr)->LRU_head_ptr, \
                    (cache_ptr)->LRU_tail_ptr, \
                    (cache_ptr)->LRU_list_len, \
                    (cache_ptr)->LRU_list_size, \
                    (FAIL))

    /* now, re-insert it at the head of the LRU list, and at the tail of
     * the ring buffer.
     */

    HDassert( ((cache_ptr->epoch_markers)[i]).addr == (haddr_t)i );
    HDassert( ((cache_ptr->epoch_markers)[i]).next == NULL );
    HDassert( ((cache_ptr->epoch_markers)[i]).prev == NULL );

    cache_ptr->epoch_marker_ringbuf_last =
        (cache_ptr->epoch_marker_ringbuf_last + 1) %
        (H5C__MAX_EPOCH_MARKERS + 1);

    (cache_ptr->epoch_marker_ringbuf)[cache_ptr->epoch_marker_ringbuf_last] = i;

    cache_ptr->epoch_marker_ringbuf_size += 1;

    if ( cache_ptr->epoch_marker_ringbuf_size > H5C__MAX_EPOCH_MARKERS ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "ring buffer overflow.")
    }

    H5C__DLL_PREPEND((&((cache_ptr->epoch_markers)[i])), \
                     (cache_ptr)->LRU_head_ptr, \
                     (cache_ptr)->LRU_tail_ptr, \
                     (cache_ptr)->LRU_list_len, \
                     (cache_ptr)->LRU_list_size, \
                     (FAIL))
done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C__autoadjust__ageout__cycle_epoch_marker() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__autoadjust__ageout__evict_aged_out_entries
 *
 * Purpose:     Evict clean entries in the cache that haven't
 *		been accessed for at least
 *              (cache_ptr->resize_ctl).epochs_before_eviction epochs,
 *      	and flush dirty entries that haven't been accessed for
 *		that amount of time.
 *
 *		Depending on configuration, the function will either
 *		flush or evict all such entries, or all such entries it
 *		encounters until it has freed the maximum amount of space
 *		allowed under the maximum decrement.
 *
 *		If we are running in parallel mode, writes may not be
 *		permitted.  If so, the function simply skips any dirty
 *		entries it may encounter.
 *
 *		The function makes no attempt to maintain the minimum
 *		clean size, as there is no guarantee that the cache size
 *		will be changed.
 *
 *		If there is no cache size change, the minimum clean size
 *		constraint will be met through a combination of clean
 *		entries and free space in the cache.
 *
 *		If there is a cache size reduction, the minimum clean size
 *		will be re-calculated, and will be enforced the next time
 *		we have to make space in the cache.
 *
 *              The primary_dxpl_id and secondary_dxpl_id parameters
 *              specify the dxpl_ids used depending on the value of
 *		*first_flush_ptr.  The idea is to use the primary_dxpl_id
 *		on the first write in a sequence of writes, and to use
 *		the secondary_dxpl_id on all subsequent writes.
 *
 *              This is useful in the metadata cache, but may not be
 *		needed elsewhere.  If so, just use the same dxpl_id for
 *		both parameters.
 *
 *              Observe that this function cannot occasion a read.
 *
 * Return:      Non-negative on success/Negative on failure.
 *
 * Programmer:  John Mainzer, 11/22/04
 *
 * Changes:	Modified function to detect deletions of entries
 *              during a scan of the LRU, and where appropriate,
 *              restart the scan to avoid proceeding with a next
 *              entry that is no longer in the cache.
 *
 *              Note the absence of checks after flushes of clean
 *              entries.  As a second entry can only be removed by
 *              by a call to the pre_serialize or serialize callback
 *              of the first, and as these callbacks will not be called
 *              on clean entries, no checks are needed.
 *
 *                                              JRM -- 4/6/15
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__autoadjust__ageout__evict_aged_out_entries(H5F_t * f,
                                                hid_t   dxpl_id,
                                                hbool_t write_permitted)
{
    H5C_t *		cache_ptr = f->shared->cache;
    size_t		eviction_size_limit;
    size_t		bytes_evicted = 0;
    hbool_t		prev_is_dirty = FALSE;
    hbool_t             restart_scan;
    H5C_cache_entry_t * entry_ptr;
    H5C_cache_entry_t * next_ptr;
    H5C_cache_entry_t * prev_ptr;
    herr_t              ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( f );
    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    /* if there is a limit on the amount that the cache size can be decrease
     * in any one round of the cache size reduction algorithm, load that
     * limit into eviction_size_limit.  Otherwise, set eviction_size_limit
     * to the equivalent of infinity.  The current size of the index will
     * do nicely.
     */
    if ( (cache_ptr->resize_ctl).apply_max_decrement ) {

        eviction_size_limit = (cache_ptr->resize_ctl).max_decrement;

    } else {

        eviction_size_limit = cache_ptr->index_size; /* i.e. infinity */
    }

    if ( write_permitted ) {

        restart_scan = FALSE;
        entry_ptr = cache_ptr->LRU_tail_ptr;

        while ( ( entry_ptr != NULL ) &&
                ( (entry_ptr->type)->id != H5AC_EPOCH_MARKER_ID ) &&
                ( bytes_evicted < eviction_size_limit ) )
        {
	    hbool_t		corked = FALSE;

            HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
            HDassert( ! (entry_ptr->is_protected) );
            HDassert( ! (entry_ptr->is_read_only) );
            HDassert( (entry_ptr->ro_ref_count) == 0 );

	    next_ptr = entry_ptr->next;
            prev_ptr = entry_ptr->prev;

	    if(prev_ptr != NULL)
                prev_is_dirty = prev_ptr->is_dirty;

            if(entry_ptr->is_dirty ) {
                /* dirty corked entry is skipped */
                if(entry_ptr->tag_info && entry_ptr->tag_info->corked)
                    corked = TRUE;
                else {
                    /* reset entries_removed_counter and
                     * last_entry_removed_ptr prior to the call to
                     * H5C__flush_single_entry() so that we can spot
                     * unexpected removals of entries from the cache,
                     * and set the restart_scan flag if proceeding
                     * would be likely to cause us to scan an entry
                     * that is no longer in the cache.
                     */
                    cache_ptr->entries_removed_counter = 0;
                    cache_ptr->last_entry_removed_ptr  = NULL;

                    if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__NO_FLAGS_SET) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to flush entry")

                    if(cache_ptr->entries_removed_counter > 1 || cache_ptr->last_entry_removed_ptr == prev_ptr)
                        restart_scan = TRUE;
                } /* end else */
            } /* end if */
            else {

                bytes_evicted += entry_ptr->size;

                if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__FLUSH_INVALIDATE_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG) < 0 )
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to flush entry")
            }

            if ( prev_ptr != NULL ) {

		if(corked)   /* dirty corked entry is skipped */
                    entry_ptr = prev_ptr;

		else if ( ( restart_scan )
                     ||
                     ( prev_ptr->is_dirty != prev_is_dirty )
                     ||
                     ( prev_ptr->next != next_ptr )
                     ||
                     ( prev_ptr->is_protected )
                     ||
                     ( prev_ptr->is_pinned ) ) {

                    /* something has happened to the LRU -- start over
		     * from the tail.
                     */
                    restart_scan = FALSE;
                    entry_ptr = cache_ptr->LRU_tail_ptr;

		    H5C__UPDATE_STATS_FOR_LRU_SCAN_RESTART(cache_ptr)

                } else {

                    entry_ptr = prev_ptr;

                }
	    } else {

		entry_ptr = NULL;

	    }
        } /* end while */

        /* for now at least, don't bother to maintain the minimum clean size,
         * as the cache should now be less than its maximum size.  Due to
         * the vaguries of the cache size reduction algorthim, we may not
         * reduce the size of the cache.
         *
         * If we do, we will calculate a new minimum clean size, which will
         * be enforced the next time we try to make space in the cache.
         *
         * If we don't, no action is necessary, as we have just evicted and/or
         * or flushed a bunch of entries and therefore the sum of the clean
         * and free space in the cache must be greater than or equal to the
         * min clean space requirement (assuming that requirement was met on
         * entry).
         */

    } else /* ! write_permitted */  {

        /* since we are not allowed to write, all we can do is evict
         * any clean entries that we may encounter before we either
         * hit the eviction size limit, or encounter the epoch marker.
         *
         * If we are operating read only, this isn't an issue, as there
         * will not be any dirty entries.
         *
         * If we are operating in R/W mode, all the dirty entries we
         * skip will be flushed the next time we attempt to make space
         * when writes are permitted.  This may have some local
         * performance implications, but it shouldn't cause any net
         * slowdown.
         */

        HDassert( H5C_MAINTAIN_CLEAN_AND_DIRTY_LRU_LISTS );

        entry_ptr = cache_ptr->LRU_tail_ptr;

        while ( ( entry_ptr != NULL ) &&
                ( (entry_ptr->type)->id != H5AC_EPOCH_MARKER_ID ) &&
                ( bytes_evicted < eviction_size_limit ) )
        {
            HDassert( ! (entry_ptr->is_protected) );

            prev_ptr = entry_ptr->prev;

            if ( ! (entry_ptr->is_dirty) ) {
                if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__FLUSH_INVALIDATE_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to flush clean entry")
            }
            /* just skip the entry if it is dirty, as we can't do
             * anything with it now since we can't write.
	     *
	     * Since all entries are clean, serialize() will not be called,
	     * and thus we needn't test to see if the LRU has been changed
	     * out from under us.
             */

            entry_ptr = prev_ptr;

        } /* end while */
    }

    if ( cache_ptr->index_size < cache_ptr->max_cache_size ) {

        cache_ptr->cache_full = FALSE;
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C__autoadjust__ageout__evict_aged_out_entries() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__autoadjust__ageout__insert_new_marker
 *
 * Purpose:     Find an unused marker cache entry, mark it as used, and
 *		insert it at the head of the LRU list.  Also add the
 *		marker's index in the epoch_markers array.
 *
 * Return:      SUCCEED on success/FAIL on failure.
 *
 * Programmer:  John Mainzer, 11/19/04
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__autoadjust__ageout__insert_new_marker(H5C_t * cache_ptr)
{
    herr_t                      ret_value = SUCCEED;      /* Return value */
    int i;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    if ( cache_ptr->epoch_markers_active >=
         (cache_ptr->resize_ctl).epochs_before_eviction ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "Already have a full complement of markers.")
    }

    /* find an unused marker */
    i = 0;
    while ( ( (cache_ptr->epoch_marker_active)[i] ) &&
            ( i < H5C__MAX_EPOCH_MARKERS ) )
    {
        i++;
    }

    if(i >= H5C__MAX_EPOCH_MARKERS)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Can't find unused marker.")

    HDassert( ((cache_ptr->epoch_markers)[i]).addr == (haddr_t)i );
    HDassert( ((cache_ptr->epoch_markers)[i]).next == NULL );
    HDassert( ((cache_ptr->epoch_markers)[i]).prev == NULL );

    (cache_ptr->epoch_marker_active)[i] = TRUE;

    cache_ptr->epoch_marker_ringbuf_last =
        (cache_ptr->epoch_marker_ringbuf_last + 1) %
        (H5C__MAX_EPOCH_MARKERS + 1);

    (cache_ptr->epoch_marker_ringbuf)[cache_ptr->epoch_marker_ringbuf_last] = i;

    cache_ptr->epoch_marker_ringbuf_size += 1;

    if ( cache_ptr->epoch_marker_ringbuf_size > H5C__MAX_EPOCH_MARKERS ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "ring buffer overflow.")
    }

    H5C__DLL_PREPEND((&((cache_ptr->epoch_markers)[i])), \
                     (cache_ptr)->LRU_head_ptr, \
                     (cache_ptr)->LRU_tail_ptr, \
                     (cache_ptr)->LRU_list_len, \
                     (cache_ptr)->LRU_list_size, \
                     (FAIL))

    cache_ptr->epoch_markers_active += 1;

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C__autoadjust__ageout__insert_new_marker() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__autoadjust__ageout__remove_all_markers
 *
 * Purpose:     Remove all epoch markers from the LRU list and mark them
 *		as inactive.
 *
 * Return:      SUCCEED on success/FAIL on failure.
 *
 * Programmer:  John Mainzer, 11/22/04
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__autoadjust__ageout__remove_all_markers(H5C_t * cache_ptr)
{
    herr_t                      ret_value = SUCCEED;      /* Return value */
    int i;
    int ring_buf_index;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    while ( cache_ptr->epoch_markers_active > 0 )
    {
        /* get the index of the last epoch marker in the LRU list
         * and remove it from the ring buffer.
         */

        ring_buf_index = cache_ptr->epoch_marker_ringbuf_first;
        i = (cache_ptr->epoch_marker_ringbuf)[ring_buf_index];

        cache_ptr->epoch_marker_ringbuf_first =
            (cache_ptr->epoch_marker_ringbuf_first + 1) %
            (H5C__MAX_EPOCH_MARKERS + 1);

        cache_ptr->epoch_marker_ringbuf_size -= 1;

        if ( cache_ptr->epoch_marker_ringbuf_size < 0 ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "ring buffer underflow.")
        }

        if ( (cache_ptr->epoch_marker_active)[i] != TRUE ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "unused marker in LRU?!?")
        }

        /* remove the epoch marker from the LRU list */
        H5C__DLL_REMOVE((&((cache_ptr->epoch_markers)[i])), \
                        (cache_ptr)->LRU_head_ptr, \
                        (cache_ptr)->LRU_tail_ptr, \
                        (cache_ptr)->LRU_list_len, \
                        (cache_ptr)->LRU_list_size, \
                        (FAIL))

        /* mark the epoch marker as unused. */
        (cache_ptr->epoch_marker_active)[i] = FALSE;

        HDassert( ((cache_ptr->epoch_markers)[i]).addr == (haddr_t)i );
        HDassert( ((cache_ptr->epoch_markers)[i]).next == NULL );
        HDassert( ((cache_ptr->epoch_markers)[i]).prev == NULL );

        /* decrement the number of active epoch markers */
        cache_ptr->epoch_markers_active -= 1;

        HDassert( cache_ptr->epoch_markers_active == \
                  cache_ptr->epoch_marker_ringbuf_size );
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C__autoadjust__ageout__remove_all_markers() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__autoadjust__ageout__remove_excess_markers
 *
 * Purpose:     Remove epoch markers from the end of the LRU list and
 *		mark them as inactive until the number of active markers
 *		equals the the current value of
 *		(cache_ptr->resize_ctl).epochs_before_eviction.
 *
 * Return:      SUCCEED on success/FAIL on failure.
 *
 * Programmer:  John Mainzer, 11/19/04
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__autoadjust__ageout__remove_excess_markers(H5C_t * cache_ptr)
{
    herr_t	ret_value = SUCCEED;      /* Return value */
    int		i;
    int		ring_buf_index;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    if ( cache_ptr->epoch_markers_active <=
         (cache_ptr->resize_ctl).epochs_before_eviction ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "no excess markers on entry.")
    }

    while ( cache_ptr->epoch_markers_active >
            (cache_ptr->resize_ctl).epochs_before_eviction )
    {
        /* get the index of the last epoch marker in the LRU list
         * and remove it from the ring buffer.
         */

        ring_buf_index = cache_ptr->epoch_marker_ringbuf_first;
        i = (cache_ptr->epoch_marker_ringbuf)[ring_buf_index];

        cache_ptr->epoch_marker_ringbuf_first =
            (cache_ptr->epoch_marker_ringbuf_first + 1) %
            (H5C__MAX_EPOCH_MARKERS + 1);

        cache_ptr->epoch_marker_ringbuf_size -= 1;

        if ( cache_ptr->epoch_marker_ringbuf_size < 0 ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "ring buffer underflow.")
        }

        if ( (cache_ptr->epoch_marker_active)[i] != TRUE ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "unused marker in LRU?!?")
        }

        /* remove the epoch marker from the LRU list */
        H5C__DLL_REMOVE((&((cache_ptr->epoch_markers)[i])), \
                        (cache_ptr)->LRU_head_ptr, \
                        (cache_ptr)->LRU_tail_ptr, \
                        (cache_ptr)->LRU_list_len, \
                        (cache_ptr)->LRU_list_size, \
                        (FAIL))

        /* mark the epoch marker as unused. */
        (cache_ptr->epoch_marker_active)[i] = FALSE;

        HDassert( ((cache_ptr->epoch_markers)[i]).addr == (haddr_t)i );
        HDassert( ((cache_ptr->epoch_markers)[i]).next == NULL );
        HDassert( ((cache_ptr->epoch_markers)[i]).prev == NULL );

        /* decrement the number of active epoch markers */
        cache_ptr->epoch_markers_active -= 1;

        HDassert( cache_ptr->epoch_markers_active == \
                  cache_ptr->epoch_marker_ringbuf_size );
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C__autoadjust__ageout__remove_excess_markers() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__flash_increase_cache_size
 *
 * Purpose:     If there is not at least new_entry_size - old_entry_size
 *              bytes of free space in the cache and the current
 *              max_cache_size is less than (cache_ptr->resize_ctl).max_size,
 *              perform a flash increase in the cache size and then reset
 *              the full cache hit rate statistics, and exit.
 *
 * Return:      Non-negative on success/Negative on failure.
 *
 * Programmer:  John Mainzer, 12/31/07
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__flash_increase_cache_size(H5C_t * cache_ptr,
                               size_t old_entry_size,
                               size_t new_entry_size)
{
    size_t                     new_max_cache_size = 0;
    size_t                     old_max_cache_size = 0;
    size_t                     new_min_clean_size = 0;
    size_t                     old_min_clean_size = 0;
    size_t                     space_needed;
    enum H5C_resize_status     status = flash_increase;  /* may change */
    double                     hit_rate;
    herr_t                     ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( cache_ptr->flash_size_increase_possible );
    HDassert( new_entry_size > cache_ptr->flash_size_increase_threshold );
    HDassert( old_entry_size < new_entry_size );

    if ( old_entry_size >= new_entry_size ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "old_entry_size >= new_entry_size")
    }

    space_needed = new_entry_size - old_entry_size;

    if ( ( (cache_ptr->index_size + space_needed) >
                            cache_ptr->max_cache_size ) &&
         ( cache_ptr->max_cache_size < (cache_ptr->resize_ctl).max_size ) ) {

        /* we have work to do */

        switch ( (cache_ptr->resize_ctl).flash_incr_mode )
        {
            case H5C_flash_incr__off:
                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                   "flash_size_increase_possible but H5C_flash_incr__off?!")
                break;

            case H5C_flash_incr__add_space:
                if ( cache_ptr->index_size < cache_ptr->max_cache_size ) {

                    HDassert( (cache_ptr->max_cache_size - cache_ptr->index_size)
                               < space_needed );
                    space_needed -= cache_ptr->max_cache_size -
			            cache_ptr->index_size;
                }
                space_needed =
                    (size_t)(((double)space_needed) *
                             (cache_ptr->resize_ctl).flash_multiple);

                new_max_cache_size = cache_ptr->max_cache_size + space_needed;

                break;

            default: /* should be unreachable */
                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                            "Unknown flash_incr_mode?!?!?.")
                break;
        }

        if ( new_max_cache_size > (cache_ptr->resize_ctl).max_size ) {

            new_max_cache_size = (cache_ptr->resize_ctl).max_size;
        }

        HDassert( new_max_cache_size > cache_ptr->max_cache_size );

        new_min_clean_size = (size_t)
                             ((double)new_max_cache_size *
                              ((cache_ptr->resize_ctl).min_clean_fraction));

        HDassert( new_min_clean_size <= new_max_cache_size );

        old_max_cache_size = cache_ptr->max_cache_size;
        old_min_clean_size = cache_ptr->min_clean_size;

        cache_ptr->max_cache_size = new_max_cache_size;
        cache_ptr->min_clean_size = new_min_clean_size;

        /* update flash cache size increase fields as appropriate */
        HDassert ( cache_ptr->flash_size_increase_possible );

        switch ( (cache_ptr->resize_ctl).flash_incr_mode )
        {
            case H5C_flash_incr__off:
                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                    "flash_size_increase_possible but H5C_flash_incr__off?!")
                break;

            case H5C_flash_incr__add_space:
                cache_ptr->flash_size_increase_threshold =
                    (size_t)
                    (((double)(cache_ptr->max_cache_size)) *
                     ((cache_ptr->resize_ctl).flash_threshold));
                break;

            default: /* should be unreachable */
                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                            "Unknown flash_incr_mode?!?!?.")
                break;
        }

        /* note that we don't cycle the epoch markers.  We can
	 * argue either way as to whether we should, but for now
	 * we don't.
	 */

        if ( (cache_ptr->resize_ctl).rpt_fcn != NULL ) {

            /* get the hit rate for the reporting function.  Should still
             * be good as we havent reset the hit rate statistics.
             */
            if ( H5C_get_cache_hit_rate(cache_ptr, &hit_rate) != SUCCEED ) {

                HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Can't get hit rate.")
            }

            (*((cache_ptr->resize_ctl).rpt_fcn))
                (cache_ptr,
                 H5C__CURR_AUTO_RESIZE_RPT_FCN_VER,
                 hit_rate,
                 status,
                 old_max_cache_size,
                 new_max_cache_size,
                 old_min_clean_size,
                 new_min_clean_size);
        }

        if ( H5C_reset_cache_hit_rate_stats(cache_ptr) != SUCCEED ) {

            /* this should be impossible... */
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, \
                        "H5C_reset_cache_hit_rate_stats failed.")
        }
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C__flash_increase_cache_size() */


/*-------------------------------------------------------------------------
 * Function:    H5C_flush_invalidate_cache
 *
 * Purpose:	Flush and destroy the entries contained in the target
 *		cache.
 *
 *		If the cache contains protected entries, the function will
 *		fail, as protected entries cannot be either flushed or
 *		destroyed.  However all unprotected entries should be
 *		flushed and destroyed before the function returns failure.
 *
 *		While pinned entries can usually be flushed, they cannot
 *		be destroyed.  However, they should be unpinned when all
 *		the entries that reference them have been destroyed (thus
 *		reduding the pinned entry's reference count to 0, allowing
 *		it to be unpinned).
 *
 *		If pinned entries are present, the function makes repeated
 *		passes through the cache, flushing all dirty entries
 *		(including the pinned dirty entries where permitted) and
 *		destroying all unpinned entries.  This process is repeated
 *		until either the cache is empty, or the number of pinned
 *		entries stops decreasing on each pass.
 *
 *		The primary_dxpl_id and secondary_dxpl_id parameters
 *		specify the dxpl_ids used on the first write occasioned
 *		by the flush (primary_dxpl_id), and on all subsequent
 *		writes (secondary_dxpl_id).
 *
 * Return:      Non-negative on success/Negative on failure or if there was
 *		a request to flush all items and something was protected.
 *
 * Programmer:  John Mainzer
 *		3/24/065
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C_flush_invalidate_cache(const H5F_t * f, hid_t dxpl_id, unsigned flags)
{
    H5C_t *		cache_ptr;
    H5C_ring_t		ring;
    herr_t		ret_value = SUCCEED;

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f);
    HDassert(f->shared);
    cache_ptr = f->shared->cache;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(cache_ptr->slist_ptr);

#if H5C_DO_SANITY_CHECKS
{
    int32_t		i;
    int32_t		index_len = 0;
    int32_t		slist_len = 0;
    size_t		index_size = (size_t)0;
    size_t		clean_index_size = (size_t)0;
    size_t		dirty_index_size = (size_t)0;
    size_t		slist_size = (size_t)0;

    HDassert(cache_ptr->index_ring_len[H5C_RING_UNDEFINED] == 0);
    HDassert(cache_ptr->index_ring_size[H5C_RING_UNDEFINED] == (size_t)0);
    HDassert(cache_ptr->clean_index_ring_size[H5C_RING_UNDEFINED] == (size_t)0);
    HDassert(cache_ptr->dirty_index_ring_size[H5C_RING_UNDEFINED] == (size_t)0);
    HDassert(cache_ptr->slist_ring_len[H5C_RING_UNDEFINED] == 0);
    HDassert(cache_ptr->slist_ring_size[H5C_RING_UNDEFINED] == (size_t)0);

    for(i = H5C_RING_USER; i < H5C_RING_NTYPES; i++) {
        index_len += cache_ptr->index_ring_len[i];
        index_size += cache_ptr->index_ring_size[i];
        clean_index_size += cache_ptr->clean_index_ring_size[i];
        dirty_index_size += cache_ptr->dirty_index_ring_size[i];

        slist_len += cache_ptr->slist_ring_len[i];
        slist_size += cache_ptr->slist_ring_size[i];
    } /* end for */

    HDassert(cache_ptr->index_len == index_len);
    HDassert(cache_ptr->index_size == index_size);
    HDassert(cache_ptr->clean_index_size == clean_index_size);
    HDassert(cache_ptr->dirty_index_size == dirty_index_size);
    HDassert(cache_ptr->slist_len == slist_len);
    HDassert(cache_ptr->slist_size == slist_size);
}
#endif /* H5C_DO_SANITY_CHECKS */

    /* remove ageout markers if present */
    if(cache_ptr->epoch_markers_active > 0)
        if(H5C__autoadjust__ageout__remove_all_markers(cache_ptr) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "error removing all epoch markers.")

    /* flush invalidate each ring, starting from the outermost ring and
     * working inward.
     */
    ring = H5C_RING_USER;
    while(ring < H5C_RING_NTYPES) {
        if(H5C_flush_invalidate_ring(f, dxpl_id, ring, flags) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "flush invalidate ring failed.")
        ring++;
    } /* end while */

    /* Invariants, after destroying all entries in the hash table */
    if(!(flags & H5C__EVICT_ALLOW_LAST_PINS_FLAG)) {
        HDassert(cache_ptr->index_size == 0);
        HDassert(cache_ptr->clean_index_size == 0);
        HDassert(cache_ptr->pel_len == 0);
        HDassert(cache_ptr->pel_size == 0);
    } /* end if */
    else {
        H5C_cache_entry_t *entry_ptr;   /* Cache entry */
        unsigned u;                     /* Local index variable */

        /* All rings except ring 4 should be empty now */
        /* (Ring 4 has the superblock) */
        for(u = H5C_RING_USER; u < H5C_RING_SB; u++) {
            HDassert(cache_ptr->index_ring_len[u] == 0);
            HDassert(cache_ptr->index_ring_size[u] == 0);
            HDassert(cache_ptr->clean_index_ring_size[u] == 0);
        } /* end for */

        /* Check that any remaining pinned entries are in the superblock ring */
        entry_ptr = cache_ptr->pel_head_ptr;
        while(entry_ptr) {
            /* Check ring */
            HDassert(entry_ptr->ring == H5C_RING_SB);
            
            /* Advance to next entry in pinned entry list */
            entry_ptr = entry_ptr->next;
        } /* end while */
    } /* end else */
    HDassert(cache_ptr->dirty_index_size == 0);
    HDassert(cache_ptr->slist_len == 0);
    HDassert(cache_ptr->slist_size == 0);
    HDassert(cache_ptr->pl_len == 0);
    HDassert(cache_ptr->pl_size == 0);
    HDassert(cache_ptr->LRU_list_len == 0);
    HDassert(cache_ptr->LRU_list_size == 0);

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_flush_invalidate_cache() */


/*-------------------------------------------------------------------------
 * Function:    H5C_flush_invalidate_ring
 *
 * Purpose:	Flush and destroy the entries contained in the target
 *		cache and ring.
 *
 *		If the ring contains protected entries, the function will
 *		fail, as protected entries cannot be either flushed or
 *		destroyed.  However all unprotected entries should be
 *		flushed and destroyed before the function returns failure.
 *
 *		While pinned entries can usually be flushed, they cannot
 *		be destroyed.  However, they should be unpinned when all
 *		the entries that reference them have been destroyed (thus
 *		reduding the pinned entry's reference count to 0, allowing
 *		it to be unpinned).
 *
 *		If pinned entries are present, the function makes repeated
 *		passes through the cache, flushing all dirty entries
 *		(including the pinned dirty entries where permitted) and
 *		destroying all unpinned entries.  This process is repeated
 *		until either the cache is empty, or the number of pinned
 *		entries stops decreasing on each pass.
 *
 *		If flush dependencies appear in the target ring, the 
 *		function makes repeated passes through the cache flushing
 *		entries in flush dependency order.
 *
 * Return:      Non-negative on success/Negative on failure or if there was
 *		a request to flush all items and something was protected.
 *
 * Programmer:  John Mainzer
 *		9/1/15
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C_flush_invalidate_ring(const H5F_t * f, hid_t dxpl_id, H5C_ring_t ring,
    unsigned flags)
{
    H5C_t              *cache_ptr;
    hbool_t             restart_slist_scan;
    int32_t             protected_entries = 0;
    int32_t             i;
    int32_t             cur_ring_pel_len;
    int32_t             old_ring_pel_len;
    unsigned            cooked_flags;
    unsigned            evict_flags;
    H5SL_node_t        *node_ptr = NULL;
    H5C_cache_entry_t  *entry_ptr = NULL;
    H5C_cache_entry_t  *next_entry_ptr = NULL;
#if H5C_DO_SANITY_CHECKS
    int64_t             initial_slist_len = 0;
    size_t              initial_slist_size = 0;
#endif /* H5C_DO_SANITY_CHECKS */
    herr_t              ret_value = SUCCEED;

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(f);
    HDassert(f->shared);
    cache_ptr = f->shared->cache;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(cache_ptr->slist_ptr);
    HDassert(ring > H5C_RING_UNDEFINED);
    HDassert(ring < H5C_RING_NTYPES);

    HDassert(cache_ptr->epoch_markers_active == 0);

    /* Filter out the flags that are not relevant to the flush/invalidate.
     */
    cooked_flags = flags & H5C__FLUSH_CLEAR_ONLY_FLAG;
    evict_flags = flags & H5C__EVICT_ALLOW_LAST_PINS_FLAG;

    /* The flush proceedure here is a bit strange.
     *
     * In the outer while loop we make at least one pass through the
     * cache, and then repeat until either all the pinned entries in 
     * the ring unpin themselves, or until the number of pinned entries 
     * in the ring stops declining.  In this later case, we scream and die.
     *
     * Since the fractal heap can dirty, resize, and/or move entries
     * in is flush callback, it is possible that the cache will still
     * contain dirty entries at this point.  If so, we must make more
     * passes through the skip list to allow it to empty.
     *
     * Further, since clean entries can be dirtied, resized, and/or moved
     * as the result of a flush call back (either the entries own, or that
     * for some other cache entry), we can no longer promise to flush
     * the cache entries in increasing address order.
     *
     * Instead, we just do the best we can -- making a pass through
     * the skip list, and then a pass through the "clean" entries, and
     * then repeating as needed.  Thus it is quite possible that an
     * entry will be evicted from the cache only to be re-loaded later
     * in the flush process (From what Quincey tells me, the pin
     * mechanism makes this impossible, but even it it is true now,
     * we shouldn't count on it in the future.)
     *
     * The bottom line is that entries will probably be flushed in close
     * to increasing address order, but there are no guarantees.
     */

    /* compute the number of pinned entries in this ring */
    entry_ptr = cache_ptr->pel_head_ptr;
    cur_ring_pel_len = 0;
    while(entry_ptr != NULL) {
        HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
        HDassert(entry_ptr->ring >= ring);
        if(entry_ptr->ring == ring)
            cur_ring_pel_len++;

        entry_ptr = entry_ptr->next;
    } /* end while */

    old_ring_pel_len = cur_ring_pel_len;
    while(cache_ptr->index_ring_len[ring] > 0) {
        /* first, try to flush-destroy any dirty entries.   Do this by
         * making a scan through the slist.  Note that new dirty entries
         * may be created by the flush call backs.  Thus it is possible
         * that the slist will not be empty after we finish the scan.
         */

#if H5C_DO_SANITY_CHECKS
        /* Depending on circumstances, H5C__flush_single_entry() will
         * remove dirty entries from the slist as it flushes them.
         * Thus for sanity checks we must make note of the initial
         * slist length and size before we do any flushes.
         */
        initial_slist_len = cache_ptr->slist_len;
        initial_slist_size = cache_ptr->slist_size;

        /* There is also the possibility that entries will be
         * dirtied, resized, moved, and/or removed from the cache
         * as the result of calls to the flush callbacks.  We use 
         * the slist_len_increase and slist_size_increase increase 
         * fields in struct H5C_t to track these changes for purpose 
         * of sanity checking.
         *
         * To this end, we must zero these fields before we start
         * the pass through the slist.
         */
        cache_ptr->slist_len_increase = 0;
        cache_ptr->slist_size_increase = 0;
#endif /* H5C_DO_SANITY_CHECKS */

        /* Set the cache_ptr->slist_changed to false.
         *
         * This flag is set to TRUE by H5C__flush_single_entry if the slist
         * is modified by a pre_serialize, serialize, or notify callback.
         *
         * H5C_flush_invalidate_ring() uses this flag to detect any 
         * modifications to the slist that might corrupt the scan of 
         * the slist -- and restart the scan in this event.
         */
        cache_ptr->slist_changed = FALSE;

        /* this done, start the scan of the slist */
        restart_slist_scan = TRUE;
        while(restart_slist_scan || (node_ptr != NULL)) {
            if(restart_slist_scan) {
                restart_slist_scan = FALSE;

                /* Start at beginning of skip list */
                node_ptr = H5SL_first(cache_ptr->slist_ptr);
                if(node_ptr == NULL)
                    /* the slist is empty -- break out of inner loop */
                    break;

                /* Get cache entry for this node */
                next_entry_ptr = (H5C_cache_entry_t *)H5SL_item(node_ptr);
                if(NULL == next_entry_ptr)
                    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "next_entry_ptr == NULL ?!?!")

                HDassert(next_entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
                HDassert(next_entry_ptr->is_dirty);
                HDassert(next_entry_ptr->in_slist);
                HDassert(next_entry_ptr->ring >= ring);
            } /* end if */

            entry_ptr = next_entry_ptr;

            /* It is possible that entries will be dirtied, resized, 
             * flushed, or removed from the cache via the take ownership
             * flag as the result of pre_serialize or serialized callbacks. 
             * 
             * This in turn can corrupt the scan through the slist.
             *
             * We test for slist modifications in the pre_serialize 
             * and serialize callbacks, and restart the scan of the 
             * slist if we find them.  However, best we do some extra
             * sanity checking just in case.
             */
            HDassert(entry_ptr != NULL);
            HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
            HDassert(entry_ptr->in_slist);
            HDassert(entry_ptr->is_dirty);
            HDassert(entry_ptr->ring >= ring);

            /* increment node pointer now, before we delete its target
             * from the slist.
             */
            node_ptr = H5SL_next(node_ptr);
            if(node_ptr != NULL) {
                next_entry_ptr = (H5C_cache_entry_t *)H5SL_item(node_ptr);
                if(NULL == next_entry_ptr)
                    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "next_entry_ptr == NULL ?!?!")
                HDassert(next_entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
                HDassert(next_entry_ptr->is_dirty);
                HDassert(next_entry_ptr->in_slist);
                HDassert(next_entry_ptr->ring >= ring);
                HDassert(entry_ptr != next_entry_ptr);
            } /* end if */
            else
                next_entry_ptr = NULL;

            /* Note that we now remove nodes from the slist as we flush
             * the associated entries, instead of leaving them there
             * until we are done, and then destroying all nodes in
             * the slist.
             *
             * While this optimization used to be easy, with the possibility
             * of new entries being added to the slist in the midst of the
             * flush, we must keep the slist in cannonical form at all
             * times.
             */
            if(((!entry_ptr->flush_me_last) ||
                    ((entry_ptr->flush_me_last) &&
                        (cache_ptr->num_last_entries >= cache_ptr->slist_len))) &&
                    (entry_ptr->flush_dep_nchildren == 0) &&
                    (entry_ptr->ring == ring)) {
                if(entry_ptr->is_protected) {
                    /* we have major problems -- but lets flush
                     * everything we can before we flag an error.
                     */
                    protected_entries++;
                } else if(entry_ptr->is_pinned) {
                    if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__DURING_FLUSH_FLAG) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "dirty pinned entry flush failed.")

                    if(cache_ptr->slist_changed) {
                        /* The slist has been modified by something
                         * other than the simple removal of the
                         * of the flushed entry after the flush.
                         *
                         * This has the potential to corrupt the
                         * scan through the slist, so restart it.
                         */
                        restart_slist_scan = TRUE;
                        cache_ptr->slist_changed = FALSE;
                        H5C__UPDATE_STATS_FOR_SLIST_SCAN_RESTART(cache_ptr);
                    } /* end if */
                } /* end if */
                else {
                    if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, (cooked_flags | H5C__DURING_FLUSH_FLAG | H5C__FLUSH_INVALIDATE_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG)) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "dirty entry flush destroy failed.")

                    if(cache_ptr->slist_changed) {
                        /* The slist has been modified by something
                         * other than the simple removal of the
                         * of the flushed entry after the flush.
                         *
                         * This has the potential to corrupt the
                         * scan through the slist, so restart it.
                         */
                        restart_slist_scan = TRUE;
                        cache_ptr->slist_changed = FALSE;
                        H5C__UPDATE_STATS_FOR_SLIST_SCAN_RESTART(cache_ptr)
                    } /* end if */
                } /* end else */
            } /* end if */
        } /* end while loop scanning skip list */

#if H5C_DO_SANITY_CHECKS
        /* It is possible that entries were added to the slist during
         * the scan, either before or after scan pointer.  The following
         * asserts take this into account.
         *
         * Don't bother with the sanity checks if node_ptr != NULL, as
         * in this case we broke out of the loop because it got changed
         * out from under us.
         */

        if(node_ptr == NULL) {
            HDassert(cache_ptr->slist_len == (initial_slist_len + cache_ptr->slist_len_increase));
            HDassert((int64_t)cache_ptr->slist_size == ((int64_t)initial_slist_size + cache_ptr->slist_size_increase));
        } /* end if */
#endif /* H5C_DO_SANITY_CHECKS */

        /* Since we are doing a destroy, we must make a pass through
         * the hash table and try to flush - destroy all entries that
         * remain.
         *
         * It used to be that all entries remaining in the cache at
         * this point had to be clean, but with the fractal heap mods
         * this may not be the case.  If so, we will flush entries out
         * in increasing address order.
         *
         * Writes to disk are possible here.
         */

        /* reset the counters so that we can detect insertions, loads,
         * and moves caused by the pre_serialize and serialize calls.
         */
        cache_ptr->entries_loaded_counter         = 0;
        cache_ptr->entries_inserted_counter       = 0;
        cache_ptr->entries_relocated_counter      = 0;

        next_entry_ptr = cache_ptr->il_head;

        while(next_entry_ptr != NULL) {

            entry_ptr = next_entry_ptr;
            HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
            HDassert(entry_ptr->ring >= ring);

            next_entry_ptr = entry_ptr->il_next;

            HDassert((next_entry_ptr == NULL) ||
                     (next_entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC));

            if ( ( ( ! entry_ptr->flush_me_last ) 
                   ||
                   ( ( entry_ptr->flush_me_last ) 
                     &&
                     ( cache_ptr->num_last_entries >= cache_ptr->slist_len ) 
                   )
                 ) 
                 &&
                 ( entry_ptr->flush_dep_nchildren == 0 ) 
                 &&
                 ( entry_ptr->ring == ring )
               ) {

                if(entry_ptr->is_protected) {
                    /* we have major problems -- but lets flush and 
                     * destroy everything we can before we flag an 
                     * error.
                     */
                    protected_entries++;

                    if(!entry_ptr->in_slist)
                        HDassert(!(entry_ptr->is_dirty));

                } else if(!(entry_ptr->is_pinned)) {

                    /* if *entry_ptr is dirty, it is possible 
                     * that one or more other entries may be 
                     * either removed from the cache, loaded 
                     * into the cache, or moved to a new location
                     * in the file as a side effect of the flush.
                     *
                     * It's also possible that removing a clean
                     * entry will remove the last child of a proxy
                     * entry, allowing it to be removed also and
                     * invalidating the next_entry_ptr.
                     *
                     * If either of these happen, and one of the target 
                     * or proxy entries happens to be the next entry in 
                     * the hash bucket, we could either find ourselves
                     * either scanning a non-existant entry, scanning
                     * through a different bucket, or skipping an entry.
                     *
                     * Neither of these are good, so restart the 
                     * the scan at the head of the hash bucket 
                     * after the flush if we detect that the next_entry_ptr
                     * becomes invalid.
                     *
                     * This is not as inefficient at it might seem,
                     * as hash buckets typically have at most two
                     * or three entries.
                     */
                    cache_ptr->entry_watched_for_removal = next_entry_ptr;

                   if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, 
                                   (cooked_flags 
                                       | H5C__DURING_FLUSH_FLAG 
                                       | H5C__FLUSH_INVALIDATE_FLAG 
                                       | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG)) < 0)
                       HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, \
                                   "Entry flush destroy failed.")

                    /* restart the index list scan if necessary.  Must
                     * do this if the next entry is evicted, and also if 
                     * one or more entries are inserted, loaded, or moved
                     * as these operations can result in part of the scan
                     * being skipped -- which can cause a spurious failure
                     * if this results in the size of the pinned entry 
                     * failing to decline during the pass.
                     */
                    if ( ( ( NULL != next_entry_ptr ) &&
                           ( NULL == cache_ptr->entry_watched_for_removal ) ) ||
                         ( cache_ptr->entries_loaded_counter > 0 ) ||
                         ( cache_ptr->entries_inserted_counter > 0 ) ||
                         ( cache_ptr->entries_relocated_counter > 0 ) ) {

                        next_entry_ptr = cache_ptr->il_head;

                        cache_ptr->entries_loaded_counter         = 0;
                        cache_ptr->entries_inserted_counter       = 0;
                        cache_ptr->entries_relocated_counter      = 0;

                        H5C__UPDATE_STATS_FOR_INDEX_SCAN_RESTART(cache_ptr)
                    } /* end if */
                    else
                       cache_ptr->entry_watched_for_removal = NULL;
                } /* end if */
            } /* end if */
        } /* end for loop scanning hash table */

        /* We can't do anything if entries are pinned.  The
         * hope is that the entries will be unpinned as the
         * result of destroys of entries that reference them.
         *
         * We detect this by noting the change in the number
         * of pinned entries from pass to pass.  If it stops
         * shrinking before it hits zero, we scream and die.
         */
        old_ring_pel_len = cur_ring_pel_len;
        entry_ptr = cache_ptr->pel_head_ptr;
        cur_ring_pel_len = 0;

        while(entry_ptr != NULL) {

            HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
            HDassert(entry_ptr->ring >= ring);

            if(entry_ptr->ring == ring)
                cur_ring_pel_len++;

            entry_ptr = entry_ptr->next;
        } /* end while */

        if((cur_ring_pel_len > 0) && (cur_ring_pel_len >= old_ring_pel_len)) {

            /* Don't error if allowed to have pinned entries remaining */
            if(evict_flags)
                HGOTO_DONE(TRUE)

           /* The number of pinned entries in the ring is positive, and 
            * it is not declining.  Scream and die.
            */
            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Pinned entry count not decreasing, cur_ring_pel_len = %d, old_ring_pel_len = %d, ring = %d", (int)cur_ring_pel_len, (int)old_ring_pel_len, (int)ring)
        } /* end if */

        HDassert(protected_entries == cache_ptr->pl_len);

        if ( ( protected_entries > 0 ) && 
             ( protected_entries == cache_ptr->index_len ) )

            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, \
                "Only protected entries left in cache, protected_entries = %d", \
                (int)protected_entries)
    } /* main while loop */

    /* Invariants, after destroying all entries in the ring */
    for(i = (int)H5C_RING_UNDEFINED; i <= (int)ring; i++) {
        HDassert(cache_ptr->index_ring_len[i] == 0);
        HDassert(cache_ptr->index_ring_size[i] == (size_t)0);
        HDassert(cache_ptr->clean_index_ring_size[i] == (size_t)0);
        HDassert(cache_ptr->dirty_index_ring_size[i] == (size_t)0);

        HDassert(cache_ptr->slist_ring_len[i] == 0);
        HDassert(cache_ptr->slist_ring_size[i] == (size_t)0);
    } /* end for */

    HDassert(protected_entries <= cache_ptr->pl_len);

    if(protected_entries > 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Cache has protected entries.")
    else if(cur_ring_pel_len > 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Can't unpin all pinned entries in ring.")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_flush_invalidate_ring() */


/*-------------------------------------------------------------------------
 * Function:    H5C_flush_ring
 *
 * Purpose:	Flush the entries contained in the specified cache and 
 *		ring.  All entries in rings outside the specified ring
 *		must have been flushed on entry.
 *
 *		If the cache contains protected entries in the specified
 *		ring, the function will fail, as protected entries cannot 
 *		be flushed.  However all unprotected entries in the target
 *		ring should be flushed before the function returns failure.
 *
 *		If flush dependencies appear in the target ring, the 
 *		function makes repeated passes through the slist flushing
 *		entries in flush dependency order.
 *
 * Return:      Non-negative on success/Negative on failure or if there was
 *		a request to flush all items and something was protected.
 *
 * Programmer:  John Mainzer
 *		9/1/15
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_flush_ring(H5F_t *f, hid_t dxpl_id, H5C_ring_t ring,  unsigned flags)
{
    H5C_t * cache_ptr = f->shared->cache;
    hbool_t		flushed_entries_last_pass;
    hbool_t		flush_marked_entries;
    hbool_t		ignore_protected;
    hbool_t		tried_to_flush_protected_entry = FALSE;
    hbool_t		restart_slist_scan;
    int32_t		protected_entries = 0;
    H5SL_node_t * 	node_ptr = NULL;
    H5C_cache_entry_t *	entry_ptr = NULL;
    H5C_cache_entry_t *	next_entry_ptr = NULL;
#if H5C_DO_SANITY_CHECKS
    int64_t		initial_slist_len = 0;
    size_t              initial_slist_size = 0;
#endif /* H5C_DO_SANITY_CHECKS */
    int                 i;
    herr_t		ret_value = SUCCEED;

    FUNC_ENTER_NOAPI(FAIL)

    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(cache_ptr->slist_ptr);
    HDassert((flags & H5C__FLUSH_INVALIDATE_FLAG) == 0);
    HDassert(ring > H5C_RING_UNDEFINED);
    HDassert(ring < H5C_RING_NTYPES);

#if H5C_DO_EXTREME_SANITY_CHECKS
    if((H5C_validate_protected_entry_list(cache_ptr) < 0) ||
            (H5C_validate_pinned_entry_list(cache_ptr) < 0) ||
            (H5C_validate_lru_list(cache_ptr) < 0))
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "an extreme sanity check failed on entry.\n");
#endif /* H5C_DO_EXTREME_SANITY_CHECKS */

    ignore_protected = ( (flags & H5C__FLUSH_IGNORE_PROTECTED_FLAG) != 0 );
    flush_marked_entries = ( (flags & H5C__FLUSH_MARKED_ENTRIES_FLAG) != 0 );

    if(!flush_marked_entries)
        for(i = (int)H5C_RING_UNDEFINED; i < (int)ring; i++)
	    HDassert(cache_ptr->slist_ring_len[i] == 0);

    HDassert(cache_ptr->flush_in_progress);

    /* When we are only flushing marked entries, the slist will usually
     * still contain entries when we have flushed everything we should.
     * Thus we track whether we have flushed any entries in the last
     * pass, and terminate if we haven't.
     */
    flushed_entries_last_pass = TRUE;

    /* Set the cache_ptr->slist_changed to false.
     *
     * This flag is set to TRUE by H5C__flush_single_entry if the 
     * slist is modified by a pre_serialize, serialize, or notify callback.
     * H5C_flush_cache uses this flag to detect any modifications
     * to the slist that might corrupt the scan of the slist -- and 
     * restart the scan in this event.
     */
    cache_ptr->slist_changed = FALSE;

    while((cache_ptr->slist_ring_len[ring] > 0) &&
	    (protected_entries == 0)  &&
	    (flushed_entries_last_pass)) {
        flushed_entries_last_pass = FALSE;

#if H5C_DO_SANITY_CHECKS
        /* For sanity checking, try to verify that the skip list has
         * the expected size and number of entries at the end of each
         * internal while loop (see below).
         *
         * Doing this get a bit tricky, as depending on flags, we may
         * or may not flush all the entries in the slist.
         *
         * To make things more entertaining, with the advent of the
         * fractal heap, the entry serialize callback can cause entries
         * to be dirtied, resized, and/or moved.  Also, the 
         * pre_serialize callback can result in an entry being 
         * removed from the cache via the take ownership flag.
         *
         * To deal with this, we first make note of the initial
         * skip list length and size:
         */
        initial_slist_len = cache_ptr->slist_len;
        initial_slist_size = cache_ptr->slist_size;

        /* As mentioned above, there is the possibility that
         * entries will be dirtied, resized, flushed, or removed
         * from the cache via the take ownership flag  during
         * our pass through the skip list.  To capture the number
         * of entries added, and the skip list size delta,
         * zero the slist_len_increase and slist_size_increase of
         * the cache's instance of H5C_t.  These fields will be
         * updated elsewhere to account for slist insertions and/or
         * dirty entry size changes.
         */
        cache_ptr->slist_len_increase = 0;
        cache_ptr->slist_size_increase = 0;

        /* at the end of the loop, use these values to compute the
         * expected slist length and size and compare this with the
         * value recorded in the cache's instance of H5C_t.
         */
#endif /* H5C_DO_SANITY_CHECKS */

        restart_slist_scan = TRUE;

        while((restart_slist_scan ) || (node_ptr != NULL)) {
            if(restart_slist_scan) {
                restart_slist_scan = FALSE;

                /* Start at beginning of skip list */
                node_ptr = H5SL_first(cache_ptr->slist_ptr);

                if(node_ptr == NULL)
                    /* the slist is empty -- break out of inner loop */
                    break;

                /* Get cache entry for this node */
                next_entry_ptr = (H5C_cache_entry_t *)H5SL_item(node_ptr);

                if(NULL == next_entry_ptr)
                    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "next_entry_ptr == NULL ?!?!")

                HDassert(next_entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
                HDassert(next_entry_ptr->is_dirty);
                HDassert(next_entry_ptr->in_slist);
            } /* end if */
                
            entry_ptr = next_entry_ptr;

            /* With the advent of the fractal heap, the free space
             * manager, and the version 3 cache, it is possible
             * that the pre-serialize or serialize callback will 
             * dirty, resize, or take ownership of other entries 
             * in the cache.  
             *
             * To deal with this, I have inserted code to detect any
             * change in the skip list not directly under the control
             * of this function.  If such modifications are detected,
             * we must re-start the scan of the skip list to avoid 
             * the possibility that the target of the next_entry_ptr
             * may have been flushed or deleted from the cache.
             *
             * To verify that all such possibilities have been dealt
             * with, we do a bit of extra sanity checking on 
             * entry_ptr.
             */
            HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
            HDassert(entry_ptr->in_slist);
            HDassert(entry_ptr->is_dirty);
            if(!flush_marked_entries || entry_ptr->flush_marker)
                HDassert(entry_ptr->ring >= ring);

            /* advance node pointer now, before we delete its target
             * from the slist.
             */
            node_ptr = H5SL_next(node_ptr);
            if(node_ptr != NULL) {
                next_entry_ptr = (H5C_cache_entry_t *)H5SL_item(node_ptr);
                if(NULL == next_entry_ptr)
                    HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "next_entry_ptr == NULL ?!?!")

                HDassert(next_entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
                HDassert(next_entry_ptr->is_dirty);
                HDassert(next_entry_ptr->in_slist);

                if(!flush_marked_entries || next_entry_ptr->flush_marker)
                    HDassert(next_entry_ptr->ring >= ring);

                HDassert(entry_ptr != next_entry_ptr);
            } /* end if */
            else
                next_entry_ptr = NULL;

            HDassert(entry_ptr != NULL);
            HDassert(entry_ptr->in_slist);

            if ( ( ( ! flush_marked_entries ) 
                   || 
                   ( entry_ptr->flush_marker )
                 ) 
                 &&
                 ( ( !entry_ptr->flush_me_last ) 
                   ||
                   ( entry_ptr->flush_me_last 
                     &&
                     ( ( cache_ptr->num_last_entries >= cache_ptr->slist_len ) 
                       ||
                       ( flush_marked_entries && entry_ptr->flush_marker )
                     )
                   )
                 ) 
                 &&
                 ( ( entry_ptr->flush_dep_nchildren == 0 ) 
                   ||
                   ( entry_ptr->flush_dep_ndirty_children == 0 ) 
                 ) 
                 &&
                 ( entry_ptr->ring == ring ) 
               ) {

                HDassert(entry_ptr->flush_dep_nunser_children == 0);

                if(entry_ptr->is_protected) {
                    /* we probably have major problems -- but lets 
                     * flush everything we can before we decide 
                     * whether to flag an error.
                     */
                    tried_to_flush_protected_entry = TRUE;
                    protected_entries++;
                } /* end if */
                else {
                    if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, (flags | H5C__DURING_FLUSH_FLAG)) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Can't flush entry.")

                    if(cache_ptr->slist_changed) {
                        /* The slist has been modified by something
                         * other than the simple removal of the 
                         * of the flushed entry after the flush.
                         * 
                         * This has the potential to corrupt the
                         * scan through the slist, so restart it.
                         */
                        restart_slist_scan = TRUE;
                        cache_ptr->slist_changed = FALSE;
                        H5C__UPDATE_STATS_FOR_SLIST_SCAN_RESTART(cache_ptr)
                    } /* end if */

                    flushed_entries_last_pass = TRUE;
                } /* end else */
            } /* end if */
        } /* while ( ( restart_slist_scan ) || ( node_ptr != NULL ) ) */

#if H5C_DO_SANITY_CHECKS
        /* Verify that the slist size and length are as expected. */
        HDassert((initial_slist_len + cache_ptr->slist_len_increase) == cache_ptr->slist_len);
        HDassert((size_t)((int64_t)initial_slist_size + cache_ptr->slist_size_increase) == cache_ptr->slist_size);
#endif /* H5C_DO_SANITY_CHECKS */
    } /* while */

    HDassert(protected_entries <= cache_ptr->pl_len);

    if(((cache_ptr->pl_len > 0) && (!ignore_protected)) || (tried_to_flush_protected_entry))
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "cache has protected items")

#if H5C_DO_SANITY_CHECKS
    if(!flush_marked_entries) {
        HDassert(cache_ptr->slist_ring_len[ring] == 0);
        HDassert(cache_ptr->slist_ring_size[ring] == 0);
    } /* end if */
#endif /* H5C_DO_SANITY_CHECKS */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_flush_ring() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__flush_single_entry
 *
 * Purpose:     Flush or clear (and evict if requested) the cache entry
 *		with the specified address and type.  If the type is NULL,
 *		any unprotected entry at the specified address will be
 *		flushed (and possibly evicted).
 *
 *		Attempts to flush a protected entry will result in an
 *		error.
 *
 *		If the H5C__FLUSH_INVALIDATE_FLAG flag is set, the entry will
 *		be cleared and not flushed, and the call can't be part of a
 *              sequence of flushes.
 *
 *		If the caller knows the address of the skip list node at
 *		which the target entry resides, it can avoid a lookup
 *		by supplying that address in the tgt_node_ptr parameter.
 *		If this parameter is NULL, the function will do a skip list
 *		search for the entry instead.
 *
 *		The function does nothing silently if there is no entry
 *		at the supplied address, or if the entry found has the
 *		wrong type.
 *
 * Return:      Non-negative on success/Negative on failure or if there was
 *		an attempt to flush a protected item.
 *
 * Programmer:  John Mainzer, 5/5/04
 *
 * Changes:	Refactored function to remove the type_ptr parameter.
 *
 *						JRM -- 8/7/14
 *
 *              Added code to check for slist changes in pre_serialize and
 *              serialize calls, and set 
 *              cache_ptr->slist_change_in_pre_serialize and 
 *		cache_ptr->slist_change_in_serialize as appropriate.
 *
 *                                              JRM -- 12/13/14
 *
 *		Refactored function to delay all modifications of the 
 *		metadata cache data structures until after any calls 
 *		to the pre-serialize or serialize callbacks.
 *
 *		Need to do this, as some pre-serialize or serialize 
 *		calls result in calls to the metadata cache and 
 *		modifications to its data structures.  Thus, at the
 *		time of any such call, the target entry flags and 
 *		the metadata cache must all be consistant.
 *
 *		Also added the entry_size_change_ptr parameter, which 
 *              allows the function to report back any change in the size 
 *		of the entry during the flush.  Such size changes may 
 *		occur during the pre-serialize callback.
 *
 *						JRM -- 12/24/14
 *
 *		Modified code to omit entry writes and entry image frees
 *		as indicated when constructing a file image.
 *
 *						JRM -- 8/4/15
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C__flush_single_entry(const H5F_t *f, hid_t dxpl_id, H5C_cache_entry_t *entry_ptr,
    unsigned flags)
{
    H5C_t *	     	cache_ptr;              /* Cache for file */
    hbool_t		destroy;		/* external flag */
    hbool_t		clear_only;		/* external flag */
    hbool_t		free_file_space;	/* external flag */
    hbool_t		take_ownership;		/* external flag */
    hbool_t             del_from_slist_on_destroy;    /* external flag */
    hbool_t		during_flush;		/* external flag */
    hbool_t		write_entry;		/* internal flag */
    hbool_t		destroy_entry;		/* internal flag */
    hbool_t		generate_image;		/* internal flag */
    hbool_t		was_dirty;
    hbool_t		suppress_image_entry_writes = FALSE;
    hbool_t		suppress_image_entry_frees = FALSE;
    haddr_t             entry_addr = HADDR_UNDEF;
    herr_t		ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_PACKAGE

    HDassert(f);
    cache_ptr = f->shared->cache;
    HDassert(cache_ptr);
    HDassert(cache_ptr->magic == H5C__H5C_T_MAGIC);
    HDassert(entry_ptr);
    HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
    HDassert(entry_ptr->ring != H5C_RING_UNDEFINED);

    /* setup external flags from the flags parameter */
    destroy                = ((flags & H5C__FLUSH_INVALIDATE_FLAG) != 0);
    clear_only             = ((flags & H5C__FLUSH_CLEAR_ONLY_FLAG) != 0);
    free_file_space        = ((flags & H5C__FREE_FILE_SPACE_FLAG) != 0);
    take_ownership         = ((flags & H5C__TAKE_OWNERSHIP_FLAG) != 0);
    del_from_slist_on_destroy = ((flags & H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG) != 0);
    during_flush           = ((flags & H5C__DURING_FLUSH_FLAG) != 0);
    generate_image         = ((flags & H5C__GENERATE_IMAGE_FLAG) != 0);

    /* Set the flag for destroying the entry, based on the 'take ownership'
     * and 'destroy' flags
     */
    if(take_ownership)
        destroy_entry = FALSE;
    else
        destroy_entry = destroy;

#ifdef H5_HAVE_PARALLEL
    HDassert(FALSE == entry_ptr->coll_access);
#endif

    /* we will write the entry to disk if it exists, is dirty, and if the
     * clear only flag is not set.
     */
    if(entry_ptr->is_dirty && !clear_only)
        write_entry = TRUE;
    else
        write_entry = FALSE;

    /* if we have received close warning, and we have been instructed to 
     * generate a metadata cache image, and we have actually constructed
     * the entry images, set suppress_image_entry_frees to TRUE.
     *
     * Set suppress_image_entry_writes to TRUE if indicated by the 
     * image_ctl flags.
     */
    if ( ( cache_ptr->close_warning_received ) &&
         ( cache_ptr->image_ctl.generate_image ) &&
         ( cache_ptr->num_entries_in_image > 0 ) &&
         ( cache_ptr->image_entries ) ) {

        HDassert(entry_ptr->image_up_to_date || !(entry_ptr->include_in_image));
        HDassert(entry_ptr->image_ptr || !(entry_ptr->include_in_image));
        HDassert((!clear_only) || !(entry_ptr->include_in_image));
        HDassert((!take_ownership) || !(entry_ptr->include_in_image));
        HDassert((!free_file_space) || !(entry_ptr->include_in_image));

        suppress_image_entry_frees = TRUE;

        if(cache_ptr->image_ctl.flags & H5C_CI__SUPRESS_ENTRY_WRITES)
            suppress_image_entry_writes = TRUE;
    } /* end if */

    /* run initial sanity checks */
#if H5C_DO_SANITY_CHECKS
    if(entry_ptr->in_slist) {
        HDassert(entry_ptr->is_dirty);

        if((entry_ptr->flush_marker) && (!entry_ptr->is_dirty))
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "entry in slist failed sanity checks.")
    } else {
        HDassert(!entry_ptr->is_dirty);
        HDassert(!entry_ptr->flush_marker);

        if((entry_ptr->is_dirty) || (entry_ptr->flush_marker))
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "entry failed sanity checks.")
    }
#endif /* H5C_DO_SANITY_CHECKS */

    if(entry_ptr->is_protected) {
        HDassert(!entry_ptr->is_protected);

        /* Attempt to flush a protected entry -- scream and die. */
        HGOTO_ERROR(H5E_CACHE, H5E_PROTECT, FAIL, "Attempt to flush a protected entry.")
    } /* end if */

    /* set entry_ptr->flush_in_progress = TRUE and set
     * entry_ptr->flush_marker = FALSE
     *
     * in the parallel case, do some sanity checking in passing.
     */
    HDassert(entry_ptr->type);

    was_dirty = entry_ptr->is_dirty;  /* needed later for logging */

    /* We will set flush_in_progress back to FALSE at the end if the
     * entry still exists at that point.
     */
    entry_ptr->flush_in_progress = TRUE;
    entry_ptr->flush_marker = FALSE;

    /* The entry is dirty, and we are doing a flush, a flush destroy or have
     * been requested to generate an image.  In those cases, serialize the
     * entry.
     */
    if(write_entry || generate_image) {
        HDassert(entry_ptr->is_dirty);

        if(NULL == entry_ptr->image_ptr) {
            if(NULL == (entry_ptr->image_ptr = H5MM_malloc(entry_ptr->size + H5C_IMAGE_EXTRA_SPACE)))
                HGOTO_ERROR(H5E_CACHE, H5E_CANTALLOC, FAIL, "memory allocation failed for on disk image buffer")
#if H5C_DO_MEMORY_SANITY_CHECKS
            HDmemcpy(((uint8_t *)entry_ptr->image_ptr) + entry_ptr->size, H5C_IMAGE_SANITY_VALUE, H5C_IMAGE_EXTRA_SPACE);
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */
        } /* end if */

        if(!(entry_ptr->image_up_to_date)) {
            /* Sanity check */
            HDassert(!entry_ptr->prefetched);

            /* Generate the entry's image */
            if(H5C__generate_image(f, cache_ptr, entry_ptr, dxpl_id) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTGET, FAIL, "can't generate entry's image")
        } /* end if ( ! (entry_ptr->image_up_to_date) ) */
    } /* end if */

    /* Finally, write the image to disk.  
     * 
     * Note that if the H5AC__CLASS_SKIP_WRITES flag is set in the 
     * in the entry's type, we silently skip the write.  This
     * flag should only be used in test code. 
     */
    if(write_entry) {
        HDassert(entry_ptr->is_dirty);

#if H5C_DO_SANITY_CHECKS
        if(cache_ptr->check_write_permitted && !(cache_ptr->write_permitted))
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Write when writes are always forbidden!?!?!")
#endif /* H5C_DO_SANITY_CHECKS */

        /* Write the image to disk unless the write is suppressed.
         *
         * This happens if both suppress_image_entry_writes and 
         * entry_ptr->include_in_image are TRUE, or if the 
         * H5AC__CLASS_SKIP_WRITES is set in the entry's type.  This
         * flag should only be used in test code
         */
        if((!suppress_image_entry_writes || !(entry_ptr->include_in_image))
             &&
             (((entry_ptr->type->flags) & H5C__CLASS_SKIP_WRITES) == 0)) {

            H5FD_mem_t mem_type = H5FD_MEM_DEFAULT;

#ifdef H5_HAVE_PARALLEL
            if(cache_ptr->coll_write_list) {
                if(H5SL_insert(cache_ptr->coll_write_list, entry_ptr, &entry_ptr->addr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTINSERT, FAIL, "unable to insert skip list item")
            } /* end if */
            else
#endif /* H5_HAVE_PARALLEL */

            if(entry_ptr->prefetched) {
                HDassert(entry_ptr->type->id == H5AC_PREFETCHED_ENTRY_ID);
                mem_type = H5C__class_mem_types[entry_ptr->prefetch_type_id];
            } /* end if */
            else
                mem_type = entry_ptr->type->mem_type;

            if(H5F_block_write(f, mem_type, entry_ptr->addr, entry_ptr->size, dxpl_id, entry_ptr->image_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Can't write image to file.")
        } /* end if */

        /* if the entry has a notify callback, notify it that we have 
         * just flushed the entry.
         */
        if(entry_ptr->type->notify &&
                (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_AFTER_FLUSH, entry_ptr) < 0 )
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client of entry flush")
    } /* if ( write_entry ) */

    /* At this point, all pre-serialize and serialize calls have been
     * made if it was appropriate to make them.  Similarly, the entry
     * has been written to disk if desired.
     *
     * Thus it is now safe to update the cache data structures for the 
     * flush.
     */

    /* start by updating the statistics */
    if(clear_only) {
        /* only log a clear if the entry was dirty */
        if(was_dirty) {
            H5C__UPDATE_STATS_FOR_CLEAR(cache_ptr, entry_ptr)
        } /* end if */
    } else if(write_entry) {
        HDassert(was_dirty);

        /* only log a flush if we actually wrote to disk */
        H5C__UPDATE_STATS_FOR_FLUSH(cache_ptr, entry_ptr)
    } /* end else if */

    /* Note that the algorithm below is (very) similar to the set of operations
     * in H5C_remove_entry() and should be kept in sync with changes
     * to that code. - QAK, 2016/11/30
     */

    /* Update the cache internal data structures. */
    if(destroy) {
        /* Sanity checks */
        if(take_ownership)
            HDassert(!destroy_entry);
        else
            HDassert(destroy_entry);
        HDassert(!entry_ptr->is_pinned);

        /* Update stats, while entry is still in the cache */
        H5C__UPDATE_STATS_FOR_EVICTION(cache_ptr, entry_ptr, take_ownership)

        /* If the entry's type has a 'notify' callback and the entry is about
         * to be removed from the cache, send a 'before eviction' notice while
         * the entry is still fully integrated in the cache.
         */
        if(entry_ptr->type->notify && (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_BEFORE_EVICT, entry_ptr) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry to evict")

        /* Update the cache internal data structures as appropriate
         * for a destroy.  Specifically:
         *
         * 1) Delete it from the index
         *
         * 2) Delete it from the skip list if requested.
         *
         * 3) Update the replacement policy for eviction
         *
         * 4) Remove it from the tag list for this object
         *
         * Finally, if the destroy_entry flag is set, discard the 
         * entry.
         */
        H5C__DELETE_FROM_INDEX(cache_ptr, entry_ptr, FAIL)

        if(entry_ptr->in_slist && del_from_slist_on_destroy)
            H5C__REMOVE_ENTRY_FROM_SLIST(cache_ptr, entry_ptr, during_flush)

        H5C__UPDATE_RP_FOR_EVICTION(cache_ptr, entry_ptr, FAIL)

        /* Remove entry from tag list */
        if(H5C__untag_entry(cache_ptr, entry_ptr) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove entry from tag list")

	/* verify that the entry is no longer part of any flush dependencies */
        HDassert(entry_ptr->flush_dep_nparents == 0);
	HDassert(entry_ptr->flush_dep_nchildren == 0);
    } /* end if */
    else {
        HDassert(clear_only || write_entry);
        HDassert(entry_ptr->is_dirty);
        HDassert(entry_ptr->in_slist);

        /* We are either doing a flush or a clear.
         *
         * A clear and a flush are the same from the point of
         * view of the replacement policy and the slist.  
         * Hence no differentiation between them.
         *
         * 					JRM -- 7/7/07
         */

        H5C__UPDATE_RP_FOR_FLUSH(cache_ptr, entry_ptr, FAIL)

        H5C__REMOVE_ENTRY_FROM_SLIST(cache_ptr, entry_ptr, during_flush)

        /* mark the entry as clean and update the index for 
         * entry clean.  Also, call the clear callback 
         * if defined.
         */
        entry_ptr->is_dirty = FALSE;

        H5C__UPDATE_INDEX_FOR_ENTRY_CLEAN(cache_ptr, entry_ptr);

        /* Check for entry changing status and do notifications, etc. */
        if(was_dirty) {
            /* If the entry's type has a 'notify' callback send a 'entry cleaned'
             * notice now that the entry is fully integrated into the cache.
             */
            if(entry_ptr->type->notify &&
                    (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_CLEANED, entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag cleared")

            /* Propagate the clean flag up the flush dependency chain if appropriate */
            HDassert(entry_ptr->flush_dep_ndirty_children == 0);
            if(entry_ptr->flush_dep_nparents > 0)
                if(H5C__mark_flush_dep_clean(entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTMARKDIRTY, FAIL, "Can't propagate flush dep clean flag")
        } /* end if */
    } /* end else */

    /* reset the flush_in progress flag */
    entry_ptr->flush_in_progress = FALSE;

    /* capture the cache entry address for the log_flush call at the
       end before the entry_ptr gets freed */
    entry_addr = entry_ptr->addr;

    /* Internal cache data structures should now be up to date, and 
     * consistant with the status of the entry.  
     *
     * Now discard the entry if appropriate.
     */
    if(destroy) {
        /* Sanity check */
        HDassert(0 == entry_ptr->flush_dep_nparents);

        /* if both suppress_image_entry_frees and entry_ptr->include_in_image
         * are true, simple set entry_ptr->image_ptr to NULL, as we have 
         * another pointer to the buffer in an instance of H5C_image_entry_t
         * in cache_ptr->image_entries.
         *
         * Otherwise, free the buffer if it exists.
         */
        if(suppress_image_entry_frees && entry_ptr->include_in_image) 
            entry_ptr->image_ptr = NULL;
        else if(entry_ptr->image_ptr != NULL)
            entry_ptr->image_ptr = H5MM_xfree(entry_ptr->image_ptr);

        /* If the entry is not a prefetched entry, verify that the flush 
         * dependency parents addresses array has been transfered.
         *
         * If the entry is prefetched, the free_isr routine will dispose of
         * the flush dependency parents adresses array if necessary.
         */
        if ( ! entry_ptr->prefetched ) {
            HDassert(0 == entry_ptr->fd_parent_count);
            HDassert(NULL == entry_ptr->fd_parent_addrs);
        }

        /* Check whether we should free the space in the file that 
         * the entry occupies 
         */
        if(free_file_space) {
            size_t fsf_size;

            /* Sanity checks */
            HDassert(H5F_addr_defined(entry_ptr->addr));
            HDassert(!H5F_IS_TMP_ADDR(f, entry_ptr->addr));
#ifndef NDEBUG
            {
                size_t curr_len;

                /* Get the actual image size for the thing again */
                entry_ptr->type->image_len((void *)entry_ptr, &curr_len);
                HDassert(curr_len == entry_ptr->size);
            }
#endif /* NDEBUG */

            /* If the file space free size callback is defined, use
             * it to get the size of the block of file space to free.
             * Otherwise use entry_ptr->size.
             */
            if(entry_ptr->type->fsf_size) {
                if((entry_ptr->type->fsf_size)((void *)entry_ptr, &fsf_size) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTFREE, FAIL, "unable to get file space free size")
            } /* end if */
            else    /* no file space free size callback -- use entry size */
                fsf_size = entry_ptr->size;

            /* Release the space on disk */
            if(H5MF_xfree(f, entry_ptr->type->mem_type, dxpl_id, entry_ptr->addr, (hsize_t)fsf_size) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTFREE, FAIL, "unable to free file space for cache entry")
        } /* end if ( free_file_space ) */

        /* Reset the pointer to the cache the entry is within. -QAK */
        entry_ptr->cache_ptr = NULL;

        /* increment entries_removed_counter and set 
         * last_entry_removed_ptr.  As we are likely abuut to 
         * free the entry, recall that last_entry_removed_ptr 
         * must NEVER be dereferenced.
         *
         * Recall that these fields are maintained to allow functions
         * that perform scans of lists of entries to detect the 
         * unexpected removal of entries (via expunge, eviction, 
         * or take ownership at present), so that they can re-start
         * their scans if necessary.
         *
         * Also check if the entry we are watching for removal is being
         * removed (usually the 'next' entry for an iteration) and reset
         * it to indicate that it was removed.
         */
        cache_ptr->entries_removed_counter++;
        cache_ptr->last_entry_removed_ptr = entry_ptr;
        if(entry_ptr == cache_ptr->entry_watched_for_removal)
            cache_ptr->entry_watched_for_removal = NULL;

        /* Check for actually destroying the entry in memory */
        /* (As opposed to taking ownership of it) */
        if(destroy_entry) {
            if(entry_ptr->is_dirty) {
                /* Reset dirty flag */
                entry_ptr->is_dirty = FALSE;

                /* If the entry's type has a 'notify' callback send a 'entry cleaned'
                 * notice now that the entry is fully integrated into the cache.
                 */
                if(entry_ptr->type->notify &&
                        (entry_ptr->type->notify)(H5C_NOTIFY_ACTION_ENTRY_CLEANED, entry_ptr) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry dirty flag cleared")
            } /* end if */

            /* we are about to discard the in core representation --
             * set the magic field to bad magic so we can detect a
             * freed entry if we see one.
             */
            entry_ptr->magic = H5C__H5C_CACHE_ENTRY_T_BAD_MAGIC;

            /* verify that the image has been freed */
            HDassert(entry_ptr->image_ptr == NULL);

            if(entry_ptr->type->free_icr((void *)entry_ptr) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "free_icr callback failed.")
        }  /* end if */
        else {
            HDassert(take_ownership);

            /* client is taking ownership of the entry.
             * set bad magic here too so the cache will choke 
             * unless the entry is re-inserted properly
             */
            entry_ptr->magic = H5C__H5C_CACHE_ENTRY_T_BAD_MAGIC;
        } /* end else */
    } /* if (destroy) */

    if(cache_ptr->log_flush)
        if((cache_ptr->log_flush)(cache_ptr, entry_addr, was_dirty, flags) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "log_flush callback failed.")

done:
    HDassert( ( ret_value != SUCCEED ) || ( destroy_entry ) || 
              ( ! entry_ptr->flush_in_progress ) );

    HDassert( ( ret_value != SUCCEED ) || ( destroy_entry ) || 
              ( take_ownership ) || ( ! entry_ptr->is_dirty ) );

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__flush_single_entry() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__verify_len_eoa
 *
 * Purpose:     Verify that 'len' does not exceed eoa when 'actual' is
 *              false i.e. 'len" is the initial speculative length from
 *              get_load_size callback with null image pointer.
 *              If exceed, adjust 'len' accordingly.
 *
 *              Verify that 'len' should not exceed eoa when 'actual' is
 *              true i.e. 'len' is the actual length from get_load_size 
 *              callback with non-null image pointer.
 *              If exceed, return error.
 *
 * Return:      FAIL if error is detected, SUCCEED otherwise.
 *
 * Programmer:  Vailin Choi
 *              9/6/15
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__verify_len_eoa(H5F_t *f, const H5C_class_t *type, haddr_t addr,
    size_t *len, hbool_t actual)
{
    H5FD_mem_t cooked_type;             /* Modified type, accounting for switching global heaps */
    haddr_t eoa;                	/* End-of-allocation in the file */
    herr_t ret_value = SUCCEED;      	/* Return value */

    FUNC_ENTER_STATIC

    /* if type == H5FD_MEM_GHEAP, H5F_block_read() forces 
     * type to H5FD_MEM_DRAW via its call to H5F__accum_read().
     * Thus we do the same for purposes of computing the EOA
     * for sanity checks.
     */
    cooked_type = (type->mem_type == H5FD_MEM_GHEAP) ? H5FD_MEM_DRAW : type->mem_type;

    /* Get the file's end-of-allocation value */
    eoa = H5F_get_eoa(f, cooked_type);
    if(!H5F_addr_defined(eoa))
	HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "invalid EOA address for file")

    /* Check for bad address in general */
    if(H5F_addr_gt(addr, eoa))
	HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "address of object past end of allocation")

    /* Check if the amount of data to read will be past the EOA */
    if(H5F_addr_gt((addr + *len), eoa)) {
	if(actual)
	    HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "actual len exceeds EOA.")
	else
	    /* Trim down the length of the metadata */
	    *len = (size_t)(eoa - addr);
    } /* end if */

    if(*len <= 0)
	HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, FAIL, "len not positive after adjustment for EOA.")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__verify_len_eoa() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_load_entry
 *
 * Purpose:     Attempt to load the entry at the specified disk address
 *              and with the specified type into memory.  If successful.
 *              return the in memory address of the entry.  Return NULL
 *              on failure.
 *
 *              Note that this function simply loads the entry into
 *              core.  It does not insert it into the cache.
 *
 * Return:      Non-NULL on success / NULL on failure.
 *
 * Programmer:  John Mainzer, 5/18/04
 *
 *-------------------------------------------------------------------------
 */
static void *
H5C_load_entry(H5F_t *              f,
                hid_t               dxpl_id,
#ifdef H5_HAVE_PARALLEL
                hbool_t             coll_access,
#endif /* H5_HAVE_PARALLEL */
                const H5C_class_t * type,
                haddr_t             addr,
                void *              udata)
{
    hbool_t     dirty = FALSE;          /* Flag indicating whether thing was dirtied during deserialize */
    uint8_t *   image = NULL;           /* Buffer for disk image                    */
    void *      thing = NULL;           /* Pointer to thing loaded                  */
    H5C_cache_entry_t *entry = NULL;    /* Alias for thing loaded, as cache entry   */
    size_t      len;                    /* Size of image in file                    */
#ifdef H5_HAVE_PARALLEL
    int         mpi_rank = 0;           /* MPI process rank                         */
    MPI_Comm    comm = MPI_COMM_NULL;   /* File MPI Communicator                    */
    int         mpi_code;               /* MPI error code                           */
#endif /* H5_HAVE_PARALLEL */
    void *      ret_value = NULL;       /* Return value                             */

    FUNC_ENTER_NOAPI_NOINIT

    /* Sanity checks */
    HDassert(f);
    HDassert(f->shared);
    HDassert(f->shared->cache);
    HDassert(type);
    HDassert(H5F_addr_defined(addr));
    HDassert(type->get_initial_load_size);
    if(type->flags & H5C__CLASS_SPECULATIVE_LOAD_FLAG)
        HDassert(type->get_final_load_size);
    else
        HDassert(NULL == type->get_final_load_size);
    HDassert(type->deserialize);

    /* Can't see how skip reads could be usefully combined with
     * the speculative read flag.  Hence disallow.
     */
    HDassert(!((type->flags & H5C__CLASS_SKIP_READS) &&
               (type->flags & H5C__CLASS_SPECULATIVE_LOAD_FLAG)));

    /* Call the get_initial_load_size callback, to retrieve the initial size of image */
    if(type->get_initial_load_size(udata, &len) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTGET, NULL, "can't retrieve image size")
    HDassert(len > 0);

    /* Check for possible speculative read off the end of the file */
    if(type->flags & H5C__CLASS_SPECULATIVE_LOAD_FLAG)
        if(H5C__verify_len_eoa(f, type, addr, &len, FALSE) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, NULL, "invalid len with respect to EOA")

    /* Allocate the buffer for reading the on-disk entry image */
    if(NULL == (image = (uint8_t *)H5MM_malloc(len + H5C_IMAGE_EXTRA_SPACE)))
        HGOTO_ERROR(H5E_CACHE, H5E_CANTALLOC, NULL, "memory allocation failed for on disk image buffer.")
#if H5C_DO_MEMORY_SANITY_CHECKS
    HDmemcpy(image + len, H5C_IMAGE_SANITY_VALUE, H5C_IMAGE_EXTRA_SPACE);
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */

#ifdef H5_HAVE_PARALLEL
    if(H5F_HAS_FEATURE(f, H5FD_FEAT_HAS_MPI)) {
        if((mpi_rank = H5F_mpi_get_rank(f)) < 0)
            HGOTO_ERROR(H5E_FILE, H5E_CANTGET, NULL, "Can't get MPI rank")
        if((comm = H5F_mpi_get_comm(f)) == MPI_COMM_NULL)
            HGOTO_ERROR(H5E_FILE, H5E_CANTGET, NULL, "get_comm request failed")
    } /* end if */
#endif /* H5_HAVE_PARALLEL */

    /* Get the on-disk entry image */
    if(0 == (type->flags & H5C__CLASS_SKIP_READS)) {
        unsigned tries, max_tries;      /* The # of read attempts               */
        unsigned retries;               /* The # of retries                     */
        htri_t chk_ret;                 /* return from verify_chksum callback   */
        size_t actual_len = len;        /* The actual length, after speculative reads have been resolved */
        uint64_t nanosec = 1;           /* # of nanoseconds to sleep between retries */
        void *new_image;                /* Pointer to image                     */
        hbool_t len_changed = TRUE;     /* Whether to re-check speculative entries */

        /* Get the # of read attempts */
        max_tries = tries = H5F_GET_READ_ATTEMPTS(f);

        /* 
         * This do/while loop performs the following till the metadata checksum
         * is correct or the file's number of allowed read attempts are reached.
         *   --read the metadata
         *   --determine the actual size of the metadata
         *   --perform checksum verification
         */
        do {
            if(actual_len != len) {
                if(NULL == (new_image = H5MM_realloc(image, len + H5C_IMAGE_EXTRA_SPACE)))
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTALLOC, NULL, "image null after H5MM_realloc()")
                image = (uint8_t *)new_image;
#if H5C_DO_MEMORY_SANITY_CHECKS
                HDmemcpy(image + len, H5C_IMAGE_SANITY_VALUE, H5C_IMAGE_EXTRA_SPACE);
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */
            } /* end if */

#ifdef H5_HAVE_PARALLEL
            if(!coll_access || 0 == mpi_rank) {
#endif /* H5_HAVE_PARALLEL */
                if(H5F_block_read(f, type->mem_type, addr, len, dxpl_id, image) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_READERROR, NULL, "Can't read image*")
#ifdef H5_HAVE_PARALLEL
            } /* end if */
            /* if the collective metadata read optimization is turned on,
             * bcast the metadata read from process 0 to all ranks in the file
             * communicator
             */
            if(coll_access) {
                int buf_size;

                H5_CHECKED_ASSIGN(buf_size, int, len, size_t);
                if(MPI_SUCCESS != (mpi_code = MPI_Bcast(image, buf_size, MPI_BYTE, 0, comm)))
                    HMPI_GOTO_ERROR(NULL, "MPI_Bcast failed", mpi_code)
            } /* end if */
#endif /* H5_HAVE_PARALLEL */

            /* If the entry could be read speculatively and the length is still
             *  changing, check for updating the actual size
             */
            if((type->flags & H5C__CLASS_SPECULATIVE_LOAD_FLAG) && len_changed) {
                /* Retrieve the actual length */
                actual_len = len;
                if(type->get_final_load_size(image, len, udata, &actual_len) < 0)
                    continue;   /* Transfer control to while() and count towards retries */

                /* Check for the length changing */
                if(actual_len != len) {
                    /* Verify that the length isn't past the EOA for the file */
                    if(H5C__verify_len_eoa(f, type, addr, &actual_len, TRUE) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, NULL, "actual_len exceeds EOA.")

                    /* Expand buffer to new size */
                    if(NULL == (new_image = H5MM_realloc(image, actual_len + H5C_IMAGE_EXTRA_SPACE)))
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTALLOC, NULL, "image null after H5MM_realloc()")
                    image = (uint8_t *)new_image;
#if H5C_DO_MEMORY_SANITY_CHECKS
                    HDmemcpy(image + actual_len, H5C_IMAGE_SANITY_VALUE, H5C_IMAGE_EXTRA_SPACE);
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */

                    if(actual_len > len) {
#ifdef H5_HAVE_PARALLEL
                        if(!coll_access || 0 == mpi_rank) {
#endif /* H5_HAVE_PARALLEL */
                            /* If the thing's image needs to be bigger for a speculatively
                             * loaded thing, go get the on-disk image again (the extra portion).
                             */
                            if(H5F_block_read(f, type->mem_type, addr + len, actual_len - len, dxpl_id, image + len) < 0)
                                HGOTO_ERROR(H5E_CACHE, H5E_CANTLOAD, NULL, "can't read image")
#ifdef H5_HAVE_PARALLEL
                        }
                        /* If the collective metadata read optimization is turned on,
                         * Bcast the metadata read from process 0 to all ranks in the file
                         * communicator */
                        if(coll_access) {
                            int buf_size;

                            H5_CHECKED_ASSIGN(buf_size, int, actual_len - len, size_t);
                            if(MPI_SUCCESS != (mpi_code = MPI_Bcast(image + len, buf_size, MPI_BYTE, 0, comm)))
                                HMPI_GOTO_ERROR(NULL, "MPI_Bcast failed", mpi_code)
                        } /* end if */
#endif /* H5_HAVE_PARALLEL */
                    } /* end if */
                } /* end if (actual_len != len) */
                else {
                    /* The length has stabilized */
                    len_changed = FALSE;

                    /* Set the final length */
                    len = actual_len;
                } /* else */
            } /* end if */

            /* If there's no way to verify the checksum for a piece of metadata
             * (usually because there's no checksum in the file), leave now
             */
            if(type->verify_chksum == NULL)
                break;

            /* Verify the checksum for the metadata image */
            if((chk_ret = type->verify_chksum(image, actual_len, udata)) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTGET, NULL, "failure from verify_chksum callback")
            if(chk_ret == TRUE)
                break;

            /* Sleep for some time */
            H5_nanosleep(nanosec);
            nanosec *= 2;               /* Double the sleep time next time */
        } while(--tries);

        /* Check for too many tries */
        if(tries == 0)
            HGOTO_ERROR(H5E_CACHE, H5E_READERROR, NULL, "incorrect metadatda checksum after all read attempts")

        /* Calculate and track the # of retries */
        retries = max_tries - tries;
        if(retries)     /* Does not track 0 retry */
            if(H5F_track_metadata_read_retries(f, (unsigned)type->mem_type, retries) < 0)
                HGOTO_ERROR(H5E_CACHE, H5E_BADVALUE, NULL, "cannot track read tries = %u ", retries)

        /* Set the final length (in case it wasn't set earlier) */
        len = actual_len;
    } /* end if !H5C__CLASS_SKIP_READS */

    /* Deserialize the on-disk image into the native memory form */
    if(NULL == (thing = type->deserialize(image, len, udata, &dirty)))
        HGOTO_ERROR(H5E_CACHE, H5E_CANTLOAD, NULL, "Can't deserialize image")

    entry = (H5C_cache_entry_t *)thing;

    /* In general, an entry should be clean just after it is loaded.
     *
     * However, when this code is used in the metadata cache, it is
     * possible that object headers will be dirty at this point, as
     * the deserialize function will alter object headers if necessary to
     * fix an old bug.
     *
     * In the following assert:
     *
     * 	HDassert( ( dirty == FALSE ) || ( type->id == 5 || type->id == 6 ) );
     *
     * note that type ids 5 & 6 are associated with object headers in the
     * metadata cache.
     *
     * When we get to using H5C for other purposes, we may wish to
     * tighten up the assert so that the loophole only applies to the
     * metadata cache.
     */

    HDassert( ( dirty == FALSE ) || ( type->id == 5 || type->id == 6) );

    entry->magic                        = H5C__H5C_CACHE_ENTRY_T_MAGIC;
    entry->cache_ptr                    = f->shared->cache;
    entry->addr                         = addr;
    entry->size                         = len;
    HDassert(entry->size < H5C_MAX_ENTRY_SIZE);
    entry->image_ptr                    = image;
    entry->image_up_to_date             = !dirty;
    entry->type                         = type;
    entry->is_dirty	                    = dirty;
    entry->dirtied                      = FALSE;
    entry->is_protected                 = FALSE;
    entry->is_read_only                 = FALSE;
    entry->ro_ref_count                 = 0;
    entry->is_pinned                    = FALSE;
    entry->in_slist                     = FALSE;
    entry->flush_marker                 = FALSE;
#ifdef H5_HAVE_PARALLEL
    entry->clear_on_unprotect           = FALSE;
    entry->flush_immediately            = FALSE;
    entry->coll_access                  = coll_access;
#endif /* H5_HAVE_PARALLEL */
    entry->flush_in_progress            = FALSE;
    entry->destroy_in_progress          = FALSE;

    entry->ring                         = H5C_RING_UNDEFINED;

    /* Initialize flush dependency fields */
    entry->flush_dep_parent             = NULL;
    entry->flush_dep_nparents           = 0;
    entry->flush_dep_parent_nalloc      = 0;
    entry->flush_dep_nchildren          = 0;
    entry->flush_dep_ndirty_children    = 0;
    entry->flush_dep_nunser_children    = 0;
    entry->ht_next                      = NULL;
    entry->ht_prev                      = NULL;
    entry->il_next                      = NULL;
    entry->il_prev             	        = NULL;

    entry->next                         = NULL;
    entry->prev                         = NULL;

    entry->aux_next                     = NULL;
    entry->aux_prev                     = NULL;

#ifdef H5_HAVE_PARALLEL
    entry->coll_next                    = NULL;
    entry->coll_prev                    = NULL;
#endif /* H5_HAVE_PARALLEL */

    /* initialize cache image related fields */
    entry->include_in_image             = FALSE;
    entry->lru_rank                     = 0;
    entry->image_index                  = -1;
    entry->image_dirty                  = FALSE;
    entry->fd_parent_count              = 0;
    entry->fd_parent_addrs              = NULL;
    entry->fd_child_count               = 0;
    entry->fd_dirty_child_count         = 0;
    entry->image_fd_height              = 0;
    entry->prefetched                   = FALSE;
    entry->prefetch_type_id             = 0;
    entry->age                          = 0;
#ifndef NDEBUG  /* debugging field */
    entry->serialization_count          = 0;
#endif /* NDEBUG */

    H5C__RESET_CACHE_ENTRY_STATS(entry);

    ret_value = thing;

done:
    /* Cleanup on error */
    if(NULL == ret_value) {
        /* Release resources */
        if(thing && type->free_icr(thing) < 0)
            HDONE_ERROR(H5E_CACHE, H5E_CANTFLUSH, NULL, "free_icr callback failed")
        if(image)
            image = (uint8_t *)H5MM_xfree(image);
    } /* end if */

    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_load_entry() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_make_space_in_cache
 *
 * Purpose:     Attempt to evict cache entries until the index_size
 *		is at least needed_space below max_cache_size.
 *
 *		In passing, also attempt to bring cLRU_list_size to a
 *		value greater than min_clean_size.
 *
 *		Depending on circumstances, both of these goals may
 *		be impossible, as in parallel mode, we must avoid generating
 *		a write as part of a read (to avoid deadlock in collective
 *		I/O), and in all cases, it is possible (though hopefully
 *		highly unlikely) that the protected list may exceed the
 *		maximum size of the cache.
 *
 *		Thus the function simply does its best, returning success
 *		unless an error is encountered.
 *
 *		The primary_dxpl_id and secondary_dxpl_id parameters
 *		specify the dxpl_ids used on the first write occasioned
 *		by the call (primary_dxpl_id), and on all subsequent
 *		writes (secondary_dxpl_id).  This is useful in the metadata
 *		cache, but may not be needed elsewhere.  If so, just use the
 *		same dxpl_id for both parameters.
 *
 *		Observe that this function cannot occasion a read.
 *
 * Return:      Non-negative on success/Negative on failure.
 *
 * Programmer:  John Mainzer, 5/14/04
 *
 * Changes:     Modified function to skip over entries with the 
 *		flush_in_progress flag set.  If this is not done,
 *		an infinite recursion is possible if the cache is 
 *		full, and the pre-serialize or serialize routine 
 *		attempts to load another entry.
 *
 *		This error was exposed by a re-factor of the 
 *		H5C__flush_single_entry() routine.  However, it was 
 *		a potential bug from the moment that entries were 
 *		allowed to load other entries on flush.
 *
 *		In passing, note that the primary and secondary dxpls 
 *		mentioned in the comment above have been replaced by 
 *		a single dxpl at some point, and thus the discussion 
 *		above is somewhat obsolete.  Date of this change is 
 *		unkown.
 *
 *						JRM -- 12/26/14
 *
 *		Modified function to detect deletions of entries 
 *		during a scan of the LRU, and where appropriate, 
 *		restart the scan to avoid proceeding with a next 
 *		entry that is no longer in the cache.
 *
 *		Note the absence of checks after flushes of clean 
 *		entries.  As a second entry can only be removed by 
 *		by a call to the pre_serialize or serialize callback
 *		of the first, and as these callbacks will not be called
 *		on clean entries, no checks are needed.
 *
 *						JRM -- 4/6/15
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_make_space_in_cache(H5F_t *	f,
                        hid_t	dxpl_id,
		        size_t	space_needed,
                        hbool_t	write_permitted)
{
    H5C_t *		cache_ptr = f->shared->cache;
#if H5C_COLLECT_CACHE_STATS
    int32_t             clean_entries_skipped = 0;
    int32_t             total_entries_scanned = 0;
#endif /* H5C_COLLECT_CACHE_STATS */
    int32_t		entries_examined = 0;
    int32_t		initial_list_len;
    size_t		empty_space;
    hbool_t		prev_is_dirty = FALSE;
    hbool_t             didnt_flush_entry = FALSE;
    hbool_t		restart_scan;
    H5C_cache_entry_t *	entry_ptr;
    H5C_cache_entry_t *	prev_ptr;
    H5C_cache_entry_t *	next_ptr;
    int32_t 		num_corked_entries = 0;
    herr_t		ret_value = SUCCEED;      /* Return value */

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( f );
    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( cache_ptr->index_size ==
	      (cache_ptr->clean_index_size + cache_ptr->dirty_index_size) );

    if ( write_permitted ) {

        restart_scan = FALSE;
        initial_list_len = cache_ptr->LRU_list_len;
        entry_ptr = cache_ptr->LRU_tail_ptr;

	if ( cache_ptr->index_size >= cache_ptr->max_cache_size ) {

	   empty_space = 0;

	} else {

	   empty_space = cache_ptr->max_cache_size - cache_ptr->index_size;

	}

        while ( ( ( (cache_ptr->index_size + space_needed)
                    >
                    cache_ptr->max_cache_size
                  )
		  ||
		  (
		    ( empty_space + cache_ptr->clean_index_size )
		    <
		    ( cache_ptr->min_clean_size )
                  )
		)
                &&
                ( entries_examined <= (2 * initial_list_len) )
                &&
                ( entry_ptr != NULL )
              )
        {
	    HDassert(entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
            HDassert( !(entry_ptr->is_protected) );
            HDassert( ! (entry_ptr->is_read_only) );
            HDassert( (entry_ptr->ro_ref_count) == 0 );

	    next_ptr = entry_ptr->next;
            prev_ptr = entry_ptr->prev;

	    if(prev_ptr != NULL)
		prev_is_dirty = prev_ptr->is_dirty;

	    if(entry_ptr->is_dirty &&
                    (entry_ptr->tag_info && entry_ptr->tag_info->corked)) {

                /* Skip "dirty" corked entries.  */
		++num_corked_entries;
                didnt_flush_entry = TRUE;

	    } else if ( ( (entry_ptr->type)->id != H5AC_EPOCH_MARKER_ID ) &&
                 ( ! entry_ptr->flush_in_progress ) ) {

                didnt_flush_entry = FALSE;

                if ( entry_ptr->is_dirty ) {

#if H5C_COLLECT_CACHE_STATS
                    if ( (cache_ptr->index_size + space_needed)
                           >
                          cache_ptr->max_cache_size ) {

                        cache_ptr->entries_scanned_to_make_space++;
                    }
#endif /* H5C_COLLECT_CACHE_STATS */

		    /* reset entries_removed_counter and 
                     * last_entry_removed_ptr prior to the call to 
                     * H5C__flush_single_entry() so that we can spot 
                     * unexpected removals of entries from the cache,
                     * and set the restart_scan flag if proceeding
                     * would be likely to cause us to scan an entry
                     * that is no longer in the cache.
                     */
                    cache_ptr->entries_removed_counter = 0;
                    cache_ptr->last_entry_removed_ptr  = NULL;

#ifdef H5_HAVE_PARALLEL
                    if(TRUE == entry_ptr->coll_access) {
                        entry_ptr->coll_access = FALSE;
                        H5C__REMOVE_FROM_COLL_LIST(cache_ptr, entry_ptr, FAIL)
                    } /* end if */
#endif

                    if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__NO_FLAGS_SET) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to flush entry")

		    if ( ( cache_ptr->entries_removed_counter > 1 ) ||
                         ( cache_ptr->last_entry_removed_ptr == prev_ptr ) )

                        restart_scan = TRUE;

                } else if ( (cache_ptr->index_size + space_needed) > cache_ptr->max_cache_size 
#ifdef H5_HAVE_PARALLEL
                            && !(entry_ptr->coll_access)
#endif /* H5_HAVE_PARALLEL */
                            ) {
#if H5C_COLLECT_CACHE_STATS
                    cache_ptr->entries_scanned_to_make_space++;
#endif /* H5C_COLLECT_CACHE_STATS */

                    if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__FLUSH_INVALIDATE_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG) < 0)
                        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to flush entry")
                } else {
                    /* We have enough space so don't flush clean entry. */
#if H5C_COLLECT_CACHE_STATS
                    clean_entries_skipped++;
#endif /* H5C_COLLECT_CACHE_STATS */
                    didnt_flush_entry = TRUE;
                }

#if H5C_COLLECT_CACHE_STATS
                total_entries_scanned++;
#endif /* H5C_COLLECT_CACHE_STATS */

            } else {

                /* Skip epoch markers and entries that are in the process
                 * of being flushed.
                 */
                didnt_flush_entry = TRUE;
            }

	    if ( prev_ptr != NULL ) {

		if ( didnt_flush_entry ) {

		    /* epoch markers don't get flushed, and we don't touch
                     * entries that are in the process of being flushed.
                     * Hence no need for sanity checks, as we haven't
                     * flushed anything.  Thus just set entry_ptr to prev_ptr
                     * and go on.
		     */
                    entry_ptr = prev_ptr;

		} else if ( ( restart_scan )
                            ||
                            ( prev_ptr->is_dirty != prev_is_dirty )
		            ||
		            ( prev_ptr->next != next_ptr )
		            ||
		            ( prev_ptr->is_protected )
		            ||
		            ( prev_ptr->is_pinned ) ) {

		    /* something has happened to the LRU -- start over
		     * from the tail.
		     */
                    restart_scan = FALSE;
	            entry_ptr = cache_ptr->LRU_tail_ptr;
		    H5C__UPDATE_STATS_FOR_LRU_SCAN_RESTART(cache_ptr)

		} else {

		    entry_ptr = prev_ptr;

		}
	    } else {

		entry_ptr = NULL;

	    }

	    entries_examined++;

	    if ( cache_ptr->index_size >= cache_ptr->max_cache_size ) {

	       empty_space = 0;

	    } else {

	       empty_space = cache_ptr->max_cache_size - cache_ptr->index_size;

	    }

	    HDassert( cache_ptr->index_size ==
	              (cache_ptr->clean_index_size +
		       cache_ptr->dirty_index_size) );

	}

#if H5C_COLLECT_CACHE_STATS
        cache_ptr->calls_to_msic++;

        cache_ptr->total_entries_skipped_in_msic += clean_entries_skipped;
        cache_ptr->total_entries_scanned_in_msic += total_entries_scanned;

        if ( clean_entries_skipped > cache_ptr->max_entries_skipped_in_msic ) {

            cache_ptr->max_entries_skipped_in_msic = clean_entries_skipped;
        }

        if ( total_entries_scanned > cache_ptr->max_entries_scanned_in_msic ) {

            cache_ptr->max_entries_scanned_in_msic = total_entries_scanned;
        }
#endif /* H5C_COLLECT_CACHE_STATS */


	/* NEED: work on a better assert for corked entries */
	HDassert( ( entries_examined > (2 * initial_list_len) ) ||
		  ( (cache_ptr->pl_size + cache_ptr->pel_size + cache_ptr->min_clean_size) >
		    cache_ptr->max_cache_size ) ||
		  ( ( cache_ptr->clean_index_size + empty_space )
		    >= cache_ptr->min_clean_size ) ||
		  ( ( num_corked_entries )));
#if H5C_MAINTAIN_CLEAN_AND_DIRTY_LRU_LISTS

        HDassert( ( entries_examined > (2 * initial_list_len) ) ||
		  ( cache_ptr->cLRU_list_size <= cache_ptr->clean_index_size ) );
        HDassert( ( entries_examined > (2 * initial_list_len) ) ||
		  ( cache_ptr->dLRU_list_size <= cache_ptr->dirty_index_size ) );

#endif /* H5C_MAINTAIN_CLEAN_AND_DIRTY_LRU_LISTS */

    } else {

        HDassert( H5C_MAINTAIN_CLEAN_AND_DIRTY_LRU_LISTS );

        initial_list_len = cache_ptr->cLRU_list_len;
        entry_ptr = cache_ptr->cLRU_tail_ptr;

        while ( ( (cache_ptr->index_size + space_needed)
                  >
                  cache_ptr->max_cache_size
                )
                &&
                ( entries_examined <= initial_list_len )
                &&
                ( entry_ptr != NULL )
              )
        {
            HDassert( ! (entry_ptr->is_protected) );
            HDassert( ! (entry_ptr->is_read_only) );
            HDassert( (entry_ptr->ro_ref_count) == 0 );
            HDassert( ! (entry_ptr->is_dirty) );

            prev_ptr = entry_ptr->aux_prev;

#ifdef H5_HAVE_PARALLEL
            if(!(entry_ptr->coll_access)) {
#endif /* H5_HAVE_PARALLEL */
                if(H5C__flush_single_entry(f, dxpl_id, entry_ptr, H5C__FLUSH_INVALIDATE_FLAG | H5C__DEL_FROM_SLIST_ON_DESTROY_FLAG) < 0)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to flush entry")
#ifdef H5_HAVE_PARALLEL
            } /* end if */
#endif /* H5_HAVE_PARALLEL */

	    /* we are scanning the clean LRU, so the serialize function
	     * will not be called on any entry -- thus there is no
	     * concern about the list being modified out from under
	     * this function.
	     */

            entry_ptr = prev_ptr;
	    entries_examined++;
        }
    }

done:

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_make_space_in_cache() */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_validate_lru_list
 *
 * Purpose:     Debugging function that scans the LRU list for errors.
 *
 *		If an error is detected, the function generates a
 *		diagnostic and returns FAIL.  If no error is detected,
 *		the function returns SUCCEED.
 *
 * Return:      FAIL if error is detected, SUCCEED otherwise.
 *
 * Programmer:  John Mainzer, 7/14/05
 *
 * Changes:
 *
 *		Added code to verify that the LRU contains no pinned 
 *		entries.                        JRM -- 4/25/14
 *
 *-------------------------------------------------------------------------
 */
#if H5C_DO_EXTREME_SANITY_CHECKS

static herr_t
H5C_validate_lru_list(H5C_t * cache_ptr)
{
    herr_t		ret_value = SUCCEED;      /* Return value */
    int32_t             len = 0;
    size_t              size = 0;
    H5C_cache_entry_t *	entry_ptr = NULL;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    if ( ( ( cache_ptr->LRU_head_ptr == NULL )
           ||
           ( cache_ptr->LRU_tail_ptr == NULL )
         )
         &&
         ( cache_ptr->LRU_head_ptr != cache_ptr->LRU_tail_ptr )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 1 failed")
    }

    if(cache_ptr->LRU_list_len < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 2 failed")

    if ( ( cache_ptr->LRU_list_len == 1 )
         &&
         ( ( cache_ptr->LRU_head_ptr != cache_ptr->LRU_tail_ptr )
           ||
           ( cache_ptr->LRU_head_ptr == NULL )
           ||
           ( cache_ptr->LRU_head_ptr->size != cache_ptr->LRU_list_size )
         )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 3 failed")
    }

    if ( ( cache_ptr->LRU_list_len >= 1 )
         &&
         ( ( cache_ptr->LRU_head_ptr == NULL )
           ||
           ( cache_ptr->LRU_head_ptr->prev != NULL )
           ||
           ( cache_ptr->LRU_tail_ptr == NULL )
           ||
           ( cache_ptr->LRU_tail_ptr->next != NULL )
         )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 4 failed")
    }

    entry_ptr = cache_ptr->LRU_head_ptr;
    while ( entry_ptr != NULL )
    {

        if ( ( entry_ptr != cache_ptr->LRU_head_ptr ) &&
             ( ( entry_ptr->prev == NULL ) ||
               ( entry_ptr->prev->next != entry_ptr ) ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 5 failed")
        }

        if ( ( entry_ptr != cache_ptr->LRU_tail_ptr ) &&
             ( ( entry_ptr->next == NULL ) ||
               ( entry_ptr->next->prev != entry_ptr ) ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 6 failed")
        }

        if ( ( entry_ptr->is_pinned ) || 
             ( entry_ptr->pinned_from_client ) ||
             ( entry_ptr->pinned_from_cache ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 7 failed")
        }

        len++;
        size += entry_ptr->size;
        entry_ptr = entry_ptr->next;
    }

    if ( ( cache_ptr->LRU_list_len != len ) ||
         ( cache_ptr->LRU_list_size != size ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 8 failed")
    }

done:

    if ( ret_value != SUCCEED ) {

        HDassert(0);
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_validate_lru_list() */

#endif /* H5C_DO_EXTREME_SANITY_CHECKS */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_validate_pinned_entry_list
 *
 * Purpose:     Debugging function that scans the pinned entry list for 
 *              errors.
 *
 *		If an error is detected, the function generates a
 *		diagnostic and returns FAIL.  If no error is detected,
 *		the function returns SUCCEED.
 *
 * Return:      FAIL if error is detected, SUCCEED otherwise.
 *
 * Programmer:  John Mainzer, 4/25/14
 *
 * Changes:
 *
 *		None.
 *
 *-------------------------------------------------------------------------
 */
#if H5C_DO_EXTREME_SANITY_CHECKS

static herr_t
H5C_validate_pinned_entry_list(H5C_t * cache_ptr)
{
    herr_t		ret_value = SUCCEED;      /* Return value */
    int32_t             len = 0;
    size_t              size = 0;
    H5C_cache_entry_t *	entry_ptr = NULL;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    if ( ( ( cache_ptr->pel_head_ptr == NULL )
           ||
           ( cache_ptr->pel_tail_ptr == NULL )
         )
         &&
         ( cache_ptr->pel_head_ptr != cache_ptr->pel_tail_ptr )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 1 failed")
    }

    if(cache_ptr->pel_len < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 2 failed")

    if ( ( cache_ptr->pel_len == 1 )
         &&
         ( ( cache_ptr->pel_head_ptr != cache_ptr->pel_tail_ptr )
           ||
           ( cache_ptr->pel_head_ptr == NULL )
           ||
           ( cache_ptr->pel_head_ptr->size != cache_ptr->pel_size )
         )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 3 failed")
    }

    if ( ( cache_ptr->pel_len >= 1 )
         &&
         ( ( cache_ptr->pel_head_ptr == NULL )
           ||
           ( cache_ptr->pel_head_ptr->prev != NULL )
           ||
           ( cache_ptr->pel_tail_ptr == NULL )
           ||
           ( cache_ptr->pel_tail_ptr->next != NULL )
         )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 4 failed")
    }

    entry_ptr = cache_ptr->pel_head_ptr;
    while ( entry_ptr != NULL )
    {

        if ( ( entry_ptr != cache_ptr->pel_head_ptr ) &&
             ( ( entry_ptr->prev == NULL ) ||
               ( entry_ptr->prev->next != entry_ptr ) ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 5 failed")
        }

        if ( ( entry_ptr != cache_ptr->pel_tail_ptr ) &&
             ( ( entry_ptr->next == NULL ) ||
               ( entry_ptr->next->prev != entry_ptr ) ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 6 failed")
        }

        if ( ! entry_ptr->is_pinned ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 7 failed")
        }

        if ( ! ( ( entry_ptr->pinned_from_client ) ||
                 ( entry_ptr->pinned_from_cache ) ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 8 failed")
        }

        len++;
        size += entry_ptr->size;
        entry_ptr = entry_ptr->next;
    }

    if ( ( cache_ptr->pel_len != len ) ||
         ( cache_ptr->pel_size != size ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 9 failed")
    }

done:

    if ( ret_value != SUCCEED ) {

        HDassert(0);
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_validate_pinned_entry_list() */

#endif /* H5C_DO_EXTREME_SANITY_CHECKS */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_validate_protected_entry_list
 *
 * Purpose:     Debugging function that scans the protected entry list for 
 *              errors.
 *
 *		If an error is detected, the function generates a
 *		diagnostic and returns FAIL.  If no error is detected,
 *		the function returns SUCCEED.
 *
 * Return:      FAIL if error is detected, SUCCEED otherwise.
 *
 * Programmer:  John Mainzer, 4/25/14
 *
 * Changes:
 *
 *		None.
 *
 *-------------------------------------------------------------------------
 */
#if H5C_DO_EXTREME_SANITY_CHECKS

static herr_t
H5C_validate_protected_entry_list(H5C_t * cache_ptr)
{
    herr_t		ret_value = SUCCEED;      /* Return value */
    int32_t             len = 0;
    size_t              size = 0;
    H5C_cache_entry_t *	entry_ptr = NULL;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );

    if(((cache_ptr->pl_head_ptr == NULL) || (cache_ptr->pl_tail_ptr == NULL))
             && (cache_ptr->pl_head_ptr != cache_ptr->pl_tail_ptr))
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 1 failed")

    if(cache_ptr->pl_len < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 2 failed")

    if ( ( cache_ptr->pl_len == 1 )
         &&
         ( ( cache_ptr->pl_head_ptr != cache_ptr->pl_tail_ptr )
           ||
           ( cache_ptr->pl_head_ptr == NULL )
           ||
           ( cache_ptr->pl_head_ptr->size != cache_ptr->pl_size )
         )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 3 failed")
    }

    if ( ( cache_ptr->pl_len >= 1 )
         &&
         ( ( cache_ptr->pl_head_ptr == NULL )
           ||
           ( cache_ptr->pl_head_ptr->prev != NULL )
           ||
           ( cache_ptr->pl_tail_ptr == NULL )
           ||
           ( cache_ptr->pl_tail_ptr->next != NULL )
         )
       ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 4 failed")
    }

    entry_ptr = cache_ptr->pl_head_ptr;
    while ( entry_ptr != NULL )
    {

        if ( ( entry_ptr != cache_ptr->pl_head_ptr ) &&
             ( ( entry_ptr->prev == NULL ) ||
               ( entry_ptr->prev->next != entry_ptr ) ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 5 failed")
        }

        if ( ( entry_ptr != cache_ptr->pl_tail_ptr ) &&
             ( ( entry_ptr->next == NULL ) ||
               ( entry_ptr->next->prev != entry_ptr ) ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 6 failed")
        }

        if ( ! entry_ptr->is_protected ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 7 failed")
        }

        if ( ( entry_ptr->is_read_only ) &&
             ( entry_ptr->ro_ref_count <= 0 ) ) {

            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 8 failed")
        }

        len++;
        size += entry_ptr->size;
        entry_ptr = entry_ptr->next;
    }

    if ( ( cache_ptr->pl_len != len ) ||
         ( cache_ptr->pl_size != size ) ) {

        HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "Check 9 failed")
    }

done:

    if ( ret_value != SUCCEED ) {

        HDassert(0);
    }

    FUNC_LEAVE_NOAPI(ret_value)

} /* H5C_validate_protected_entry_list() */

#endif /* H5C_DO_EXTREME_SANITY_CHECKS */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_entry_in_skip_list
 *
 * Purpose:     Debugging function that scans skip list to see if it 
 *		is in present.  We need this, as it is possible for 
 *		an entry to be in the skip list twice.
 *
 * Return:      FALSE if the entry is not in the skip list, and TRUE 
 *		if it is.
 *
 * Programmer:  John Mainzer, 11/1/14
 *
 * Changes:
 *
 *		None.
 *
 *-------------------------------------------------------------------------
 */
#if H5C_DO_SLIST_SANITY_CHECKS

static hbool_t
H5C_entry_in_skip_list(H5C_t * cache_ptr, H5C_cache_entry_t *target_ptr)
{
    hbool_t in_slist              = FALSE;
    H5SL_node_t *       node_ptr  = NULL;
    H5C_cache_entry_t *	entry_ptr = NULL;

    HDassert( cache_ptr );
    HDassert( cache_ptr->magic == H5C__H5C_T_MAGIC );
    HDassert( cache_ptr->slist_ptr );

    node_ptr = H5SL_first(cache_ptr->slist_ptr);

    while ( ( node_ptr != NULL ) && ( ! in_slist ) )
    {
        entry_ptr = (H5C_cache_entry_t *)H5SL_item(node_ptr);

	HDassert( entry_ptr );
	HDassert( entry_ptr->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC );
        HDassert( entry_ptr->is_dirty );
        HDassert( entry_ptr->in_slist );

        if ( entry_ptr == target_ptr ) {

	    in_slist = TRUE;

	} else {

	    node_ptr = H5SL_next(node_ptr);
	}
    }

    return(in_slist);

} /* H5C_entry_in_skip_list() */
#endif /* H5C_DO_SLIST_SANITY_CHECKS */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C__flush_marked_entries
 *
 * Purpose:     Flushes all marked entries in the cache.
 *
 * Return:      FAIL if error is detected, SUCCEED otherwise.
 *
 * Programmer:  Mike McGreevy
 *              November 3, 2010
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C__flush_marked_entries(H5F_t * f, hid_t dxpl_id)
{ 
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_PACKAGE

    /* Assertions */
    HDassert(f != NULL);

    /* Flush all marked entries */
    if(H5C_flush_cache(f, dxpl_id, H5C__FLUSH_MARKED_ENTRIES_FLAG | H5C__FLUSH_IGNORE_PROTECTED_FLAG) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "Can't flush cache")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__flush_marked_entries */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_cork
 *
 * Purpose:     To cork/uncork/get cork status of an object depending on "action":
 *		H5C__SET_CORK: 
 *			To cork the object
 *			Return error if the object is already corked
 *		H5C__UNCORK:
 *			To uncork the obejct
 *			Return error if the object is not corked
 * 		H5C__GET_CORKED:
 *			To retrieve the cork status of an object in
 *			the parameter "corked"
 *		
 * Return:      Success:        Non-negative
 *              Failure:        Negative
 *
 * Programmer:  Vailin Choi
 *		January 2014
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_cork(H5C_t *cache_ptr, haddr_t obj_addr, unsigned action, hbool_t *corked) 
{
    H5C_tag_info_t *tag_info;	/* Points to a tag info struct */
    herr_t              ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    /* Assertions */
    HDassert(cache_ptr != NULL);
    HDassert(H5F_addr_defined(obj_addr));
    HDassert(action == H5C__SET_CORK || action == H5C__UNCORK || action == H5C__GET_CORKED);

    /* Search the list of corked object addresses in the cache */
    tag_info = (H5C_tag_info_t *)H5SL_search(cache_ptr->tag_list, &obj_addr);

    if(H5C__GET_CORKED == action) {
        HDassert(corked);
        if(tag_info != NULL && tag_info->corked)
            *corked = TRUE;
        else
            *corked = FALSE;
    } /* end if */
    else {
        /* Sanity check */
        HDassert(H5C__SET_CORK == action || H5C__UNCORK == action);

        /* Perform appropriate action */
        if(H5C__SET_CORK == action) {
            /* Check if this is the first entry for this tagged object */
            if(NULL == tag_info) {
                /* Allocate new tag info struct */
                if(NULL == (tag_info = H5FL_CALLOC(H5C_tag_info_t)))
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTALLOC, FAIL, "can't allocate tag info for cache entry")

                /* Set the tag for all entries */
                tag_info->tag = obj_addr;

                /* Insert tag info into skip list */
                if(H5SL_insert(cache_ptr->tag_list, tag_info, &(tag_info->tag)) < 0 )
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTINSERT, FAIL, "can't insert tag info in skip list")
            } /* end if */
            else {
                /* Check for object already corked */
                if(tag_info->corked)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTCORK, FAIL, "object already corked")
                HDassert(tag_info->entry_cnt > 0 && tag_info->head);
            } /* end else */

            /* Set the corked status for the entire object */
            tag_info->corked = TRUE;
        } /* end if */
        else {
            /* Sanity check */
            HDassert(tag_info);

            /* Check for already uncorked */
            if(!tag_info->corked)
                HGOTO_ERROR(H5E_CACHE, H5E_CANTUNCORK, FAIL, "object already uncorked")

            /* Set the corked status for the entire object */
            tag_info->corked = FALSE;

            /* Remove the tag info from the tag list, if there's no more entries with this tag */
            if(0 == tag_info->entry_cnt) {
                /* Sanity check */
                HDassert(NULL == tag_info->head);

                if(H5SL_remove(cache_ptr->tag_list, &(tag_info->tag)) != tag_info)
                    HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove tag info from list")

                /* Release the tag info */
                tag_info = H5FL_FREE(H5C_tag_info_t, tag_info);
            } /* end if */
            else
                HDassert(NULL != tag_info->head);
        } /* end else */
    } /* end else */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C_cork() */


/*-------------------------------------------------------------------------
 * Function:    H5C__mark_flush_dep_dirty()
 *
 * Purpose:     Recursively propagate the flush_dep_ndirty_children flag
 *              up the dependency chain in response to entry either
 *              becoming dirty or having its flush_dep_ndirty_children
 *              increased from 0.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              11/13/12
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__mark_flush_dep_dirty(H5C_cache_entry_t * entry)
{
    unsigned u;                         /* Local index variable */
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(entry);

    /* Iterate over the parent entries, if any */
    for(u = 0; u < entry->flush_dep_nparents; u++) {
	/* Sanity check */
	HDassert(entry->flush_dep_parent[u]->flush_dep_ndirty_children < entry->flush_dep_parent[u]->flush_dep_nchildren);

	/* Adjust the parent's number of dirty children */
	entry->flush_dep_parent[u]->flush_dep_ndirty_children++;

        /* If the parent has a 'notify' callback, send a 'child entry dirtied' notice */
        if(entry->flush_dep_parent[u]->type->notify &&
                (entry->flush_dep_parent[u]->type->notify)(H5C_NOTIFY_ACTION_CHILD_DIRTIED, entry->flush_dep_parent[u]) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry dirty flag set")
    } /* end for */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__mark_flush_dep_dirty() */


/*-------------------------------------------------------------------------
 * Function:    H5C__mark_flush_dep_clean()
 *
 * Purpose:     Recursively propagate the flush_dep_ndirty_children flag
 *              up the dependency chain in response to entry either
 *              becoming clean or having its flush_dep_ndirty_children
 *              reduced to 0.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Neil Fortner
 *              11/13/12
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__mark_flush_dep_clean(H5C_cache_entry_t * entry)
{
    unsigned u;                         /* Local index variable */
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(entry);

    /* Iterate over the parent entries, if any */
    for(u = 0; u < entry->flush_dep_nparents; u++) {
	/* Sanity check */
	HDassert(entry->flush_dep_parent[u]->flush_dep_ndirty_children > 0);

	/* Adjust the parent's number of dirty children */
	entry->flush_dep_parent[u]->flush_dep_ndirty_children--;

        /* If the parent has a 'notify' callback, send a 'child entry cleaned' notice */
        if(entry->flush_dep_parent[u]->type->notify &&
                (entry->flush_dep_parent[u]->type->notify)(H5C_NOTIFY_ACTION_CHILD_CLEANED, entry->flush_dep_parent[u]) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry dirty flag reset")
    } /* end for */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__mark_flush_dep_clean() */


/*-------------------------------------------------------------------------
 * Function:    H5C__mark_flush_dep_serialized()
 *
 * Purpose:     Decrement the flush_dep_nunser_children fields of all the 
 *		target entry's flush dependency parents in response to 
 *		the target entry becoming serialized.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              8/30/16
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C__mark_flush_dep_serialized(H5C_cache_entry_t * entry_ptr)
{
    unsigned u;                         /* Local index variable */
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(entry_ptr);

    /* Iterate over the parent entries, if any */
    for(u = 0; u < entry_ptr->flush_dep_nparents; u++) {

        HDassert(entry_ptr->flush_dep_parent);
        HDassert(entry_ptr->flush_dep_parent[u]->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
        HDassert(entry_ptr->flush_dep_parent[u]->flush_dep_nunser_children > 0);

        /* decrement the parents number of unserialized children */
        entry_ptr->flush_dep_parent[u]->flush_dep_nunser_children--;

        /* If the parent has a 'notify' callback, send a 'child entry serialized' notice */
        if(entry_ptr->flush_dep_parent[u]->type->notify &&
                (entry_ptr->flush_dep_parent[u]->type->notify)(H5C_NOTIFY_ACTION_CHILD_SERIALIZED, entry_ptr->flush_dep_parent[u]) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry serialized flag set")
    } /* end for */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__mark_flush_dep_serialized() */


/*-------------------------------------------------------------------------
 * Function:    H5C__mark_flush_dep_unserialized()
 *
 * Purpose:     Decrement the flush_dep_nunser_children fields of all the 
 *              target entry's flush dependency parents in response to 
 *              the target entry becoming unserialized.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  John Mainzer
 *              8/30/16
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C__mark_flush_dep_unserialized(H5C_cache_entry_t * entry_ptr)
{
    unsigned u;                         /* Local index variable */
    herr_t ret_value = SUCCEED;         /* Return value */

    FUNC_ENTER_STATIC

    /* Sanity checks */
    HDassert(entry_ptr);

    /* Iterate over the parent entries, if any */
    for(u = 0; u < entry_ptr->flush_dep_nparents; u++) {
        /* Sanity check */
        HDassert(entry_ptr->flush_dep_parent);
        HDassert(entry_ptr->flush_dep_parent[u]->magic == H5C__H5C_CACHE_ENTRY_T_MAGIC);
        HDassert(entry_ptr->flush_dep_parent[u]->flush_dep_nunser_children < 
                 entry_ptr->flush_dep_parent[u]->flush_dep_nchildren);

        /* increment parents number of usserialized children */
        entry_ptr->flush_dep_parent[u]->flush_dep_nunser_children++;

        /* If the parent has a 'notify' callback, send a 'child entry unserialized' notice */
        if(entry_ptr->flush_dep_parent[u]->type->notify &&
                (entry_ptr->flush_dep_parent[u]->type->notify)(H5C_NOTIFY_ACTION_CHILD_UNSERIALIZED, entry_ptr->flush_dep_parent[u]) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify parent about child entry serialized flag reset")
    } /* end for */

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__mark_flush_dep_unserialized() */


#ifndef NDEBUG
/*-------------------------------------------------------------------------
 * Function:    H5C__assert_flush_dep_nocycle()
 *
 * Purpose:     Assert recursively that base_entry is not the same as
 *              entry, and perform the same assertion on all of entry's
 *              flush dependency parents.  This is used to detect cycles
 *              created by flush dependencies.
 *
 * Return:      void
 *
 * Programmer:  Neil Fortner
 *              12/10/12
 *
 *-------------------------------------------------------------------------
 */
static void
H5C__assert_flush_dep_nocycle(const H5C_cache_entry_t * entry,
    const H5C_cache_entry_t * base_entry)
{
    unsigned u;                         /* Local index variable */

    FUNC_ENTER_STATIC_NOERR

    /* Sanity checks */
    HDassert(entry);
    HDassert(base_entry);

    /* Make sure the entries are not the same */
    HDassert(base_entry != entry);

    /* Iterate over entry's parents (if any) */
    for(u = 0; u < entry->flush_dep_nparents; u++)
	H5C__assert_flush_dep_nocycle(entry->flush_dep_parent[u], base_entry);

    FUNC_LEAVE_NOAPI_VOID
} /* H5C__assert_flush_dep_nocycle() */
#endif /* NDEBUG */


/*-------------------------------------------------------------------------
 * Function:    H5C__generate_image
 *
 * Purpose:     Serialize an entry and generate its image.
 *
 * Note:	This may cause the entry to be re-sized and/or moved in
 *              the cache.  
 *
 *              As we will not update the metadata cache's data structures 
 *              until we we finish the write, we must touch up these 
 *              data structures for size and location changes even if we 
 *              are about to delete the entry from the cache (i.e. on a 
 *              flush destroy).
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Mohamad Chaarawi
 *              2/10/16
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5C__generate_image(const H5F_t *f, H5C_t *cache_ptr, H5C_cache_entry_t *entry_ptr, 
    hid_t dxpl_id)
{
    haddr_t		new_addr = HADDR_UNDEF;
    haddr_t		old_addr = HADDR_UNDEF;
    size_t		new_len = 0;
    unsigned            serialize_flags = H5C__SERIALIZE_NO_FLAGS_SET;
    herr_t              ret_value = SUCCEED;

    FUNC_ENTER_STATIC

    /* Sanity check */
    HDassert(!entry_ptr->image_up_to_date);

    /* make note of the entry's current address */
    old_addr = entry_ptr->addr;

    /* Call client's pre-serialize callback, if there's one */
    if(entry_ptr->type->pre_serialize && 
            (entry_ptr->type->pre_serialize)(f, dxpl_id, (void *)entry_ptr,
                entry_ptr->addr, entry_ptr->size, &new_addr, &new_len, &serialize_flags) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to pre-serialize entry")

    /* Check for any flags set in the pre-serialize callback */
    if(serialize_flags != H5C__SERIALIZE_NO_FLAGS_SET) {
        /* Check for unexpected flags from serialize callback */
        if(serialize_flags & ~(H5C__SERIALIZE_RESIZED_FLAG | 
                               H5C__SERIALIZE_MOVED_FLAG))
            HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unknown serialize flag(s)")

#ifdef H5_HAVE_PARALLEL
        /* In the parallel case, resizes and moves in
         * the serialize operation can cause problems.
         * If they occur, scream and die.
         *
         * At present, in the parallel case, the aux_ptr
         * will only be set if there is more than one
         * process.  Thus we can use this to detect
         * the parallel case.
         *
         * This works for now, but if we start using the
         * aux_ptr for other purposes, we will have to
         * change this test accordingly.
         *
         * NB: While this test detects entryies that attempt
         *     to resize or move themselves during a flush
         *     in the parallel case, it will not detect an
         *     entry that dirties, resizes, and/or moves
         *     other entries during its flush.
         *
         *     From what Quincey tells me, this test is
         *     sufficient for now, as any flush routine that
         *     does the latter will also do the former.
         *
         *     If that ceases to be the case, further
         *     tests will be necessary.
         */
        if(cache_ptr->aux_ptr != NULL)
            HGOTO_ERROR(H5E_CACHE, H5E_SYSTEM, FAIL, "resize/move in serialize occured in parallel case.")
#endif

        /* If required, resize the buffer and update the entry and the cache
         * data structures */
        if(serialize_flags & H5C__SERIALIZE_RESIZED_FLAG) {
            /* Allocate a new image buffer */
            if(NULL == (entry_ptr->image_ptr = H5MM_realloc(entry_ptr->image_ptr, new_len + H5C_IMAGE_EXTRA_SPACE)))
                HGOTO_ERROR(H5E_CACHE, H5E_CANTALLOC, FAIL, "memory allocation failed for on disk image buffer")
#if H5C_DO_MEMORY_SANITY_CHECKS
            HDmemcpy(((uint8_t *)entry_ptr->image_ptr) + new_len, H5C_IMAGE_SANITY_VALUE, H5C_IMAGE_EXTRA_SPACE);
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */

            /* Update statistics for resizing the entry */
            H5C__UPDATE_STATS_FOR_ENTRY_SIZE_CHANGE(cache_ptr, entry_ptr, new_len);

            /* update the hash table for the size change */
            H5C__UPDATE_INDEX_FOR_SIZE_CHANGE(cache_ptr, entry_ptr->size, \
                                              new_len, entry_ptr, !(entry_ptr->is_dirty));

            /* The entry can't be protected since we are in the process of
             * flushing it.  Thus we must update the replacement policy data
             * structures for the size change.  The macro deals with the pinned
             * case.
             */
            H5C__UPDATE_RP_FOR_SIZE_CHANGE(cache_ptr, entry_ptr, new_len);

            /* As we haven't updated the cache data structures for 
             * for the flush or flush destroy yet, the entry should
             * be in the slist.  Thus update it for the size change.
             */
            HDassert(entry_ptr->in_slist);
            H5C__UPDATE_SLIST_FOR_SIZE_CHANGE(cache_ptr, entry_ptr->size, new_len);

            /* finally, update the entry for its new size */
            entry_ptr->size = new_len;
        } /* end if */

        /* If required, udate the entry and the cache data structures 
         * for a move 
         */
        if(serialize_flags & H5C__SERIALIZE_MOVED_FLAG) {
            H5C__UPDATE_STATS_FOR_MOVE(cache_ptr, entry_ptr);

            /* We must update cache data structures for the change in address */
            if(entry_ptr->addr == old_addr) {

                /* we must update cache data structures for the 
                 * change in address.
                 */

                /* update stats and entries relocated counter */
                H5C__UPDATE_STATS_FOR_MOVE(cache_ptr, entry_ptr)
                cache_ptr->entries_relocated_counter++;

                /* delete the entry from the hash table and the slist */
                H5C__DELETE_FROM_INDEX(cache_ptr, entry_ptr, FAIL);
                H5C__REMOVE_ENTRY_FROM_SLIST(cache_ptr, entry_ptr, FALSE);

                /* update the entry for its new address */
                entry_ptr->addr = new_addr;

                /* and then reinsert in the index and slist */
                H5C__INSERT_IN_INDEX(cache_ptr, entry_ptr, FAIL);
                H5C__INSERT_ENTRY_IN_SLIST(cache_ptr, entry_ptr, FAIL);
            } /* end if */
            else /* move is already done for us -- just do sanity checks */
                HDassert(entry_ptr->addr == new_addr);
        } /* end if */
    } /* end if(serialize_flags != H5C__SERIALIZE_NO_FLAGS_SET) */

    /* Serialize object into buffer */
    if(entry_ptr->type->serialize(f, entry_ptr->image_ptr, entry_ptr->size, (void *)entry_ptr) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTFLUSH, FAIL, "unable to serialize entry")
#if H5C_DO_MEMORY_SANITY_CHECKS
    HDassert(0 == HDmemcmp(((uint8_t *)entry_ptr->image_ptr) + entry_ptr->size, H5C_IMAGE_SANITY_VALUE, H5C_IMAGE_EXTRA_SPACE));
#endif /* H5C_DO_MEMORY_SANITY_CHECKS */
    entry_ptr->image_up_to_date = TRUE;

    if(entry_ptr->flush_dep_nparents > 0)
        if(H5C__mark_flush_dep_serialized(entry_ptr) < 0)
            HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "Can't propagate serialization status to fd parents")

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__generate_image */


/*-------------------------------------------------------------------------
 *
 * Function:    H5C_remove_entry
 *
 * Purpose:     Remove an entry from the cache.  Must be not protected, pinned,
 *		dirty, involved in flush dependencies, etc.
 *
 * Return:      Non-negative on success/Negative on failure
 *
 * Programmer:  Quincey Koziol
 *              September 17, 2016
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5C_remove_entry(void *_entry)
{
    H5C_cache_entry_t *entry = (H5C_cache_entry_t *)_entry;     /* Entry to remove */
    H5C_t *cache;               /* Cache for file */
    herr_t ret_value = SUCCEED; /* Return value */

    FUNC_ENTER_NOAPI(FAIL)

    /* Sanity checks */
    HDassert(entry);
    HDassert(entry->ring != H5C_RING_UNDEFINED);
    cache = entry->cache_ptr;
    HDassert(cache);
    HDassert(cache->magic == H5C__H5C_T_MAGIC);

    /* Check for error conditions */
    if(entry->is_dirty)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove dirty entry from cache")
    if(entry->is_protected)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove protected entry from cache")
    if(entry->is_pinned)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove pinned entry from cache")
    if(entry->flush_dep_nparents > 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove entry with flush dependency parents from cache")
    if(entry->flush_dep_nchildren > 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove entry with flush dependency children from cache")

    /* Additional internal cache consistency checks */
    HDassert(!entry->in_slist);
    HDassert(!entry->flush_marker);
    HDassert(!entry->flush_in_progress);

    /* Note that the algorithm below is (very) similar to the set of operations
     * in H5C__flush_single_entry() and should be kept in sync with changes
     * to that code. - QAK, 2016/11/30
     */

    /* Update stats, as if we are "destroying" and taking ownership of the entry */
    H5C__UPDATE_STATS_FOR_EVICTION(cache, entry, TRUE)

    /* If the entry's type has a 'notify' callback, send a 'before eviction'
     * notice while the entry is still fully integrated in the cache.
     */
    if(entry->type->notify && (entry->type->notify)(H5C_NOTIFY_ACTION_BEFORE_EVICT, entry) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTNOTIFY, FAIL, "can't notify client about entry to evict")

    /* Update the cache internal data structures as appropriate for a destroy.
     * Specifically:
     *	1) Delete it from the index
     *	2) Update the replacement policy for eviction
     *	3) Remove it from the tag list for this object
     */

    H5C__DELETE_FROM_INDEX(cache, entry, FAIL)

    H5C__UPDATE_RP_FOR_EVICTION(cache, entry, FAIL)

    /* Remove entry from tag list */
    if(H5C__untag_entry(cache, entry) < 0)
        HGOTO_ERROR(H5E_CACHE, H5E_CANTREMOVE, FAIL, "can't remove entry from tag list")

    /* Increment entries_removed_counter and set last_entry_removed_ptr.
     * As we me be about to free the entry, recall that last_entry_removed_ptr
     * must NEVER be dereferenced.
     *
     * Recall that these fields are maintained to allow functions that perform
     * scans of lists of entries to detect the unexpected removal of entries
     * (via expunge, eviction, or take ownership at present), so that they can
     * re-start their scans if necessary.
     *
     * Also check if the entry we are watching for removal is being
     * removed (usually the 'next' entry for an iteration) and reset
     * it to indicate that it was removed.
     */
    cache->entries_removed_counter++;
    cache->last_entry_removed_ptr = entry;
    if(entry == cache->entry_watched_for_removal)
        cache->entry_watched_for_removal = NULL;

    /* Internal cache data structures should now be up to date, and 
     * consistant with the status of the entry.  
     *
     * Now clean up internal cache fields if appropriate.
     */

    /* Free the buffer for the on disk image */
    if(entry->image_ptr != NULL)
        entry->image_ptr = H5MM_xfree(entry->image_ptr);

    /* Reset the pointer to the cache the entry is within */
    entry->cache_ptr = NULL;

    /* Client is taking ownership of the entry.  Set bad magic here so the
     * cache will choke unless the entry is re-inserted properly
     */
    entry->magic = H5C__H5C_CACHE_ENTRY_T_BAD_MAGIC;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* H5C__remove_entry() */

