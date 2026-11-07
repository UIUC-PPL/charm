#include "charmHpccg.h"
#include "make_local_matrix.hpp"

#include <string>
#include <iostream>

using namespace std;

/*readonly*/ int numChares;
/*readonly*/ CProxy_charmMain mainProxy;
/*readonly*/ CProxy_CompletionDetector detector;

charmMain::charmMain(CkArgMsg* msg) {
  __sdag_init();
  CkPrintf("charm HPCCG startup\n");
  fflush(stdout);
  
  mainProxy = thisProxy;

  int argc = msg->argc;
  char** argv = msg->argv;

  if(argc != 3 && argc!=5) {
    cerr << "Usage:" << endl
         << "Mode 1: " << argv[0] << " numChares nx ny nz" << endl
         << "     where nx, ny and nz are the local sub-block dimensions, or" << endl
         << "Mode 2: " << argv[0] << " numChares HPC_data_file " << endl
         << "     where HPC_data_file is a globally accessible file containing matrix data." << endl;
    CkExit();
  }

  detector = CProxy_CompletionDetector::ckNew();

  numChares = atoi(argv[1]);
  array = CProxy_charmHpccg::ckNew(numChares);

  if (argc == 5) {
    int nx = atoi(argv[2]);
    int ny = atoi(argv[3]);
    int nz = atoi(argv[4]);
    array.generateMatrix(nx, ny, nz);
  } else {
    array.readMatrix(argv[2]);
  }
  
  delete msg;

  thisProxy.start();
}

void charmHpccg::findExternals() {
  map<int, int> externals;
  identify_externals(A, externals);

  for (map<int, int>::iterator iter = externals.begin(); iter != externals.end(); ++iter) {
    int row = iter->first;
    int chunksize = A->total_nrow / numChares;
    int remainder = A->total_nrow % numChares;
    int cutoffRow = (chunksize + 1) * remainder;

    int chare;
    int remoteOffset;
    if (row < cutoffRow) {
      chare     = row / (chunksize + 1);
      remoteOffset = row % (chunksize + 1);
    } else {
      int rowsPastCutoff = row - cutoffRow;
      chare = remainder + rowsPastCutoff / chunksize;
      remoteOffset = rowsPastCutoff % chunksize;
    }

    xToReceive[chare].rows.push_back(row);
    xToReceive[chare].values.push_back(remoteOffset);
  }

  map<int, int> row_to_offset;

  int offset = A->local_nrow;
  for(map<int, RemoteX>::iterator iter = xToReceive.begin();
      iter != xToReceive.end(); ++iter) {
    iter->second.offset = offset;
    thisProxy[iter->first].needXElements(thisIndex, iter->second.values);

    for (int i = 0; i < iter->second.values.size(); ++i) {
      int row = iter->second.rows[i];
      row_to_offset[row] = offset + i;
    }

    offset += iter->second.values.size();
  }

  for (int i = 0; i < A->local_nrow; i++) {
    for (int j = 0; j < A->nnz_in_row[i]; j++) {
      if (A->ptr_to_inds_in_row[i][j] < 0) { // Change index values of externals
        int cur_ind = - A->ptr_to_inds_in_row[i][j] - 1;
        CkAssert(row_to_offset.find(cur_ind) != row_to_offset.end());
        A->ptr_to_inds_in_row[i][j] = row_to_offset[cur_ind];
      }
    }
  }

  detector.ckLocalBranch()->produce(xToReceive.size());
  detector.ckLocalBranch()->done();
}

void charmHpccg::needXElements(int requester, vector<int> rows) {
  xToSend[requester].values = rows;
  detector.ckLocalBranch()->consume();
}

#include "charmHpccg.def.h"
