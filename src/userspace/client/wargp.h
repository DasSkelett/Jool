#ifndef SRC_USERSPACE_CLIENT_WARGP_H_
#define SRC_USERSPACE_CLIENT_WARGP_H_

#include <argp.h>
#include <stdbool.h>
#include <stddef.h>

#include "types.h"

/*
 * I guess that argp is more versatile than I need it for. I use it for very
 * simple option parsing, which leads to lots of clumsy, redundant code.
 *
 * So I made this module. "wargp" stands for "wrapped argp". It's simply a mask
 * over argp that removes a lot of annoying unneeded stuff.
 */

typedef int (*wargp_parse_type)(void *input, int key, char *str);

struct wargp_type {
	char *doc;
	wargp_parse_type parse;
};

extern struct wargp_type wt_bool;
extern struct wargp_type wt_u32;
extern struct wargp_type wt_l4proto;

struct wargp_option {
	const char *name;
	int key;
	const char *doc;
	size_t offset;
	struct wargp_type *type;
};

struct wargp_bool {
	bool value;
};

struct wargp_l4proto {
	bool set;
	l4_protocol proto;
};

#define ARGP_TCP 't'
#define ARGP_UDP 'u'
#define ARGP_ICMP 'i'
#define ARGP_CSV 2000
#define ARGP_NO_HEADERS 2001
#define ARGP_NUMERIC 2002

#define WARGP_TCP(container, field, description) \
	{ \
		.name = "tcp", \
		.key = ARGP_TCP, \
		.doc = description, \
		.offset = offsetof(container, field), \
		.type = &wt_l4proto, \
	}
#define WARGP_UDP(container, field, description) \
	{ \
		.name = "udp", \
		.key = ARGP_UDP, \
		.doc = description, \
		.offset = offsetof(container, field), \
		.type = &wt_l4proto, \
	}
#define WARGP_ICMP(container, field, description) \
	{ \
		.name = "icmp", \
		.key = ARGP_ICMP, \
		.doc = description, \
		.offset = offsetof(container, field), \
		.type = &wt_l4proto, \
	}
#define WARGP_NO_HEADERS(container, field) \
	{ \
		.name = "no-headers", \
		.key = ARGP_NO_HEADERS, \
		.doc = "Do not print table headers", \
		.offset = offsetof(container, field), \
		.type = &wt_bool, \
	}
#define WARGP_CSV(container, field) { \
		.name = "csv", \
		.key = ARGP_CSV, \
		.doc = "Print in CSV format", \
		.offset = offsetof(container, field), \
		.type = &wt_bool, \
	}
#define WARGP_NUMERIC(container, field) { \
		.name = "numeric", \
		.key = ARGP_NUMERIC, \
		.doc = "Do not resolve names", \
		.offset = offsetof(container, field), \
		.type = &wt_bool, \
	}

int wargp_parse(struct wargp_option *wopts, int argc, char **argv, void *input);
void print_wargp_opts(struct wargp_option *opts, char *prefix);

#endif /* SRC_USERSPACE_CLIENT_WARGP_H_ */
