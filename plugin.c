#include <uwsgi.h>
#include <curl/curl.h>

/*

this is a stats pusher plugin for the datadog server:

--stats-push datadog:https://app.datadoghq.com/api/v1/series?api_key=API_KEY

it exports values exposed by the metric subsystem

*/

extern struct uwsgi_server uwsgi;

static struct uwsgi_stats_datadog {
	char prefix[16];
	size_t prefix_len;
	int use_canonical;
	char hostname[512];
	size_t hostname_len;
} datadog_config;

static void datadog_set_prefix(char *opt, char *value, void *key) {
	size_t len;
	if (!value) {
		return;
	}
	len = strlen(value);
	if (len > sizeof(datadog_config.prefix) - 1) {
		uwsgi_log_verbose("[datadog] metrics prefix too long, max %d chars\n",
				sizeof(datadog_config.prefix) - 1);
		return;
	}

	memcpy(datadog_config.prefix, value, len);
	datadog_config.prefix[len] = '\0';
	datadog_config.prefix_len = len;
}

static struct uwsgi_option datadog_options[] = {
	{"datadog-prefix", required_argument, 's', "add a prefix to the exported metrics", datadog_set_prefix, &datadog_config.prefix, 0},
	{"datadog-canonical-hostname", required_argument, 0, "send canonical name for the hostname", uwsgi_opt_true, &datadog_config.use_canonical, 0}
};

/*
JSON body:

{"series":[{"metric":"NAME","points":[[NOW,VALUE]],"type":"GAUGE|COUNTER","host":"HOST"}]}

*/

size_t silent_stream_handler(char *ptr, size_t size, size_t nmemb, void *userdata)
{
   return size * nmemb;
}

static void stats_pusher_datadog(struct uwsgi_stats_pusher_instance *uspi, time_t now, char *json, size_t json_len) {

	// we use the same buffer for all of the metrics
	struct uwsgi_buffer *ub = uwsgi_buffer_new(uwsgi.page_size);

	if (uwsgi_buffer_append(ub, "{\"series\":[", 11)) goto error;

	struct uwsgi_metric *um = uwsgi.metrics;
	while(um) {
		uwsgi_rlock(uwsgi.metrics_lock);
		int64_t value = *um->value;
		uwsgi_rwunlock(uwsgi.metrics_lock);

		if (um->reset_after_push){
			uwsgi_wlock(uwsgi.metrics_lock);
			*um->value = um->initial_value;
			uwsgi_rwunlock(uwsgi.metrics_lock);
		}

		if (uwsgi_buffer_append(ub, "{\"metric\":\"", 11)) goto error;
		if (datadog_config.prefix) {
			if (uwsgi_buffer_append_json(ub, datadog_config.prefix, datadog_config.prefix_len)) goto error;
		}
		if (uwsgi_buffer_append_json(ub, um->name, um->name_len)) goto error;
		if (uwsgi_buffer_append(ub, "\",\"points\":[[", 13)) goto error;
		if (uwsgi_buffer_num64(ub, now)) goto error;
		if (uwsgi_buffer_append(ub, ",", 1)) goto error;
		if (uwsgi_buffer_num64(ub, value)) goto error;
		if (uwsgi_buffer_append(ub, "]],\"type\":\"", 11)) goto error;
		if (um->type == UWSGI_METRIC_GAUGE) {
			if (uwsgi_buffer_append(ub, "gauge", 5)) goto error;
		} else {
			if (uwsgi_buffer_append(ub, "counter", 7)) goto error;
		}
		if (uwsgi_buffer_append(ub, "\",\"host\":\"", 10)) goto error;
		if (datadog_config.use_canonical && datadog_config.hostname) {
			if (uwsgi_buffer_append_json(ub, datadog_config.hostname, datadog_config.hostname_len)) goto error;
		} else {
			if (uwsgi_buffer_append_json(ub, uwsgi.hostname, uwsgi.hostname_len)) goto error;
		}
		if (uwsgi_buffer_append(ub, "\"}", 2)) goto error;
		if (um->next) {
			if (uwsgi_buffer_append(ub, ",", 1)) goto error;
		}
		um = um->next;
	}

	if (uwsgi_buffer_append(ub, "]}", 2)) goto error;

	// now send the JSON to the datadog server via curl
	CURL *curl = curl_easy_init();
	if (!curl) {
		uwsgi_log_verbose("[datadog] unable to initialize curl\n");
		return;
	}
	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, uwsgi.socket_timeout);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, uwsgi.socket_timeout);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_URL, uspi->arg);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, ub->buf);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &silent_stream_handler);  // disable CURL output to stdout
	CURLcode res = curl_easy_perform(curl);
	curl_slist_free_all(headers);
	if (res != CURLE_OK) {
		uwsgi_log_verbose("[datadog] error sending metrics: %s\n", curl_easy_strerror(res));
		curl_easy_cleanup(curl);
		return;
	}
	long http_code = 0;
#ifdef CURLINFO_RESPONSE_CODE
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
#else
	curl_easy_getinfo(curl, CURLINFO_HTTP_CODE, &http_code);
#endif
	if (http_code != 200 && http_code != 201 && http_code != 202) {
		uwsgi_log_verbose("[datadog] HTTP api returned non-200 response code: %d\n", (int) http_code);
	}
	curl_easy_cleanup(curl);
	uwsgi_buffer_destroy(ub);
	return;
error:
	uwsgi_log_verbose("[datadog] unable to generate JSON\n");

	uwsgi_buffer_destroy(ub);
}

static void datadog_init(void) {
	struct uwsgi_stats_pusher *usp = uwsgi_register_stats_pusher("datadog", stats_pusher_datadog);
	// we use a custom format not the JSON one
	usp->raw = 1;

	// find the canonical name
	struct addrinfo hints, *result, *p;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_CANONNAME;

	if (getaddrinfo(uwsgi.hostname, "http", &hints, &result) != 0) {
		uwsgi_log_verbose("[datadog] unable to get canonical name for hostname\n");
		return;
	}
	for (p = result; p != NULL; p = p->ai_next) {
		size_t len = strlen(p->ai_canonname);
		len = UMIN(len, sizeof(datadog_config.hostname)-1);
		memcpy(datadog_config.hostname, p->ai_canonname, len);
		datadog_config.hostname[len] = '\0';
		datadog_config.hostname_len = len;
		break;
	}
	freeaddrinfo(result);
}

struct uwsgi_plugin datadog_plugin = {
	.name = "datadog",
	.options = datadog_options,
	.on_load = datadog_init,
};
