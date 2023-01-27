/*
 * Copyright (c) 2011 Citrix Systems, Inc.
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

#ifndef __XENBACKEND_H__
# define __XENBACKEND_H__

# ifdef __cplusplus
extern "C"
{
# endif
    /*
     * Internal structures.
     */
    struct xen_device;
    struct xen_backend;
    typedef struct xen_backend *xen_backend_t;

    /*
     * Generic type passed to callbacks.
     */
    typedef void *backend_private_t;

    /*
     * Opaque type to be used by libxenbackend caller to represent a device
     * handled by the library.
     * This type will be allocated and released by the alloc() and free()
     * callbacks respectively.
     * This is to be the main interface for the caller to implement the device
     * this backend should manage. It is passed as an argument to every
     * callback registered.
     */
    typedef void *xen_device_t;

    /*
     * Callbacks to register in order to have a device handled by libxenbackend.
     * - alloc()
     *   @param backend xen_backend_t object.
     *   @param devid   device identifier.
     *   @param priv    opaque pointer to a generic type passed to
     *                  backend_init() in the first place.
     *   @return On success, a pointer to an allocated xen_device_t object.
     *           Otherwise, NULL.
     *
     *   Perform the allocation and initialisation required for the xen_device_t.
     *   Called when libxenbackend notices the device's node in xenstore has
     *   been created (usualy immediately during backend_register(), because
     *   the nodes already existed, otherwise during
     *   backend_xenstore_handler()).
     *
     * - init()
     *   @param xendev  xen_device_t object.
     *   @return On success 0, otherwise any non-nul value (e.g. -errno).
     *
     *   Perform any initialisation the xen_device_t object should require
     *   before the backend tries to connect to the frontend.
     *   Called when the backend is ready but has not attempted to be connect
     *   to the frontend (backend in "Initialising" state).
     *
     * - connect()
     *   @param xendev  xen_device_t object.
     *   @return On success 0, otherwise any non-nul value (e.g. -errno).
     *
     *   Perform any remaining operation required for the backend to connect to
     *   the frontend (frontend should be in "Initialised" state, some might
     *   even switch directly to "Connected"). This is usually when
     *   event-channels are bound.
     *   Called when the backend is waiting for the frontend to complete the
     *   connection (backend in "Init. Wait") state.
     *
     * - disconnect()
     *   @param xendev  xen_device_t object.
     *
     *   Perform the relevant operation for the xen_device_t when the frontend
     *   closes. This is usually when a xendev needs to unmap shared pages and
     *   rings, and unbinding event-channels.
     *   Called when libxenbackend notifies a frontend disconnected or trying
     *   to disconnect (frontend in "Closing" state or already "Closed").
     *
     * - backend_changed()
     *   @param xendev  xen_device_t object.
     *   @param node    string representing the absolute node path that changed
     *                  in xenstore.
     *   @param val     the value of the node that changed.
     *
     *   Notification that the device backend node or one of its sub-nodes
     *   changed.
     *   This callback is optional.
     *
     * - frontend_changed()
     *   @param xendev  xen_device_t object.
     *   @param node    string representing the absolute node path that changed
     *                  in xenstore.
     *   @param val     the value of the node that changed.
     *
     *   Notification that the device backend node or one of its sub-nodes
     *   changed.
     *   This callback is optional.
     *
     * - event()
     *   @param xendev  xen_device_t object.
     *
     *   Notification that an event was received on the bound event-channel for
     *   that device.
     *
     * - free()
     *   @param xendev  xen_device_t object.
     *
     *   Release any resource allocated during alloc().
     */
    struct xen_backend_ops
    {
        xen_device_t    (*alloc)            (xen_backend_t backend,
                                             int devid,
                                             backend_private_t priv);
        int             (*init)             (xen_device_t xendev);
        int             (*connect)          (xen_device_t xendev);
        void            (*disconnect)       (xen_device_t xendev);
        void            (*backend_changed)  (xen_device_t xendev,
                                             const char *node,
                                             const char *val);
        void            (*frontend_changed) (xen_device_t xendev,
                                             const char *node,
                                             const char *val);
        void            (*event)            (xen_device_t xendev);
        void            (*free)             (xen_device_t xendev);
    };

/*
 * Print functions.
 */
int backend_print(xen_backend_t xenback, int devid, const char *node, const char *fmt, ...);

int backend_scan(xen_backend_t xenback, int devid, const char *node, const char *fmt, ...);
int frontend_scan(xen_backend_t xenback, int devid, const char *node, const char *fmt, ...);

/** Backend methods. **/
/**
 * Initialise the file descriptors required to interact with Xen toolstack
 * component: xenstore, xenctrl, grant-tables, and retrieve the domain path.
 * backend_close() is expected to be called to release resources.
 *
 * @param backend_domid domid of the domain where the backend is handled.
 *
 * @return 0 on success, else -1 and pass on /errno/.
 */
int backend_init(int backend_domid);

/**
 * Initialise the file descriptors required to interact with Xen toolstack
 * component: xenstore, grant-tables, and retrieve the domain path.
 * backend_close() is expected to be called to release resources.
 *
 * Notably this doesn't use xenctrl.
 *
 * @param backend_domid domid of the domain where the backend is handled.
 *
 * @return 0 on success, else -1 and pass on /errno/.
 */
int backend_init_noxc(int backend_domid);

/**
 * Close file descriptors with Xen toolstack components: xenstore, xenctrl,
 * grant-tables.
 *
 * @return 0 //TODO: Useless, should be void.
 */
int backend_close(void);

/**
 * Allocate a /xen_backend_t/ object with /ops/ callbacks.
 * That object will setup a xenstore watch on
 * /<domain_path>/backend/<backend-type>/<domid> and call the appropriate
 * callbacks on actions taken upon this node and its sub-nodes.
 * backend_release() needs to be called to remove the watch and release resources.
 *
 * @param type  string representing the type of this backend (e.g. "vfb",
 *              "vbd", "vkbd", etc).
 * @param domid domid of the domain where this sets the backend.
 * @param ops   structure with the required callbacks to manage the backend
 *              (see xen_backend_ops above).
 * @param priv  private generic pointer to be passed to the /xen_backend_ops/
 *              alloc() callback if required.
 * @return An pointer to an allocated xen_backend_t object on success, else
 *         NULL and pass on /errno/.
 */
xen_backend_t backend_register(const char *type, int domid, struct
                               xen_backend_ops *ops, backend_private_t priv);

/**
 * Release resources reserved by a /xen_backend_t/ object.
 * Remove the xenstore watch on the backend xenstore node, calls /disconnect/
 * and /free/ callbacks on any allocated device for that backend before
 * releasing any resource reserved for that device, then release the memory
 * allocated for the /xen_backend_t/ object.
 *
 * @param xenback   /xen_backend_t/ object.
 */
void backend_release(xen_backend_t xenback);

/*
 * Helpers provided to help interface libxenbackend with libevent.
 */
/**
 * libxenbackend xenstore handler (e.g. libevent callback).
 * This handler provides the necessary logic for libxenbackend when the
 * monitored xenstore file-descriptor becomes "ready".
 * This handler will IMPLICITELY use the xenstore file-descriptor reserved by
 * backend_init(), and will expect that function to have been called prior it
 * is executed. Behaviour is otherwise undefined.
 *
 * @param unused    dummy pointer (NULL is recommended), to match libevent
 *                  callback definition.
 */
void backend_xenstore_handler(void *unused);
/**
 * Return the xenstore file-descriptor reserved by backend_init().
 * libxenbackend caller is responsible for monitoring the xenstore
 * file-descriptor and needs to call backend_xenstore_handler() when it is
 * "ready".
 * @return A file-descriptor opened by backend_init() on xenstore.
 */
int backend_xenstore_fd(void);

/*
 * Event-channel manipulation.
 */
/**
 * Read the xenstore frontend for the device referenced by /devid/ to find the
 * remote port, bind (see xc_evtchn_bind_interdomain()) an event-channel on it
 * and return a file-descriptor for that event-channel.
 * backend_unbind_evtchn() is expected to be called to unbind the event-channel.
 *
 * TODO: Recent versions of Xen only offer xc_evtchn_bind_interdomain() for
 * retro-compatibiliy, the same feature is now exposed through /dev/xen/evtchn
 * devfs.
 *
 * @param xenback   xen_backend_t object.
 * @param devid     identifier for the device exposing the event-channel.
 *
 * @return A file-descriptor opened for the bound event-channel on success.
 *         Otherwise it shall return -1 and pass on /errno/.
 */
int backend_bind_evtchn(xen_backend_t xenback, int devid);

/**
 * Unbind the event-channel bound through backend_bind_evtchn().
 * @param xenback   xen_backend_t object.
 * @param devid     identifier for the device owning the event-channel.
 */
void backend_unbind_evtchn(xen_backend_t xenback, int devid);

/**
 * Send an event to the event-channel of the device /devid/.
 * Use xen_backend_t object to send an event on the event-channel bound for
 * that device on its local port through xc_evtchn_notify().
 *
 * TODO: Recent versions of Xen only offer xc_evtchn_notify () for
 * retro-compatibiliy, the same feature is now exposed through /dev/xen/evtchn
 * devfs.
 *
 * @param xenback   xen_backend_t object.
 * @param devid     identifier for the device owning the event-channel.
 *
 * @return 0 on success, -1 otherwise and set /errno/.
 */
int backend_evtchn_notify(xen_backend_t xenback, int devid);

/*
 * Helpers provided to help interface libxenbackend with libevent.
 */
/**
 * Return a pointer to an internal structure for backend_evtchn_handler().
 * This is used to pass the required event-channel related internal structure
 * to backend_evtchn_handler() when it is registered in libevent.
 *
 * @return a pointer to an internal structure.
 */
void *backend_evtchn_priv(xen_backend_t xenback, int devid);
/**
 * libxenbackend event-channel handler (e.g. libevent callback).
 * This handler provides the necessary logic for libxenbackend when the
 * monitored event-channel file-descriptor becomes "ready".
 * This handler will use private structure created by backend_bind_evtchn() and
 * expects to have that structure passed as an opaque agrument through /priv/.
 * This opaque pointer is returned by backend_evtchn_priv().
 * General xen_backend_t callback /event()/ will be called via this handler.
 *
 * @param priv  opaque pointer returned by backend_evtchn_priv().
 */
void backend_evtchn_handler(void *priv);

/*
 * Shared-page helpers.
 */
/**
 * Map a shared page advertised by the device frontend xenstore nodes, if it
 * exists.
 * Search for the "page-ref" xenstore node of the frontend and map the page
 * through xc_map_foreign_range(), if this node exists.
 * backend_unmap_shared_page() is expected to be called to unmap the
 * shared-page.
 *
 * @param xenback   xen_backend_t object.
 * @param devid     identifier for the device of which the frontend is expected
 *                  to share a page.
 * @return On success, a pointer to the mapped page. Otherwise, NULL and pass
 *         on /errno/.
 */
void *backend_map_shared_page(xen_backend_t xenback, int devid);
/**
 * Unmap a shared page previously mapped by backend_map_shared_page().
 * (munmap())
 *
 * @param xenback   xen_backend_t object.
 * @param devid     identifier for the device of which the frontend is expected
 *                  to share a page.
 * @return On success, 0. Otherwise, -1 and pass on /errno/ from munmap().
 */
int backend_unmap_shared_page(xen_backend_t xenback, int devid, void *page);

/*
 * Grant-table ring helpers.
 */
/**
 * Map a grant-table ring advertised by the device frontend xenstore nodes, if
 * it exists.
 * Search for the "ring-ref" xenstore node of the frontend and map the ring
 * through xc_gnttab_map_grant_ref(), if this node exists.
 * backend_unmap_granted_ring() is expected to be called to unmap the ring.
 *
 * @param xenback   xen_backend_t object.
 * @param devid     identifier for the device of which the frontend is expected
 *                  to share a ring.
 * @return On success, a pointer to the mapped ring. Otherwise, NULL and pass
 *         on /errno/.
 */
void *backend_map_granted_ring(xen_backend_t xenback, int devid);
/**
 * Unmap a grant ring previously mapped by backend_map_granted_ring().
 * (xc_gnttab_munmap())
 *
 * @param xenback   xen_backend_t object.
 * @param devid     identifier for the device of which the frontend is expected
 *                  to share a ring.
 * @return On success, 0. Otherwise, -1 and pass on /errno/.
 */
int backend_unmap_granted_ring(xen_backend_t xenback, int devid, void *page);

#ifdef __cplusplus
}
#endif

#endif /* __XENBACKEND_H__ */
