#include "nat64/mod/ttp/6to4.h"

#include <linux/ip.h>
#include <net/ipv6.h>
#include <linux/icmp.h>
#include <linux/icmpv6.h>

#include "nat64/mod/icmp_wrapper.h"
#include "nat64/mod/ipv6_hdr_iterator.h"
#include "nat64/mod/ttp/config.h"
#include "nat64/mod/send_packet.h"
#include "nat64/mod/stats.h"

int ttp64_create_skb(struct pkt_parts *in, struct sk_buff **out)
{
	int total_len;
	struct sk_buff *new_skb;
	bool is_first;

	is_first = is_first_fragment_ipv6(in->l3_hdr.ptr);

	/*
	 * These are my assumptions to compute total_len:
	 *
	 * Any L3 headers will be replaced by an IPv4 header.
	 * The L4 header will never change in size (in particular, ICMPv4 hdr len == ICMPv6 hdr len).
	 * The payload will not change in TCP, UDP and ICMP infos.
	 *
	 * As for ICMP errors:
	 * Any sub-L3 headers will be replaced by an IPv4 header.
	 * The sub-L4 header will never change in size.
	 * The subpayload will never change in size (unless it gets truncated later, but I don't care).
	 */
	total_len = sizeof(struct iphdr) + in->l4_hdr.len + in->payload.len;
	if (is_first && in->l4_hdr.proto == L4PROTO_ICMP
			&& is_icmp6_error(icmp6_hdr(in->skb)->icmp6_type)) {
		struct hdr_iterator iterator = HDR_ITERATOR_INIT((struct ipv6hdr *) (in->payload.ptr));
		hdr_iterator_result result = hdr_iterator_last(&iterator);

		if (WARN(result != HDR_ITERATOR_END, "Validated packet has an invalid l3 header.")) {
			inc_stats(in->skb, IPSTATS_MIB_INDISCARDS);
			return -EINVAL;
		}

		/* Add the IPv4 subheader, remove the IPv6 subheaders. */
		total_len += sizeof(struct iphdr) - (iterator.data - in->payload.ptr);
	}

	new_skb = alloc_skb(LL_MAX_HEADER + total_len, GFP_ATOMIC);
	if (!new_skb) {
		inc_stats(in->skb, IPSTATS_MIB_INDISCARDS);
		return -ENOMEM;
	}

	skb_reserve(new_skb, LL_MAX_HEADER);
	skb_put(new_skb, total_len);
	skb_reset_mac_header(new_skb);
	skb_reset_network_header(new_skb);
	skb_set_transport_header(new_skb, sizeof(struct iphdr));

	skb_set_jcb(new_skb, L3PROTO_IPV4, in->l4_hdr.proto,
			skb_transport_header(new_skb) + in->l4_hdr.len,
			NULL, skb_original_skb(in->skb));

	new_skb->mark = in->skb->mark;
	new_skb->protocol = htons(ETH_P_IP);
	new_skb->next = NULL;
	new_skb->prev = NULL;

	*out = new_skb;
	return 0;
}

/**
 * One-liner for creating the IPv4 header's Identification field.
 * It assumes that the packet will not contain a fragment header.
 */
static __be16 generate_ipv4_id_nofrag(struct ipv6hdr *ip6_header)
{
	__u16 packet_len;
	__be16 random;

	packet_len = sizeof(*ip6_header) + be16_to_cpu(ip6_header->payload_len);
	if (88 < packet_len && packet_len <= 1280) {
		get_random_bytes(&random, 2);
		return random;
	}

	return 0; /* Because the DF flag will be set. */
}

/**
 * One-liner for creating the IPv4 header's Dont Fragment flag.
 */
static bool generate_df_flag(struct ipv6hdr *ip6_header)
{
	__u16 packet_len = sizeof(*ip6_header) + be16_to_cpu(ip6_header->payload_len);
	return (88 < packet_len && packet_len <= 1280) ? false : true;
}

/**
 * One-liner for creating the IPv4 header's Protocol field.
 */
static __u8 build_protocol_field(struct ipv6hdr *ip6_header)
{
	struct hdr_iterator iterator = HDR_ITERATOR_INIT(ip6_header);

	/* Skip stuff that does not exist in IPv4. */
	while (iterator.hdr_type == NEXTHDR_HOP
			|| iterator.hdr_type == NEXTHDR_ROUTING
			|| iterator.hdr_type == NEXTHDR_DEST)
		hdr_iterator_next(&iterator);

	if (iterator.hdr_type == NEXTHDR_ICMP)
		return IPPROTO_ICMP;
	if (iterator.hdr_type == NEXTHDR_FRAGMENT) {
		hdr_iterator_last(&iterator);
		return iterator.hdr_type;
	}

	return iterator.hdr_type;
}

/**
 * Returns "true" if ip6_hdr's first routing header contains a Segments Field which is not zero.
 *
 * @param ip6_hdr IPv6 header of the packet you want to test.
 * @param field_location (out parameter) if the header contains a routing header, the offset of the
 *		segments left field (from the start of ip6_hdr) will be stored here.
 * @return whether ip6_hdr's first routing header contains a Segments Field which is not zero.
 */
static bool has_nonzero_segments_left(struct ipv6hdr *ip6_hdr, __u32 *field_location)
{
	struct ipv6_rt_hdr *rt_hdr;
	__u32 rt_hdr_offset, segments_left_offset;

	rt_hdr = get_extension_header(ip6_hdr, NEXTHDR_ROUTING);
	if (!rt_hdr)
		return false;

	rt_hdr_offset = ((void *) rt_hdr) - ((void *) ip6_hdr);
	segments_left_offset = offsetof(struct ipv6_rt_hdr, segments_left);
	*field_location = rt_hdr_offset + segments_left_offset;

	return (rt_hdr->segments_left != 0);
}

/**
 * One-liner for creating the IPv4 header's Identification field.
 * It assumes that the packet will contain a fragment header.
 */
static __be16 generate_ipv4_id_dofrag(struct frag_hdr *ipv6_frag_hdr)
{
	return cpu_to_be16(be32_to_cpu(ipv6_frag_hdr->identification));
}

/**
 * Translates in's ipv6 header into out's ipv4 header.
 * This is RFC 6145 sections 5.1 and 5.1.1, except lengths and checksum (See post_ipv4()).
 *
 * Aside from the main call (to translate a normal IPv6 packet's layer 3 header), this function can
 * also be called to translate a packet's inner packet, which severely constraints the information
 * from "in" it can use; see translate_inner_packet().
 */
int ttp64_ipv4(struct tuple *tuple4, struct pkt_parts *in, struct pkt_parts *out)
{
	struct ipv6hdr *ip6_hdr = in->l3_hdr.ptr;
	struct frag_hdr *ip6_frag_hdr;
	struct iphdr *ip4_hdr;
	struct translate_config *config;

	bool reset_tos, build_ipv4_id, df_always_on;
	__u8 dont_fragment, new_tos;

	rcu_read_lock_bh();
	config = ttpconfig_get();
	reset_tos = config->reset_tos;
	build_ipv4_id = config->build_ipv4_id;
	df_always_on = config->df_always_on;
	new_tos = config->new_tos;
	rcu_read_unlock_bh();

	ip4_hdr = out->l3_hdr.ptr;
	ip4_hdr->version = 4;
	ip4_hdr->ihl = 5;
	ip4_hdr->tos = reset_tos ? new_tos : get_traffic_class(ip6_hdr);
	ip4_hdr->id = build_ipv4_id ? generate_ipv4_id_nofrag(ip6_hdr) : 0;
	dont_fragment = df_always_on ? 1 : generate_df_flag(ip6_hdr);
	ip4_hdr->frag_off = build_ipv4_frag_off_field(dont_fragment, 0, 0);
	if (!is_inner_pkt(in)) {
		ip4_hdr->tot_len = cpu_to_be16(out->l3_hdr.len + out->l4_hdr.len + out->payload.len);
		if (ip6_hdr->hop_limit <= 1) {
			icmp64_send(in->skb, ICMPERR_HOP_LIMIT, 0);
			inc_stats(in->skb, IPSTATS_MIB_INHDRERRORS);
			return -EINVAL;
		}
		ip4_hdr->ttl = ip6_hdr->hop_limit - 1;
	} else {
		ip4_hdr->tot_len = cpu_to_be16(be16_to_cpu(ip6_hdr->payload_len)
				- (in->l3_hdr.len - sizeof(*ip6_hdr)) + sizeof(*ip4_hdr));
		ip4_hdr->ttl = ip6_hdr->hop_limit;
	}
	ip4_hdr->protocol = build_protocol_field(ip6_hdr);
	/* ip4_hdr->check is set later; please scroll down. */
	ip4_hdr->saddr = tuple4->src.addr4.l3.s_addr;
	ip4_hdr->daddr = tuple4->dst.addr4.l3.s_addr;

	if (!is_inner_pkt(in)) {
		__u32 nonzero_location;
		if (has_nonzero_segments_left(ip6_hdr, &nonzero_location)) {
			log_debug("Packet's segments left field is nonzero.");
			icmp64_send(in->skb, ICMPERR_HDR_FIELD, nonzero_location);
			inc_stats(in->skb, IPSTATS_MIB_INHDRERRORS);
			return -EINVAL;
		}
	}

	ip6_frag_hdr = get_extension_header(ip6_hdr, NEXTHDR_FRAGMENT);
	if (ip6_frag_hdr) {
		__u16 ipv6_fragment_offset = get_fragment_offset_ipv6(ip6_frag_hdr);
		__u16 ipv6_m = is_more_fragments_set_ipv6(ip6_frag_hdr);

		struct hdr_iterator iterator = HDR_ITERATOR_INIT(ip6_hdr);
		hdr_iterator_last(&iterator);

		/* No need to override tot_len, because our way already takes the frag hdr into account. */
		ip4_hdr->id = generate_ipv4_id_dofrag(ip6_frag_hdr);
		ip4_hdr->frag_off = build_ipv4_frag_off_field(0, ipv6_m, ipv6_fragment_offset);
		/*
		 * This kinda contradicts the RFC.
		 * But following its logic, if the last extension header says ICMPv6 it wouldn't be switched
		 * to ICMPv4.
		 */
		ip4_hdr->protocol = (iterator.hdr_type == NEXTHDR_ICMP) ? IPPROTO_ICMP : iterator.hdr_type;
	}

	ip4_hdr->check = 0;
	ip4_hdr->check = ip_fast_csum(ip4_hdr, ip4_hdr->ihl);

	/*
	 * The kernel already drops packets if they don't allow fragmentation
	 * and the next hop MTU is smaller than their size.
	 */

	return 0;
}

/**
 * One liner for creating the ICMPv4 header's MTU field.
 * Returns the smallest out of the three parameters.
 */
static __be16 icmp4_minimum_mtu(__u32 packet_mtu, __u16 nexthop4_mtu, __u16 nexthop6_mtu)
{
	__u16 result;

	if (nexthop4_mtu < packet_mtu)
		result = (nexthop4_mtu < nexthop6_mtu) ? nexthop4_mtu : nexthop6_mtu;
	else
		result = (packet_mtu < nexthop6_mtu) ? packet_mtu : nexthop6_mtu;

	return cpu_to_be16(result);
}

static int compute_mtu4(struct sk_buff *in, struct sk_buff *out)
{
	struct icmphdr *out_icmp = icmp_hdr(out);
#ifndef UNIT_TESTING
	struct dst_entry *out_dst;
	struct icmp6hdr *in_icmp = icmp6_hdr(in);
	int error;

	error = sendpkt_route4(out);
	if (error)
		return error;

	log_debug("Packet MTU: %u", be32_to_cpu(in_icmp->icmp6_mtu));

	if (!in || !in->dev)
		return -EINVAL;
	log_debug("In dev MTU: %u", in->dev->mtu);

	out_dst = skb_dst(out);
	log_debug("Out dev MTU: %u", out_dst->dev->mtu);

	out_icmp->un.frag.mtu = icmp4_minimum_mtu(be32_to_cpu(in_icmp->icmp6_mtu) - 20,
			out_dst->dev->mtu,
			in->dev->mtu - 20);
	log_debug("Resulting MTU: %u", be16_to_cpu(out_icmp->un.frag.mtu));

#else
	out_icmp->un.frag.mtu = cpu_to_be16(1500);
#endif

	return 0;
}

/**
 * One liner for translating the ICMPv6's pointer field to ICMPv4.
 * "Pointer" is a field from "Parameter Problem" ICMP messages.
 */
static int icmp6_to_icmp4_param_prob_ptr(struct icmp6hdr *icmpv6_hdr,
		struct icmphdr *icmpv4_hdr)
{
	__u32 icmp6_ptr = be32_to_cpu(icmpv6_hdr->icmp6_dataun.un_data32[0]);
	__u32 icmp4_ptr;

	if (icmp6_ptr < 0 || 39 < icmp6_ptr)
		goto failure;

	switch (icmp6_ptr) {
	case 0:
		icmp4_ptr = 0;
		goto success;
	case 1:
		icmp4_ptr = 1;
		goto success;
	case 2:
	case 3:
		goto failure;
	case 4:
	case 5:
		icmp4_ptr = 2;
		goto success;
	case 6:
		icmp4_ptr = 9;
		goto success;
	case 7:
		icmp4_ptr = 8;
		goto success;
	}

	if (icmp6_ptr >= 24) {
		icmp4_ptr = 16;
		goto success;
	}
	if (icmp6_ptr >= 8) {
		icmp4_ptr = 12;
		goto success;
	}

	/* This is critical because the above ifs are supposed to cover all the possible values. */
	WARN(true, "Unknown pointer '%u' for parameter problem message.", icmp6_ptr);
	goto failure;

success:
	icmpv4_hdr->icmp4_unused = cpu_to_be32(icmp4_ptr << 24);
	return 0;
failure:
	log_debug("ICMP parameter problem pointer %u has no ICMP4 counterpart.", icmp6_ptr);
	return -EINVAL;
}

/**
 * One-liner for translating "Destination Unreachable" messages from ICMPv6 to ICMPv4.
 */
static int icmp6_to_icmp4_dest_unreach(struct icmp6hdr *icmpv6_hdr, struct icmphdr *icmpv4_hdr)
{
	icmpv4_hdr->type = ICMP_DEST_UNREACH;
	icmpv4_hdr->icmp4_unused = 0;

	switch (icmpv6_hdr->icmp6_code) {
	case ICMPV6_NOROUTE:
	case ICMPV6_NOT_NEIGHBOUR:
	case ICMPV6_ADDR_UNREACH:
		icmpv4_hdr->code = ICMP_HOST_UNREACH;
		break;

	case ICMPV6_ADM_PROHIBITED:
		icmpv4_hdr->code = ICMP_HOST_ANO;
		break;

	case ICMPV6_PORT_UNREACH:
		icmpv4_hdr->code = ICMP_PORT_UNREACH;
		break;

	default:
		log_debug("ICMPv6 messages type %u code %u do not exist in ICMPv4.",
				icmpv6_hdr->icmp6_type, icmpv6_hdr->icmp6_code);
		return -EINVAL;
	}

	return 0;
}

/**
 * One-liner for translating "Parameter Problem" messages from ICMPv6 to ICMPv4.
 */
static int icmp6_to_icmp4_param_prob(struct icmp6hdr *icmpv6_hdr, struct icmphdr *icmpv4_hdr)
{
	int error;

	switch (icmpv6_hdr->icmp6_code) {
	case ICMPV6_HDR_FIELD:
		icmpv4_hdr->type = ICMP_PARAMETERPROB;
		icmpv4_hdr->code = 0;
		error = icmp6_to_icmp4_param_prob_ptr(icmpv6_hdr, icmpv4_hdr);
		if (error)
			return error;
		break;

	case ICMPV6_UNK_NEXTHDR:
		icmpv4_hdr->type = ICMP_DEST_UNREACH;
		icmpv4_hdr->code = ICMP_PROT_UNREACH;
		icmpv4_hdr->icmp4_unused = 0;
		break;

	default:
		/* ICMPV6_UNK_OPTION is known to fall through here. */
		log_debug("ICMPv6 messages type %u code %u do not exist in ICMPv4.", icmpv6_hdr->icmp6_type,
				icmpv6_hdr->icmp6_code);
		return -EINVAL;
	}

	return 0;
}

static int buffer6_to_parts(struct ipv6hdr *hdr6, unsigned int len, struct pkt_parts *parts,
		int *field)
{
	struct hdr_iterator iterator;
	struct icmp6hdr *hdr_icmp;
	int error;

	error = validate_ipv6_integrity(hdr6, len, true, &iterator, field);
	if (error)
		return error;

	parts->l3_hdr.proto = L3PROTO_IPV6;
	parts->l3_hdr.len = iterator.data - (void *) hdr6;
	parts->l3_hdr.ptr = hdr6;
	parts->l4_hdr.ptr = iterator.data;

	switch (iterator.hdr_type) {
	case NEXTHDR_TCP:
		error = validate_lengths_tcp(len, parts->l3_hdr.len, iterator.data);
		if (error) {
			*field = IPSTATS_MIB_INTRUNCATEDPKTS;
			return error;
		}

		parts->l4_hdr.proto = L4PROTO_TCP;
		parts->l4_hdr.len = tcp_hdr_len(iterator.data);
		break;
	case NEXTHDR_UDP:
		error = validate_lengths_udp(len, parts->l3_hdr.len);
		if (error) {
			*field = IPSTATS_MIB_INTRUNCATEDPKTS;
			return error;
		}

		parts->l4_hdr.proto = L4PROTO_UDP;
		parts->l4_hdr.len = sizeof(struct udphdr);
		break;
	case NEXTHDR_ICMP:
		error = validate_lengths_icmp6(len, parts->l3_hdr.len);
		if (error) {
			*field = IPSTATS_MIB_INTRUNCATEDPKTS;
			return error;
		}
		hdr_icmp = parts->l4_hdr.ptr;
		if (icmpv6_has_inner_packet(hdr_icmp->icmp6_type)) {
			*field = IPSTATS_MIB_INHDRERRORS;
			return -EINVAL; /* packet inside packet inside packet. */
		}

		parts->l4_hdr.proto = L4PROTO_ICMP;
		parts->l4_hdr.len = sizeof(struct icmp6hdr);
		break;
	default:
		/*
		 * Why are we translating a error packet of a packet we couldn't have translated?
		 * Either an attack or shouldn't happen, so drop silently.
		 */
		*field = IPSTATS_MIB_INUNKNOWNPROTOS;
		return -EINVAL;
	}

	parts->payload.len = len - parts->l3_hdr.len - parts->l4_hdr.len;
	parts->payload.ptr = parts->l4_hdr.ptr + parts->l4_hdr.len;
	parts->skb = NULL;

	return 0;
}

static bool is_truncated_ipv4(struct pkt_parts *parts)
{
	struct iphdr *hdr4;
	struct udphdr *hdr_udp;

	switch (parts->l4_hdr.proto) {
	case L4PROTO_TCP:
	case L4PROTO_ICMP:
		/* Calculating the checksum doesn't hurt. Not calculating it might. */
		return false;
	case L4PROTO_UDP:
		hdr4 = parts->l3_hdr.ptr;
		hdr_udp = parts->l4_hdr.ptr;
		return (ntohs(hdr4->tot_len) - (4 * hdr4->ihl)) != ntohs(hdr_udp->len);
	}

	return true; /* whatever. */
}

static bool is_csum4_computable(struct pkt_parts *parts)
{
	if (!is_first_fragment_ipv4(parts->l3_hdr.ptr))
		return false;

	if (!is_inner_pkt(parts))
		return true;

	if (is_truncated_ipv4(parts))
		return false;

	if (is_fragmented_ipv4(parts->l3_hdr.ptr))
		return false;

	return true;
}

/*
 * Use this when only the ICMP header changed, so all there is to do is subtract the old data from
 * the checksum and add the new one.
 */
static int update_icmp4_csum(struct pkt_parts *in, struct pkt_parts *out)
{
	struct ipv6hdr *in_ip6 = in->l3_hdr.ptr;
	struct icmp6hdr *in_icmp = in->l4_hdr.ptr;
	struct icmphdr *out_icmp = out->l4_hdr.ptr;
	struct icmp6hdr copy_hdr;
	unsigned int len;
	int error;
	__wsum csum, tmp;

	if (is_inner_pkt(out)) {
		len = out->l4_hdr.len + out->payload.len;
	} else {
		error = skb_aggregate_ipv6_payload_len(in->skb, &len);
		if (error) {
			inc_stats(out->skb, IPSTATS_MIB_OUTDISCARDS);
			return error;
		}
	}

	csum = ~csum_unfold(in_icmp->icmp6_cksum);

	/* Remove the ICMPv6 pseudo-header. */
	tmp = ~csum_unfold(csum_ipv6_magic(&in_ip6->saddr, &in_ip6->daddr, len, NEXTHDR_ICMP, 0));
	csum = csum_sub(csum, tmp);

	/*
	 * Remove the ICMPv6 header.
	 * I'm working on a copy because I need to zero out its checksum.
	 * If I did that directly on the skb, I suspect I'd need to make it writable first.
	 */
	memcpy(&copy_hdr, in_icmp, sizeof(*in_icmp));
	copy_hdr.icmp6_cksum = 0;
	tmp = csum_partial(&copy_hdr, sizeof(copy_hdr), 0);
	csum = csum_sub(csum, tmp);

	/* Add the ICMPv4 header. There's no ICMPv4 pseudo-header. */
	out_icmp->checksum = 0;
	tmp = csum_partial(out_icmp, sizeof(*out_icmp), 0);
	csum = csum_add(csum, tmp);

	out_icmp->checksum = csum_fold(csum);
	return 0;
}

/**
 * Use this when header and payload both changed completely, so we gotta just trash the old
 * checksum and start anew.
 */
static int compute_icmp4_csum(struct pkt_parts *out)
{
	struct icmphdr *hdr = out->l4_hdr.ptr;
	__wsum csum;

	hdr->checksum = 0;
	csum = csum_partial(hdr, out->l4_hdr.len, 0);
	csum = csum_partial(out->payload.ptr, out->payload.len, csum);
	hdr->checksum = csum_fold(csum);

	return 0;
}

static int post_icmp4info(struct pkt_parts *in, struct pkt_parts *out)
{
	memcpy(out->payload.ptr, in->payload.ptr, in->payload.len);
	return is_csum4_computable(out) ? update_icmp4_csum(in, out) : 0;
}

static int post_icmp4error(struct tuple *tuple4, struct pkt_parts *in_outer,
		struct pkt_parts *out_outer, int *field)
{
	struct pkt_parts in_inner;
	int error;

	log_debug("Translating the inner packet (6->4)...");

	memset(&in_inner, 0, sizeof(in_inner));
	error = buffer6_to_parts(in_outer->payload.ptr, in_outer->payload.len, &in_inner, field);
	if (error)
		return error;

	error = ttpcomm_translate_inner_packet(tuple4, &in_inner, out_outer);
	if (error)
		return error;

	if (is_csum4_computable(out_outer))
		error = compute_icmp4_csum(out_outer);

	return error;
}

/**
 * Translates in's icmp6 header and payload into out's icmp4 header and payload.
 * This is the core of RFC 6145 sections 5.2 and 5.3, except checksum (See post_icmp4()).
 */
int ttp64_icmp(struct tuple* tuple4, struct pkt_parts *in, struct pkt_parts *out)
{
	struct icmp6hdr *icmpv6_hdr = in->l4_hdr.ptr;
	struct icmphdr *icmpv4_hdr = out->l4_hdr.ptr;
	int field = 0;
	int error = 0;

	switch (icmpv6_hdr->icmp6_type) {
	case ICMPV6_ECHO_REQUEST:
		icmpv4_hdr->type = ICMP_ECHO;
		icmpv4_hdr->code = 0;
		icmpv4_hdr->un.echo.id = cpu_to_be16(tuple4->icmp4_id);
		icmpv4_hdr->un.echo.sequence = icmpv6_hdr->icmp6_dataun.u_echo.sequence;
		error = post_icmp4info(in, out);
		break;

	case ICMPV6_ECHO_REPLY:
		icmpv4_hdr->type = ICMP_ECHOREPLY;
		icmpv4_hdr->code = 0;
		icmpv4_hdr->un.echo.id = cpu_to_be16(tuple4->icmp4_id);
		icmpv4_hdr->un.echo.sequence = icmpv6_hdr->icmp6_dataun.u_echo.sequence;
		error = post_icmp4info(in, out);
		break;

	case ICMPV6_DEST_UNREACH:
		error = icmp6_to_icmp4_dest_unreach(icmpv6_hdr, icmpv4_hdr);
		if (error) {
			inc_stats(in->skb, IPSTATS_MIB_INHDRERRORS);
			return error;
		}
		error = post_icmp4error(tuple4, in, out, &field);
		break;

	case ICMPV6_PKT_TOOBIG:
		/*
		 * BTW, I have no idea what the RFC means by "taking into account whether or not
		 * the packet in error includes a Fragment Header"... What does the fragment header
		 * have to do with anything here?
		 */
		icmpv4_hdr->type = ICMP_DEST_UNREACH;
		icmpv4_hdr->code = ICMP_FRAG_NEEDED;
		icmpv4_hdr->un.frag.__unused = htons(0);
		error = compute_mtu4(in->skb, out->skb);
		if (error)
			return error;
		error = post_icmp4error(tuple4, in, out, &field);
		break;

	case ICMPV6_TIME_EXCEED:
		icmpv4_hdr->type = ICMP_TIME_EXCEEDED;
		icmpv4_hdr->code = icmpv6_hdr->icmp6_code;
		icmpv4_hdr->icmp4_unused = 0;
		error = post_icmp4error(tuple4, in, out, &field);
		break;

	case ICMPV6_PARAMPROB:
		error = icmp6_to_icmp4_param_prob(icmpv6_hdr, icmpv4_hdr);
		if (error) {
			inc_stats(in->skb, IPSTATS_MIB_INHDRERRORS);
			return error;
		}
		error = post_icmp4error(tuple4, in, out, &field);
		break;

	default:
		/*
		 * The following codes are known to fall through here:
		 * ICMPV6_MGM_QUERY, ICMPV6_MGM_REPORT, ICMPV6_MGM_REDUCTION,
		 * Neighbor Discover messages (133 - 137).
		 */
		log_debug("ICMPv6 messages type %u do not exist in ICMPv4.", icmpv6_hdr->icmp6_type);
		error = -EINVAL;
	}

	if (field)
		inc_stats(in->skb, field);

	return error;
}

static __sum16 update_csum_6to4(__sum16 csum16,
		struct ipv6hdr *in_ip6, void *in_l4_hdr, size_t in_l4_hdr_len,
		struct iphdr *out_ip4, void *out_l4_hdr, size_t out_l4_hdr_len)
{
	__wsum csum, pseudohdr_csum;

	csum = ~csum_unfold(csum16);

	/*
	 * Regarding the pseudoheaders:
	 * The length is pretty hard to obtain if there's fragmentation, and whatever it is,
	 * it's not going to change. Therefore, instead of computing it only to cancel it out with
	 * itself later, simply sum (and substract) zero.
	 * Do the same with proto since we're feeling hackish.
	 */

	/* Remove the IPv6 crap. */
	pseudohdr_csum = ~csum_unfold(csum_ipv6_magic(&in_ip6->saddr, &in_ip6->daddr, 0, 0, 0));
	csum = csum_sub(csum, pseudohdr_csum);
	csum = csum_sub(csum, csum_partial(in_l4_hdr, in_l4_hdr_len, 0));

	/* Add the IPv4 crap. */
	pseudohdr_csum = csum_tcpudp_nofold(out_ip4->saddr, out_ip4->daddr, 0, 0, 0);
	csum = csum_add(csum, pseudohdr_csum);
	csum = csum_add(csum, csum_partial(out_l4_hdr, out_l4_hdr_len, 0));

	return csum_fold(csum);
}

int ttp64_tcp(struct tuple *tuple4, struct pkt_parts *in, struct pkt_parts *out)
{
	struct tcphdr *tcp_in = in->l4_hdr.ptr;
	struct tcphdr *tcp_out = out->l4_hdr.ptr;
	struct tcphdr tcp_copy;

	/* Header */
	memcpy(tcp_out, tcp_in, in->l4_hdr.len);
	tcp_out->source = cpu_to_be16(tuple4->src.addr4.l4);
	tcp_out->dest = cpu_to_be16(tuple4->dst.addr4.l4);

	if (is_csum4_computable(out)) {
		memcpy(&tcp_copy, tcp_in, sizeof(*tcp_in));
		tcp_copy.check = 0;

		tcp_out->check = 0;
		tcp_out->check = update_csum_6to4(tcp_in->check,
				in->l3_hdr.ptr, &tcp_copy, sizeof(tcp_copy),
				out->l3_hdr.ptr, tcp_out, sizeof(*tcp_out));
	}

	/* Payload */
	memcpy(out->payload.ptr, in->payload.ptr, in->payload.len);

	return 0;
}

int ttp64_udp(struct tuple *tuple4, struct pkt_parts *in, struct pkt_parts *out)
{
	struct udphdr *udp_in = in->l4_hdr.ptr;
	struct udphdr *udp_out = out->l4_hdr.ptr;
	struct udphdr udp_copy;

	/* Header */
	udp_out->source = cpu_to_be16(tuple4->src.addr4.l4);
	udp_out->dest = cpu_to_be16(tuple4->dst.addr4.l4);
	udp_out->len = udp_in->len;
	udp_out->check = 0;

	if (is_csum4_computable(out)) {
		memcpy(&udp_copy, udp_in, sizeof(*udp_in));
		udp_copy.check = 0;

		udp_out->check = 0;
		udp_out->check = update_csum_6to4(udp_in->check,
				in->l3_hdr.ptr, &udp_copy, sizeof(udp_copy),
				out->l3_hdr.ptr, udp_out, sizeof(*udp_out));
	}
	if (udp_out->check == 0)
		udp_out->check = CSUM_MANGLED_0;

	/* Payload */
	memcpy(out->payload.ptr, in->payload.ptr, in->payload.len);

	return 0;
}
