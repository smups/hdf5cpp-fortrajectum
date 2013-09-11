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

/* Programmer:  Mohamad Chaarawi <chaarawi@hdfgroup.org>
 *
 * Purpose:	IOD plugin client code
 */

#define H5G_PACKAGE		/*suppress error about including H5Gpkg   */
#define H5D_PACKAGE		/*suppress error about including H5Dpkg   */

#include "H5private.h"		/* Generic Functions			*/
#include "H5Dpkg.h"		/* Datasets		  		*/
#include "H5Eprivate.h"		/* Error handling		  	*/
#include "H5Gpkg.h"		/* Groups		  		*/
#include "H5Iprivate.h"		/* IDs			  		*/
#include "H5MMprivate.h"	/* Memory management			*/
#include "H5Pprivate.h"		/* Property lists			*/
#include "H5Sprivate.h"		/* Dataspaces		  		*/
#include "H5VLprivate.h"	/* VOL plugins				*/
#include "H5VLiod.h"            /* Iod VOL plugin			*/
#include "H5VLiod_common.h"
#include "H5VLiod_client.h"
#include "H5WBprivate.h"        /* Wrapped Buffers                      */

#ifdef H5_HAVE_EFF

H5FL_EXTERN(H5VL_iod_file_t);
H5FL_EXTERN(H5VL_iod_attr_t);
H5FL_EXTERN(H5VL_iod_group_t);
H5FL_EXTERN(H5VL_iod_dset_t);
H5FL_EXTERN(H5VL_iod_dtype_t);

/* H5Diterate op-data for VL traversal */
typedef struct {
    size_t buf_size;
    uint8_t *buf_ptr;
    uint32_t checksum;
    uint8_t **off;
    size_t *len;
    hsize_t curr_seq;
    size_t *str_len; /* used only for VL strings */
} H5VL_iod_pre_write_t;

static herr_t H5VL__iod_pre_write_cb(void UNUSED *elem, hid_t type_id, unsigned ndim, 
                                     const hsize_t *point, void *_udata);

static herr_t H5VL__iod_vl_read_finalize(size_t buf_size, void *read_buf, void *user_buf, 
                                         H5S_t *mem_space, hid_t mem_type_id, hid_t dset_type_id);

herr_t 
H5VL_iod_request_decr_rc(H5VL_iod_request_t *request)
{
    FUNC_ENTER_NOAPI_NOINIT_NOERR

    request->ref_count --;

    if(0 == request->ref_count) {
        //request->parent_reqs = (H5VL_iod_request_t **)H5MM_xfree(request->parent_reqs);
        request = (H5VL_iod_request_t *)H5MM_xfree(request);
    }

    FUNC_LEAVE_NOAPI(SUCCEED)
}


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_request_add
 *
 * Purpose:     Adds a request pointer to the Doubly linked list on the
 *              file.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_request_add(H5VL_iod_file_t *file, H5VL_iod_request_t *request)
{
    H5VL_iod_req_info_t *req_info = request->trans_info;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    HDassert(request);

    if (file->request_list_tail) {
        file->request_list_tail->file_next = request;
        request->file_prev = file->request_list_tail;
        file->request_list_tail = request;
    }
    else {
        file->request_list_head = request;
        file->request_list_tail = request;
        request->file_prev = NULL;
    }
    request->file_next = NULL;
    file->num_req ++;

    if(req_info) {
        if (req_info->tail) {
            req_info->tail->trans_next = request;
            request->trans_prev = req_info->tail;
            req_info->tail = request;
        }
        else {
            req_info->head = request;
            req_info->tail = request;
            request->trans_prev = NULL;
        }
        request->trans_next = NULL;
        req_info->num_req ++;

        request->ref_count ++;
    }

    FUNC_LEAVE_NOAPI(SUCCEED)
}


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_request_delete
 *
 * Purpose:     Removes a request pointer from the Doubly linked list on the
 *              file.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_request_delete(H5VL_iod_file_t *file, H5VL_iod_request_t *request)
{
    H5VL_iod_request_t *prev;
    H5VL_iod_request_t *next;
    unsigned u;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    HDassert(request);

    /* decrement ref count on parent requests */
    for(u=0 ; u<request->num_parents ; u++) {
        /* Decrement ref count on request */
        H5VL_iod_request_decr_rc(request->parent_reqs[u]);
    }

    request->parent_reqs = (H5VL_iod_request_t **)H5MM_xfree(request->parent_reqs);

    /* remove the request from the container link list */
    prev = request->file_prev;
    next = request->file_next;
    if (prev) {
        if (next) {
            prev->file_next = next;
            next->file_prev = prev;
        }
        else {
            prev->file_next = NULL;
            file->request_list_tail = prev;
        }
    }
    else {
        if (next) {
            next->file_prev = NULL;
            file->request_list_head = next;
        }
        else {
            file->request_list_head = NULL;
            file->request_list_tail = NULL;
        }
    }

    if(request == request->obj->request)
        request->obj->request = NULL;
    request->file_prev = NULL;
    request->file_next = NULL;

    file->num_req --;

    FUNC_LEAVE_NOAPI(SUCCEED)
}


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_request_wait
 *
 * Purpose: 
 *    Waits for a particular request to complete. This will test
 *    the request completion using Mercury's test routine. If the
 *    request is still pending we test for completion of other requests in
 *    the file's linked list to try and keep making progress. Once the
 *    original requests completes, we remove it from the linked list 
 *    and return.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_request_wait(H5VL_iod_file_t *file, H5VL_iod_request_t *request)
{
    H5VL_iod_request_t *cur_req = file->request_list_head;
    int ret;
    hg_status_t status;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    HDassert(request);
    HDassert(request->req);

    /* Loop to complete the request while poking through other requests on the 
       container to avoid deadlock. */
    while(1) {
        HDassert(request->state == H5VL_IOD_PENDING);
        /* test the operation status */
        ret = HG_Wait(*((hg_request_t *)request->req), 0, &status);
        if(HG_FAIL == ret) {
            fprintf(stderr, "failed to wait on request\n");
            request->status = H5AO_FAILED;
            request->state = H5VL_IOD_COMPLETED;
            H5VL_iod_request_delete(file, request);
            break;
        }
        else {
            if(status) {
                request->status = H5AO_SUCCEEDED;
                request->state = H5VL_IOD_COMPLETED;
            }
        }

        /* if it has not completed, go through the list of requests on the container to
           test progress */
        if(!status) {
            H5VL_iod_request_t *tmp_req = NULL;

            if(cur_req) {
                if(HG_FILE_CLOSE != cur_req->type && cur_req->req != request->req) {
                    hg_status_t tmp_status;

                    tmp_req = cur_req->file_next;

                    HDassert(cur_req->state == H5VL_IOD_PENDING);
                    ret = HG_Wait(*((hg_request_t *)cur_req->req), 0, &tmp_status);
                    if(HG_FAIL == ret) {
                        fprintf(stderr, "failed to wait on request\n");
                        cur_req->status = H5AO_FAILED;
                        cur_req->state = H5VL_IOD_COMPLETED;
                        H5VL_iod_request_delete(file, cur_req);
                    }
                    else {
                        if(tmp_status) {
                            cur_req->status = H5AO_SUCCEEDED;
                            cur_req->state = H5VL_IOD_COMPLETED;
                            if(H5VL_iod_request_complete(file, cur_req) < 0) {
                                fprintf(stderr, "Operation Failed!\n");
                            }
                        }
                    }
                }
                /* next time, test the next request in the list */
                cur_req = tmp_req;
            }
        }
        /* request complete, remove it from list & break */
        else {
            if(H5VL_iod_request_complete(file, request) < 0) {
                fprintf(stderr, "Operation Failed!\n");
            }
            break;
        }
    }

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5VL_iod_wait */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_request_wait_all
 *
 * Purpose:     Wait and complete all the requests in the linked list.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_request_wait_all(H5VL_iod_file_t *file)
{
    H5VL_iod_request_t *cur_req = file->request_list_head;
    hg_status_t status;
    int ret;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    /* Loop to complete all requests */
    while(cur_req) {
        H5VL_iod_request_t *tmp_req = NULL;

        tmp_req = cur_req->file_next;

        HDassert(cur_req->state == H5VL_IOD_PENDING);
        ret = HG_Wait(*((hg_request_t *)cur_req->req), HG_MAX_IDLE_TIME, &status);
        if(HG_FAIL == ret) {
            fprintf(stderr, "failed to wait on request\n");
            cur_req->status = H5AO_FAILED;
            cur_req->state = H5VL_IOD_COMPLETED;
        }
        else {
            if(!status) {
                fprintf(stderr, "Wait timeout reached\n");
                cur_req->status = H5AO_FAILED;
                cur_req->state = H5VL_IOD_COMPLETED;
                H5VL_iod_request_delete(file, cur_req);
                goto done;
            }
            else {
                cur_req->status = H5AO_SUCCEEDED;
                cur_req->state = H5VL_IOD_COMPLETED;
            }
        }

        if(H5VL_iod_request_complete(file, cur_req) < 0)
            fprintf(stderr, "Operation Failed!\n");

        cur_req = tmp_req;
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_request_wait_all */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_request_wait_some
 *
 * Purpose:     Wait for some requests on the linked list, particularly 
 *              the ones that are tracked with a particular object.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_request_wait_some(H5VL_iod_file_t *file, const void *object)
{
    H5VL_iod_request_t *cur_req = file->request_list_head;
    hg_status_t status;
    int ret;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    /* Loop to complete some requests */
    while(cur_req) {
        H5VL_iod_request_t *tmp_req;

        tmp_req = cur_req->file_next;

        /* If the request is pending on the object we want, complete it */
        if(cur_req->obj == object) {
            HDassert(cur_req->state == H5VL_IOD_PENDING);
            ret = HG_Wait(*((hg_request_t *)cur_req->req), HG_MAX_IDLE_TIME, 
                          &status);
            if(HG_FAIL == ret) {
                fprintf(stderr, "failed to wait on request\n");
                cur_req->status = H5AO_FAILED;
                cur_req->state = H5VL_IOD_COMPLETED;
                H5VL_iod_request_delete(file, cur_req);
            }
            else {
                if(!status) {
                    fprintf(stderr, "Wait timeout reached\n");
                    cur_req->status = H5AO_FAILED;
                    cur_req->state = H5VL_IOD_COMPLETED;
                    H5VL_iod_request_delete(file, cur_req);
                }
                else {
                    cur_req->status = H5AO_SUCCEEDED;
                    cur_req->state = H5VL_IOD_COMPLETED;
                    if(H5VL_iod_request_complete(file, cur_req) < 0)
                        fprintf(stderr, "Operation Failed!\n");
                }
            }
        }
        cur_req = tmp_req;
    }

    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_request_wait_some */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_request_complete
 *
 * Purpose:     Completion calls for every type of request. This checks 
 *              the return status from the server, and frees memory 
 *              allocated by this request.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_request_complete(H5VL_iod_file_t *file, H5VL_iod_request_t *req)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert(req->state == H5VL_IOD_COMPLETED);

    switch(req->type) {
    case HG_FILE_CREATE:
    case HG_FILE_OPEN:
        if(IOD_OH_UNDEFINED == req->obj->file->remote_file.coh.cookie) {
            fprintf(stderr, "failed to create/open file\n");
            req->status = H5AO_FAILED;
            req->state = H5VL_IOD_COMPLETED;
        }

        H5VL_iod_request_delete(file, req);
        break;
    case HG_ATTR_CREATE:
    case HG_ATTR_OPEN:
        {
            H5VL_iod_attr_t *attr = (H5VL_iod_attr_t *)req->obj;

            if(IOD_OH_UNDEFINED == attr->remote_attr.iod_oh.cookie) {
                fprintf(stderr, "failed to create/open Attribute\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_GROUP_CREATE:
    case HG_GROUP_OPEN:
        {
            H5VL_iod_group_t *group = (H5VL_iod_group_t *)req->obj;

            if(IOD_OH_UNDEFINED == group->remote_group.iod_oh.cookie) {
                fprintf(stderr, "failed to create/open Group\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_MAP_CREATE:
    case HG_MAP_OPEN:
        {
            H5VL_iod_map_t *map = (H5VL_iod_map_t *)req->obj;

            if(IOD_OH_UNDEFINED == map->remote_map.iod_oh.cookie) {
                fprintf(stderr, "failed to create/open Map\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_DSET_CREATE:
    case HG_DSET_OPEN:
        {
            H5VL_iod_dset_t *dset = (H5VL_iod_dset_t *)req->obj;

            if(IOD_OH_UNDEFINED == dset->remote_dset.iod_oh.cookie) {
                fprintf(stderr, "failed to create/open Dataset\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_DTYPE_COMMIT:
    case HG_DTYPE_OPEN:
        {
            H5VL_iod_dtype_t *dtype = (H5VL_iod_dtype_t *)req->obj;

            if(IOD_OH_UNDEFINED == dtype->remote_dtype.iod_oh.cookie) {
                fprintf(stderr, "failed to create/open Attribute\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_DSET_WRITE:
    case HG_DSET_READ:
        {
            H5VL_iod_io_info_t *info = (H5VL_iod_io_info_t *)req->data;

            /* Free memory handle */
            if(HG_SUCCESS != HG_Bulk_handle_free(*info->bulk_handle)) {
                fprintf(stderr, "failed to free dataset bulk handle\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }
            if(HG_DSET_WRITE == req->type && SUCCEED != *((int *)info->status)) {
                fprintf(stderr, "Errrr! Dataset Write Failure Reported from Server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }
            else if(HG_DSET_READ == req->type) {
                H5VL_iod_read_status_t *read_status = (H5VL_iod_read_status_t *)info->status;

                if(SUCCEED != read_status->ret) {
                    fprintf(stderr, "Errrrr!  Dataset Read Failure Reported from Server\n");
                    req->status = H5AO_FAILED;
                    req->state = H5VL_IOD_COMPLETED;
                }
                else {
                    uint32_t internal_cs = 0;

                    /* calculate a checksum for the data recieved */
                    internal_cs = H5S_checksum(info->buf_ptr, info->type_size, 
                                               info->nelmts, info->space);

                    /* verify data integrity */
                    if(internal_cs != read_status->cs) {
                        fprintf(stderr, "Errrrr!  Dataset Read integrity failure (expecting %u got %u).\n",
                                read_status->cs, internal_cs);
                        req->status = H5AO_FAILED;
                        req->state = H5VL_IOD_COMPLETED;
                    }
                    if(info->space && H5S_close(info->space) < 0)
                        HDONE_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to release dataspace");

                    /* If the app gave us a buffer to store the checksum, then put it there */
                    if(info->cs_ptr)
                        *info->cs_ptr = internal_cs;
                }
            }

            free(info->status);
            info->status = NULL;
            info->bulk_handle = (hg_bulk_t *)H5MM_xfree(info->bulk_handle);

            if(info->vl_string_len)
                free(info->vl_string_len);

            info = (H5VL_iod_io_info_t *)H5MM_xfree(info);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_DSET_GET_VL_SIZE:
        {
            H5VL_iod_io_info_t *info = (H5VL_iod_io_info_t *)req->data;
            H5VL_iod_read_status_t *status = (H5VL_iod_read_status_t *)info->status;

            if(SUCCEED != status->ret) {
                fprintf(stderr, "Errrrr!  Dataset Read Failure Reported from Server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }
            else {
                dset_io_in_t input;
                void *read_buf;
                H5VL_iod_dset_t *dset = (H5VL_iod_dset_t *)req->obj;
                uint32_t internal_cs = 0;
                size_t buf_size = status->buf_size;
                hid_t rcxt_id;
                H5RC_t *rc;
                H5P_genplist_t *plist = NULL;
                H5VL_iod_read_status_t vl_status;

                if(NULL == (read_buf = (void *)HDmalloc(buf_size)))
                    HGOTO_ERROR(H5E_DATASET, H5E_NOSPACE, FAIL, "can't allocate VL recieve buffer");

                /* Register memory with bulk_handle */
                if(HG_SUCCESS != HG_Bulk_handle_create(read_buf, buf_size, 
                                                       HG_BULK_READWRITE, info->bulk_handle))
                    HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't create Bulk Data Handle");

                /* get the context ID */
                if(NULL == (plist = (H5P_genplist_t *)H5I_object(info->dxpl_id)))
                    HGOTO_ERROR(H5E_ATOM, H5E_BADATOM, FAIL, "can't find object for ID");
                if(H5P_get(plist, H5VL_CONTEXT_ID, &rcxt_id) < 0)
                    HGOTO_ERROR(H5E_PLIST, H5E_CANTGET, FAIL, "can't get property value for trans_id");

                /* get the RC object */
                if(NULL == (rc = (H5RC_t *)H5I_object_verify(rcxt_id, H5I_RC)))
                    HGOTO_ERROR(H5E_ARGS, H5E_BADVALUE, FAIL, "not a READ CONTEXT ID")

                /* Fill input structure for reading data */
                input.coh = file->remote_file.coh;
                input.iod_oh = dset->remote_dset.iod_oh;
                input.iod_id = dset->remote_dset.iod_id;
                input.bulk_handle = *info->bulk_handle;
                input.checksum = 0;
                input.dxpl_id = info->dxpl_id;
                input.space_id = info->file_space_id;
                input.mem_type_id = info->mem_type_id;
                input.dset_type_id = dset->remote_dset.type_id;

                if(H5VL__iod_create_and_forward(info->read_id, HG_DSET_READ, 
                                                (H5VL_iod_object_t *)dset, 0, 0, NULL,
                                                (H5VL_iod_req_info_t *)rc,
                                                &input, &vl_status, NULL, NULL) < 0)
                    HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "failed to create and ship dataset read");

                /* Free memory handle */
                if(HG_SUCCESS != HG_Bulk_handle_free(*info->bulk_handle)) {
                    fprintf(stderr, "failed to free dataset bulk handle\n");
                    req->status = H5AO_FAILED;
                    req->state = H5VL_IOD_COMPLETED;
                }

                if(SUCCEED != vl_status.ret) {
                    fprintf(stderr, "Errrrr!  Dataset Read Failure Reported from Server\n");
                    req->status = H5AO_FAILED;
                    req->state = H5VL_IOD_COMPLETED;
                }

                /* calculate a checksum for the data recieved */
                internal_cs = H5_checksum_lookup4(read_buf, buf_size, NULL);

                /* scatter the data into the user's buffer */
                if(H5VL__iod_vl_read_finalize(buf_size, read_buf, (void *)info->buf_ptr, 
                                              info->space, info->mem_type_id, 
                                              dset->remote_dset.type_id) < 0)
                    HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "failed to store VL data in user buffer");

                HDfree(read_buf);

                /* verify data integrity */
                if(internal_cs != vl_status.cs) {
                    fprintf(stderr, "Errrrr!  Dataset Read integrity failure (expecting %u got %u).\n",
                            internal_cs, status->cs);
                    req->status = H5AO_FAILED;
                    req->state = H5VL_IOD_COMPLETED;
                }

                /* If the app gave us a buffer to store the checksum, then put it there */
                if(info->cs_ptr)
                    *info->cs_ptr = internal_cs;
            }

            if(info->space && H5S_close(info->space) < 0)
                HGOTO_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to release dataspace");
            if(H5Sclose(info->file_space_id) < 0)
                HGOTO_ERROR(H5E_DATASPACE, H5E_CANTRELEASE, FAIL, "unable to release dataspace");
            if(H5Tclose(info->mem_type_id) < 0)
                HGOTO_ERROR(H5E_DATATYPE, H5E_CANTRELEASE, FAIL, "unable to release datatype");
            if(H5Pclose(info->dxpl_id) < 0)
                HGOTO_ERROR(H5E_PLIST, H5E_CANTRELEASE, FAIL, "unable to release plist");

            free(info->status);
            info->status = NULL;
            info->bulk_handle = (hg_bulk_t *)H5MM_xfree(info->bulk_handle);
            info = (H5VL_iod_io_info_t *)H5MM_xfree(info);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_ATTR_WRITE:
    case HG_ATTR_READ:
        {
            H5VL_iod_io_info_t *info = (H5VL_iod_io_info_t *)req->data;

            /* Free memory handle */
            if(HG_SUCCESS != HG_Bulk_handle_free(*info->bulk_handle)) {
                fprintf(stderr, "failed to free attribute bulk handle\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }
            if(SUCCEED != *((int *)info->status)) {
                fprintf(stderr, "Attribute I/O Failure Reported from Server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(info->status);
            info->status = NULL;
            info->bulk_handle = (hg_bulk_t *)H5MM_xfree(info->bulk_handle);
            info = (H5VL_iod_io_info_t *)H5MM_xfree(info);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_MAP_SET:
        {
            int *status = (int *)req->data;

            if(SUCCEED != *status) {
                fprintf(stderr, "MAP set failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_MAP_DELETE:
        {
            int *status = (int *)req->data;

            if(SUCCEED != *status) {
                fprintf(stderr, "MAP delete failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_MAP_GET:
        {
            map_get_out_t *output = (map_get_out_t *)req->data;

            if(SUCCEED != output->ret) {
                fprintf(stderr, "MAP get failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(output);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_MAP_GET_COUNT:
        {
            hsize_t *count = (hsize_t *)req->data;

            if(*count == IOD_COUNT_UNDEFINED) {
                fprintf(stderr, "MAP get_count failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_MAP_EXISTS:
        {
            htri_t *exists = (htri_t *)req->data;

            if(*exists < 0) {
                fprintf(stderr, "MAP exists failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_FILE_CLOSE:
        {
            int *status = (int *)req->data;

            if(SUCCEED != *status) {
                fprintf(stderr, "FILE close failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            file->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free everything */
            free(file->file_name);
            free(file->common.obj_name);
            if(file->common.comment)
                HDfree(file->common.comment);
            if(file->fapl_id != H5P_FILE_ACCESS_DEFAULT && H5Pclose(file->fapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(file->remote_file.fcpl_id != H5P_FILE_CREATE_DEFAULT && 
               H5Pclose(file->remote_file.fcpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            file = H5FL_FREE(H5VL_iod_file_t, file);
            break;
        }
    case HG_ATTR_RENAME:
        {
            int *status = (int *)req->data;
            H5VL_iod_object_t *obj = (H5VL_iod_object_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "ATTR rename failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            obj->request = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_ATTR_REMOVE:
        {
            int *status = (int *)req->data;
            H5VL_iod_object_t *obj = (H5VL_iod_object_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "ATTR remove failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            obj->request = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_ATTR_EXISTS:
    case HG_LINK_EXISTS:
    case HG_OBJECT_EXISTS:
        {
            htri_t *ret = (htri_t *)req->data;
            H5VL_iod_object_t *obj = (H5VL_iod_object_t *)req->obj;

            if(*ret < 0) {
                fprintf(stderr, "EXIST OP failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            req->data = NULL;
            obj->request = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_ATTR_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_attr_t *attr = (H5VL_iod_attr_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "ATTR close failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            attr->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free attr components */
            if(attr->common.obj_name)
                free(attr->common.obj_name);
            if(attr->loc_name)
                free(attr->loc_name);
            if(attr->common.comment)
                HDfree(attr->common.comment);
            if(attr->remote_attr.acpl_id != H5P_ATTRIBUTE_CREATE_DEFAULT &&
               H5Pclose(attr->remote_attr.acpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(H5Tclose(attr->remote_attr.type_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            if(H5Sclose(attr->remote_attr.space_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dspace");
            attr = H5FL_FREE(H5VL_iod_attr_t, attr);
            break;
        }
    case HG_GROUP_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_group_t *grp = (H5VL_iod_group_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "GROUP CLOSE failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            grp->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free group components */
            if(grp->common.obj_name)
                free(grp->common.obj_name);
            if(grp->common.comment)
                HDfree(grp->common.comment);
            if(grp->gapl_id != H5P_GROUP_ACCESS_DEFAULT && H5Pclose(grp->gapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(grp->remote_group.gcpl_id != H5P_GROUP_CREATE_DEFAULT && 
               H5Pclose(grp->remote_group.gcpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            grp = H5FL_FREE(H5VL_iod_group_t, grp);
            break;
        }
    case HG_DSET_SET_EXTENT:
        {
            int *status = (int *)req->data;
            H5VL_iod_dset_t *dset = (H5VL_iod_dset_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "DATASET set extent failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            dset->common.request = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_DSET_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_dset_t *dset = (H5VL_iod_dset_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "DATASET CLOSE failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            dset->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free dset components */
            if(dset->common.obj_name)
                free(dset->common.obj_name);
            if(dset->common.comment)
                HDfree(dset->common.comment);
            if(dset->remote_dset.dcpl_id != H5P_DATASET_CREATE_DEFAULT &&
               H5Pclose(dset->remote_dset.dcpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(dset->dapl_id != H5P_DATASET_ACCESS_DEFAULT &&
               H5Pclose(dset->dapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(H5Tclose(dset->remote_dset.type_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            if(H5Sclose(dset->remote_dset.space_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dspace");
            dset = H5FL_FREE(H5VL_iod_dset_t, dset);
            break;
        }
    case HG_MAP_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_map_t *map = (H5VL_iod_map_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "MAP close failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            map->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free map components */
            if(map->common.obj_name)
                free(map->common.obj_name);
            if(map->common.comment)
                HDfree(map->common.comment);
            if(H5Tclose(map->remote_map.keytype_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            if(H5Tclose(map->remote_map.valtype_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            map = H5FL_FREE(H5VL_iod_map_t, map);
            break;
        }
    case HG_DTYPE_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_dtype_t *dtype = (H5VL_iod_dtype_t *)req->obj;

            if(SUCCEED != *status) {
                fprintf(stderr, "DATATYPE delete failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            dtype->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free dtype components */
            if(dtype->common.obj_name)
                free(dtype->common.obj_name);
            if(dtype->common.comment)
                HDfree(dtype->common.comment);
            if(dtype->remote_dtype.tcpl_id != H5P_DATATYPE_CREATE_DEFAULT &&
               H5Pclose(dtype->remote_dtype.tcpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(dtype->tapl_id != H5P_DATATYPE_ACCESS_DEFAULT &&
               H5Pclose(dtype->tapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(H5Tclose(dtype->remote_dtype.type_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            dtype = H5FL_FREE(H5VL_iod_dtype_t, dtype);
            break;
        }
    case HG_LINK_CREATE:
    case HG_LINK_MOVE:
    case HG_LINK_REMOVE:
    case HG_OBJECT_SET_COMMENT:
    case HG_OBJECT_COPY:
        {
            int *status = (int *)req->data;

            if(SUCCEED != *status) {
                fprintf(stderr, "Link operation failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_LINK_GET_INFO:
        {
            H5L_ff_info_t *linfo = (H5L_ff_info_t *)req->data;

            if(linfo->type == H5L_TYPE_ERROR) {
                fprintf(stderr, "Link get_info failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_OBJECT_GET_INFO:
        {
            H5O_ff_info_t *oinfo = (H5O_ff_info_t *)req->data;

            if(oinfo->type == H5O_TYPE_UNKNOWN) {
                fprintf(stderr, "OBJECT get_info failed at the server\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_LINK_GET_VAL:
        {
            link_get_val_out_t *result = (link_get_val_out_t *)req->data;

            if(SUCCEED != result->ret) {
                fprintf(stderr, "get comment failed\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(result);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_OBJECT_OPEN:
        req->data = NULL;
        H5VL_iod_request_delete(file, req);
        break;
    case HG_OBJECT_GET_COMMENT:
        {
            object_get_comment_out_t *result = (object_get_comment_out_t *)req->data;

            if(SUCCEED != result->ret) {
                fprintf(stderr, "get comment failed\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(result);
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_RC_ACQUIRE:
        {
            H5VL_iod_rc_info_t *rc_info = (H5VL_iod_rc_info_t *)req->data;

            if(SUCCEED != rc_info->result.ret) {
                fprintf(stderr, "Failed to Acquire Read Context %llu\n", *(rc_info->c_version_ptr));
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            rc_info->read_cxt->c_version = rc_info->result.c_version;
            *rc_info->c_version_ptr = rc_info->result.c_version;
            rc_info = (H5VL_iod_rc_info_t *)H5MM_xfree(rc_info);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_RC_RELEASE:
    case HG_RC_PERSIST:
    case HG_RC_SNAPSHOT:
        {
            int *status = (int *)req->data;

            if(SUCCEED != *status) {
                fprintf(stderr, "Failed to Read Context OP\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_TR_START:
    case HG_TR_FINISH:
    case HG_TR_SET_DEPEND:
    case HG_TR_SKIP:
    case HG_TR_ABORT:
        {
            int *status = (int *)req->data;

            if(SUCCEED != *status) {
                fprintf(stderr, "Failed transaction OP\n");
                req->status = H5AO_FAILED;
                req->state = H5VL_IOD_COMPLETED;
            }

            free(status);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_LINK_ITERATE:
    case HG_OBJECT_VISIT:
    case HG_MAP_ITERATE:
    default:
        req->status = H5AO_FAILED;
        req->state = H5VL_IOD_COMPLETED;
        req->data = NULL;
        H5VL_iod_request_delete(file, req);
        HGOTO_ERROR(H5E_SYM, H5E_CANTFREE, FAIL, "Request Type not supported");
    }
done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_request_complete */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_request_cancel
 *
 * Purpose:     Cancels a particular request by freeing memory 
 *              associated with it.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_request_cancel(H5VL_iod_file_t *file, H5VL_iod_request_t *req)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    HDassert(req->state == H5VL_IOD_CANCELLED);

    switch(req->type) {
    case HG_DSET_WRITE:
    case HG_DSET_READ:
    case HG_ATTR_WRITE:
    case HG_ATTR_READ:
    case HG_DSET_GET_VL_SIZE:
        {
            H5VL_iod_io_info_t *info = (H5VL_iod_io_info_t *)req->data;

            /* Free memory handle */
            if(HG_SUCCESS != HG_Bulk_handle_free(*info->bulk_handle)) {
                fprintf(stderr, "failed to free bulk handle\n");
            }
            free(info->status);
            info->status = NULL;
            info->bulk_handle = (hg_bulk_t *)H5MM_xfree(info->bulk_handle);
            if(info->vl_string_len)
                free(info->vl_string_len);
            info = (H5VL_iod_io_info_t *)H5MM_xfree(info);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_FILE_CREATE:
    case HG_FILE_OPEN:
    case HG_FILE_CLOSE:
        {
            int *status = (int *)req->data;

            free(status);
            req->data = NULL;
            file->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free everything */
            free(file->file_name);
            free(file->common.obj_name);
            if(file->common.comment)
                HDfree(file->common.comment);
            if(file->fapl_id != H5P_FILE_ACCESS_DEFAULT && H5Pclose(file->fapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(file->remote_file.fcpl_id != 0 && 
               file->remote_file.fcpl_id != H5P_FILE_CREATE_DEFAULT && 
               H5Pclose(file->remote_file.fcpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            file = H5FL_FREE(H5VL_iod_file_t, file);
            break;
        }
    case HG_ATTR_REMOVE:
    case HG_ATTR_RENAME:
        {
            int *status = (int *)req->data;
            H5VL_iod_object_t *obj = (H5VL_iod_object_t *)req->obj;

            free(status);
            req->data = NULL;
            obj->request = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_ATTR_EXISTS:
    case HG_LINK_EXISTS:
    case HG_OBJECT_EXISTS:
    case HG_MAP_GET_COUNT:
    case HG_MAP_EXISTS:
        {
            H5VL_iod_object_t *obj = (H5VL_iod_object_t *)req->obj;

            req->data = NULL;
            obj->request = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_ATTR_CREATE:
    case HG_ATTR_OPEN:
    case HG_ATTR_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_attr_t *attr = (H5VL_iod_attr_t *)req->obj;

            free(status);
            req->data = NULL;
            attr->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free attr components */
            if(attr->common.obj_name)
                free(attr->common.obj_name);
            if(attr->loc_name)
                free(attr->loc_name);
            if(attr->common.comment)
                HDfree(attr->common.comment);
            if(attr->remote_attr.acpl_id != 0 &&
               attr->remote_attr.acpl_id != H5P_ATTRIBUTE_CREATE_DEFAULT &&
               H5Pclose(attr->remote_attr.acpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(attr->remote_attr.type_id != 0 &&
               H5Tclose(attr->remote_attr.type_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            if(attr->remote_attr.space_id != 0 &&
               H5Sclose(attr->remote_attr.space_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dspace");
            attr = H5FL_FREE(H5VL_iod_attr_t, attr);
            break;
        }
    case HG_GROUP_CREATE:
    case HG_GROUP_OPEN:
    case HG_GROUP_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_group_t *grp = (H5VL_iod_group_t *)req->obj;

            free(status);
            req->data = NULL;
            grp->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free group components */
            if(grp->common.obj_name)
                free(grp->common.obj_name);
            if(grp->common.comment)
                HDfree(grp->common.comment);
            if(grp->gapl_id != H5P_GROUP_ACCESS_DEFAULT && H5Pclose(grp->gapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(grp->remote_group.gcpl_id != 0 &&
               grp->remote_group.gcpl_id != H5P_GROUP_CREATE_DEFAULT && 
               H5Pclose(grp->remote_group.gcpl_id) < 0) {
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            }
            grp = H5FL_FREE(H5VL_iod_group_t, grp);
            break;
        }
    case HG_DSET_SET_EXTENT:
        {
            int *status = (int *)req->data;
            H5VL_iod_dset_t *dset = (H5VL_iod_dset_t *)req->obj;

            free(status);
            req->data = NULL;
            dset->common.request = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_DSET_CREATE:
    case HG_DSET_OPEN:
    case HG_DSET_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_dset_t *dset = (H5VL_iod_dset_t *)req->obj;

            free(status);
            req->data = NULL;
            dset->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free dset components */
            if(dset->common.obj_name)
                free(dset->common.obj_name);
            if(dset->common.comment)
                HDfree(dset->common.comment);
            if(dset->remote_dset.dcpl_id != 0 &&
               dset->remote_dset.dcpl_id != H5P_DATASET_CREATE_DEFAULT &&
               H5Pclose(dset->remote_dset.dcpl_id) < 0) {
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            }
            if(dset->dapl_id != H5P_DATASET_ACCESS_DEFAULT &&
               H5Pclose(dset->dapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(dset->remote_dset.type_id != 0 &&
               H5Tclose(dset->remote_dset.type_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            if(dset->remote_dset.space_id != 0 &&
               H5Sclose(dset->remote_dset.space_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dspace");
            dset = H5FL_FREE(H5VL_iod_dset_t, dset);
            break;
        }
    case HG_MAP_CREATE:
    case HG_MAP_OPEN:
    case HG_MAP_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_map_t *map = (H5VL_iod_map_t *)req->obj;

            free(status);
            req->data = NULL;
            map->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free map components */
            if(map->common.obj_name)
                free(map->common.obj_name);
            if(map->common.comment)
                HDfree(map->common.comment);
            if(H5Tclose(map->remote_map.keytype_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            if(H5Tclose(map->remote_map.valtype_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            map = H5FL_FREE(H5VL_iod_map_t, map);
            break;
        }
    case HG_DTYPE_COMMIT:
    case HG_DTYPE_OPEN:
    case HG_DTYPE_CLOSE:
        {
            int *status = (int *)req->data;
            H5VL_iod_dtype_t *dtype = (H5VL_iod_dtype_t *)req->obj;

            free(status);
            req->data = NULL;
            dtype->common.request = NULL;
            H5VL_iod_request_delete(file, req);

            /* free dtype components */
            if(dtype->common.obj_name)
                free(dtype->common.obj_name);
            if(dtype->common.comment)
                HDfree(dtype->common.comment);
            if(dtype->remote_dtype.tcpl_id != 0 &&
               dtype->remote_dtype.tcpl_id != H5P_DATATYPE_CREATE_DEFAULT &&
               H5Pclose(dtype->remote_dtype.tcpl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(dtype->tapl_id != H5P_DATATYPE_ACCESS_DEFAULT &&
               H5Pclose(dtype->tapl_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close plist");
            if(dtype->remote_dtype.type_id != 0 && 
               H5Tclose(dtype->remote_dtype.type_id) < 0)
                HGOTO_ERROR(H5E_SYM, H5E_CANTDEC, FAIL, "failed to close dtype");
            dtype = H5FL_FREE(H5VL_iod_dtype_t, dtype);
            break;
        }

    case HG_MAP_GET:
        {
            map_get_out_t *output = (map_get_out_t *)req->data;

            free(output);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_MAP_SET:
    case HG_MAP_DELETE:
    case HG_LINK_CREATE:
    case HG_LINK_MOVE:
    case HG_LINK_REMOVE:
    case HG_OBJECT_SET_COMMENT:
    case HG_OBJECT_COPY:
        {
            int *status = (int *)req->data;

            free(status);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_OBJECT_GET_COMMENT:
        {
            object_get_comment_out_t *result = (object_get_comment_out_t *)req->data;

            free(result);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_OBJECT_OPEN:
    case HG_LINK_GET_INFO:
    case HG_OBJECT_GET_INFO:
        req->data = NULL;
        H5VL_iod_request_delete(file, req);
        break;
    case HG_LINK_GET_VAL:
        {
            link_get_val_out_t *result = (link_get_val_out_t *)req->data;

            free(result);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_RC_ACQUIRE:
        {
            H5VL_iod_rc_info_t *rc_info = (H5VL_iod_rc_info_t *)req->data;

            rc_info = (H5VL_iod_rc_info_t *)H5MM_xfree(rc_info);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_RC_RELEASE:
    case HG_RC_PERSIST:
    case HG_RC_SNAPSHOT:
    case HG_TR_START:
    case HG_TR_FINISH:
    case HG_TR_SET_DEPEND:
    case HG_TR_SKIP:
    case HG_TR_ABORT:
        {
            int *status = (int *)req->data;

            free(status);
            req->data = NULL;
            H5VL_iod_request_delete(file, req);
            break;
        }
    case HG_LINK_ITERATE:
    case HG_OBJECT_VISIT:
    case HG_MAP_ITERATE:
    default:
        H5VL_iod_request_delete(file, req);
        HGOTO_ERROR(H5E_SYM, H5E_CANTFREE, FAIL, "Request Type not supported");
    }
done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_request_cancel */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_get_obj_requests
 *
 * Purpose:     returns the number of requests that are associated 
 *              with a particular object. If the parent array is not NULL, 
 *              the request pointers are stored.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_get_obj_requests(H5VL_iod_object_t *obj, /*IN/OUT*/ size_t *count, 
                          /*OUT*/ H5VL_iod_request_t **parent_reqs)
{
    H5VL_iod_file_t *file = obj->file;
    H5VL_iod_request_t *cur_req = file->request_list_head;
    size_t size = 0;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    while(cur_req) {
        /* If the request is pending on the object we want, add its axe_id */
        if(cur_req->obj == obj) {
            if(cur_req->status == H5AO_PENDING) {
                if(NULL != parent_reqs) {
                    parent_reqs[size] = cur_req;
                    cur_req->ref_count ++;
                }
                size ++;
            }
        }
        cur_req = cur_req->file_next;
    }

    *count = size;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5VL_iod_get_obj_requests */

herr_t
H5VL_iod_get_loc_info(H5VL_iod_object_t *obj, iod_obj_id_t *iod_id, 
                      iod_handle_t *iod_oh)
{
    iod_obj_id_t id;
    iod_handle_t oh;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    switch(obj->obj_type) {
        case H5I_FILE:
            oh = obj->file->remote_file.root_oh;
            id = obj->file->remote_file.root_id;
            break;
        case H5I_GROUP:
            oh = ((const H5VL_iod_group_t *)obj)->remote_group.iod_oh;
            id = ((const H5VL_iod_group_t *)obj)->remote_group.iod_id;
            break;
        case H5I_DATASET:
            oh = ((const H5VL_iod_dset_t *)obj)->remote_dset.iod_oh;
            id = ((const H5VL_iod_dset_t *)obj)->remote_dset.iod_id;
            break;
        case H5I_DATATYPE:
            oh = ((const H5VL_iod_dtype_t *)obj)->remote_dtype.iod_oh;
            id = ((const H5VL_iod_dtype_t *)obj)->remote_dtype.iod_id;
            break;
        case H5I_MAP:
            oh = ((const H5VL_iod_map_t *)obj)->remote_map.iod_oh;
            id = ((const H5VL_iod_map_t *)obj)->remote_map.iod_id;
            break;
        default:
            HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "bad location object");
    }

    *iod_id = id;
    *iod_oh = oh;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_get_loc_info() */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_get_parent_requests
 *
 * Purpose:     Returns the parent requests associated with an object 
 *              and transaction.
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_get_parent_requests(H5VL_iod_object_t *obj, H5VL_iod_req_info_t *req_info, 
                             H5VL_iod_request_t **parent_reqs, size_t *num_parents)
{
    size_t count = 0;

    FUNC_ENTER_NOAPI_NOINIT_NOERR

    if(obj && obj->request && obj->request->status == H5AO_PENDING) {
        parent_reqs[count] = obj->request;
        obj->request->ref_count ++;
        count ++;
    }

    if(req_info && req_info->request && req_info->request->status == H5AO_PENDING) {
        parent_reqs[count] = req_info->request;
        req_info->request->ref_count ++;
        count ++;
    }

    *num_parents += count;

    FUNC_LEAVE_NOAPI(SUCCEED)
} /* end H5VL_iod_get_parent_requests */

herr_t
H5VL_iod_gen_obj_id(int myrank, int nranks, uint64_t cur_index, 
                    iod_obj_type_t type, uint64_t *id)
{
    herr_t ret_value = SUCCEED;
    uint64_t tmp_id;

    FUNC_ENTER_NOAPI_NOINIT

    /* determine first the rank of the object with the first 59
       bits */
    tmp_id = myrank + (nranks * cur_index);

    /* toggle the object type bits */
    switch(type) {
    case IOD_OBJ_ARRAY:
        tmp_id |= IOD_OBJ_TYPE_ARRAY;
        break;
    case IOD_OBJ_KV:
        tmp_id |= IOD_OBJ_TYPE_KV;
        break;
    case IOD_OBJ_BLOB:
        tmp_id |= IOD_OBJ_TYPE_BLOB;
        break;
    case IOD_OBJ_ANY:
    default:
        HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "bad object type");
    }

    /* toggle the owner bit */
    tmp_id |= IOD_OBJID_APP;

    *id = tmp_id;
done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_gen_obj_id */


/*-------------------------------------------------------------------------
 * Function:    H5VL__iod_pre_write_cb
 *
 * The callback to the H5Diterate routine called in
 * H5VL__iod_pre_write. This will generate the offset,length pair for
 * the serialzed form of the VL data.
 *
 * Return:	Success:	SUCCEED 
 *		Failure:	Negative
 *
 * Programmer:  Mohamad Chaarawi
 *              August, 2013
 *
 *-------------------------------------------------------------------------
 */
static herr_t 
H5VL__iod_pre_write_cb(void UNUSED *elem, hid_t type_id, unsigned UNUSED ndim, 
                       const hsize_t UNUSED *point, void *_udata)
{
    H5VL_iod_pre_write_t *udata = (H5VL_iod_pre_write_t *)_udata;
    H5T_t *dt = NULL;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    /* Check args */
    if(NULL == (dt = (H5T_t *)H5I_object_verify(type_id, H5I_DATATYPE)))
	HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, H5T_NO_CLASS, "not a datatype")

    switch(H5T_get_class(dt, FALSE)) {
        case H5T_INTEGER:
        case H5T_FLOAT:
        case H5T_TIME:
        case H5T_BITFIELD:
        case H5T_OPAQUE:
        case H5T_ENUM:
        case H5T_ARRAY:
        case H5T_NO_CLASS:
        case H5T_REFERENCE:
        case H5T_NCLASSES:
        case H5T_COMPOUND:
            HDassert(0 && fprintf(stderr, "Should not be here \n"));
            break;

        case H5T_STRING:
            {
                char **buf;
                int i = udata->curr_seq/2;

                HDassert(H5T_is_variable_str(dt));

                buf = (char **)udata->buf_ptr;

                udata->str_len[i] = HDstrlen(buf[i]) + 1;
                udata->buf_size += udata->str_len[i] + sizeof(size_t);

                udata->off[udata->curr_seq] = (uint8_t *)(udata->str_len+i);
                udata->len[udata->curr_seq] = sizeof(size_t);
                udata->curr_seq ++;

                udata->off[udata->curr_seq] = (uint8_t*)buf[i];
                udata->len[udata->curr_seq] = udata->str_len[i];
                udata->curr_seq ++;

                break;
            }
        case H5T_VLEN:
            {
                H5T_t *super = NULL;
                size_t elmt_size;
                hvl_t *vl;

                vl = (hvl_t *)udata->buf_ptr;

                if(NULL == (super = H5T_get_super(dt)))
                    HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "invalid super type of VL type");

                elmt_size = H5T_get_size(super) * vl->len;
                udata->buf_size += elmt_size + sizeof(size_t);

                udata->off[udata->curr_seq] = (uint8_t *)udata->buf_ptr;
                udata->len[udata->curr_seq] = sizeof(size_t);
                udata->curr_seq ++;

                udata->off[udata->curr_seq] = (uint8_t *)(vl->p);
                udata->len[udata->curr_seq] = elmt_size;
                udata->curr_seq ++;

                vl ++;
                udata->buf_ptr = (uint8_t *)vl;

                H5T_close(super);

                break;
            }
        default:
            HGOTO_ERROR(H5E_ARGS, H5E_CANTINIT, FAIL, "unsupported datatype");
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_pre_write_cb */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_pre_write
 *
 * Depending on the type, this routine generates all the necessary
 * parameters for forwarding a write call to IOD. It sets up the
 * Mercury Bulk handle and checksums the data.
 *
 * Return:	Success:	SUCCEED 
 *		Failure:	Negative
 *
 * Programmer:  Mohamad Chaarawi
 *              August, 2013
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_pre_write(hid_t type_id, hid_t space_id, const void *buf, 
                   /*out*/uint32_t *_checksum, 
                   /*out*/hg_bulk_t *bulk_handle,
                   /*out*/size_t **vl_str_len)
{
    hsize_t buf_size = 0;
    uint32_t checksum = 0;
    H5S_t *space = NULL;
    H5T_t *dt = NULL;
    size_t nelmts;
    H5T_class_t class;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    if(NULL == (space = (H5S_t *)H5I_object_verify(space_id, H5I_DATASPACE)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "invalid dataspace")
    if(NULL == (dt = (H5T_t *)H5I_object_verify(type_id, H5I_DATATYPE)))
	HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, H5T_NO_CLASS, "not a datatype")

    nelmts = H5S_GET_SELECT_NPOINTS(space);
    class = H5T_get_class(dt, FALSE);
    *vl_str_len = NULL;

    switch(class) {
        case H5T_STRING:
            /* If this is a variable length string, serialize it
               through a Mercury Segmented handle */
            if(H5T_is_variable_str(dt)) {
                char bogus;                 /* bogus value to pass to H5Diterate() */
                H5VL_iod_pre_write_t udata;
                H5_checksum_seed_t cs;
                hg_bulk_segment_t *bulk_segments = NULL;
                unsigned u;

                /* allocate array that hold every string's length */
                udata.str_len = (size_t *)malloc(sizeof(size_t) * nelmts);

                /* set H5Diterate op_data */
                udata.buf_size = 0;
                udata.buf_ptr = (uint8_t *)buf;
                udata.checksum = 0;
                udata.off = (uint8_t **)malloc(nelmts * 2 * sizeof(uint8_t *));
                udata.len = (size_t *)malloc(nelmts * 2 * sizeof(size_t));
                udata.curr_seq = 0;

                /* iterate over every element and compute it's size adding it to
                   the udata buf_size */
                if(H5D__iterate(&bogus, type_id, space, H5VL__iod_pre_write_cb, &udata) < 0)
                    HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "failed to compute buffer size");

                buf_size = udata.buf_size;

                /* Register memory with segmented HG handle */
                bulk_segments = (hg_bulk_segment_t *)malloc((size_t)udata.curr_seq * 
                                                            sizeof(hg_bulk_segment_t));

                for (u = 0; u <udata.curr_seq ; u++) {
                    bulk_segments[u].address = (void *)udata.off[u];
                    bulk_segments[u].size = udata.len[u];
                }

                /* create Bulk handle */
                if (HG_SUCCESS != HG_Bulk_handle_create_segments(bulk_segments, (size_t)udata.curr_seq, 
                                                                 HG_BULK_READWRITE, bulk_handle))
                    HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't create Bulk Data Handle");

                free(bulk_segments);
                bulk_segments = NULL;

                /* compute checksum of length array and the actual stings array */
                cs.a = cs.b = cs.c = cs.state = 0;
                cs.total_length = buf_size;
                for(u = 0; u < udata.curr_seq ; u++) {
                    checksum = H5_checksum_lookup4(udata.off[u], udata.len[u], &cs);
                }

                /* cleanup */
                if(udata.curr_seq) {
                    free(udata.len);
                    udata.len = NULL;
                    free(udata.off);
                    udata.off = NULL;
                }

                *vl_str_len = udata.str_len;

                break;
            }
        case H5T_INTEGER:
        case H5T_FLOAT:
        case H5T_TIME:
        case H5T_BITFIELD:
        case H5T_OPAQUE:
        case H5T_ENUM:
        case H5T_ARRAY:
        case H5T_NO_CLASS:
        case H5T_REFERENCE:
        case H5T_NCLASSES:
        case H5T_COMPOUND:
            /* Data is not variable length, so no need to iterate over
               every element in selection */
            /* MSC - This is not correct. Compound/Array can contian
               VL datatypes, but for now we don't support that. Need
               to check for that too */
            {
                size_t type_size;

                type_size = H5T_get_size(dt);

                buf_size = type_size * nelmts;
                checksum = H5S_checksum(buf, type_size, nelmts, space);

                /* If the memory selection is contiguous, create simple HG Bulk Handle */
                if(H5S_select_is_contiguous(space)) {
                    /* Register memory with bulk_handle */
                    if(HG_SUCCESS != HG_Bulk_handle_create(buf, (size_t)buf_size, 
                                                           HG_BULK_READ_ONLY, bulk_handle))
                        HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't create Bulk Data Handle");
                }

                /* if the memory selection is non-contiguous, create a segmented selection */
                else {
                    hsize_t *off = NULL; /* array that contains the memory addresses of the memory selection */
                    size_t *len = NULL; /* array that contains the length of a contiguous block at each address */
                    size_t count = 0; /* number of offset/length entries in selection */
                    size_t i;
                    hg_bulk_segment_t *bulk_segments = NULL;
                    uint8_t *start_offset = (uint8_t *) buf;

                    /* generate the offsets/lengths pair arrays from the memory dataspace selection */
                    if(H5S_get_offsets(space, type_size, nelmts, &off, &len, &count) < 0)
                        HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't retrieve offets/lengths of memory space");

                    /* Register memory with segmented HG handle */
                    bulk_segments = (hg_bulk_segment_t *)malloc(count * sizeof(hg_bulk_segment_t));
                    for (i = 0; i < count ; i++) {
                        bulk_segments[i].address = (void *)(start_offset + off[i]);
                        bulk_segments[i].size = len[i];
                    }

                    /* create Bulk handle */
                    if (HG_SUCCESS != HG_Bulk_handle_create_segments(bulk_segments, count, 
                                                                     HG_BULK_READWRITE, bulk_handle))
                        HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't create Bulk Data Handle");

                    /* cleanup */
                    if(count) {
                        free(bulk_segments);
                        bulk_segments = NULL;
                        free(len);
                        len = NULL;
                        free(off);
                        off = NULL;
                    }
                }
                break;
            }
            /* If this is a variable length datatype, serialize it
               through a Mercury Segmented handle */
        case H5T_VLEN:
            {
                char bogus;                 /* bogus value to pass to H5Diterate() */
                H5VL_iod_pre_write_t udata;
                H5_checksum_seed_t cs;
                hg_bulk_segment_t *bulk_segments = NULL;
                unsigned u;

                udata.buf_size = 0;
                udata.buf_ptr = (uint8_t *)buf;
                udata.checksum = 0;
                udata.off = (uint8_t **)malloc(nelmts * 2 * sizeof(uint8_t *));
                udata.len = (size_t *)malloc(nelmts * 2 * sizeof(size_t));
                udata.curr_seq = 0;

                /* iterate over every element and compute it's size adding it to
                   the udata buf_size */
                if(H5D__iterate(&bogus, type_id, space, H5VL__iod_pre_write_cb, &udata) < 0)
                    HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "failed to compute buffer size");

                buf_size = udata.buf_size;

                /* Register memory with segmented HG handle */
                bulk_segments = (hg_bulk_segment_t *)malloc((size_t)udata.curr_seq * 
                                                            sizeof(hg_bulk_segment_t));
                for (u = 0; u < udata.curr_seq ; u++) {
                    bulk_segments[u].address = (void *)udata.off[u];
                    bulk_segments[u].size = udata.len[u];
                }

                /* create Bulk handle */
                if (HG_SUCCESS != HG_Bulk_handle_create_segments(bulk_segments, 
                                                                 (size_t)udata.curr_seq, 
                                                                 HG_BULK_READWRITE, bulk_handle))
                    HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't create Bulk Data Handle");

                free(bulk_segments);
                bulk_segments = NULL;

                cs.a = cs.b = cs.c = cs.state = 0;
                cs.total_length = buf_size;

                for(u = 0; u < udata.curr_seq ; u++) {
                    checksum = H5_checksum_lookup4(udata.off[u], udata.len[u], &cs);
                }

                /* cleanup */
                if(udata.curr_seq) {
                    free(udata.len);
                    udata.len = NULL;
                    free(udata.off);
                    udata.off = NULL;
                }

                break;
            }
        default:
            HGOTO_ERROR(H5E_ARGS, H5E_CANTINIT, FAIL, "unsupported datatype");
    }

    *_checksum = checksum;

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_pre_write */


/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_pre_write
 *
 * Depending on the type, this routine generates all the necessary
 * parameters for forwarding a write call to IOD. It sets up the
 * Mercury Bulk handle and checksums the data. If the type is of
 * variable length, then just return that it is, because special
 * processing is required.
 *
 * Return:	Success:	SUCCEED 
 *		Failure:	Negative
 *
 * Programmer:  Mohamad Chaarawi
 *              August, 2013
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_pre_read(hid_t type_id, hid_t space_id, const void *buf, 
                  /*out*/hg_bulk_t *bulk_handle, hbool_t *is_vl_data)
{
    size_t buf_size = 0;
    H5S_t *space = NULL;
    H5T_t *dt = NULL;
    size_t nelmts;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    if(NULL == (space = (H5S_t *)H5I_object_verify(space_id, H5I_DATASPACE)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "invalid dataspace")
    if(NULL == (dt = (H5T_t *)H5I_object_verify(type_id, H5I_DATATYPE)))
	HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, H5T_NO_CLASS, "not a datatype")

    nelmts = H5S_GET_SELECT_NPOINTS(space);

    switch(H5T_get_class(dt, FALSE)) {
        case H5T_INTEGER:
        case H5T_FLOAT:
        case H5T_TIME:
        case H5T_BITFIELD:
        case H5T_OPAQUE:
        case H5T_ENUM:
        case H5T_ARRAY:
        case H5T_NO_CLASS:
        case H5T_STRING:
            if(H5T_is_variable_str(dt)) {
                *is_vl_data = TRUE;
                break;
            }
        case H5T_REFERENCE:
        case H5T_NCLASSES:
        case H5T_COMPOUND:
            {
                size_t type_size;

                *is_vl_data = FALSE;

                type_size = H5T_get_size(dt);
                buf_size = type_size * nelmts;

                /* If the memory selection is contiguous, create simple HG Bulk Handle */
                if(H5S_select_is_contiguous(space)) {
                    /* Register memory with bulk_handle */
                    if(HG_SUCCESS != HG_Bulk_handle_create(buf, buf_size, 
                                                           HG_BULK_READWRITE, bulk_handle))
                        HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't create Bulk Data Handle");
                }

                /* if the memory selection is non-contiguous, create a segmented selection */
                else {
                    hsize_t *off = NULL; /* array that contains the memory addresses of the memory selection */
                    size_t *len = NULL; /* array that contains the length of a contiguous block at each address */
                    size_t count = 0; /* number of offset/length entries in selection */
                    size_t i;
                    hg_bulk_segment_t *bulk_segments = NULL;
                    uint8_t *start_offset = (uint8_t *) buf;

                    /* generate the offsets/lengths pair arrays from the memory dataspace selection */
                    if(H5S_get_offsets(space, type_size, nelmts, &off, &len, &count) < 0)
                        HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't retrieve offets/lengths of memory space");

                    /* Register memory with segmented HG handle */
                    bulk_segments = (hg_bulk_segment_t *)malloc(count * sizeof(hg_bulk_segment_t));
                    for (i = 0; i < count ; i++) {
                        bulk_segments[i].address = (void *)(start_offset + off[i]);
                        bulk_segments[i].size = len[i];
                    }

                    /* create Bulk handle */
                    if (HG_SUCCESS != HG_Bulk_handle_create_segments(bulk_segments, count, 
                                                                     HG_BULK_READWRITE, bulk_handle))
                        HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't create Bulk Data Handle");

                    /* cleanup */
                    if(count) {
                        free(bulk_segments);
                        bulk_segments = NULL;
                        free(len);
                        len = NULL;
                        free(off);
                        off = NULL;
                    }
                }
                break;
            }
        case H5T_VLEN:
            *is_vl_data = TRUE;
            break;
        default:
            HGOTO_ERROR(H5E_ARGS, H5E_CANTINIT, FAIL, "unsupported datatype");
    }

done:
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_pre_read */


/*-------------------------------------------------------------------------
 * Function:    H5VL__iod_vl_read_finalize
 *
 * Finalize the data read by deserializing it into the user's buffer.
 *
 * Return:	Success:	SUCCEED 
 *		Failure:	Negative
 *
 * Programmer:  Mohamad Chaarawi
 *              August, 2013
 *
 *-------------------------------------------------------------------------
 */
static herr_t
H5VL__iod_vl_read_finalize(size_t buf_size, void *read_buf, void *user_buf, 
                           H5S_t *mem_space, hid_t mem_type_id, hid_t dset_type_id)
{
    H5T_t *mem_dt = NULL;
    H5T_t *super = NULL;
    size_t super_size;
    hsize_t nelmts;
    size_t elmt_size = 0;
    H5T_class_t class;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    if(NULL == (mem_dt = (H5T_t *)H5I_object_verify(mem_type_id, H5I_DATATYPE)))
	HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, H5T_NO_CLASS, "not a datatype")
    if(NULL == (super = H5T_get_super(mem_dt)))
        HGOTO_ERROR(H5E_ARGS, H5E_BADTYPE, FAIL, "invalid super type of VL type");
    
    super_size = H5T_get_size(super);
    nelmts = H5S_GET_SELECT_NPOINTS(mem_space);
    class = H5T_get_class(mem_dt, FALSE);

    /* If the memory selection is contiguous, simply iterate over
       every element and store the VL data */
    if(H5S_select_is_contiguous(mem_space)) {
        uint8_t *buf_ptr = (uint8_t *)read_buf;
        unsigned u;

        if(H5T_VLEN == class) {
            size_t seq_len;
            hvl_t *vl = (hvl_t *)user_buf;

            for(u=0 ; u<nelmts ; u++) {
                seq_len = *((size_t *)buf_ptr);
                buf_ptr += sizeof(size_t);

                elmt_size = super_size * seq_len;

                vl[u].len = seq_len;
                vl[u].p = malloc(super_size * seq_len);
                HDmemcpy(vl[u].p, buf_ptr, elmt_size);
                buf_ptr += elmt_size;
            }
        }
        else if(H5T_STRING == class) {
            char **buf = (char **)user_buf;

            for(u=0 ; u<nelmts ; u++) {
                elmt_size = *((size_t *)buf_ptr);
                buf_ptr += sizeof(size_t);

                buf[u] = HDstrdup((char *)buf_ptr);
                buf_ptr += elmt_size;
            }
        }
    }
#if 0
    else {
        hsize_t *off = NULL; /* array that contains the memory addresses of the memory selection */
        size_t *len = NULL; /* array that contains the length of a contiguous block at each address */
        size_t count = 0; /* number of offset/length entries in selection */
        size_t i;
        hg_bulk_segment_t *bulk_segments = NULL;
        uint8_t *start_offset = (uint8_t *) read_buf;

        /* generate the offsets/lengths pair arrays from the memory dataspace selection */
        if(H5S_get_offsets(space, type_size, nelmts, &off, &len, &count) < 0)
            HGOTO_ERROR(H5E_DATASET, H5E_READERROR, FAIL, "can't retrieve offets/lengths of memory space");

        /* cleanup */
        if(count) {
            free(bulk_segments);
            bulk_segments = NULL;
            free(len);
            len = NULL;
            free(off);
            off = NULL;
        }
    }
#endif

done:
    if(super && H5T_close(super) < 0)
        HDONE_ERROR(H5E_DATATYPE, H5E_CANTDEC, FAIL, "can't close super type")
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL__iod_vl_read_finalize */

#if 0
static herr_t
H5VL_generate_axe_ids(int myrank, int nranks, uint64_t *start_id)
{
    uint64_t seed;
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    seed = (pow(2,64) - 1) / nranks;
    *start_id = seed * my_rank;

done:
    FUNC_LEAVE_NOAPI(ret_value)
}

static herr_t
H5VL_iod_get_axe_id(int myrank, int nranks, int cur_index, uint64_t *id)
{
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT



done:
    FUNC_LEAVE_NOAPI(ret_value)
}

/*-------------------------------------------------------------------------
 * Function:    H5VL_iod_get_parent_info
 *
 * Purpose:     This routine traverses the path in name, or in loc_params 
 *              if the path is specified there, to determine the components
 *              of the path that are present locally in the ID space. 
 *              Once a component in the path is not found, the routine
 *              breaks at that point and stores the remaining path in new_name.
 *              This is where the traversal can begin at the server. 
 *              The IOD ID, OH, and axe_id belonging to the last object 
 *              present are returned too. 
 *
 * Return:	Non-negative on success/Negative on failure
 *
 *-------------------------------------------------------------------------
 */
herr_t
H5VL_iod_get_parent_info(H5VL_iod_object_t *obj, H5VL_loc_params_t loc_params, 
                         const char *name, /*OUT*/iod_obj_id_t *iod_id, 
                         /*OUT*/iod_handle_t *iod_oh, /*OUT*/H5VL_iod_request_t **parent_req, 
                         /*OUT*/char **new_name, /*OUT*/H5VL_iod_object_t **last_obj)
{
    iod_obj_id_t cur_id;
    iod_handle_t cur_oh;
    size_t cur_size; /* current size of the path traversed so far */
    char *cur_name;  /* full path to object that is currently being looked for */
    H5VL_iod_object_t *cur_obj = obj;   /* current object in the traversal loop */
    H5VL_iod_object_t *next_obj = NULL; /* the next object to traverse */
    const char *path;        /* specified path for the object to traverse to */
    H5WB_t *wb = NULL;       /* Wrapped buffer for temporary buffer */
    char comp_buf[1024];     /* Temporary buffer for path components */
    char *comp;              /* Pointer to buffer for path components */
    size_t nchars;	     /* component name length	*/
    H5VL_iod_file_t *file = obj->file; /* pointer to file where the search happens */
    hbool_t last_comp = FALSE; /* Flag to indicate that a component is the last component in the name */
    herr_t ret_value = SUCCEED;

    FUNC_ENTER_NOAPI_NOINIT

    if(loc_params.type == H5VL_OBJECT_BY_SELF)
        path = name;
    else if (loc_params.type == H5VL_OBJECT_BY_NAME)
        path = loc_params.loc_data.loc_by_name.name;

    if (NULL == (cur_name = (char *)malloc(HDstrlen(obj->obj_name) + HDstrlen(path) + 2)))
	HGOTO_ERROR(H5E_RESOURCE, H5E_NOSPACE, FAIL, "can't allocate");

    HDstrcpy(cur_name, obj->obj_name);
    cur_size = HDstrlen(obj->obj_name);

    if(obj->obj_type != H5I_FILE) {
        HDstrcat(cur_name, "/");
        cur_size += 1;
    }
        
    /* Wrap the local buffer for serialized header info */
    if(NULL == (wb = H5WB_wrap(comp_buf, sizeof(comp_buf))))
        HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "can't wrap buffer")
    /* Get a pointer to a buffer that's large enough  */
    if(NULL == (comp = (char *)H5WB_actual(wb, (HDstrlen(path) + 1))))
        HGOTO_ERROR(H5E_SYM, H5E_NOSPACE, FAIL, "can't get actual buffer")

    /* Traverse the path */
    while((path = H5G__component(path, &nchars)) && *path) {
        const char *s;                  /* Temporary string pointer */

	/*
	 * Copy the component name into a null-terminated buffer so
	 * we can pass it down to the other symbol table functions.
	 */
	HDmemcpy(comp, path, nchars);
	comp[nchars] = '\0';

	/*
	 * The special name `.' is a no-op.
	 */
	if('.' == comp[0] && !comp[1]) {
	    path += nchars;
	    continue;
	} /* end if */

        /* Check if this is the last component of the name */
        if(!((s = H5G__component(path + nchars, NULL)) && *s))
            last_comp = TRUE;

        HDstrcat(cur_name, comp);
        cur_size += nchars;
        cur_name[cur_size] = '\0';

        if(NULL == (next_obj = (const H5VL_iod_object_t *)H5I_search_name(file, cur_name, H5I_GROUP))) {
            if(last_comp) {
                if(NULL == (next_obj = (const H5VL_iod_object_t *)H5I_search_name
                            (file, cur_name, H5I_DATASET))
                   && NULL == (next_obj = (H5VL_iod_object_t *)H5I_search_name
                               (file, cur_name, H5I_DATATYPE))
                   && NULL == (next_obj = (H5VL_iod_object_t *)H5I_search_name
                               (file, cur_name, H5I_MAP)))
                    break;
            }
            else {
                break;
            }
        }

#if H5VL_IOD_DEBUG
        printf("Found %s Locally\n", comp);
#endif

	/* Advance to next component in string */
	path += nchars;
        HDstrcat(cur_name, "/");
        cur_size += 1;
        cur_obj = next_obj;
    }

    switch(cur_obj->obj_type) {
        case H5I_FILE:
            cur_oh = cur_obj->file->remote_file.root_oh;
            cur_id = cur_obj->file->remote_file.root_id;
            break;
        case H5I_GROUP:
            cur_oh = ((const H5VL_iod_group_t *)cur_obj)->remote_group.iod_oh;
            cur_id = ((const H5VL_iod_group_t *)cur_obj)->remote_group.iod_id;
            break;
        case H5I_DATASET:
            cur_oh = ((const H5VL_iod_dset_t *)cur_obj)->remote_dset.iod_oh;
            cur_id = ((const H5VL_iod_dset_t *)cur_obj)->remote_dset.iod_id;
            break;
        case H5I_DATATYPE:
            cur_oh = ((const H5VL_iod_dtype_t *)cur_obj)->remote_dtype.iod_oh;
            cur_id = ((const H5VL_iod_dtype_t *)cur_obj)->remote_dtype.iod_id;
            break;
        case H5I_MAP:
            cur_oh = ((const H5VL_iod_map_t *)cur_obj)->remote_map.iod_oh;
            cur_id = ((const H5VL_iod_map_t *)cur_obj)->remote_map.iod_id;
            break;
        default:
            HGOTO_ERROR(H5E_SYM, H5E_CANTINIT, FAIL, "bad location object");
    }

    if(cur_obj->request && cur_obj->request->status == H5AO_PENDING) {
        *parent_req = cur_obj->request;
        cur_obj->request->ref_count ++;
    }
    else {
        *parent_req = NULL;
        HDassert(cur_oh.cookie != IOD_OH_UNDEFINED);
    }

    *iod_id = cur_id;
    *iod_oh = cur_oh;

    if(*path)
        *new_name = strdup(path);
    else
        *new_name = strdup(".");

    if(last_obj)
        *last_obj = cur_obj;

done:
    free(cur_name);
    /* Release temporary component buffer */
    if(wb && H5WB_unwrap(wb) < 0)
        HDONE_ERROR(H5E_SYM, H5E_CANTRELEASE, FAIL, "can't release wrapped buffer")
    FUNC_LEAVE_NOAPI(ret_value)
} /* end H5VL_iod_get_parent_info */
#endif

#endif /* H5_HAVE_EFF */
