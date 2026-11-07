/**
 * This is the main header file for the Kripke Mini-App.
 */
#ifndef KRIPKE_H__
#define KRIPKE_H__

#include <string>
#include <vector>
#include <stdio.h>
#include <cmath>
#include <strings.h>
#include "charm++.h"
#include "Input_Variables.h"
#include "kripke.decl.h"

#define KRESTRICT __restrict__

// Forward Decl
struct Grid_Data;
struct Subdomain;
class Timing;

typedef std::pair<int, int> IntPair;

/**
 * Main chare
 */
class Main : public CBase_Main {
  private:
    int iter;
    double part_last;
    Input_Variables vars;

  public:
    Main(CkArgMsg *m);
    Main(CkMigrateMessage *m);
    void /*reductiontarget*/ unknownCounts(long psi, long rhs, long phi, long phi_out);
    void /*reductiontarget*/ regionVolumes(double reg1, double reg2, double reg3);
    void /*reductiontarget*/ particleEdit(double part);
    void /*reductiontarget*/ done();
    void /*entry*/ outputStats(Timing timing);
};

/**
 * Kripke chare array
 */
class Kripke : public CBase_Kripke {
  private:
    int iter, dep, totalDeps;
    bool domainCorner;
    int *currDeps;
    Grid_Data *grid_data;
    Kernel *kernel;

  public:
    Kripke_SDAG_CODE
    
    Kripke(Input_Variables ivars);
    Kripke(CkMigrateMessage *m);
    ~Kripke();
    void pup(PUP::er &p);
  
    void contributeLocalInfo();
    double getNumParticles();
    void setDomainCorner();
    void setTotalSweepDeps();
    void updateSweepDeps();
    void applyBoundaryConditions(Subdomain *sdom);
    void /*entry*/ getTimers();
};

/**
 * Scattered layout of zonesets/chares to cores
 */
class ScatterLayout : public CBase_ScatterLayout {
  public:
    ScatterLayout();
    ~ScatterLayout();
    int procNum(int, const CkArrayIndex &idx);
};

/**
 * Converts a nesting tag to a human-readable string.
 */
inline std::string nestingString(Nesting_Order nesting){
  switch(nesting){
    case NEST_DGZ: return("DGZ");
    case NEST_DZG: return("DZG");
    case NEST_GDZ: return("GDZ");
    case NEST_GZD: return("GZD");
    case NEST_ZDG: return("ZDG");
    case NEST_ZGD: return("ZGD");
  }
  return("UNKNOWN");
}

/**
 * Converts a string (eg. from command line) to a nesting tag.
 */
inline Nesting_Order nestingFromString(std::string const &str){
  for(int i = 0;i < 6;++ i){
    if(!strcasecmp(str.c_str(), nestingString((Nesting_Order)i).c_str())){
      return (Nesting_Order)i;
  }
 }
  return (Nesting_Order)-1;
}


/**
 * Compares two vectors for differences.
 * Used in testing suite.
 */
inline bool compareVector(std::string const &name,
    std::vector<double> const &a,
    std::vector<double> const &b, double tol, bool verbose){

  if(a.size() != b.size()){
    if(verbose){
      CkPrintf("Vectors are different lengths: %ld, %ld\n",
          (long)a.size(), (long)b.size());
    }
    return true;
  }

  bool is_diff = false;
  for(size_t i = 0;i < a.size();++i){
    if(std::abs(a[i]-b[i]) > tol){
      is_diff = true;
      if(verbose){
        CkPrintf("%s[%d]:%e, %e [%e]\n",
            name.c_str(), (int)i,
            a[i], b[i], std::abs(a[i]-b[i]));
        is_diff = true;
      }
      else{
        break;
      }
    }
  }

  return is_diff;
}

/**
 * Compares two scalars for differences.
 * Used in testing suite.
 */
inline bool compareScalar(std::string const &name,
    double a, double b, double tol, bool verbose){

  if(std::abs(a-b) > tol){
    if(verbose){
      CkPrintf("%s:%e, %e [%e]\n",
          name.c_str(),
          a, b, std::abs(a-b));
    }
    return true;
  }
  return false;
}

#endif

