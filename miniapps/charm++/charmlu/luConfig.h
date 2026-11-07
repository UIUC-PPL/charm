#ifndef LU_CONFIG_H
#define LU_CONFIG_H

#include <pup.h>
#include <charm++.h>

/// Class the stores configurations relevant to the LU factorization library
class LUConfig {
public:
  LUConfig()
    : blockSize(0)
    , numBlocks(0)
    , pivotBatchSize(0)
    , peTileRows(0)
    , peTileCols(0)
    , peTileRotate(0)
    , peTileStride(0)
    , memThreshold(0)
    , mappingScheme(0)
  { }

  /// Serialization function
  void pup(PUP::er &p) {
    p | matrixSize;
    p | blockSize;
    p | numBlocks;
    p | pivotBatchSize;
    p | map;
    p | peTileRows;
    p | peTileCols;
    p | peTileRotate;
    p | peTileStride;
    p | memThreshold;
    p | mappingScheme;
    p | mcastMgrGID;
  }

  /// The size of the matrix
  int matrixSize;
  /// The size of each square block of the matrix
  int blockSize;
  /// The number of blocks comprising the whole square matrix
  int numBlocks;
  /// The number of pivots to agglomerate into one batch
  int pivotBatchSize;
  /// The mapping logic that has to be used
  CkGroupID map;
  /// The num of PEs in the PEtile
  int peTileRows;
  /// The num of rows in the PEtile
  int peTileCols;
  /// The amount of rotation in the PEtile
  int peTileRotate;
  /// The stride between PEs on a column
  int peTileStride;
  /// The max memory (in MB) that the adaptive RTS should limit the app to
  int memThreshold;
  /// The mapping scheme
  int mappingScheme;
  /// The group ID of the multicast manager
  CkGroupID mcastMgrGID;
};

#endif // LU_CONFIG_H

