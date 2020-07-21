#include "mod/common/types.h"

#include <linux/icmp.h>
#include <linux/icmpv6.h>
#include <net/ipv6.h>
#include "mod/common/log.h"
#include "mod/common/translation_state.h"

bool is_icmp6_info(__u8 type)
{
	return (type == ICMPV6_ECHO_REQUEST) || (type == ICMPV6_ECHO_REPLY);
}

bool is_icmp6_error(__u8 type)
{
	/*
	 * We do not return !is_icmp6_info(type) because unknown codes should be considered
	 * untranslatable.
	 */
	return (type == ICMPV6_DEST_UNREACH)
			|| (type == ICMPV6_PKT_TOOBIG)
			|| (type == ICMPV6_TIME_EXCEED)
			|| (type == ICMPV6_PARAMPROB);
}

bool is_icmp4_info(__u8 type)
{
	return (type == ICMP_ECHO) || (type == ICMP_ECHOREPLY);
}

bool is_icmp4_error(__u8 type)
{
	return (type == ICMP_DEST_UNREACH)
			|| (type == ICMP_SOURCE_QUENCH)
			|| (type == ICMP_REDIRECT)
			|| (type == ICMP_TIME_EXCEEDED)
			|| (type == ICMP_PARAMETERPROB);
}

/**
* log_tuple() - Prints the "tuple" tuple in the kernel ring buffer.
* @tuple: Structure to be dumped on logging.
*
* It's a ripoff of nf_ct_dump_tuple(), adjusted to comply to this project's logging requirements.
*/
void log_tuple(struct xlation *state, struct tuple *tuple)
{
	switch (tuple->l3_proto) {
	case L3PROTO_IPV4:
		log_debug(state, "Tuple: %pI4#%u -> %pI4#%u (%s)",
				&tuple->src.addr4.l3, tuple->src.addr4.l4,
				&tuple->dst.addr4.l3, tuple->dst.addr4.l4,
				l4proto_to_string(tuple->l4_proto));
		break;
	case L3PROTO_IPV6:
		log_debug(state, "Tuple: %pI6c#%u -> %pI6c#%u (%s)",
				&tuple->src.addr6.l3, tuple->src.addr6.l4,
				&tuple->dst.addr6.l3, tuple->dst.addr6.l4,
				l4proto_to_string(tuple->l4_proto));
		break;
	}
}
