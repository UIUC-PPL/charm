#!/usr/bin/python

import math

# Fraction of memory that we want to fill
memFractionMin = 0.71
memFractionMax = 0.77
# Range of processor counts that we want to try and run on
peMin          = 120
peMax          = 150
# Range of permissible block sizes
blockSizeMin   = 490
blockSizeMax   = 510
# Range of permissible aspect ratios
aspectMin      =  0.9
aspectMax      =  6.0


#### Machine configs
# The num cores per node
coresPerNode   = 12
# Mem per node in GB
memPerNode     = 16.0
# USeful constants
GBperDouble    = 8.0 / 1024 / 1024 / 1024;

#### Compute the ideal block sizes and size ranges
memPerCoreMin = (memFractionMin * memPerNode) / coresPerNode
memPerCoreMax = (memFractionMax * memPerNode) / coresPerNode
dblPerCoreMin = memPerCoreMin / GBperDouble
dblPerCoreMax = memPerCoreMax / GBperDouble



# Generate factors of n with the inclusive [minFact, maxFact] range
def factors(n, minFact=2, maxFact=None):
    if maxFact is None: maxFact = n/2
    fact=[]
    limL = max(minFact, 2)
    limU = min(maxFact, int(n/2))
    for f in range(limL, limU+1):
        if n % f == 0: fact.append(f)
    return fact



# Check if a given input combination is valid
def isInputValid(mtxSz, blkSz, tileX, tileY):
    nBlks = mtxSz / blkSz
    if nBlks % tileX != 0: return False
    if nBlks % tileY != 0: return False
    if tileX * tileY % coresPerNode != 0: return False

    aspectRatio= 1.0*tileX/tileY
    if (aspectRatio < aspectMin or aspectRatio > aspectMax): return False

    return True


# Generate valid input combinations within specified ranges
def generateInputParams():
    print "# %%Mem AspectRatio\tPE  matrixSize(N) blockSize tileX x tileY numBlocks"
    for numPes in range(peMin, peMax+1):
        # Compute the range of matrix sizes permissible for this num PEs
        matrixSizeMin = int( math.sqrt(dblPerCoreMin * numPes) )
        matrixSizeMax = int( math.sqrt(dblPerCoreMax * numPes) )
        # Compute the possible tile shapes
        tileDimCandidates = factors( numPes, int(math.sqrt(numPes)) )
        for N in range(matrixSizeMin, matrixSizeMax+1):
            blockSizeCandidates = factors(N, blockSizeMin, blockSizeMax)
            for blkSize in blockSizeCandidates:
                for x in tileDimCandidates:
                    g = max(x,numPes/x)
                    l = min(x,numPes/x)
                    if isInputValid(N, blkSize, g, l):
                        aspectRatio= 1.0*g/l
                        memPerCore = (N*N*GBperDouble) / numPes
                        memFraction= memPerCore * coresPerNode / memPerNode
                        print "%2.2f%% %2.2f \t%7d %9d %4d (%4d,%4d)\t%4d" % (memFraction*100, aspectRatio, numPes, N, blkSize, g, l, N/blkSize)


# Execute this script
generateInputParams()

