/*
 * Naemon Event Radio Dispatcher
 *
 * This is a subscriber service which initiates a unix domain socket,
 * listens to it and lets other programs connect to it and subscribe
 * to various channels using a simple text-based syntax.
 *
 * This code uses the eventbroker api to get its data, which means
 * we're finally eating our own dogfood in that respect.
 */

#include <stdio.h>
#include <string.h>
#include "config.h"
#include <sys/types.h>
#include <sys/socket.h>
#include "lib/libnaemon.h"
#include "common.h"
#include "broker.h"
#include "nebmods.h"
#include "nebmodules.h"
#include "nebstructs.h"
#include "query-handler.h"
#include "utils.h"
#include "logging.h"
#include "nerd.h"
#include "globals.h"
#include "nm_alloc.h"
#include "events.h"

struct nerd_channel {
	const char *name; /* name of this channel */
	const char *description; /* user-presentable string to document the purpose of this channel */
	unsigned int id; /* channel id (might vary between invocations) */
	unsigned int required_options; /* event_broker_options required for this channel */
	unsigned int num_callbacks;
	unsigned int callbacks[NEBCALLBACK_NUMITEMS];
	int (*handler)(int, void *);  /* callback handler for this channel */
	objectlist *subscriptions; /* subscriber list */
};

static nebmodule nerd_mod; /* fake module to get our callbacks accepted */
static struct nerd_channel **channels;
static unsigned int num_channels, alloc_channels;
static unsigned int chan_host_checks_id, chan_service_checks_id;


static struct nerd_channel *find_channel(const char *name)
{
	unsigned int i;

	for (i = 0; i < num_channels; i++) {
		struct nerd_channel *chan = channels[i];
		if (!strcmp(name, chan->name)) {
			return chan;
		}
	}
	return NULL;
}

int nerd_get_channel_id(const char *name)
{
	struct nerd_channel *chan;

	chan = find_channel(name);
	if (!chan)
		return -1;

	return chan->id;
}

static struct nerd_channel *nerd_get_channel(unsigned int chan_id)
{
	return chan_id >= num_channels ? NULL : channels[chan_id];
}

objectlist *nerd_get_subscriptions(int chan_id)
{
	struct nerd_channel *chan = nerd_get_channel(chan_id);

	return chan ? chan->subscriptions : NULL;
}

static int nerd_register_channel_callbacks(struct nerd_channel *chan)
{
	unsigned int i;

	for (i = 0; i < chan->num_callbacks; i++) {
		int result = neb_register_callback(chan->callbacks[i], &nerd_mod, 0, chan->handler);
		if (result != 0) {
			nm_log(NSLOG_RUNTIME_ERROR, "nerd: Failed to register callback %d for channel '%s': %d\n",
			       chan->callbacks[i], chan->name, result);
			return -1;
		}
	}
	return 0;
}

static int nerd_deregister_channel_callbacks(struct nerd_channel *chan)
{
	unsigned int i;

	for (i = 0; i < chan->num_callbacks; i++) {
		neb_deregister_callback(chan->callbacks[i], chan->handler);
	}
	return 0;
}

static int subscribe(int sd, struct nerd_channel *chan, char *fmt)
{
	struct nerd_subscription *subscr;

	subscr = nm_calloc(1, sizeof(*subscr));
	subscr->sd = sd;
	subscr->chan = chan;
	subscr->format = fmt ? nm_strdup(fmt) : NULL;

	if (!chan->subscriptions) {
		nerd_register_channel_callbacks(chan);
	}

	prepend_object_to_objectlist(&chan->subscriptions, subscr);
	return 0;
}

static int cancel_channel_subscription(struct nerd_channel *chan, int sd)
{
	objectlist *list, *next, *prev = NULL;
	int cancelled = 0;

	if (!chan)
		return -1;

	for (list = chan->subscriptions; list; list = next) {
		struct nerd_subscription *subscr = (struct nerd_subscription *)list->object_ptr;
		next = list->next;

		if (subscr->sd == sd) {
			cancelled++;
			free(list);
			free(subscr);
			if (prev) {
				prev->next = next;
			} else {
				chan->subscriptions = next;
			}
			continue;
		}
		prev = list;
	}

	if (cancelled) {
		nm_log(NSLOG_INFO_MESSAGE, "nerd: Cancelled %d subscription%s to channel '%s' for %d\n",
		       cancelled, cancelled == 1 ? "" : "s", chan->name, sd);
	}

	if (chan->subscriptions == NULL)
		nerd_deregister_channel_callbacks(chan);

	return 0;
}

static int unsubscribe(int sd, struct nerd_channel *chan)
{
	objectlist *list, *next, *prev = NULL;

	for (list = chan->subscriptions; list; list = next) {
		struct nerd_subscription *subscr = (struct nerd_subscription *)list->object_ptr;
		next = list->next;
		if (subscr->sd == sd) {
			/* found it, so remove it */
			free(subscr);
			free(list);
			if (!prev) {
				chan->subscriptions = next;
				continue;
			} else {
				prev->next = next;
			}
			continue;
		}
		prev = list;
	}

	if (chan->subscriptions == NULL) {
		nerd_deregister_channel_callbacks(chan);
	}
	return 0;
}

/* removes a subscriber entirely and closes its socket */
int nerd_cancel_subscriber(int sd)
{
	unsigned int i;

	for (i = 0; i < num_channels; i++) {
		cancel_channel_subscription(channels[i], sd);
	}

	iobroker_close(nagios_iobs, sd);
	return 0;
}

int nerd_broadcast(unsigned int chan_id, void *buf, unsigned int len)
{
	struct nerd_channel *chan;
	objectlist *list, *next;

	if (!(chan = nerd_get_channel(chan_id)))
		return -1;

	for (list = chan->subscriptions; list; list = next) {
		struct nerd_subscription *subscr = (struct nerd_subscription *)list->object_ptr;
		int result;

		next = list->next;

		result = send(subscr->sd, buf, len, 0);
		if (result < 0) {
			if (errno == EAGAIN)
				return 0;

			nerd_cancel_subscriber(subscr->sd);
			return 500;
		}
	}

	return 0;
}


static int chan_host_checks(int cb, void *data)
{
	nebstruct_host_check_data *ds = (nebstruct_host_check_data *)data;
	check_result *cr = (check_result *)ds->check_result_ptr;
	host *h;
	char *buf;

	if (ds->type != NEBTYPE_HOSTCHECK_PROCESSED)
		return 0;

	if (channels[chan_host_checks_id]->subscriptions == NULL)
		return 0;

	h = (host *)ds->object_ptr;
	nm_asprintf(&buf, "%s from %d -> %d: %s\n", h->name, h->last_state, h->current_state, cr->output);
	nerd_broadcast(chan_host_checks_id, buf, strlen(buf));
	free(buf);
	return 0;
}

static int chan_service_checks(int cb, void *data)
{
	nebstruct_service_check_data *ds = (nebstruct_service_check_data *)data;
	check_result *cr = (check_result *)ds->check_result_ptr;
	service *s;
	char *buf;

	if (ds->type != NEBTYPE_SERVICECHECK_PROCESSED)
		return 0;
	s = (service *)ds->object_ptr;
	nm_asprintf(&buf, "%s;%s from %d -> %d: %s\n", s->host_name, s->description, s->last_state, s->current_state, cr->output);
	nerd_broadcast(chan_service_checks_id, buf, strlen(buf));
	free(buf);
	return 0;
}

static int nerd_deinit(void)
{
	unsigned int i;

	for (i = 0; i < num_channels; i++) {
		struct nerd_channel *chan = channels[i];
		objectlist *list, *next;

		for (list = chan->subscriptions; list; list = next) {
			struct nerd_subscription *subscr = (struct nerd_subscription *)list->object_ptr;
			iobroker_close(nagios_iobs, subscr->sd);
			next = list->next;
			free(list);
			free(subscr);
		}
		chan->subscriptions = NULL;
		nm_free(chan);
	}
	nm_free(channels);
	num_channels = 0;
	alloc_channels = 0;

	return 0;
}

int nerd_mkchan(const char *name, const char *description, int (*handler)(int, void *), unsigned int callbacks)
{
	struct nerd_channel *chan, **ptr;
	int i;

	if (num_channels + 1 >= alloc_channels) {
		alloc_channels = alloc_nr(alloc_channels);
		ptr = nm_realloc(channels, alloc_channels * sizeof(struct nerd_channel *));
		channels = ptr;
	}

	chan = nm_calloc(1, sizeof(*chan));
	chan->name = name;
	chan->description = description;
	chan->handler = handler;
	for (i = 0; callbacks && i < NEBCALLBACK_NUMITEMS; i++) {
		if (!(callbacks & (1 << i)))
			continue;

		chan->callbacks[chan->num_callbacks++] = i;
	}

	channels[num_channels++] = chan;

	nm_log(NSLOG_INFO_MESSAGE, "nerd: Channel %s registered successfully\n", chan->name);
	return num_channels - 1;
}

#define NERD_SUBSCRIBE   0
#define NERD_UNSUBSCRIBE 1
static int nerd_qh_handler(int sd, char *request, unsigned int len)
{
	char *chan_name, *fmt;
	struct nerd_channel *chan;
	int action;

	if (!*request || !strcmp(request, "help")) {
		nsock_printf_nul(sd, "Manage subscriptions to NERD channels.\n"
		                 "Valid commands:\n"
		                 "  list                      list available channels\n"
		                 "  subscribe <channel>       subscribe to a channel\n"
		                 "  unsubscribe <channel>     unsubscribe to a channel\n");
		return 0;
	}

	if (!strcmp(request, "list")) {
		unsigned int i;
		for (i = 0; i < num_channels; i++) {
			chan = channels[i];
			nsock_printf(sd, "%-15s %s\n", chan->name, chan->description);
		}
		nsock_printf(sd, "%c", 0);
		return 0;
	}

	chan_name = strchr(request, ' ');
	if (!chan_name)
		return 400;

	*chan_name = 0;
	chan_name++;
	if (!strcmp(request, "subscribe"))
		action = NERD_SUBSCRIBE;
	else if (!strcmp(request, "unsubscribe"))
		action = NERD_UNSUBSCRIBE;
	else {
		return 400;
	}

	/* might have a format-string */
	if ((fmt = strchr(chan_name, ':')))
		* (fmt++) = 0;

	chan = find_channel(chan_name);
	if (!chan) {
		return 400;
	}

	if (action == NERD_SUBSCRIBE)
		subscribe(sd, chan, fmt);
	else
		unsubscribe(sd, chan);

	return 0;
}

/* nebmod_init(), but loaded even if no modules are */
int nerd_init(void)
{
	nerd_mod.deinit_func = nerd_deinit;
	nerd_mod.filename = (char *)"NERD"; /* something to log */

	if (qh_register_handler("nerd", "Naemon Event Radio Dispatcher - Subscriber Service", 0, nerd_qh_handler) < 0) {
		nm_log(NSLOG_RUNTIME_ERROR, "nerd: Failed to register with query handler\n");
		return ERROR;
	}

	neb_add_core_module(&nerd_mod);

	chan_host_checks_id = nerd_mkchan("hostchecks",
	                                  "Host check results",
	                                  chan_host_checks, nebcallback_flag(NEBCALLBACK_HOST_CHECK_DATA));
	chan_service_checks_id = nerd_mkchan("servicechecks",
	                                     "Service check results",
	                                     chan_service_checks, nebcallback_flag(NEBCALLBACK_SERVICE_CHECK_DATA));

	nm_log(NSLOG_INFO_MESSAGE, "nerd: Fully initialized and ready to rock!\n");
	return 0;
}
