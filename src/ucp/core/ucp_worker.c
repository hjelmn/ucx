/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2015.  ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#include "ucp_worker.h"

#include <ucp/wireup/wireup.h>
#include <ucp/tag/eager.h>


static void ucp_worker_close_ifaces(ucp_worker_h worker)
{
    ucp_rsc_index_t rsc_index;

    for (rsc_index = 0; rsc_index < worker->context->num_tls; ++rsc_index) {
        if (worker->ifaces[rsc_index] == NULL) {
            continue;
        }

        uct_iface_close(worker->ifaces[rsc_index]);
    }
}

static ucs_status_t ucp_worker_set_am_handlers(ucp_worker_h worker,
                                               uct_iface_h iface)
{
    ucp_context_h context = worker->context;
    ucs_status_t status;
    unsigned am_id;

    for (am_id = 0; am_id < UCP_AM_ID_LAST; ++am_id) {
        if (context->config.features & ucp_am_handlers[am_id].features) {
            status = uct_iface_set_am_handler(iface, am_id, ucp_am_handlers[am_id].cb,
                                              worker, 
                                              ucp_am_handlers[am_id].flags);
            if (status != UCS_OK) {
                return status;
            }
        }
    }

    return UCS_OK;
}

static ucs_status_t ucp_stub_am_handler(void *arg, void *data, size_t length, void *desc)
{
    ucp_worker_h worker = arg;
    ucs_trace("worker %p: drop message", worker);
    return UCS_OK;
}

static void ucp_worker_remove_am_handlers(ucp_worker_h worker)
{
    ucp_context_h context = worker->context;
    ucp_rsc_index_t tl_id;
    unsigned am_id;

    ucs_debug("worker %p: remove active message handlers", worker);
    for (tl_id = 0; tl_id < context->num_tls; ++tl_id) {
        for (am_id = 0; am_id < UCP_AM_ID_LAST; ++am_id) {
            if (context->config.features & ucp_am_handlers[am_id].features) {
                (void)uct_iface_set_am_handler(worker->ifaces[tl_id], am_id,
                                               ucp_stub_am_handler, worker, 0);
            }
        }
    }
}

static void ucp_worker_am_tracer(void *arg, uct_am_trace_type_t type,
                                 uint8_t id, const void *data, size_t length,
                                 char *buffer, size_t max)
{
    ucp_worker_h worker = arg;
    ucp_am_tracer_t tracer;

    tracer = ucp_am_handlers[id].tracer;
    if (tracer != NULL) {
        tracer(worker, type, id, data, length, buffer, max);
    }
}

static ucs_status_t ucp_worker_add_iface(ucp_worker_h worker,
                                         ucp_rsc_index_t tl_id)
{
    ucp_context_h context = worker->context;
    ucp_tl_resource_desc_t *resource = &context->tl_rscs[tl_id];
    uct_iface_config_t *iface_config;
    ucs_status_t status;
    uct_iface_h iface;

    /* Read configuration
     * TODO pass env_prefix from context */
    status = uct_iface_config_read(resource->tl_rsc.tl_name, NULL, NULL,
                                   &iface_config);
    if (status != UCS_OK) {
        goto out;
    }

    /* Open UCT interface */
    status = uct_iface_open(context->pds[resource->pd_index], worker->uct,
                            resource->tl_rsc.tl_name, resource->tl_rsc.dev_name,
                            sizeof(ucp_recv_desc_t), iface_config, &iface);
    uct_config_release(iface_config);

    if (status != UCS_OK) {
        goto out;
    }

    status = uct_iface_query(iface, &worker->iface_attrs[tl_id]);
    if (status != UCS_OK) {
        goto out;
    }

    /* Set active message handlers for tag matching */
    status = ucp_worker_set_am_handlers(worker, iface);
    if (status != UCS_OK) {
        goto out_close_iface;
    }

    status = uct_iface_set_am_tracer(iface, ucp_worker_am_tracer, worker);
    if (status != UCS_OK) {
        goto out_close_iface;
    }

    ucs_debug("created interface[%d] using "UCT_TL_RESOURCE_DESC_FMT" on worker %p",
              tl_id, UCT_TL_RESOURCE_DESC_ARG(&resource->tl_rsc), worker);

    worker->ifaces[tl_id] = iface;
    return UCS_OK;

out_close_iface:
    uct_iface_close(iface);
out:
    return status;
}

static void ucp_worker_set_config(ucp_worker_h worker, ucp_rsc_index_t tl_id)
{
    ucp_context_h context        = worker->context;
    uct_iface_attr_t *iface_attr = &worker->iface_attrs[tl_id];
    ucp_ep_config_t *config      = &worker->ep_config[tl_id];

    if (iface_attr->cap.flags & UCT_IFACE_FLAG_AM_SHORT) {
        config->eager.max_short    = iface_attr->cap.am.max_short - sizeof(ucp_tag_t);
    } else {
        config->eager.max_short    = 0;
    }
    if (iface_attr->cap.flags & UCT_IFACE_FLAG_AM_BCOPY) {
        config->eager.max_bcopy    = iface_attr->cap.am.max_bcopy - sizeof(ucp_eager_hdr_t);
    } else {
        config->eager.max_bcopy    = 0;
    }
    config->eager.max_zcopy        = 0;
    config->eager.bcopy_thresh     = -1;
    config->eager.zcopy_thresh     = -1;

    config->put.max_short          = iface_attr->cap.put.max_short;
    config->put.max_bcopy          = iface_attr->cap.put.max_bcopy;
    config->put.max_zcopy          = 0;
    config->put.bcopy_thresh       = context->config.ext.bcopy_thresh;
    config->put.zcopy_thresh       = -1;

    config->get.max_short          = 0;
    config->get.max_bcopy          = iface_attr->cap.get.max_bcopy;
    config->get.max_zcopy          = 0;
    config->get.bcopy_thresh       = -1;
    config->get.zcopy_thresh       = -1;
}

ucs_status_t ucp_worker_create(ucp_context_h context, ucs_thread_mode_t thread_mode,
                               ucp_worker_h *worker_p)
{
    ucp_rsc_index_t tl_id;
    ucp_worker_h worker;
    ucs_status_t status;

    worker = ucs_calloc(1, sizeof(*worker) +
                           sizeof(*worker->ep_config) * context->num_tls,
                        "ucp worker");
    if (worker == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    worker->context       = context;
    worker->uuid          = ucs_generate_uuid((uintptr_t)worker);
#if ENABLE_ASSERT
    worker->inprogress    = 0;
#endif
    worker->stub_pend_count = 0;

    worker->ep_hash = ucs_malloc(sizeof(*worker->ep_hash) * UCP_WORKER_EP_HASH_SIZE,
                                 "ucp_ep_hash");
    if (worker->ep_hash == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_free;
    }

    sglib_hashed_ucp_ep_t_init(worker->ep_hash);

    worker->ifaces = ucs_calloc(context->num_tls, sizeof(*worker->ifaces),
                                "ucp iface");
    if (worker->ifaces == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_free_ep_hash;
    }

    worker->iface_attrs = ucs_calloc(context->num_tls,
                                     sizeof(*worker->iface_attrs),
                                     "ucp iface_attr");
    if (worker->iface_attrs == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_free_ifaces;
    }

    status = ucs_async_context_init(&worker->async, UCS_ASYNC_MODE_THREAD);
    if (status != UCS_OK) {
        goto err_free_attrs;
    }

    /* Create the underlying UCT worker */
    status = uct_worker_create(&worker->async, thread_mode, &worker->uct);
    if (status != UCS_OK) {
        goto err_destroy_async;
    }

    /* Create memory pool for requests */
    status = ucs_mpool_init(&worker->req_mp, 0,
                            sizeof(ucp_request_t) + context->config.request.size,
                            0, UCS_SYS_CACHE_LINE_SIZE, 128, UINT_MAX,
                            &ucp_request_mpool_ops, "ucp_requests");
    if (status != UCS_OK) {
        goto err_destroy_uct_worker;
    }

    /* Open all resources as interfaces on this worker */
    for (tl_id = 0; tl_id < context->num_tls; ++tl_id) {
        status = ucp_worker_add_iface(worker, tl_id);
        if (status != UCS_OK) {
            goto err_close_ifaces;
        }

        ucp_worker_set_config(worker, tl_id);
    }

    *worker_p = worker;
    return UCS_OK;

err_close_ifaces:
    ucp_worker_close_ifaces(worker);
    ucs_mpool_cleanup(&worker->req_mp, 1);
err_destroy_uct_worker:
    uct_worker_destroy(worker->uct);
err_destroy_async:
    ucs_async_context_cleanup(&worker->async);
err_free_attrs:
    ucs_free(worker->iface_attrs);
err_free_ifaces:
    ucs_free(worker->ifaces);
err_free_ep_hash:
    ucs_free(worker->ep_hash);
err_free:
    ucs_free(worker);
err:
    return status;
}

static void ucp_worker_destroy_eps(ucp_worker_h worker)
{
    struct sglib_hashed_ucp_ep_t_iterator iter;
    ucp_ep_h ep;

    ucs_debug("worker %p: destroy all endpoints", worker);
    for (ep = sglib_hashed_ucp_ep_t_it_init(&iter, worker->ep_hash); ep != NULL;
         ep = sglib_hashed_ucp_ep_t_it_next(&iter))
    {
        ucp_ep_destroy(ep);
    }
}

void ucp_worker_destroy(ucp_worker_h worker)
{
    ucs_trace_func("worker=%p", worker);
    ucp_worker_remove_am_handlers(worker);
    ucp_worker_destroy_eps(worker);
    ucp_worker_close_ifaces(worker);
    ucs_mpool_cleanup(&worker->req_mp, 1);
    uct_worker_destroy(worker->uct);
    ucs_async_context_cleanup(&worker->async);
    ucs_free(worker->iface_attrs);
    ucs_free(worker->ifaces);
    ucs_free(worker->ep_hash);
    ucs_free(worker);
}

void ucp_worker_progress(ucp_worker_h worker)
{
    /* worker->inprogress is used only for assertion check.
     * coverity[assert_side_effect]
     */
    ucs_assert(worker->inprogress++ == 0);
    uct_worker_progress(worker->uct);
    ucs_async_check_miss(&worker->async);

    /* coverity[assert_side_effect] */
    ucs_assert(--worker->inprogress == 0);
}

static ucs_status_t
ucp_worker_pack_resource_address(ucp_worker_h worker, ucp_rsc_index_t rsc_index,
                                 void **addr_buf_p, size_t *length_p)
{
    ucp_context_h context = worker->context;
    uct_iface_attr_t *iface_attr = &worker->iface_attrs[rsc_index];
    ucp_tl_resource_desc_t *resource = &context->tl_rscs[rsc_index];
    ucs_status_t status;
    void *buffer, *ptr;
    uint8_t tl_name_len;
    size_t length;
    int af;

    tl_name_len = strlen(resource->tl_rsc.tl_name);

    /* Calculate new address buffer size */
    length   = 1 +                           /* address length */
               iface_attr->iface_addr_len +  /* address */
               1 +                           /* tl name length */
               tl_name_len +                 /* tl name */
               1 +                           /* pd index */
               1;                            /* resource index */

    /* Enlarge address buffer */
    buffer = ucs_malloc(length, "ucp address");
    if (buffer == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    ptr = buffer;

    /* Copy iface address length */
    ucs_assert_always(iface_attr->iface_addr_len <= UINT8_MAX);
    ucs_assert_always(iface_attr->iface_addr_len > 0);
    *((uint8_t*)ptr++) = iface_attr->iface_addr_len;

    /* Copy iface address */
    status = uct_iface_get_address(worker->ifaces[rsc_index], ptr);
    if (status != UCS_OK) {
        goto err_free;
    }

    af   = ((struct sockaddr*)ptr)->sa_family;
    ptr += iface_attr->iface_addr_len;

    /* Transport name */
    *((uint8_t*)ptr++) = tl_name_len;
    memcpy(ptr, resource->tl_rsc.tl_name, tl_name_len);
    ptr += tl_name_len;

    *((ucp_rsc_index_t*)ptr++) = resource->pd_index;
    *((ucp_rsc_index_t*)ptr++) = rsc_index;

    ucs_trace("adding resource[%d]: "UCT_TL_RESOURCE_DESC_FMT" family %d address length %zu",
               rsc_index, UCT_TL_RESOURCE_DESC_ARG(&resource->tl_rsc), af, length);

    *addr_buf_p = buffer;
    *length_p   = length;
    return UCS_OK;

err_free:
    ucs_free(buffer);
err:
    return status;
}

ucs_status_t ucp_worker_get_address(ucp_worker_h worker, ucp_address_t **address_p,
                                    size_t *address_length_p)
{
    ucp_context_h context = worker->context;
    char name[UCP_PEER_NAME_MAX];
    ucp_address_t *address;
    size_t address_length, rsc_addr_length = 0;
    ucs_status_t status;
    ucp_rsc_index_t rsc_index = -1;
    void *rsc_addr = NULL;

    UCS_STATIC_ASSERT((ucp_address_t*)0 + 1 == (void*)0 + 1);

#if ENABLE_DEBUG_DATA
    ucs_snprintf_zero(name, UCP_PEER_NAME_MAX, "%s:%d", ucs_get_host_name(),
                      getpid()); /* TODO tid? */
#else
    memset(name, 0, sizeof(name));
#endif

    address_length = sizeof(uint64_t) + strlen(name) + 1;
    address        = ucs_malloc(address_length + 1, "ucp address");
    if (address == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_free;
    }

    /* Set address UUID */
    *(uint64_t*)address = worker->uuid;

    /* Set peer name */
    UCS_STATIC_ASSERT(UCP_PEER_NAME_MAX <= UINT8_MAX);
    *(uint8_t*)(address + sizeof(uint64_t)) = strlen(name);
    strcpy(address + sizeof(uint64_t) + 1, name);

    ucs_assert(ucp_address_iter_start(address) == address + address_length);

    for (rsc_index = 0; rsc_index < context->num_tls; ++rsc_index) {
        status = ucp_worker_pack_resource_address(worker, rsc_index,
                                                  &rsc_addr, &rsc_addr_length);
        if (status != UCS_OK) {
            goto err_free;
        }

        /* Enlarge address buffer, leave room for NULL terminator */
        address_length += rsc_addr_length;
        address = ucs_realloc(address, address_length + 1, "ucp address");
        if (address == NULL) {
            status = UCS_ERR_NO_MEMORY;
            ucs_free(rsc_addr);
            goto err_free;
        }

        /* Add the address of the current resource */
        memcpy(address + address_length - rsc_addr_length, rsc_addr, rsc_addr_length);
        ucs_free(rsc_addr);
    }

    if (address_length == 0) {
        ucs_error("No valid transport found");
        status = UCS_ERR_NO_DEVICE;
        goto err_free;
    }

    /* The final NULL terminator */
    *((uint8_t*)address + address_length) = 0;
    ++address_length;
    ucs_debug("worker uuid 0x%"PRIx64" address length: %zu", worker->uuid, address_length);

    *address_p        = address;
    *address_length_p = address_length;
    return UCS_OK;

err_free:
    ucs_free(address);
    return status;
}

void ucp_worker_release_address(ucp_worker_h worker, ucp_address_t *address)
{
    ucs_free(address);
}

SGLIB_DEFINE_LIST_FUNCTIONS(ucp_ep_t, ucp_worker_ep_compare, next);
SGLIB_DEFINE_HASHED_CONTAINER_FUNCTIONS(ucp_ep_t, UCP_WORKER_EP_HASH_SIZE,
                                        ucp_worker_ep_hash);

static void ucp_worker_ep_proto_config_print(FILE *stream, const char *proto,
                                             ucp_ep_proto_config_t *config)
{
    fprintf(stream, "# %20s   %15zd %15zd %15zd %15zd %15zd\n",
            proto,
            config->bcopy_thresh,
            config->zcopy_thresh,
            config->max_short,
            config->max_bcopy,
            config->max_zcopy);
}

void ucp_worker_proto_print(ucp_worker_h worker, FILE *stream, const char *title,
                            ucs_config_print_flags_t print_flags)
{
    ucp_context_h context = worker->context;
    ucp_rsc_index_t tl_id;
    char rsc_name[UCT_TL_NAME_MAX + UCT_DEVICE_NAME_MAX + 2];

    if (print_flags & UCS_CONFIG_PRINT_HEADER) {
        fprintf(stream, "#\n");
        fprintf(stream, "# %s\n", title);
        fprintf(stream, "#\n");
    }

    fprintf(stream, "# Transports: \n");
    fprintf(stream, "#\n");

    for (tl_id = 0; tl_id < worker->context->num_tls; ++tl_id) {

        snprintf(rsc_name, sizeof(rsc_name), UCT_TL_RESOURCE_DESC_FMT,
                 UCT_TL_RESOURCE_DESC_ARG(&context->tl_rscs[tl_id].tl_rsc));

        fprintf(stream, "# %3d %-18s %15s %15s %15s %15s %15s\n", tl_id, rsc_name,
                "bcopy_thresh", "zcopy_thresh", "max_short", "max_bcopy", "max_zcopy");

        ucp_worker_ep_proto_config_print(stream, "eager",
                                         &worker->ep_config[tl_id].eager);
        ucp_worker_ep_proto_config_print(stream, "put",
                                         &worker->ep_config[tl_id].put);
        ucp_worker_ep_proto_config_print(stream, "get",
                                         &worker->ep_config[tl_id].get);
        fprintf(stream, "#\n");
    }
}
