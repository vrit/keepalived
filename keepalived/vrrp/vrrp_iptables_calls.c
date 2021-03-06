/*
 * Soft:        Keepalived is a failover program for the LVS project
 *              <www.linuxvirtualserver.org>. It monitor & manipulate
 *              a loadbalanced server pool using multi-layer checks.
 *
 * Part:        iptables manipulation.
 *
 * Author:      Quentin Armitage, <quentin@armitage.org.uk>
 *
 *              This program is distributed in the hope that it will be useful,
 *              but WITHOUT ANY WARRANTY; without even the implied warranty of
 *              MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *              See the GNU General Public License for more details.
 *
 *              This program is free software; you can redistribute it and/or
 *              modify it under the terms of the GNU General Public License
 *              as published by the Free Software Foundation; either version
 *              2 of the License, or (at your option) any later version.
 *
 * Copyright (C) 2001-2016 Alexandre Cassen, <acassen@gmail.com>
 */

#include <xtables.h>
#include <libiptc/libiptc.h>
#include <libiptc/libip6tc.h>
#include <linux/netfilter/xt_set.h>
#include <unistd.h>
#include <signal.h>

#include "vrrp_iptables_calls.h"
#include "logger.h"

/* We sometimes get a resource_busy on iptc_commit. This appears to happen
 * when someone else is also updating it.
 * Tests show that the EAGAIN error is generated if someone else did an
 * update via iptc_commit between us doing iptc_init and iptc_commit, i.e.
 * if there had been an update since out init prior to our commit.
 *
 * Documentation seems to suggest that iptc_init takes a snapshot of the
 * state of iptables. This fits with the tests, but also means that we could
 * be interferred with by anyone else doing an update.
 */

/* Initializes a new iptables instance and returns an iptables resource associated with the new iptables table */
struct iptc_handle* ip4tables_open ( const char* tablename )
{
	struct iptc_handle *h ;

	if ( !( h = iptc_init ( tablename ) ) )
		return NULL ;

	return h ;
}

int ip4tables_close ( struct iptc_handle* handle, int updated )
{
	int res = 1;
	int sav_errno ;

	if (updated) {
		if ( ( res = iptc_commit ( handle ) ) != 1 )
		{
			sav_errno = errno ;
			log_message(LOG_INFO, "iptc_commit returned %d: %s", res, iptc_strerror (sav_errno) );
		}
	}

	iptc_free ( handle ) ;

	if ( res == 1 )
		return 0 ;
	else
		return ( sav_errno ) ;
}

int ip4tables_is_chain(struct iptc_handle* handle, const char* chain_name)
{
	return iptc_is_chain(chain_name, handle);
}

int ip4tables_process_entry( struct iptc_handle* handle, const char* chain_name, int rulenum, const char* target_name, const ip_address_t* src_ip_address, const ip_address_t* dst_ip_address, const char* in_iface, const char* out_iface, uint16_t protocol, uint16_t type, int cmd)
{
	int size;
	struct ipt_entry *fw;
	struct xt_entry_target *target;
	struct xt_entry_match *match ;
	ipt_chainlabel chain;
	int res;
	int sav_errno;

	/* Add an entry */

	memset (chain, 0, sizeof (chain));

	size = XT_ALIGN (sizeof (struct ipt_entry)) +
			XT_ALIGN ( sizeof ( struct xt_entry_match ) ) +
			XT_ALIGN (sizeof (struct xt_entry_target) + 1);

	if ( protocol == IPPROTO_ICMP )
		size += XT_ALIGN ( sizeof(struct xt_entry_match) ) + XT_ALIGN ( sizeof(struct ipt_icmp) ) ;

	fw = (struct ipt_entry*)malloc(size);
	memset (fw, 0, size);

	fw->target_offset = XT_ALIGN ( sizeof ( struct ipt_entry ) ) ;

	if ( src_ip_address && src_ip_address->ifa.ifa_family != AF_UNSPEC )
	{
		memcpy(&fw->ip.src, &src_ip_address->u.sin.sin_addr, sizeof ( src_ip_address->u.sin.sin_addr ) );
		memset ( &fw->ip.smsk, 0xff, sizeof(fw->ip.smsk));
	}

	if ( dst_ip_address && dst_ip_address->ifa.ifa_family != AF_UNSPEC )
	{
		memcpy(&fw->ip.dst, &dst_ip_address->u.sin.sin_addr, sizeof ( dst_ip_address->u.sin.sin_addr ) );
		memset ( &fw->ip.dmsk, 0xff, sizeof(fw->ip.dmsk));
	}

	if ( in_iface )
		strcpy ( fw->ip.iniface, in_iface ) ;
	if ( out_iface )
		strcpy ( fw->ip.outiface, out_iface ) ;

	if ( protocol != IPPROTO_NONE ) {
		fw->ip.proto = protocol ;

//		fw->ip.flags |= IP6T_F_PROTO ;		// IPv6 only

		if ( protocol == IPPROTO_ICMP )
		{
			match = (struct xt_entry_match*)((char*)fw + fw->target_offset);
			match->u.match_size = XT_ALIGN ( sizeof (struct xt_entry_match) ) + XT_ALIGN ( sizeof (struct ipt_icmp) ) ;
			match->u.user.revision = 0;
			fw->target_offset += match->u.match_size ;
			strcpy ( match->u.user.name, "icmpv" ) ;

			struct ipt_icmp *icmpinfo = (struct ipt_icmp *) match->data;
			icmpinfo->type = type ;		// type to match
			icmpinfo->code[0] = 0 ;		// code lower
			icmpinfo->code[1] = 0xff ;	// code upper
			icmpinfo->invflags = 0 ;	// don't invert
		}
	}

// target is XTC_LABEL_DROP/XTC_LABEL_ACCEPT
	fw->next_offset = size;
	target = ipt_get_target ( fw ) ;
	target->u.user.target_size = XT_ALIGN (sizeof (struct xt_entry_target) + 1);
	strcpy (target->u.user.name, target_name );
//	fw->ip.flags |= IPT_F_GOTO;
	strcpy (chain, chain_name);
	// Use iptc_append_entry to add to the chain
	if (cmd == IPADDRESS_DEL) {
		unsigned char matchmask[fw->next_offset];
		memset(matchmask, 0xff, fw->next_offset);
		res = iptc_delete_entry(chain, fw, matchmask, handle);
	}
	else if ( rulenum == -1 )
		res = iptc_append_entry (chain, fw, handle ) ;
	else
		res = iptc_insert_entry (chain, fw, rulenum, handle ) ;

	sav_errno = errno ;

	if (res!= 1)
	{
		log_message(LOG_INFO, "ip4tables_process_entry for chain %s returned %d: %s", chain, res, iptc_strerror (sav_errno) ) ;

		return sav_errno ;
	}

	return 0 ;
}

/* Initializes a new iptables instance and returns an iptables resource associated with the new iptables table */
struct ip6tc_handle* ip6tables_open ( const char* tablename )
{
	struct ip6tc_handle *h ;

	if ( !( h = ip6tc_init ( tablename ) ) )
		return NULL ;

	return h ;
}

int ip6tables_close ( struct ip6tc_handle* handle, int updated )
{
	int res = 1;
	int sav_errno ;

	if (updated) {
		if ( ( res = ip6tc_commit ( handle ) ) != 1 )
		{
			sav_errno = errno ;
			log_message(LOG_INFO, "iptc_commit returned %d: %s", res, ip6tc_strerror (sav_errno) );
		}
	}

	ip6tc_free ( handle ) ;

	if ( res == 1 )
		return 0 ;
	else
		return ( sav_errno ) ;
}

int ip6tables_is_chain(struct ip6tc_handle* handle, const char* chain_name)
{
	return ip6tc_is_chain(chain_name, handle);
}

int ip6tables_process_entry( struct ip6tc_handle* handle, const char* chain_name, int rulenum, const char* target_name, const ip_address_t* src_ip_address, const ip_address_t* dst_ip_address, const char* in_iface, const char* out_iface, uint16_t protocol, uint16_t type, int cmd)
{
	int size;
	struct ip6t_entry *fw;
	struct xt_entry_target *target;
	struct xt_entry_match *match ;
	ip6t_chainlabel chain;
	int res;
	int sav_errno;

	/* Add an entry */

	memset (chain, 0, sizeof (chain));

	size = XT_ALIGN (sizeof (struct ip6t_entry)) +
			XT_ALIGN ( sizeof ( struct xt_entry_match ) ) +
			XT_ALIGN (sizeof (struct xt_entry_target) + 1);

	if ( protocol == IPPROTO_ICMPV6 )
		size += XT_ALIGN ( sizeof(struct xt_entry_match) ) + XT_ALIGN ( sizeof(struct ip6t_icmp) ) ;

	fw = (struct ip6t_entry*)malloc(size);
	memset (fw, 0, size);

	fw->target_offset = XT_ALIGN ( sizeof ( struct ip6t_entry ) ) ;

	if ( src_ip_address && src_ip_address->ifa.ifa_family != AF_UNSPEC ) {
		memcpy(&fw->ipv6.src, &src_ip_address->u.sin6_addr, sizeof ( src_ip_address->u.sin6_addr ) );
		memset ( &fw->ipv6.smsk, 0xff, sizeof(fw->ipv6.smsk));
	}

	if ( dst_ip_address && dst_ip_address->ifa.ifa_family != AF_UNSPEC ) {
		memcpy(&fw->ipv6.dst, &dst_ip_address->u.sin6_addr, sizeof ( dst_ip_address->u.sin6_addr ) );
		memset ( &fw->ipv6.dmsk, 0xff, sizeof(fw->ipv6.smsk));
	}

	if ( in_iface )
		strcpy ( fw->ipv6.iniface, in_iface ) ;
	if ( out_iface )
		strcpy ( fw->ipv6.outiface, out_iface ) ;

	if ( protocol != IPPROTO_NONE ) {
		fw->ipv6.proto = protocol ;

		fw->ipv6.flags |= IP6T_F_PROTO ;		// IPv6 only

		if ( protocol == IPPROTO_ICMPV6 )
		{
			match = (struct xt_entry_match*)((char*)fw + fw->target_offset);
			match->u.match_size = XT_ALIGN ( sizeof (struct xt_entry_match) ) + XT_ALIGN ( sizeof (struct ip6t_icmp) ) ;
			match->u.user.revision = 0;
			fw->target_offset += match->u.match_size ;
			strcpy ( match->u.user.name, "icmp6" ) ;

			struct ip6t_icmp *icmpinfo = (struct ip6t_icmp *) match->data;
			icmpinfo->type = type ;		// type to match
			icmpinfo->code[0] = 0 ;		// code lower
			icmpinfo->code[1] = 0xff ;	// code upper
			icmpinfo->invflags = 0 ;	// don't invert
		}
	}

// target is XTC_LABEL_DROP/XTC_LABEL_ACCEPT
	fw->next_offset = size;
	target = ip6t_get_target ( fw ) ;
	target->u.user.target_size = XT_ALIGN (sizeof (struct xt_entry_target) + 1);
	strcpy (target->u.user.name, target_name );
//	fw->ip.flags |= IPT_F_GOTO;
	strcpy (chain, chain_name);

	// Use iptc_append_entry to add to the chain
	if (cmd == IPADDRESS_DEL) {
		unsigned char matchmask[fw->next_offset];
		memset(matchmask, 0xff, fw->next_offset);
		res = ip6tc_delete_entry ( chain, fw, matchmask, handle);
	}
	else if ( rulenum == -1 )
		res = ip6tc_append_entry (chain, fw, handle ) ;
	else
		res = ip6tc_insert_entry (chain, fw, rulenum, handle ) ;

	sav_errno = errno ;

	if (res != 1)
	{
		log_message(LOG_INFO, "ip6tables_process_entry for chain %s returned %d: %s", chain, res, ip6tc_strerror (sav_errno) ) ;

		return sav_errno ;
	}

	return 0 ;
}

#ifdef _HAVE_LIBIPSET_
int load_mod_xt_set(void)
{
	struct sigaction act, old_act;
	bool res = true;

	/* Enable SIGCHLD since xtables_insmod forks/execs modprobe */
	act.sa_handler = SIG_DFL;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;

	sigaction(SIGCHLD, &act, &old_act);

	if (xtables_insmod("xt_set", NULL, true))
		res = false;

	sigaction(SIGCHLD, &old_act, NULL);
	return res;
}

static int
get_version(unsigned int* version)
{
	int res, sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	struct ip_set_req_version req_version;
	socklen_t size = sizeof(req_version);

	if (sockfd < 0)
		log_message(LOG_INFO, "Can't open socket to ipset.");

	if (fcntl(sockfd, F_SETFD, FD_CLOEXEC) == -1) {
		log_message(LOG_INFO, "Could not set close on exec: %s",
			      strerror(errno));
	}

	req_version.op = IP_SET_OP_VERSION;
	res = getsockopt(sockfd, SOL_IP, SO_IP_SET, &req_version, &size);
	if (res != 0)
		log_message(LOG_INFO, "Kernel module xt_set is not loaded in.");

	*version = req_version.version;

	return sockfd;
}

static void
get_set_byname_only(const char *setname, struct xt_set_info *info,
		    int sockfd, unsigned int version)
{
	struct ip_set_req_get_set req = { .version = version };
	socklen_t size = sizeof(struct ip_set_req_get_set);
	int res;

	req.op = IP_SET_OP_GET_BYNAME;
	strncpy(req.set.name, setname, IPSET_MAXNAMELEN);
	req.set.name[IPSET_MAXNAMELEN - 1] = '\0';
	res = getsockopt(sockfd, SOL_IP, SO_IP_SET, &req, &size);
	close(sockfd);

	if (res != 0)
		log_message(LOG_INFO, "Problem when communicating with ipset, errno=%d.",
			errno);
	if (size != sizeof(struct ip_set_req_get_set))
		log_message(LOG_INFO, "Incorrect return size from kernel during ipset lookup, "
			"(want %zu, got %zu)",
			sizeof(struct ip_set_req_get_set), (size_t)size);
	if (req.set.index == IPSET_INVALID_ID)
		log_message(LOG_INFO, "Set %s doesn't exist.", setname);

	info->index = req.set.index;
}

static void
get_set_byname(const char *setname, struct xt_set_info *info, int family)
{
#if defined IP_SET_OP_GET_FNAME
	struct ip_set_req_get_set_family req;
	socklen_t size = sizeof(struct ip_set_req_get_set_family);
	int res;
#endif
	int sockfd;
	unsigned int version;

	sockfd = get_version(&version);
#if defined IP_SET_OP_GET_FNAME
	req.version = version;
	req.op = IP_SET_OP_GET_FNAME;
	strncpy(req.set.name, setname, IPSET_MAXNAMELEN);
	req.set.name[IPSET_MAXNAMELEN - 1] = '\0';
	res = getsockopt(sockfd, SOL_IP, SO_IP_SET, &req, &size);

	if (res != 0 && errno == EBADMSG)
#endif
		/* Backward compatibility */
		return get_set_byname_only(setname, info, sockfd, version);

#if defined IP_SET_OP_GET_FNAME
	close(sockfd);
	if (res != 0)
		log_message(LOG_INFO, "Problem when communicating with ipset, errno=%d.",
			errno);
	if (size != sizeof(struct ip_set_req_get_set_family))
		log_message(LOG_INFO, "Incorrect return size from kernel during ipset lookup, "
			"(want %zu, got %zu)",
			sizeof(struct ip_set_req_get_set_family),
			(size_t)size);
	if (req.set.index == IPSET_INVALID_ID)
		log_message(LOG_INFO, "Set %s doesn't exist.", setname);
	if (!(req.family == family ||
	      req.family == NFPROTO_UNSPEC))
		log_message(LOG_INFO, "The protocol family of set %s is %s, "
			      "which is not applicable.",
			      setname,
			      req.family == NFPROTO_IPV4 ? "IPv4" : "IPv6");

	info->index = req.set.index;
#endif
}

int ip4tables_add_rules(struct iptc_handle* handle, const char* chain_name, int rulenum, int dim, int src_dst, const char* target_name, const char* set_name, uint16_t protocol, int param, int cmd)
{
	int size;
	struct ipt_entry *fw;
	struct xt_entry_target *target;
	struct xt_entry_match *match;
	struct xt_set_info_match_v1 *setinfo;
	ipt_chainlabel chain;
	int res;
	int sav_errno;

	/* Add an entry */

	memset(chain, 0, sizeof(chain));

	size = XT_ALIGN(sizeof (struct ipt_entry)) +
			XT_ALIGN(sizeof(struct xt_entry_match)) +
			XT_ALIGN(sizeof(struct xt_entry_target) + 1) +
			XT_ALIGN(sizeof( struct xt_entry_match)) +
			XT_ALIGN(sizeof(struct xt_set_info_match_v1));

	if (protocol == IPPROTO_ICMP)
		size += XT_ALIGN(sizeof(struct xt_entry_match)) + XT_ALIGN(sizeof(struct ipt_icmp));

	fw = (struct ipt_entry*)malloc(size);
	memset(fw, 0, size);

	fw->target_offset = XT_ALIGN(sizeof(struct ipt_entry));

	// set
	match = (struct xt_entry_match*)((char*)fw + fw->target_offset);
	match->u.match_size = XT_ALIGN(sizeof(struct xt_entry_match)) + XT_ALIGN(sizeof(struct xt_set_info_match_v1));
	match->u.user.revision = 1;
	fw->target_offset += match->u.match_size;
	strcpy(match->u.user.name, "set");

	setinfo = (struct xt_set_info_match_v1 *)match->data;
	get_set_byname (set_name, &setinfo->match_set, NFPROTO_IPV4);
	setinfo->match_set.dim = dim;
	setinfo->match_set.flags = src_dst;

	if (protocol != IPPROTO_NONE) {
		fw->ip.proto = protocol;

//		fw->ip.flags |= IP6T_F_PROTO ;		// IPv6 only

		if (protocol == IPPROTO_ICMP)
		{
			match = (struct xt_entry_match*)((char*)fw + fw->target_offset);
			match->u.match_size = XT_ALIGN(sizeof(struct xt_entry_match)) + XT_ALIGN(sizeof(struct ipt_icmp));
			match->u.user.revision = 0;
			fw->target_offset += match->u.match_size;
			strcpy(match->u.user.name, "icmp");

			struct ipt_icmp *icmpinfo = (struct ipt_icmp *)match->data;
			icmpinfo->type = param;		// type to match
			icmpinfo->code[0] = 0;		// code lower
			icmpinfo->code[1] = 0xff;	// code upper
			icmpinfo->invflags = 0;		// don't invert
		}
	}

// target is XTC_LABEL_DROP/XTC_LABEL_ACCEPT
	fw->next_offset = size;
	target = ipt_get_target(fw);
	target->u.user.target_size = XT_ALIGN(sizeof(struct xt_entry_target) + 1);
	strcpy(target->u.user.name, target_name);
//	fw->ip.flags |= IPT_F_GOTO;
	strcpy(chain, chain_name);

	// Use iptc_append_entry to add to the chain
	if (cmd == IPADDRESS_DEL) {
		unsigned char matchmask[fw->next_offset];
		memset(matchmask, 0xff, fw->next_offset);
		res = iptc_delete_entry(chain, fw, matchmask, handle);
	}
	else if (rulenum == -1)
		res = iptc_append_entry(chain, fw, handle) ;
	else
		res = iptc_insert_entry(chain, fw, rulenum, handle) ;

	sav_errno = errno;

	if (res!= 1)
	{
		log_message(LOG_INFO, "iptc_insert_entry for chain %s returned %d: %s", chain, res, iptc_strerror(sav_errno)) ;

		return sav_errno;
	}

	return 0;
}

int ip6tables_add_rules(struct ip6tc_handle* handle, const char* chain_name, int rulenum, int dim, int src_dst, const char* target_name, const char* set_name, uint16_t protocol, int param, int cmd)
{
	int size;
	struct ip6t_entry *fw;
	struct xt_entry_target *target;
	struct xt_entry_match *match;
	struct xt_set_info_match_v1 *setinfo;
	ip6t_chainlabel chain;
	int res;
	int sav_errno;

	/* Add an entry */

	memset(chain, 0, sizeof(chain));

	size = XT_ALIGN(sizeof (struct ip6t_entry)) +
			XT_ALIGN(sizeof(struct xt_entry_match)) +
			XT_ALIGN(sizeof(struct xt_entry_target) + 1) +
			XT_ALIGN(sizeof( struct xt_entry_match)) +
			XT_ALIGN(sizeof(struct xt_set_info_match_v1));

	if (protocol == IPPROTO_ICMPV6)
		size += XT_ALIGN(sizeof(struct xt_entry_match)) + XT_ALIGN(sizeof(struct ip6t_icmp));

	fw = (struct ip6t_entry*)malloc(size);
	memset(fw, 0, size);

	fw->target_offset = XT_ALIGN(sizeof(struct ip6t_entry));

	// set
	match = (struct xt_entry_match*)((char*)fw + fw->target_offset);
	match->u.match_size = XT_ALIGN(sizeof(struct xt_entry_match)) + XT_ALIGN(sizeof(struct xt_set_info_match_v1));
	match->u.user.revision = 1;
	fw->target_offset += match->u.match_size;
	strcpy(match->u.user.name, "set");

	setinfo = (struct xt_set_info_match_v1 *)match->data;
	get_set_byname (set_name, &setinfo->match_set, NFPROTO_IPV6);
	setinfo->match_set.dim = dim;
	setinfo->match_set.flags = src_dst;

	if (protocol != IPPROTO_NONE) {
		fw->ipv6.proto = protocol;

		fw->ipv6.flags |= IP6T_F_PROTO ;		// IPv6 only

		if (protocol == IPPROTO_ICMPV6)
		{
			match = (struct xt_entry_match*)((char*)fw + fw->target_offset);
			match->u.match_size = XT_ALIGN(sizeof(struct xt_entry_match)) + XT_ALIGN(sizeof(struct ip6t_icmp));
			match->u.user.revision = 0;
			fw->target_offset += match->u.match_size;
			strcpy(match->u.user.name, "icmp6");

			struct ip6t_icmp *icmpinfo = (struct ip6t_icmp *)match->data;
			icmpinfo->type = param;		// type to match
			icmpinfo->code[0] = 0;		// code lower
			icmpinfo->code[1] = 0xff;	// code upper
			icmpinfo->invflags = 0;		// don't invert
		}
	}

// target is XTC_LABEL_DROP/XTC_LABEL_ACCEPT
	fw->next_offset = size;
	target = ip6t_get_target(fw);
	target->u.user.target_size = XT_ALIGN(sizeof(struct xt_entry_target) + 1);
	strcpy(target->u.user.name, target_name);
//	fw->ip.flags |= IP6T_F_GOTO;
	strcpy(chain, chain_name);

	// Use iptc_append_entry to add to the chain
	if (cmd == IPADDRESS_DEL) {
		unsigned char matchmask[fw->next_offset];
		memset(matchmask, 0xff, fw->next_offset);
		res = ip6tc_delete_entry(chain, fw, matchmask, handle);
	}
	else if (rulenum == -1)
		res = ip6tc_append_entry(chain, fw, handle) ;
	else
		res = ip6tc_insert_entry(chain, fw, rulenum, handle) ;

	sav_errno = errno;

	if (res!= 1)
	{
		log_message(LOG_INFO, "iptc_insert_entry for chain %s returned %d: %s", chain, res, ip6tc_strerror(sav_errno)) ;

		return sav_errno;
	}

	return 0;
}
#endif
