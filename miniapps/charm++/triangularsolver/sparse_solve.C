/*
 author: Ehsan Totoni, totoni2@illinois.edu
 December, 2011
 */

#include <stdio.h>
#include <vector>
#include <cmath>
#include <limits>
#include "charm++.h"
using namespace std;
struct rowAttr {
// no. of chare waiting and local row index in that chare
	int chare, row; 
	void pup(PUP::er &p) { p|chare; p|row;}
};

struct rowSum { int row; double val; rowSum(int r, double d):row(r),val(d){} rowSum(){} 
void pup(PUP::er &p) { p|row; p|val;}
};

#include "sparse_solve.decl.h"
#include "MessagePool.h"
#include "ckmulticast.h"

// right hand side is a simple vector full of a constant
#define B_CONST 1.0
#define MIN_ENTRIES_PER_X 20

/*readonly*/ CProxy_Main mainProxy;

class xValMsg : public CkMcastBaseMsg, public CMessage_xValMsg
{ public: double *xVal; };

#include "ColumnsSolve.h"

// for each block chare keeps deps
struct chareDepsStr {
	int chareNo; 		// no. of chare
	rowAttr* nextRow; 	// next of each row
	int size;
};

/*mainchare*/
class Main : public CBase_Main {
	CProxy_ColumnsSolve arr;
	double startTime;
	// total columns of matrix, all chares before and after creation
	int totalColumns, totalChares, nElements;
	int *allMap;
	char fileName[250];
	double* data; // data in columns sparse compressed
	int* rowInd;  // row index of each element
	int *colsInd; // index of each column in data array, one more than columns for convenience
public:
	Main(CkArgMsg* m)
	{
		//Process command-line arguments
		if(m->argc < 3) {
			CkPrintf("Arguments: numChares fileName\n");
			CkExit();
		}
		nElements = atoi(m->argv[1]);
		strcpy(fileName, m->argv[2]);
		
		//Start the computation
		CkPrintf("Running ColumnsSolve on %d processors for %d elements\n", CkNumPes(), nElements);
		mainProxy = thisProxy;
		//	CProxy_BlockMap myMap=CProxy_BlockMap::ckNew();
		CProxy_RRMap myMap=CProxy_RRMap::ckNew();

		//Make a new array using that map
		CkArrayOptions opts(nElements);
		opts.setMap(myMap);
		arr = CProxy_ColumnsSolve::ckNew(opts);
		setupInput(fileName);

	};

	void setupInput(char* fileName) {
		// read input
		int m, nzl;
		readInput(fileName, data, rowInd, colsInd, m, totalColumns, nzl);

		// number of local columns
		int numLocCols = (totalColumns/nElements);
		int* lastRowInd = new int[m];
		memset(lastRowInd, 0, m*sizeof(int));
		
		double* tmpData = new double[nzl]; // too much memory!!!!
		int* tmpCol = new int[nzl];
		int* tmpRow = new int[m+1];
		bool* tmpDep = new bool[m];
		int chareNo = nElements;
		int *mapRows = allMap = new int[m];

		vector<chareDepsStr> chareDeps; 	// nexts for all block chares
		rowAttr* prevInRow = new rowAttr[m]; 	// previous chare in that row
		for (int i=0; i<m; i++) prevInRow[i].chare = -1;

		int offdiags=0, diagdeps=0;
		CkGroupID mCastGrpId = CProxy_CkMulticastMgr::ckNew();
		
		for (int i=0; i<nElements; i++) {
			int lastNoDiagChare = chareNo;

			// first column
			int startCol = i*numLocCols;
			int endCol = (i+1)*numLocCols;
			if (i==nElements-1)
				endCol = totalColumns;

			// min entries: constant times number of x values
			int minEntries = MIN_ENTRIES_PER_X*(endCol-startCol);
			int indepRowNo=0;
			
			reorder(mapRows, indepRowNo, startCol, endCol, prevInRow,
					rowInd, colsInd, data, tmpData, tmpRow, tmpCol, tmpDep, chareDeps, i, lastRowInd);

			// last row of next diagonal chare
			int nextChareLastRow = endCol + (endCol-startCol);
			if (i>=nElements-2)
				nextChareLastRow = totalColumns;
			int currRow = endCol;

			// get size of below diagonal corresponding to next diagonal Chare
			int belowSize = getBelowSize(rowInd, colsInd, endCol, endCol, nextChareLastRow, lastRowInd);
			int entries = tmpRow[endCol-startCol];
			int firstBelowChunkRows = 0; 	// number of rows of first chunk below diagonal
			int firstBelowChunkMaxCol = 0; 	// last needed column (X value) for this part
			int restBelowChunkRows = 0; 	// number of rows after first chunk below diagonal
			int restBelowChunkMaxCol = 0; 	// last needed column (X value) for this part
			if (belowSize<minEntries && belowSize!=0) {				
				addBelowRows(firstBelowChunkRows, firstBelowChunkMaxCol, entries, rowInd, colsInd, data, tmpRow,
							 tmpCol, tmpData, tmpDep, startCol, endCol, endCol, nextChareLastRow, 
							 lastRowInd, mapRows, chareDeps, prevInRow, i);
				offdiags += belowSize;
				currRow = nextChareLastRow;

				// rest of rows
				belowSize = getBelowSize(rowInd, colsInd, endCol, nextChareLastRow, totalColumns, lastRowInd);
				if (belowSize<minEntries && belowSize!=0) {
					restBelowChunkRows = firstBelowChunkRows; // for rowInd calcs
					addBelowRows(restBelowChunkRows, restBelowChunkMaxCol, entries, rowInd, colsInd, data, tmpRow,
								 tmpCol, tmpData, tmpDep, startCol, endCol, nextChareLastRow, totalColumns,
								 lastRowInd, mapRows, chareDeps, prevInRow, i);
					restBelowChunkRows -= firstBelowChunkRows;
					offdiags += belowSize;
					currRow = totalColumns;
				}
				if (belowSize==0)
					currRow = totalColumns;
			}
			
			chareDepsStr newChare; newChare.size=firstBelowChunkRows+restBelowChunkRows; newChare.chareNo=i;
			newChare.nextRow = new rowAttr[firstBelowChunkRows+restBelowChunkRows];
			chareDeps.push_back(newChare);
			
			int myRows = endCol-startCol+firstBelowChunkRows+restBelowChunkRows;
			
			arr[i].getInput(entries, myRows, endCol-startCol, tmpData, tmpCol, tmpRow, tmpDep, true, indepRowNo, 
							 firstBelowChunkRows, firstBelowChunkMaxCol, restBelowChunkRows, restBelowChunkMaxCol);
			
			int depCols = endCol-startCol-indepRowNo;

			diagdeps += depCols;
			
			while (currRow!=m) {
				int tmpCurrRow = 0;
				entries = 0;
				while (entries<minEntries) {
					
					tmpRow[tmpCurrRow] = entries;
					int j = rowInd[currRow]+lastRowInd[currRow];
					while (colsInd[j]<endCol) {
						tmpData[entries] = data[j];
						tmpCol[entries] = mapRows[colsInd[j]-startCol];
						j++;
						entries++;
					}

					// if empty go to next row
					if (entries==tmpRow[tmpCurrRow]) {
						currRow++;
						if (currRow==m) break;
						continue;
					}
					lastRowInd[currRow] = j-rowInd[currRow];
					tmpDep[tmpCurrRow] = false;

					// if row is not empty, this is the last
					// set dependency of previous block chare
					int lastChare = prevInRow[currRow].chare;
					if (lastChare!=-1) {
						for (int k=0; k<chareDeps.size(); k++) {
							if (chareDeps[k].chareNo==lastChare) {
								rowAttr tmprattr; tmprattr.chare=chareNo; tmprattr.row=tmpCurrRow;
								chareDeps[k].nextRow[prevInRow[currRow].row] = tmprattr;
							}
						}
						tmpDep[tmpCurrRow] = true;
					}
					rowAttr rtr; rtr.chare= chareNo; rtr.row=tmpCurrRow;
					prevInRow[currRow] = rtr;
					
					currRow++;
					tmpCurrRow++;
					
					if (currRow==m) break;
					
				}
				// empty lower diagonal
				if (entries==0) break;
				
				offdiags += entries;
				// CkPrintf("chare:%d nonzeros:%d\n",chareNo, entries);
				tmpRow[tmpCurrRow] = entries;
				chareDepsStr newChare; newChare.size=tmpCurrRow; newChare.chareNo=chareNo;
				newChare.nextRow = new rowAttr[tmpCurrRow];
				chareDeps.push_back(newChare);
				arr[chareNo].insert();
				arr[chareNo++].getInput(entries, tmpCurrRow, numLocCols, tmpData, tmpCol, tmpRow, tmpDep, false, 0,0,0, 0,0);
			}
			// send section proxy to diag element
			CProxySection_ColumnsSolve nondiags = CProxySection_ColumnsSolve::ckNew(arr, lastNoDiagChare, chareNo-1, 1);
			bool emptyNondiags = lastNoDiagChare == chareNo;
			mapRows += numLocCols;
			arr[i].getSection(nondiags, emptyNondiags, mCastGrpId);
		}
		for (int i=0; i<chareDeps.size(); i++) 
			arr[chareDeps[i].chareNo].getDeps(chareDeps[i].size, chareDeps[i].nextRow);
		arr.doneInserting();
//		CkPrintf("offdiags:%d out of %d nonzeros, fraction:%f\n",offdiags, nzl, offdiags/(double)nzl);
//		CkPrintf("diagdeps:%d out of %d rows, fraction:%f\n",diagdeps, m, diagdeps/(double)m);
		totalChares = chareNo;
		arr.init();
		delete[] lastRowInd;
		delete[] tmpRow;
		delete[] tmpData;
		delete[] tmpCol;
		delete[] tmpDep;
		delete[] data;
		delete[] rowInd;
		delete[] colsInd;
		delete[] prevInRow;
	}

	void reportIn() {
		CkPrintf("All done in %f\n",CmiWallTimer()-startTime);
		arr.sendResults();
	}

	void initDone(void) {
		arr.start();
		startTime = CmiWallTimer();
	}
	
	void validate(CkReductionMsg* msg) {
		double *xVals = (double*) msg->getData();
		// read input again
		int m, nzl, numLocCols = totalColumns/nElements;
		readInput(fileName, data, rowInd, colsInd, m, totalColumns, nzl);
		// keep track of 3 infinity norms
		double maxX = 0, maxRowsum = 0, maxResult = 0;
		// for each chare
		for (int i=0; i<nElements; i++) {
			int first_col = i*numLocCols;
			int last_col = (i==nElements-1) ? totalColumns : (i+1)*(totalColumns/nElements);
			// for each x value
			for (int j=first_col; j<last_col; j++) {
				
				double sum=0, rowsum=0;
				for (int k=rowInd[j]; k<rowInd[j+1]; k++) {
					// find index of required x in new order using global map
					int colindStart = min((nElements-1)*numLocCols,(colsInd[k]/numLocCols)*numLocCols);
					int xindex = colindStart + allMap[colindStart+(colsInd[k]-colindStart)];
					sum += xVals[xindex]*data[k];
					rowsum += abs(data[k]);
				}
				maxX = max(xVals[j],maxX);
				maxRowsum = max(rowsum,maxRowsum);
				maxResult = max(sum-B_CONST,maxResult);
			}
		}
		CkPrintf("residual=%lf \n",maxResult/((maxRowsum*maxX+B_CONST)*totalColumns*numeric_limits<double>::epsilon()) );
		CkExit();
	}

	void reorder(int *mapRows, int& noIndeps, int startCol, int endCol, rowAttr* prevInRow, int* rowInd,
			int* colInd, double* data, double* tmpData, int* tmpRow, int* tmpCol,
			bool* tmpDep, vector<chareDepsStr> &chareDeps, int chareNo, int *lastRowInd) {
	
		int size = endCol-startCol;
		// depend on messages
		bool *markDep = new bool[size];
		memset(markDep, 0, size*sizeof(bool));
		// done reordering
		bool *markDone = new bool[size];
		memset(markDone, 0, size*sizeof(bool));
		
		// find dependents
		for (int i=startCol; i<endCol; i++) {
			if (prevInRow[i].chare!=-1) { // receives message
				markDep[i-startCol] = true;
				continue;
			}
			for (int j=rowInd[i+1]-2; j>=rowInd[i]+lastRowInd[i]; j--) {
				// depends on dependant row
				if (markDep[colInd[j]-startCol]) {
					markDep[i-startCol] = true;
					break;
				}
			}
		}
		// place non-deps
		int targetNzls = 0;
		int targetRow = 0;
		tmpRow[0]=0;
		for (int i=endCol-1; i>=startCol; i--) {
			if (!markDep[i-startCol] && !markDone[i-startCol])
				place(i, targetRow, targetNzls, mapRows, startCol, rowInd,
					  colInd, data, tmpData, tmpRow, tmpCol, markDone, lastRowInd);
		}
		// number of independent rows
		noIndeps = targetRow;
		
		// place dependents
		for (int i=startCol; i<endCol; i++) {
			if (markDep[i-startCol]) {
				mapRows[i-startCol] = targetRow;
				for (int j=rowInd[i]+lastRowInd[i]; j<rowInd[i+1]; j++) {
					tmpData[targetNzls] = data[j];
					tmpCol[targetNzls] = mapRows[colInd[j]-startCol];
					targetNzls++;
				}
				tmpRow[targetRow+1] = targetNzls;
				if (prevInRow[i].chare!=-1) {
					tmpDep[targetRow-noIndeps] = true;
					for (int k=0; k<chareDeps.size(); k++) {
						if (chareDeps[k].chareNo==prevInRow[i].chare) {
							rowAttr tmprattr; tmprattr.chare=chareNo; tmprattr.row=targetRow;
							chareDeps[k].nextRow[prevInRow[i].row] = tmprattr;
						}
					}
				} else	tmpDep[targetRow-noIndeps] = false;

				targetRow++;
			}
		}
		
	}
	void place(int row, int& targetRow, int& targetNzls, int *mapRows, int startCol, int* rowInd,
			   int* colInd, double* data,double *tmpData,int* tmpRow,int* tmpCol, bool* markDone, int *lastRowInd) {
		// place dependencies
		int j=rowInd[row+1]-2;
		for (; j>=rowInd[row]+lastRowInd[row]; j--) {
			if (colInd[j]<startCol || colInd[j]==row) continue;
			
			if (!markDone[colInd[j]-startCol]) {
				place(colInd[j], targetRow, targetNzls, mapRows, startCol, rowInd,
					  colInd, data, tmpData, tmpRow, tmpCol, markDone, lastRowInd);
			}
		}
		mapRows[row-startCol] = targetRow;
		markDone[row-startCol] = true;
		
		for (int j= rowInd[row]+lastRowInd[row]; j<rowInd[row+1]; j++) {
			tmpData[targetNzls] = data[j];
			tmpCol[targetNzls] = mapRows[colInd[j]-startCol];
			targetNzls++;
		}
		tmpRow[targetRow+1] = targetNzls;
		targetRow++;
			
	}
	int getBelowSize(int *rowInd, int* colInd,int endCol,int nextChareFirstRow, int nextChareLastRow, int* lastRowInd) {
		int size=0;
		for (int i=nextChareFirstRow; i<nextChareLastRow; i++) {
			int j=rowInd[i]+lastRowInd[i];
			while (colInd[j]<endCol) {
				j++;
				size++;
			}
		}
		return size;
	}
	void addBelowRows(int &rowNo, int &maxCol, int& entries, int* rowInd, int* colInd, double* data, int* tmpRow,
					  int* tmpCol, double* tmpData, bool* tmpDep, int startCol,int endCol, int nextChareFirstRow, int nextChareLastRow, 
					  int* lastRowInd, int* mapRows, vector<chareDepsStr> &chareDeps, rowAttr* prevInRow, int thisChare) {
		maxCol = 0;
		tmpRow[endCol-startCol+rowNo] = entries;
		for (int i=nextChareFirstRow; i<nextChareLastRow; i++) {
			int j=rowInd[i]+lastRowInd[i];
			while (colInd[j]<endCol) {
				tmpData[entries] = data[j];
				tmpCol[entries] = mapRows[colInd[j]-startCol];
				if (tmpCol[entries]>maxCol)
					maxCol = tmpCol[entries];
				entries++;
				j++;
			}
			// if row was empty
			if (j==rowInd[i]+lastRowInd[i]) continue;
			
			if (lastRowInd[i]!=0) {
				tmpDep[endCol-startCol+rowNo] = true;
				setDependentChare(chareDeps, prevInRow[i], thisChare, endCol-startCol+rowNo);
			}
			else tmpDep[endCol-startCol+rowNo] = false;
			
			lastRowInd[i] = j-rowInd[i];
			rowAttr rtr; rtr.chare= thisChare; rtr.row=rowNo;
			prevInRow[i] = rtr;
			rowNo++;
			tmpRow[endCol-startCol+rowNo] = entries;
		}
	}

	void setDependentChare(vector<chareDepsStr> &chareDeps, rowAttr prevRow, int thisChare, int rowNo)
	{
		for (int k=0; k<chareDeps.size(); k++)
			if (chareDeps[k].chareNo==prevRow.chare) {
				rowAttr tmprattr; tmprattr.chare=thisChare; tmprattr.row=rowNo;
				chareDeps[k].nextRow[prevRow.row] = tmprattr;
			}
	}

	void readInput(char * fileName, double * & val, int * &rowind,int * & colptr, int & m, int&  n, int & nzl){

		FILE * fp= fopen(fileName, "r");
		if(fp==NULL)
			printf("file read error!\n");
		/*first line */
		fscanf(fp,"%d %d %d", &m, &n, &nzl);

		val = new double[nzl];
		rowind = new int[m+1];
		colptr = new int[nzl];
		/* second line */
		for(int i=0;i<m+1;i++)
			fscanf(fp,"%d",&rowind[i]);
		/*third line */
		for(int i=0;i<nzl;i++)
			fscanf(fp,"%d",&colptr[i]);
		/* fourth line */
		for(int i=0;i<nzl;i++)
			fscanf(fp,"%lf",&val[i]);
		fclose(fp);
	}
};

#include "sparse_solve.def.h"
