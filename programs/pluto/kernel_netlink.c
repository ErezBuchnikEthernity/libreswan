/* netlink interface to the kernel's IPsec mechanism
 * Copyright (C) 2003 Herbert Xu.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * RCSID $Id: kernel_netlink.c,v 1.11.2.4 2004/06/01 14:42:36 ken Exp $
 */

#if defined(linux) && defined(KERNEL26_SUPPORT)

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <unistd.h>

#include "kameipsec.h"
#include <rtnetlink.h>
#include <xfrm.h>

#include <openswan.h>
#include <pfkeyv2.h>
#include <pfkey.h>

#include "constants.h"
#include "defs.h"
#include "id.h"
#include "connections.h"
#include "kernel.h"
#include "kernel_netlink.h"
#include "kernel_pfkey.h"
#include "log.h"
#include "whack.h"	/* for RC_LOG_SERIOUS */

#ifdef XAUTH_USEPAM
#include <security/pam_appl.h>
#endif

/* Minimum priority number in SPD used by pluto. */
#define MIN_SPD_PRIORITY 1024

static int netlinkfd = NULL_FD;
static int netlink_bcast_fd = NULL_FD;

#define NE(x) { x, #x }	/* Name Entry -- shorthand for sparse_names */

static sparse_names xfrm_type_names = {
	NE(NLMSG_NOOP),
	NE(NLMSG_ERROR),
	NE(NLMSG_DONE),
	NE(NLMSG_OVERRUN),

	NE(XFRM_MSG_NEWSA),
	NE(XFRM_MSG_DELSA),
	NE(XFRM_MSG_GETSA),

	NE(XFRM_MSG_NEWPOLICY),
	NE(XFRM_MSG_DELPOLICY),
	NE(XFRM_MSG_GETPOLICY),

	NE(XFRM_MSG_ALLOCSPI),
	NE(XFRM_MSG_ACQUIRE),
	NE(XFRM_MSG_EXPIRE),

	NE(XFRM_MSG_UPDPOLICY),
	NE(XFRM_MSG_UPDSA),

	NE(XFRM_MSG_POLEXPIRE),

	NE(XFRM_MSG_MAX),

	{ 0, sparse_end }
};

#undef NE

static sparse_names aalg_list = {
	{ SADB_X_AALG_NULL, "digest_null" },
	{ SADB_AALG_MD5HMAC, "md5" },
	{ SADB_AALG_SHA1HMAC, "sha1" },
	{ SADB_X_AALG_SHA2_256HMAC, "sha256" },
	{ SADB_X_AALG_RIPEMD160HMAC, "ripemd160" },
	{ 0, sparse_end }
};

static sparse_names ealg_list = {
	{ SADB_EALG_NULL, "cipher_null" },
	{ SADB_EALG_DESCBC, "des" },
	{ SADB_EALG_3DESCBC, "des3_ede" },
	{ SADB_X_EALG_CASTCBC, "cast128" },
	{ SADB_X_EALG_BLOWFISHCBC, "blowfish" },
	{ SADB_X_EALG_AESCBC, "aes" },
	{ 0, sparse_end }
};

static sparse_names calg_list = {
	{ SADB_X_CALG_DEFLATE, "deflate" },
	{ SADB_X_CALG_LZS, "lzs" },
	{ SADB_X_CALG_LZJH, "lzjh" },
	{ 0, sparse_end }
};

static void ip2xfrm(const ip_address *addr, xfrm_address_t *xaddr)
{
    if (addr->u.v4.sin_family == AF_INET)
    {
	xaddr->a4 = addr->u.v4.sin_addr.s_addr;
    }
    else
    {
	memcpy(xaddr->a6, &addr->u.v6.sin6_addr, sizeof(xaddr->a6));
    }
}

static void init_netlink(void)
{
    struct sockaddr_nl addr;

    netlinkfd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_XFRM);

    if (netlinkfd < 0)
	exit_log_errno((e, "socket() in init_netlink()"));

    if (fcntl(netlinkfd, F_SETFD, FD_CLOEXEC) != 0)
	exit_log_errno((e, "fcntl(FD_CLOEXEC) in init_netlink()"));

    netlink_bcast_fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_XFRM);

    if (netlink_bcast_fd < 0)
	exit_log_errno((e, "socket() for bcast in init_netlink()"));

    if (fcntl(netlink_bcast_fd, F_SETFD, FD_CLOEXEC) != 0)
	exit_log_errno((e, "fcntl(FD_CLOEXEC) for bcast in init_netlink()"));

    if (fcntl(netlink_bcast_fd, F_SETFL, O_NONBLOCK) != 0)
	exit_log_errno((e, "fcntl(O_NONBLOCK) for bcast in init_netlink()"));

    addr.nl_family = AF_NETLINK;
    addr.nl_pid = getpid();
    addr.nl_groups = XFRMGRP_ACQUIRE | XFRMGRP_EXPIRE;
    if (bind(netlink_bcast_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
	exit_log_errno((e, "Failed to bind bcast socket in init_netlink()"));
}

static bool
send_netlink_msg(struct nlmsghdr *hdr, struct nlmsghdr *rbuf, size_t rbuf_len
, const char *description, const char *text_said)
{
    struct {
	struct nlmsghdr n;
	struct nlmsgerr e;
	char data[1024];
    } rsp;
    size_t len;
    ssize_t r;
    struct sockaddr_nl addr;
    static uint32_t seq;

    if (no_klips)
    {
	return TRUE;
    }

    hdr->nlmsg_seq = ++seq;
    len = hdr->nlmsg_len;
    do {
	r = write(netlinkfd, hdr, len);
    } while (r < 0 && errno == EINTR);
    if (r < 0)
    {
	log_errno((e
	    , "netlink write() of %s message"
	      " for %s %s failed"
	    , sparse_val_show(xfrm_type_names, hdr->nlmsg_type)
	    , description, text_said));
	return FALSE;
    }
    else if ((size_t)r != len)
    {
	loglog(RC_LOG_SERIOUS
	    , "ERROR: netlink write() of %s message"
	      " for %s %s truncated: %ld instead of %lu"
	    , sparse_val_show(xfrm_type_names, hdr->nlmsg_type)
	    , description, text_said
	    , (long)r, (unsigned long)len);
	return FALSE;
    }

    for (;;) {
	socklen_t alen;

	alen = sizeof(addr);
	r = recvfrom(netlinkfd, &rsp, sizeof(rsp), 0
	    , (struct sockaddr *)&addr, &alen);
	if (r < 0)
	{
	    if (errno == EINTR)
	    {
		continue;
	    }
	    log_errno((e
		, "netlink recvfrom() of response to our %s message"
		  " for %s %s failed"
		, sparse_val_show(xfrm_type_names, hdr->nlmsg_type)
		, description, text_said));
	    return FALSE;
	}
	else if ((size_t) r < sizeof(rsp.n))
	{
	    plog("netlink read truncated message: %ld bytes; ignore message"
		, (long) r);
	    continue;
	}
	else if (addr.nl_pid != 0)
	{
	    /* not for us: ignore */
	    DBG(DBG_KLIPS,
		DBG_log("netlink: ignoring %s message from process %u"
		    , sparse_val_show(xfrm_type_names, rsp.n.nlmsg_type)
		    , addr.nl_pid));
	    continue;
	}
	else if (rsp.n.nlmsg_seq != seq)
	{
	    DBG(DBG_KLIPS,
		DBG_log("netlink: ignoring out of sequence (%u/%u) message %s"
		    , rsp.n.nlmsg_seq, seq
		    , sparse_val_show(xfrm_type_names, rsp.n.nlmsg_type)));
	    continue;
	}
	break;
    }

    if (rsp.n.nlmsg_len > (size_t) r)
    {
	loglog(RC_LOG_SERIOUS
	    , "netlink recvfrom() of response to our %s message"
	      " for %s %s was truncated: %ld instead of %lu"
	    , sparse_val_show(xfrm_type_names, hdr->nlmsg_type)
	    , description, text_said
	    , (long) len, (unsigned long) rsp.n.nlmsg_len);
	return FALSE;
    }
    else if (rsp.n.nlmsg_type != NLMSG_ERROR
    && (rbuf && rsp.n.nlmsg_type != rbuf->nlmsg_type))
    {
	loglog(RC_LOG_SERIOUS
	    , "netlink recvfrom() of response to our %s message"
	      " for %s %s was of wrong type (%s)"
	    , sparse_val_show(xfrm_type_names, hdr->nlmsg_type)
	    , description, text_said
	    , sparse_val_show(xfrm_type_names, rsp.n.nlmsg_type));
	return FALSE;
    }
    else if (rbuf)
    {
	if ((size_t) r > rbuf_len)
	{
	    loglog(RC_LOG_SERIOUS
		, "netlink recvfrom() of response to our %s message"
		  " for %s %s was too long: %ld > %lu"
		, sparse_val_show(xfrm_type_names, hdr->nlmsg_type)
		, description, text_said
		, (long)r, (unsigned long)rbuf_len);
	    return FALSE;
	}
	memcpy(rbuf, &rsp, r);
	return TRUE;
    }
    else if (rsp.n.nlmsg_type == NLMSG_ERROR && rsp.e.error)
    {
	loglog(RC_LOG_SERIOUS
	    , "ERROR: netlink response for %s %s included errno %d: %s"
	    , description, text_said
	    , -rsp.e.error
	    , strerror(-rsp.e.error));
	return FALSE;
    }

    return TRUE;
}

static bool
netlink_policy(struct nlmsghdr *hdr, bool enoent_ok, const char *text_said)
{
    struct {
	struct nlmsghdr n;
	struct nlmsgerr e;
    } rsp;
    int error;

    rsp.n.nlmsg_type = NLMSG_ERROR;
    if (!send_netlink_msg(hdr, &rsp.n, sizeof(rsp), "policy", text_said))
    {
	return FALSE;
    }

    error = -rsp.e.error;
    if (!error)
    {
	return TRUE;
    }

    if (error == ENOENT && enoent_ok)
    {
	return TRUE;
    }

    loglog(RC_LOG_SERIOUS
	, "ERROR: netlink %s response for flow %s included errno %d: %s"
	, sparse_val_show(xfrm_type_names, hdr->nlmsg_type)
	, text_said
	, error
	, strerror(error));
    return FALSE;
}

static bool
netlink_raw_eroute(const ip_address *this_host
		   , const ip_subnet *this_client
		   , const ip_address *that_host
		   , const ip_subnet *that_client
		   , ipsec_spi_t spi
		   , unsigned int proto UNUSED
		   , unsigned int transport_proto UNUSED
		   , unsigned int satype
		   , const struct pfkey_proto_info *proto_info
		   , time_t use_lifetime UNUSED
		   , unsigned int op
		   , const char *text_said)
{
    struct {
	struct nlmsghdr n;
	union {
	    struct xfrm_userpolicy_info p;
	    struct xfrm_userpolicy_id id;
	} u;
	char data[1024];
    } req;
    int shift;
    int dir;
    int family;
    int policy;
    bool ok;
    bool enoent_ok;

    policy = IPSEC_POLICY_IPSEC;

    if (satype == SADB_X_SATYPE_INT)
    {
	/* shunt route */
	switch (ntohl(spi))
	{
	case SPI_PASS:
	    policy = IPSEC_POLICY_NONE;
	    break;
	case SPI_DROP:
	case SPI_REJECT:
	default:
	    policy = IPSEC_POLICY_DISCARD;
	    break;
	case SPI_TRAP:
	case SPI_TRAPSUBNET:
	case SPI_HOLD:
	    if (op & (SADB_X_SAFLAGS_INFLOW << ERO_FLAG_SHIFT))
	    {
		return TRUE;
	    }
	    break;
	}
    }

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    family = that_client->addr.u.v4.sin_family;
    shift = (family == AF_INET) ? 5 : 7;

    req.u.p.sel.sport = portof(&this_client->addr);
    req.u.p.sel.dport = portof(&that_client->addr);
    req.u.p.sel.sport_mask = (req.u.p.sel.sport) ? ~0:0;
    req.u.p.sel.dport_mask = (req.u.p.sel.dport) ? ~0:0;
    ip2xfrm(&this_client->addr, &req.u.p.sel.saddr);
    ip2xfrm(&that_client->addr, &req.u.p.sel.daddr);
    req.u.p.sel.prefixlen_s = this_client->maskbits;
    req.u.p.sel.prefixlen_d = that_client->maskbits;
    req.u.p.sel.proto = transport_proto;
    req.u.p.sel.family = family;

    dir = XFRM_POLICY_OUT;
    if (op & (SADB_X_SAFLAGS_INFLOW << ERO_FLAG_SHIFT))
    {
	dir = XFRM_POLICY_IN;
    }

    if ((op & ERO_MASK) == ERO_DELETE)
    {
	req.u.id.dir = dir;
	req.n.nlmsg_type = XFRM_MSG_DELPOLICY;
	req.n.nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(req.u.id)));
    }
    else
    {
    	int src, dst;

	req.u.p.dir = dir;

    	src = req.u.p.sel.prefixlen_s;
    	dst = req.u.p.sel.prefixlen_d;
	if (dir != XFRM_POLICY_OUT) {
	    src = req.u.p.sel.prefixlen_d;
	    dst = req.u.p.sel.prefixlen_s;
	}
	req.u.p.priority = MIN_SPD_PRIORITY
	    + (((2 << shift) - src) << shift)
	    + (2 << shift) - dst;

	req.u.p.action = XFRM_POLICY_ALLOW;
	if (policy == IPSEC_POLICY_DISCARD)
	{
	    req.u.p.action = XFRM_POLICY_BLOCK;
	}
	req.u.p.lft.soft_use_expires_seconds = use_lifetime;
	req.u.p.lft.soft_byte_limit = XFRM_INF;
	req.u.p.lft.soft_packet_limit = XFRM_INF;
	req.u.p.lft.hard_byte_limit = XFRM_INF;
	req.u.p.lft.hard_packet_limit = XFRM_INF;

	req.n.nlmsg_type = XFRM_MSG_NEWPOLICY;
	if (op & (SADB_X_SAFLAGS_REPLACEFLOW << ERO_FLAG_SHIFT))
	{
	    req.n.nlmsg_type = XFRM_MSG_UPDPOLICY;
	}
	req.n.nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(req.u.p)));
    }

    if (policy == IPSEC_POLICY_IPSEC && (op & ERO_MASK) != ERO_DELETE)
    {
	struct rtattr *attr;
	struct xfrm_user_tmpl tmpl[4];
	int i;

	memset(tmpl, 0, sizeof(tmpl));
	for (i = 0; proto_info[i].proto; i++)
	{
	    tmpl[i].reqid = proto_info[i].reqid;
	    tmpl[i].id.proto = proto_info[i].proto;
	    tmpl[i].optional = proto_info[i].proto == IPPROTO_COMP;
	    tmpl[i].aalgos = tmpl[i].ealgos = tmpl[i].calgos = ~0;
	    tmpl[i].mode =
		proto_info[i].encapsulation == ENCAPSULATION_MODE_TUNNEL;

	    if (!tmpl[i].mode)
	    {
		continue;
	    }

	    ip2xfrm(this_host, &tmpl[i].saddr);
	    ip2xfrm(that_host, &tmpl[i].id.daddr);
	}

	attr = (struct rtattr *)((char *)&req + req.n.nlmsg_len);
	attr->rta_type = XFRMA_TMPL;
	attr->rta_len = i * sizeof(tmpl[0]);
	memcpy(RTA_DATA(attr), tmpl, attr->rta_len);
	attr->rta_len = RTA_LENGTH(attr->rta_len);
	req.n.nlmsg_len += attr->rta_len;
    }

    enoent_ok = FALSE;
    if (op == ERO_DEL_INBOUND)
    {
	enoent_ok = TRUE;
    }
    else if (op == ERO_DELETE && ntohl(spi) == SPI_HOLD)
    {
	enoent_ok = TRUE;
    }

    ok = netlink_policy(&req.n, enoent_ok, text_said);
    switch (dir)
    {
    case XFRM_POLICY_IN:
	if (req.n.nlmsg_type == XFRM_MSG_DELPOLICY)
	{
	    req.u.id.dir = XFRM_POLICY_FWD;
	}
	else if (!ok)
	{
	    break;
	}
	else if (proto_info[0].encapsulation != ENCAPSULATION_MODE_TUNNEL
	&& satype != SADB_X_SATYPE_INT)
	{
	    break;
	}
	else
	{
	    req.u.p.dir = XFRM_POLICY_FWD;
	}
	ok &= netlink_policy(&req.n, enoent_ok, text_said);
	break;
    }

    return ok;
}

static bool
netlink_add_sa(const struct kernel_sa *sa, bool replace)
{
    struct {
	struct nlmsghdr n;
	struct xfrm_usersa_info p;
	char data[1024];
    } req;
    struct rtattr *attr;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.n.nlmsg_type = replace ? XFRM_MSG_UPDSA : XFRM_MSG_NEWSA;

    ip2xfrm(sa->src, &req.p.saddr);
    ip2xfrm(sa->dst, &req.p.id.daddr);

    if (sa->src_client)
    {
	ip2xfrm(&sa->src_client->addr, &req.p.sel.saddr);
	ip2xfrm(&sa->dst_client->addr, &req.p.sel.daddr);
	req.p.sel.prefixlen_s = sa->src_client->maskbits;
	req.p.sel.prefixlen_d = sa->dst_client->maskbits;
        req.p.sel.family = sa->src_client->addr.u.v4.sin_family;
    }

    req.p.id.spi = sa->spi;
    req.p.id.proto = satype2proto(sa->satype);
    req.p.family = sa->src->u.v4.sin_family;
    req.p.mode = (sa->encapsulation == ENCAPSULATION_MODE_TUNNEL);
    req.p.replay_window = sa->replay_window;
    req.p.reqid = sa->reqid;
    req.p.lft.soft_byte_limit = XFRM_INF;
    req.p.lft.soft_packet_limit = XFRM_INF;
    req.p.lft.hard_byte_limit = XFRM_INF;
    req.p.lft.hard_packet_limit = XFRM_INF;

    req.n.nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(req.p)));

    attr = (struct rtattr *)((char *)&req + req.n.nlmsg_len);

    if (sa->authkeylen)
    {
	struct xfrm_algo algo;
	const char *name;

	name = sparse_name(aalg_list, sa->authalg);
	if (!name) {
	    loglog(RC_LOG_SERIOUS, "unknown authentication algorithm: %u"
		, sa->authalg);
	    return FALSE;
	}

	strcpy(algo.alg_name, name);
	algo.alg_key_len = sa->authkeylen * BITS_PER_BYTE;

	attr->rta_type = XFRMA_ALG_AUTH;
	attr->rta_len = RTA_LENGTH(sizeof(algo) + sa->authkeylen);

	memcpy(RTA_DATA(attr), &algo, sizeof(algo));
	memcpy((char *)RTA_DATA(attr) + sizeof(algo), sa->authkey
	    , sa->authkeylen);

	req.n.nlmsg_len += attr->rta_len;
	attr = (struct rtattr *)((char *)attr + attr->rta_len);
    }

    if (sa->enckeylen)
    {
	struct xfrm_algo algo;
	const char *name;

	name = sparse_name(ealg_list, sa->encalg);
	if (!name) {
	    loglog(RC_LOG_SERIOUS, "unknown encryption algorithm: %u"
		, sa->encalg);
	    return FALSE;
	}

	strcpy(algo.alg_name, name);
	algo.alg_key_len = sa->enckeylen * BITS_PER_BYTE;

	attr->rta_type = XFRMA_ALG_CRYPT;
	attr->rta_len = RTA_LENGTH(sizeof(algo) + sa->enckeylen);

	memcpy(RTA_DATA(attr), &algo, sizeof(algo));
	memcpy((char *)RTA_DATA(attr) + sizeof(algo), sa->enckey
	    , sa->enckeylen);

	req.n.nlmsg_len += attr->rta_len;
	attr = (struct rtattr *)((char *)attr + attr->rta_len);
    }

    if (sa->satype == SADB_X_SATYPE_COMP)
    {
	struct xfrm_algo algo;
	const char *name;

	name = sparse_name(calg_list, sa->encalg);
	if (!name) {
	    loglog(RC_LOG_SERIOUS, "unknown compression algorithm: %u"
		, sa->encalg);
	    return FALSE;
	}

	strcpy(algo.alg_name, name);
	algo.alg_key_len = 0;

	attr->rta_type = XFRMA_ALG_COMP;
	attr->rta_len = RTA_LENGTH(sizeof(algo));

	memcpy(RTA_DATA(attr), &algo, sizeof(algo));

	req.n.nlmsg_len += attr->rta_len;
	attr = (struct rtattr *)((char *)attr + attr->rta_len);
    }

#ifdef NAT_TRAVERSAL
    if (sa->natt_type)
    {
	struct xfrm_encap_tmpl natt;

	natt.encap_type = sa->natt_type;
	natt.encap_sport = ntohs(sa->natt_sport);
	natt.encap_dport = ntohs(sa->natt_dport);
	memset (&natt.encap_oa, 0, sizeof (natt.encap_oa));

	attr->rta_type = XFRMA_ENCAP;
	attr->rta_len = RTA_LENGTH(sizeof(natt));

	memcpy(RTA_DATA(attr), &natt, sizeof(natt));

	req.n.nlmsg_len += attr->rta_len;
	attr = (struct rtattr *)((char *)attr + attr->rta_len);
    }
#endif

    return send_netlink_msg(&req.n, NULL, 0, "Add SA", sa->text_said);
}

static bool
netlink_del_sa(const struct kernel_sa *sa)
{
    struct {
	struct nlmsghdr n;
	struct xfrm_usersa_id id;
	char data[1024];
    } req;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    req.n.nlmsg_type = XFRM_MSG_DELSA;

    ip2xfrm(sa->dst, &req.id.daddr);

    req.id.spi = sa->spi;
    req.id.family = sa->src->u.v4.sin_family;
    req.id.proto = sa->proto;

    req.n.nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(req.id)));

    return send_netlink_msg(&req.n, NULL, 0, "Del SA", sa->text_said);
}

static void
linux_pfkey_register_response(const struct sadb_msg *msg)
{
    switch (msg->sadb_msg_satype)
    {
    case SADB_X_SATYPE_IPCOMP:
	can_do_IPcomp = TRUE;
	break;
    default:
	break;
    }
}

static void
linux_pfkey_register(void)
{
    pfkey_register_proto(SADB_SATYPE_AH, "AH");
    pfkey_register_proto(SADB_SATYPE_ESP, "ESP");
    pfkey_register_proto(SADB_X_SATYPE_IPCOMP, "IPCOMP");
    pfkey_close();
}

/* Create ip_address out of xfrm_address_t. */
static err_t
xfrm_to_ip_address(unsigned family
, const xfrm_address_t *src
, ip_address *dst)
{
    switch (family)
    {
    case AF_INET:
	initaddr((const void *) &src->a4, sizeof(src->a4), family, dst);
	return NULL;
    case AF_INET6:
	initaddr((const void *) &src->a6, sizeof(src->a6), family, dst);
	return NULL;
    default:
	return "unknown address family";
    }
}

static void
netlink_acquire(struct nlmsghdr *n)
{
    struct xfrm_user_acquire *acquire;
    const xfrm_address_t *srcx, *dstx;
    int src_proto, dst_proto;
    ip_address src, dst;
    ip_subnet ours, his;
    unsigned family;
    unsigned transport_proto;
    err_t ugh = NULL;

    if (n->nlmsg_len < NLMSG_LENGTH(sizeof(*acquire)))
    {
	plog("netlink_acquire got message with length %lu < %lu bytes; ignore message"
	    , (unsigned long) n->nlmsg_len
	    , (unsigned long) sizeof(*acquire));
	return;
    }

    acquire = NLMSG_DATA(n);
    srcx = &acquire->sel.saddr;
    dstx = &acquire->sel.daddr;
    family = acquire->policy.sel.family;
    transport_proto = acquire->sel.proto;

    src_proto = 0;   /* XXX-MCR where to get protocol from? */
    dst_proto = 0;   /* ditto */

    /* XXX also the type of src/dst should be checked to make sure
     *     that they aren't v4 to v6 or something goofy
     */

    if (!(ugh = xfrm_to_ip_address(family, srcx, &src))
	&& !(ugh = xfrm_to_ip_address(family, dstx, &dst))
	&& !(ugh = src_proto == dst_proto? NULL : "src and dst protocols differ")
	&& !(ugh = addrtosubnet(&src, &ours))
	&& !(ugh = addrtosubnet(&dst, &his)))
      record_and_initiate_opportunistic(&ours, &his, transport_proto
					  , "%acquire-netlink");


    if (ugh != NULL)
	plog("XFRM_MSG_ACQUIRE message from kernel malformed: %s", ugh);
}

static void
netlink_shunt_expire(struct xfrm_userpolicy_info *pol)
{
    const xfrm_address_t *srcx, *dstx;
    ip_address src, dst;
    unsigned family;
    unsigned transport_proto;
    err_t ugh = NULL;
  
    srcx = &pol->sel.saddr;
    dstx = &pol->sel.daddr;
    family = pol->sel.family;
    transport_proto = pol->sel.proto;

    if ((ugh = xfrm_to_ip_address(family, srcx, &src))
    || (ugh = xfrm_to_ip_address(family, dstx, &dst)))
    {
	plog("XFRM_MSG_POLEXPIRE message from kernel malformed: %s", ugh);
	return;
    }

    replace_bare_shunt(&src, &dst
		       , BOTTOM_PRIO
		       , SPI_PASS
		       , FALSE
		       , transport_proto
		       , "delete expired bare shunt");
}

static void
netlink_policy_expire(struct nlmsghdr *n)
{
    struct xfrm_user_polexpire *upe;
    struct {
	struct nlmsghdr n;
	struct xfrm_userpolicy_id id;
    } req;
    struct {
	struct nlmsghdr n;
	struct xfrm_userpolicy_info pol;
	char data[1024];
    } rsp;

    if (n->nlmsg_len < NLMSG_LENGTH(sizeof(*upe)))
    {
	plog("netlink_policy_expire got message with length %lu < %lu bytes; ignore message"
	    , (unsigned long) n->nlmsg_len
	    , (unsigned long) sizeof(*upe));
	return;
    }

    upe = NLMSG_DATA(n);
    req.id.dir = upe->pol.dir;
    req.id.index = upe->pol.index;
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.n.nlmsg_type = XFRM_MSG_GETPOLICY;
    req.n.nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(req.id)));

    rsp.n.nlmsg_type = XFRM_MSG_NEWPOLICY;
    if (!send_netlink_msg(&req.n, &rsp.n, sizeof(rsp), "Get policy", "?"))
    {
	return;
    }
    else if (rsp.n.nlmsg_type == NLMSG_ERROR)
    {
	DBG(DBG_KLIPS,
	    DBG_log("netlink_policy_expire: policy died on us: "
		    "dir=%d, index=%d"
		, req.id.dir, req.id.index));
	return;
    }
    else if (rsp.n.nlmsg_len < NLMSG_LENGTH(sizeof(rsp.pol)))
    {
	plog("netlink_policy_expire: XFRM_MSG_GETPOLICY returned message with length %lu < %lu bytes; ignore message"
	    , (unsigned long) rsp.n.nlmsg_len
	    , (unsigned long) sizeof(rsp.pol));
	return;
    }
    else if (req.id.index != rsp.pol.index)
    {
	DBG(DBG_KLIPS,
	    DBG_log("netlink_policy_expire: policy was replaced: "
		    "dir=%d, oldindex=%d, newindex=%d"
		, req.id.dir, req.id.index, rsp.pol.index));
	return;
    }
    else if (upe->pol.curlft.add_time != rsp.pol.curlft.add_time)
    {
	DBG(DBG_KLIPS,
	    DBG_log("netlink_policy_expire: policy was replaced "
		    " and you have won the lottery: "
		    "dir=%d, index=%d"
		, req.id.dir, req.id.index));
	return;
    }

    switch (upe->pol.dir)
    {
    case XFRM_POLICY_OUT:
	netlink_shunt_expire(&rsp.pol);
	break;
    }
}

static bool
netlink_get(void)
{
    struct {
	struct nlmsghdr n;
	char data[1024];
    } rsp;
    ssize_t r;
    struct sockaddr_nl addr;
    socklen_t alen;

    alen = sizeof(addr);
    r = recvfrom(netlink_bcast_fd, &rsp, sizeof(rsp), 0
	, (struct sockaddr *)&addr, &alen);
    if (r < 0)
    {
	if (errno == EAGAIN)
	    return FALSE;
	if (errno != EINTR)
	    log_errno((e, "recvfrom() failed in netlink_get"));
	return TRUE;
    }
    else if ((size_t) r < sizeof(rsp.n))
    {
	plog("netlink_get read truncated message: %ld bytes; ignore message"
	    , (long) r);
	return TRUE;
    }
    else if (addr.nl_pid != 0)
    {
	/* not for us: ignore */
	DBG(DBG_KLIPS,
	    DBG_log("netlink_get: ignoring %s message from process %u"
		, sparse_val_show(xfrm_type_names, rsp.n.nlmsg_type)
		, addr.nl_pid));
	return TRUE;
    }
    else if ((size_t) r != rsp.n.nlmsg_len)
    {
	plog("netlink_get read message with length %ld that doesn't equal nlmsg_len %lu bytes; ignore message"
	    , (long) r
	    , (unsigned long) rsp.n.nlmsg_len);
	return TRUE;
    }

    DBG(DBG_KLIPS,
	DBG_log("netlink_get: %s message"
		, sparse_val_show(xfrm_type_names, rsp.n.nlmsg_type)));

    switch (rsp.n.nlmsg_type)
    {
    case XFRM_MSG_ACQUIRE:
	netlink_acquire(&rsp.n);
	break;
    case XFRM_MSG_POLEXPIRE:
	netlink_policy_expire(&rsp.n);
	break;
    default:
	/* ignored */
	break;
    }

    return TRUE;
}

static void
netlink_process_msg(void)
{
    while (netlink_get())
	;
}

static ipsec_spi_t
netlink_get_spi(const ip_address *src
, const ip_address *dst
, int proto
, bool tunnel_mode
, unsigned reqid
, ipsec_spi_t min
, ipsec_spi_t max
, const char *text_said)
{
    struct {
	struct nlmsghdr n;
	struct xfrm_userspi_info spi;
    } req;
    struct {
	struct nlmsghdr n;
	union {
	    struct nlmsgerr e;
	    struct xfrm_usersa_info sa;
	} u;
	char data[1024];
    } rsp;

    memset(&req, 0, sizeof(req));
    req.n.nlmsg_flags = NLM_F_REQUEST;
    req.n.nlmsg_type = XFRM_MSG_ALLOCSPI;

    ip2xfrm(src, &req.spi.info.saddr);
    ip2xfrm(dst, &req.spi.info.id.daddr);
    req.spi.info.mode = tunnel_mode;
    req.spi.info.reqid = reqid;
    req.spi.info.id.proto = proto;
    req.spi.info.family = src->u.v4.sin_family;
    req.spi.min = min;
    req.spi.max = max;

    req.n.nlmsg_len = NLMSG_ALIGN(NLMSG_LENGTH(sizeof(req.spi)));

    rsp.n.nlmsg_type = XFRM_MSG_NEWSA;
    if (!send_netlink_msg(&req.n, &rsp.n, sizeof(rsp), "Get SPI", text_said))
	return 0;
    else if (rsp.n.nlmsg_type == NLMSG_ERROR)
    {
	loglog(RC_LOG_SERIOUS
	    , "ERROR: netlink_get_spi for %s failed with errno %d: %s"
	    , text_said
	    , -rsp.u.e.error
	    , strerror(-rsp.u.e.error));
	return 0;
    }
    else if (rsp.n.nlmsg_len < NLMSG_LENGTH(sizeof(rsp.u.sa)))
    {
	plog("netlink_get_spi: XFRM_MSG_ALLOCSPI returned message with length %lu < %lu bytes; ignore message"
	    , (unsigned long) rsp.n.nlmsg_len
	    , (unsigned long) sizeof(rsp.u.sa));
	return 0;
    }

    DBG(DBG_KLIPS,
	DBG_log("netlink_get_spi: allocated 0x%x for %s"
	    , ntohl(rsp.u.sa.id.spi), text_said));
    return rsp.u.sa.id.spi;
}

const struct kernel_ops linux_kernel_ops = {
	type: KERNEL_TYPE_LINUX,
	inbound_eroute: 1,
	policy_lifetime: 1,
	async_fdp: &netlink_bcast_fd,

	init: init_netlink,
	pfkey_register: linux_pfkey_register,
	pfkey_register_response: linux_pfkey_register_response,
	process_msg: netlink_process_msg,
	raw_eroute: netlink_raw_eroute,
	add_sa: netlink_add_sa,
	del_sa: netlink_del_sa,
	process_queue: NULL,
	grp_sa: NULL,
	get_spi: netlink_get_spi,
};
#endif /* linux && KLIPS */
