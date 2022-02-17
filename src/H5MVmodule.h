/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Copyright by The HDF Group.                                               *
 * Copyright by Akadio, Inc.                                                 *
 * All rights reserved.                                                      *
 *                                                                           *
 * This file is part of HDF5.  The full HDF5 copyright notice, including     *
 * terms governing use, modification, and redistribution, is contained in    *
 * the COPYING file, which can be found at the root of the source code       *
 * distribution tree, or in https://www.hdfgroup.org/licenses.               *
 * If you do not have access to either file, you may request a copy from     *
 * help@hdfgroup.org.                                                        *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*-------------------------------------------------------------------------
 *
 * Created:             H5MVmodule.h
 *
 * Purpose:             This file contains declarations which define macros for
 *                      the H5MV package.  Including this header means that the
 *                      source file is part of the H5MV package.
 *
 *-------------------------------------------------------------------------
 */

#ifndef _H5MVmodule_H
#define _H5MVmodule_H

/* Define the proper control macros for the generic FUNC_ENTER/LEAVE and error
 *      reporting macros.
 */
#define H5MV_MODULE
#define H5_MY_PKG      H5MV
#define H5_MY_PKG_ERR  H5E_RESOURCE
#define H5_MY_PKG_INIT NO

#endif /* _H5MVmodule_H */
