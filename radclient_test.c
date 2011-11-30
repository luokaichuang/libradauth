#include <stdio.h>
#include <stdlib.h>
#include <netdb.h>

#include <libradius.h>
#include <radpaths.h>
#include <conf.h>

#define strlcpy(A,B,C) strncpy(A,B,C), *(A+(C)-1)='\0'

#define RADIUS_SECRET "Foobarbaz"

static int getport(const char *name)
{
	struct servent *svp;

	svp = getservbyname (name, "udp");
	if (!svp)
		return 0;

	return ntohs(svp->s_port);
}

int main(int argc, char *argv[])
{
	RADIUS_PACKET *request;
	VALUE_PAIR *vp;
	fr_packet_list_t *pl;
	fr_ipaddr_t client;

	char username[32], password[32];

	int rc = -1;

	fprintf(stdout, "<user> <pw>: ");
	fscanf(stdin, "%s %s", &username, &password);

	/* if (dict_init(RADDBDIR, RADIUS_DICTIONARY) < 0) { */
	if (dict_init(".", RADIUS_DICTIONARY) < 0) {
		fr_perror("dict_init");
		rc = -1;
		goto done;
	}

	request = rad_alloc(1);
	if(!request) {
		fr_perror("foo");
		rc = -1;
		goto done;
	}

	request->code = PW_AUTHENTICATION_REQUEST;

	request->dst_ipaddr.af = AF_INET;
	request->dst_ipaddr.ipaddr.ip4addr.s_addr = inet_addr("127.0.0.1");
	request->dst_port = getport("radius");

	/* bind to 0.0.0.0 */
	memset(&client, 0, sizeof(client));
	client.af = AF_INET;
	client.ipaddr.ip4addr.s_addr = inet_addr("0.0.0.0");

	/* int sockfd = fr_socket(&request->dst_ipaddr, 0); */
	int sockfd = fr_socket(&client, 0);
	if(!sockfd) {
		fr_perror("fr_socket");
		rc = -1;
		goto done;
	}
	/* request->sockfd = sockfd; */
	request->sockfd = -1;
	request->code = PW_AUTHENTICATION_REQUEST;

	pl = fr_packet_list_create(1);
	if(!pl) {
		fr_perror("fr_packet_list_create");
		rc = -1;
		goto done;
	}

	if(!fr_packet_list_socket_add(pl, sockfd)) {
		fr_perror("fr_packet_list_socket_add");
		rc = -1;
		goto done;
	}

	/* construct value pairs */
	vp = pairmake("User-Name", username, 0);
	pairadd(&request->vps, vp);
	vp = pairmake("CHAP-Password", password, 0);
	pairadd(&request->vps, vp);
	/* request->vps = readvp2(stdin, &done, "readvp2"); */

	if(fr_packet_list_id_alloc(pl, request) < 0) {
		fr_perror("fr_packet_list_id_alloc");
		rc = -1;
		goto done;
	}

	if ((vp = pairfind(request->vps, PW_CHAP_PASSWORD)) != NULL) {
		strlcpy(vp->vp_strvalue, password,
			sizeof(vp->vp_strvalue));
		vp->length = strlen(vp->vp_strvalue);

		rad_chap_encode(request, vp->vp_octets, request->id, vp);
		vp->length = 17;
	}

	if(rad_send(request, NULL, RADIUS_SECRET) == -1) {
		fr_perror("rad_send");
		rc = -1;
		goto done;
	}

	/* GESENDET! :-) */

	fd_set		set;
	struct timeval  tv;
	RADIUS_PACKET	*reply, **request_p;
	volatile int max_fd;

	/* And wait for reply, timing out as necessary */
	FD_ZERO(&set);

	max_fd = fr_packet_list_fd_set(pl, &set);
	if (max_fd < 0) {
		rc = -1;
		goto done;
	}

	tv.tv_sec = 3;

	if (select(max_fd, &set, NULL, NULL, &tv) <= 0) {
		fprintf(stderr, "no packet received!\n");
		rc = -1;
		goto done;
	}

	reply = fr_packet_list_recv(pl, &set);
	if (!reply) {
		fprintf(stderr, "radclient: received bad packet: %s\n",
				fr_strerror());
		rc = -1;	/* bad packet */
		goto done;
	}

	if (rad_verify(reply, request, RADIUS_SECRET) < 0) {
		fr_perror("rad_verify");
		rc = -1;
		goto done;
	}

	fr_packet_list_yank(pl, request);

	if (rad_decode(reply, request, RADIUS_SECRET) != 0) {
		fr_perror("rad_decode");
		rc = -1;
		goto done;
	}

	if(reply->code == PW_AUTHENTICATION_ACK)
		rc = 0;

	if(reply->code == PW_AUTHENTICATION_REJECT)
		rc = 1;

	done:
	rad_free(&request);
	rad_free(&reply);

	return rc;
}

/* vim:set noet sw=8 ts=8: */
