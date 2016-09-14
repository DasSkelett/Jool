#include "nat64/mod/common/nf_hook.h"
#include "nat64/common/xlat.h"
#include "nat64/mod/common/config.h"
#include "nat64/mod/common/core.h"
#include "nat64/mod/common/log_time.h"
#include "nat64/mod/common/namespace.h"
#include "nat64/mod/common/nf_wrapper.h"
#include "nat64/mod/common/nl_handler.h"
#include "nat64/mod/common/pool6.h"
#include "nat64/mod/stateful/filtering_and_updating.h"
#include "nat64/mod/stateful/fragment_db.h"
#include "nat64/mod/stateful/pool4/db.h"

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <net/netfilter/ipv6/nf_defrag_ipv6.h>
#include <net/netfilter/ipv4/nf_defrag_ipv4.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("NIC-ITESM");
MODULE_DESCRIPTION("Stateful NAT64 (RFC 6146)");
MODULE_VERSION(JOOL_VERSION_STR);

static char *pool6[5];
static int pool6_len;
module_param_array(pool6, charp, &pool6_len, 0);
MODULE_PARM_DESC(pool6, "The IPv6 pool's prefixes.");

static char *pool4[5];
static int pool4_len;
module_param_array(pool4, charp, &pool4_len, 0);
MODULE_PARM_DESC(pool4, "The IPv4 pool's addresses.");

static unsigned int pool4_size;
module_param(pool4_size, uint, 0);
MODULE_PARM_DESC(pool4_size, "Size of pool4 DB's hashtable.");

static bool disabled;
module_param(disabled, bool, 0);
MODULE_PARM_DESC(disabled, "Disable the translation at the beginning of the module insertion.");

static int nl_family = NETLINK_USERSOCK;
module_param(nl_family, int, 0);
MODULE_PARM_DESC(nl_family, "Netlink family to bind the socket to.");


static char *banner = "\n"
	"                                   ,----,                       \n"
	"         ,--.                    ,/   .`|                 ,--,  \n"
	"       ,--.'|   ,---,          ,`   .'**:               ,--.'|  \n"
	"   ,--,:  :*|  '  .'*\\       ;    ;*****/  ,---.     ,--,  |#:  \n"
	",`--.'`|  '*: /  ;****'.   .'___,/****,'  /     \\ ,---.'|  :#'  \n"
	"|   :**:  |*|:  :*******\\  |    :*****|  /    /#' ;   :#|  |#;  \n"
	":   |***\\ |*::  |***/\\***\\ ;    |.';**; .    '#/  |   |#: _'#|  \n"
	"|   :*'**'; ||  :**' ;.***:`----'  |**|'    /#;   :   :#|.'##|  \n"
	"'   '*;.****;|  |**;/  \\***\\   '   :**;|   :##\\   |   '#'##;#:  \n"
	"|   |*| \\***|'  :**| \\  \\*,'   |   |**';   |###``.\\   \\##.'.#|  \n"
	"'   :*|  ;*.'|  |**'  '--'     '   :**|'   ;######\\`---`:  |#'  \n"
	"|   |*'`--'  |  :**:           ;   |.' '   |##.\\##|     '  ;#|  \n"
	"'   :*|      |  |*,'           '---'   |   :##';##:     |  :#;  \n"
	";   |.'      `--''                      \\   \\####/      '  ,/   \n"
	"'---'                                    `---`--`       '--'    \n";


static NF_CALLBACK(hook_ipv6, skb)
{
	return core_6to4(skb, skb->dev);
}

static NF_CALLBACK(hook_ipv4, skb)
{
	return core_4to6(skb, skb->dev);
}

static struct nf_hook_ops nfho[] = {
	{
		.hook = hook_ipv6,
		.pf = PF_INET6,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP6_PRI_JOOL,
	},
	{
		.hook = hook_ipv4,
		.pf = PF_INET,
		.hooknum = NF_INET_PRE_ROUTING,
		.priority = NF_IP_PRI_JOOL,
	},
};

static int __init nat64_init(void)
{
	int error;

	log_debug("%s", banner);
	log_debug("Inserting %s...", xlat_get_name());

	nf_defrag_ipv6_enable();
	nf_defrag_ipv4_enable();

	/* Init Jool's submodules. */
	error = joolns_init();
	if (error)
		goto joolns_failure;
	error = config_init(disabled);
	if (error)
		goto config_failure;
	error = nlhandler_init(nl_family);
	if (error)
		goto nlhandler_failure;
	error = pool6_init(pool6, pool6_len);
	if (error)
		goto pool6_failure;
	error = pool4db_init(pool4_size, pool4, pool4_len);
	if (error)
		goto pool4_failure;
	error = filtering_init();
	if (error)
		goto filtering_failure;
	error = fragdb_init();
	if (error)
		goto fragdb_failure;
#ifdef BENCHMARK
	error = logtime_init();
	if (error)
		goto log_time_failure;
#endif

	/* Hook Jool to Netfilter. */
	error = nf_register_hooks(nfho, ARRAY_SIZE(nfho));
	if (error)
		goto nf_register_hooks_failure;

	/* Yay */
	log_info("%s v" JOOL_VERSION_STR " module inserted.", xlat_get_name());
	return error;

nf_register_hooks_failure:
#ifdef BENCHMARK
	logtime_destroy();

log_time_failure:
#endif
	fragdb_destroy();

fragdb_failure:
	filtering_destroy();

filtering_failure:
	pool4db_destroy();

pool4_failure:
	pool6_destroy();

pool6_failure:
	nlhandler_destroy();

nlhandler_failure:
	config_destroy();

config_failure:
	joolns_destroy();

joolns_failure:
	return error;
}

static void __exit nat64_exit(void)
{
	/* Release the hook. */
	nf_unregister_hooks(nfho, ARRAY_SIZE(nfho));

	/* Deinitialize the submodules. */
#ifdef BENCHMARK
	logtime_destroy();
#endif
	fragdb_destroy();
	filtering_destroy();
	pool4db_destroy();
	pool6_destroy();
	nlhandler_destroy();
	config_destroy();
	joolns_destroy();

	log_info("%s v" JOOL_VERSION_STR " module removed.", xlat_get_name());
}

module_init(nat64_init);
module_exit(nat64_exit);
