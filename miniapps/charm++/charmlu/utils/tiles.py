#!/usr/bin/python

import math

def factors(n):
   fact=[1,n]
   check=2
   rootn=math.sqrt(n)
   while check<rootn:
     if n%check==0:
       fact.append(check)
       #fact.append(n/check)
     check += 1
   if rootn==check:
     fact.append(rootn)
   fact.sort()
   return fact

def test(b, x, y):
   n = math.sqrt(b*x*y)
   if n != math.floor(n): return False
   if n % x != 0: return False
   if n % y != 0: return False
   if x*y % 12 != 0: return False
   return True


for p in range(100,150):
    for b in range(500,600):
        f = factors(p)
        for x in f:
            if test(b, x, p/x):
                g = max(x,p/x)
                l = min(x,p/x)
                print "numblks: %d, \t%d PEs: \t%d x %d, \taspect: %f" % (math.sqrt(b*p), g*l, g, l, 1.0*g/l)
