/**
 * Copyright (C) UT-Battelle, LLC. 2015. ALL RIGHTS RESERVED.
 * Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */
#include <ucs/datastruct/sglib_wrapper.h>
#include <ucs/debug/memtrack.h>
#include <ucs/debug/log.h>
#include <uct/base/uct_log.h>

#include "ugni_smsg_ep.h"
#include "ugni_smsg_iface.h"
#include <uct/ugni/base/ugni_device.h>
#include <uct/ugni/base/ugni_ep.h>

SGLIB_DEFINE_LIST_FUNCTIONS(uct_ugni_smsg_desc_t, uct_ugni_smsg_desc_compare, next);
SGLIB_DEFINE_HASHED_CONTAINER_FUNCTIONS(uct_ugni_smsg_desc_t, UCT_UGNI_HASH_SIZE, uct_ugni_smsg_desc_hash);

static void uct_ugni_smsg_mbox_init(uct_ugni_smsg_iface_t *iface, uct_ugni_smsg_mbox_t *mbox_info){
    void *mbox_data = (void *)(mbox_info+1);

    mbox_info->mbox_attr.mem_hndl = mbox_info->gni_mem;
    mbox_info->mbox_attr.msg_buffer = mbox_data;
    mbox_info->mbox_attr.msg_type = GNI_SMSG_TYPE_MBOX_AUTO_RETRANSMIT;
    mbox_info->mbox_attr.buff_size = iface->bytes_per_mbox;
    mbox_info->mbox_attr.mbox_offset = 0;
    mbox_info->mbox_attr.mbox_maxcredit = iface->config.smsg_max_credit;
    mbox_info->mbox_attr.msg_maxsize = iface->config.smsg_seg_size;
}

static ucs_status_t uct_ugni_smsg_mbox_reg(uct_ugni_smsg_iface_t *iface, uct_ugni_smsg_mbox_t *mbox)
{
    gni_return_t ugni_rc;
    void *address = (mbox+1);

    if (0 == iface->bytes_per_mbox) {
        ucs_error("Unexpected length %zu", iface->bytes_per_mbox);
        return UCS_ERR_INVALID_PARAM;
    }
    pthread_mutex_lock(&uct_ugni_global_lock);

    ugni_rc = GNI_MemRegister(iface->super.nic_handle, (uint64_t)address,
                              iface->bytes_per_mbox, iface->remote_cq,
                              GNI_MEM_READWRITE,
                              -1, &(mbox->gni_mem));

    pthread_mutex_unlock(&uct_ugni_global_lock);

    if (GNI_RC_SUCCESS != ugni_rc) {
        ucs_error("GNI_MemRegister failed (addr %p, size %zu), Error status: %s %d",
                  address, iface->bytes_per_mbox, gni_err_str[ugni_rc], ugni_rc);
        return UCS_ERR_IO_ERROR;
    }

    mbox->base_address = (uintptr_t)address;

    return UCS_OK;
}

static ucs_status_t uct_ugni_smsg_mbox_dereg(uct_ugni_smsg_iface_t *iface, uct_ugni_smsg_mbox_t *mbox){
    gni_return_t ugni_rc;

    pthread_mutex_lock(&uct_ugni_global_lock);
    ugni_rc = GNI_MemDeregister(iface->super.nic_handle, &mbox->gni_mem);
    pthread_mutex_unlock(&uct_ugni_global_lock);

    if (GNI_RC_SUCCESS != ugni_rc) {
        ucs_error("GNI_MemDeegister failed Error status: %s %d",
                  gni_err_str[ugni_rc], ugni_rc);
        return UCS_ERR_IO_ERROR;
    }

    return UCS_OK;
}

static UCS_CLASS_INIT_FUNC(uct_ugni_smsg_ep_t, uct_iface_t *tl_iface)
{
    UCS_CLASS_CALL_SUPER_INIT(uct_ugni_ep_t, tl_iface, NULL);
    uct_ugni_smsg_iface_t *iface = ucs_derived_of(tl_iface, uct_ugni_smsg_iface_t);
    void *mbox;

    UCT_TL_IFACE_GET_TX_DESC(&iface->super.super, &iface->free_mbox,
                             mbox, return UCS_ERR_NO_RESOURCE);

    self->smsg_attr = (uct_ugni_smsg_mbox_t *)mbox;

    uct_ugni_smsg_mbox_reg(iface, self->smsg_attr);
    uct_ugni_smsg_mbox_init(iface, self->smsg_attr);

    return UCS_OK;
}

static UCS_CLASS_CLEANUP_FUNC(uct_ugni_smsg_ep_t)
{
    uct_ugni_smsg_iface_t *iface = ucs_derived_of(self->super.super.super.iface, uct_ugni_smsg_iface_t);
    uct_ugni_smsg_mbox_dereg(iface, self->smsg_attr);
    ucs_mpool_put(self->smsg_attr);
}

UCS_CLASS_DEFINE(uct_ugni_smsg_ep_t, uct_ugni_ep_t);
UCS_CLASS_DEFINE_NEW_FUNC(uct_ugni_smsg_ep_t, uct_ep_t, uct_iface_t*);
UCS_CLASS_DEFINE_DELETE_FUNC(uct_ugni_smsg_ep_t, uct_ep_t);

ucs_status_t uct_ugni_smsg_ep_get_address(uct_ep_h tl_ep, struct sockaddr *addr) {

    uct_ugni_smsg_ep_t *ep = ucs_derived_of(tl_ep, uct_ugni_smsg_ep_t);
    uct_sockaddr_smsg_ugni_t *ep_addr = (uct_sockaddr_smsg_ugni_t*)addr;

    ucs_status_t rc;

    rc = uct_ugni_iface_get_address(tl_ep->iface, addr);

    if(UCS_OK != rc){
        return rc;
    }

    ep_addr->ep_hash = ep->super.hash_key;
    memcpy(&ep_addr->smsg_attr, &ep->smsg_attr->mbox_attr, sizeof(gni_smsg_attr_t));

    return UCS_OK;
}

ucs_status_t uct_ugni_smsg_ep_connect_to_ep(uct_ep_h tl_ep, const struct sockaddr *addr){
    uct_ugni_smsg_ep_t *ep = ucs_derived_of(tl_ep, uct_ugni_smsg_ep_t);
    uct_ugni_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ugni_iface_t);
    const uct_sockaddr_smsg_ugni_t *iface_addr = (const uct_sockaddr_smsg_ugni_t*)addr;
    gni_smsg_attr_t *local_attr = (gni_smsg_attr_t*)&ep->smsg_attr->mbox_attr;
    gni_smsg_attr_t *remote_attr = (gni_smsg_attr_t *)&iface_addr->smsg_attr;
    gni_return_t gni_rc;
    ucs_status_t rc = UCS_OK;
    uint32_t ep_hash;

    pthread_mutex_lock(&uct_ugni_global_lock);
    rc = ugni_connect_ep(iface, &iface_addr->super, &ep->super);

    if(UCS_OK != rc){
        ucs_error("Could not connect ep in smsg");
        goto exit_lock;
    }

    gni_rc = GNI_SmsgInit(ep->super.ep, local_attr, remote_attr);

    if(GNI_RC_SUCCESS != gni_rc){
        ucs_error("Failed to initalize smsg. %s [%i]", gni_err_str[gni_rc], gni_rc);
        if(GNI_RC_INVALID_PARAM == gni_rc){
            rc = UCS_ERR_INVALID_PARAM;
        } else {
            rc = UCS_ERR_NO_MEMORY;
        }
        goto exit_lock;
    }

    ep_hash = (uint32_t)iface_addr->ep_hash;
    gni_rc = GNI_EpSetEventData(ep->super.ep, iface->domain_id, ep_hash);

    if(GNI_RC_SUCCESS != gni_rc){
        ucs_error("Could not set GNI_EpSetEventData!");
    }


 exit_lock:
    pthread_mutex_unlock(&uct_ugni_global_lock);

    return rc;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
uct_ugni_smsg_ep_am_common_send(uct_ugni_smsg_ep_t *ep, uct_ugni_smsg_iface_t *iface,
                                uint8_t am_id, unsigned header_length, void *header,
                                unsigned payload_length, void *payload, uct_ugni_smsg_desc_t *desc)
{
    gni_return_t gni_rc;

    desc->msg_id = iface->smsg_id++;
    desc->ep = &ep->super;

    gni_rc = GNI_SmsgSendWTag(ep->super.ep, header, header_length, payload, payload_length, desc->msg_id, am_id);

    if(GNI_RC_SUCCESS != gni_rc){
        ucs_mpool_put(desc);
        UCT_TL_IFACE_STAT_TX_NO_RES(&iface->super.super);
        return UCS_ERR_NO_RESOURCE;
    }

    ++ep->super.outstanding;
    ++iface->super.outstanding;

    sglib_hashed_uct_ugni_smsg_desc_t_add(iface->smsg_list, desc);

    return UCS_OK;
}

ucs_status_t uct_ugni_smsg_ep_am_short(uct_ep_h tl_ep, uint8_t id, uint64_t header,
                                       const void *payload, unsigned length)
{

    uct_ugni_smsg_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ugni_smsg_iface_t);
    uct_ugni_smsg_ep_t *ep = ucs_derived_of(tl_ep, uct_ugni_smsg_ep_t);
    uct_ugni_smsg_header_t *smsg_header;
    uint64_t *header_data;
    uct_ugni_smsg_desc_t *desc;
    ucs_status_t rc;

    UCT_CHECK_AM_ID(id);
    UCT_CHECK_LENGTH(length, iface->config.smsg_seg_size -
                     (sizeof(smsg_header) + sizeof(header)), "am_short");

    UCT_TL_IFACE_GET_TX_DESC(&iface->super.super, &iface->free_desc,
                             desc, return UCS_ERR_NO_RESOURCE);

    ucs_trace_data("AM_SHORT [%p] am_id: %d buf=%p length=%u",
                   iface, id, payload, length);

    smsg_header = (uct_ugni_smsg_header_t *)(desc+1);
    smsg_header->length = length + sizeof(header);

    header_data = (uint64_t*)(smsg_header+1);
    *header_data = header;
    memcpy((void*)(header_data+1), payload, length);

    uct_iface_trace_am(&iface->super.super, UCT_AM_TRACE_TYPE_SEND,
                       id, header_data, length, "TX: AM_SHORT");

    rc = uct_ugni_smsg_ep_am_common_send(ep, iface, id, sizeof(uct_ugni_smsg_header_t),
                                            smsg_header, smsg_header->length, (void*)header_data, desc);

    UCT_TL_EP_STAT_OP_IF_SUCCESS(rc, ucs_derived_of(tl_ep, uct_base_ep_t), AM, SHORT, sizeof(header) + length);

    return rc;
}

ssize_t uct_ugni_smsg_ep_am_bcopy(uct_ep_h tl_ep, uint8_t id,
                                  uct_pack_callback_t pack_cb,
                                  void *arg)
{
    uct_ugni_smsg_iface_t *iface = ucs_derived_of(tl_ep->iface, uct_ugni_smsg_iface_t);
    uct_ugni_smsg_ep_t *ep = ucs_derived_of(tl_ep, uct_ugni_smsg_ep_t);
    ssize_t packed;
    uct_ugni_smsg_desc_t *desc;
    ucs_status_t rc;
    void *smsg_data;
    uct_ugni_smsg_header_t *smsg_header;

    UCT_CHECK_AM_ID(id);

    UCT_TL_IFACE_GET_TX_DESC(&iface->super.super, &iface->free_desc,
                             desc, return UCS_ERR_NO_RESOURCE);

    ucs_trace_data("AM_BCOPY [%p] am_id: %d buf=%p",
                   iface, id, arg );

    smsg_header = (uct_ugni_smsg_header_t *)(desc+1);
    smsg_data = (void*)(smsg_header+1);

    packed = pack_cb(smsg_data, arg);

    smsg_header->length = packed;

    uct_iface_trace_am(&iface->super.super, UCT_AM_TRACE_TYPE_SEND,
                       id, smsg_data, packed, "TX: AM_BCOPY");

    rc = uct_ugni_smsg_ep_am_common_send(ep, iface, id, sizeof(uct_ugni_smsg_header_t),
                                         smsg_header, packed, smsg_data, desc);

    UCT_TL_EP_STAT_OP_IF_SUCCESS(rc, ucs_derived_of(ep, uct_base_ep_t), AM, BCOPY, packed);

    return (UCS_OK == rc) ? packed : rc;
}
