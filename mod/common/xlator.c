#include "nat64/mod/common/xlator.h"

#include <linux/sched.h>
#include "nat64/common/types.h"
#include "nat64/common/xlat.h"
#include "nat64/mod/common/atomic_config.h"
#include "nat64/mod/common/pool6.h"
#include "nat64/mod/common/wkmalloc.h"
#include "nat64/mod/stateless/blacklist4.h"
#include "nat64/mod/stateless/eam.h"
#include "nat64/mod/stateless/rfc6791.h"
#include "nat64/mod/stateful/fragment_db.h"
#include "nat64/mod/stateful/joold.h"
#include "nat64/mod/stateful/pool4/db.h"
#include "nat64/mod/stateful/bib/db.h"
#include "nat64/mod/stateful/session/db.h"

/**
 * All the configuration and state of the Jool instance in the given network
 * namespace (@ns).
 */
struct jool_instance {
	struct xlator jool;

	/*
	 * I want to turn this into a hash table, but it doesn't seem like
	 * @ns holds anything reminiscent of an identifier...
	 */
	struct list_head list_hook;
};

static struct list_head __rcu *pool;
static DEFINE_MUTEX(lock);

static void xlator_get(struct xlator *jool)
{
	get_net(jool->ns);

	config_get(jool->global);
	pool6_get(jool->pool6);

	if (xlat_is_siit()) {
		eamt_get(jool->siit.eamt);
		blacklist_get(jool->siit.blacklist);
		rfc6791_get(jool->siit.pool6791);
	} else {
		fragdb_get(jool->nat64.frag);
		pool4db_get(jool->nat64.pool4);
		bibdb_get(jool->nat64.bib);
		sessiondb_get(jool->nat64.session);
		joold_get(jool->nat64.joold);
	}

	cfgcandidate_get(jool->newcfg);
}

/**
 * exit_net - stops translation of packets traveling through the @ns namespace.
 */
static int exit_net(struct net *ns)
{
	struct list_head *list;
	struct jool_instance *instance;

	mutex_lock(&lock);

	list = rcu_dereference_protected(pool, lockdep_is_held(&lock));
	list_for_each_entry(instance, list, list_hook) {
		if (instance->jool.ns == ns) {
			list_del_rcu(&instance->list_hook);
			mutex_unlock(&lock);
			xlator_put(&instance->jool);

			synchronize_rcu_bh();

			wkfree(struct jool_instance, instance);
			return 0;
		}
	}

	mutex_unlock(&lock);
	return -ESRCH;
}

static void __net_exit joolns_exit_net(struct net *ns)
{
	exit_net(ns);
}

static struct pernet_operations joolns_ops = {
	.exit = joolns_exit_net,
};

/**
 * xlator_init - Initializes this module. Do not call other functions before
 * this one.
 */
int xlator_init(void)
{
	struct list_head *list;
	int error;

	list = __wkmalloc("xlator DB", sizeof(struct list_head), GFP_KERNEL);
	if (!list)
		return -ENOMEM;
	INIT_LIST_HEAD(list);
	RCU_INIT_POINTER(pool, list);

	error = register_pernet_subsys(&joolns_ops);
	if (error) {
		__wkfree("xlator DB", list);
		return error;
	}

	return 0;
}

/**
 * xlator_destroy - Graceful termination of this module. Reverts xlator_init().
 * Will clean up any allocated memory.
 */
void xlator_destroy(void)
{
	struct list_head *list;
	struct jool_instance *instance;
	struct jool_instance *tmp;

	unregister_pernet_subsys(&joolns_ops);

	list = rcu_dereference_raw(pool);
	list_for_each_entry_safe(instance, tmp, list, list_hook) {
		xlator_put(&instance->jool);
		wkfree(struct jool_instance, instance);
	}
	__wkfree("xlator DB", list);
}

static int init_siit(struct xlator *jool)
{
	int error;

	error = config_init(&jool->global);
	if (error)
		goto config_fail;
	error = pool6_init(&jool->pool6);
	if (error)
		goto pool6_fail;
	error = eamt_init(&jool->siit.eamt);
	if (error)
		goto eamt_fail;
	error = blacklist_init(&jool->siit.blacklist);
	if (error)
		goto blacklist_fail;
	error = rfc6791_init(&jool->siit.pool6791);
	if (error)
		goto rfc6791_fail;

	error = mapt_init(&jool->siit.mapt_bmr_table);
	if (error)
		goto mapt_fail;

	error = mapt_init_enduser_prefix6_table(&jool->siit.mapt_enduprefix6_table);
	if (error)
		goto mapt_enduser_fail;

	jool->newcfg = cfgcandidate_create();
	if (!jool->newcfg)
		goto newcfg_fail;

	return 0;


newcfg_fail:
	mapt_enduser_put(jool->siit.mapt_enduprefix6_table);
mapt_enduser_fail:
	mapt_put(jool->siit.mapt_bmr_table);
mapt_fail:
	rfc6791_put(jool->siit.pool6791);
rfc6791_fail:
	blacklist_put(jool->siit.blacklist);
blacklist_fail:
	eamt_put(jool->siit.eamt);
eamt_fail:
	pool6_put(jool->pool6);
pool6_fail:
	config_put(jool->global);
config_fail:
	return error;
}

static int init_nat64(struct xlator *jool, struct net *ns)
{
	int error;

	error = config_init(&jool->global);
	if (error)
		goto config_fail;
	error = pool6_init(&jool->pool6);
	if (error)
		goto pool6_fail;
	jool->nat64.frag = fragdb_create();
	if (!jool->nat64.frag)
		goto fragdb_fail;
	error = pool4db_init(&jool->nat64.pool4, 0);
	if (error)
		goto pool4_fail;
	error = bibdb_init(&jool->nat64.bib);
	if (error)
		goto bibdb_fail;
	error = sessiondb_init(&jool->nat64.session);
	if (error)
		goto sessiondb_fail;
	jool->nat64.joold = joold_create(ns);
	if (!jool->nat64.joold)
		goto joold_fail;

	jool->newcfg = cfgcandidate_create();
	if (!jool->newcfg)
		goto newcfg_fail;

	return 0;

newcfg_fail:
	joold_put(jool->nat64.joold);
joold_fail:
	sessiondb_put(jool->nat64.session);
sessiondb_fail:
	bibdb_put(jool->nat64.bib);
bibdb_fail:
	pool4db_put(jool->nat64.pool4);
pool4_fail:
	fragdb_put(jool->nat64.frag);
fragdb_fail:
	pool6_put(jool->pool6);
pool6_fail:
	config_put(jool->global);
config_fail:
	return error;
}

/**
 * xlator_add - Whenever called, starts translation of packets traveling through
 * the namespace running in the caller's context.
 * @result: Will be initialized with a reference to the new translator. Send
 *     NULL if you're not interested.
 */
int xlator_add(struct xlator *result)
{
	struct list_head *list;
	struct jool_instance *instance;
	struct net *ns;
	int error;

	ns = get_net_ns_by_pid(task_pid_nr(current));
	if (IS_ERR(ns)) {
		log_err("Could not retrieve the current namespace.");
		return PTR_ERR(ns);
	}

	instance = wkmalloc(struct jool_instance, GFP_KERNEL);
	if (!instance) {
		put_net(ns);
		return -ENOMEM;
	}

	instance->jool.ns = ns;
	error = xlat_is_siit()
			? init_siit(&instance->jool)
			: init_nat64(&instance->jool, ns);
	if (error) {
		put_net(ns);
		wkfree(struct jool_instance, instance);
		return error;
	}

	mutex_lock(&lock);
	error = xlator_find(ns, NULL);
	switch (error) {
	case 0:
		log_err("This namespace already has a Jool instance.");
		error = -EEXIST;
		goto mutex_fail;
	case -ESRCH: /* Happy path. */
		break;
	default:
		log_err("Unknown error code: %d.", error);
		goto mutex_fail;
	}

	list = rcu_dereference_protected(pool, lockdep_is_held(&lock));
	list_add_tail_rcu(&instance->list_hook, list);
	mutex_unlock(&lock);

	if (result) {
		xlator_get(&instance->jool);
		memcpy(result, &instance->jool, sizeof(instance->jool));
	}

	return 0;

mutex_fail:
	mutex_unlock(&lock);
	xlator_put(&instance->jool);
	wkfree(struct jool_instance, instance);
	return error;
}

/**
 * xlator_rm - Whenever called, stops translation of packets traveling through
 * the namespace running in the caller's context.
 */
int xlator_rm(void)
{
	struct net *ns;
	int error;

	ns = get_net_ns_by_pid(task_pid_nr(current));
	if (IS_ERR(ns)) {
		log_err("Could not retrieve the current namespace.");
		return PTR_ERR(ns);
	}

	error = exit_net(ns);
	switch (error) {
	case 0:
		break;
	case -ESRCH:
		log_err("This namespace doesn't have a Jool instance.");
		break;
	default:
		log_err("Unknown error code: %d.", error);
		break;
	}

	put_net(ns);
	return error;
}

int xlator_replace(struct xlator *jool)
{
	struct list_head *list;
	struct jool_instance *old;
	struct jool_instance *new;

	new = wkmalloc(struct jool_instance, GFP_KERNEL);
	if (!new)
		return -ENOMEM;
	memcpy(&new->jool, jool, sizeof(*jool));
	xlator_get(&new->jool);

	mutex_lock(&lock);

	list = rcu_dereference_protected(pool, lockdep_is_held(&lock));
	list_for_each_entry_rcu(old, list, list_hook) {
		if (old->jool.ns == new->jool.ns) {
			list_replace_rcu(&old->list_hook, &new->list_hook);
			mutex_unlock(&lock);
			xlator_put(&old->jool);

			synchronize_rcu_bh();

			wkfree(struct jool_instance, old);
			return 0;
		}
	}

	mutex_unlock(&lock);
	return -ESRCH;
}

/**
 * xlator_find - Retrieves the Jool instance currently loaded in namespace @ns.
 *
 * Please xlator_put() the instance when you're done using it.
 *
 * If @result is NULL, it can be used to know whether the instance exists.
 */
int xlator_find(struct net *ns, struct xlator *result)
{
	struct list_head *list;
	struct jool_instance *instance;

	rcu_read_lock_bh();

	list = rcu_dereference_bh(pool);
	list_for_each_entry_rcu(instance, list, list_hook) {
		if (instance->jool.ns == ns) {
			if (result) {
				xlator_get(&instance->jool);
				memcpy(result, &instance->jool, sizeof(instance->jool));
			}
			rcu_read_unlock_bh();
			return 0;
		}
	}

	rcu_read_unlock_bh();
	return -ESRCH;
}

/**
 * xlator_find_current - Retrieves the Jool instance loaded in the current
 * namespace.
 *
 * Please xlator_put() the instance when you're done using it.
 */
int xlator_find_current(struct xlator *result)
{
	struct net *ns;
	int error;

	ns = get_net_ns_by_pid(task_pid_nr(current)); /* +1 to ns. */
	if (IS_ERR(ns)) {
		log_err("Could not retrieve the current namespace.");
		return PTR_ERR(ns);
	}

	error = xlator_find(ns, result); /* +1 to result's DBs, including ns. */
	put_net(ns); /* -1 to ns. */
	return error;
}

void xlator_put(struct xlator *jool)
{
	put_net(jool->ns);

	config_put(jool->global);
	pool6_put(jool->pool6);

	if (xlat_is_siit()) {
		eamt_put(jool->siit.eamt);
		blacklist_put(jool->siit.blacklist);
		rfc6791_put(jool->siit.pool6791);
	} else {
		fragdb_put(jool->nat64.frag);
		pool4db_put(jool->nat64.pool4);
		sessiondb_put(jool->nat64.session);
		bibdb_put(jool->nat64.bib);
		joold_put(jool->nat64.joold);
	}

	cfgcandidate_put(jool->newcfg);
}

int xlator_foreach(xlator_foreach_cb cb, void *args)
{
	struct list_head *list;
	struct jool_instance *instance;
	int error = 0;

	/* TODO (final) are timers really BH context? */
	rcu_read_lock_bh();

	list = rcu_dereference_bh(pool);
	list_for_each_entry_rcu(instance, list, list_hook) {
		error = cb(&instance->jool, args);
		if (error)
			break;
	}

	rcu_read_unlock_bh();
	return error;
}

void xlator_copy_config(struct xlator *jool, struct full_config *copy)
{
	config_copy(&jool->global->cfg, &copy->global);
	bibdb_config_copy(jool->nat64.bib, &copy->bib);
	sessiondb_config_copy(jool->nat64.session, &copy->session);
	joold_config_copy(jool->nat64.joold, &copy->joold);
	fragdb_config_copy(jool->nat64.frag, &copy->frag);
}
