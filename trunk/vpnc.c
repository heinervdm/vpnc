/* IPSec VPN client compatible with Cisco equipment.
   Copyright (C) 2002, 2003  Geoffrey Keating and Maurice Massar

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include <sys/fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <poll.h>
#include <net/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>

#include <gcrypt.h>

#include "isakmp-pkt.h"
#include "tun_dev.h"
#include "math_group.h"
#include "dh.h"
#include "vpnc.h"

extern void vpnc_doit(unsigned long tous_spi,
		      const unsigned char *tous_key, 
		      struct sockaddr_in *tous_dest,
		      unsigned long tothem_spi,
		      const unsigned char *tothem_key,
		      struct sockaddr_in *tothem_dest,
		      int tun_fd, int md_algo, int cry_algo,
		      uint8_t *kill_packet_p, size_t kill_packet_size_p,
		      struct sockaddr *kill_dest_p,
		      const char *pidfile);

enum config_enum {
  CONFIG_DEBUG,
  CONFIG_ND,
  CONFIG_NON_INTERACTIVE,
  CONFIG_PID_FILE,
  CONFIG_LOCAL_PORT,
  CONFIG_IF_NAME,
  CONFIG_IKE_DH,
  CONFIG_IPSEC_PFS,
  CONFIG_IPSEC_GATEWAY,
  CONFIG_IPSEC_ID,
  CONFIG_IPSEC_SECRET,
  CONFIG_XAUTH_USERNAME,
  CONFIG_XAUTH_PASSWORD,
  LAST_CONFIG
};

static const char *config[LAST_CONFIG];

int opt_debug = 0;
int opt_nd;

enum supp_algo_key {
	SUPP_ALGO_NAME,
	SUPP_ALGO_MY_ID,
	SUPP_ALGO_IKE_SA,
	SUPP_ALGO_IPSEC_SA
};

enum algo_group {
	SUPP_ALGO_DH_GROUP,
	SUPP_ALGO_HASH,
	SUPP_ALGO_CRYPT
};

typedef struct {
	const char *name;
	int my_id, ike_sa_id, ipsec_sa_id;
	int keylen;
} supported_algo_t;

supported_algo_t supp_dh_group[] = {
	{ "nopfs", 0, 0, 0, 0 },
	{ "dh1", OAKLEY_GRP_1, IKE_GROUP_MODP_768, IKE_GROUP_MODP_768, 0 },
	{ "dh2", OAKLEY_GRP_2, IKE_GROUP_MODP_1024, IKE_GROUP_MODP_1024, 0 },
	{ "dh5", OAKLEY_GRP_5, IKE_GROUP_MODP_1536, IKE_GROUP_MODP_1536, 0 },
	/*{ "dh7", OAKLEY_GRP_7, IKE_GROUP_EC2N_163K, IKE_GROUP_EC2N_163K, 0 }*/
};

supported_algo_t supp_hash[] = {
	{ "md5", GCRY_MD_MD5, IKE_HASH_MD5, IPSEC_AUTH_HMAC_MD5, 0 },
	{ "sha1", GCRY_MD_SHA1, IKE_HASH_SHA, IPSEC_AUTH_HMAC_SHA, 0 }
};

supported_algo_t supp_crypt[] = {
	{ "3des", GCRY_CIPHER_3DES, IKE_ENC_3DES_CBC, ISAKMP_IPSEC_ESP_3DES, 0 },
	{ "aes128", GCRY_CIPHER_AES128, IKE_ENC_AES_CBC, ISAKMP_IPSEC_ESP_AES, 128 },
	{ "aes192", GCRY_CIPHER_AES192, IKE_ENC_AES_CBC, ISAKMP_IPSEC_ESP_AES, 192 },
	{ "aes256", GCRY_CIPHER_AES256, IKE_ENC_AES_CBC, ISAKMP_IPSEC_ESP_AES, 256 },
};

const supported_algo_t *
get_algo(enum algo_group what, enum supp_algo_key key, int id, const char *name, int keylen)
{
	supported_algo_t *sa = NULL;
	int i = 0, cnt = 0, val = 0;
	const char *valname = NULL;
	
	assert(what <= SUPP_ALGO_CRYPT);
	assert(key <= SUPP_ALGO_IPSEC_SA);
	
	switch (what) {
		case SUPP_ALGO_DH_GROUP:
			sa = supp_dh_group;
			cnt = sizeof(supp_dh_group) / sizeof(supp_dh_group[0]);
			break;
		case SUPP_ALGO_HASH:
			sa = supp_hash;
			cnt = sizeof(supp_hash) / sizeof(supp_hash[0]);
			break;
		case SUPP_ALGO_CRYPT:
			sa = supp_crypt;
			cnt = sizeof(supp_crypt) / sizeof(supp_crypt[0]);
			break;
	}
	
	for (i = 0; i < cnt; i++) {
		switch (key) {
			case SUPP_ALGO_NAME:
				valname = sa[i].name;
				break;
			case SUPP_ALGO_MY_ID:
				val = sa[i].my_id;
				break;
			case SUPP_ALGO_IKE_SA:
				val = sa[i].ike_sa_id;
				break;
			case SUPP_ALGO_IPSEC_SA:
				val = sa[i].ipsec_sa_id;
				break;
		}
		if ((key == SUPP_ALGO_NAME) ?
			!strcasecmp(name, valname) :
			(val == id))
			if (keylen == sa[i].keylen)
				return sa + i;
	}
	
	return NULL;
}

const supported_algo_t *get_dh_group_ike(void)
{
	return get_algo(SUPP_ALGO_DH_GROUP, SUPP_ALGO_NAME, 0, config[CONFIG_IKE_DH], 0);
}
const supported_algo_t *get_dh_group_ipsec(void)
{
	return get_algo(SUPP_ALGO_DH_GROUP, SUPP_ALGO_NAME, 0, config[CONFIG_IPSEC_PFS], 0);
}

/* * */

static __inline__ int min(int a, int b)
{
	return (a < b) ? a : b;
}

void hex_dump (const char *str, const void *data, size_t len)
{
	size_t i;
	const uint8_t *p = data;
	
  if(opt_debug >= 3) {
       	printf("%s:%c", str, (len <= 32)? ' ':'\n');
       	for (i = 0; i < len; i++) {
       		if (i && !(i%32))
       			printf("\n");
       		else if (i && !(i%4))
       			printf(" ");
       		printf("%02x", p[i]);
       	}
       	printf("\n");
  }
}

static int
make_socket (uint16_t port)
{
  int sock;
  struct sockaddr_in name;

  /* Create the socket. */
  sock = socket (PF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    error (1, errno, "making socket");

  /* Give the socket a name. */
  name.sin_family = AF_INET;
  name.sin_port = htons (port);
  name.sin_addr.s_addr = htonl (INADDR_ANY);
  if (bind (sock, (struct sockaddr *) &name, sizeof (name)) < 0)
    error (1, errno, "binding to port %d", port);

  return sock;
}

static struct sockaddr *
init_sockaddr (const char *hostname,
	       uint16_t port)
{
  struct hostent *hostinfo;
  struct sockaddr_in *result;
  
  result = malloc (sizeof (struct sockaddr_in));
  if (result == NULL)
    error (1, errno, "out of memory");

  result->sin_family = AF_INET;
  result->sin_port = htons (port);
  if (inet_aton (hostname, &result->sin_addr) == 0)
    {
      hostinfo = gethostbyname (hostname);
      if (hostinfo == NULL)
	error (1, 0, "unknown host `%s'\n", hostname);
      result->sin_addr = *(struct in_addr *) hostinfo->h_addr;
    }
  return (struct sockaddr *)result;
}

static int tun_fd = -1;
static char tun_name[IFNAMSIZ];

static void
setup_tunnel(void)
{
  if (config[CONFIG_IF_NAME]) 
	  memcpy(tun_name, config[CONFIG_IF_NAME], strlen(config[CONFIG_IF_NAME]));
  
  tun_fd = tun_open(tun_name);

  if (tun_fd == -1)
    error (1, errno, "can't initialise tunnel interface");
}

static int sockfd = -1;
static struct sockaddr *dest_addr;
static int timeout = 5000;  /* 5 seconds */
static uint8_t *resend_hash = NULL;

static int
recv_ignore_dup (void *recvbuf, size_t recvbufsize, uint8_t reply_extype)
{
  uint8_t *resend_check_hash;
  int recvsize, hash_len;
  struct sockaddr_in recvaddr;
  socklen_t recvaddr_size = sizeof (recvaddr);
  char ntop_buf[32];

  recvsize = recvfrom (sockfd, recvbuf, recvbufsize, 0, &recvaddr,
		       &recvaddr_size);
  if (recvsize == -1)
    error (1, errno, "receiving packet");
  if (recvsize > 0)
    {
      if (recvaddr_size != sizeof (recvaddr)
	  || recvaddr.sin_family != dest_addr->sa_family
	  || recvaddr.sin_port != ((struct sockaddr_in *)dest_addr)->sin_port
	  || memcmp (&recvaddr.sin_addr, 
		     &((struct sockaddr_in *)dest_addr)->sin_addr, 
		     sizeof (struct in_addr)) != 0)
	{
	  error (0, 0, "got response from unknown host %s:%d",
		 inet_ntop (recvaddr.sin_family, &recvaddr.sin_addr,
			    ntop_buf, sizeof (ntop_buf)),
		 ntohs (recvaddr.sin_port));
	  return -1;
	}
      
hex_dump("exchange_type", ((uint8_t*)recvbuf) + ISAKMP_EXCHANGE_TYPE_O, 1);
      if (reply_extype && (((uint8_t*)recvbuf)[ISAKMP_EXCHANGE_TYPE_O] != reply_extype)) {
DEBUG(2, printf("want extype %d, got %d, ignoring\n", reply_extype, ((uint8_t*)recvbuf)[ISAKMP_EXCHANGE_TYPE_O]));
	return -1;
      }

      hash_len = gcry_md_get_algo_dlen(GCRY_MD_SHA1);
      resend_check_hash = malloc(hash_len);
      gcry_md_hash_buffer(GCRY_MD_SHA1, resend_check_hash, recvbuf, recvsize);
      if (resend_hash && memcmp(resend_hash, resend_check_hash, hash_len) == 0) {
	free(resend_check_hash);
	return -1;
      }
      if (!resend_hash) {
	resend_hash = resend_check_hash;
      } else {
	memcpy (resend_hash, resend_check_hash, hash_len);
	free(resend_check_hash);
      }
    }
  return recvsize;
}

/* Send TOSEND of size SENDSIZE to the socket.  Then wait for a new packet,
   resending TOSEND on timeout, and ignoring duplicate packets; the
   new packet is put in RECVBUF of size RECVBUFSIZE and the actual size
   of the new packet is returned.  */

static ssize_t
sendrecv (void *recvbuf, size_t recvbufsize, void *tosend, size_t sendsize, uint8_t reply_extype)
{
  struct pollfd pfd;
  int tries = 0;
  int recvsize;
  time_t start = time (NULL);
  time_t end;
  
  pfd.fd = sockfd;
  pfd.events = POLLIN;
  tries = 0;
  
  for (;;)
    {
      int pollresult;
      
      if (sendto (sockfd, tosend, sendsize, 0,
		  dest_addr, sizeof (struct sockaddr_in)) != (int) sendsize)
	error (1, errno, "can't send packet");
      do {
	pollresult = poll (&pfd, 1, timeout << tries);
      } while (pollresult == -1 && errno == EINTR);
      if (pollresult == -1)
	error (1, errno, "can't poll socket");
      if (pollresult != 0)
	{
	  recvsize = recv_ignore_dup (recvbuf, recvbufsize, reply_extype);
	  end = time (NULL);
	  if (recvsize != -1)
	    break;
	  continue;
	}
      if (tries > 5)
	error (1, 0, "no response from target");
      tries++;
    }

  /* Wait at least 2s for a response or 4 times the time it took
     last time.  */
  if (start == end)
    timeout = 2000;
  else
    timeout = 4000 * (end - start);

  return recvsize;
}

struct isakmp_attribute *
make_transform_ike(int dh_group, int crypt, int hash, int keylen)
{
  struct isakmp_attribute *a;
  
  a = new_isakmp_attribute_16 (IKE_ATTRIB_GROUP_DESC, dh_group, NULL);
  a = new_isakmp_attribute_16 (IKE_ATTRIB_AUTH_METHOD, 
			       XAUTH_AUTH_XAUTHInitPreShared, a);
  a = new_isakmp_attribute_16 (IKE_ATTRIB_HASH, hash, a);
  a = new_isakmp_attribute_16 (IKE_ATTRIB_ENC, crypt, a);
  if (keylen != 0)
    a = new_isakmp_attribute_16 (IKE_ATTRIB_KEY_LENGTH,
				 keylen, a);
  return a;
}

struct isakmp_payload *
make_our_sa_ike (void)
{
  struct isakmp_payload *r = new_isakmp_payload (ISAKMP_PAYLOAD_SA);
  struct isakmp_payload *t = NULL, *tn;
  struct isakmp_attribute *a;
  int dh_grp = get_dh_group_ike()->ike_sa_id;
  unsigned int crypt, hash, keylen;
  int i;
  
  r->u.sa.doi = ISAKMP_DOI_IPSEC;
  r->u.sa.situation = ISAKMP_IPSEC_SIT_IDENTITY_ONLY;
  r->u.sa.proposals = new_isakmp_payload (ISAKMP_PAYLOAD_P);
  r->u.sa.proposals->u.p.prot_id = ISAKMP_IPSEC_PROTO_ISAKMP;
  for (crypt = 0; crypt < sizeof(supp_crypt) / sizeof(supp_crypt[0]); crypt++) {
    keylen = supp_crypt[crypt].keylen;
    for (hash = 0; hash < sizeof(supp_hash) / sizeof(supp_hash[0]); hash++) {
      tn = t;
      t = new_isakmp_payload (ISAKMP_PAYLOAD_T);
      t->u.t.id = ISAKMP_IPSEC_KEY_IKE;
      a = make_transform_ike(dh_grp, supp_crypt[crypt].ike_sa_id,
			 supp_hash[hash].ike_sa_id, keylen);
      t->u.t.attributes = a;
      t->next = tn;
    }
  }
  for (i = 0, tn = t; tn; tn = tn->next)
      tn->u.t.number = i++;
  r->u.sa.proposals->u.p.transforms = t;
  return r;
}

struct sa_block 
{
  uint8_t i_cookie[ISAKMP_COOKIE_LENGTH];
  uint8_t r_cookie[ISAKMP_COOKIE_LENGTH];
  uint8_t *key;
  int keylen;
  uint8_t *initial_iv;
  uint8_t *skeyid_a;
  uint8_t *skeyid_d;
  int cry_algo, ivlen;
  int md_algo, md_len;
  uint8_t current_iv_msgid[4];
  uint8_t *current_iv;
  uint8_t our_address[4], our_netmask[4];
  uint32_t tous_esp_spi, tothem_esp_spi;
  uint8_t *kill_packet;
  size_t kill_packet_size;
};

void
isakmp_crypt (struct sa_block *s, uint8_t *block, size_t blocklen, int enc)
{
  unsigned char *new_iv;
  GcryCipherHd cry_ctx;
  
  if (blocklen < ISAKMP_PAYLOAD_O 
      || ((blocklen - ISAKMP_PAYLOAD_O) % s->ivlen != 0))
    abort ();

  if ((memcmp (block + ISAKMP_MESSAGE_ID_O, s->current_iv_msgid, 4) != 0)&&(enc >= 0))
    {
      unsigned char *iv;
      GcryMDHd md_ctx;
      
      md_ctx = gcry_md_open(s->md_algo, 0);
      gcry_md_write(md_ctx, s->initial_iv, s->ivlen);
      gcry_md_write(md_ctx, block + ISAKMP_MESSAGE_ID_O, 4);
      gcry_md_final(md_ctx);
      iv = gcry_md_read(md_ctx, 0);
      memcpy (s->current_iv, iv, s->ivlen);
      memcpy (s->current_iv_msgid, block + ISAKMP_MESSAGE_ID_O, 4);
      gcry_md_close(md_ctx);
    }

  new_iv = xallocc (s->ivlen);
  cry_ctx = gcry_cipher_open(s->cry_algo, GCRY_CIPHER_MODE_CBC, 0);
  gcry_cipher_setkey(cry_ctx, s->key, s->keylen);
  gcry_cipher_setiv(cry_ctx, s->current_iv, s->ivlen);
  if (!enc) {
    memcpy (new_iv, block + blocklen - s->ivlen, s->ivlen);
    gcry_cipher_decrypt(cry_ctx, block + ISAKMP_PAYLOAD_O, blocklen - ISAKMP_PAYLOAD_O, NULL, 0);
    memcpy (s->current_iv, new_iv, s->ivlen);
  } else { /* enc == -1 (no longer used) || enc == 1 */
    gcry_cipher_encrypt(cry_ctx, block + ISAKMP_PAYLOAD_O, blocklen - ISAKMP_PAYLOAD_O, NULL, 0);
    if (enc > 0)
      memcpy (s->current_iv, block + blocklen - s->ivlen, s->ivlen);
  }
  gcry_cipher_close(cry_ctx);
  
}

static uint8_t r_packet[2048];
static ssize_t r_length;

void
do_phase_1 (const char *key_id, const char *shared_key,
	    struct sa_block *d)
{
  unsigned char i_nonce[20];
  struct group *dh_grp;
  unsigned char *dh_public;
  unsigned char *returned_hash;
  static const uint8_t xauth_vid[] = XAUTH_VENDOR_ID;
  static const uint8_t my_vid[] = { 
    0x35, 0x53, 0x07, 0x6c, 0x4f, 0x65, 0x12, 0x68, 0x02, 0x82, 0xf2, 0x15,
    0x8a, 0xa8, 0xa0, 0x9e };
  
  struct isakmp_packet *p1;
  
DEBUG(2, printf("S4.1\n"));
  gcry_randomize(d->i_cookie, ISAKMP_COOKIE_LENGTH, GCRY_STRONG_RANDOM);
  if (d->i_cookie[0] == 0)
    d->i_cookie[0] = 1;
hex_dump("i_cookie", d->i_cookie, ISAKMP_COOKIE_LENGTH);
  gcry_randomize(i_nonce, sizeof (i_nonce), GCRY_STRONG_RANDOM);
hex_dump("i_nonce", i_nonce, sizeof (i_nonce));
DEBUG(2, printf("S4.2\n"));
  /* Set up the Diffie-Hellman stuff.  */
  {
    group_init();
    dh_grp = group_get(get_dh_group_ike()->my_id);
    dh_public = xallocc(dh_getlen (dh_grp));
    dh_create_exchange(dh_grp, dh_public);
hex_dump("dh_public", dh_public, dh_getlen (dh_grp));
  }
  
DEBUG(2, printf("S4.3\n"));
  /* Create the first packet.  */
  {
    struct isakmp_payload *l;
    uint8_t *pkt;
    size_t pkt_len;

    p1 = new_isakmp_packet();
    memcpy (p1->i_cookie, d->i_cookie, ISAKMP_COOKIE_LENGTH);
    p1->isakmp_version = ISAKMP_VERSION;
    p1->exchange_type = ISAKMP_EXCHANGE_AGGRESSIVE;
    p1->payload = l = make_our_sa_ike();
    l->next = new_isakmp_data_payload (ISAKMP_PAYLOAD_KE,
				       dh_public, dh_getlen (dh_grp));
    l->next->next = new_isakmp_data_payload (ISAKMP_PAYLOAD_NONCE,
					     i_nonce, sizeof (i_nonce));
    l = l->next->next;
    l->next = new_isakmp_payload (ISAKMP_PAYLOAD_ID);
    l = l->next;
    l->u.id.type = ISAKMP_IPSEC_ID_KEY_ID;
    l->u.id.length = (strlen (key_id) + 1 + 3) & ~3;
    l->u.id.data = xallocc (l->u.id.length);
    memcpy (l->u.id.data, key_id, strlen (key_id));
    l->next = new_isakmp_data_payload (ISAKMP_PAYLOAD_VID,
				       xauth_vid, sizeof (xauth_vid));
    l->next->next = new_isakmp_data_payload (ISAKMP_PAYLOAD_VID,
					     my_vid, sizeof (my_vid));
    l->next->next->next = new_isakmp_data_payload (ISAKMP_PAYLOAD_VID,
						   my_vid, sizeof (my_vid));
    flatten_isakmp_packet (p1, &pkt, &pkt_len, 0);

    /* Now, send that packet and receive a new one.  */
    r_length = sendrecv (r_packet, sizeof (r_packet), 
			 pkt, pkt_len, 0);
    free (pkt);
  }
DEBUG(2, printf("S4.4\n"));
  /* Decode the recieved packet.  */
  {
     struct isakmp_packet *r;
     uint16_t reject;
     struct isakmp_payload *rp;
     struct isakmp_payload *nonce = NULL;
     struct isakmp_payload *ke = NULL;
     struct isakmp_payload *hash = NULL;
     struct isakmp_payload *idp = NULL;
     int seen_xauth_vid = 0;
     unsigned char *skeyid;
     GcryMDHd skeyid_ctx;
     
     reject = 0;
     r = parse_isakmp_packet (r_packet, r_length, &reject);

     /* Verify the correctness of the recieved packet.  */
     if (reject == 0 && 
	 memcmp (r->i_cookie, d->i_cookie, ISAKMP_COOKIE_LENGTH) != 0)
       reject = ISAKMP_N_INVALID_COOKIE;
     if (reject == 0)
       memcpy (d->r_cookie, r->r_cookie, ISAKMP_COOKIE_LENGTH);
     if (reject == 0 && r->exchange_type != ISAKMP_EXCHANGE_AGGRESSIVE)
       reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;
     if (reject == 0 && r->flags != 0)
       reject = ISAKMP_N_INVALID_FLAGS;
     if (reject == 0 && r->message_id != 0)
       reject = ISAKMP_N_INVALID_MESSAGE_ID;
     if (reject != 0)
       error (1, 0, "response was invalid [1]: %s", 
	      isakmp_notify_to_error (reject));
     for (rp = r->payload; rp && reject == 0; rp = rp->next)
       switch (rp->type)
	 {
	 case ISAKMP_PAYLOAD_SA:
	   if (reject == 0 && rp->u.sa.doi != ISAKMP_DOI_IPSEC)
	     reject = ISAKMP_N_DOI_NOT_SUPPORTED;
	   if (reject == 0 && 
	       rp->u.sa.situation != ISAKMP_IPSEC_SIT_IDENTITY_ONLY)
	     reject = ISAKMP_N_SITUATION_NOT_SUPPORTED;
	   if (reject == 0 && 
	       (rp->u.sa.proposals == NULL
		|| rp->u.sa.proposals->next != NULL))
	     reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	   if (reject == 0 &&
	       rp->u.sa.proposals->u.p.prot_id != ISAKMP_IPSEC_PROTO_ISAKMP)
	     reject = ISAKMP_N_INVALID_PROTOCOL_ID;
	   if (reject == 0 &&
	       rp->u.sa.proposals->u.p.spi_size != 0)
	     reject = ISAKMP_N_INVALID_SPI;
	   if (reject == 0 &&
	       (rp->u.sa.proposals->u.p.transforms == NULL
		|| rp->u.sa.proposals->u.p.transforms->next != NULL))
	     reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	   if (reject == 0 &&
	       (rp->u.sa.proposals->u.p.transforms->u.t.id 
		!= ISAKMP_IPSEC_KEY_IKE))
	     reject = ISAKMP_N_INVALID_TRANSFORM_ID;
	   if (reject == 0) {
	     struct isakmp_attribute *a 
	       = rp->u.sa.proposals->u.p.transforms->u.t.attributes;
	     int seen_enc = 0, seen_hash = 0, seen_auth = 0;
	     int seen_group = 0, seen_keylen = 0;
	     for (; a && reject == 0; a = a->next)
	       switch (a->type)
		 {
		   case IKE_ATTRIB_GROUP_DESC: 
		     if (a->af == isakmp_attr_16 &&
			 a->u.attr_16 == get_dh_group_ike()->ike_sa_id)
		       seen_group = 1;
		     else
		       reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		     break;
		   case IKE_ATTRIB_AUTH_METHOD: 
		     if (a->af == isakmp_attr_16 &&
			 a->u.attr_16 == XAUTH_AUTH_XAUTHInitPreShared)
		       seen_auth = 1;
		     else
		       reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		     break;
		   case IKE_ATTRIB_HASH: 
		     if (a->af == isakmp_attr_16)
		       seen_hash = a->u.attr_16;
		     else
		       reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		     break;
		   case IKE_ATTRIB_ENC: 
		     if (a->af == isakmp_attr_16)
		       seen_enc = a->u.attr_16;
		     else
		       reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		     break;
		   case IKE_ATTRIB_KEY_LENGTH: 
		     if (a->af == isakmp_attr_16)
		       seen_keylen = a->u.attr_16;
		     else
		       reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		     break;
		 default:
		   reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
		   break;
		 }
	     if (! seen_group || ! seen_auth || ! seen_hash || ! seen_enc)
	       reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	     
	     if (get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IKE_SA, seen_hash, NULL, 0) == NULL)
	       reject = ISAKMP_N_NO_PROPOSAL_CHOSEN;
	     if (get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IKE_SA, seen_enc, NULL, seen_keylen) == NULL)
	       reject = ISAKMP_N_NO_PROPOSAL_CHOSEN;
	     
	     if (reject == 0) {
	       d->cry_algo = get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IKE_SA,
			     seen_enc, NULL, seen_keylen)->my_id;
	       d->md_algo = get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IKE_SA,
			     seen_hash, NULL, 0)->my_id;
	       DEBUG(1, printf("IKE SA selected %s-%s\n",
			    get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IKE_SA,
				    seen_enc, NULL, seen_keylen)->name,
			    get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IKE_SA,
				    seen_auth, NULL, 0)->name));
	     }
	   }
	   break;

	 case ISAKMP_PAYLOAD_ID:	idp   = rp; break;
	 case ISAKMP_PAYLOAD_KE:	ke    = rp; break;
	 case ISAKMP_PAYLOAD_NONCE:	nonce = rp; break;
	 case ISAKMP_PAYLOAD_HASH:	hash  = rp; break;
	 case ISAKMP_PAYLOAD_VID:
	   if (rp->u.vid.length == sizeof (xauth_vid)
	       && memcmp (rp->u.vid.data, xauth_vid, sizeof (xauth_vid)) == 0)
	     seen_xauth_vid = 1;
	   break;
	 default:
	   reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
	   break;
	 }

     d->md_len = gcry_md_get_algo_dlen(d->md_algo);
     d->ivlen = gcry_cipher_algo_info(d->cry_algo, GCRYCTL_GET_BLKLEN, NULL, NULL);
     d->keylen = gcry_cipher_algo_info(d->cry_algo, GCRYCTL_GET_KEYLEN, NULL, NULL);

     if (reject == 0
	 && (ke == NULL || ke->u.ke.length != dh_getlen (dh_grp)))
       reject = ISAKMP_N_INVALID_KEY_INFORMATION;
     if (reject == 0 && nonce == NULL)
       reject = ISAKMP_N_INVALID_HASH_INFORMATION;
     if (reject != 0)
       error (1, 0, "response was invalid [2]: %s", 
	      isakmp_notify_to_error (reject));
     if (reject == 0 && idp == NULL)
       reject = ISAKMP_N_INVALID_ID_INFORMATION;
     if (reject == 0 && (hash == NULL 
			 || hash->u.hash.length != d->md_len))
       reject = ISAKMP_N_INVALID_HASH_INFORMATION;
     if (reject != 0)
       error (1, 0, "response was invalid [3]: %s", 
	      isakmp_notify_to_error (reject));

     /* Generate SKEYID.  */
     {
       skeyid_ctx = gcry_md_open(d->md_algo, GCRY_MD_FLAG_HMAC);
       gcry_md_setkey(skeyid_ctx, shared_key, strlen (shared_key));
       gcry_md_write(skeyid_ctx, i_nonce, sizeof (i_nonce));
       gcry_md_write(skeyid_ctx, nonce->u.nonce.data, nonce->u.nonce.length);
       gcry_md_final(skeyid_ctx);
       skeyid = gcry_md_read(skeyid_ctx, 0);
hex_dump("skeyid", skeyid, d->md_len);
     }

     /* Verify the hash.  */
     {
       GcryMDHd hm;
       unsigned char *expected_hash;
       uint8_t *sa_f, *idi_f, *idp_f;
       size_t sa_size, idi_size, idp_size;
       struct isakmp_payload *sa, *idi;

       sa = p1->payload;
       for (idi = sa; idi->type != ISAKMP_PAYLOAD_ID; idi = idi->next)
	 ;
       sa->next = NULL;
       idi->next = NULL;
       idp->next = NULL;
       flatten_isakmp_payload (sa, &sa_f, &sa_size);
       flatten_isakmp_payload (idi, &idi_f, &idi_size);
       flatten_isakmp_payload (idp, &idp_f, &idp_size);

       hm = gcry_md_open(d->md_algo, GCRY_MD_FLAG_HMAC);
       gcry_md_setkey(hm, skeyid, d->md_len);
       gcry_md_write(hm, ke->u.ke.data, ke->u.ke.length);
       gcry_md_write(hm, dh_public, dh_getlen (dh_grp));
       gcry_md_write(hm, d->r_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, d->i_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, sa_f + 4, sa_size - 4);
       gcry_md_write(hm, idp_f + 4, idp_size - 4);
       gcry_md_final(hm);
       expected_hash = gcry_md_read(hm, 0);
       
       if (memcmp (expected_hash, hash->u.hash.data, d->md_len) != 0)
	 {
	   error (1, 0, "hash comparison failed: %s\ncheck group password!", 
		  isakmp_notify_to_error (ISAKMP_N_AUTHENTICATION_FAILED));
	 }
       gcry_md_close(hm);

       hm = gcry_md_open(d->md_algo, GCRY_MD_FLAG_HMAC);
       gcry_md_setkey(hm, skeyid, d->md_len);
       gcry_md_write(hm, dh_public, dh_getlen (dh_grp));
       gcry_md_write(hm, ke->u.ke.data, ke->u.ke.length);
       gcry_md_write(hm, d->i_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, d->r_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, sa_f + 4, sa_size - 4);
       gcry_md_write(hm, idi_f + 4, idi_size - 4);
       gcry_md_final(hm);
       returned_hash = xallocc(d->md_len);
       memcpy(returned_hash, gcry_md_read(hm, 0), d->md_len);
       gcry_md_close(hm);
hex_dump("returned_hash", returned_hash, d->md_len);
       
       free (sa_f);
       free (idi);
       free (idp);
     }

     /* Determine all the SKEYID_x keys.  */
     {
       GcryMDHd hm;
       int i;
       static const unsigned char c012[3] = { 0, 1, 2 };
       unsigned char *skeyid_e;
       unsigned char *dh_shared_secret;

       /* Determine the shared secret.  */
       dh_shared_secret = xallocc(dh_getlen (dh_grp));
       dh_create_shared (dh_grp, dh_shared_secret, ke->u.ke.data);
hex_dump("dh_shared_secret", dh_shared_secret, dh_getlen (dh_grp));

       hm = gcry_md_open(d->md_algo, GCRY_MD_FLAG_HMAC);
       gcry_md_setkey(hm, skeyid, d->md_len);
       gcry_md_write(hm, dh_shared_secret, dh_getlen (dh_grp));
       gcry_md_write(hm, d->i_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, d->r_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, c012+0, 1);
       gcry_md_final(hm);
       d->skeyid_d = xallocc(d->md_len);
       memcpy(d->skeyid_d, gcry_md_read(hm, 0), d->md_len);
       gcry_md_close(hm);
hex_dump("skeyid_d", d->skeyid_d, d->md_len);
       
       hm = gcry_md_open(d->md_algo, GCRY_MD_FLAG_HMAC);
       gcry_md_setkey(hm, skeyid, d->md_len);
       gcry_md_write(hm, d->skeyid_d, d->md_len);
       gcry_md_write(hm, dh_shared_secret, dh_getlen (dh_grp));
       gcry_md_write(hm, d->i_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, d->r_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, c012+1, 1);
       gcry_md_final(hm);
       d->skeyid_a = xallocc(d->md_len);
       memcpy(d->skeyid_a, gcry_md_read(hm, 0), d->md_len);
       gcry_md_close(hm);
hex_dump("skeyid_a", d->skeyid_a, d->md_len);
       
       hm = gcry_md_open(d->md_algo, GCRY_MD_FLAG_HMAC);
       gcry_md_setkey(hm, skeyid, d->md_len);
       gcry_md_write(hm, d->skeyid_a, d->md_len);
       gcry_md_write(hm, dh_shared_secret, dh_getlen (dh_grp));
       gcry_md_write(hm, d->i_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, d->r_cookie, ISAKMP_COOKIE_LENGTH);
       gcry_md_write(hm, c012+2, 1);
       gcry_md_final(hm);
       skeyid_e = xallocc(d->md_len);
       memcpy(skeyid_e, gcry_md_read(hm, 0), d->md_len);
       gcry_md_close(hm);
hex_dump("skeyid_e", skeyid_e, d->md_len);

       memset (dh_shared_secret, 0, sizeof (dh_shared_secret));

       /* Determine the IKE encryption key.  */
       d->key = xallocc(d->keylen);
       
       if (d->keylen > d->md_len) {
         for (i = 0; i * d->md_len < d->keylen; i++) {
           hm = gcry_md_open(d->md_algo, GCRY_MD_FLAG_HMAC);
           gcry_md_setkey(hm, skeyid_e, d->md_len);
	   if (i == 0)
             gcry_md_write(hm, "" /* &'\0' */, 1);
	   else
             gcry_md_write(hm, d->key + (i-1) * d->md_len, d->md_len);
           gcry_md_final(hm);
           memcpy(d->key + i * d->md_len, gcry_md_read(hm, 0),
		  min(d->md_len, d->keylen - i*d->md_len));
           gcry_md_close(hm);
         }
       } else { /* keylen <= md_len*/
           memcpy(d->key, skeyid_e, d->keylen);
       }
hex_dump("enc-key", d->key, d->keylen);
       
       memset (skeyid_e, 0, d->md_len);
     }

     /* Determine the initial 3DES IV.  */
     {
       GcryMDHd hm;
       
       assert(d->ivlen < d->md_len);
       hm = gcry_md_open(d->md_algo, 0);
       gcry_md_write(hm, dh_public, dh_getlen (dh_grp));
       gcry_md_write(hm, ke->u.ke.data, ke->u.ke.length);
       gcry_md_final(hm);
       d->current_iv = xallocc(d->ivlen);
       memcpy(d->current_iv, gcry_md_read(hm, 0), d->ivlen);
       gcry_md_close(hm);
hex_dump("current_iv", d->current_iv, d->ivlen);
       memset (d->current_iv_msgid, 0, 4);
     }
     
     gcry_md_close(skeyid_ctx);
  }

DEBUG(2, printf("S4.5\n"));
  /* Send final phase 1 packet.  */
  {
    struct isakmp_packet *p2;
    uint8_t *p2kt;
    size_t p2kt_len;
    struct isakmp_payload *pl;
    
    p2 = new_isakmp_packet ();
    memcpy (p2->i_cookie, d->i_cookie, ISAKMP_COOKIE_LENGTH);
    memcpy (p2->r_cookie, d->r_cookie, ISAKMP_COOKIE_LENGTH);
    p2->flags = ISAKMP_FLAG_E;
    p2->isakmp_version = ISAKMP_VERSION;
    p2->exchange_type = ISAKMP_EXCHANGE_AGGRESSIVE;
    p2->payload = new_isakmp_data_payload (ISAKMP_PAYLOAD_HASH,
					   returned_hash, 
					   d->md_len);
    p2->payload->next = pl = new_isakmp_payload (ISAKMP_PAYLOAD_N);
    pl->u.n.doi = ISAKMP_DOI_IPSEC;
    pl->u.n.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
    pl->u.n.type = ISAKMP_N_IPSEC_INITIAL_CONTACT;
    pl->u.n.spi_length = 2*ISAKMP_COOKIE_LENGTH;
    pl->u.n.spi = xallocc(2*ISAKMP_COOKIE_LENGTH);
    memcpy(pl->u.n.spi+ISAKMP_COOKIE_LENGTH*0, d->i_cookie, ISAKMP_COOKIE_LENGTH);
    memcpy(pl->u.n.spi+ISAKMP_COOKIE_LENGTH*1, d->r_cookie, ISAKMP_COOKIE_LENGTH);
    flatten_isakmp_packet (p2, &p2kt, &p2kt_len, d->ivlen);
    free_isakmp_packet (p2);
    isakmp_crypt (d, p2kt, p2kt_len, 1);

    d->initial_iv = xallocc(d->ivlen);
    memcpy (d->initial_iv, d->current_iv, d->ivlen);
hex_dump("initial_iv", d->initial_iv, d->ivlen);
    
    /* Now, send that packet and receive a new one.  */
    r_length = sendrecv (r_packet, sizeof (r_packet), 
			 p2kt, p2kt_len, 0);
    free (p2kt);
  }
DEBUG(2, printf("S4.6\n"));
  
  free(returned_hash);
}

static uint16_t
unpack_verify_phase2 (struct sa_block *s, 
		      uint8_t *r_packet, 
		      size_t r_length,
		      struct isakmp_packet **r_p,
		      const uint8_t *nonce, size_t nonce_size)
{
  struct isakmp_packet *r;
  uint16_t reject = 0;
  
  *r_p = NULL;
  
  if (r_length < ISAKMP_PAYLOAD_O 
      || ((r_length - ISAKMP_PAYLOAD_O) % s->ivlen
	  != 0))
    return ISAKMP_N_UNEQUAL_PAYLOAD_LENGTHS;

  isakmp_crypt (s, r_packet, r_length, 0);
      
  {
    r = parse_isakmp_packet (r_packet, r_length, &reject);
    if (reject != 0)
      return reject;
  }
	
  /* Verify the basic stuff.  */
  if (memcmp (r->i_cookie, s->i_cookie, ISAKMP_COOKIE_LENGTH) != 0
      || memcmp (r->r_cookie, s->r_cookie, ISAKMP_COOKIE_LENGTH) != 0)
    return ISAKMP_N_INVALID_COOKIE;
  if (r->flags != ISAKMP_FLAG_E)
    return ISAKMP_N_INVALID_FLAGS;
  
  {
    size_t sz, spos;
    GcryMDHd hm;
    unsigned char *expected_hash;
    struct isakmp_payload *h = r->payload;
    
    if (h == NULL
	|| h->type != ISAKMP_PAYLOAD_HASH
	|| h->u.hash.length != s->md_len)
      return ISAKMP_N_INVALID_HASH_INFORMATION;
    
    spos = (ISAKMP_PAYLOAD_O
	    + (r_packet[ISAKMP_PAYLOAD_O + 2] << 8) 
	    + r_packet[ISAKMP_PAYLOAD_O + 3]);
    
    /* Compute the real length based on the payload lengths.  */
    for (sz = spos; 
	 r_packet[sz] != 0; 
	 sz += r_packet [sz+2] << 8 | r_packet[sz+3])
      ;
    sz += r_packet [sz+2] << 8 | r_packet[sz+3];
    
    hm = gcry_md_open(s->md_algo, GCRY_MD_FLAG_HMAC);
    gcry_md_setkey(hm, s->skeyid_a, s->md_len);
    gcry_md_write(hm, r_packet + ISAKMP_MESSAGE_ID_O, 4);
    if (nonce)
      gcry_md_write(hm, nonce, nonce_size);
    gcry_md_write(hm, r_packet + spos, sz - spos);
    gcry_md_final(hm);
    expected_hash = gcry_md_read(hm, 0);
    
    if(opt_debug >= 3) {
	    printf("hashlen: %d\n", s->md_len);
	    printf("u.hash.length: %d\n", h->u.hash.length);
	    hex_dump("expected_hash", expected_hash,  s->md_len);
	    hex_dump("h->u.hash.data", h->u.hash.data, s->md_len);
    }
    
    reject = 0;
    if (memcmp (h->u.hash.data, expected_hash, s->md_len) != 0)
      reject = ISAKMP_N_AUTHENTICATION_FAILED;
    gcry_md_close(hm);
#if 0
    if (reject != 0)
      return reject;
#endif
  }
  *r_p = r;
  return 0;
}

static void
phase2_authpacket (struct sa_block *s, struct isakmp_payload *pl,
		   uint8_t exchange_type, uint32_t msgid,
		   uint8_t **p_flat, size_t *p_size,
		   uint8_t *nonce_i, int ni_len, uint8_t *nonce_r, int nr_len)
{
  struct isakmp_packet *p;
  uint8_t *pl_flat;
  size_t pl_size;
  GcryMDHd hm;
  uint8_t msgid_sent[4];

  /* Build up the packet.  */
  p = new_isakmp_packet();
  memcpy (p->i_cookie, s->i_cookie, ISAKMP_COOKIE_LENGTH);
  memcpy (p->r_cookie, s->r_cookie, ISAKMP_COOKIE_LENGTH);
  p->flags = ISAKMP_FLAG_E;
  p->isakmp_version = ISAKMP_VERSION;
  p->exchange_type = exchange_type;
  p->message_id = msgid;
  p->payload = new_isakmp_payload (ISAKMP_PAYLOAD_HASH);
  p->payload->next = pl;
  p->payload->u.hash.length = s->md_len;
  p->payload->u.hash.data = xallocc (s->md_len);
  
  /* Set the MAC.  */
  hm = gcry_md_open(s->md_algo, GCRY_MD_FLAG_HMAC);
  gcry_md_setkey(hm, s->skeyid_a, s->md_len);
  
  if (pl == NULL) {
    DEBUG(3, printf("authing NULL package!\n"));
    gcry_md_write(hm, "" /* \0 */, 1);
  }
  
  msgid_sent[0] = msgid >> 24;
  msgid_sent[1] = msgid >> 16;
  msgid_sent[2] = msgid >> 8;
  msgid_sent[3] = msgid;
  gcry_md_write(hm, msgid_sent, sizeof (msgid_sent));

  if (nonce_i != NULL)
    gcry_md_write(hm, nonce_i, ni_len);
  
  if (nonce_r != NULL)
    gcry_md_write(hm, nonce_r, nr_len);
  
  if (pl != NULL) {
    flatten_isakmp_payload (pl, &pl_flat, &pl_size);
    gcry_md_write(hm, pl_flat, pl_size);
    memset (pl_flat, 0, pl_size);
    free (pl_flat);
  }

  gcry_md_final(hm);
  memcpy(p->payload->u.hash.data, gcry_md_read(hm, 0), s->md_len);
  gcry_md_close(hm);

  flatten_isakmp_packet (p, p_flat, p_size, s->ivlen);
  free_isakmp_packet (p);
}

static void
sendrecv_phase2 (struct sa_block *s, struct isakmp_payload *pl,
		 uint8_t exchange_type, uint32_t msgid, int sendonly, uint8_t reply_extype,
		 uint8_t **save_p_flat, size_t *save_p_size,
		 uint8_t *nonce_i, int ni_len, uint8_t *nonce_r, int nr_len)
{
  uint8_t *p_flat;
  size_t p_size;

  if ((save_p_flat == NULL)||(*save_p_flat == NULL)) {
    phase2_authpacket (s, pl, exchange_type, msgid, &p_flat, &p_size,
		  nonce_i, ni_len, nonce_r, nr_len);
    isakmp_crypt (s, p_flat, p_size, 1);
  } else {
    p_flat = *save_p_flat;
    p_size = *save_p_size;
  }

  if (! sendonly)
    r_length = sendrecv (r_packet, sizeof (r_packet), 
			 p_flat, p_size, reply_extype);
  else
    {
      if (sendto (sockfd, p_flat, p_size, 0,
		  dest_addr, sizeof (struct sockaddr_in)) != (int) p_size
	  && sendonly == 1)
	error (1, errno, "can't send packet");
    }
  if (save_p_flat == NULL) {
    free (p_flat);
  } else {
    *save_p_flat = p_flat;
    *save_p_size = p_size;
  }
}

static void
phase2_fatal (struct sa_block *s, const char *msg, uint16_t id)
{
  struct isakmp_payload *pl;
  uint32_t msgid;

DEBUG(1, printf("\n\n---!!!!!!!!! entering phase2_fatal !!!!!!!!!---\n\n\n"));
  gcry_randomize((uint8_t *) &msgid, sizeof (msgid), GCRY_WEAK_RANDOM);
  pl = new_isakmp_payload (ISAKMP_PAYLOAD_N);
  pl->u.n.doi = ISAKMP_DOI_IPSEC;
  pl->u.n.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
  pl->u.n.type = id;
  sendrecv_phase2 (s, pl, ISAKMP_EXCHANGE_INFORMATIONAL, msgid, 2, 0,0,0,0,0,0,0);
  
  gcry_randomize((uint8_t *) &msgid, sizeof (msgid), GCRY_WEAK_RANDOM);
  pl = new_isakmp_payload (ISAKMP_PAYLOAD_D);
  pl->u.d.doi = ISAKMP_DOI_IPSEC;
  pl->u.d.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
  pl->u.d.spi_length = 2*ISAKMP_COOKIE_LENGTH;
  pl->u.d.num_spi = 1;
  pl->u.d.spi = xallocc(1 * sizeof (uint8_t *));
  pl->u.d.spi[0] = xallocc(2*ISAKMP_COOKIE_LENGTH);
  memcpy(pl->u.d.spi[0]+ISAKMP_COOKIE_LENGTH*0, s->i_cookie, ISAKMP_COOKIE_LENGTH);
  memcpy(pl->u.d.spi[0]+ISAKMP_COOKIE_LENGTH*1, s->r_cookie, ISAKMP_COOKIE_LENGTH);
  sendrecv_phase2 (s, pl, ISAKMP_EXCHANGE_INFORMATIONAL, msgid, 2, 0,0,0,0,0,0,0);

  error (1, 0, msg, isakmp_notify_to_error (id));
}

static void 
do_phase_2_xauth (struct sa_block *s)
{
  struct isakmp_packet *r;
  int loopcount;

DEBUG(2, printf("S5.1\n"));
  /* This can go around for a while.  */
  for (loopcount = 0;; loopcount++)
    {
      uint16_t reject;
      struct isakmp_payload *rp;
      struct isakmp_attribute *a, *ap, *reply_attr;
      char ntop_buf[32];
      
DEBUG(2, printf("S5.2\n"));
      reject = unpack_verify_phase2 (s, r_packet, r_length, &r, NULL, 0);
      if (reject == ISAKMP_N_PAYLOAD_MALFORMED)
	{
	  r_length = sendrecv (r_packet, sizeof (r_packet), NULL, 0, 0);
	  continue;
	}
      
DEBUG(2, printf("S5.3\n"));
      /* Check the transaction type is OK.  */
      if (reject == 0 && 
	  r->exchange_type != ISAKMP_EXCHANGE_MODECFG_TRANSACTION)
	reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;
      
      /* After the hash, expect an attribute block.  */
      if (reject == 0
	  && (r->payload->next == NULL
	      || r->payload->next->next != NULL
	      || r->payload->next->type != ISAKMP_PAYLOAD_MODECFG_ATTR))
	reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
      
      if (reject == 0 &&
	  r->payload->next->u.modecfg.type == ISAKMP_MODECFG_CFG_SET)
	break;
      if (reject == 0
	  && r->payload->next->u.modecfg.type != ISAKMP_MODECFG_CFG_REQUEST)
	reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
      
      if (reject != 0)
	phase2_fatal (s, "expected xauth packet; rejected: %s", reject);
      
DEBUG(2, printf("S5.4\n"));
      a = r->payload->next->u.modecfg.attributes;
      /* First, print any messages, and verify that we understand the
	 conversation.  */
      for (ap = a; ap && reject == 0; ap = ap->next)
	switch (ap->type)
	  {
	  case ISAKMP_XAUTH_ATTRIB_TYPE:
	    if (ap->af != isakmp_attr_16 || ap->u.attr_16 != 0)
	      reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
	    break;
	  case ISAKMP_XAUTH_ATTRIB_USER_NAME:
	  case ISAKMP_XAUTH_ATTRIB_USER_PASSWORD:
	  case ISAKMP_XAUTH_ATTRIB_PASSCODE:
	  case ISAKMP_XAUTH_ATTRIB_ANSWER:
	    break;
	  case ISAKMP_XAUTH_ATTRIB_MESSAGE:
	    if (ap->af == isakmp_attr_16)
	      DEBUG(1, printf ("%c%c\n", ap->u.attr_16 >> 8, ap->u.attr_16));
	    else
	      DEBUG(1, printf ("%.*s%s", ap->u.lots.length, ap->u.lots.data,
		      ((ap->u.lots.data
			&& ap->u.lots.data[ap->u.lots.length - 1] != '\n')
		       ? "\n" : "")));
	    break;
	  default:
	    reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
	  }
DEBUG(2, printf("S5.5\n"));
      if (reject != 0)
	phase2_fatal (s, "xauth packet unsupported: %s", reject);
      
      inet_ntop (dest_addr->sa_family, 
		 &((struct sockaddr_in *)dest_addr)->sin_addr,
		 ntop_buf, sizeof (ntop_buf));
      
      /* Collect data from the user.  */
      reply_attr = NULL;
      for (ap = a; ap && reject == 0; ap = ap->next)
	switch (ap->type)
	  {
	  case ISAKMP_XAUTH_ATTRIB_USER_NAME:
	    /*if (loopcount == 0)*/
	      {
		struct isakmp_attribute *na;
		na = new_isakmp_attribute(ap->type, reply_attr);
		reply_attr = na;
		na->u.lots.length = strlen (config[CONFIG_XAUTH_USERNAME]) + 1;
		na->u.lots.data = xallocc (na->u.lots.length);
		strcpy (na->u.lots.data, config[CONFIG_XAUTH_USERNAME]);
		break;
	      }
	    printf ("User name for %s: ", ntop_buf);
	    fflush (stdout);
	  case ISAKMP_XAUTH_ATTRIB_ANSWER:
	    {
	      char *line = NULL;
	      size_t linelen = 0;
	      ssize_t linesz;
	      struct isakmp_attribute *na;
	      
	      if ((linesz = getline (&line, &linelen, stdin)) == -1)
		error (1, errno, "reading user input");
	      if (line[linesz - 1] == '\n')
		linesz--;
	      
	      na = new_isakmp_attribute(ap->type, reply_attr);
	      reply_attr = na;
	      na->u.lots.length = linesz;
	      na->u.lots.data = line;
	    }
	    break;
	    
	  case ISAKMP_XAUTH_ATTRIB_USER_PASSWORD:
	    /*if (loopcount == 0)*/
	      {
		struct isakmp_attribute *na;
		na = new_isakmp_attribute(ap->type, reply_attr);
		reply_attr = na;
		na->u.lots.length = strlen (config[CONFIG_XAUTH_PASSWORD]) + 1;
		na->u.lots.data = xallocc (na->u.lots.length);
		strcpy (na->u.lots.data, config[CONFIG_XAUTH_PASSWORD]);
		break;
	      }
	  case ISAKMP_XAUTH_ATTRIB_PASSCODE:
	    {
	      char prompt[64];
	      char *pass;
	      struct isakmp_attribute *na;
	      
	      if (ap->type == ISAKMP_XAUTH_ATTRIB_USER_PASSWORD)
		sprintf (prompt, "Password for VPN at %s: ", ntop_buf);
	      else
		sprintf (prompt, "Passcode for VPN at %s: ", ntop_buf);
	      pass = getpass (prompt);
	      
	      na = new_isakmp_attribute(ap->type, reply_attr);
	      reply_attr = na;
	      na->u.lots.length = strlen (pass) + 1;
	      na->u.lots.data = xallocc (na->u.lots.length);
	      memcpy (na->u.lots.data, pass, na->u.lots.length);
	      memset (pass, 0, na->u.lots.length);
	    }
	    break;
	  default:
	    ;
	  }
      
      /* Send the response.  */
      rp = new_isakmp_payload (ISAKMP_PAYLOAD_MODECFG_ATTR);
      rp->u.modecfg.type = ISAKMP_MODECFG_CFG_REPLY;
      rp->u.modecfg.id = r->payload->next->u.modecfg.id;
      rp->u.modecfg.attributes = reply_attr;
      sendrecv_phase2 (s, rp, ISAKMP_EXCHANGE_MODECFG_TRANSACTION,
		       r->message_id, 0, 0,0,0,0,0,0,0);
      
      free_isakmp_packet (r);
    }
  
DEBUG(2, printf("S5.6\n"));
  {
    /* The final SET should have just one attribute.  */
    uint16_t reject = 0;
    struct isakmp_attribute *a = r->payload->next->u.modecfg.attributes;
    uint16_t set_result;
    
    if (a == NULL
	|| a->type != ISAKMP_XAUTH_ATTRIB_STATUS
	|| a->af != isakmp_attr_16
	|| a->next != NULL)
      {
	reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
	phase2_fatal (s, "xauth SET response rejected: %s", reject);
      }
    set_result = a->u.attr_16;

    /* ACK the SET.  */
    r->payload->next->u.modecfg.type = ISAKMP_MODECFG_CFG_ACK;
    sendrecv_phase2 (s, r->payload->next, ISAKMP_EXCHANGE_MODECFG_TRANSACTION,
		     r->message_id, 1, 0,0,0,0,0,0,0);
    r->payload->next = NULL;
    free_isakmp_packet (r);

    if (set_result == 0)
      error (2, 0, "authentication unsuccessful");
  }
DEBUG(2, printf("S5.7\n"));
}

static void 
do_phase_2_config (struct sa_block *s)
{
  struct isakmp_payload *rp;
  struct isakmp_attribute *a;
  struct isakmp_packet *r;
  uint32_t msgid;
  uint16_t reject;
  int seen_address = 0;
  
  gcry_randomize((uint8_t *)&msgid, sizeof (msgid), GCRY_WEAK_RANDOM);
  if (msgid == 0)
    msgid = 1;
  
  rp = new_isakmp_payload (ISAKMP_PAYLOAD_MODECFG_ATTR);
  rp->u.modecfg.type = ISAKMP_MODECFG_CFG_REQUEST;
  rp->u.modecfg.id = 20;
  a = NULL;
  a = new_isakmp_attribute(ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_ADDRESS, a);
  rp->u.modecfg.attributes = a;
  sendrecv_phase2 (s, rp, ISAKMP_EXCHANGE_MODECFG_TRANSACTION,
		   msgid, 0, 0,0,0,0,0,0,0);

  reject = unpack_verify_phase2 (s, r_packet, r_length, &r, NULL, 0);

  /* Check the transaction type & message ID are OK.  */
  if (reject == 0 && r->message_id != msgid)
    reject = ISAKMP_N_INVALID_MESSAGE_ID;
  if (reject == 0 && r->exchange_type != ISAKMP_EXCHANGE_MODECFG_TRANSACTION)
    reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;
  
  /* After the hash, expect an attribute block.  */
  if (reject == 0
      && (r->payload->next == NULL
	  || r->payload->next->next != NULL
	  || r->payload->next->type != ISAKMP_PAYLOAD_MODECFG_ATTR
#if 0
	  || r->payload->next->u.modecfg.id != 20
#endif
	  || r->payload->next->u.modecfg.type != ISAKMP_MODECFG_CFG_REPLY))
    reject = ISAKMP_N_PAYLOAD_MALFORMED;

  if (reject != 0)
    phase2_fatal (s, "configuration response rejected: %s", reject);

  for (a = r->payload->next->u.modecfg.attributes; 
       a && reject == 0; 
       a = a->next)
    switch (a->type)
      {
      case ISAKMP_MODECFG_ATTRIB_INTERNAL_IP4_ADDRESS:
	if (a->af != isakmp_attr_lots || a->u.lots.length != 4)
	  reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
	else
	  memcpy (s->our_address, a->u.lots.data, 4);
	seen_address = 1;
	break;

      default:
	reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
	break;
      }
  
  if (reject == 0 && ! seen_address)
    reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;

  if (reject != 0)
    phase2_fatal (s, "configuration response rejected: %s", reject);

}

static void
config_tunnel (struct sa_block *s)
{
  int sock;
  struct ifreq ifr;
  uint8_t *addr;
  uint8_t real_netmask[4];
  /*int i;*/
  
  real_netmask[0] = 0xFF;
  real_netmask[1] = 0xFF;
  real_netmask[2] = 0xFF;
  real_netmask[3] = 0xFF;

  sock = socket (AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    error (1, errno, "making socket");
  memset (&ifr, 0, sizeof(ifr));
  memcpy (ifr.ifr_name, tun_name, IFNAMSIZ);
  ifr.ifr_addr.sa_family = AF_INET;
  addr = (uint8_t *)&((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
  memcpy (addr, s->our_address, 4);
  if (ioctl (sock, SIOCSIFADDR, &ifr) != 0)
    error (1, errno, "setting interface address");
  memcpy (addr, real_netmask, 4);
  if (ioctl (sock, SIOCSIFNETMASK, &ifr) != 0)
    error (1, errno, "setting interface netmask");
#if 0
  for (i = 0; i < 4; i++)
    addr[i] = s->our_address[i] | ~real_netmask[i];
  if (ioctl (sock, SIOCSIFBRDADDR, &ifr) != 0)
    error (1, errno, "setting interface broadcast");
#endif
  /* The magic constants are:
     1500  the normal ethernet MTU
     -20   the size of an IP header
     -8    the size of an ESP header
     -2    minimum padding length
     -12   the size of the HMAC
  ifr.ifr_mtu = 1500-20-8-2-12;
   Override: experimental found best value */
  ifr.ifr_mtu = 1412;
  if (ioctl (sock, SIOCSIFMTU, &ifr) != 0)
    error (1, errno, "setting interface MTU");
  if (ioctl (sock, SIOCGIFFLAGS, &ifr) != 0)
    error (1, errno, "getting interface flags");
  ifr.ifr_flags |= (IFF_UP /*| IFF_BROADCAST | IFF_MULTICAST */ | IFF_RUNNING);
  if (ioctl (sock, SIOCSIFFLAGS, &ifr) != 0)
    error (1, errno, "setting interface flags");
  close (sock);
}

static uint8_t *
gen_keymat (struct sa_block *s,
	    uint8_t protocol, uint32_t spi, 
	    int md_algo, int crypt_algo,
	    const uint8_t *dh_shared, size_t dh_size,
	    const uint8_t *ni_data, size_t ni_size,
	    const uint8_t *nr_data, size_t nr_size)
{
  GcryMDHd hm;
  uint8_t *block;
  int i;
  int blksz;
  int cnt;
  
  int md_len = gcry_md_get_algo_dlen(md_algo);
  int cry_len = gcry_cipher_algo_info(crypt_algo, GCRYCTL_GET_KEYLEN, NULL, NULL);
  
  blksz = md_len + cry_len;
  cnt = (blksz + s->md_len - 1) / s->md_len;
  block = xallocc (cnt * s->md_len);
DEBUG(3, printf("generating %d bytes keymat (cnt=%d)\n", blksz, cnt));
  if (cnt < 1)
    abort ();

  for (i = 0; i < cnt; i++)
    {
      hm = gcry_md_open(s->md_algo, GCRY_MD_FLAG_HMAC);
      gcry_md_setkey(hm, s->skeyid_d, s->md_len);
      if (i != 0)
	gcry_md_write(hm, block + (i-1) * s->md_len, s->md_len);
      if (dh_shared != NULL)
	gcry_md_write(hm, dh_shared, dh_size);
      gcry_md_write(hm, &protocol, 1);
      gcry_md_write(hm, (uint8_t *)&spi, sizeof (spi));
      gcry_md_write(hm, ni_data, ni_size);
      gcry_md_write(hm, nr_data, nr_size);
      gcry_md_final(hm);
      memcpy(block + i * s->md_len, gcry_md_read(hm, 0), s->md_len);
      gcry_md_close(hm);
    }
  return block;
}

struct isakmp_attribute *
make_transform_ipsec(int dh_group, int hash, int keylen)
{
  struct isakmp_attribute *a = NULL;
  
  if (dh_group)
    a = new_isakmp_attribute_16 (ISAKMP_IPSEC_ATTRIB_GROUP_DESC,
				 dh_group, NULL);
  a = new_isakmp_attribute_16 (ISAKMP_IPSEC_ATTRIB_AUTH_ALG, 
			       hash, a);
  a = new_isakmp_attribute_16 (ISAKMP_IPSEC_ATTRIB_ENCAP_MODE,
			       IPSEC_ENCAP_TUNNEL, a);
  if (keylen != 0)
    a = new_isakmp_attribute_16 (ISAKMP_IPSEC_ATTRIB_KEY_LENGTH,
				 keylen, a);
  
  return a;
}

struct isakmp_payload *
make_our_sa_ipsec (struct sa_block *s)
{
  struct isakmp_payload *r = new_isakmp_payload (ISAKMP_PAYLOAD_SA);
  struct isakmp_payload *t = NULL, *tn;
  struct isakmp_attribute *a;
  int dh_grp = get_dh_group_ipsec()->ipsec_sa_id;
  unsigned int crypt, hash, keylen;
  int i;
  
  r = new_isakmp_payload (ISAKMP_PAYLOAD_SA);
  r->u.sa.doi = ISAKMP_DOI_IPSEC;
  r->u.sa.situation = ISAKMP_IPSEC_SIT_IDENTITY_ONLY;
  r->u.sa.proposals = new_isakmp_payload (ISAKMP_PAYLOAD_P);
  r->u.sa.proposals->u.p.spi_size = 4;
  r->u.sa.proposals->u.p.spi = xallocc (4);
  /* The sadb_sa_spi field is already in network order.  */
  memcpy (r->u.sa.proposals->u.p.spi, &s->tous_esp_spi, 4);
  r->u.sa.proposals->u.p.prot_id = ISAKMP_IPSEC_PROTO_IPSEC_ESP;
  for (crypt = 0; crypt < sizeof(supp_crypt) / sizeof(supp_crypt[0]); crypt++) {
    keylen = supp_crypt[crypt].keylen;
    for (hash = 0; hash < sizeof(supp_hash) / sizeof(supp_hash[0]); hash++) {
      tn = t;
      t = new_isakmp_payload (ISAKMP_PAYLOAD_T);
      t->u.t.id = supp_crypt[crypt].ipsec_sa_id;
      a = make_transform_ipsec(dh_grp, supp_hash[hash].ipsec_sa_id, keylen);
      t->u.t.attributes = a;
      t->next = tn;
    }
  }
  for (i = 0, tn = t; tn; tn = tn->next)
      tn->u.t.number = i++;
  r->u.sa.proposals->u.p.transforms = t;
  return r;
}

static void
setup_link (struct sa_block *s)
{
  struct isakmp_payload *rp, *us, *ke = NULL, *them, *nonce_r = NULL;
  struct isakmp_packet *r;
  struct group *dh_grp = NULL;
  uint32_t msgid;
  uint16_t reject;
  uint8_t *p_flat = NULL, *realiv = NULL, realiv_msgid[4];
  size_t p_size = 0;
  uint8_t nonce[20], *dh_public = NULL;
  int ipsec_cry_algo = 0, ipsec_hash_algo = 0, i;
  
DEBUG(2, printf("S8.1\n"));
  /* Set up the Diffie-Hellman stuff.  */
  if (get_dh_group_ipsec()->my_id) {
    dh_grp = group_get(get_dh_group_ipsec()->my_id);
    DEBUG(3, printf("len = %d\n", dh_getlen (dh_grp)));
    dh_public = xallocc(dh_getlen (dh_grp));
    dh_create_exchange(dh_grp, dh_public);
hex_dump("dh_public", dh_public, dh_getlen (dh_grp));
  }
  
  gcry_randomize((uint8_t *)&s->tous_esp_spi, sizeof (s->tous_esp_spi), GCRY_WEAK_RANDOM);
  rp = make_our_sa_ipsec(s);
  gcry_randomize((uint8_t *)nonce, sizeof (nonce), GCRY_WEAK_RANDOM);
  rp->next = new_isakmp_data_payload (ISAKMP_PAYLOAD_NONCE,
				      nonce, sizeof (nonce));
  
  us = new_isakmp_payload (ISAKMP_PAYLOAD_ID);
  us->u.id.type = ISAKMP_IPSEC_ID_IPV4_ADDR;
  us->u.id.length = 4;
  us->u.id.data = xallocc(4);
  memcpy (us->u.id.data, s->our_address, sizeof (struct in_addr));
  them = new_isakmp_payload (ISAKMP_PAYLOAD_ID);
  them->u.id.type = ISAKMP_IPSEC_ID_IPV4_ADDR_SUBNET;
  them->u.id.length = 8;
  them->u.id.data = xallocc(8);
  memset(them->u.id.data, 0, 8);
  us->next = them;
  
  if (!dh_grp) {
    rp->next->next = us;
  } else {
    rp->next->next = new_isakmp_data_payload (ISAKMP_PAYLOAD_KE,
				       dh_public, dh_getlen (dh_grp));
    rp->next->next->next = us;
  }

  gcry_randomize((uint8_t *)&msgid, sizeof (&msgid), GCRY_WEAK_RANDOM);
  if (msgid == 0)
    msgid = 1;
  
DEBUG(2, printf("S8.2\n"));
  for (i = 0; i < 4; i++) {
    sendrecv_phase2 (s, rp, ISAKMP_EXCHANGE_IKE_QUICK,
		     msgid, 0, 0, &p_flat, &p_size, 0,0,0,0);
    
    if (realiv == NULL) {
      realiv = xallocc(s->ivlen);
      memcpy(realiv, s->current_iv, s->ivlen);
      memcpy(realiv_msgid, s->current_iv_msgid, 4);
    }

DEBUG(2, printf("S8.3\n"));
    reject = unpack_verify_phase2 (s, r_packet, r_length, &r, 
				   nonce, sizeof (nonce));

DEBUG(2, printf("S8.4\n"));
    if (((reject == 0)||(reject == ISAKMP_N_AUTHENTICATION_FAILED))
	&& r->exchange_type == ISAKMP_EXCHANGE_INFORMATIONAL) {
       /* handle notifie responder-lifetime (ignore)*/
       /* (broken hash => ignore AUTHENTICATION_FAILED) */
       if (reject == 0 && r->payload->next->type != ISAKMP_PAYLOAD_N)
         reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
       
       if (reject == 0 && r->payload->next->u.n.type == ISAKMP_N_IPSEC_RESPONDER_LIFETIME) {
DEBUG(2, printf("ignoring responder-lifetime notify\n"));
	 memcpy(s->current_iv, realiv, s->ivlen);
	 memcpy(s->current_iv_msgid, realiv_msgid, 4);
	 continue;
       }
    }
    
    /* Check the transaction type & message ID are OK.  */
    if (reject == 0 && r->message_id != msgid)
      reject = ISAKMP_N_INVALID_MESSAGE_ID;
    
    if (reject == 0 && r->exchange_type != ISAKMP_EXCHANGE_IKE_QUICK)
      reject = ISAKMP_N_INVALID_EXCHANGE_TYPE;
  
    /* The SA payload must be second.  */
    if (reject == 0 && r->payload->next->type != ISAKMP_PAYLOAD_SA)
      reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
    
    if (p_flat)
      free(p_flat);
    if (realiv)
      free(realiv);
    break;
  }

DEBUG(2, printf("S8.5\n"));
  if (reject != 0)
    phase2_fatal (s, "quick mode response rejected: %s\ncheck pfs setting", reject);

DEBUG(2, printf("S8.6\n"));
  for (rp = r->payload->next; rp && reject == 0; rp = rp->next)
    switch (rp->type)
      {
      case ISAKMP_PAYLOAD_SA:
	if (reject == 0 && rp->u.sa.doi != ISAKMP_DOI_IPSEC)
	  reject = ISAKMP_N_DOI_NOT_SUPPORTED;
	if (reject == 0 && 
	    rp->u.sa.situation != ISAKMP_IPSEC_SIT_IDENTITY_ONLY)
	  reject = ISAKMP_N_SITUATION_NOT_SUPPORTED;
	if (reject == 0 && 
	    (rp->u.sa.proposals == NULL
	     || rp->u.sa.proposals->next != NULL))
	  reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	if (reject == 0 &&
	    rp->u.sa.proposals->u.p.prot_id != ISAKMP_IPSEC_PROTO_IPSEC_ESP)
	  reject = ISAKMP_N_INVALID_PROTOCOL_ID;
	if (reject == 0 &&
	    rp->u.sa.proposals->u.p.spi_size != 4)
	  reject = ISAKMP_N_INVALID_SPI;
	if (reject == 0 &&
	    (rp->u.sa.proposals->u.p.transforms == NULL
	     || rp->u.sa.proposals->u.p.transforms->next != NULL))
	  reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	if (reject == 0) {
	  struct isakmp_attribute *a 
	    = rp->u.sa.proposals->u.p.transforms->u.t.attributes;
	  int seen_enc = rp->u.sa.proposals->u.p.transforms->u.t.id;
	  int seen_auth = 0, seen_encap = 0, seen_group = 0, seen_keylen = 0;

	  memcpy (&s->tothem_esp_spi, rp->u.sa.proposals->u.p.spi, 4);

	  for (; a && reject == 0; a = a->next)
	    switch (a->type)
	      {
	      case ISAKMP_IPSEC_ATTRIB_AUTH_ALG:
		if (a->af == isakmp_attr_16)
		  seen_auth = a->u.attr_16;
		else
		  reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		break;
	      case ISAKMP_IPSEC_ATTRIB_ENCAP_MODE:
		if (a->af == isakmp_attr_16 &&
		    a->u.attr_16 == IPSEC_ENCAP_TUNNEL)
		  seen_encap = 1;
		else
		  reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		break;
	      case ISAKMP_IPSEC_ATTRIB_GROUP_DESC: 
		if (dh_grp &&
		    a->af == isakmp_attr_16 &&
		    a->u.attr_16 == get_dh_group_ipsec()->ipsec_sa_id)
		  seen_group = 1;
		else
		  reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		break;
	      case ISAKMP_IPSEC_ATTRIB_KEY_LENGTH: 
		if (a->af == isakmp_attr_16)
		  seen_keylen = a->u.attr_16;
		else
		  reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
		break;
	      default:
		reject = ISAKMP_N_ATTRIBUTES_NOT_SUPPORTED;
		break;
	      }
	  if (reject == 0 && (! seen_auth || ! seen_encap ||
	      (dh_grp && !seen_group)))
	    reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	    
	  if (get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IPSEC_SA, seen_auth, NULL, 0) == NULL)
	    reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	  if (get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IPSEC_SA, seen_enc, NULL, seen_keylen) == NULL)
	    reject = ISAKMP_N_BAD_PROPOSAL_SYNTAX;
	  
	  if (reject == 0) {
	    ipsec_cry_algo = get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IPSEC_SA,
		     seen_enc, NULL, seen_keylen)->my_id;
	    ipsec_hash_algo = get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IPSEC_SA,
			     seen_auth, NULL, 0)->my_id;
	    DEBUG(1, printf("IPSEC SA selected %s-%s\n",
			    get_algo(SUPP_ALGO_CRYPT, SUPP_ALGO_IPSEC_SA,
				    seen_enc, NULL, seen_keylen)->name,
			    get_algo(SUPP_ALGO_HASH, SUPP_ALGO_IPSEC_SA,
				    seen_auth, NULL, 0)->name));
	  }
	}
	break;
	
      case ISAKMP_PAYLOAD_N:		
	break;
      case ISAKMP_PAYLOAD_ID:		
	break;
      case ISAKMP_PAYLOAD_KE:
	ke = rp;
	break;
      case ISAKMP_PAYLOAD_NONCE:	
	nonce_r = rp;
	break;

      default:
	reject = ISAKMP_N_INVALID_PAYLOAD_TYPE;
	break;
      }
  
  if (reject == 0 && nonce_r == NULL)
    reject = ISAKMP_N_INVALID_HASH_INFORMATION;
  if (reject == 0 && dh_grp
	 && (ke == NULL || ke->u.ke.length != dh_getlen (dh_grp)))
       reject = ISAKMP_N_INVALID_KEY_INFORMATION;
  if (reject != 0)
    phase2_fatal (s, "quick mode response rejected [2]: %s", reject);
  
  /* send final packet */
  sendrecv_phase2 (s, NULL, ISAKMP_EXCHANGE_IKE_QUICK,
                   msgid, 1, 0,0,0, nonce, sizeof (nonce),
		   nonce_r->u.nonce.data, nonce_r->u.nonce.length);
  
DEBUG(2, printf("S8.7\n"));
  /* Create the delete payload, now that we have all the information.  */
  {
    struct isakmp_payload *d_isakmp, *d_ipsec;
    uint32_t del_msgid;

    gcry_randomize((uint8_t *)&del_msgid, sizeof (del_msgid), GCRY_WEAK_RANDOM);
    d_isakmp = new_isakmp_payload (ISAKMP_PAYLOAD_D);
    d_isakmp->u.d.doi = ISAKMP_DOI_IPSEC;
    d_isakmp->u.d.protocol = ISAKMP_IPSEC_PROTO_ISAKMP;
    d_isakmp->u.d.spi_length = 2*ISAKMP_COOKIE_LENGTH;
    d_isakmp->u.d.num_spi = 1;
    d_isakmp->u.d.spi = xallocc(1 * sizeof (uint8_t *));
    d_isakmp->u.d.spi[0] = xallocc(2*ISAKMP_COOKIE_LENGTH);
    memcpy(d_isakmp->u.d.spi[0]+ISAKMP_COOKIE_LENGTH*0, s->i_cookie, ISAKMP_COOKIE_LENGTH);
    memcpy(d_isakmp->u.d.spi[0]+ISAKMP_COOKIE_LENGTH*1, s->r_cookie, ISAKMP_COOKIE_LENGTH);
    d_ipsec = new_isakmp_payload (ISAKMP_PAYLOAD_D);
    d_ipsec->next = d_isakmp;
    d_ipsec->u.d.doi = ISAKMP_DOI_IPSEC;
    d_ipsec->u.d.protocol = ISAKMP_IPSEC_PROTO_IPSEC_ESP;
    d_ipsec->u.d.spi_length = 4;
    d_ipsec->u.d.num_spi = 2;
    d_ipsec->u.d.spi = xallocc (2 * sizeof (uint8_t *));
    d_ipsec->u.d.spi[0] = xallocc (d_ipsec->u.d.spi_length);
    memcpy (d_ipsec->u.d.spi[0], &s->tous_esp_spi, 4);
    d_ipsec->u.d.spi[1] = xallocc (d_ipsec->u.d.spi_length);
    memcpy (d_ipsec->u.d.spi[1], &s->tothem_esp_spi, 4);
    phase2_authpacket (s, d_ipsec, ISAKMP_EXCHANGE_INFORMATIONAL,
		       del_msgid, &s->kill_packet, &s->kill_packet_size,
		       nonce, sizeof (nonce),
		       nonce_r->u.nonce.data, nonce_r->u.nonce.length);
    isakmp_crypt (s, s->kill_packet, s->kill_packet_size, 1);
  }
DEBUG(2, printf("S8.8\n"));
  
  /* Set up the interface here so it's ready when our acknowledgement
     arrives.  */
  {
    uint8_t *tous_keys, *tothem_keys;
    struct sockaddr_in tothem_dest;
    unsigned char *dh_shared_secret = NULL;

    if (dh_grp) {
      /* Determine the shared secret.  */
      dh_shared_secret = xallocc(dh_getlen (dh_grp));
      dh_create_shared (dh_grp, dh_shared_secret, ke->u.ke.data);
hex_dump("dh_shared_secret", dh_shared_secret, dh_getlen (dh_grp));
    }
    tous_keys = gen_keymat (s, ISAKMP_IPSEC_PROTO_IPSEC_ESP, s->tous_esp_spi,
			    ipsec_hash_algo, ipsec_cry_algo,
			    dh_shared_secret, dh_grp?dh_getlen (dh_grp):0,
			    nonce, sizeof (nonce), 
			    nonce_r->u.nonce.data, nonce_r->u.nonce.length);
    memset (&tothem_dest, 0, sizeof (tothem_dest));
    tothem_dest.sin_family = AF_INET;
    memcpy (&tothem_dest.sin_addr, s->our_address, 4);
    tothem_keys = gen_keymat (s, ISAKMP_IPSEC_PROTO_IPSEC_ESP, s->tothem_esp_spi,
			      ipsec_hash_algo, ipsec_cry_algo,
			      dh_shared_secret, dh_grp?dh_getlen (dh_grp):0,
			      nonce, sizeof (nonce), 
			      nonce_r->u.nonce.data, nonce_r->u.nonce.length);
DEBUG(2, printf("S8.9\n"));
    vpnc_doit (s->tous_esp_spi, tous_keys, &tothem_dest,
	       s->tothem_esp_spi, tothem_keys, (struct sockaddr_in *)dest_addr,
	       tun_fd, ipsec_hash_algo, ipsec_cry_algo,
	       s->kill_packet, s->kill_packet_size, dest_addr, config[CONFIG_PID_FILE]);
  }
}

static const struct config_names_s {
  const char *desc;
  const char *name;
  const char *option;
  enum config_enum nm;
  const int needsArgument;
} config_names[] = {
  /* Note: broken config file parser does NOT support option
   * names where one is a prefix of another option */
  { "<0/1/2/3/99> Show extraneous debug messages", "Debug ", "--debug", CONFIG_DEBUG, 1 },
  { "Don't detach from the console after login", "No Detach", "--no-detach", CONFIG_ND, 0 },
  { "Don't ask anything, exit on missing options", "Noninteractive", "--non-inter", CONFIG_NON_INTERACTIVE, 0 },
  { "<filename> -- store the pid of background process there", "Pidfile ", "--pid-file", CONFIG_PID_FILE, 1 },
  { "<0-65535> -- store the pid of background process there", "Local Port ", "--local-port", CONFIG_LOCAL_PORT, 1 },
  { "<ascii string> -- visible name of the TUN interface", "Interface name ", "--ifname", CONFIG_IF_NAME, 1 },
  { "<dh1/dh2/dh5> -- name of the IKE DH Group", "IKE DH Group ", "--dh", CONFIG_IKE_DH, 1 },
  { "<nopfs/dh1/dh2/dh5>", "Perfect Forward Secrecy ", "--pfs", CONFIG_IPSEC_PFS, 1 },
  { "<ip/hostname> -- name of your IPSec gateway", "IPSec gateway ", "--gateway", CONFIG_IPSEC_GATEWAY, 1 },
  { "<ascii string>", "IPSec ID ", "--id", CONFIG_IPSEC_ID, 1 },
  { "<ascii string>", "IPSec secret ", NULL, CONFIG_IPSEC_SECRET, 1 },
  { "<ascii string>", "Xauth username ", "--username", CONFIG_XAUTH_USERNAME, 1 },
  { "<ascii string>", "Xauth password ", NULL, CONFIG_XAUTH_PASSWORD, 1 },
  { NULL, NULL, NULL, 0, 0 }
};

void
read_config_file (char *name, const char **configs, int missingok)
{
  FILE *f;
  char *line = NULL;
  ssize_t line_length = 0;
  int linenum = 0;
  
  f = fopen (name, "r");
  if (missingok && f == NULL && errno == ENOENT)
    return;
  if (f == NULL)
    error (1, errno, "couldn't open `%s'", name);
  for (;;)
    {
      ssize_t llen;
      int i;
      
      llen = getline (&line, &line_length, f);
      if (llen == -1 && feof (f))
	break;
      if (llen == -1)
	error (1, errno, "reading `%s'", name);
      if (line[llen - 1] == '\n')
	line[llen - 1] = 0;
      linenum++;
      for (i = 0; config_names[i].name != NULL; i++)
	if (strncasecmp (config_names[i].name, line, 
			 strlen (config_names[i].name)) == 0)
	  {
	    // boolean implementation, using harmles pointer targets as true
	    if (!config_names[i].needsArgument) {
	      configs[config_names[i].nm] = config_names[i].name;
	      break;
	    }
	    if (configs[config_names[i].nm] == NULL)
	      configs[config_names[i].nm] = 
		strdup (line + strlen (config_names[i].name));
	    if (configs[config_names[i].nm] == NULL)
	      error (1, errno, "can't allocate memory");
	    break;
	  }
      if (config_names[i].name == NULL && line[0] != '#' && line[0] != 0)
	error_at_line (0, 0, name, linenum, 
		       "warning: unknown configuration directive");
    }
}

int main(int argc, char **argv)
{
  struct sa_block oursa;
  int i;
  int print_config = 0;
  const uint8_t hex_test[] = { 0, 1, 2, 3};
  
  test_pack_unpack();
  gcry_check_version("1.1.12");
  gcry_control( GCRYCTL_INIT_SECMEM, 16384, 0 );
  hex_dump("hex_test", hex_test, sizeof(hex_test));

  for (i = 1; i < argc; i++)
    if (argv[i][0] == '-')
      {
	int c;
	int known = 0;

	for (c = 0; config_names[c].name != NULL && ! known; c++)
	  if (config_names[c].option != NULL
	      && strncmp (argv[i], config_names[c].option,
			  strlen (config_names[c].option)) == 0)
	    {
	      char *s = NULL;
	      
	      known = 1;
	      if (argv[i][strlen (config_names[c].option)] == '=')
		s = strdup (argv[i] + strlen (config_names[c].option) + 1);
	      else if (argv[i][strlen (config_names[c].option)] == 0) {
		if (config_names[c].needsArgument) {
		  if (i + 1 < argc)
		    s = argv[++i];
		  else
		    known = 0;
		} else
		  s = argv[i]; /* no arg, fill in something */
	      } else
		known = 0;
	      if (known)
		config[config_names[c].nm] = s;
	    }
	
	if (! known && strcmp (argv[i], "--version") == 0)
	  {
	    unsigned int i;
	    
	    printf ("vpnc version " VERSION "\n");
	    printf ("Copyright (C) 2002, 2003 Geoffrey Keating, Maurice Massar\n");
	    printf ("%s",
"vpnc comes with NO WARRANTY, to the extent permitted by law.\n"
"You may redistribute copies of vpnc under the terms of the GNU General\n"
"Public License.  For more information about these matters, see the files\n"
		    "named COPYING.\n");
	    printf ("\n");
	    printf ("Supported DH-Groups:");
	    for (i = 0; i < sizeof(supp_dh_group) / sizeof(supp_dh_group[0]); i++)
		    printf(" %s", supp_dh_group[i].name);
	    printf ("\n");
	    printf ("Supported Hash-Methods:");
	    for (i = 0; i < sizeof(supp_hash) / sizeof(supp_hash[0]); i++)
		    printf(" %s", supp_hash[i].name);
	    printf ("\n");
	    printf ("Supported Encryptions:");
	    for (i = 0; i < sizeof(supp_crypt) / sizeof(supp_crypt[0]); i++)
		    printf(" %s", supp_crypt[i].name);
	    printf ("\n");
	    exit (0);
	  }
	if (! known && strcmp (argv[i], "--print-config") == 0)
	  {
	    print_config = 1;
	    known = 1;
	    break;
	  }
	
	if (! known)
	  {
	    int c;
	    
	    printf ("usage: %s [--version] [--print-config] [options] [config file]\n", argv[0]);
	    printf ("%12s %s\n", "Option", "Config file directive <arguments> -- Description");
	    for (c = 0; config_names[c].name != NULL; c++)
	      printf ("%12s %s %s\n", 
		      (config_names[c].option == NULL
		       ? "(no option)"
		       : config_names[c].option),
		      config_names[c].name, config_names[c].desc);
	    printf ("Report bugs to vpnc@unix-ag.uni-kl.de\n");
	    exit (0);
	  }
      }
    else
      read_config_file (argv[i], config, 0);

  read_config_file ("/etc/vpnc.conf", config, 1);

  if (!config[CONFIG_IKE_DH])
    config[CONFIG_IKE_DH] = "dh2";
  if (!config[CONFIG_IPSEC_PFS])
    config[CONFIG_IPSEC_PFS] = "nopfs";
  if (!config[CONFIG_LOCAL_PORT])
    config[CONFIG_LOCAL_PORT] = "500";

  opt_debug=(config[CONFIG_DEBUG]) ? atoi(config[CONFIG_DEBUG]) : 0;
  opt_nd=(config[CONFIG_ND]) ? 1 : 0;

  if (opt_debug >= 99) {
	  printf("WARNING! debug levels >= 99 include username and password (hex encoded)\n");
  }
  
  for (i = 0; i < LAST_CONFIG; i++)
    if ((config[i] == NULL)&&(config[CONFIG_NON_INTERACTIVE] == NULL))
      {
	char *s = NULL;
	size_t s_len = 0;
	
	switch (i)
	  {
	  case CONFIG_DEBUG:
	  case CONFIG_IF_NAME:
          case CONFIG_ND:
          case CONFIG_NON_INTERACTIVE:
          case CONFIG_PID_FILE:
          case CONFIG_LOCAL_PORT:
	  case CONFIG_IKE_DH:
	  case CONFIG_IPSEC_PFS:
             /* no interaction */
	    break;
	  case CONFIG_IPSEC_GATEWAY:
	    printf ("Enter IPSec gateway address: ");
	    break;
	  case CONFIG_IPSEC_ID:
	    printf ("Enter IPSec ID for %s: ", 
		    config[CONFIG_IPSEC_GATEWAY]);
	    break;
	  case CONFIG_IPSEC_SECRET:
	    printf ("Enter IPSec secret for %s@%s: ",
		    config[CONFIG_IPSEC_ID], config[CONFIG_IPSEC_GATEWAY]);
	    break;
	  case CONFIG_XAUTH_USERNAME:
	    printf ("Enter username for %s: ", config[CONFIG_IPSEC_GATEWAY]);
	    break;
	  case CONFIG_XAUTH_PASSWORD:
	    printf ("Enter password for %s@%s: ",
		    config[CONFIG_XAUTH_USERNAME],
		    config[CONFIG_IPSEC_GATEWAY]);
	    break;
	  }
	fflush (stdout);
        switch (i)
        {
           case CONFIG_IPSEC_SECRET:
           case CONFIG_XAUTH_PASSWORD:
              s = strdup (getpass (""));
              break;
           case CONFIG_DEBUG:
           case CONFIG_IF_NAME:
           case CONFIG_ND:
           case CONFIG_NON_INTERACTIVE:
           case CONFIG_PID_FILE:
           case CONFIG_LOCAL_PORT:
	   case CONFIG_IKE_DH:
	   case CONFIG_IPSEC_PFS:
              /* no interaction */
              break;
           default:
              getline (&s, &s_len, stdin);
        }
	if (s != NULL && s[strlen (s) - 1] == '\n')
	  s[strlen (s) - 1] = 0;
	config[i] = s;
      }
  
  if (!config[CONFIG_IPSEC_GATEWAY])
	error (1, 0, "missing IPSec gatway address");
  if (!config[CONFIG_IPSEC_ID])
	error (1, 0, "missing IPSec ID");
  if (!config[CONFIG_IPSEC_SECRET])
	error (1, 0, "missing IPSec secret");
  if (!config[CONFIG_XAUTH_USERNAME])
	error (1, 0, "missing Xauth username");
  if (!config[CONFIG_XAUTH_PASSWORD])
	error (1, 0, "missing Xauth password");
  if (get_dh_group_ike() == NULL)
	error (1, 0, "IKE DH Group \"%s\" unsupported\n", config[CONFIG_IKE_DH]);
  if (get_dh_group_ipsec() == NULL)
	error (1, 0, "Perfect Forward Secrecy \"%s\" unsupported\n", config[CONFIG_IPSEC_PFS]);
  if (get_dh_group_ike()->ike_sa_id == 0)
	error (1, 0, "IKE DH Group must not be nopfs\n");
  
  if (print_config)
    {
      for (i = 0; config_names[i].name != NULL; i++)
	printf ("%s%s\n", config_names[i].name, config[config_names[i].nm]);
      exit (0);
    }

DEBUG(2, printf("S1\n"));
  dest_addr = init_sockaddr (config[CONFIG_IPSEC_GATEWAY], 500);
DEBUG(2, printf("S2\n"));
  sockfd = make_socket (atoi(config[CONFIG_LOCAL_PORT]));
DEBUG(2, printf("S3\n"));
  setup_tunnel();
DEBUG(2, printf("S4\n"));

  memset(&oursa, '\0', sizeof(oursa));
  do_phase_1 (config[CONFIG_IPSEC_ID], config[CONFIG_IPSEC_SECRET], &oursa);
DEBUG(2, printf("S5\n"));
  do_phase_2_xauth (&oursa);
DEBUG(2, printf("S6\n"));
  do_phase_2_config (&oursa);
DEBUG(2, printf("S7\n"));
  config_tunnel (&oursa);
DEBUG(2, printf("S8\n"));

  setup_link (&oursa);

  return 0;
}
