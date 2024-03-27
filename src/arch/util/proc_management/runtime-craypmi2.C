/*
 * Copyright (c) 2017, Intel Corporation. All rights reserved.
 * See LICENSE in src/arch/ofi.
 *
 * Runtime functions used by OFI LRTS machine layer to exchange
 * addresses during the initialization.
 *
 * This example uses the PMI2 API as described in pmi2.h.
 */

/* This example has been modified to use the cray extensions
 */
#include <stdlib.h>
#include <string.h>

#include <pmi_cray.h>

#include "runtime.h"

/* For encode/decode functions */
#include "runtime-codec.h"

static int initialized;
static int max_keylen = PMI2_MAX_KEYLEN;
static int max_valuelen = PMI2_MAX_VALLEN;
static char *kvsname;
static char *key;
static char *value;

int runtime_init(int *rank, int *jobsize)
{
    int ret;
    int spawned;
    int appnum;
    int max_kvsnamelen = PMI2_MAX_VALLEN;

    ret = PMI2_Init(&spawned, jobsize, rank, &appnum);
    //    printf("PMI2_init ret %d jobsize %d rank %d\n",ret, *jobsize, *rank);
    if (PMI2_SUCCESS != ret) {
        return 1;
    }

    kvsname = (char*)calloc(max_kvsnamelen, sizeof(char));
    if (!kvsname) {
        return 2;
    }

    ret = PMI2_Job_GetId(kvsname, max_kvsnamelen);
    if (PMI2_SUCCESS != ret) {
        return 3;
    }

    key = (char*)calloc(max_keylen, sizeof(char));
    if (!key) {
        free(kvsname);
        return 4;
    }

    value = (char*)calloc(max_valuelen, sizeof(char));
    if (!value) {
        free(key);
        free(kvsname);
        return 5;
    }

    initialized = 1;
    return 0;
}

int runtime_fini()
{
    int ret;

    if (initialized) {
        ret = PMI2_Finalize();
        if (PMI2_SUCCESS != ret) {
            return 1;
        }
    }

    if (value) {
        free(value);
        value = NULL;
    }
    if (key) {
        free(key);
        key = NULL;
    }
    if (kvsname) {
        free(kvsname);
        kvsname = NULL;
    }

    initialized = 0;
    return 0;
}

int runtime_get_max_keylen(int *len)
{
    if (!initialized) {
        return 1;
    }
    *len = max_keylen;
    return 0;
}

int runtime_get_max_vallen(int *len)
{
    if (!initialized) {
        return 1;
    }
    *len = (max_valuelen - 1) / 2;
    return 0;
}

int runtime_kvs_put(const char *k, const void *v, int vlen)
{
    int ret;
    int keylen;

    if (!initialized) {
        return 1;
    }

    keylen = strlen(k);
    if (keylen > max_keylen) {
        return 2;
    }

    if (vlen > max_valuelen) {
        return 3;
    }

    ret = encode(v, vlen, value, max_valuelen);
    if (ret) {
        return 4;
    }

    ret = PMI2_KVS_Put(k, value);
    if (ret) {
        return 5;
    }

    return 0;
}

int runtime_kvs_get(const char *k, void *v, int vlen, int id)
{
    int ret;
    int len;

    if (!initialized) {
        return 1;
    }

    ret = PMI2_KVS_Get(kvsname, PMI2_ID_NULL, k, value, max_valuelen, &len);
    if (ret) {
        return 2;
    }

    ret = decode(value, v, vlen);
    if (ret) {
        return 3;
    }

    return 0;
}

int runtime_barrier()
{
    int ret;

    if (!initialized) {
        return 1;
    }

    ret = PMI2_KVS_Fence();
    if (ret) {
        return 2;
    }
    return 0;
}
