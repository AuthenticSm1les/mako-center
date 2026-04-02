#ifndef MAKO_HISTORY_H
#define MAKO_HISTORY_H

#include <stdbool.h>
#include <stdint.h>

#if defined(HAVE_LIBSYSTEMD)
#include <systemd/sd-bus.h>
#elif defined(HAVE_LIBELOGIND)
#include <elogind/sd-bus.h>
#elif defined(HAVE_BASU)
#include <basu/sd-bus.h>
#endif

struct mako_notification;
struct mako_state;

enum mako_history_filter {
	MAKO_HISTORY_ALL,
	MAKO_HISTORY_READ,
	MAKO_HISTORY_UNREAD,
};

char *mako_default_history_path(void);
void mako_history_init_state(struct mako_state *state);
bool mako_history_prune(struct mako_state *state);
bool mako_history_add_notification(struct mako_state *state,
	const struct mako_notification *notif);
int mako_history_handle_list(sd_bus_message *msg, struct mako_state *state,
	enum mako_history_filter filter);
int mako_history_delete(struct mako_state *state, uint32_t id, bool all);
int mako_history_mark_read(struct mako_state *state, uint32_t id, bool all);
struct mako_notification *mako_history_restore_latest(struct mako_state *state);

#endif
