#include <uwsgi.h>

extern struct uwsgi_server uwsgi;

struct riemann_config {
	char *state;
	char *host;
	uint64_t host_len;
	char *description;
	char *tags;
	int fd;
	union uwsgi_sockaddr addr;
        socklen_t addr_len;
	struct uwsgi_buffer *ub_body;
	struct uwsgi_buffer *ub;
};

/*
protocol buffer encoders
*/

static int varint(struct uwsgi_buffer *ub, uint64_t n) {
	uint64_t shifted_value = 1;
	while(shifted_value > 0) {
		shifted_value = n >> 7;
		if (uwsgi_buffer_u8(ub, (n & 0x7F) | (shifted_value ? 0x80 : 0x00))) return -1;
		n = shifted_value;
	}
	return 0;	
}

// unsigned varint
static int pb_varint(struct uwsgi_buffer *ub, uint8_t pb_num, uint64_t n) {
	if (uwsgi_buffer_u8(ub, pb_num << 3)) return -1;
	return varint(ub, n);
}

// signed varint
static int pb_svarint(struct uwsgi_buffer *ub, uint8_t pb_num, int64_t n) {
	uint64_t encoded = abs(n) << 1;
	if (n < 0) encoded--;
        return pb_varint(ub, pb_num, encoded);
}

// string/bytes
static int pb_bytes(struct uwsgi_buffer *ub, uint8_t pb_num, char *buf, uint64_t len) {
	if (uwsgi_buffer_u8(ub, (pb_num << 3) | 2)) return -1;
	if (varint(ub, len)) return -1;
	return uwsgi_buffer_append(ub, buf, len);
}

static int riemann_metric(struct riemann_config *rc, struct uwsgi_metric *um, uint64_t now) {
	// build the packet (it will be sent via udp)
	// epoch + host + service + metric_sint64
	struct uwsgi_buffer *ub = rc->ub_body;
	ub->pos = 0;
	if (pb_varint(ub, 1, now)) return -1;
	if (pb_bytes(ub, 4, rc->host, rc->host_len)) return -1;
	if (pb_bytes(ub, 3, um->name, um->name_len)) return -1;
	if (pb_svarint(ub, 13, *um->value)) return -1;
	// now add the prefix
	ub = rc->ub;
	ub->pos = 0;
	if (uwsgi_buffer_u8(ub, ((6 << 3) | 2))) return -1;
	if (varint(ub, rc->ub_body->pos)) return -1; 
	if (uwsgi_buffer_append(ub, rc->ub_body->buf, rc->ub_body->pos)) return -1;
	return 0;
}

// main function for sending stats
static void stats_pusher_riemann(struct uwsgi_stats_pusher_instance *uspi, time_t now, char *json, size_t json_len) {
	// on setup error, we simply exit to avoid leaks
	if (!uspi->configured) {
		struct riemann_config *rc = uwsgi_calloc(sizeof(struct riemann_config));
		rc->host = uwsgi.hostname;
		rc->host_len = uwsgi.hostname_len;
		char *node = NULL;
		if (strchr(uspi->arg, '=')) {	
			if (uwsgi_kvlist_parse(uspi->arg, strlen(uspi->arg), ',', '=',
				"addr", &node,
				"node", &node,
				"host", &rc->host,
				NULL)) {
				uwsgi_log("[uwsgi-riemann] invalid keyval syntax\n");
				exit(1);
			}
			if (rc->host) rc->host_len = strlen(rc->host);
		}
		else {
			node = uspi->arg;
		}
		if (!node) {
			uwsgi_log("[uwsgi-riemann] you need to specify an address\n");
			exit(1);
		}
		char *colon = strchr(node, ':');
		if (!colon) {
			uwsgi_log("[uwsgi-riemann] invalid address\n");
			exit(1);
		}
		rc->addr_len = socket_to_in_addr(node, colon, 0, &rc->addr.sa_in);
		rc->fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (rc->fd < 0) {
			uwsgi_error("stats_pusher_riemann()/socket()");
			exit(1);
		}
		uwsgi_socket_nb(rc->fd);
		rc->ub = uwsgi_buffer_new(uwsgi.page_size);
		rc->ub_body = uwsgi_buffer_new(uwsgi.page_size);
		uspi->data = rc;
		uspi->configured = 1;
		uwsgi_log_verbose("[uwsgi-riemann] configured node %s\n", node);
	}


	struct riemann_config *rc = (struct riemann_config *) uspi->data;
	struct uwsgi_metric *um = uwsgi.metrics;
	while(um) {
		uwsgi_rlock(uwsgi.metrics_lock);
		int ret = riemann_metric(rc, um, (uint64_t)now);
		uwsgi_rwunlock(uwsgi.metrics_lock);
		if (ret) {
			uwsgi_log_verbose("[uwsgi-riemann] unable to generate packet for %.*s\n", um->name_len, um->name);
		}
		else {
			// send the packet
			if (sendto(rc->fd, rc->ub->buf, rc->ub->pos, 0, (struct sockaddr *) &rc->addr.sa_in, rc->addr_len) < 0) {
                		uwsgi_error("stats_pusher_riemann()/sendto()");
        		}
		}
		um = um->next;
	}

}

static void riemann_register(void) {
        struct uwsgi_stats_pusher *usp = uwsgi_register_stats_pusher("riemann", stats_pusher_riemann);
        // we use a custom format not the JSON one
        usp->raw = 1;
}

struct uwsgi_plugin riemann_plugin = {
        .name = "riemann",
        .on_load = riemann_register,
};

