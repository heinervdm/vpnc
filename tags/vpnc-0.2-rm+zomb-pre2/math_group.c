/* borrowed from isakmpd-20030718 (-; */

/*	$OpenBSD: math_group.c,v 1.18 2003/06/03 14:28:16 ho Exp $	*/
/*	$EOM: math_group.c,v 1.25 2000/04/07 19:53:26 niklas Exp $	*/

/*
 * Copyright (c) 1998 Niels Provos.  All rights reserved.
 * Copyright (c) 1999, 2000 Niklas Hallqvist.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This code was written under funding by Ericsson Radio Systems.
 */

#include <assert.h>
#include <sys/param.h>
#include <stdlib.h>
#include <string.h>

#include <gcrypt.h>

#include "math_group.h"

/* We do not want to export these definitions.  */
int modp_getlen (struct group *);
void modp_getraw (struct group *, GcryMPI, unsigned char *);
int modp_setraw (struct group *, GcryMPI, unsigned char *, int);
int modp_setrandom (struct group *, GcryMPI);
int modp_operation (struct group *, GcryMPI, GcryMPI, GcryMPI);

struct modp_group {
  GcryMPI gen;			/* Generator */
  GcryMPI p;				/* Prime */
  GcryMPI a, b, c, d;
};

/*
 * This module provides access to the operations on the specified group
 * and is absolutly free of any cryptographic devices. This is math :-).
 */

/* Describe preconfigured MODP groups */

/*
 * The Generalized Number Field Sieve has an asymptotic running time
 * of: O(exp(1.9223 * (ln q)^(1/3) (ln ln q)^(2/3))), where q is the
 * group order, e.g. q = 2**768.
 */

struct modp_dscr oakley_modp[] =
{
  { OAKLEY_GRP_1, 72,	/* This group is insecure, only sufficient for DES */
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A63A3620FFFFFFFFFFFFFFFF",
    "2"
  },
  { OAKLEY_GRP_2, 82,	/* This group is a bit better */
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD1"
    "29024E088A67CC74020BBEA63B139B22514A08798E3404DD"
    "EF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245"
    "E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE65381"
    "FFFFFFFFFFFFFFFF",
    "2"
  },
};


/* XXX I want to get rid of the casting here.  */
struct group groups[] = {
  {
    MODP, OAKLEY_GRP_1, 0, &oakley_modp[0], 0, 0, 0, 0, 0,
    (int (*) (struct group *))modp_getlen,
    (void (*) (struct group *, void *, unsigned char *))modp_getraw,
    (int (*) (struct group *, void *, unsigned char *, int))modp_setraw,
    (int (*) (struct group *, void *))modp_setrandom,
    (int (*) (struct group *, void *, void *, void *))modp_operation
  },
  {
    MODP, OAKLEY_GRP_2, 0, &oakley_modp[1], 0, 0, 0, 0, 0,
    (int (*) (struct group *))modp_getlen,
    (void (*) (struct group *, void *, unsigned char *))modp_getraw,
    (int (*) (struct group *, void *, unsigned char *, int))modp_setraw,
    (int (*) (struct group *, void *))modp_setrandom,
    (int (*) (struct group *, void *, void *, void *))modp_operation
  },
};

/*
 * Initialize the group structure for later use,
 * this is done by converting the values given in the describtion
 * and converting them to their native representation.
 */
void
group_init (void)
{
  int i;

  for (i = sizeof (groups) / sizeof (groups[0]) - 1; i >= 0; i--) {
    assert(groups[i].type == MODP);
    modp_init (&groups[i]); /* Initialize an over GF(p) */
  }
}

struct group *
group_get (int id)
{
  struct group *new, *clone;

  assert(id >= 1);
  assert(id <= (int)(sizeof (groups) / sizeof (groups[0])));

  clone = &groups[id - 1];

  new = malloc (sizeof *new);
  assert(new);

  assert(clone->type == MODP);
  new = modp_clone (new, clone);
  return new;
}

void
group_free (struct group *grp)
{
  assert (grp->type == MODP);
  modp_free (grp);
  free (grp);
}

struct group *
modp_clone (struct group *new, struct group *clone)
{
  struct modp_group *new_grp, *clone_grp = clone->group;

  new_grp = malloc (sizeof *new_grp);
  assert (new_grp);

  memcpy (new, clone, sizeof (struct group));

  new->group = new_grp;
  new_grp->p = gcry_mpi_copy (clone_grp->p);
  new_grp->gen = gcry_mpi_copy (clone_grp->gen);

  new_grp->a = gcry_mpi_new (clone->bits);
  new_grp->b = gcry_mpi_new (clone->bits);
  new_grp->c = gcry_mpi_new (clone->bits);

  new->gen = new_grp->gen;
  new->a = new_grp->a;
  new->b = new_grp->b;
  new->c = new_grp->c;

  return new;
}

void
modp_free (struct group *old)
{
  struct modp_group *grp = old->group;

  gcry_mpi_release (grp->p);
  gcry_mpi_release (grp->gen);
  gcry_mpi_release (grp->a);
  gcry_mpi_release (grp->b);
  gcry_mpi_release (grp->c);

  free (grp);
}

void
modp_init (struct group *group)
{
  struct modp_dscr *dscr = (struct modp_dscr *)group->group;
  struct modp_group *grp;

  grp = malloc (sizeof *grp);
  assert(grp);

  group->bits = dscr->bits;

  gcry_mpi_scan(&grp->p, GCRYMPI_FMT_HEX, dscr->prime, NULL);
  gcry_mpi_scan(&grp->gen, GCRYMPI_FMT_HEX, dscr->gen, NULL);

  grp->a = gcry_mpi_new (group->bits);
  grp->b = gcry_mpi_new (group->bits);
  grp->c = gcry_mpi_new (group->bits);

  group->gen = grp->gen;
  group->a = grp->a;
  group->b = grp->b;
  group->c = grp->c;

  group->group = grp;
}

int
modp_getlen (struct group *group)
{
  struct modp_group *grp = (struct modp_group *)group->group;

  return (gcry_mpi_get_nbits (grp->p) + 7) / 8;
}

void
modp_getraw (struct group *grp, GcryMPI v, unsigned char *d)
{
  size_t l, l2;
  unsigned char *tmp;
  int ret;
  
  l = grp->getlen (grp);
  ret = gcry_mpi_aprint(GCRYMPI_FMT_STD, (void **)&tmp, &l2, v);
  memcpy(d, tmp + (l2-l), l);
#if 0
  {
	  char *p;
	  gcry_mpi_aprint(GCRYMPI_FMT_HEX, (void **)&p, NULL, v);
	  printf("export %d - %d(%d):\n%s\n", l, l2, ret, p);
  }
#endif
}

int
modp_setraw (struct group *grp, GcryMPI d, unsigned char *s, int l)
{
  int i;
  
  grp = 0; /* unused */
  
  gcry_mpi_set_ui(d, 0);
  for (i = 0; i < l; i++)
    {
      gcry_mpi_mul_2exp (d, d, 8);
      gcry_mpi_add_ui (d, d, s[i]);
    }
#if 0
  {
	  char *p;
	  gcry_mpi_aprint(GCRYMPI_FMT_HEX, (void **)&p, NULL, d);
	  printf("import %d:\n%s\n", l, p);
  }
#endif
  return 0;
}

int
modp_setrandom (struct group *grp, GcryMPI d)
{
  int i, l = grp->getlen (grp);
  u_int32_t tmp = 0;

  gcry_mpi_set_ui (d, 0);

  for (i = 0; i < l; i++)
    {
      if (i % 4)
	gcry_randomize((unsigned char *)&tmp, sizeof(tmp), GCRY_WEAK_RANDOM);

      gcry_mpi_mul_2exp (d, d, 8);
      gcry_mpi_add_ui (d, d, tmp & 0xFF);
      tmp >>= 8;
    }
  return 0;
}

int
modp_operation (struct group *group, GcryMPI d, GcryMPI a, GcryMPI e)
{
  struct modp_group *grp = (struct modp_group *)group->group;

  gcry_mpi_powm(d, a, e, grp->p);
  return 0;
}

