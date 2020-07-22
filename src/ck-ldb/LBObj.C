/**
 * \addtogroup CkLdb
*/
/*@{*/

#include <charm++.h>

#if CMK_LBDB_ON

#include "LBObj.h"
#include "LBOM.h"

/*************************************************************
 * LBObj Object-data Code
 *************************************************************/

void LBObj::Clear(void)
{
//  data.handle = myhandle;
//  data.id = myid;
//  data.omHandle = parentOM;
//  data.omID = parentDB->LbOM(parentOM)->id();
  if (!data.vectorLoad.empty())
  {
    data.vectorLoad.clear();
  }
  data.wallTime = 0.;
#if CMK_LB_CPUTIMER
  data.cpuTime = 0.;
#endif
#if ! COMPRESS_LDB
  data.minWall = 1e6;
  data.maxWall = 0.;
#endif
}

void LBObj::IncrementTime(LBRealType walltime, LBRealType cputime)
{
  if (phase >= 0)
  {
    if (data.vectorLoad.size() < phase + 1)
    {
      data.vectorLoad.resize(phase + 1);
    }
    data.vectorLoad[phase] += walltime;
  }
  data.wallTime += walltime;
#if CMK_LB_CPUTIMER
  data.cpuTime += cputime;
#endif
#if ! COMPRESS_LDB
  if (walltime < data.minWall) data.minWall = walltime;
  if (walltime > data.maxWall) data.maxWall = walltime;
#endif
}

#endif

/*@}*/
