/*  
    VTun - Virtual Tunnel over TCP/IP network.

    Copyright (C) 1998-2000  Maxim Krasnyansky <max_mk@yahoo.com>

    VTun has been derived from VPPP package by Maxim Krasnyansky. 

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 */

/*
 * $Id: tun_dev.c,v 1.1.2.2 2000/11/20 08:15:53 maxk Exp $
 */ 

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <stdarg.h>

#include <sys/socket.h>
#include <net/if.h>
#include <net/if_tun.h>

#include "sysdep.h"

/* 
 * Allocate TUN device, returns opened fd. 
 * Stores dev name in the first arg(must be large enough).
 */  
int tun_open(char *dev)
{
    char tunname[14];
    int i, fd;

    if( *dev ) {
       sprintf(tunname, "/dev/%s", dev);
       return open(tunname, O_RDWR);
    }

    for(i=0; i < 255; i++){
       sprintf(tunname, "/dev/tun%d", i);
       /* Open device */
       if( (fd=open(tunname, O_RDWR)) > 0 ){
          sprintf(dev, "tun%d", i);
          return fd;
       }
    }
    return -1;
}

int tun_close(int fd, char *dev)
{
    dev = NULL; /*unused*/
    return close(fd);
}

/* Read/write frames from TUN device */
int tun_write(int fd, char *buf, int len)
{
    return write(fd, buf, len);
}

int tun_read(int fd, char *buf, int len)
{
    return read(fd, buf, len);
}

/***********************************************************************/
/* other support functions */

void config_tunnel(const char *dev, struct in_addr myaddr)
{
  int sock;
  struct ifreq ifr;
  uint8_t *addr;

  /* prepare socket and ifr */
  sock = socket (AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    error (1, errno, "making socket");
  memset (&ifr, 0, sizeof(ifr));
  memcpy (ifr.ifr_name, dev, IFNAMSIZ);
  
  addr = (uint8_t *)&((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
  
  /* set my address */
  ifr.ifr_addr.sa_family = AF_INET;
  memcpy (addr, &myaddr, 4);
  if (ioctl (sock, SIOCSIFADDR, &ifr) != 0)
    error (1, errno, "setting interface address");
  
  /* set p-t-p addr (== my address) */
  ifr.ifr_addr.sa_family = AF_INET;
  memcpy (addr, &myaddr, 4);
  if (ioctl (sock, SIOCSIFDSTADDR, &ifr) != 0)
    error (1, errno, "setting interface address");
  
  /* set netmask (== 255.255.255.255) */
  ifr.ifr_addr.sa_family = AF_INET;
  memset (addr, 0xFF, 4);
  if (ioctl (sock, SIOCSIFNETMASK, &ifr) != 0)
    error (1, errno, "setting interface netmask");
  
  /* set MTU */
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
  
  /* set interface flags */
  if (ioctl (sock, SIOCGIFFLAGS, &ifr) != 0)
    error (1, errno, "getting interface flags");
  ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
  if (ioctl (sock, SIOCSIFFLAGS, &ifr) != 0)
    error (1, errno, "setting interface flags");
  
  close (sock);
}

void error(int status, int errornum, const char *fmt, ...)
{
	char   *buf2;
	va_list        ap;

	va_start(ap, fmt);
	vasprintf(&buf2, fmt, ap);
	va_end(ap);
	fprintf(stderr, "%s", buf2);
	if (errornum)
		fprintf(stderr, ": %s\n", strerror(errornum));
	free(buf2);
	
	if (status)
		exit(status);
}

int getline(char **line, size_t *length, FILE *stream)
{
	char *tmpline;
	size_t len;

	tmpline = fgetln(stream, &len);
	if (feof(stream))
		return -1;
	if (*line == NULL) {
		*line = malloc(len + 1);
		*length = len + 1;
	}
	if (*length < len + 1) {
		*line = realloc(*line, len + 1);
		*length = len + 1;
	}
	if (*line == NULL)
		return -1;
	memcpy(*line, tmpline, len);
	(*line)[len] = '\0';
	return len;
}
