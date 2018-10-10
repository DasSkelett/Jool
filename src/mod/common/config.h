#ifndef _JOOL_MOD_CONFIG_H
#define _JOOL_MOD_CONFIG_H

#include <linux/kref.h>
#include "common/config.h"

struct global_config {
	struct global_config_usr cfg;
	struct kref refcounter;
};

struct global_config *config_alloc(void);
void config_get(struct global_config *global);
void config_put(struct global_config *global);

void config_copy(struct global_config_usr *from, struct global_config_usr *to);

#define pool6_contains(state, addr) \
	prefix6_contains(&(state)->jool.global->cfg.pool6.prefix, addr)

#endif /* _JOOL_MOD_CONFIG_H */
