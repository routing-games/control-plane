#ifdef OPENLISP
#include "lib.h"
#include "udp.h"
#include <net/lisp/lisp.h>
#include <net/lisp/maptables.h>
/* y5er */
#include "rgl.h"
#include <time.h>
#include <sys/time.h>
/* y5er */

#define PSIZE	4089
static int timeout = MAP_REPLY_TIMEOUT;

struct eid_lookup {
    union sockunion eid;		/* Destination EID */
    int rx;                     /* Receiving socket */
    uint32_t nonce0[MAX_COUNT]; /* First half of the nonce */
    uint32_t nonce1[MAX_COUNT]; /* Second half of the nonce */
    uint16_t sport;             /* EMR inner header source port */
    struct timespec start;      /* Start time of lookup */
    int count;                  /* Current count of retries */
    uint64_t active;            /* Unique lookup identifier, 0 if inactive */
	union sockunion *mr;		/* Point to mapresolver */
	/* y5er */
	struct in_addr source_eid;  /* Source EID */
	/* y5er */
};

/* y5er */
// rg_locator structure
// a locator structure designed to support equilibrium traffic engineering in LISP
// + used to construct routing strategy (both local and remote strategy)
// + weight of each locator will be updated according to the output of routing game
struct rg_locator
{
	uint8_t 			id;
	struct in_addr 		*addr;
	struct map_entry 	*entry; // only applicable for destination locator
	uint8_t 			icost;
	uint8_t 			ecost;
	uint8_t 			weight;
	uint8_t 			selected;
};
/* y5er */

struct eid_lookup lookups[MAX_LOOKUPS];
struct pollfd fds[MAX_LOOKUPS + 1];
int fds_idx[MAX_LOOKUPS +1];
nfds_t nfds = 0;
struct protoent	    *proto;
int udpproto;
int seq;
int openlispsck;
static void map_message_handler(union sockunion *mr);
int check_eid(union sockunion *eid);
void  new_lookup(union sockunion *eid,  union sockunion *mr);
/* y5er */
void  new_lookup_with_src(union sockunion *eid,  union sockunion *mr, struct in_addr *ip_src);
int construct_routing_strategy(int ns, int nd,
							struct rg_locator local_loc[ns],struct rg_locator remote_loc[nd],
							struct routing_strategy strategy[ns*nd]);
void update_dst_locator_weight(int n,struct routing_strategy strategy[],struct rg_locator dst_loc[]);
void calculating_weight(int n,int dst_loc_id,struct routing_strategy strategy[],struct rg_locator src_loc[],struct rg_locator dst_loc[]);

/* y5er */
int  send_mr(int idx);
int read_rec(union map_reply_record_generic *rec);
int opl_add(int s, struct db_node *node, int db);
int opl_del(int s, struct db_node *node, int db);
int opl_get(int s, struct db_node *mapp, int db, struct db_node *rs);
int opl_update(int s, struct db_node *node, uint8_t);


/* y5er */
// routing strategy is constructed from the list of local locator (gateway)
// and the list of RLOC (remote locator)
// TODO
// the egress cost and ingress cost for each locator is manually configured
// consider the technique for dynamically updating these cost
// for egress cost, also need to consider the AS forward cost
int construct_routing_strategy(int ns, int nd,
							struct rg_locator local_loc[ns],struct rg_locator remote_loc[nd],
							struct routing_strategy strategy[ns*nd])
{
	int i,j;
	int n=0;
	for (i=0;i<ns;i++ )
	{
		for (j=0;j<nd;j++)
		{
			strategy[n].s_id 		= i;
			strategy[n].selected 	= 0;
			strategy[n].weight 		= 0;
			strategy[n].src_id	 	= i;
			strategy[n].dst_id 		= j;

			strategy[n].loc_in_cost = local_loc[i].icost;
			strategy[n].loc_eg_cost = local_loc[i].ecost;
			// strategy[i].loc_eg_cost = local_loc[i].ecost + local_as_fwcost[i][j];

			strategy[n].rmt_in_cost = remote_loc[j].icost;
			strategy[n].rmt_eg_cost = remote_loc[j].ecost;
			// strategy[i].rmt_eg_cost = remote_loc[j].ecost + remote_as_fwcost[j][i];

			cp_log(LDEBUG,"\n Strategy %d (%d,%d) %d %d %d %d ",
					n,i,j,strategy[n].loc_in_cost,strategy[n].loc_eg_cost,strategy[n].rmt_in_cost,strategy[n].rmt_eg_cost);
			n++;
		}
	}
	return n;
}


// decide the weight for selected destination locators
// from the result of routing game, the traffic load (or weight) is assgined to selected routing strategies
// to enforce load balancing according to the weight in each local routing strategy
// we need to do a conversion to translate routing strategy's weight to corresponding weight for source and destination locator
// first is to calculate the weight for each selected destination locator
void update_dst_locator_weight(int n,struct routing_strategy strategy[],struct rg_locator dst_loc[])
{
	int i;
	for (i=0;i <n;i++)
		if ( strategy[i].selected )
		{
			int d_id = strategy[i].dst_id;
			dst_loc[d_id].selected 	= 1;
			dst_loc[d_id].weight 	= dst_loc[d_id].weight + strategy[i].weight;
			cp_log(LLOG, " ------ strategy %d is selected with weight = %d \n",i,strategy[i].weight );
			cp_log(LLOG, " ------ update destination loc %d with weight = %d \n",d_id,dst_loc[d_id].weight  );
		}
}


// then for each selected destination locator dst_loc_id, compute the weight for its followed source locators
void calculating_weight(int n,int dst_loc_id,struct routing_strategy strategy[],struct rg_locator src_loc[],struct rg_locator dst_loc[])
{
	int i;
	int sum=0;

	if (dst_loc[dst_loc_id].selected)
		sum = dst_loc[dst_loc_id].weight;

	if (sum)
	{
		// calculate the weight for each source locator that have destination as dst_loc_id
		for (i=0;i<n;i++)
			if ( (strategy[i].selected) && (strategy[i].dst_id == dst_loc_id ))
			{
				src_loc[strategy[i].src_id].selected = 1;
				src_loc[strategy[i].src_id].weight =(strategy[i].weight * 100)/sum;
			}
	}
}

/* y5er */

	size_t
prefix2sockaddr(struct prefix *p, union sockunion *rs)
{	
	char ip[INET6_ADDRSTRLEN];
	inet_ntop(p->family, (void *)&p->u.prefix, ip, INET6_ADDRSTRLEN);
	
	struct addrinfo *res;
	struct addrinfo hints;
	int e; 
    struct protoent	    *proto;
    if ((proto = getprotobyname("UDP")) == NULL) {
		perror ("getprotobyname");
		return 0;
    }
		
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_UNSPEC;	//Allow IPv4 or IPv6 
	hints.ai_socktype  = SOCK_DGRAM;	// Datagram socket 
	hints.ai_flags  = AI_PASSIVE;	
    hints.ai_canonname = NULL;
    hints.ai_addr      = NULL;
    hints.ai_next      = NULL;
	
	if ((e = getaddrinfo(ip, NULL, &hints, &res)) != 0) {
		cp_log(LLOG, "getaddrinfo: %s\n", gai_strerror(e));
		return 0;
	}
		
	//----------------------------
	
	switch (p->family) {
	case AF_INET:
		memcpy(&rs->sin, res->ai_addr, sizeof(struct sockaddr_in));
		return sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		memcpy(&rs->sin6, res->ai_addr, sizeof(struct sockaddr_in6));
		return sizeof(struct sockaddr_in6);
		break;
	default:
		return 0;
	}
	return 0;
}

	size_t
sockaddr2prefix(union sockunion *rs,struct prefix *p) {
	
	switch (rs->sa.sa_family) {
	case AF_INET:
		p->family = AF_INET;
		memcpy(&p->u.prefix4, &rs->sin.sin_addr, sizeof(struct in_addr));	
		return sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		p->family = AF_INET6;
		memcpy(&p->u.prefix6, &rs->sin6.sin6_addr, sizeof(struct in6_addr));						
		return sizeof(struct sockaddr_in6);
		break;
	default:
		return 0;
	}
}

	size_t
_get_sock_size(union sockunion *eid)
{
	size_t ss_len;
	switch (eid->sa.sa_family) {
	case AF_INET:
		ss_len = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		ss_len = sizeof(struct sockaddr_in6);
		break;
	default:
		cp_log(LLOG, "AF not support::%d\n",eid->sa.sa_family);
		return -1;
	}
	return ss_len;
}

/*Get a random map-resolver from list */
	union sockunion *
_get_mr()
{
	int nms;
	int rn;
	struct list_entry_t *rt;
	
	nms =xtr_mr->count;
	rn = ((random()^time(NULL) ) % nms);
	rt = xtr_mr->head.next;
	while (rt != &xtr_mr->tail && rn-- > 0)
		rt = rt->next;
		
	return ((union sockunion *)rt->data);	
}

/* Process message from Openlis socket */
	static void 
map_message_handler(union sockunion *mr)
{
    struct timespec now;
    char msg[PSIZE];         /* buffer for mapping messages */
    int n = 0;              /* number of bytes received on mapping socket */
    union sockunion *eid;
    /*y5er*/
    struct ip *ip_hdr;
    /*y5er*/
    n = read(lookups[0].rx, msg, PSIZE);
    clock_gettime(CLOCK_REALTIME, &now);
	
    if (((struct map_msghdr *)msg)->map_type == MAPM_MISS_EID) {
        eid = (union sockunion *)CO(msg,sizeof(struct map_msghdr));
        /* y5er */
        ip_hdr = (struct ip *)CO(eid,_get_sock_size(eid));
        // get the IP header attached in the MAPM_MISS_EID message
        // this message is sent by data plane when destination EID not found
        if ( ip_hdr != NULL)
        	cp_log(LLOG, "Handling MAPM_MISS_EID with attached IP Header \n");

        /* y5er */
		if (check_eid(eid)) {
			//new_lookup(eid, mr);
			/* y5er */
			// add new EID, also including the "source ip address" to poll
			new_lookup_with_src(eid, mr,&ip_hdr->ip_src);
			/* y5er */
		}
	}
}

/*Check if an EID-prefix exist in poll */
	int 
check_eid(union sockunion *eid)
{
    int i;
	
	for (i = 1; i < MAX_LOOKUPS; i++)
        if (lookups[i].active)
            if (!memcmp(eid, &lookups[i].eid, _get_sock_size(eid))) {				
				return 0;				
			}
    return 1;
}

/*Add new EID with including also the "source ip address" to poll*/
	void
new_lookup_with_src(union sockunion *eid,  union sockunion *mr, struct in_addr *ip_src)
{
	int i,e,r;
	uint16_t sport;             /* inner EMR header source port */
	char sport_str[NI_MAXSERV]; /* source port in string format */
	struct addrinfo hints;
	struct addrinfo *res;


	/* Find an inactive slot in the lookup table */
	for (i = 1; i < MAX_LOOKUPS; i++)
		if (!lookups[i].active)
			break;

	if (i >= MAX_LOOKUPS)
		return;

	if (srcport_rand) {
		/*new socket for map-request */
		if ((r = socket(mr->sa.sa_family, SOCK_DGRAM, udpproto)) < 0) {
			cp_log(LLOG, "Error when create new socket\n");
			return;
		}

		/*random source port of map-request */
		e = -1;
		while (e == -1) {
			sport = MIN_EPHEMERAL_PORT + random() % (MAX_EPHEMERAL_PORT - MIN_EPHEMERAL_PORT);
			sprintf(sport_str, "%d", sport);
			memset(&hints, 0, sizeof(struct addrinfo));
			hints.ai_family    = mr->sa.sa_family;
			hints.ai_socktype  = SOCK_DGRAM;
			hints.ai_flags     = AI_PASSIVE;
			hints.ai_canonname = NULL;
			hints.ai_addr      = NULL;
			hints.ai_next      = NULL;

			if ((e = getaddrinfo(NULL, sport_str, &hints, &res)) != 0) {
				cp_log(LLOG, "getaddrinfo: %s\n", gai_strerror(e));
				e = -1;
				continue;
			}

			if ((e = bind(r, res->ai_addr, res->ai_addrlen)) == -1) {
				cp_log(LLOG, "bind error to port %s\n", sport_str);
				e = -1;
				continue;
			}
			freeaddrinfo(res);
		}
	}else{
		r = (mr->sa.sa_family == AF_INET)? skfd : skfd6;
		sport = LISP_CP_PORT;
	}
	memcpy(&lookups[i].eid, eid, _get_sock_size(eid));
	lookups[i].rx = r;
	lookups[i].sport = sport;
	clock_gettime(CLOCK_REALTIME, &lookups[i].start);
	lookups[i].count = 0;
	lookups[i].active = 1;
	if (mr->sa.sa_family == AF_INET)
		mr->sin.sin_port = htons(LISP_CP_PORT);
	else
		mr->sin6.sin6_port = htons(LISP_CP_PORT);
	lookups[i].mr = mr;
	/* y5er */
	memcpy(&lookups[i].source_eid, ip_src, sizeof(struct in_addr));
	/* y5er */
	send_mr(i);
}
/* y5er */

/*Add new EID to poll*/
	void 
new_lookup(union sockunion *eid,  union sockunion *mr)
{
    int i,e,r;
    uint16_t sport;             /* inner EMR header source port */
    char sport_str[NI_MAXSERV]; /* source port in string format */
    struct addrinfo hints;
    struct addrinfo *res;


    /* Find an inactive slot in the lookup table */
    for (i = 1; i < MAX_LOOKUPS; i++)
        if (!lookups[i].active)
            break;

    if (i >= MAX_LOOKUPS)
	    return;
    	
	if (srcport_rand) {
		/*new socket for map-request */
		if ((r = socket(mr->sa.sa_family, SOCK_DGRAM, udpproto)) < 0) {
			cp_log(LLOG, "Error when create new socket\n");
			return;
		}

		/*random source port of map-request */
		e = -1;
		while (e == -1) {
			sport = MIN_EPHEMERAL_PORT + random() % (MAX_EPHEMERAL_PORT - MIN_EPHEMERAL_PORT);
			sprintf(sport_str, "%d", sport);
			memset(&hints, 0, sizeof(struct addrinfo));
			hints.ai_family    = mr->sa.sa_family; 
			hints.ai_socktype  = SOCK_DGRAM;                
			hints.ai_flags     = AI_PASSIVE;                
			hints.ai_canonname = NULL;
			hints.ai_addr      = NULL;
			hints.ai_next      = NULL;
			
			if ((e = getaddrinfo(NULL, sport_str, &hints, &res)) != 0) {
				cp_log(LLOG, "getaddrinfo: %s\n", gai_strerror(e));	
				e = -1;
				continue;
			}
			
			if ((e = bind(r, res->ai_addr, res->ai_addrlen)) == -1) {
				cp_log(LLOG, "bind error to port %s\n", sport_str);
				e = -1;
				continue;
			}
			freeaddrinfo(res);
		}
	}else{
		r = (mr->sa.sa_family == AF_INET)? skfd : skfd6;
		sport = LISP_CP_PORT;
	}
    memcpy(&lookups[i].eid, eid, _get_sock_size(eid));
    lookups[i].rx = r;
    lookups[i].sport = sport;
    clock_gettime(CLOCK_REALTIME, &lookups[i].start);
    lookups[i].count = 0;
    lookups[i].active = 1;
	if (mr->sa.sa_family == AF_INET)
		mr->sin.sin_port = htons(LISP_CP_PORT);
	else
		mr->sin6.sin6_port = htons(LISP_CP_PORT);
	lookups[i].mr = mr;
    send_mr(i);
}

/* Send map-request */
	int 
send_mr(int idx)
{
    uint32_t nonce0, nonce1;
    int cnt;
    union sockunion *eid;
	char buf[PSIZE];
	struct lisp_control_hdr *lh;
	struct ip *ih;	
	struct ip6_hdr *ih6;
	struct udphdr *udp ;
	struct map_request_hdr *lcm;
	union afi_address_generic *itr_rloc;
	union map_request_record_generic *rec;
	union afi_address_generic afi_addr_src;
	union afi_address_generic afi_addr_dst;
	/* y5er */
	union afi_address_generic *src_eid;
	/* y5er */
	uint8_t *ptr;
	int sockaddr_len;
	size_t itr_size, ip_len;
	char ip[INET6_ADDRSTRLEN];
	int mask; 
	eid = &lookups[idx].eid;
	if (lookups[idx].count >= COUNT && srcport_rand) {
		lookups[idx].active = 0;
		close(lookups[idx].rx);
        return 0;
    }
	bzero(buf,PSIZE);
	lh = (struct lisp_control_hdr *)buf;
	ih = (struct ip *)CO(lh, sizeof(struct lisp_control_hdr));
	ih6 = (struct ip6_hdr *)CO(lh, sizeof(struct lisp_control_hdr));
	
	/*choose source/destionation ip */
	switch (lookups[idx].mr->sa.sa_family ) {
	case AF_INET:
		sockaddr_len = sizeof(struct sockaddr_in);
		break;
	case AF_INET6:
		sockaddr_len = sizeof(struct sockaddr_in6);
		break;
	default:
		cp_log(LLOG, "AF not support\n");
		return -1;
	}
	
	switch (eid->sa.sa_family ) {
	case AF_INET:
		afi_addr_dst.ip.afi = AF_INET;
		memcpy(&afi_addr_dst.ip.address,(struct in_addr *)&(lookups[idx].mr->sin.sin_addr),sizeof(struct in_addr));
		afi_addr_src.ip.afi = AF_INET;
		memcpy(&afi_addr_src.ip.address,(struct in_addr *)(src_addr[0]),sizeof(struct in_addr));
		udp = (struct udphdr *)CO(ih, sizeof(struct ip));			
		break;
	case AF_INET6:
		afi_addr_dst.ip6.afi = AF_INET6;
		memcpy(&afi_addr_dst.ip6.address,(struct in6_addr *)&(lookups[idx].mr->sin6.sin6_addr),sizeof(struct in6_addr));
		afi_addr_src.ip.afi = AF_INET6;
		memcpy(&afi_addr_src.ip6.address,(struct in6_addr *)(src_addr6[0]),sizeof(struct in6_addr));
		udp = (struct udphdr *)CO(ih, sizeof(struct ip6_hdr));			
		break;
	default:
		cp_log(LLOG, "AF not support\n");
		return -1;
	}
	
	lcm = (struct map_request_hdr*)CO(udp, sizeof(struct udphdr));

	/*build message header*/
	/* set all the LISP flags  */
	uint64_t nonce;
	
	_make_nonce(&nonce);
	nonce0 = (uint32_t)nonce;
	nonce1 = (uint32_t)(*((uint32_t *)(((uint8_t *)(&nonce))+4)));
	
	lh->type = LISP_TYPE_ENCAPSULATED_CONTROL_MESSAGE;
	lh->security_bit = 0;
	lh->ddt_originated = 0;
	lcm->lisp_type = LISP_TYPE_MAP_REQUEST;
	lcm->auth_bit = 0;
	lcm->map_data_present = 0;
	lcm->rloc_probe = 0;
	lcm->smr_bit = 0;
	lcm->pitr_bit = 0;
	lcm->smr_invoke_bit = 0;
	lcm->irc = 0;
	lcm->record_count = 1;
	lcm->lisp_nonce0 = htonl(nonce0);
	lcm->lisp_nonce1 = htonl(nonce1);

	/* y5er */
	// add source eid address to request message with AFI=LISP_AFI_IP
	// the field for soruce eid is already defined in legacy map request message
	// src_eid is of type "union afi_address_generic" defined in udp.h include <afi,address>
	// update itr_rloc pointer, point to address after the source eid field
	if (lookups[idx].source_eid.s_addr)
	{
		src_eid = ( union afi_address_generic *)CO(lcm, sizeof(struct map_request_hdr));
		// add source eid <AFI,Address> into the
		// currently only support IPv4 so using LISP_AFI_IP
		src_eid->ip.afi = htons(LISP_AFI_IP);
		memcpy(&src_eid->ip.address, (struct in_addr *)&(lookups[idx].source_eid),sizeof(struct in_addr));
		cp_log(LLOG, "Add Source EID to request message \n");
		itr_rloc = (union afi_address_generic *)CO(src_eid, sizeof(struct afi_address));
	}
	else
	{
		itr_rloc = (union afi_address_generic *)CO(lcm, sizeof(struct map_request_hdr) + 2);
	}
	/* y5er */

	/* set no source EID <AFI=0, addres is empty> -> jump of 2 bytes */
	/* nothing to do as bzero of the packet at init */
	
	//itr_rloc = (union afi_address_generic *)CO(lcm, sizeof(struct map_request_hdr) + 2);

	/* set source ITR */
	switch (lookups[idx].mr->sa.sa_family ) {
	case AF_INET:
		itr_rloc->ip.afi = htons(LISP_AFI_IP);
		itr_size = sizeof(struct afi_address);
		memcpy(&itr_rloc->ip.address, (struct in_addr *)(src_addr[0]), sizeof(struct in_addr));				
		break;
	case AF_INET6:
		itr_rloc->ip6.afi = htons(LISP_AFI_IPV6);			
		memcpy(&itr_rloc->ip6.address, (struct in6_addr *)(src_addr6[0]), sizeof(struct in6_addr));
		itr_size = sizeof(struct afi_address6);	
		break;
	default:
		cp_log(LLOG, "not supported\n");
		return (FALSE);
	}
	rec = (union map_request_record_generic *)CO(itr_rloc, itr_size);

	/* assign correctly the EID prefix */
	switch (eid->sa.sa_family) {
	case AF_INET:
		/* EID prefix is an IPv4 so 32 bits (4 bytes) */
		rec->record.eid_mask_len = mask = 32;
		rec->record.eid_prefix_afi = htons(LISP_AFI_IP);
		memcpy(&rec->record.eid_prefix, &(eid->sin.sin_addr), sizeof(struct in_addr));	
		inet_ntop(AF_INET, (void *)&rec->record.eid_prefix, ip, INET6_ADDRSTRLEN);
		ptr = (uint8_t *)CO(rec,4+4);	
		break;
	case AF_INET6:
		/* EID prefix is an IPv6 so 128 bits (16 bytes) */ 
		rec->record6.eid_mask_len = mask = 128;
		rec->record.eid_prefix_afi = htons(LISP_AFI_IPV6);
		memcpy(&rec->record6.eid_prefix, &(eid->sin6.sin6_addr), sizeof(struct in6_addr));
		inet_ntop(AF_INET6, (void *)&rec->record6.eid_prefix, ip, INET6_ADDRSTRLEN);			
		ptr = (uint8_t *)CO(rec,4+16);	
		break;
	default:
		cp_log(LLOG, "not supported\n");
		return (FALSE);
	}
	
	/* set the UDP parameters */
#ifdef BSD
	udp->uh_sport = htons(lookups[idx].sport);
	udp->uh_dport = htons(LISP_CP_PORT);
	udp->uh_ulen = htons((uint8_t *)ptr - (uint8_t *) udp);
	udp->uh_sum = 0;
#else
	udp->source = htons(lookups[idx].sport);
	udp->dest = htons(LISP_CP_PORT);
	udp->len = htons((uint8_t *)ptr - (uint8_t *) udp );
	udp->check = 0;
#endif

	/* setup the IP parameters */
	switch (eid->sa.sa_family ) {
	case AF_INET:
		ip_len = (uint8_t *)ptr - (uint8_t *) ih;
		ih->ip_hl         = 5;
		ih->ip_v          = 4;
		ih->ip_tos        = 0;
		ih->ip_len        = htons(ip_len);
		ih->ip_id         = htons(0);
		ih->ip_off        = 0;
		ih->ip_ttl        = 255;
		ih->ip_p          = IPPROTO_UDP;
		ih->ip_sum        = 0;         
		ih->ip_src.s_addr = afi_addr_src.ip.address.s_addr;
		ih->ip_dst.s_addr = eid->sin.sin_addr.s_addr;
		ih->ip_sum        = ip_checksum((uint16_t *)ih, (ih->ip_hl)*2);
		break;
	case AF_INET6:
		ip_len = (uint8_t *)ptr - (uint8_t *)ih;
		ih6->ip6_vfc	  = 0x6E; //version
		ih6->ip6_plen	  = htons(ip_len); //payload length
		ih6->ip6_nxt      = IPPROTO_UDP;//nex header
		ih6->ip6_hlim     = 64; //hop limit      
		memcpy(&ih6->ip6_src, &afi_addr_src.ip6.address, sizeof(struct in6_addr));
		memcpy(&ih6->ip6_dst, &eid->sin6.sin6_addr, sizeof(struct in6_addr));			
		break;		
	}
	
	char ip2[INET6_ADDRSTRLEN];
	if (sendto(lookups[idx].rx, (void *)buf, (uint8_t *)ptr - (uint8_t *)lh, 0, 
						&(lookups[idx].mr->sa), sockaddr_len) < 0) {
		cp_log(LLOG, "\n#Error send Map-Request to %s:%d <nonce=0x%x - 0x%x>\n", \
						sk_get_ip(lookups[idx].mr, ip2) , sk_get_port(lookups[idx].mr),\
						nonce0, nonce1);			
		cp_log(LDEBUG, "   EID %s/%d\n",ip,mask);		
        return 0;
    } else {
        cnt = lookups[idx].count;
        lookups[idx].nonce0[cnt] = nonce0;
        lookups[idx].nonce1[cnt] = nonce1;
		lookups[idx].count++;
		
		
		cp_log(LLOG, "\n#Send Map-Request to %s:%d <nonce=0x%x - 0x%x>\n", \
						sk_get_ip(lookups[idx].mr, ip2) , sk_get_port(lookups[idx].mr),\
						nonce0, nonce1);			
		cp_log(LDEBUG, "   EID %s/%d\n",ip,mask);		
	}   
    return 1;
}

/* Process with map-reply */
	int
read_rec(union map_reply_record_generic *rec)
{
	size_t rlen;
	union map_reply_locator_generic *loc;
	char buf[BSIZE];
	size_t len;
	struct map_entry *entry;
	uint8_t lcount;
	struct prefix eid;
	struct mapping_flags mflags;
	void *mapping;
	struct db_node node;
	struct lcaf_hdr *lcaf;
	union rloc_te_generic *hop;
	void *barr;
	/* y5er */
	struct list_entry_t *db_entry;
	struct db_node *local_map_node = NULL;
	db_entry = etr_db->head.next;
	int is_peer = 0;
	int use_local_opt = 0 ;
	/* y5er */
	
	node.flags = NULL;
	rlen = 0;
	bzero(buf, BSIZE);
	mapping = NULL;
	
	bzero(&eid, sizeof(struct prefix));
	switch (ntohs(rec->record.eid_prefix_afi)) {
	case LISP_AFI_IP:
		eid.family = AF_INET;
		eid.u.prefix4 = rec->record.eid_prefix;
		inet_ntop(AF_INET, (void *)&eid.u.prefix4, buf, BSIZE);
		rlen += sizeof(struct map_reply_record);
		break;
	case LISP_AFI_IPV6:
		eid.family = AF_INET6;
		eid.u.prefix6 = rec->record6.eid_prefix;
		inet_ntop(AF_INET6, (void *)&eid.u.prefix6, buf, BSIZE);
		rlen += sizeof(struct map_reply_record6);
		break;
	default:
		cp_log(LLOG, "unsuported family\n");
		return (0);
	}
	
	eid.prefixlen = rec->record.eid_mask_len;
	lcount = rec->record.locator_count;
	bzero(&mflags, sizeof(struct mapping_flags));
	mflags.act = rec->record.act;
	mflags.A = rec->record.a;
	mflags.version = rec->record.version;
	mflags.ttl = ntohl(rec->record.ttl);
	memcpy(&node.p, &eid, sizeof(struct prefix));
	generic_mapping_set_flags(&node, &mflags);
	node.info = list_init();
	
	/* y5er */
	struct timeval start_process_reply;			// map reply received and processed
	struct timeval finish_build_routing_game;	// finish constructing the routing game
	struct timeval finish_update_cp_mapping;	// the mapping db at control plane is updated
	struct timeval finish_send_map_add;			// map add message is sent
	/* y5er */

	gettimeofday(&start_process_reply,NULL);
	cp_log(LDEBUG, " Processing the map reply message at %ld \n",start_process_reply.tv_sec*1000000+start_process_reply.tv_usec);


	/* ====================================================== */
	cp_log(LDEBUG, "  EID %s/%d: ", buf, eid.prefixlen);
	cp_log(LDEBUG, "<");
	cp_log(LDEBUG,  "Lcount=%u", lcount);

	cp_log(LDEBUG, ", ");
	cp_log(LDEBUG, "TTL=%u", mflags.ttl);

	if (lcount == 0) {
		cp_log(LDEBUG, ", ");
		cp_log(LDEBUG, "ACT=%d", mflags.act);
	}

	cp_log(LDEBUG, ", ");
	cp_log(LDEBUG, "version=%u", mflags.version);

	cp_log(LDEBUG, ", ");
	cp_log(LDEBUG, "A=%u", mflags.A);

	cp_log(LDEBUG, ">\n");
	
	/* ====================================================== */

	/* y5er */

	// both n_src and n_dst are used for constructing the local and remote locator array
	// they support routing game building functions
	int n_src = 0;
	// maximum number of source locator
	int n_dst = lcount;
	// set the maximum number of destination locator to lcount
	// the number of locators that are considered in the routing game could be smaller
	// we only count RLOC with RC flag on ( and local )

	// check the received EID to determine whether it is peer with any local EID or not
	// if yes, extra step is required for processing the message
	// using the local_map_node to keep track of the mapping entry of corresponding local EID
	if ( _fncs & (_FNC_XTR | _FNC_RTR )) {
		while ( db_entry != &etr_db->tail )
		{
			if ((local_map_node = (struct db_node *)(db_entry->data)))
			{
				struct mapping_flags *mp_flag = (struct mapping_flags *)local_map_node->flags;
				if ( mp_flag->local_opt )
				{
					cp_log(LDEBUG, " Local Optimize \n");
					use_local_opt = 1;
				}

				char peer_ipadd[BSIZE];
				bzero(peer_ipadd, BSIZE);
				inet_ntop(AF_INET,(void *)&local_map_node->peer.u.prefix4, peer_ipadd, BSIZE);

				// if ( !memcmp(&(local_map_node->peer.u.prefix4),&(eid.u.prefix4),sizeof(struct in_addr)) )
				if (strcmp(peer_ipadd,buf)==0)  //buf is currently set as eid.u.prefix
				{
					is_peer = 1;
					cp_log(LDEBUG, " peer eid  \n");

					struct list_t *tmp_l;
					if (!(tmp_l = (struct list_t *)(local_map_node->info)) || (tmp_l->count <= 0) )
						return 0;

					// update the max number of source locator by total number of locator in the local mapping entry
					n_src = tmp_l->count;

					break;
				}
			}
			db_entry = db_entry->next;
		}
	}


	int all_src_loc_added = 0;
	// same list of src_loc added for all destination locator
	// so just need to process one to contruct the rg_src_locator array

	int i_src = 0; // index for rg_src_locator array
	int i_dst = 0; // index for dst_src_locator array
	struct rg_locator rg_src_locator[n_src];
	struct rg_locator rg_dst_locator[n_dst];
	int n_nongame_rloc = 0; // count the number of advertised RLOC but have RC flag off or not a local one
	/* y5er */
	// TODO
	// consider the case when one loc is play routing game (RC flag on), the other not playing ?
	// how we can handle this specific case
	// currently the entry will also bit added to the local cache, with no list of source locator
	// then also add to the data plane with no source locator
	// how the incoming packet is processed at the data plane ?
	// if the priority of these two loc are the same, and the non-play loc also have weight != 0
	// how the data plane handled ? the source IP address encapsulation could get issue
	// also it is incorrect behavior for having that case
	// to avoid confusion in that case, for non-playing loc set priority to 100
	// OR we can just not allow that case to happend
	// for an map reply from peer, all the advertised RLOC should participate in game

	loc = (union map_reply_locator_generic *)CO(rec, rlen);

	/* ==================== RLOCs ========================= */
	while (lcount--) {
		bzero(buf, BSIZE);
		entry = (struct map_entry *)calloc(1, sizeof(struct map_entry));
		entry->priority = loc->rloc.priority;
		entry->weight = loc->rloc.weight;
		entry->m_priority = loc->rloc.m_priority;
		entry->m_weight = loc->rloc.m_weight;
		entry->r = loc->rloc.R;
		entry->L =loc->rloc.L;
		entry->p = loc->rloc.p;
		/* y5er */
		entry->RC = loc->rloc.RC; //include routing cost or not
		/* y5er */
		lcaf = (struct lcaf_hdr *)&loc->rloc.rloc_afi;

		// traffic engineering
		if (ntohs(lcaf->afi) == LCAF_AFI && lcaf->type == LCAF_TE) {
			struct sockaddr_in hop_inet;
			struct sockaddr_in6 hop_inet6;
			
			int pec = 0;
			int rtr = 0;
						
			barr = (void *)CO(lcaf,sizeof(struct lcaf_hdr)+ntohs(lcaf->payload_len));
			hop = (union rloc_te_generic *)CO(lcaf,sizeof(struct lcaf_hdr));
			
			/* run over pe 
				if lisp_te 
					if xTR --> get the first hop
					if RTR --> get the hop after RTR
				if not lisp_te --> get last hop				
			*/

			cp_log(LDEBUG, "\t•[rloc=ELP, priority=%u, weight=%u, m_priority=%u, m_weight=%u, r=%d, L=%d, p=%d]\n", \
						entry->priority, \
						entry->weight, \
						entry->m_priority, \
						entry->m_weight, \
						entry->r, \
						entry->L, \
						entry->p);
			while ((char *)hop < (char *)barr) {
				switch (ntohs(hop->rloc.afi)) {
				case LISP_AFI_IP:
					inet_ntop(AF_INET, (void *)&hop->rloc.hop_addr, buf, BSIZE);
					cp_log(LDEBUG, "\t\t•[hop=%s]\n",buf);							
					hop = (union rloc_te_generic *)CO(hop,sizeof(struct rloc_te));
					break;						
				case LISP_AFI_IPV6:
					inet_ntop(AF_INET6, (void *)&hop->rloc6.hop_addr, buf, BSIZE);
					cp_log(LDEBUG, "\t\t•[hop=%s]\n",buf);
					
					hop = (union rloc_te_generic *)CO(hop,sizeof(struct rloc6_te));
					break;
				default:
					cp_log(LLOG, "unsuported family\n");
					free(entry);
					return (0);
				}					
			}
			
			hop = (union rloc_te_generic *)CO(lcaf,sizeof(struct lcaf_hdr));
			while ((char *)hop < (char *)barr) {
				switch (ntohs(hop->rloc.afi)) {
				case LISP_AFI_IP:
					/* xTR get first hop in pe */
					if (!pec && lisp_te && (_fncs & _FNC_XTR)) {
						entry->rloc.sin.sin_family = AF_INET;
						memcpy(&entry->rloc.sin.sin_addr, &hop->rloc.hop_addr, sizeof(struct in_addr));
						hop = barr;
						loc = barr;							
						continue;
					}
					/* RTR get next hop after it in pe */
					if (lisp_te && (_fncs & _FNC_RTR)) {
						if (!rtr) {
							/* check if hop's ip is rtr's ip  */
							hop_inet.sin_family = AF_INET;
							hop_inet.sin_addr.s_addr = hop->rloc.hop_addr.s_addr;
							if (is_my_addr((union sockunion *)&hop_inet))
								rtr = 1;								
						}
						else{
							entry->rloc.sin.sin_family = AF_INET;
							memcpy(&entry->rloc.sin.sin_addr, &hop->rloc.hop_addr,sizeof(struct in_addr));
							hop = barr;
							loc = barr;
							rtr = 0;
							continue;								
						}
					}
					
					/* not lisp_te function get last hop */
					if (!lisp_te && (CO(hop,sizeof(struct rloc_te) >= (char *)barr )) ) {
						entry->rloc.sin.sin_family = AF_INET;
						memcpy(&entry->rloc.sin.sin_addr, &hop->rloc.hop_addr,sizeof(struct in_addr));
						hop = barr;
						loc = barr;							
						continue;
					}
					hop = (union rloc_te_generic *)CO(hop,sizeof(struct rloc_te));
					break;						
				case LISP_AFI_IPV6:
					/* xTR get first hop in pe */
					if (lisp_te && !pec && (_fncs & _FNC_XTR)) {
						entry->rloc.sin6.sin6_family = AF_INET6;
						memcpy(&entry->rloc.sin6.sin6_addr, &hop->rloc6.hop_addr, sizeof(struct in6_addr));
						hop = barr;
						loc = barr;
						continue;
					}
					/* RTR get next hop after it in pe */
					if (lisp_te && (_fncs & _FNC_RTR)) {
						if (!rtr) {
							hop_inet6.sin6_family = AF_INET6;
							memcpy(&hop_inet6.sin6_addr,&hop->rloc6.hop_addr,sizeof(struct in6_addr));
							if (is_my_addr((union sockunion *)&hop_inet6))
								rtr = 1;
						}
						else{
							entry->rloc.sin6.sin6_family = AF_INET6;
							memcpy(&entry->rloc.sin6.sin6_addr, &hop->rloc6.hop_addr,sizeof(struct in6_addr));
							hop = barr;
							loc = barr;
							rtr = 0;
							continue;								
						}
					}
					/* not lisp_te function get last hop */
					if ((char *)(hop + sizeof(struct rloc6_te)) > (char *)barr) {
						entry->rloc.sin6.sin6_family = AF_INET6;
						memcpy(&entry->rloc.sin6.sin6_addr, &hop->rloc6.hop_addr,sizeof(struct in6_addr));
						hop = barr;
						loc = barr;
						continue;
					}
					hop = (union rloc_te_generic *)CO(hop,sizeof(struct rloc6_te));
					break;
				default:
					cp_log(LLOG, "unsuported family\n");
					free(entry);
					return (0);
				}; /* end switch */
				pec++;
			}
			loc = barr;		
		}
		else{
			switch (ntohs(loc->rloc.rloc_afi)) {
			case LISP_AFI_IP:
				entry->rloc.sin.sin_family = AF_INET;
				memcpy(&entry->rloc.sin.sin_addr, &loc->rloc.rloc, sizeof(struct in_addr));					
				len = sizeof(struct map_reply_locator);
				break;
			case LISP_AFI_IPV6:
				entry->rloc.sin6.sin6_family = AF_INET6;
				memcpy(&entry->rloc.sin6.sin6_addr, &loc->rloc6.rloc, sizeof(struct in6_addr));					
				len = sizeof(struct map_reply_locator6);
				break;
			default:
				cp_log(LLOG, "unsuported family\n");
				free(entry);
				return (0);
			}
			
			inet_ntop(entry->rloc.sin.sin_family, (void *)&loc->rloc.rloc, buf, BSIZE);
			cp_log(LDEBUG, "\t•[rloc=%s, priority=%u, weight=%u, m_priority=%u, m_weight=%u, r=%d, L=%d, p=%d]\n", \
					buf, \
					entry->priority, \
					entry->weight, \
					entry->m_priority, \
					entry->m_weight, \
					entry->r, \
					entry->L, \
					entry->p);
			
			loc = (union map_reply_locator_generic *)CO(loc, len);	
		}

		/* y5er begin */
		// using local traffic engineering method for that remote eid
		// for each destination RLOC, add a list of local source locator
		// we prioritize the use_local_opt than the equilibirium, if local_opt flag is on
		// so ignore the routing cost , not building the game
		if (use_local_opt)
		{
			int cnt = 0;
			if ( _fncs & (_FNC_XTR | _FNC_RTR ))
			{
				if (local_map_node)
				{
					struct list_t 		*ll;
					struct list_entry_t *rl_entry;
					struct map_entry 	*rl;

					if (!(ll = (struct list_t *)(local_map_node->info)) || (ll->count <= 0) )
						return 0;
					rl_entry = ll->head.next;

					// create a list of source locator
					struct list_t *src_loc_list;
					struct source_locator *src_loc;
					entry->src_loc = src_loc_list = list_init();

					// go throught the list of all RLOC in the local mapping entry
					while (rl_entry != &ll->tail)
					{
						rl = (struct map_entry*)rl_entry->data;
						// RLOC is a local locator,then add to the new source locator list src_loc_list
						if (rl->L)
						{
							// create new source locator
							src_loc 						= calloc(1,sizeof(struct source_locator));
							src_loc->weight 				= rl->weight;
							src_loc->priority 				= rl->priority;
							src_loc->addr.sin.sin_family 	= AF_INET;
							memcpy(&src_loc->addr.sin.sin_addr,&rl->rloc.sin.sin_addr,sizeof(struct in_addr));

							list_insert(src_loc_list,src_loc,NULL);
							cnt++;
						}
						rl_entry = rl_entry->next;
					}
				}
				entry->src_loc_count = cnt; // update the number of source locator count
				}

		}


		// received EID peers with one of the local EID, the check for is_peer is done before when processing the map header
		// local_map_node points to the corresponding mapping entry for that local EID in the local_db
		// now attach to each of the advertised RLOC a same list of local locator (source locator) collected from the local_node
		if (is_peer && !use_local_opt )
		{
			if ( entry->RC && entry->L )
			{
				int count = 0;
				if ( _fncs & (_FNC_XTR | _FNC_RTR ))
				{
					if (local_map_node)
					{
						struct list_t 		*ll;
						struct list_entry_t *rl_entry;
						struct map_entry 	*rl;

						if (!(ll = (struct list_t *)(local_map_node->info)) || (ll->count <= 0) )
							return 0;
						rl_entry = ll->head.next;

						// create a list of source locator
						struct list_t *src_loc_list;
						struct source_locator *src_loc;
						entry->src_loc = src_loc_list = list_init();

						// go throught the list of all RLOC in the local mapping entry
						while (rl_entry != &ll->tail)
						{
							rl = (struct map_entry*)rl_entry->data;

							// RLOC is a local locator,then add to the new source locator list src_loc_list
							if (rl->L)
							{
								// create new source locator
								src_loc 						= calloc(1,sizeof(struct source_locator));
								src_loc->weight 				= rl->weight;
								src_loc->priority 				= rl->priority;
								src_loc->addr.sin.sin_family 	= AF_INET;
								memcpy(&src_loc->addr.sin.sin_addr,&rl->rloc.sin.sin_addr,sizeof(struct in_addr));

								list_insert(src_loc_list,src_loc,NULL);
								count++;

								// add to rg_src_locator array - support routing game
								// this array is constructed one time, when the first destination RLOC is processed
								if (i_src < n_src && !all_src_loc_added)
								{
									rg_src_locator[i_src].id 		= i_src;
									rg_src_locator[i_src].addr 		= &src_loc->addr.sin.sin_addr;
									rg_src_locator[i_src].icost 	= rl->i_cost;
									rg_src_locator[i_src].ecost 	= rl->e_cost;
									rg_src_locator[i_src].weight 	= 0;
									rg_src_locator[i_src].selected 	= 0;
									i_src++;
								}
							}
							rl_entry = rl_entry->next;
						}
						if ( i_src < 2 )
						{
							cp_log(LLOG, " Routing game required more than one local RLOC \n");
							free(entry);
							return (0);
						}
						if ( i_src == count && !all_src_loc_added )
							all_src_loc_added++;
					}
					entry->src_loc_count = count; // update the number of source locator count
				}
				// add to rg_dst_locator array - support routing game
				if (i_dst < n_dst)
				{
					rg_dst_locator[i_dst].id 		= i_dst;
					rg_dst_locator[i_dst].addr 		= &entry->rloc.sin.sin_addr;
					rg_dst_locator[i_dst].entry 	= entry;
					rg_dst_locator[i_dst].icost 	= entry->priority;
					rg_dst_locator[i_dst].ecost 	= entry->weight;
					rg_dst_locator[i_dst].weight 	= 0;
					rg_dst_locator[i_dst].selected 	= 0;
					i_dst++;
				}
			}
			else // RC flag is off or RLOC is not local at the remote site, RLOC does not particiate in routing game
			{
				entry->src_loc_count 	= 0;
				entry->priority 		= 100+n_nongame_rloc;
				entry->weight 			= 100;
				n_nongame_rloc++;
			}
		}
		/* y5er end */

		/* add locator to the table */
		rlen = (char *)loc - (char *)rec;	
		assert((struct list_t *)node.info);
		struct list_entry_t *m;
		struct map_entry *n_entry;
		if (entry->rloc.sa.sa_family) {
			if (!(m = list_search(node.info, entry,entrycmp))) {
				list_insert((struct list_t *)node.info, entry, NULL);	
			}				
			else{				
				/* new rloc exist, only update priority and pe */
				n_entry = (struct map_entry *)m->data;
				if (n_entry->priority > entry->priority) { 
					m->data = entry;					
					free(n_entry);
				}
				else
					free(entry);
			}			
		}
		else{
			free(entry);
			return 0;
		}
	// continue with next RLOC (destination locator)
	}

	/* y5er begin */
	// update the weight and priority for entry before adding to data plane

	// only perform routing game update when not using use_local_opt
	if (!use_local_opt)
	{
		// i_src and i_dst are number of source and destination locator that join routing game
		if ( is_peer && (i_dst < 2 || i_src < 2) )
		{
			// Not able to build the routing game, return a warning
			cp_log(LDEBUG, " Routing game required more than 1 destination RLOC \n");
			// TODO: free allocated resource, it is not added to dataplane but it is already created at control plane
			return 0;
		}

		// converting the rg_src_loc and rg_dst_loc into local and remote routing strategy arrays
		// and building the routing game
		if ( is_peer && i_dst > 1 && i_src > 1 )
		{
			struct routing_strategy local_strategy[i_src*i_dst],remote_strategy[i_src*i_dst];
			construct_routing_strategy(i_src,i_dst,rg_src_locator,rg_dst_locator,local_strategy);
			construct_routing_strategy(i_dst,i_src,rg_dst_locator,rg_src_locator,remote_strategy);

			// TODO: update rgl, since the thresold is set by user, it is depended on the distribution of potential value
			if ( !routing_game_result_LISP(i_src*i_dst,local_strategy,remote_strategy) )
			{
				cp_log(LDEBUG, " Error while building the routing game \n");
				return 0;
			}

			gettimeofday(&finish_build_routing_game,NULL);
			cp_log(LDEBUG, " Time to contruct routing game %ld \n",(finish_build_routing_game.tv_sec*1000000+finish_build_routing_game.tv_usec)
									- (start_process_reply.tv_sec*1000000+start_process_reply.tv_usec) );
			//cp_log(LDEBUG, " Routing game constructed at %ld \n",finish_build_routing_game.tv_sec*1000000+finish_build_routing_game.tv_usec);

			update_dst_locator_weight(i_src*i_dst,local_strategy,rg_dst_locator);
			int i;
			for (i=0;i<i_dst;i++) // check all destination locator in rg_dst_locator array, and update their corresponding entry in the db
			{
				struct map_entry *correspond_entry =  rg_dst_locator[i].entry;
				if (rg_dst_locator[i].selected)
				{
					char buff[BSIZE];
					bzero(buff,BSIZE);
					inet_ntop(AF_INET,(void *)rg_dst_locator[i].addr, buff, BSIZE);
					cp_log(LDEBUG, " Destination locator %s with weight %d \n",buff,rg_dst_locator[i].weight);

					// update weight and priority for the corresponding entry
					correspond_entry->priority 	= 1;  // all selected locator have same priority of 1
					correspond_entry->weight 	= rg_dst_locator[i].weight; // weight is set according to the calculated weight

					// caculating weight for each source locator (each destination RLOC maintain a list of source locator)
					calculating_weight(i_src*i_dst,rg_dst_locator[i].id,local_strategy,rg_src_locator,rg_dst_locator);
					// NOTE: one rg_src_locator array is used for all destination locator
					// for next destination locator the weight for each source in this array will be updated with different value
					// the purpose is to update the db in data plane, so we just temporarily put it in the rg_src_locator

					// update the source locator list for corresponding entry
					struct list_t *src_loc_ls = correspond_entry->src_loc;
					struct list_entry_t 	*src_loc_entry;
					struct source_locator 	*src_loc_tmp;
					int j;
					int src_selected;
					// find correspond source entry in the src_loc_ls list and update weight
					src_loc_entry = src_loc_ls->head.next;

					while (src_loc_entry != &src_loc_ls->tail) {
						src_selected = 0;
						src_loc_tmp = (struct source_locator *)src_loc_entry->data;
						bzero(buff,BSIZE);
						inet_ntop(AF_INET,(void *)&src_loc_tmp->addr.sin.sin_addr, buff, BSIZE);
						// check each src locator in the list with all the locator in the rg_src_locator array
						for (j=0;j<i_src;j++)
						{
							char ip_buf[BSIZE];
							bzero(ip_buf,BSIZE);
							inet_ntop(AF_INET,(void *)rg_src_locator[j].addr, ip_buf, BSIZE);

							// use string compare to compare two ip address
							if ( (rg_src_locator[j].selected) && (strcmp(ip_buf,buff)==0) )
							{
								cp_log(LDEBUG, " ++ Source locator %s with weight %d \n",ip_buf,rg_src_locator[j].weight);

								src_loc_tmp->priority 	= 1; // priority for source locator is not used in the data plane yet
								src_loc_tmp->weight  	= rg_src_locator[j].weight;
								src_selected++;
								break;
							}
						}
						// if this src_locator is not selected, set weight = 0
						if ( !src_selected )
						{
							src_loc_tmp->priority 	= 100;
							src_loc_tmp->weight  	= 0;
						}
						// move to next src locator
						src_loc_entry = src_loc_entry->next;
					}
				}
				else // the destination locator is not selected, update cooresponding entry with high priority
				{
					correspond_entry->priority 	= 99; // priority >= is for non game locator
					correspond_entry->weight 	= 10; // remember the total weight for RLOC of same prioirty must smaller than 100
				}
			}

			gettimeofday(&finish_update_cp_mapping,NULL);
			cp_log(LDEBUG, " Time to update mapping db %ld \n",(finish_update_cp_mapping.tv_sec*1000000+finish_update_cp_mapping.tv_usec)
						- (start_process_reply.tv_sec*1000000+start_process_reply.tv_usec) );
			// cp_log(LDEBUG, " The mapping entry is updated at %ld \n",finish_update_cp_mapping.tv_sec*1000000+finish_update_cp_mapping.tv_usec);
		}
	}
	/* y5er */

	/* add to OpenLISP mapping cache */
	opl_add(openlispsck, &node, 0);

	gettimeofday(&finish_send_map_add,NULL);
	cp_log(LDEBUG, " Map_add message is sent to the dataplane at %ld \n",finish_send_map_add.tv_sec*1000000+finish_send_map_add.tv_usec);

	// record the total delay from processing the map reply message until the MAP_ADD message is sent
	// expect very low delay for legacy LISP and higher delay for routing game based solution

	cp_log(LDEBUG, " Total delay  %ld \n",(finish_send_map_add.tv_sec*1000000+finish_send_map_add.tv_usec)
			- (start_process_reply.tv_sec*1000000+start_process_reply.tv_usec) );

	if (node.info)
		list_destroy((struct list_t *)node.info, NULL);
	return (rlen);
}

/* get map-reply */
	int 
read_mr(int idx)
{
	int i;
	int rcvl;
	char buf[PSIZE];
	union sockunion si;
	struct map_reply_hdr *lh;
	union map_reply_record_generic *lcm;
	uint32_t nonce0, nonce1;
	socklen_t sockaddr_len;
	int rec_len;
	char ip[INET6_ADDRSTRLEN];
	
	if (lookups[idx].mr->sa.sa_family == AF_INET)
		sockaddr_len = sizeof(struct sockaddr_in);
	else
		sockaddr_len = sizeof(struct sockaddr_in6);
		
	/* read package */

	if ((rcvl = recvfrom(lookups[idx].rx,
			 buf,
			 PSIZE,
			0,
			(struct sockaddr *)&(si.sa),
			&sockaddr_len)) < 0) {
		return 0;
	}
	
	/*only accept map-reply with not empty record */
	lh = (struct map_reply_hdr *)buf;	
	if (lh->lisp_type != LISP_TYPE_MAP_REPLY) {
		return 0;
	}
	/* check nonce to see reply for what */
	nonce0 = ntohl(lh->lisp_nonce0);
	nonce1 = ntohl(lh->lisp_nonce1);
	cp_log(LLOG, "\n#Received Map-Reply from %s:%d <nonce=0x%x - 0x%x>\n",\
					sk_get_ip(&si, ip) , sk_get_port(&si),\
					nonce0,nonce1);
	
		
	for (i = 0;i <= MAX_COUNT ; i++) {
		if (lookups[idx].nonce0[i] == nonce0 && lookups[idx].nonce1[i] == nonce1)
			break;		
	}
	if (i > MAX_COUNT)
		return 0;
		
	if (lh->record_count <= 0)
		return 0;

	/* process map-reply */
	lcm = (union map_reply_record_generic *)CO(lh,sizeof(struct  map_reply_hdr));
	
	for (i = 0; i < lh->record_count; i++) {
		if ((rec_len = read_rec(lcm)) < 0) {
			cp_log(LLOG, "Record error\n");
			return -1;
		}
		lcm = (union map_reply_record_generic *)CO(lcm,rec_len);
	}
	
	lookups[idx].active = 0;
    	if (srcport_rand)
		close(lookups[idx].rx);
	return 0;
}

	int 
get_mr(void *data)
{
	int idx, i;
	struct pk_req_entry *pke;
	union sockunion *si;
	struct map_reply_hdr *lh;
	union map_reply_record_generic *lcm;
	uint32_t nonce0, nonce1;	
	int rec_len;
	char ip[INET6_ADDRSTRLEN];
	char *buf;
	
	pke = (struct pk_req_entry *)data;
	buf = pke->buf;
	si = &pke->si;
	
	/*only accept map-reply with not empty record */
	lh = (struct map_reply_hdr *)buf;	
	if (lh->lisp_type != LISP_TYPE_MAP_REPLY) {
		return 0;
	}
	/* check nonce to see reply for what */
	nonce0 = ntohl(lh->lisp_nonce0);
	nonce1 = ntohl(lh->lisp_nonce1);
	cp_log(LLOG, "\n#Received Map-Reply from %s:%d <nonce=0x%x - 0x%x>\n",\
					sk_get_ip(si, ip) , sk_get_port(si),\
					nonce0,nonce1);
			
	for (idx = 1; idx < MAX_LOOKUPS; idx++) {
		for (i = 0; i <= MAX_COUNT ; i++) {
			if (lookups[idx].nonce0[i] == nonce0 && lookups[idx].nonce1[i] == nonce1)
				break;		
		}
		if (i > MAX_COUNT)
			continue;
		else
			break;
	}	
	
	if (idx > MAX_LOOKUPS)
		return 0;
		
	if (lh->record_count <= 0)
		return 0;

	/* process map-reply */
	lcm = (union map_reply_record_generic *)CO(lh,sizeof(struct  map_reply_hdr));
	
	for (i = 0; i < lh->record_count; i++) {
		if ((rec_len = read_rec(lcm)) < 0) {
			cp_log(LLOG, "Record error\n");
			return -1;
		}
		lcm = (union map_reply_record_generic *)CO(lcm,rec_len);
	}	
	lookups[idx].active = 0;    
	return 0;
}

/* Main poll function */

	static void 
event_loop(void)
{	
	for (;;) {
        int e, i, j, l = -1;
        int poll_timeout = INFTIM; /* poll() timeout in milliseconds. We initialize
                                   to INFTIM = -1 (infinity). If there are no
                                   active lookups, we wait in poll() until a
                                   mapping socket event is received. */
        struct timespec now, deadline, delta, to, tmp;
		
        to.tv_sec  = timeout;
        to.tv_nsec = 0;

        nfds = 1;

        clock_gettime(CLOCK_REALTIME, &now);

        for (i = 1; i < MAX_LOOKUPS; i++) {
            if (!(lookups[i].active)) continue;

            deadline.tv_sec = lookups[i].start.tv_sec + (lookups[i].count +1) * timeout; 
            deadline.tv_nsec = lookups[i].start.tv_nsec;

            timespec_subtract(&delta, &deadline, &now);
			if (srcport_rand) {
				fds[nfds].fd     = lookups[i].rx;
				fds[nfds].events = POLLIN;
				fds_idx[nfds]    = i;
				nfds++;
			}
			
            /* Find the minimum delta */
            if (timespec_subtract(&tmp, &delta, &to)) {
				to.tv_sec    = delta.tv_sec;
                to.tv_nsec   = delta.tv_nsec;
                poll_timeout = to.tv_sec * 1000 + to.tv_nsec / 1000000;
                if (to.tv_sec < 0) poll_timeout = 0;
                l = i;
            }			
        } /* Finished iterating through all lookups */

        e = poll(fds, nfds, poll_timeout);
        if (e < 0) continue;
        if (e == 0)                             /* If timeout expires */
            if (l >= 0)                         /* and slot is defined */
	         send_mr(l);                    /* retry Map-Request */
        for (j = nfds - 1; j >= 0; j--) {
            if (fds[j].revents == POLLIN) {
				if (j == 0)
                    map_message_handler(_get_mr());
                else
                    read_mr(fds_idx[j]);
            }
        }
    }
}

/* Main function of thread with intergrate with OpenLisp */
/* Size of socket must be multiple of long (from OpenLISP code) 
	so size of sockaddr_in6 is 32 instead 28 
*/
#define SS_LEN(ss)							\
    ( (!(ss) || ((struct sockaddr_storage *)(ss))->ss_len == 0) ?	\
       sizeof(long)		:					\
	1 + ((((struct sockaddr_storage *)(ss))->ss_len - 1) | (sizeof(long) - 1) ) )
	void *
plugin_openlisp(void *data)
{
	int i;
	struct protoent	    *proto;

	if ((proto = getprotobyname("UDP")) == NULL) {
		perror ("getprotobyname");
		exit(0);
    }
    udpproto = proto->p_proto;
	
	openlispsck = socket(PF_MAP, SOCK_RAW, 0);

	lookups[0].rx = fds[0].fd = openlispsck;
    fds[0].events = POLLIN;
    fds_idx[0] = -1;
    nfds = 1;

	/* Initialize lookups[]: all inactive */
	for (i = 0; i < MAX_LOOKUPS; i++)
        lookups[i].active = 0;
	/* add local mapping to openlisp */
	struct list_entry_t *ptr;
	struct db_node *node;
	ptr = etr_db->head.next;
	if (_fncs & (_FNC_XTR | _FNC_RTR)) {
		while (ptr != &etr_db->tail) {
			if ((node = (struct db_node *)(ptr->data))) {
				opl_update(openlispsck, node, 1);			
			}
			ptr = ptr->next;
		}
	}
	event_loop();	
  	pthread_exit(NULL);
	return 0;
}

	void
opl_errno(int oerrno)
{
	const char *err;
	if (oerrno == 0) {
		cp_log(LLOG, ": Done!\n");
	} else {
		switch (oerrno) {
		case ESRCH:
			err = "not in table";
			break;
		case EBUSY:
			err = "entry in use";
			break;
		case ENOBUFS:
			err = "not enough memory";
			break;
		case EEXIST:
			err = "map already in table";
			break;
		default:
			err = strerror(oerrno);
			break;
		}
		cp_log(LLOG, ": %s\n", err);
	}
}
	
	void *
opl_new_msg(uint16_t version, uint16_t map_type, uint32_t map_flags, uint16_t map_addrs)
{
	struct map_msghdr *mhdr;
		
	mhdr = calloc(PSIZE+sizeof(struct map_msghdr), sizeof(char));
	mhdr->map_version = version;
	mhdr->map_type =  map_type;      
	mhdr->map_flags = map_flags;
	mhdr->map_addrs = map_addrs;
	mhdr->map_pid = getpid();
	mhdr->map_versioning = 0;
	mhdr->map_errno = 0;
	mhdr->map_seq = ++seq;
	mhdr->map_msglen = sizeof(struct map_msghdr);
	return mhdr;
}

	size_t
opl_mask2sockaddr(int masklen, int af, union sockunion *rs)
{
	unsigned char buf[16];
	unsigned char *p;
	unsigned char t, mm, l;
	
	mm = (af==AF_INET)?4:16;
	if (masklen < 0 || masklen > (mm*8))
		return 0;
	
	memset(buf,0,mm);	
	memset(buf,255,masklen/8);
	if (masklen % 8 != 0) {
		p = (unsigned char *)CO(buf,(masklen / 8)) ;
		memset(p,255,1);
		t = *p;
		t = t << (8-(masklen % 8));
		*p = t;
	}
	
	switch (af) {
	case AF_INET:
		l = rs->sa.sa_len = sizeof(struct sockaddr_in);
		rs->sin.sin_family = AF_INET;
		memcpy(&rs->sin.sin_addr,buf,mm);
		return l;
	case AF_INET6:
		l = rs->sa.sa_len = sizeof(struct sockaddr_in6);
		rs->sin6.sin6_family = AF_INET6;
		memcpy(&rs->sin6.sin6_addr,buf,mm);			
		return l;
	default:
		return 0;
	}	
}
	int 
opl_sockaddr2mask(union sockunion *sk, int *rs)
{
	u_char *buf;
	int max;
	
	buf = calloc(SIN_LEN(sk->sa.sa_family),sizeof(char));
	switch (sk->sa.sa_family) {
	case AF_INET:
		memcpy(buf,(u_char *)&((sk->sin).sin_addr), SIN_LEN(sk->sa.sa_family));
		break;
	case AF_INET6:
		memcpy(buf, (u_char *)&((sk->sin6).sin6_addr), SIN_LEN(sk->sa.sa_family));
		break;
	default:
		return -1;
	}
	max = (sk->sa.sa_family == AF_INET)?32:128;
	*rs = 0;
	while (max > 0) {
		if (*buf == 255)
			*rs +=8;
		else{
			while (*buf > 0) {
				*rs +=1;
				*buf = *buf <<1;
			}
			break;
		}

		buf = (u_char *)CO(buf,1);
		max = max - 8;
	}
	return 0;
}

/* Add a mapping to database of openlisp 
	db = 1:database, db=0:cache
*/
	int 
opl_add_mapp(void *buf, struct db_node *mapp)
{
	void *mcm;
	struct map_msghdr *mhdr;
	size_t l;
	struct prefix *p;
	union sockunion *skp;
	unsigned char mm;
	
	mhdr = (struct map_msghdr *)buf;	
	mcm = CO(buf,sizeof(struct map_msghdr));
	mhdr->map_rloc_count = 0;
		
	skp = mcm;
	p = &mapp->p;
	/* Add EID */
	/* OpenLISP use fied sa_len, must be set or error will be return: invalid parameter*/
	switch (p->family) {
	case AF_INET:
		l = sizeof(struct sockaddr_in);
		skp->sa.sa_len = l;				
		skp->sin.sin_family = AF_INET;
		memcpy(&skp->sin.sin_addr,&p->u.prefix4,sizeof(struct in_addr));
		l = SS_LEN(skp);
		break;
	case AF_INET6:
		l = sizeof(struct sockaddr_in6);
		skp->sa.sa_len = l;				
		skp->sin.sin_family = AF_INET6;
		memcpy(&skp->sin6.sin6_addr,&p->u.prefix6,sizeof(struct in6_addr));
		l = SS_LEN(skp);
		break;
	default:
		return -1;
	}
	skp = mcm = CO(mcm,l);
	
	/*add EID-Mask */
	/*not include MAPA_EIDMASK if subnetmask =32(IPV4) or 128(IPV6)*/
	mm = (p->family == AF_INET)?32:128;
	if ((mapp->p.prefixlen > 0) && (mapp->p.prefixlen  < mm) ) {
		if ((l = opl_mask2sockaddr(p->prefixlen, p->family, (union sockunion *)mcm)) <=0) {
			cp_log(LLOG, "subnetmask not correct\n");
			return -1;
		}
		l = SS_LEN(skp);
		mcm = CO(mcm,l);		
		mhdr->map_addrs |= MAPA_EIDMASK;
	}
	
	mhdr->map_msglen = (char *)mcm - (char *)mhdr;	
	return mhdr->map_msglen;
}

	int 
opl_add_rloc(void *buf, struct db_node *mapp)
{
	void *mcm;
	struct map_msghdr *mhdr;
	int lcount,l;
	struct list_t *ll;
	struct list_entry_t *rl_entry;
	struct map_entry *rl;
	struct rloc_mtx *mx;
	
	union sockunion *skp;	
	
	/* y5er */
	int sl_count; 					// source locator count for each mapping entry
	struct list_t *lls;  			// list of source locator
	struct list_entry_t *sl_entry; 	// each source locator in the list as a list entry
	struct source_locator *s_loc; 	// source locator
	struct rloc_mtx *mxx; 			// same role as mx
	union sockunion *skpp; 			// same role as skp
	/* y5er */

	if (!(ll = (struct list_t *)mapp->info) || (ll->count <= 0))
		return 0;
	
	lcount = ll->count;
	
	mhdr = (struct map_msghdr *)buf;	
	mhdr->map_rloc_count = 0;
	mcm = CO(buf,mhdr->map_msglen);
	
	rl_entry = ll->head.next;
	while (rl_entry != &ll->tail) {
		/* add rloc */
		rl = (struct map_entry *)rl_entry->data;
		skp = mcm;
		l = SA_LEN(rl->rloc.sa.sa_family);
		switch (rl->rloc.sa.sa_family) {
		case AF_INET:
			memcpy(&skp->sin,&rl->rloc.sin,l);
			break;
		case AF_INET6:
			memcpy(&skp->sin6,&rl->rloc.sin6,l);					
			break;
		default:
			return -1;
		}	
		skp->sa.sa_len = l;
		l = SS_LEN(skp);
		
				
		/* add rloc property */
		mx = (struct rloc_mtx *)CO(mcm,l);			
		mx->priority = rl->priority;
		mx->weight = rl->weight;
		mx->flags |= rl->L?RLOCF_LIF:0;
		mx->flags |= rl->r?RLOCF_UP:0;

		/* y5er */
		mx->src_loc_count = 0 ;

		mcm = CO(mx,sizeof(struct rloc_mtx));
		mhdr->map_rloc_count +=1;

		/* y5er */
		if ( rl->src_loc_count )
		{
			// TODO enable these flag again
			// also add validation at data plane
			// mx->flags |= rl->src_loc_count?RLOCF_HAVE_SRC:0;
			mx->flags |= RLOCF_HAVE_SRC;
			mx->src_loc_count = rl->src_loc_count;
			// we should update this latter after adding all the source locator to the message
			// to ensure that only correctly added src locator is counted

			// add source rloc and source rloc property here
			// first is to get the list of source locator of that rl
			sl_count = 0; // currently it is not used

			lls = (struct list_t *)rl->src_loc;
			// Do we need to check for the availability of the list ?
			// Or just need to the check via src_loc_count ?
			// we can also compare the lls->count and rl->src_loc_count
			// it shoud be the same
			// cp_log(LLOG, "number of src locator %d %d \n",  lls->count,  rl->src_loc_count);

			sl_entry = lls->head.next;
			while (sl_entry != &lls->tail) {
				// add source locator
				s_loc = (struct source_locator *)sl_entry->data;
				skpp = mcm;
				l = SA_LEN(s_loc->addr.sa.sa_family);
				switch (s_loc->addr.sa.sa_family) {
				case AF_INET:
					memcpy(&skpp->sin,&s_loc->addr.sin,l);
					break;
				case AF_INET6:
					memcpy(&skpp->sin6,&s_loc->addr.sin6,l);
					break;
				default:
					return -1;
				}
				skpp->sa.sa_len = l;
				l = SS_LEN(skpp);
				// add source locator property
				mxx = (struct rloc_mtx *)CO(mcm,l);
				mxx->priority = s_loc->priority;
				mxx->weight = s_loc->weight;
				mxx->flags |= 0;
				mxx->flags |= 0;
				mxx->flags |= RLOCF_SRC_LOC;
				mxx->src_loc_count = 0;

				// update mcm
				mcm = CO(mxx,sizeof(struct rloc_mtx));
				sl_count++;
				sl_entry = sl_entry->next;
				// cp_log(LLOG, " source locator added \n");
				// cp_log(LLOG, " message length %d \n",(char *)mcm - (char *)mhdr);
			}
			// not increase the map_rloc_count and update the mcm
			// remember to keep the mcm update after a new field added
		}
		/* end y5er */
		rl_entry = rl_entry->next;
	}	

	mhdr->map_msglen = (char *)mcm - (char *)mhdr;
	cp_log(LLOG, " message length %d \n",mhdr->map_msglen );
	return mhdr->map_msglen;
}

	int 
opl_add(int s, struct db_node *mapp, int db)
{
	void *buf;
	ssize_t l;
	int lcount;
	struct list_t *ll;
	int map_neg;
	
	if (!mapp->info) {
		lcount = 0;
	}else{
		ll = (struct list_t *)mapp->info;
		lcount = ll->count;
	}
	map_neg = 0;
	if (lcount == 0 && _petr == NULL)
		map_neg = MAPF_NEGATIVE;
		
	buf = opl_new_msg(MAPM_VERSION, \
						MAPM_ADD,\
						((db == 1? MAPF_DB: 0) | MAPF_STATIC | MAPF_UP) | map_neg,\
                                                MAPA_EID | (map_neg? 0 : MAPA_RLOC));

	if ((l = opl_add_mapp(buf, mapp)) <= 0)
		return -1;
	if (lcount > 0 && (l=opl_add_rloc(buf, mapp))<= 0)
		return -1;
	if (lcount == 0 && _petr && (l = opl_add_rloc(buf, _petr)) <=0)
		return -1;
	cp_log(LLOG, "add %s %s", (db ==1 ? "database":"cache"), (char *)prefix2str(&mapp->p));	
	

	/*send to openlisp database */
	errno = 0;
	if ((l = write(s, (char *)buf, l)) < 0 ) {
		if (_debug == LLOG || _debug == LDEBUG)
			opl_errno(errno);
		return -1;
	}
	
	if (_debug == LLOG || _debug == LDEBUG)
		opl_errno(0);
	return l;
}

/* Delete a mapping from Openlisp database
	db: for future
*/
	int 
opl_del(int s, struct db_node *mapp, int db)
{
	
	void *buf;
	ssize_t l;
		
	buf = opl_new_msg(MAPM_VERSION, \
						MAPM_DELETE,\
						(db == 1? MAPF_DB: MAPF_ALL) | MAPF_STATIC | MAPF_UP,\
						MAPA_EID);
	
	if ((l=opl_add_mapp(buf, mapp)) < 0)
		return -1;
		
	cp_log(LLOG, "delete  %s", (char *)prefix2str(&mapp->p));	
	
	/*send to openlisp database */
	errno = 0;
	/*send to openlisp database */
	if ((l = write(s, (char *)buf, l)) < 0) {
		if (_debug == LLOG || _debug == LDEBUG)
			opl_errno(errno);
		return -1;
	}
		
	if (_debug == LLOG || _debug == LDEBUG)
		opl_errno(0);	
	
	return l;
}

/* Find a mapping from Openlisp database */
	int 
opl_get(int s, struct db_node *mapp, int db, struct db_node *rs)
{
	void *buf;
	struct map_msghdr *mhdr;
	void *mmc;
	union sockunion *rc;
		
	ssize_t l;
	int c_seq, c;
	pid_t   c_pid;  
	
	buf = opl_new_msg(MAPM_VERSION, \
						MAPM_GET,\
						(db?MAPF_DB:MAPF_ALL) | MAPF_STATIC | MAPF_UP,\
						MAPA_EID);
	
	if ((l=opl_add_mapp(buf, mapp)) < 0)
		return -1;
	mhdr = (struct map_msghdr *)buf;	
	c_seq =	mhdr->map_seq;
	c_pid = mhdr->map_pid;
	
	/*send to openlisp database */
	errno = 0;	
	if ((l = write(s, (char *)buf,l)) < 0) {
		if (_debug == LLOG || _debug == LDEBUG)
			opl_errno(errno);		
		return -1;
	}
	
	/*wait for return from mapping soket*/
	/* need timeout detection here ??? */
	c = 0;
	do {
		l = read(s, (char *)buf, PSIZE);
		mhdr = (struct map_msghdr *)buf;	
	} while (l > 0 && (mhdr->map_seq != c_seq || mhdr->map_pid != c_pid) && ++c < 10);
	
	/* get result */
	if (l < 0 || c >= 10) {
		return -1;
	}
	
	mhdr = (struct map_msghdr *)buf;	
	mmc = CO(mhdr, sizeof(struct map_msghdr));
	
	/* check eid prefix */
	if ((mhdr->map_flags & MAPF_DONE) <=0) {
		return -1;
	}

	/*get EID */ 
	if ((mhdr->map_addrs & MAPA_EID) <= 0 ) {
		return -1 ;
	}
	rc = mmc;
	if ((l = sockaddr2prefix(rc, &rs->p))<=0) {
		return -1 ;
	}
	
	l = SS_LEN(rc);
	rc = (union sockunion *)CO(rc,l);
	
	//get EID-masklen if exist
	if ((mhdr->map_addrs & MAPA_EIDMASK ) > 0) {
		opl_sockaddr2mask(rc, (int *)&(rs->p.prefixlen));
		l = SS_LEN(rc);
		rc = (union sockunion *)CO(rc,l);
	}else
		rs->p.prefixlen = ((rc->sa).sa_family == AF_INET)?32:128;
	
	/*get Rloc if exist */
	if ((mhdr->map_addrs & MAPA_RLOC ) > 0) {
		int i;
		struct list_t *rl;
		struct map_entry *re;
		struct rloc_mtx *mx;
		
		rs->info = rl = list_init();
		for (i = 0; i < mhdr->map_rloc_count ; i++) {
			re = calloc(1,sizeof(struct map_entry));
			l = SS_LEN(rc);
			memcpy(&re->rloc, rc, l);
			rc = (union sockunion *)CO(rc,l);
			mx = (struct rloc_mtx *)rc;
			re->priority = mx->priority;
			re->weight = mx->weight;
			re->r = (mx->flags & RLOCF_UP)>0?1:0;
			re->L = (mx->flags & RLOCF_LIF)>0?1:0;
			list_insert(rl,re, NULL);
						
			rc = (union sockunion *)CO(rc,sizeof(struct rloc_mtx));			
		}
		
	}
	return 0;
}

/* update db */
	int 
opl_update(int s, struct db_node *node, uint8_t db)
{	
	opl_del(s,node,db);
	opl_add(s,node,db);
	return 0;
}
#endif
