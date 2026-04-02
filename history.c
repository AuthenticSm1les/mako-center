#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <json-glib/json-glib.h>

#include "config.h"
#include "history.h"
#include "mako.h"
#include "notification.h"

struct mako_history_entry {
	uint32_t id;
	bool read;
	char *app_name;
	char *app_icon;
	char *category;
	char *desktop_entry;
	char *summary;
	char *body;
	char *created_at;
	char *dismissed_at;
	uint8_t urgency;
	char **actions;
};

static void replace_string(char **dst, char *value) {
	free(*dst);
	*dst = value != NULL ? value : strdup("");
}

static void history_entry_finish(struct mako_history_entry *entry) {
	free(entry->app_name);
	free(entry->app_icon);
	free(entry->category);
	free(entry->desktop_entry);
	free(entry->summary);
	free(entry->body);
	free(entry->created_at);
	free(entry->dismissed_at);
	if (entry->actions != NULL) {
		for (size_t i = 0; entry->actions[i] != NULL; ++i) {
			free(entry->actions[i]);
		}
		free(entry->actions);
	}
}

static void history_entry_free(gpointer data) {
	struct mako_history_entry *entry = data;
	history_entry_finish(entry);
	free(entry);
}

static char **duplicate_actions(const char *const *actions, size_t actions_len) {
	if (actions == NULL || actions_len == 0) {
		return NULL;
	}

	char **copy = calloc(actions_len + 1, sizeof(char *));
	if (copy == NULL) {
		return NULL;
	}

	for (size_t i = 0; i < actions_len; ++i) {
		copy[i] = strdup(actions[i]);
		if (copy[i] == NULL) {
			for (size_t j = 0; j < i; ++j) {
				free(copy[j]);
			}
			free(copy);
			return NULL;
		}
	}

	return copy;
}

static size_t action_count(const char *const *actions) {
	size_t count = 0;
	if (actions == NULL) {
		return 0;
	}
	while (actions[count] != NULL) {
		++count;
	}
	return count;
}

char *mako_default_history_path(void) {
	const char *home = getenv("HOME");
	if (home == NULL) {
		fprintf(stderr, "HOME env var not set\n");
		return NULL;
	}

	const char *state_home = getenv("XDG_STATE_HOME");
	if (state_home == NULL || state_home[0] == '\0') {
		return g_strdup_printf("%s/.local/state/mako/history.json", home);
	}
	return g_strdup_printf("%s/mako/history.json", state_home);
}

static bool history_ensure_parent_dir(const char *path) {
	g_autofree char *dir = g_path_get_dirname(path);
	if (g_mkdir_with_parents(dir, 0700) != 0) {
		fprintf(stderr, "Failed to create history directory %s: %s\n",
			dir, strerror(errno));
		return false;
	}
	return true;
}

static uint8_t urgency_from_string(const char *urgency) {
	if (urgency == NULL) {
		return 1;
	}
	if (strcmp(urgency, "low") == 0) {
		return 0;
	}
	if (strcmp(urgency, "critical") == 0) {
		return 2;
	}
	return 1;
}

static const char *urgency_to_string(uint8_t urgency) {
	switch (urgency) {
	case 0:
		return "low";
	case 2:
		return "critical";
	default:
		return "normal";
	}
}

static const char *json_object_get_string_member_or_default(JsonObject *object,
		const char *member, const char *default_value) {
	if (!json_object_has_member(object, member)) {
		return default_value;
	}

	JsonNode *node = json_object_get_member(object, member);
	if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
			!g_type_is_a(json_node_get_value_type(node), G_TYPE_STRING)) {
		return default_value;
	}

	const char *value = json_object_get_string_member(object, member);
	return value != NULL ? value : default_value;
}

static char *history_now_iso8601(void) {
	GDateTime *now = g_date_time_new_now_utc();
	char *formatted = g_date_time_format_iso8601(now);
	g_date_time_unref(now);
	return formatted;
}

static struct mako_history_entry *history_entry_new(void) {
	struct mako_history_entry *entry = calloc(1, sizeof(*entry));
	if (entry == NULL) {
		return NULL;
	}

	entry->app_name = strdup("");
	entry->app_icon = strdup("");
	entry->category = strdup("");
	entry->desktop_entry = strdup("");
	entry->summary = strdup("");
	entry->body = strdup("");
	entry->created_at = strdup("");
	entry->dismissed_at = strdup("");
	entry->urgency = 1;
	return entry;
}

static struct mako_history_entry *history_entry_from_notification(
		const struct mako_notification *notif) {
	struct mako_history_entry *entry = history_entry_new();
	if (entry == NULL) {
		return NULL;
	}

	entry->id = notif->id;
	entry->read = false;

	replace_string(&entry->app_name, strdup(notif->app_name));
	replace_string(&entry->app_icon, strdup(notif->app_icon));
	replace_string(&entry->category, strdup(notif->category));
	replace_string(&entry->desktop_entry, strdup(notif->desktop_entry));
	replace_string(&entry->summary, strdup(notif->summary));
	replace_string(&entry->body, strdup(notif->body));
	replace_string(&entry->created_at,
		strdup(notif->created_at != NULL ? notif->created_at : ""));
	replace_string(&entry->dismissed_at, history_now_iso8601());
	entry->urgency = notif->urgency;

	size_t action_count = 0;
	struct mako_action *action;
	wl_list_for_each(action, &notif->actions, link) {
		action_count += 2;
	}
	if (action_count > 0) {
		char **actions_copy = calloc(action_count + 1, sizeof(char *));
		if (actions_copy == NULL) {
			return entry;
		}
		size_t index = 0;
		wl_list_for_each(action, &notif->actions, link) {
			actions_copy[index++] = strdup(action->key);
			actions_copy[index++] = strdup(action->title);
		}
		entry->actions = actions_copy;
	}

	return entry;
}

static gint history_entry_compare_desc(gconstpointer a, gconstpointer b) {
	const struct mako_history_entry *entry_a = *(const struct mako_history_entry * const *)a;
	const struct mako_history_entry *entry_b = *(const struct mako_history_entry * const *)b;
	if (entry_a->id < entry_b->id) {
		return 1;
	}
	if (entry_a->id > entry_b->id) {
		return -1;
	}
	return 0;
}

static struct mako_history_entry *history_entry_from_json_node(JsonNode *node) {
	if (!JSON_NODE_HOLDS_OBJECT(node)) {
		return NULL;
	}

	JsonObject *object = json_node_get_object(node);
	struct mako_history_entry *entry = history_entry_new();
	if (entry == NULL) {
		return NULL;
	}

	entry->id = (uint32_t)json_object_get_int_member(object, "id");
	entry->read = json_object_has_member(object, "read") ?
		json_object_get_boolean_member(object, "read") : false;

	replace_string(&entry->app_name,
		strdup(json_object_get_string_member_or_default(object, "app_name", "")));
	replace_string(&entry->app_icon,
		strdup(json_object_get_string_member_or_default(object, "app_icon", "")));
	replace_string(&entry->category,
		strdup(json_object_get_string_member_or_default(object, "category", "")));
	replace_string(&entry->desktop_entry,
		strdup(json_object_get_string_member_or_default(object, "desktop_entry", "")));
	replace_string(&entry->summary,
		strdup(json_object_get_string_member_or_default(object, "summary", "")));
	replace_string(&entry->body,
		strdup(json_object_get_string_member_or_default(object, "body", "")));
	replace_string(&entry->created_at,
		strdup(json_object_get_string_member_or_default(object, "created_at", "")));
	replace_string(&entry->dismissed_at,
		strdup(json_object_get_string_member_or_default(object, "dismissed_at", "")));

	if (json_object_has_member(object, "urgency")) {
		JsonNode *urgency_node = json_object_get_member(object, "urgency");
		if (JSON_NODE_HOLDS_VALUE(urgency_node) &&
				g_type_is_a(json_node_get_value_type(urgency_node), G_TYPE_STRING)) {
			entry->urgency = urgency_from_string(json_node_get_string(urgency_node));
		} else {
			entry->urgency = (uint8_t)json_node_get_int(urgency_node);
		}
	}

	if (json_object_has_member(object, "actions")) {
		JsonArray *actions = json_object_get_array_member(object, "actions");
		guint actions_len = json_array_get_length(actions);
		if (actions_len > 0) {
			entry->actions = calloc(actions_len * 2 + 1, sizeof(char *));
			for (guint i = 0; i < actions_len; ++i) {
				JsonObject *action = json_array_get_object_element(actions, i);
				entry->actions[i * 2] = strdup(json_object_get_string_member_or_default(action, "key", ""));
				entry->actions[i * 2 + 1] = strdup(json_object_get_string_member_or_default(action, "title", ""));
			}
		}
	}

	return entry;
}

static GPtrArray *history_load_entries_legacy(struct mako_state *state) {
	GKeyFile *key_file = g_key_file_new();
	const char *path = state->config.history_path;
	GPtrArray *entries = g_ptr_array_new_with_free_func(history_entry_free);

	GError *error = NULL;
	if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
		if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
			fprintf(stderr, "Failed to load legacy history file %s: %s\n", path, error->message);
		}
		g_error_free(error);
		g_key_file_unref(key_file);
		return entries;
	}

	gsize groups_len = 0;
	g_auto(GStrv) groups = g_key_file_get_groups(key_file, &groups_len);
	for (gsize i = 0; i < groups_len; ++i) {
		if (!g_key_file_has_key(key_file, groups[i], "id", NULL)) {
			continue;
		}

		struct mako_history_entry *entry = history_entry_new();
		if (entry == NULL) {
			continue;
		}
		entry->id = (uint32_t)g_key_file_get_uint64(key_file, groups[i], "id", NULL);
		entry->read = g_key_file_get_boolean(key_file, groups[i], "read", NULL);
		replace_string(&entry->app_name,
			g_key_file_get_string(key_file, groups[i], "app-name", NULL));
		replace_string(&entry->app_icon,
			g_key_file_get_string(key_file, groups[i], "app-icon", NULL));
		replace_string(&entry->category,
			g_key_file_get_string(key_file, groups[i], "category", NULL));
		replace_string(&entry->desktop_entry,
			g_key_file_get_string(key_file, groups[i], "desktop-entry", NULL));
		replace_string(&entry->summary,
			g_key_file_get_string(key_file, groups[i], "summary", NULL));
		replace_string(&entry->body,
			g_key_file_get_string(key_file, groups[i], "body", NULL));
		replace_string(&entry->created_at,
			g_key_file_get_string(key_file, groups[i], "created-at", NULL));
		replace_string(&entry->dismissed_at,
			g_key_file_get_string(key_file, groups[i], "dismissed-at", NULL));
		entry->urgency = (uint8_t)g_key_file_get_integer(key_file, groups[i], "urgency", NULL);

		gsize actions_len = 0;
		g_auto(GStrv) actions = g_key_file_get_string_list(key_file, groups[i], "actions", &actions_len, NULL);
		if (actions != NULL && actions_len > 0) {
			entry->actions = calloc(actions_len + 1, sizeof(char *));
			for (gsize j = 0; j < actions_len; ++j) {
				entry->actions[j] = strdup(actions[j]);
			}
		}

		g_ptr_array_add(entries, entry);
	}

	g_ptr_array_sort(entries, history_entry_compare_desc);
	g_key_file_unref(key_file);
	return entries;
}

static JsonArray *history_get_notifications_array(JsonNode *root) {
	if (root == NULL) {
		return NULL;
	}

	if (JSON_NODE_HOLDS_ARRAY(root)) {
		return json_node_get_array(root);
	}

	if (!JSON_NODE_HOLDS_OBJECT(root)) {
		return NULL;
	}

	JsonObject *object = json_node_get_object(root);
	if (!json_object_has_member(object, "notifications")) {
		return NULL;
	}

	JsonNode *notifications = json_object_get_member(object, "notifications");
	if (notifications == NULL || !JSON_NODE_HOLDS_ARRAY(notifications)) {
		return NULL;
	}

	return json_node_get_array(notifications);
}

static GPtrArray *history_load_entries(struct mako_state *state) {
	GPtrArray *entries = g_ptr_array_new_with_free_func(history_entry_free);
	const char *path = state->config.history_path;
	if (path == NULL || path[0] == '\0' || access(path, R_OK) != 0) {
		return entries;
	}

	JsonParser *parser = json_parser_new();
	GError *error = NULL;
	if (!json_parser_load_from_file(parser, path, &error)) {
		g_error_free(error);
		g_object_unref(parser);
		g_ptr_array_unref(entries);
		return history_load_entries_legacy(state);
	}

	JsonArray *array = history_get_notifications_array(json_parser_get_root(parser));
	if (array == NULL) {
		g_object_unref(parser);
		return entries;
	}

	guint len = json_array_get_length(array);
	for (guint i = 0; i < len; ++i) {
		struct mako_history_entry *entry =
			history_entry_from_json_node(json_array_get_element(array, i));
		if (entry != NULL) {
			g_ptr_array_add(entries, entry);
		}
	}

	g_ptr_array_sort(entries, history_entry_compare_desc);
	g_object_unref(parser);
	return entries;
}

static bool history_save_entries(struct mako_state *state, GPtrArray *entries) {
	const char *path = state->config.history_path;
	if (path == NULL || path[0] == '\0') {
		return false;
	}
	if (!history_ensure_parent_dir(path)) {
		return false;
	}

	JsonBuilder *builder = json_builder_new();
	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "version");
	json_builder_add_int_value(builder, 1);
	json_builder_set_member_name(builder, "notifications");
	json_builder_begin_array(builder);
	for (guint i = 0; i < entries->len; ++i) {
		struct mako_history_entry *entry = g_ptr_array_index(entries, i);
		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "id");
		json_builder_add_int_value(builder, entry->id);
		json_builder_set_member_name(builder, "read");
		json_builder_add_boolean_value(builder, entry->read);
		json_builder_set_member_name(builder, "app_name");
		json_builder_add_string_value(builder, entry->app_name);
		json_builder_set_member_name(builder, "app_icon");
		json_builder_add_string_value(builder, entry->app_icon);
		json_builder_set_member_name(builder, "category");
		json_builder_add_string_value(builder, entry->category);
		json_builder_set_member_name(builder, "desktop_entry");
		json_builder_add_string_value(builder, entry->desktop_entry);
		json_builder_set_member_name(builder, "summary");
		json_builder_add_string_value(builder, entry->summary);
		json_builder_set_member_name(builder, "body");
		json_builder_add_string_value(builder, entry->body);
		json_builder_set_member_name(builder, "created_at");
		json_builder_add_string_value(builder, entry->created_at);
		json_builder_set_member_name(builder, "dismissed_at");
		json_builder_add_string_value(builder, entry->dismissed_at);
		json_builder_set_member_name(builder, "urgency");
		json_builder_add_string_value(builder, urgency_to_string(entry->urgency));
		json_builder_set_member_name(builder, "actions");
		json_builder_begin_array(builder);
		if (entry->actions != NULL) {
			for (size_t j = 0; entry->actions[j] != NULL && entry->actions[j + 1] != NULL; j += 2) {
				json_builder_begin_object(builder);
				json_builder_set_member_name(builder, "key");
				json_builder_add_string_value(builder, entry->actions[j]);
				json_builder_set_member_name(builder, "title");
				json_builder_add_string_value(builder, entry->actions[j + 1]);
				json_builder_end_object(builder);
			}
		}
		json_builder_end_array(builder);
		json_builder_end_object(builder);
	}
	json_builder_end_array(builder);
	json_builder_end_object(builder);

	JsonGenerator *generator = json_generator_new();
	JsonNode *root = json_builder_get_root(builder);
	json_generator_set_root(generator, root);
	json_generator_set_pretty(generator, true);
	gchar *data = json_generator_to_data(generator, NULL);

	GError *error = NULL;
	bool ok = g_file_set_contents(path, data, -1, &error);
	if (!ok) {
		fprintf(stderr, "Failed to save history file %s: %s\n", path, error->message);
		g_error_free(error);
	}

	g_free(data);
	json_node_free(root);
	g_object_unref(generator);
	g_object_unref(builder);
	return ok;
}

static void history_prune_entries(GPtrArray *entries, int32_t max_history) {
	if (max_history < 0) {
		return;
	}

	while ((guint)max_history < entries->len) {
		g_ptr_array_remove_index(entries, entries->len - 1);
	}
}

bool mako_history_prune(struct mako_state *state) {
	g_autoptr(GPtrArray) entries = history_load_entries(state);
	history_prune_entries(entries, state->config.max_history);
	return history_save_entries(state, entries);
}

static bool history_filter_match(const struct mako_history_entry *entry,
		enum mako_history_filter filter) {
	switch (filter) {
	case MAKO_HISTORY_READ:
		return entry->read;
	case MAKO_HISTORY_UNREAD:
		return !entry->read;
	case MAKO_HISTORY_ALL:
	default:
		return true;
	}
}

static int history_entry_append_actions(sd_bus_message *reply,
		const struct mako_history_entry *entry) {
	int ret = sd_bus_message_open_container(reply, 'e', "sv");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_append_basic(reply, 's', "actions");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'v', "a{ss}");
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_open_container(reply, 'a', "{ss}");
	if (ret < 0) {
		return ret;
	}

	if (entry->actions != NULL) {
		for (size_t i = 0; entry->actions[i] != NULL && entry->actions[i + 1] != NULL; i += 2) {
			ret = sd_bus_message_append(reply, "{ss}", entry->actions[i], entry->actions[i + 1]);
			if (ret < 0) {
				return ret;
			}
		}
	}

	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_message_close_container(reply);
	if (ret < 0) {
		return ret;
	}
	return sd_bus_message_close_container(reply);
}

static int history_append_entries(sd_bus_message *reply, struct mako_state *state,
		enum mako_history_filter filter) {
	int ret = sd_bus_message_open_container(reply, 'a', "a{sv}");
	if (ret < 0) {
		return ret;
	}

	g_autoptr(GPtrArray) entries = history_load_entries(state);
	for (guint i = 0; i < entries->len; ++i) {
		struct mako_history_entry *entry = g_ptr_array_index(entries, i);
		if (!history_filter_match(entry, filter)) {
			continue;
		}

		ret = sd_bus_message_open_container(reply, 'a', "{sv}");
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "app-name", "s", entry->app_name);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "app-icon", "s", entry->app_icon);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "category", "s", entry->category);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "desktop-entry", "s", entry->desktop_entry);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "summary", "s", entry->summary);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "body", "s", entry->body);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "created-at", "s", entry->created_at);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "dismissed-at", "s", entry->dismissed_at);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "id", "u", entry->id);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "urgency", "y", entry->urgency);
		if (ret < 0) return ret;
		ret = history_entry_append_actions(reply, entry);
		if (ret < 0) return ret;
		ret = sd_bus_message_append(reply, "{sv}", "read", "b", (int)entry->read);
		if (ret < 0) return ret;
		ret = sd_bus_message_close_container(reply);
		if (ret < 0) return ret;
	}

	return sd_bus_message_close_container(reply);
}

void mako_history_init_state(struct mako_state *state) {
	mako_history_prune(state);
	g_autoptr(GPtrArray) entries = history_load_entries(state);
	for (guint i = 0; i < entries->len; ++i) {
		struct mako_history_entry *entry = g_ptr_array_index(entries, i);
		if (entry->id > state->last_id) {
			state->last_id = entry->id;
		}
	}
}

static void history_restore_notification(struct mako_notification *notif,
		const struct mako_history_entry *entry) {
	free(notif->app_name);
	free(notif->app_icon);
	free(notif->summary);
	free(notif->body);
	free(notif->category);
	free(notif->desktop_entry);
	free(notif->created_at);

	notif->app_name = strdup(entry->app_name);
	notif->app_icon = strdup(entry->app_icon);
	notif->summary = strdup(entry->summary);
	notif->body = strdup(entry->body);
	notif->category = strdup(entry->category);
	notif->desktop_entry = strdup(entry->desktop_entry);
	notif->created_at = strdup(entry->created_at);
	notif->urgency = entry->urgency;

	size_t actions_len = action_count((const char *const *)entry->actions);
	char **actions = duplicate_actions((const char *const *)entry->actions, actions_len);
	if (actions == NULL) {
		return;
	}

	for (size_t i = 0; actions[i] != NULL && actions[i + 1] != NULL; i += 2) {
		struct mako_action *action = calloc(1, sizeof(*action));
		if (action == NULL) {
			continue;
		}
		action->notification = notif;
		action->key = actions[i];
		action->title = actions[i + 1];
		actions[i] = NULL;
		actions[i + 1] = NULL;
		wl_list_insert(notif->actions.prev, &action->link);
	}

	if (actions != NULL) {
		for (size_t i = 0; actions[i] != NULL; ++i) {
			free(actions[i]);
		}
		free(actions);
	}
}

bool mako_history_add_notification(struct mako_state *state,
		const struct mako_notification *notif) {
	g_autoptr(GPtrArray) entries = history_load_entries(state);
	struct mako_history_entry *entry = history_entry_from_notification(notif);
	if (entry == NULL) {
		return false;
	}
	g_ptr_array_add(entries, entry);
	g_ptr_array_sort(entries, history_entry_compare_desc);
	history_prune_entries(entries, state->config.max_history);
	return history_save_entries(state, entries);
}

int mako_history_handle_list(sd_bus_message *msg, struct mako_state *state,
		enum mako_history_filter filter) {
	sd_bus_message *reply = NULL;
	int ret = sd_bus_message_new_method_return(msg, &reply);
	if (ret < 0) {
		return ret;
	}
	ret = history_append_entries(reply, state, filter);
	if (ret < 0) {
		return ret;
	}
	ret = sd_bus_send(NULL, reply, NULL);
	if (ret < 0) {
		return ret;
	}
	sd_bus_message_unref(reply);
	return 0;
}

int mako_history_delete(struct mako_state *state, uint32_t id, bool all) {
	g_autoptr(GPtrArray) entries = history_load_entries(state);
	for (gint i = (gint)entries->len - 1; i >= 0; --i) {
		struct mako_history_entry *entry = g_ptr_array_index(entries, i);
		if (all || entry->id == id) {
			g_ptr_array_remove_index(entries, i);
		}
	}
	return history_save_entries(state, entries) ? 0 : -EIO;
}

int mako_history_mark_read(struct mako_state *state, uint32_t id, bool all) {
	g_autoptr(GPtrArray) entries = history_load_entries(state);
	for (guint i = 0; i < entries->len; ++i) {
		struct mako_history_entry *entry = g_ptr_array_index(entries, i);
		if (all || entry->id == id) {
			entry->read = true;
		}
	}
	return history_save_entries(state, entries) ? 0 : -EIO;
}

struct mako_notification *mako_history_restore_latest(struct mako_state *state) {
	g_autoptr(GPtrArray) entries = history_load_entries(state);
	if (entries->len == 0) {
		return NULL;
	}

	struct mako_history_entry *selected = g_ptr_array_index(entries, 0);
	struct mako_notification *notif = create_notification(state);
	if (notif == NULL) {
		return NULL;
	}
	notif->id = selected->id;
	if (selected->id > state->last_id) {
		state->last_id = selected->id;
	}
	history_restore_notification(notif, selected);

	g_ptr_array_remove_index(entries, 0);
	history_save_entries(state, entries);
	return notif;
}
