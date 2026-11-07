#ifndef __PARAMETERS_H__
#define __PARAMETERS_H__

#include "defines.h"
#include "charm++.h"

#include <iostream>
#include <stdio.h>
#include <string>
#include <map>

#include "pup_stl.h"

using namespace std;

struct Parameters {
  string filename;

  Real theta;
  Real dtime;
  Real dthf;
  Real epssq;
  Real tolsq;

  int numTreePieces;
  int numParticles;
  int ppc;
  int ppb;

  int yieldPeriod;
  int cacheLineSize;

  int iterations;

  //int branchFactor;

  void pup(PUP::er &p){
    p | filename;
    p | numTreePieces;
    p | numParticles;
    p | dtime;
    p | dthf;
    p | tolsq;
    p | epssq;
    p | ppc;
    p | ppb;
    p | yieldPeriod;
    p | theta;
    p | cacheLineSize;
    p | iterations;
  }

  void extractParameters(int argc, char **argv, map<string,string> &tab){
    for(int i = 0; i < argc; i++){
      string arg = string(argv[i]);
      size_t pos = arg.find("=");
      if(pos != string::npos){
        size_t len = arg.length();
        string key = arg.substr(1,pos-1); 
        string val = arg.substr(pos+1,len-pos-1);
        tab[key] = val;
      }
    }
  }

  string getparam(string name, map<string,string> &table)
  {
    map<string,string>::iterator it = table.find(name);
    if(it != table.end()){
      return it->second;
    }
    return string();
  }

  /*
   * GETIPARAM, ..., GETDPARAM: get int, long, bool, or double parameters.
   */

  int getiparam(string name, int def, map<string,string> &tab)
  {
    string val;

    val = getparam(name,tab);
    if(val.empty())
      return def;
    else
      return (atoi(val.c_str()));
  }

  long getlparam(string name, map<string,string> &tab)
  {
    string val;

    val = getparam(name,tab);
    if(val.empty())
      return -1;
    else 
      return (atol(val.c_str()));
  }

  bool getbparam(string name, map<string,string> &tab)
  {
    string val;

    val = getparam(name,tab);
    if (strchr("tTyY1", *(val.c_str())) != 0) {
      return (true);
    }
    if (strchr("fFnN0", *(val.c_str())) != 0) {
      return (false);
    }
    fprintf(stderr,"getbparam: %s=%s not bool\n", name.c_str(), val.c_str());
    return false;
  }

  Real getrparam(string name, Real default_value, map<string,string> &tab)
  {
    string val;

    val = getparam(name,tab);
    if(val.empty())
      return default_value;
    else 
      return (atof(val.c_str()));
  }

  /*
   * EXTRVALUE: extract value from name=value string.
   */

  string getsparam(string arg, map<string,string> &tab)
  {
    return getparam(arg,tab);
  }


};

#endif
