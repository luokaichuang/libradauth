#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <netdb.h>

#include <libradius.h>
#include <radpaths.h>
#include <conf.h>

#include "libradauth.h"

#define strlcpy(A,B,C) strncpy(A,B,C), *(A+(C)-1)='\0'
#define BUFSIZE 1024

#define LIBNAME "[libradauth] "
#ifdef DEBUG
#define debug(fmt, ...) \
        fprintf(stderr, LIBNAME fmt, ##__VA_ARGS__)
#define debug_fr_error(what) debug(what ": ERROR: %s\n", fr_strerror())
#else
#define debug(fmt, ...)
#define debug_fr_error(what)
#endif

struct rad_server;
struct rad_server {
	char name[64];
	char host[64];
	int port;
	int priority;
	int timeout;
	char bind[64];
	char secret[64];
	enum { NONE, PAP, CHAP } method;
	struct rad_server *next;
};

static struct rad_server *parse_one_server(char *buf);
static struct rad_server *parse_servers(const char *config);
static void server_add_field(struct rad_server *s,
	const char *k, const char *v);
static int query_one_server(const char *username, const char *password,
	struct rad_server *server);
static struct rad_server *sort_servers(struct rad_server *list, int try);
static int cmp_prio_rand (const void *, const void *);
static void free_server_list(struct rad_server *head);
static int ipaddr_from_server(struct in_addr *addr, const char *host);

static struct rad_server *parse_servers(const char *config)
{
	FILE *fp;
	char buf[BUFSIZE];
	int n = 0;
	char *stm;
	char *brace;
	int s_len, s_size;
	int b_len = 0;
	struct rad_server *tmp, *cur, *head;

	head = NULL;

	if((fp = fopen(config, "r")) == NULL) {
		debug("Failed to open config file '%s'!\n", config);
		return NULL;
	}

	stm = malloc(BUFSIZE * sizeof(char));
	if(stm == 0)
		return NULL;
	s_size = BUFSIZE;

	*stm = '\0';
	s_len = 0;
	while(fgets(buf, BUFSIZE-1, fp) != NULL) {
		n++;
		b_len = strlen(buf);

		/* skip comments and empty lines */
		if(buf[0] == '\n' || buf[0] == '#')
			continue;

		if(s_len + b_len + 1 > s_size) {
			debug("Resizing buffer to make the statement fit\n");
			s_size += BUFSIZE;
			stm = realloc(stm, s_size);
			if(!stm)
				return NULL;
		}

		brace = strrchr(buf, '}');
		if(brace && (brace == buf ||
				*(brace-1) == ' ' && *(brace-1) == '\t')) {
			*(brace+1) = '\0'; /* statement terminated */
			strncat(stm, buf, b_len);
			s_len += buf - brace + 1;
		} else {
			strncat(stm, buf, b_len);
			s_len += b_len;
			continue; /* next line! */
		}

		tmp = parse_one_server(stm);
		s_len = 0;
		*stm = '\0';

		if(!tmp)
			continue;

		if(!head)
			cur = head = tmp;
		else {
			cur->next = tmp;
			cur = tmp;
		}
		debug("successfully added server '%s': %s:%d "
			"(prio %d, timeout %dms, method %s)\n",
			cur->name, cur->host, cur->port, cur->priority,
			cur->timeout, cur->method == CHAP ? "CHAP" : "PAP");
	}

	if(s_len > 0) {
		debug("reached EOF, could not find closing '}'!\n");
		debug("    (statement will be ignored)\n");
	}

	free(stm);
	fclose(fp);

	return head;
}

static struct rad_server *sort_servers(struct rad_server *list, int try)
{
	int i, n;
	struct rad_server *head, *tmp;
	struct rad_server **servers;

	for(n = 0, tmp = list; tmp; tmp = tmp->next)
		n++;

	servers = malloc(n * sizeof(struct rad_server *));
	if(!servers)
		return NULL;

	for(i = 0, tmp = list; i < n; i++, tmp = tmp->next)
		servers[i] = tmp;

	srand(time(NULL) + 0xF00 * try);
	qsort(servers, n, sizeof(struct rad_server *), cmp_prio_rand);

	/* reconstruct the list */
	head = servers[0];
	for(i = 1, tmp = head; i < n; i++, tmp = tmp->next)
		tmp->next = servers[i];
	tmp->next = NULL; /* last entry */

	/* debugging */
	tmp = head;
	debug("Servers will be tried in this order: \n");
	debug("   prio name = host:port\n");
	do {
		debug("%7d %s = %s:%d\n", tmp->priority,
			tmp->name, tmp->host, tmp->port);
	} while ((tmp = tmp->next));

	free(servers);

	return head;
}

static int cmp_prio_rand (const void *a, const void *b)
{
	struct rad_server *r, *s;
	r = * (struct rad_server * const *) a;
	s = * (struct rad_server * const *) b;

	if(r->priority < s->priority)
		return 1;
	if(r->priority > s->priority)
		return -1;
	/* same priority - pick a random one :-) */
	return (rand() % 2 ? -1 : 1);
}

static struct rad_server *parse_one_server(char *buf)
{
	struct rad_server *s;
	const char delim[4] = " \t\n";
	char *t, *v;
	char token[64];

	s = malloc(sizeof(*s));
	if(!s) {
		free(s);
		return NULL;
	}

	if(!(t = strtok(buf, delim))) {
		free(s);
		return NULL;
	}
	strlcpy(s->name, t, 64);

	/* fill with defaults */
	*(s->host) = '\0';
	*(s->secret) = '\0';
	s->port = 1812;
	s->priority = 0;
	s->timeout = 1000; /* 1 second */
	s->method = NONE;
	strcpy(s->bind, "0.0.0.0");

	if(!(t = strtok(NULL, delim)) || *t != '{') {
		debug("could not find '{' after statement name!\n");
		free(s);
		return NULL;
	}

	while((t = strtok(NULL, delim)) && *t != '}') {
		strlcpy(token, t, 64);
		v = strtok(NULL, delim);
		server_add_field(s, token, v);
	}

	if(!*(s->host) || s->method == NONE) {
		debug("%s: error in format: at least 'host' "
			"or 'method' are missing!\n", s->name);
		free(s);
		return NULL;
	}

	s->next = NULL;
	return s;
}

static void server_add_field(struct rad_server *s, const char *k, const char *v)
{
	if(!strcmp(k, "host"))
		strlcpy(s->host, v, sizeof(s->host));
	else if(!strcmp(k, "bind"))
		strlcpy(s->bind, v, sizeof(s->bind));
	else if(!strcmp(k, "secret"))
		strlcpy(s->secret, v, sizeof(s->secret));
	else if(!strcmp(k, "port"))
		s->port = atoi(v);
	else if(!strcmp(k, "priority"))
		s->priority = atoi(v);
	else if(!strcmp(k, "timeout"))
		s->timeout = atoi(v);
	else if(!strcmp(k, "method")) {
		if(!strcasecmp(v, "CHAP"))
			s->method = CHAP;
		else if(!strcasecmp(v, "PAP"))
			s->method = PAP;
	} else {
		debug("%s: wrong or unknown key: %s = %s\n", s->name, k, v);
	}
}

static void free_server_list(struct rad_server *head)
{
	struct rad_server *cur, *tmp;
	cur = head;
	do {
		tmp = cur->next;
		free(cur);
		cur = tmp;
	} while(cur);
}

static int ipaddr_from_server(struct in_addr *addr, const char *host)
{
	struct hostent *res;

	if(!(res = gethostbyname(host))) {
		debug("  -> Failed to resolve host '%s'.\n", host);
		return 0;
	}

	addr->s_addr = ((struct in_addr*) res->h_addr_list[0])->s_addr;
	debug("  -> resolved '%s' to '%s'\n", host, inet_ntoa(*addr));
	return 1;
}

static int query_one_server(const char *username, const char *password,
		struct rad_server *server)
{
	struct timeval tv;
	volatile int max_fd;
	fd_set set;
	struct sockaddr_in src;
	int src_size = sizeof(src);
	int rc = -1;

	fr_ipaddr_t client;
	fr_packet_list_t *pl = 0;
	VALUE_PAIR *vp;
	RADIUS_PACKET *request = 0, *reply = 0;

	request = rad_alloc(1);
	if(!request) {
		debug_fr_error("rad_alloc");
		rc = -1;
		goto done;
	}

	request->code = PW_AUTHENTICATION_REQUEST;

	request->dst_ipaddr.af = AF_INET;
	request->dst_port = server->port;
	if(!ipaddr_from_server(
		&(request->dst_ipaddr.ipaddr.ip4addr), server->host))
		goto done;

	memset(&client, 0, sizeof(client));
	client.af = AF_INET;
	if(!ipaddr_from_server(&(client.ipaddr.ip4addr), server->bind))
		goto done;

	/* int sockfd = fr_socket(&request->dst_ipaddr, 0); */
	int sockfd = fr_socket(&client, 0);
	if(!sockfd) {
		debug_fr_error("fr_socket");
		rc = -1;
		goto done;
	}
	/* request->sockfd = sockfd; */
	request->sockfd = -1;
	request->code = PW_AUTHENTICATION_REQUEST;

	pl = fr_packet_list_create(1);
	if(!pl) {
		debug_fr_error("fr_packet_list_create");
		rc = -1;
		goto done;
	}

	if(!fr_packet_list_socket_add(pl, sockfd)) {
		debug_fr_error("fr_packet_list_socket_add");
		rc = -1;
		goto done;
	}

	if(fr_packet_list_id_alloc(pl, request) < 0) {
		debug_fr_error("fr_packet_list_id_alloc");
		rc = -1;
		goto done;
	}

	/* construct value pairs */
	vp = pairmake("User-Name", username, 0);
	pairadd(&request->vps, vp);

	if(server->method == PAP) {
		debug("  -> Using PAP-scrambled passwords\n");
		/* encryption of the packet will happen *automatically* just
		 * before sending the packet via make_passwd() */
		vp = pairmake("User-Password", password, 0);
		vp->flags.encrypt = FLAG_ENCRYPT_USER_PASSWORD;
	} else if(server->method == CHAP) {
		debug("  -> Using CHAP-scrambled passwords\n");
		vp = pairmake("CHAP-Password", password, 0);
		strlcpy(vp->vp_strvalue, password,
			sizeof(vp->vp_strvalue));
		vp->length = strlen(vp->vp_strvalue);
		rad_chap_encode(request, vp->vp_octets, request->id, vp);
		vp->length = 17;
	}
	pairadd(&request->vps, vp); /* the password */

	memset(&src, 0, sizeof(src));
	if(getsockname(sockfd, (struct sockaddr *) &src, &src_size) < 0) {
		rc = -1;
		goto done;
	}
	debug("  -> Sending packet via %s:%d...\n",
		inet_ntoa(src.sin_addr), ntohs(src.sin_port));

	if(rad_send(request, NULL, server->secret) == -1) {
		debug_fr_error("rad_send");
		rc = -1;
		goto done;
	}

	/* GESENDET! :-) */

	/* And wait for reply, timing out as necessary */
	FD_ZERO(&set);

	max_fd = fr_packet_list_fd_set(pl, &set);
	if (max_fd < 0) {
		rc = -1;
		goto done;
	}

	/* wait a configured time (default: 1.0s) */
	tv.tv_sec  = server->timeout / 1000;
	tv.tv_usec = 1000 * (server->timeout % 1000);

	if (select(max_fd, &set, NULL, NULL, &tv) <= 0) {
		debug("  -> ERROR: no packet received!\n");
		rc = -1;
		goto done;
	}

	reply = fr_packet_list_recv(pl, &set);
	if (!reply) {
		debug("  -> received bad packet: %s\n", fr_strerror());
		rc = -1;
		goto done;
	}

	if (rad_verify(reply, request, server->secret) < 0) {
		debug_fr_error("rad_verify");
		rc = -1;
		goto done;
	}

	fr_packet_list_yank(pl, request);

	if (rad_decode(reply, request, server->secret) != 0) {
		debug_fr_error("rad_decode");
		rc = -1;
		goto done;
	}

	if(reply->code == PW_AUTHENTICATION_ACK) {
		debug("  -> ACK: Authentication was successful.\n");
		rc = 0;
	}

	if(reply->code == PW_AUTHENTICATION_REJECT) {
		rc = 1;
		debug("  -> REJECT: Authentication was not successful.\n");
	}

	done:
	if(request)
		rad_free(&request);
	if(reply)
		rad_free(&reply);
	if(pl)
		fr_packet_list_free(pl);

	return rc;
}

int rad_auth(const char *username, const char *password,
		int retries, const char *config)
{
	struct rad_server *serverlist, *server;
	int rc = -1;
	int try;

	debug("initiating dictionary '%s'...\n", RADIUS_DICTIONARY);
	if (dict_init(".", RADIUS_DICTIONARY) < 0) {
		debug_fr_error("dict_init");
		rc = -1;
		goto done;
	}

	debug("parsing servers from config file '%s'\n", config);
	serverlist = parse_servers(config);
	if(!serverlist) {
		debug("Could not parse servers, bailing!\n");
		rc = -1;
		goto done;
	}

	for(try = 1; try <= retries; try++) {
		debug("ATTEMPT #%d of %d\n", try, retries);
		server = serverlist = sort_servers(serverlist, try);
		do {
			debug("Querying server: %s:%d\n", server->name, server->port);
			rc = query_one_server(username, password, server);
			if(rc != -1)
				goto done;
		} while((server = server->next) != NULL);
		debug("FAILED to reach any of the servers after %d tries. "
			"Giving up.\n", try);
	}

	done:
	if(serverlist)
		free_server_list(serverlist);

	return rc;
}

/* vim:set noet sw=8 ts=8: */
