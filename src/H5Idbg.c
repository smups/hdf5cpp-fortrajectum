/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by the Board of Trustees of the University of Illinois.         *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://www.hdfgroup.org/licenses.               *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * H5Idbg.c - Debugging routines for handling IDs
 */

/****************/
/* Module Setup */
/****************/

#include "H5Imodule.h" /* This source code file is part of the H5I module */

/***********/
/* Headers */
/***********/
#include "H5private.h"   /* Generic Functions                        */
#include "H5Dprivate.h"  /* Datasets                                 */
#include "H5Eprivate.h"  /* Error handling                           */
#include "H5Gprivate.h"  /* Groups                                   */
#include "H5Ipkg.h"      /* IDs                                      */
#include "H5RSprivate.h" /* Reference-counted strings                */
#include "H5Tprivate.h"  /* Datatypes                                */

/****************/
/* Local Macros */
/****************/

/******************/
/* Local Typedefs */
/******************/

/********************/
/* Package Typedefs */
/********************/

/********************/
/* Local Prototypes */
/********************/

static int H5I__id_dump_cb(void *_item, void *_key, void *_udata);

/*********************/
/* Package Variables */
/*********************/

/*****************************/
/* Library Private Variables */
/*****************************/

/*******************/
/* Local Variables */
/*******************/

/*-------------------------------------------------------------------------
 * Function:    H5I__id_dump_cb
 *
 * Purpose:     Dump the contents of an ID to stderr for debugging.
 *
 * Return:      H5_ITER_CONT (always)
 *
 *-------------------------------------------------------------------------
 */
static int
H5I__id_dump_cb(void *_item, void H5_ATTR_UNUSED *_key, void *_udata)
{
    H5I_id_info_t    *info = (H5I_id_info_t *)_item; /* Pointer to the ID node */
    H5I_type_t        type = *(H5I_type_t *)_udata;  /* User data */
    const H5G_name_t *path = NULL;                   /* Path to file object */

    FUNC_ENTER_STATIC_NOERR

    HDfprintf(stderr, "         id = %llu\n", (unsigned long long)(info->id));
    HDfprintf(stderr, "         count = %u\n", info->count);
    HDfprintf(stderr, "         obj   = 0x%8p\n", info->object);
    HDfprintf(stderr, "         marked = %d\n", info->marked);

    /* Get the group location, so we get get the name */
    switch (type) {
        case H5I_GROUP: {
            H5_GCC_DIAG_OFF("cast-qual")
            path = H5G_nameof((H5G_t *)info->object);
            H5_GCC_DIAG_ON("cast-qual")
            break;
        }
        case H5I_DATASET: {
            path = H5D_nameof(info->object);
            break;
        }
        case H5I_DATATYPE: {
            path = H5T_nameof(info->object);
            break;
        }
        case H5I_UNINIT:
        case H5I_BADID:
        case H5I_FILE:
        case H5I_DATASPACE:
        case H5I_ATTR:
        case H5I_REFERENCE:
        case H5I_VFL:
        case H5I_GENPROP_CLS:
        case H5I_GENPROP_LST:
        case H5I_ERROR_CLASS:
        case H5I_ERROR_MSG:
        case H5I_ERROR_STACK:
        case H5I_NTYPES:
        default:
            break; /* Other types of IDs are not stored in files */
    }

    if (path) {
        if (path->user_path_r)
            HDfprintf(stderr, "                user_path = %s\n", H5RS_get_str(path->user_path_r));
        if (path->full_path_r)
            HDfprintf(stderr, "                full_path = %s\n", H5RS_get_str(path->full_path_r));
    }

    FUNC_LEAVE_NOAPI(H5_ITER_CONT)
} /* end H5I__id_dump_cb() */

/*-------------------------------------------------------------------------
 * Function:    H5I_dump_ids_for_type
 *
 * Purpose:     Dump the contents of a type to stderr for debugging.
 *
 * Return:      SUCCEED/FAIL
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5I_dump_ids_for_type(H5I_type_t type)
{
    H5I_type_info_t *type_info = NULL;

    FUNC_ENTER_NOAPI_NOERR

    HDfprintf(stderr, "Dumping ID type %d\n", (int)type);
    type_info = H5I_type_info_array_g[type];

    if (type_info) {

        H5I_id_info_t *item = NULL;
        H5I_id_info_t *tmp  = NULL;

        /* Header */
        HDfprintf(stderr, "     init_count = %u\n", type_info->init_count);
        HDfprintf(stderr, "     reserved   = %u\n", type_info->cls->reserved);
        HDfprintf(stderr, "     id_count   = %llu\n", (unsigned long long)type_info->id_count);
        HDfprintf(stderr, "     nextid        = %llu\n", (unsigned long long)type_info->nextid);

        /* List */
        if (type_info->id_count > 0) {
            HDfprintf(stderr, "     List:\n");
            /* Normally we care about the callback's return value
             * (H5I_ITER_CONT, etc.), but this is an iteration over all
             * the IDs so we don't care.
             *
             * XXX: Update this to emit an error message on errors?
             */
            HDfprintf(stderr, "     (HASH TABLE)\n");
            HASH_ITER(hh, type_info->hash_table, item, tmp)
            {
                H5I__id_dump_cb((void *)item, NULL, (void *)&type);
            }
        }
    }
    else
        HDfprintf(stderr, "Global type info/tracking pointer for that type is NULL\n");

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5I_dump_ids_for_type() */
