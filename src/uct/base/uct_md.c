/**
* Copyright (C) Mellanox Technologies Ltd. 2001-2014.  ALL RIGHTS RESERVED.
* Copyright (C) UT-Battelle, LLC. 2015. ALL RIGHTS RESERVED.
* Copyright (C) ARM Ltd. 2016-2017. ALL RIGHTS RESERVED.
*
* See file LICENSE for terms.
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "uct_md.h"
#include "uct_iface.h"

#include <uct/api/uct.h>
#include <ucs/debug/log.h>
#include <ucs/debug/memtrack.h>
#include <ucs/type/class.h>
#include <ucs/sys/module.h>
#include <ucs/sys/string.h>
#include <ucs/arch/cpu.h>
#include <malloc.h>


UCS_LIST_HEAD(uct_md_components_list);

ucs_config_field_t uct_md_config_table[] = {

  {NULL}
};

ucs_config_field_t uct_md_config_rcache_table[] = {
    {"RCACHE_MEM_PRIO", "1000", "Registration cache memory event priority",
     ucs_offsetof(uct_md_rcache_config_t, event_prio), UCS_CONFIG_TYPE_UINT},

    {"RCACHE_OVERHEAD", "90ns", "Registration cache lookup overhead",
     ucs_offsetof(uct_md_rcache_config_t, overhead), UCS_CONFIG_TYPE_TIME},

    {"RCACHE_ADDR_ALIGN", UCS_PP_MAKE_STRING(UCS_SYS_CACHE_LINE_SIZE),
     "Registration cache address alignment, must be power of 2\n"
         "between "UCS_PP_MAKE_STRING(UCS_PGT_ADDR_ALIGN)"and system page size",
     ucs_offsetof(uct_md_rcache_config_t, alignment), UCS_CONFIG_TYPE_UINT},

    {NULL}
};

/**
 * Keeps information about allocated configuration structure, to be used when
 * releasing the options.
 */
typedef struct uct_config_bundle {
    ucs_config_field_t *table;
    const char         *table_prefix;
    char               data[];
} uct_config_bundle_t;


ucs_status_t uct_md_open(uct_component_h component, const char *md_name,
                         const uct_md_config_t *config, uct_md_h *md_p)
{
    ucs_status_t status;
    uct_md_h md;

    status = component->md_open(component, md_name, config, &md);
    if (status != UCS_OK) {
        return status;
    }

    *md_p = md;
    ucs_assert_always(md->component == component);
    return UCS_OK;
}

void uct_md_close(uct_md_h md)
{
    md->ops->close(md);
}

ucs_status_t uct_md_query_tl_resources(uct_md_h md,
                                       uct_tl_resource_desc_t **resources_p,
                                       unsigned *num_resources_p)
{
    uct_tl_resource_desc_t *resources, *tl_resources, *tmp;
    unsigned i, num_resources, num_tl_resources;
    uct_md_component_t *mdc = md->component;
    uct_md_registered_tl_t *tlr;
    uct_tl_component_t *tlc;
    ucs_status_t status;

    resources     = NULL;
    num_resources = 0;

    ucs_list_for_each(tlr, &mdc->tl_list, list) {
        tlc = tlr->tl;

        status = tlc->query_resources(md, &tl_resources, &num_tl_resources);
        if (status != UCS_OK) {
            ucs_debug("Failed to query %s resources: %s", tlc->name,
                      ucs_status_string(status));
            continue;
        }

        if (num_tl_resources == 0) {
            ucs_free(tl_resources);
            continue;
        }

        tmp = ucs_realloc(resources,
                          sizeof(*resources) * (num_resources + num_tl_resources),
                          "md_resources");
        if (tmp == NULL) {
            ucs_free(tl_resources);
            status = UCS_ERR_NO_MEMORY;
            goto err;
        }

        for (i = 0; i < num_tl_resources; ++i) {
            ucs_assert_always(!strcmp(tlc->name, tl_resources[i].tl_name));
        }
        resources = tmp;
        memcpy(resources + num_resources, tl_resources,
               sizeof(*tl_resources) * num_tl_resources);
        num_resources += num_tl_resources;
        ucs_free(tl_resources);
    }

    *resources_p     = resources;
    *num_resources_p = num_resources;
    return UCS_OK;

err:
    ucs_free(resources);
    return status;
}

void uct_release_tl_resource_list(uct_tl_resource_desc_t *resources)
{
    ucs_free(resources);
}

ucs_status_t
uct_md_query_single_md_resource(uct_component_t *component,
                                uct_md_resource_desc_t **resources_p,
                                unsigned *num_resources_p)
{
    uct_md_resource_desc_t *resource;

    resource = ucs_malloc(sizeof(*resource), "md resource");
    if (resource == NULL) {
        return UCS_ERR_NO_MEMORY;
    }

    ucs_snprintf_zero(resource->md_name, UCT_MD_NAME_MAX, "%s",
                      component->name);

    *resources_p = resource;
    *num_resources_p = 1;
    return UCS_OK;
}

ucs_status_t
uct_md_query_empty_md_resource(uct_md_resource_desc_t **resources_p,
                               unsigned *num_resources_p)
{
    *resources_p     = NULL;
    *num_resources_p = 0;
    return UCS_OK;
}

ucs_status_t uct_md_stub_rkey_unpack(uct_md_component_t *mdc,
                                     const void *rkey_buffer, uct_rkey_t *rkey_p,
                                     void **handle_p)
{
    *rkey_p   = 0xdeadbeef;
    *handle_p = NULL;
    return UCS_OK;
}

static ucs_status_t uct_config_read(uct_config_bundle_t **bundle,
                                    ucs_config_field_t *config_table,
                                    size_t config_size, const char *env_prefix,
                                    const char *cfg_prefix)
{
    uct_config_bundle_t *config_bundle;
    ucs_status_t status;

    config_bundle = ucs_calloc(1, sizeof(*config_bundle) + config_size, "uct_config");
    if (config_bundle == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err;
    }

    /* TODO use env_prefix */
    status = ucs_config_parser_fill_opts(config_bundle->data, config_table,
                                         env_prefix, cfg_prefix, 0);
    if (status != UCS_OK) {
        goto err_free_bundle;
    }

    config_bundle->table = config_table;
    config_bundle->table_prefix = ucs_strdup(cfg_prefix, "uct_config");
    if (config_bundle->table_prefix == NULL) {
        status = UCS_ERR_NO_MEMORY;
        goto err_free_bundle;
    }

    *bundle = config_bundle;
    return UCS_OK;

err_free_bundle:
    ucs_free(config_bundle);
err:
    return status;
}

uct_tl_component_t *uct_find_tl_on_md(uct_md_component_t *mdc,
                                      uint64_t md_flags,
                                      const char *tl_name)
{
    uct_md_registered_tl_t *tlr;

    ucs_list_for_each(tlr, &mdc->tl_list, list) {
        if (((tl_name != NULL) && !strcmp(tl_name, tlr->tl->name)) ||
            ((tl_name == NULL) && (md_flags & UCT_MD_FLAG_SOCKADDR))) {
            return tlr->tl;
        }
    }
    return NULL;
}

ucs_status_t uct_md_iface_config_read(uct_md_h md, const char *tl_name,
                                      const char *env_prefix, const char *filename,
                                      uct_iface_config_t **config_p)
{
    uct_config_bundle_t *bundle = NULL;
    uct_tl_component_t *tlc;
    uct_md_attr_t md_attr;
    ucs_status_t status;

    status = uct_md_query(md, &md_attr);
    if (status != UCS_OK) {
        ucs_error("Failed to query MD");
        return status;
    }

    tlc = uct_find_tl_on_md(md->component, md_attr.cap.flags, tl_name);
    if (tlc == NULL) {
        if (tl_name == NULL) {
            ucs_error("There is no sockaddr transport registered on the md");
        } else {
            ucs_error("Transport '%s' does not exist", tl_name);
        }
        status = UCS_ERR_NO_DEVICE; /* Non-existing transport */
        return status;
    }

    status = uct_config_read(&bundle, tlc->iface_config_table,
                             tlc->iface_config_size, env_prefix, tlc->cfg_prefix);
    if (status != UCS_OK) {
        ucs_error("Failed to read iface config");
        return status;
    }

    *config_p = (uct_iface_config_t*) bundle->data;
    /* coverity[leaked_storage] */
    return UCS_OK;
}

ucs_status_t uct_iface_open(uct_md_h md, uct_worker_h worker,
                            const uct_iface_params_t *params,
                            const uct_iface_config_t *config,
                            uct_iface_h *iface_p)
{
    uct_tl_component_t *tlc;
    uct_md_attr_t md_attr;
    ucs_status_t status;

    status = uct_md_query(md, &md_attr);
    if (status != UCS_OK) {
        ucs_error("Failed to query MD");
        return status;
    }

    UCT_CHECK_PARAM(params->field_mask & UCT_IFACE_PARAM_FIELD_OPEN_MODE,
                    "UCT_IFACE_PARAM_FIELD_OPEN_MODE is not defined");

    if (params->open_mode & UCT_IFACE_OPEN_MODE_DEVICE) {
        tlc = uct_find_tl_on_md(md->component, md_attr.cap.flags, params->mode.device.tl_name);
    } else if ((params->open_mode & UCT_IFACE_OPEN_MODE_SOCKADDR_CLIENT) ||
               (params->open_mode & UCT_IFACE_OPEN_MODE_SOCKADDR_SERVER)) {
        tlc = uct_find_tl_on_md(md->component, md_attr.cap.flags, NULL);
    } else {
        ucs_error("Invalid open mode %zu", params->open_mode);
        return status;
    }

    if (tlc == NULL) {
        /* Non-existing transport */
        return UCS_ERR_NO_DEVICE;
    }

    return tlc->iface_open(md, worker, params, config, iface_p);
}

ucs_status_t uct_md_config_read(uct_component_h component,
                                const char *env_prefix, const char *filename,
                                uct_md_config_t **config_p)
{
    uct_config_bundle_t *bundle = NULL;
    ucs_status_t status;

    status = uct_config_read(&bundle, component->md_config_table,
                             component->md_config_size, env_prefix,
                             component->cfg_prefix);
    if (status != UCS_OK) {
        ucs_error("Failed to read MD config");
        return status;
    }

    *config_p = (uct_md_config_t*) bundle->data;
    /* coverity[leaked_storage] */
    return UCS_OK;
}

void uct_config_release(void *config)
{
    uct_config_bundle_t *bundle = (uct_config_bundle_t *)config - 1;

    ucs_config_parser_release_opts(config, bundle->table);
    ucs_free((void*)(bundle->table_prefix));
    ucs_free(bundle);
}

ucs_status_t uct_config_get(void *config, const char *name, char *value,
                            size_t max)
{
    uct_config_bundle_t *bundle = (uct_config_bundle_t *)config - 1;
    return ucs_config_parser_get_value(bundle->data, bundle->table, name, value,
                                       max);
}

ucs_status_t uct_config_modify(void *config, const char *name, const char *value)
{
    uct_config_bundle_t *bundle = (uct_config_bundle_t *)config - 1;
    return ucs_config_parser_set_value(bundle->data, bundle->table, name, value);
}

ucs_status_t uct_md_mkey_pack(uct_md_h md, uct_mem_h memh, void *rkey_buffer)
{
    void *rbuf = uct_md_fill_md_name(md, rkey_buffer);
    return md->ops->mkey_pack(md, memh, rbuf);
}

ucs_status_t uct_rkey_unpack(uct_component_h component, const void *rkey_buffer,
                             uct_rkey_bundle_t *rkey_ob)
{
    char mdc_name[UCT_MD_COMPONENT_NAME_MAX + 1];

    if (ENABLE_DEBUG_DATA) {
        if (ENABLE_PARAMS_CHECK &&
            strncmp(rkey_buffer, component->name, UCT_MD_COMPONENT_NAME_MAX)) {
            ucs_snprintf_zero(mdc_name, sizeof(mdc_name), "%s",
                              (const char*)rkey_buffer);
            ucs_error("invalid component for rkey unpack; "
                      "expected: %s, actual: %s", mdc_name, component->name);
            return UCS_ERR_INVALID_PARAM;
        }

        rkey_buffer += UCT_MD_COMPONENT_NAME_MAX;
    }

    return component->rkey_unpack(component, rkey_buffer, &rkey_ob->rkey,
                                  &rkey_ob->handle);
}

ucs_status_t uct_rkey_ptr(uct_component_h component, uct_rkey_bundle_t *rkey_ob,
                          uint64_t remote_addr, void **local_addr_p)
{
    return component->rkey_ptr(component, rkey_ob->rkey, rkey_ob->handle,
                               remote_addr, local_addr_p);
}

ucs_status_t uct_rkey_release(uct_component_h component,
                              const uct_rkey_bundle_t *rkey_ob)
{
    return component->rkey_release(component, rkey_ob->rkey, rkey_ob->handle);
}

ucs_status_t uct_md_query(uct_md_h md, uct_md_attr_t *md_attr)
{
    ucs_status_t status;

    status = md->ops->query(md, md_attr);
    if (status != UCS_OK) {
        return status;
    }

    /* MD component name + data */
    memcpy(md_attr->component_name, md->component->name, UCT_MD_COMPONENT_NAME_MAX);

#if ENABLE_DEBUG_DATA
    /* MD name is packed into rkey in DEBUG mode only */
    md_attr->rkey_packed_size += UCT_MD_COMPONENT_NAME_MAX;
#endif

    return UCS_OK;
}

static ucs_status_t uct_mem_check_flags(unsigned flags)
{
    if (!(flags & UCT_MD_MEM_ACCESS_ALL)) {
        return UCS_ERR_INVALID_PARAM;
    }
    return UCS_OK;
}

ucs_status_t uct_md_mem_alloc(uct_md_h md, size_t *length_p, void **address_p,
                              unsigned flags, const char *alloc_name, uct_mem_h *memh_p)
{
    ucs_status_t status;

    status = uct_mem_check_flags(flags);
    if (status != UCS_OK) {
        return status;
    }

    return md->ops->mem_alloc(md, length_p, address_p, flags, alloc_name, memh_p);
}

ucs_status_t uct_md_mem_free(uct_md_h md, uct_mem_h memh)
{
    return md->ops->mem_free(md, memh);
}

ucs_status_t 
uct_md_mem_advise(uct_md_h md, uct_mem_h memh, void *addr, size_t length,
                  unsigned advice)
{
    if ((length == 0) || (addr == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    return md->ops->mem_advise(md, memh, addr, length, advice);
}

ucs_status_t uct_md_mem_reg(uct_md_h md, void *address, size_t length,
                            unsigned flags, uct_mem_h *memh_p)
{
    ucs_status_t status;

    if ((length == 0) || (address == NULL)) {
        return UCS_ERR_INVALID_PARAM;
    }

    status = uct_mem_check_flags(flags);
    if (status != UCS_OK) {
        return status;
    }

    return md->ops->mem_reg(md, address, length, flags, memh_p);
}

ucs_status_t uct_md_mem_dereg(uct_md_h md, uct_mem_h memh)
{
    return md->ops->mem_dereg(md, memh);
}

int uct_md_is_sockaddr_accessible(uct_md_h md, const ucs_sock_addr_t *sockaddr,
                                  uct_sockaddr_accessibility_t mode)
{
    return md->ops->is_sockaddr_accessible(md, sockaddr, mode);
}

ucs_status_t uct_md_detect_memory_type(uct_md_h md, void *addr, size_t length,
                                       uct_memory_type_t *mem_type_p)
{
    return md->ops->detect_memory_type(md, addr, length, mem_type_p);
}

int uct_md_is_hugetlb(uct_md_h md, uct_mem_h memh)
{
    if (md->ops->is_hugetlb != NULL) {
        return md->ops->is_hugetlb(md, memh);
    }
    return 0;
}
