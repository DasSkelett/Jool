#include "nat64/mod/common/rfc6145/6to4.h"

#include "nat64/mod/common/config.h"
#include "nat64/mod/common/icmp_wrapper.h"
#include "nat64/mod/common/ipv6_hdr_iterator.h"
#include "nat64/mod/common/pool6.h"
#include "nat64/mod/common/rfc6052.h"
#include "nat64/mod/common/stats.h"
#include "nat64/mod/common/route.h"
#include "nat64/mod/stateless/blacklist4.h"
#include "nat64/mod/stateless/rfc6791.h"
#include "nat64/mod/stateless/eam.h"

verdict ttp64_create_skb(struct packet *in, struct packet *out)
{
	unsigned int total_len;
	struct sk_buff *skb;
	bool is_first;

	is_first = is_first_frag6(pkt_frag_hdr(in));

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
	 * The subpayload will never change in size (unless it gets truncated later, but that's send
	 * packet's responsibility).
	 */
	total_len = sizeof(struct iphdr) + pkt_l3payload_len(in);
	if (is_first && pkt_is_icmp6_error(in)) {
		struct hdr_iterator iterator = HDR_ITERATOR_INIT((struct ipv6hdr *) pkt_payload(in));
		hdr_iterator_last(&iterator);

		/* Add the IPv4 subheader, remove the IPv6 subheaders. */
		total_len += sizeof(struct iphdr) - (iterator.data - pkt_payload(in));

		/* RFC1812 section 4.3.2.3. I'm using a literal because the RFC does. */
		if (total_len > 576)
			total_len = 576;
	}

	skb = alloc_skb(LL_MAX_HEADER + total_len, GFP_ATOMIC);
	if (!skb) {
		inc_stats(in, IPSTATS_MIB_INDISCARDS);
		return VERDICT_DROP;
	}

	skb_reserve(skb, LL_MAX_HEADER);
	skb_put(skb, total_len);
	skb_reset_mac_header(skb);
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, sizeof(struct iphdr));

	pkt_fill(out, skb, L3PROTO_IPV4, pkt_l4_proto(in),
			NULL, skb_transport_header(skb) + pkt_l4hdr_len(in),
			pkt_original_pkt(in));

	skb->mark = in->skb->mark;
	skb->protocol = htons(ETH_P_IP);

	return VERDICT_CONTINUE;
}

static __u8 __xlat_tos(bool reset_tos, __u8 new_tos, struct ipv6hdr *hdr)
{
	return reset_tos ? new_tos : get_traffic_class(hdr);
}

__u8 ttp64_xlat_tos(struct ipv6hdr *hdr)
{
	bool reset_tos;
	__u8 new_tos;
	config_get_hdr4_config(&reset_tos, &new_tos, NULL, NULL);
	return __xlat_tos(reset_tos, new_tos, hdr);
}

/**
 * One-liner for creating the IPv4 header's Total Length field.
 */
static __be16 build_tot_len(struct packet *in, struct packet *out)
{
	/*
	 * The RFC's equation is plain wrong, as the errata claims.
	 * However, this still looks different than the proposed version because:
	 *
	 * - I don't know what all that ESP stuff is since ESP is not supposed to be translated.
	 *   TODO (warning) actually, 6145bis defines semantics for ESP.
	 * - ICMP error quirks the RFC doesn't account for:
	 *
	 * ICMPv6 errors are supposed to be max 1280 bytes.
	 * ICMPv4 errors are supposed to be max 576 bytes.
	 * Therefore, the resulting ICMP4 packet might have a smaller payload than the original packet.
	 *
	 * This is further complicated by the kernel's fragmentation hacks; we can't do
	 * "result = skb_len(out)" because the first fragment's tot_len has to also cover the rest of
	 * the fragments...
	 *
	 * SIGH.
	 */

	__u16 total_len;

	if (pkt_is_inner(out)) { /* Inner packet. */
		total_len = get_tot_len_ipv6(in->skb) - pkt_hdrs_len(in) + pkt_hdrs_len(out);

	} else if (!pkt_is_fragment(out)) { /* Not fragment. */
		total_len = out->skb->len;
		if (pkt_is_icmp4_error(out) && total_len > 576)
			total_len = 576;

	} else if (skb_shinfo(out->skb)->frag_list) { /* First fragment. */
		/* This would also normally be "result = out->len", but out->len is incomplete. */
		total_len = in->skb->len - pkt_hdrs_len(in) + pkt_hdrs_len(out);

	} /* (subsequent fragments don't reach this code.) */

	return cpu_to_be16(total_len);
}

/**
 * One-liner for creating the IPv4 header's Identification field.
 * It assumes that the packet will not contain a fragment header.
 */
static __be16 generate_ipv4_id_nofrag(struct packet *skb_out)
{
	__be16 random;

	if (pkt_len(skb_out) <= 1260) {
		get_random_bytes(&random, 2);
		return random;
	}

	return 0; /* Because the DF flag will be set. */
}

/**
 * One-liner for creating the IPv4 header's Dont Fragment flag.
 */
static bool generate_df_flag(struct packet *pkt_out)
{
	return pkt_len(pkt_out) > 1260;
}

/**
 * One-liner for creating the IPv4 header's Protocol field.
 */
__u8 ttp64_xlat_proto(struct ipv6hdr *hdr6)
{
	struct hdr_iterator iterator = HDR_ITERATOR_INIT(hdr6);
	hdr_iterator_last(&iterator);
	return (iterator.hdr_type == NEXTHDR_ICMP) ? IPPROTO_ICMP : iterator.hdr_type;
}

static addrxlat_verdict generate_addr4_siit(struct in6_addr *addr6,
		__be32 *addr4, bool *was_6052)
{
	struct ipv6_prefix prefix;
	struct in_addr tmp;
	int error;

	*was_6052 = false;

	error = eamt_xlat_6to4(addr6, &tmp);
	if (!error)
		goto success;
	if (error != -ESRCH)
		return ADDRXLAT_DROP;

	error = pool6_get(addr6, &prefix);
	if (error == -ESRCH) {
		log_debug("Address %pI6c lacks the NAT64 prefix and an EAMT entry.",
				addr6);
		return ADDRXLAT_TRY_SOMETHING_ELSE;
	}
	if (error)
		return ADDRXLAT_DROP;

	error = addr_6to4(addr6, &prefix, &tmp);
	if (error)
		return ADDRXLAT_DROP;

	if (blacklist_contains(&tmp)) {
		log_debug("The resulting address (%pI4) is blacklisted.", &tmp);
		return ADDRXLAT_ACCEPT;
	}

	*was_6052 = true;
	/* Fall through. */

success:
	if (must_not_translate(&tmp)) {
		log_debug("The resulting address (%pI4) is not supposed to be xlat'd.",
				&tmp);
		return ADDRXLAT_ACCEPT;
	}

	*addr4 = tmp.s_addr;
	return ADDRXLAT_CONTINUE;
}

static verdict translate_addrs64_siit(struct packet *in, struct packet *out)
{
	struct ipv6hdr *hdr6 = pkt_ip6_hdr(in);
	struct iphdr *hdr4 = pkt_ip4_hdr(out);
	bool src_was_6052, dst_was_6052;
	addrxlat_verdict result;

	/* Dst address. (SRC DEPENDS CON DST, SO WE NEED TO XLAT DST FIRST!) */
	result = generate_addr4_siit(&hdr6->daddr, &hdr4->daddr, &dst_was_6052);
	switch (result) {
	case ADDRXLAT_CONTINUE:
		break;
	case ADDRXLAT_TRY_SOMETHING_ELSE:
		return VERDICT_ACCEPT;
	case ADDRXLAT_ACCEPT:
	case ADDRXLAT_DROP:
		return (verdict)result;
	}

	/* Src address. */
	result = generate_addr4_siit(&hdr6->saddr, &hdr4->saddr, &src_was_6052);
	switch (result) {
	case ADDRXLAT_CONTINUE:
		break;
	case ADDRXLAT_TRY_SOMETHING_ELSE:
		if (pkt_is_icmp6_error(in)
				&& !rfc6791_get(in, out, &hdr4->saddr))
			break; /* Ok, success. */
		return VERDICT_ACCEPT;
	case ADDRXLAT_ACCEPT:
	case ADDRXLAT_DROP:
		return (verdict)result;
 	}

	/*
	 * Mark intrinsic hairpinning if it's going to be needed.
	 * Why here? It's the only place where we know whether RFC 6052 was
	 * involved.
	 * See the EAM draft.
	 */
	if (config_eam_hairpin_mode() == EAM_HAIRPIN_INTRINSIC) {
		/* Condition set A */
		if (pkt_is_outer(in) && !pkt_is_icmp6_error(in)
				&& dst_was_6052
				&& eamt_contains4(hdr4->daddr)) {
			out->is_hairpin = true;

		/* Condition set B */
		} else if (pkt_is_inner(in)
				&& src_was_6052
				&& eamt_contains4(hdr4->saddr)) {
			out->is_hairpin = true;
		}
	}

	log_debug("Result: %pI4->%pI4", &hdr4->saddr, &hdr4->daddr);
	return VERDICT_CONTINUE;
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
	unsigned int offset;

	rt_hdr = hdr_iterator_find(ip6_hdr, NEXTHDR_ROUTING);
	if (!rt_hdr)
		return false;

	if (rt_hdr->segments_left == 0)
		return false;

	offset = (void *) rt_hdr - (void *) ip6_hdr;
	*field_location = offset + offsetof(struct ipv6_rt_hdr, segments_left);
	return true;
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
 * This is RFC 6145 sections 5.1 and 5.1.1.
 *
 * Aside from the main call (to translate a normal IPv6 packet's layer 3 header), this function can
 * also be called to translate a packet's inner packet.
 */
verdict ttp64_ipv4(struct tuple *tuple4, struct packet *in, struct packet *out)
{
	struct ipv6hdr *ip6_hdr = pkt_ip6_hdr(in);
	struct frag_hdr *ip6_frag_hdr;
	struct iphdr *ip4_hdr = pkt_ip4_hdr(out);
	verdict result;

	bool reset_tos, build_ipv4_id, df_always_on;
	__u8 new_tos, dont_fragment;

	config_get_hdr4_config(&reset_tos, &new_tos, &build_ipv4_id,
			&df_always_on);

	/*
	 * translate_addrs64_siit->rfc6791_get->get_host_address needs tos
	 * and protocol, so translate them first.
	 */
	ip4_hdr->tos = __xlat_tos(reset_tos, new_tos, ip6_hdr);
	ip4_hdr->protocol = ttp64_xlat_proto(ip6_hdr);

	/* Translate the address before TTL because of issue #167. */
	if (xlat_is_nat64()) {
		ip4_hdr->saddr = tuple4->src.addr4.l3.s_addr;
		ip4_hdr->daddr = tuple4->dst.addr4.l3.s_addr;
	} else {
		result = translate_addrs64_siit(in, out);
		if (result != VERDICT_CONTINUE)
			return result;
	}

	ip4_hdr->version = 4;
	ip4_hdr->ihl = 5;
	ip4_hdr->tot_len = build_tot_len(in, out);
	ip4_hdr->id = build_ipv4_id ? generate_ipv4_id_nofrag(out) : 0;
	dont_fragment = df_always_on ? 1 : generate_df_flag(out);
	ip4_hdr->frag_off = build_ipv4_frag_off_field(dont_fragment, 0, 0);
	if (pkt_is_outer(in)) {
		if (ip6_hdr->hop_limit <= 1) {
			icmp64_send(in, ICMPERR_HOP_LIMIT, 0);
			inc_stats(in, IPSTATS_MIB_INHDRERRORS);
			return VERDICT_DROP;
		}
		ip4_hdr->ttl = ip6_hdr->hop_limit - 1;
	} else {
		ip4_hdr->ttl = ip6_hdr->hop_limit;
	}
	/* ip4_hdr->check is set later; please scroll down. */

	if (pkt_is_outer(in)) {
		__u32 nonzero_location;
		if (has_nonzero_segments_left(ip6_hdr, &nonzero_location)) {
			log_debug("Packet's segments left field is nonzero.");
			icmp64_send(in, ICMPERR_HDR_FIELD, nonzero_location);
			inc_stats(in, IPSTATS_MIB_INHDRERRORS);
			return VERDICT_DROP;
		}
	}

	ip6_frag_hdr = pkt_frag_hdr(in);
	if (ip6_frag_hdr) {
		/* The logic above already includes the frag header in tot_len. */
		ip4_hdr->id = generate_ipv4_id_dofrag(ip6_frag_hdr);
		ip4_hdr->frag_off = build_ipv4_frag_off_field(0,
				is_more_fragments_set_ipv6(ip6_frag_hdr),
				get_fragment_offset_ipv6(ip6_frag_hdr));
		/* protocol also doesn't need tweaking. */
	}

	ip4_hdr->check = 0;
	ip4_hdr->check = ip_fast_csum(ip4_hdr, ip4_hdr->ihl);

	/*
	 * The kernel already drops packets if they don't allow fragmentation
	 * and the next hop MTU is smaller than their size.
	 */

	/* Adapt to kernel hacks. */
	if (skb_shinfo(in->skb)->frag_list)
		ip4_hdr->frag_off &= cpu_to_be16(~IP_MF);

	return VERDICT_CONTINUE;
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

static int compute_mtu4(struct packet *in, struct packet *out)
{
	struct icmphdr *out_icmp = pkt_icmp4_hdr(out);
#ifndef UNIT_TESTING
	struct dst_entry *out_dst;
	struct icmp6hdr *in_icmp = pkt_icmp6_hdr(in);

	out_dst = route4(out);
	if (!out_dst)
		return -EINVAL;
	if (!in->skb->dev)
		return -EINVAL;

	log_debug("Packet MTU: %u", be32_to_cpu(in_icmp->icmp6_mtu));
	log_debug("In dev MTU: %u", in->skb->dev->mtu);
	log_debug("Out dev MTU: %u", out_dst->dev->mtu);

	out_icmp->un.frag.mtu = icmp4_minimum_mtu(be32_to_cpu(in_icmp->icmp6_mtu) - 20,
			out_dst->dev->mtu,
			in->skb->dev->mtu - 20);
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

/*
 * Use this when only the ICMP header changed, so all there is to do is subtract the old data from
 * the checksum and add the new one.
 */
static int update_icmp4_csum(struct packet *in, struct packet *out)
{
	struct ipv6hdr *in_ip6 = pkt_ip6_hdr(in);
	struct icmp6hdr *in_icmp = pkt_icmp6_hdr(in);
	struct icmphdr *out_icmp = pkt_icmp4_hdr(out);
	struct icmp6hdr copy_hdr;
	__wsum csum, tmp;

	csum = ~csum_unfold(in_icmp->icmp6_cksum);

	/* Remove the ICMPv6 pseudo-header. */
	tmp = ~csum_unfold(csum_ipv6_magic(&in_ip6->saddr, &in_ip6->daddr, pkt_datagram_len(in),
			NEXTHDR_ICMP, 0));
	csum = csum_sub(csum, tmp);

	/*
	 * Remove the ICMPv6 header.
	 * I'm working on a copy because I need to zero out its checksum.
	 * If I did that directly on the skb, I'd need to make it writable first.
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
static int compute_icmp4_csum(struct packet *out)
{
	struct icmphdr *hdr = pkt_icmp4_hdr(out);

	/* This function only gets called for ICMP error checksums, so skb_datagram_len() is fine. */
	hdr->checksum = 0;
	hdr->checksum = csum_fold(skb_checksum(out->skb, skb_transport_offset(out->skb),
			pkt_datagram_len(out), 0));
	out->skb->ip_summed = CHECKSUM_NONE;

	return 0;
}

static verdict validate_icmp6_csum(struct packet *in)
{
	struct ipv6hdr *hdr6;
	unsigned int len;
	__sum16 csum;

	if (in->skb->ip_summed != CHECKSUM_NONE)
		return VERDICT_CONTINUE;

	hdr6 = pkt_ip6_hdr(in);
	len = pkt_datagram_len(in);
	csum = csum_ipv6_magic(&hdr6->saddr, &hdr6->daddr, len, NEXTHDR_ICMP,
			skb_checksum(in->skb, skb_transport_offset(in->skb),
					len, 0));
	if (csum != 0) {
		log_debug("Checksum doesn't match.");
		inc_stats(in, IPSTATS_MIB_INHDRERRORS);
		return VERDICT_DROP;
	}

	return VERDICT_CONTINUE;
}

static int post_icmp4info(struct packet *in, struct packet *out)
{
	int error;

	error = copy_payload(in, out);
	if (error)
		return error;

	return update_icmp4_csum(in, out);
}

static verdict post_icmp4error(struct tuple *tuple4, struct packet *in, struct packet *out)
{
	verdict result;

	log_debug("Translating the inner packet (6->4)...");

	result = validate_icmp6_csum(in);
	if (result != VERDICT_CONTINUE)
		return result;

	result = ttpcomm_translate_inner_packet(tuple4, in, out);
	if (result != VERDICT_CONTINUE)
		return result;

	return compute_icmp4_csum(out) ? VERDICT_DROP : VERDICT_CONTINUE;
}

/**
 * Translates in's icmp6 header and payload into out's icmp4 header and payload.
 * This is the core of RFC 6145 sections 5.2 and 5.3, except checksum (See post_icmp4*()).
 */
verdict ttp64_icmp(struct tuple* tuple4, struct packet *in, struct packet *out)
{
	struct icmp6hdr *icmpv6_hdr = pkt_icmp6_hdr(in);
	struct icmphdr *icmpv4_hdr = pkt_icmp4_hdr(out);
	int error = 0;

	icmpv4_hdr->checksum = icmpv6_hdr->icmp6_cksum; /* default. */

	switch (icmpv6_hdr->icmp6_type) {
	case ICMPV6_ECHO_REQUEST:
		icmpv4_hdr->type = ICMP_ECHO;
		icmpv4_hdr->code = 0;
		icmpv4_hdr->un.echo.id = xlat_is_nat64()
				? cpu_to_be16(tuple4->icmp4_id)
				: icmpv6_hdr->icmp6_identifier;
		icmpv4_hdr->un.echo.sequence = icmpv6_hdr->icmp6_dataun.u_echo.sequence;
		error = post_icmp4info(in, out);
		break;

	case ICMPV6_ECHO_REPLY:
		icmpv4_hdr->type = ICMP_ECHOREPLY;
		icmpv4_hdr->code = 0;
		icmpv4_hdr->un.echo.id = xlat_is_nat64()
				? cpu_to_be16(tuple4->icmp4_id)
				: icmpv6_hdr->icmp6_identifier;
		icmpv4_hdr->un.echo.sequence = icmpv6_hdr->icmp6_dataun.u_echo.sequence;
		error = post_icmp4info(in, out);
		break;

	case ICMPV6_DEST_UNREACH:
		error = icmp6_to_icmp4_dest_unreach(icmpv6_hdr, icmpv4_hdr);
		if (error) {
			inc_stats(in, IPSTATS_MIB_INHDRERRORS);
			return VERDICT_DROP;
		}
		return post_icmp4error(tuple4, in, out);

	case ICMPV6_PKT_TOOBIG:
		/*
		 * BTW, I have no idea what the RFC means by "taking into account whether or not
		 * the packet in error includes a Fragment Header"... What does the fragment header
		 * have to do with anything here?
		 */
		icmpv4_hdr->type = ICMP_DEST_UNREACH;
		icmpv4_hdr->code = ICMP_FRAG_NEEDED;
		icmpv4_hdr->un.frag.__unused = htons(0);
		error = compute_mtu4(in, out);
		if (error)
			return VERDICT_DROP;
		return post_icmp4error(tuple4, in, out);

	case ICMPV6_TIME_EXCEED:
		icmpv4_hdr->type = ICMP_TIME_EXCEEDED;
		icmpv4_hdr->code = icmpv6_hdr->icmp6_code;
		icmpv4_hdr->icmp4_unused = 0;
		return post_icmp4error(tuple4, in, out);

	case ICMPV6_PARAMPROB:
		error = icmp6_to_icmp4_param_prob(icmpv6_hdr, icmpv4_hdr);
		if (error) {
			inc_stats(in, IPSTATS_MIB_INHDRERRORS);
			return VERDICT_DROP;
		}
		return post_icmp4error(tuple4, in, out);

	default:
		/*
		 * The following codes are known to fall through here:
		 * ICMPV6_MGM_QUERY, ICMPV6_MGM_REPORT, ICMPV6_MGM_REDUCTION,
		 * Neighbor Discover messages (133 - 137).
		 */
		log_debug("ICMPv6 messages type %u do not exist in ICMPv4.", icmpv6_hdr->icmp6_type);
		return VERDICT_DROP;
	}

	return error ? VERDICT_DROP : VERDICT_CONTINUE;
}

static __sum16 update_csum_6to4(__sum16 csum16,
		struct ipv6hdr *in_ip6, void *in_l4_hdr, size_t in_l4_hdr_len,
		struct iphdr *out_ip4, void *out_l4_hdr, size_t out_l4_hdr_len)
{
	__wsum csum, pseudohdr_csum;

	csum = ~csum_unfold(csum16);

	/*
	 * Regarding the pseudoheaders:
	 * The length is pretty hard to obtain if there's TCP and fragmentation,
	 * and whatever it is, it's not going to change. Therefore, instead of
	 * computing it only to cancel it out with itself later, simply sum
	 * (and substract) zero.
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

static __sum16 update_csum_6to4_partial(__sum16 csum16, struct ipv6hdr *in_ip6,
		struct iphdr *out_ip4)
{
	__wsum csum, pseudohdr_csum;

	csum = csum_unfold(csum16);

	pseudohdr_csum = ~csum_unfold(csum_ipv6_magic(&in_ip6->saddr, &in_ip6->daddr, 0, 0, 0));
	csum = csum_sub(csum, pseudohdr_csum);

	pseudohdr_csum = csum_tcpudp_nofold(out_ip4->saddr, out_ip4->daddr, 0, 0, 0);
	csum = csum_add(csum, pseudohdr_csum);

	return ~csum_fold(csum);
}

verdict ttp64_tcp(struct tuple *tuple4, struct packet *in, struct packet *out)
{
	struct tcphdr *tcp_in = pkt_tcp_hdr(in);
	struct tcphdr *tcp_out = pkt_tcp_hdr(out);
	struct tcphdr tcp_copy;

	/* Header */
	memcpy(tcp_out, tcp_in, pkt_l4hdr_len(in));
	if (xlat_is_nat64()) {
		tcp_out->source = cpu_to_be16(tuple4->src.addr4.l4);
		tcp_out->dest = cpu_to_be16(tuple4->dst.addr4.l4);
	}

	/* Header.checksum */
	if (in->skb->ip_summed != CHECKSUM_PARTIAL) {
		memcpy(&tcp_copy, tcp_in, sizeof(*tcp_in));
		tcp_copy.check = 0;

		tcp_out->check = 0;
		tcp_out->check = update_csum_6to4(tcp_in->check,
				pkt_ip6_hdr(in), &tcp_copy, sizeof(tcp_copy),
				pkt_ip4_hdr(out), tcp_out, sizeof(*tcp_out));
		out->skb->ip_summed = CHECKSUM_NONE;
	} else {
		tcp_out->check = update_csum_6to4_partial(tcp_in->check,
				pkt_ip6_hdr(in), pkt_ip4_hdr(out));
		partialize_skb(out->skb, offsetof(struct tcphdr, check));
	}

	/* Payload */
	return copy_payload(in, out) ? VERDICT_DROP : VERDICT_CONTINUE;
}

verdict ttp64_udp(struct tuple *tuple4, struct packet *in, struct packet *out)
{
	struct udphdr *udp_in = pkt_udp_hdr(in);
	struct udphdr *udp_out = pkt_udp_hdr(out);
	struct udphdr udp_copy;

	/* Header */
	memcpy(udp_out, udp_in, pkt_l4hdr_len(in));
	if (xlat_is_nat64()) {
		udp_out->source = cpu_to_be16(tuple4->src.addr4.l4);
		udp_out->dest = cpu_to_be16(tuple4->dst.addr4.l4);
	}

	/* Header.checksum */
	if (in->skb->ip_summed != CHECKSUM_PARTIAL) {
		memcpy(&udp_copy, udp_in, sizeof(*udp_in));
		udp_copy.check = 0;

		udp_out->check = 0;
		udp_out->check = update_csum_6to4(udp_in->check,
				pkt_ip6_hdr(in), &udp_copy, sizeof(udp_copy),
				pkt_ip4_hdr(out), udp_out, sizeof(*udp_out));
		if (udp_out->check == 0)
			udp_out->check = CSUM_MANGLED_0;
		out->skb->ip_summed = CHECKSUM_NONE;
	} else {
		udp_out->check = update_csum_6to4_partial(udp_in->check,
				pkt_ip6_hdr(in), pkt_ip4_hdr(out));
		partialize_skb(out->skb, offsetof(struct udphdr, check));
	}

	/* Payload */
	return copy_payload(in, out) ? VERDICT_DROP : VERDICT_CONTINUE;
}
