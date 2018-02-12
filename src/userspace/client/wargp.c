#include "wargp.h"

#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "usr-str-utils.h"

int wargp_parse_bool(void *input, int key, char *str);
int wargp_parse_u32(void *field, int key, char *str);
int wargp_parse_l4proto(void *input, int key, char *str);

struct wargp_type wt_bool = {
	.doc = NULL,
	.parse = wargp_parse_bool,
};

struct wargp_type wt_u32 = {
	.doc = "unsigned 32-bit integer",
	.parse = wargp_parse_u32,
};

struct wargp_type wt_l4proto = {
	.doc = NULL,
	.parse = wargp_parse_l4proto,
};

struct wargp_args {
	struct wargp_option *opts;
	void *input;
};

int wargp_parse_bool(void *void_field, int key, char *str)
{
	struct wargp_bool *field = void_field;
	field->value = true;
	return 0;
}

int wargp_parse_u32(void *field, int key, char *str)
{
	return str_to_u32(str, field, 0, MAX_U32);
}

int wargp_parse_l4proto(void *void_field, int key, char *str)
{
	struct wargp_l4proto *field = void_field;

	if (field->set) {
		log_err("Only one protocol is allowed per request.");
		return -EINVAL;
	}

	switch (key) {
	case ARGP_TCP:
		field->proto = L4PROTO_TCP;
		field->set = true;
		return 0;
	case ARGP_UDP:
		field->proto = L4PROTO_UDP;
		field->set = true;
		return 0;
	case ARGP_ICMP:
		field->proto = L4PROTO_ICMP;
		field->set = true;
		return 0;
	}

	log_err("Unknown protocol key: %d", key);
	return -EINVAL;
}

static int adapt_options(struct argp *argp, struct wargp_option *wopts,
		struct argp_option **result)
{
	struct wargp_option *wopt;
	struct argp_option *opts;
	struct argp_option *opt;
	unsigned int i;
	unsigned int total_opts;

	if (!wopts) {
		*result = NULL;
		return 0;
	}

	total_opts = 0;
	for (i = 0; wopts[i].name; i++)
		if (wopts[i].key != ARGP_KEY_ARG)
			total_opts++;

	opts = calloc(total_opts + 1, sizeof(struct argp_option));
	if (!opts) {
		log_err("Out of memory.");
		return -ENOMEM;
	}
	argp->options = opts;

	wopt = wopts;
	opt = opts;
	while (wopt->name) {
		if (wopt->key != ARGP_KEY_ARG) {
			opt->name = wopt->name;
			opt->key = wopt->key;
			opt->arg = wopt->type->doc;
			opt->doc = wopt->doc;
			opt++;

		} else {
			if (argp->args_doc) {
				log_err("Bug: Only one ARGP_KEY_ARG option is allowed per option list.");
				free(opts);
				return -EINVAL;
			}
			argp->args_doc = wopt->type->doc;
		}

		wopt++;

	}

	*result = opts;
	return 0;
}

static int wargp_parser(int key, char *str, struct argp_state *state)
{
	struct wargp_args *wargs = state->input;
	struct wargp_option *opt;

	if (!wargs->opts)
		return ARGP_ERR_UNKNOWN;

	for (opt = wargs->opts; opt->name; opt++) {
		if (opt->key == key) {
			return opt->type->parse(wargs->input + opt->offset, key,
					str);
		}
	}

	return ARGP_ERR_UNKNOWN;
}

int wargp_parse(struct wargp_option *wopts, int argc, char **argv, void *input)
{
	struct wargp_args wargs = { .opts = wopts, .input = input };
	struct argp argp = { .parser = wargp_parser };
	struct argp_option *opts;
	int error;

	error = adapt_options(&argp, wopts, &opts);
	if (error)
		return error;

	error = argp_parse(&argp, argc, argv, ARGP_NO_EXIT, NULL, &wargs);

	if (opts)
		free(opts);
	return error;
}

void print_wargp_opts(struct wargp_option *opts, char *prefix)
{
	struct wargp_option *opt;

	for (opt = opts; opt->name; opt++)
		if (opt->key != ARGP_KEY_ARG)
			if (strncmp(prefix, opt->name, strlen(prefix)) == 0)
				printf("--%s\n", opt->name);
}
