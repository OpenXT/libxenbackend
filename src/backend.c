/*
 * Copyright (c) 2013 Citrix Systems, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "project.h"
#include "backend.h"

static xc_interface *xc_handle = NULL;
static xc_gnttab *xcg_handle = NULL;
struct xs_handle *xs_handle = NULL;
static char domain_path[PATH_BUFSZ];
static int domain_path_len = 0;

int backend_init_noxc(int backend_domid)
{
    xc_handle = NULL;
    char *tmp;

    xs_handle = xs_open(XS_UNWATCH_FILTER);
    if (!xs_handle)
        goto fail_xs;

    xcg_handle = xc_gnttab_open(NULL, 0);
    if (!xcg_handle)
        goto fail_xcg;

    tmp = xs_get_domain_path(xs_handle, backend_domid);
    if (!tmp)
        goto fail_domainpath;

    domain_path_len = snprintf(domain_path, PATH_BUFSZ, "%s", tmp);
    free(tmp);

    return 0;
fail_domainpath:
    xc_gnttab_close(xcg_handle);
    xcg_handle = NULL;
fail_xcg:
    xs_daemon_close(xs_handle);
    xs_handle = NULL;
fail_xs:
    return -1;
}

int backend_init(int backend_domid)
{
    int ret = backend_init_noxc(backend_domid);
    if (ret)
        return ret;

    xc_handle = xc_interface_open(NULL, NULL, 0);
    if (xc_handle)
        return 0;

    xc_interface_close(xc_handle);
    xc_handle = NULL;

    return -1;
}

/* Close all file descriptors opened by backend_init */
int backend_close(void)
{
    if (xs_handle)
        xs_daemon_close(xs_handle);
    xs_handle = NULL;

    if (xc_handle)
        xc_interface_close(xc_handle);
    xc_handle = NULL;

    if (xcg_handle)
        xc_gnttab_close(xcg_handle);
    xcg_handle = NULL;

    return 0;
}

static int setup_watch(struct xen_backend *xenback, const char *type, int domid)
{
    int sz;

    sz = snprintf(xenback->token, TOKEN_BUFSZ, MAGIC_STRING"%p", xenback);
    if (sz < 0 || sz >= TOKEN_BUFSZ)
        return -1;

    sz = snprintf(xenback->path, PATH_BUFSZ, "%s/backend/%s/%d",
                  domain_path, type, domid);
    if (sz < 0 || sz >= PATH_BUFSZ)
        return -1;
    xenback->path_len = sz;

    if (!xs_watch(xs_handle, xenback->path, xenback->token)) {
        return -1;
    }

    return 0;
}

static void free_device(struct xen_backend *xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];

    if (xenback->ops->disconnect)
        xenback->ops->disconnect(xendev->dev);

    if (xenback->ops->free)
        xenback->ops->free(xendev->dev);

    if (xendev->be) {
        free(xendev->be);
        xendev->be = NULL;
    }

    if (xendev->fe) {
        char token[TOKEN_BUFSZ];

        snprintf(token, TOKEN_BUFSZ, MAGIC_STRING"%p", xendev);
        xs_unwatch(xs_handle, xendev->fe, token);
        free(xendev->fe);
        xendev->fe = NULL;
    }

    if (xendev->evtchndev) {
        xc_evtchn_close(xendev->evtchndev);
    }

    if (xendev->protocol)
        free(xendev->protocol);

    xendev->dev = NULL;
}

static struct xen_device *alloc_device(struct xen_backend *xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];

    xendev->backend = xenback;
    xendev->local_port = -1;

    xendev->be = calloc(1, PATH_BUFSZ);
    if (!xendev->be)
        return NULL;
    snprintf(xendev->be, PATH_BUFSZ, "%s/%d", xenback->path, devid);

    xendev->evtchndev = xc_evtchn_open(NULL, 0);
    if (xendev->evtchndev) {
        fcntl(xc_evtchn_fd(xendev->evtchndev), F_SETFD, FD_CLOEXEC);
    }

    if (xenback->ops->alloc)
        xendev->dev = xenback->ops->alloc(xenback, devid, xenback->priv);

    return xendev;
}

static void scan_devices(struct xen_backend *xenback)
{
    char **dirent;
    unsigned int len, i;
    int scanned[BACKEND_DEVICE_MAX];

    memset(scanned, 0, sizeof (scanned));

    dirent = xs_directory(xs_handle, 0, xenback->path, &len);
    if (dirent) {
        for (i = 0; i < len; i++) {
            int rc;
            int devid;
            struct xen_device *xendev;

            rc = sscanf(dirent[i], "%d", &devid);
            if (rc != 1)
                continue;

            scanned[devid] = 1;
            xendev = &xenback->devices[devid];

            if (xendev->dev != NULL)
                continue;

            xendev = alloc_device(xenback, devid);

            check_state_early(xendev);
            check_state(xendev);
        }
        free(dirent);
    } else if (errno == ENOENT) {
        /* all devices removed */
    } else {
        /* other error */
        return;
    }

    /* Detect devices removed from xenstore */
    for (i = 0; i < BACKEND_DEVICE_MAX; i++) {
        if (xenback->devices[i].dev && scanned[i] == 0)
            free_device(xenback, i);
    }
}

xen_backend_t backend_register(const char *type, int domid, struct
                               xen_backend_ops *ops, backend_private_t priv)
{
    struct xen_backend *xenback;
    int rc;

    xenback = calloc(1, sizeof (*xenback));
    if (xenback == NULL)
        return NULL;

    xenback->ops = ops;
    xenback->domid = domid;
    xenback->type = type;
    xenback->priv = priv;

    rc = setup_watch(xenback, type, domid);
    if (rc) {
        free(xenback);
        return NULL;
    }

    scan_devices(xenback);

    return xenback;
}

void backend_release(xen_backend_t xenback)
{
    int i;

    xs_unwatch(xs_handle, xenback->path, xenback->token);

    for (i = 0; i < BACKEND_DEVICE_MAX; i++) {
        if (xenback->devices[i].dev) {
            free_device(xenback, i);
        }
    }

    free(xenback);
}

static int get_devid_from_path(struct xen_backend *xenback, char *path)
{
    int devid;
    int rc;
    char dummy[PATH_BUFSZ];

    rc = sscanf(path + xenback->path_len, "/%d/%255s", &devid, dummy);
    if (rc == 2)
        return devid;

    rc = sscanf(path + xenback->path_len, "/%d", &devid);
    if (rc == 1)
        return devid;

    return -1;
}

static char *get_node_from_path(char *base, char *path)
{
    int len = strlen(base);

    if (strncmp(base, path, len))
        return NULL;
    if (path[len] != '/')
        return NULL;

    return path + len + 1;
}

static void update_device(struct xen_backend *xenback, int devid, char *path)
{
    struct xen_device *xendev = &xenback->devices[devid];
    char *node = NULL;

    if (xendev->dev == NULL)
        return;

    if (xendev->be)
        node = get_node_from_path(xendev->be, path);
    backend_changed(xendev, node);
    check_state(xendev);
}

static void update_frontend(struct xen_device *xendev, char *node)
{
    frontend_changed(xendev, node);
    check_state(xendev);
}

void backend_xenstore_handler(void *unused)
{
    char **w;
    (void)unused;

    while (w = xs_check_watch(xs_handle)) {
        void *p;

        if (sscanf(w[XS_WATCH_TOKEN], MAGIC_STRING"%p", &p) == 1) {
            if (!strncmp(w[XS_WATCH_PATH], domain_path, domain_path_len)) {
                int devid;
                struct xen_backend *xenback = p;

                devid = get_devid_from_path(xenback, w[XS_WATCH_PATH]);
                if (devid != -1) {
                    update_device(xenback, devid, w[XS_WATCH_PATH]);
                }
                scan_devices(xenback);
            } else {
                struct xen_device *xendev = p;

                /*
                ** Ensure that the xenstore handler has not been called *after*
                ** unwatching the node. (yes, yes, it happens...)
                */
                if (xendev->dev) {
                    char *node;

                    node = get_node_from_path(xendev->fe, w[XS_WATCH_PATH]);
                    update_frontend(xendev, node);
                }
            }
        }
        free(w);
    }
}

int backend_xenstore_fd(void)
{
    return xs_fileno(xs_handle);
}

int backend_bind_evtchn(xen_backend_t xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];
    int remote_port;
    int rc;

    rc = xs_read_fe_int(xendev, "event-channel", &remote_port);
    if (rc)
        return -1;

    if (xendev->local_port != -1)
        return -1;

    xendev->local_port = xc_evtchn_bind_interdomain(xendev->evtchndev,
                                                    xenback->domid,
                                                    remote_port);
    if (xendev->local_port == -1)
        return -1;

    return xc_evtchn_fd(xendev->evtchndev);
}

void backend_unbind_evtchn(xen_backend_t xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];

    if (xendev->local_port == -1)
        return;

    xc_evtchn_unbind(xendev->evtchndev, xendev->local_port);
    xendev->local_port = -1;
}

int backend_evtchn_notify(xen_backend_t xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];

    return xc_evtchn_notify(xendev->evtchndev, xendev->local_port);
}

void *backend_evtchn_priv(xen_backend_t xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];

    return xendev;
}

void backend_evtchn_handler(void *priv)
{
    struct xen_device *xendev = priv;
    struct xen_backend *xenback = xendev->backend;
    int port;

    port = xc_evtchn_pending(xendev->evtchndev);
    if (port != xendev->local_port)
        return;
    xc_evtchn_unmask(xendev->evtchndev, port);

    if (xenback->ops->event)
        xenback->ops->event(xendev->dev);
}

void *backend_map_shared_page(xen_backend_t xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];
    int mfn;
    int rc;

    if (xc_handle == NULL)
        return NULL;

    rc = xs_read_fe_int(xendev, "page-ref", &mfn);
    if (rc)
        return NULL;

    return xc_map_foreign_range(xc_handle, xenback->domid,
                                XC_PAGE_SIZE, PROT_READ | PROT_WRITE,
                                mfn);
}

void *backend_map_granted_ring(xen_backend_t xenback, int devid)
{
    struct xen_device *xendev = &xenback->devices[devid];
    int ring;
    int rc;

    rc = xs_read_fe_int(xendev, "page-gref", &ring);
    if (rc) {
        rc = xs_read_fe_int(xendev, "ring-ref", &ring);
        if (rc)
            return NULL;
    }

    return xc_gnttab_map_grant_ref(xcg_handle, xenback->domid,
                                   ring, PROT_READ | PROT_WRITE);
}

int backend_unmap_shared_page(xen_backend_t xenback, int devid, void *page)
{
    (void)xenback;
    (void)devid;

    return munmap(page, XC_PAGE_SIZE);
}

int backend_unmap_granted_ring(xen_backend_t xenback, int devid, void *page)
{
    (void)xenback;
    (void)devid;

    return xc_gnttab_munmap(xcg_handle, page, 1);
}
