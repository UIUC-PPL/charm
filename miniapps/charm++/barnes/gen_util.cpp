/*************************************************************************/
/*                                                                       */
/*  Copyright (c) 1994 Stanford University                               */
/*                                                                       */
/*  All rights reserved.                                                 */
/*                                                                       */
/*  Permission is given to use, copy, and modify this software for any   */
/*  non-commercial purpose as long as this copyright notice is not       */
/*  removed.  All other uses, including redistribution in whole or in    */
/*  part, are forbidden without prior written permission.                */
/*                                                                       */
/*  This software is provided with absolutely no warranty and no         */
/*  support.                                                             */
/*                                                                       */
/*************************************************************************/

#include <stdio.h>
#include "common.h"

#define HZ 60.0
#define MULT 1103515245
#define ADD 12345
#define MASK (0x7FFFFFFF)
#define TWOTO31 2147483648.0

int A = 1;
int B = 0;
int randx = 1;
int lastrand;   /* the last random number */

/*
 * XRAND: generate floating-point random number.
 */

Real prand();

Real xrand(Real xl, Real xh)
{
   Real x;

   return (xl + (xh - xl) * prand());
}

void pranset(int seed)
{
   int proc;
  
   A = 1;
   B = 0;
   randx = (A*seed+B) & MASK;
   A = (MULT * A) & MASK;
   B = (MULT*B + ADD) & MASK;
}

Real prand()
/*
	Return a random Real in [0, 1.0)
*/
{
   lastrand = randx;
   randx = (A*randx+B) & MASK;
   return((Real)lastrand/TWOTO31);
}
