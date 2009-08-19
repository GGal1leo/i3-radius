/*
 * vim:ts=8:expandtab
 *
 * i3 - an improved dynamic tiling window manager
 *
 * © 2009 Michael Stapelberg and contributors
 *
 * See file LICENSE for license information.
 *
 * client.c: holds all client-specific functions
 *
 */
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <xcb/xcb.h>
#include <xcb/xcb_icccm.h>

#include "data.h"
#include "i3.h"
#include "xcb.h"
#include "util.h"
#include "queue.h"
#include "layout.h"
#include "client.h"

/*
 * Removes the given client from the container, either because it will be inserted into another
 * one or because it was unmapped
 *
 */
void client_remove_from_container(xcb_connection_t *conn, Client *client, Container *container, bool remove_from_focusstack) {
        CIRCLEQ_REMOVE(&(container->clients), client, clients);

        if (remove_from_focusstack)
                SLIST_REMOVE(&(container->workspace->focus_stack), client, Client, focus_clients);

        /* If the container will be empty now and is in stacking mode, we need to
           unmap the stack_win */
        if (CIRCLEQ_EMPTY(&(container->clients)) && container->mode == MODE_STACK) {
                LOG("Unmapping stack window\n");
                struct Stack_Window *stack_win = &(container->stack_win);
                stack_win->rect.height = 0;
                xcb_unmap_window(conn, stack_win->window);
                xcb_flush(conn);
        }
}

/*
 * Warps the pointer into the given client (in the middle of it, to be specific), therefore
 * selecting it
 *
 */
void client_warp_pointer_into(xcb_connection_t *conn, Client *client) {
        int mid_x = client->rect.width / 2,
            mid_y = client->rect.height / 2;
        xcb_warp_pointer(conn, XCB_NONE, client->child, 0, 0, 0, 0, mid_x, mid_y);
}

/*
 * Returns true if the client supports the given protocol atom (like WM_DELETE_WINDOW)
 *
 */
static bool client_supports_protocol(xcb_connection_t *conn, Client *client, xcb_atom_t atom) {
        xcb_get_property_cookie_t cookie;
        xcb_get_wm_protocols_reply_t protocols;
        bool result = false;

        cookie = xcb_get_wm_protocols_unchecked(conn, client->child, atoms[WM_PROTOCOLS]);
        if (xcb_get_wm_protocols_reply(conn, cookie, &protocols, NULL) != 1)
                return false;

        /* Check if the client’s protocols have the requested atom set */
        for (uint32_t i = 0; i < protocols.atoms_len; i++)
                if (protocols.atoms[i] == atom)
                        result = true;

        xcb_get_wm_protocols_reply_wipe(&protocols);

        return result;
}

/*
 * Kills the given window using WM_DELETE_WINDOW or xcb_kill_window
 *
 */
void client_kill(xcb_connection_t *conn, Client *window) {
        /* If the client does not support WM_DELETE_WINDOW, we kill it the hard way */
        if (!client_supports_protocol(conn, window, atoms[WM_DELETE_WINDOW])) {
                LOG("Killing window the hard way\n");
                xcb_kill_client(conn, window->child);
                return;
        }

        xcb_client_message_event_t ev;

        memset(&ev, 0, sizeof(xcb_client_message_event_t));

        ev.response_type = XCB_CLIENT_MESSAGE;
        ev.window = window->child;
        ev.type = atoms[WM_PROTOCOLS];
        ev.format = 32;
        ev.data.data32[0] = atoms[WM_DELETE_WINDOW];
        ev.data.data32[1] = XCB_CURRENT_TIME;

        LOG("Sending WM_DELETE to the client\n");
        xcb_send_event(conn, false, window->child, XCB_EVENT_MASK_NO_EVENT, (char*)&ev);
        xcb_flush(conn);
}

/*
 * Checks if the given window class and title match the given client
 * Window title is passed as "normal" string and as UCS-2 converted string for
 * matching _NET_WM_NAME capable clients as well as those using legacy hints.
 *
 */
bool client_matches_class_name(Client *client, char *to_class, char *to_title,
                               char *to_title_ucs, int to_title_ucs_len) {
        /* Check if the given class is part of the window class */
        if (client->window_class == NULL || strcasestr(client->window_class, to_class) == NULL)
                return false;

        /* If no title was given, we’re done */
        if (to_title == NULL)
                return true;

        if (client->name_len > -1) {
                /* UCS-2 converted window titles */
                if (client->name == NULL || memmem(client->name, (client->name_len * 2), to_title_ucs, (to_title_ucs_len * 2)) == NULL)
                        return false;
        } else {
                /* Legacy hints */
                if (client->name == NULL || strcasestr(client->name, to_title) == NULL)
                        return false;
        }

        return true;
}

/*
 * Enters fullscreen mode for the given client. This is called by toggle_fullscreen
 * and when moving a fullscreen client to another screen.
 *
 */
void client_enter_fullscreen(xcb_connection_t *conn, Client *client) {
        Workspace *workspace = client->workspace;

        if (workspace->fullscreen_client != NULL) {
                LOG("Not entering fullscreen mode, there already is a fullscreen client.\n");
                return;
        }

        client->fullscreen = true;
        workspace->fullscreen_client = client;
        LOG("Entering fullscreen mode...\n");
        /* We just entered fullscreen mode, let’s configure the window */
        uint32_t mask = XCB_CONFIG_WINDOW_X |
                        XCB_CONFIG_WINDOW_Y |
                        XCB_CONFIG_WINDOW_WIDTH |
                        XCB_CONFIG_WINDOW_HEIGHT;
        uint32_t values[4] = {workspace->rect.x,
                              workspace->rect.y,
                              workspace->rect.width,
                              workspace->rect.height};

        LOG("child itself will be at %dx%d with size %dx%d\n",
                        values[0], values[1], values[2], values[3]);

        xcb_configure_window(conn, client->frame, mask, values);

        /* Child’s coordinates are relative to the parent (=frame) */
        values[0] = 0;
        values[1] = 0;
        xcb_configure_window(conn, client->child, mask, values);

        /* Raise the window */
        values[0] = XCB_STACK_MODE_ABOVE;
        xcb_configure_window(conn, client->frame, XCB_CONFIG_WINDOW_STACK_MODE, values);

        Rect child_rect = workspace->rect;
        child_rect.x = child_rect.y = 0;
        fake_configure_notify(conn, child_rect, client->child);

        xcb_flush(conn);
}

/*
 * Toggles fullscreen mode for the given client. It updates the data structures and
 * reconfigures (= resizes/moves) the client and its frame to the full size of the
 * screen. When leaving fullscreen, re-rendering the layout is forced.
 *
 */
void client_toggle_fullscreen(xcb_connection_t *conn, Client *client) {
        /* dock clients cannot enter fullscreen mode */
        assert(!client->dock);

        Workspace *workspace = client->workspace;

        if (!client->fullscreen) {
                client_enter_fullscreen(conn, client);
                return;
        }

        LOG("leaving fullscreen mode\n");
        client->fullscreen = false;
        workspace->fullscreen_client = NULL;
        if (client_is_floating(client)) {
                /* For floating clients it’s enough if we just reconfigure that window (in fact,
                 * re-rendering the layout will not update the client.) */
                reposition_client(conn, client);
                resize_client(conn, client);
                /* redecorate_window flushes */
                redecorate_window(conn, client);
        } else {
                client_set_below_floating(conn, client);

                /* Because the coordinates of the window haven’t changed, it would not be
                   re-configured if we don’t set the following flag */
                client->force_reconfigure = true;
                /* We left fullscreen mode, redraw the whole layout to ensure enternotify events are disabled */
                render_layout(conn);
        }

        xcb_flush(conn);
}

/*
 * Sets the position of the given client in the X stack to the highest (tiling layer is always
 * on the same position, so this doesn’t matter) below the first floating client, so that
 * floating windows are always on top.
 *
 */
void client_set_below_floating(xcb_connection_t *conn, Client *client) {
        /* Ensure that it is below all floating clients */
        Client *first_floating = TAILQ_FIRST(&(client->workspace->floating_clients));
        if (first_floating != TAILQ_END(&(client->workspace->floating_clients))) {
                LOG("Setting below floating\n");
                uint32_t values[] = { first_floating->frame, XCB_STACK_MODE_BELOW };
                xcb_configure_window(conn, client->frame, XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, values);
        }
}

/*
 * Returns true if the client is floating. Makes the code more beatiful, as floating
 * is not simply a boolean, but also saves whether the user selected the current state
 * or whether it was automatically set.
 *
 */
bool client_is_floating(Client *client) {
        return (client->floating >= FLOATING_AUTO_ON);
}

/*
 * Change the border type for the given client to normal (n), 1px border (p) or
 * completely borderless (b).
 *
 */
void client_change_border(xcb_connection_t *conn, Client *client, char border_type) {
        switch (border_type) {
                case 'n':
                        LOG("Changing to normal border\n");
                        client->titlebar_position = TITLEBAR_TOP;
                        client->borderless = false;
                        break;
                case 'p':
                        LOG("Changing to 1px border\n");
                        client->titlebar_position = TITLEBAR_OFF;
                        client->borderless = false;
                        break;
                case 'b':
                        LOG("Changing to borderless\n");
                        client->titlebar_position = TITLEBAR_OFF;
                        client->borderless = true;
                        break;
                default:
                        LOG("Unknown border mode\n");
                        return;
        }

        /* Ensure that the child’s position inside our window gets updated */
        client->force_reconfigure = true;

        /* For clients inside a container, we can simply render the container.
         * If the client is floating, we need to render the whole layout */
        if (client->container != NULL)
                render_container(conn, client->container);
        else render_layout(conn);

        redecorate_window(conn, client);
}

/*
 * Unmap the client, correctly setting any state which is needed.
 *
 */
void client_unmap(xcb_connection_t *conn, Client *client) {
        /* Set WM_STATE_WITHDRAWN, it seems like Java apps need it */
        long data[] = { XCB_WM_STATE_WITHDRAWN, XCB_NONE };
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->child, atoms[WM_STATE], atoms[WM_STATE], 32, 2, data);

        xcb_unmap_window(conn, client->frame);
}

/*
 * Map the client, correctly restoring any state needed.
 *
 */
void client_map(xcb_connection_t *conn, Client *client) {
        /* Set WM_STATE_NORMAL because GTK applications don’t want to drag & drop if we don’t.
         * Also, xprop(1) needs that to work. */
        long data[] = { XCB_WM_STATE_NORMAL, XCB_NONE };
        xcb_change_property(conn, XCB_PROP_MODE_REPLACE, client->child, atoms[WM_STATE], atoms[WM_STATE], 32, 2, data);

        xcb_map_window(conn, client->frame);
}