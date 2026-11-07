/*
 author: Ehsan Totoni, totoni2@illinois.edu
 December, 2011
 */

#define MAX(a, b) (a>=b?a:b)
#include <assert.h>
#include <vector>
using namespace std;

/*array [1D]*/
class ColumnsSolve : public CBase_ColumnsSolve
{
	ColumnsSolve_SDAG_CODE;
	
	MessagePool msgPool;
	
	int* rowInd;  // index of each row, one more for convenience
	double* data; // data in rows sparse compressed
	int* colInd; // column index of each element
	
	int nMyCols, nMyRows;  // number of columns and rows of this chare
	bool onDiagonalChare; // is diagonal Chare
	int indepRowNo, firstBelowRows, firstBelowMaxCol, restBelowRows, restBelowMaxCol;
	bool firstBelowDone, restBelowDone;
	
	int allDone, belowNumDone; // done rows
	bool* rowDep; // is each row dependent
	CProxySection_ColumnsSolve lowerSection; // lower diagonal chares, for diags
	rowAttr* nextRow; // for nondiags
	
	bool finished, emptyNondiags, xvalGot;
	xValMsg* xvalMsg;
	
	double *arrivedData;
	int arrivedSize;
	bool *arrivedIs; // for diagonal, is item arrived
	
	double* xVal; // values of x
	double* rhs; // right-hand side for each column if diagonal chare

public:
	ColumnsSolve():msgPool(thisProxy) {
		arrivedSize = allDone = belowNumDone=0;
		finished=emptyNondiags=firstBelowDone=restBelowDone=xvalGot=false;
	}
	ColumnsSolve(CkMigrateMessage *m):msgPool(thisProxy) {}
	
	void setInput(int num_entries, int num_rows, int num_cols, double m_data[], int m_colInd[], 
				   int m_rowInd[],	bool dep[],	bool m_diag, int m_indepRowNo, int m_firstBelowRows,
				   int m_firstBelowMaxCol, int m_restBelowRows, int m_restBelowMaxCol) {
		data = new double[num_entries];
		colInd = new int[num_entries];
		rowInd = new int[num_rows+1];
		rowDep = new bool[num_rows];
		memcpy(data, m_data, num_entries*sizeof(double));
		memcpy(colInd, m_colInd, num_entries*sizeof(int));
		memcpy(rowInd, m_rowInd, (num_rows+1)*sizeof(int));
		memcpy(rowDep, dep, (num_rows)*sizeof(bool));
		nMyCols = num_cols;
		nMyRows = num_rows;
		onDiagonalChare = m_diag;
		indepRowNo = m_indepRowNo;
		xVal = new double[nMyCols];
		if (onDiagonalChare) {
			rhs = new double[nMyCols];
			for (int i=0; i<nMyCols; i++) {
				// simple right hand side
				rhs[i] = B_CONST;
				// invert diagonals to replace division with multiply
				data[rowInd[i+1]-1] = 1.0/data[rowInd[i+1]-1];
			}
			arrivedIs = new bool[nMyRows];
			memset(arrivedIs, 0, nMyRows*sizeof(bool));
			firstBelowRows = m_firstBelowRows;
			firstBelowMaxCol = m_firstBelowMaxCol;
			restBelowRows = m_restBelowRows;
			restBelowMaxCol = m_restBelowMaxCol;
			nextRow = new rowAttr[firstBelowRows+restBelowRows];
			if (!emptyNondiags) {
				xvalMsg = new (nMyCols, 8*sizeof(int)) xValMsg;
				*(int*)CkPriorityPtr(xvalMsg) = 0;
				CkSetQueueing(xvalMsg, CK_QUEUEING_IFIFO);
				delete[] xVal;
				xVal = xvalMsg->xVal;
			}
			
		} else nextRow = new rowAttr[nMyRows];
		
		arrivedData = new double[nMyRows];
	}
	void setDeps(int dep_size, rowAttr deps[]) {
		memcpy(nextRow, deps, (dep_size)*sizeof(rowAttr));
	}
	
	void setSection(CProxySection_ColumnsSolve nondiags, bool empty_sec, CkGroupID mCastGrpId) {
		lowerSection = nondiags;
		emptyNondiags = empty_sec;
		if (!emptyNondiags)
			lowerSection.ckSectionDelegate(CProxy_CkMulticastMgr(mCastGrpId).ckLocalBranch());
	}
	
	void myIndepCompute() {
		if (indepRowNo) {
			int i=0;
			// get next chares data faster
			if (firstBelowMaxCol < indepRowNo) {
				for (; i<=firstBelowMaxCol; i++) {
					double val = 0;
					for (int j=rowInd[i]; j<rowInd[i+1]-1; j++)
						val += data[j]*xVal[colInd[j]];
					xVal[i] = (rhs[i]-val)*(data[rowInd[i+1]-1]);
				}
				for (int i=nMyCols; i<nMyCols+firstBelowRows; i++) {
					double val=0;
					if (rowDep[i]) {
						if (arrivedIs[i]) {
							val = arrivedData[i];
							arrivedIs[i] = false;
						} else continue;
					}
					
					for (int j=rowInd[i]; j<rowInd[i+1]; j++)
						val += data[j]*xVal[colInd[j]];
					msgPool.add(nextRow[i-nMyCols].chare, nextRow[i-nMyCols].row, val);
					belowNumDone++;
				}
				firstBelowDone = true;
				msgPool.flushMsgPool();
			}
			// get other chares data
			if (restBelowMaxCol < indepRowNo) {
				for (; i<=restBelowMaxCol; i++) {
					double val = 0;
					for (int j=rowInd[i]; j<rowInd[i+1]-1; j++)
						val += data[j]*xVal[colInd[j]];
					xVal[i] = (rhs[i]-val)*(data[rowInd[i+1]-1]);
				}
				for (int i=nMyCols+firstBelowRows; i<nMyCols+firstBelowRows+restBelowRows; i++) {
					double val=0;
					if (rowDep[i]) {
						if (arrivedIs[i]) {
							val = arrivedData[i];
							arrivedIs[i] = false;
						} else continue;
					}
					
					for (int j=rowInd[i]; j<rowInd[i+1]; j++)
						val += data[j]*xVal[colInd[j]];
					msgPool.add(nextRow[i-nMyCols].chare, nextRow[i-nMyCols].row, val);
					belowNumDone++;
				}
				restBelowDone = true;
				msgPool.flushMsgPool();
			}
			for (; i<indepRowNo; i++) {
				double val = 0;
				for (int j=rowInd[i]; j<rowInd[i+1]-1; j++)
					val += data[j]*xVal[colInd[j]];
				xVal[i] = (rhs[i]-val)*(data[rowInd[i+1]-1]);
			}
			allDone = indepRowNo;
			diag_compute(allDone);
			// send dummy message for sdag while to complete
			if (finished) thisProxy[thisIndex].receiveData(0,NULL,NULL);
		}
	}
	void diag_compute(int start) {
		// if hadn't started yet
		if (allDone<indepRowNo)
			return;
		int i=start;
		for (; i<=firstBelowMaxCol; i++) {
			double val = 0;
			if (rowDep[i-indepRowNo]) {
				if (arrivedIs[i])
					val = arrivedData[i];
				else break;
			}
			for (int j=rowInd[i]; j<rowInd[i+1]-1; j++)
				val += data[j]*xVal[colInd[j]];
			xVal[i] = (rhs[i]-val)*(data[rowInd[i+1]-1]);
		}
		if (i> firstBelowMaxCol && !firstBelowDone) {
			for (int i=nMyCols; i<nMyCols+firstBelowRows; i++) {
				double val=0;
				if (rowDep[i]) {
					if (arrivedIs[i]) {
						val = arrivedData[i];
						arrivedIs[i] = false;
					} else continue;
				}
				
				for (int j=rowInd[i]; j<rowInd[i+1]; j++)
					val += data[j]*xVal[colInd[j]];
				msgPool.add(nextRow[i-nMyCols].chare, nextRow[i-nMyCols].row, val);
				belowNumDone++;
			}
			firstBelowDone = true;
			msgPool.flushMsgPool();
		}
		for (; i<=restBelowMaxCol; i++) {
			double val = 0;
			if (rowDep[i-indepRowNo]) {
				if (arrivedIs[i])
					val = arrivedData[i];
				else break;
			}
			for (int j=rowInd[i]; j<rowInd[i+1]-1; j++)
				val += data[j]*xVal[colInd[j]];
			xVal[i] = (rhs[i]-val)*(data[rowInd[i+1]-1]);
		}
		if (i> restBelowMaxCol && !restBelowDone) {
			for (int i=nMyCols+firstBelowRows; i<nMyCols+firstBelowRows+restBelowRows; i++) {
				double val=0;
				if (rowDep[i]) {
					if (arrivedIs[i]) {
						val = arrivedData[i];
						arrivedIs[i] = false;
					} else continue;
				}
				
				for (int j=rowInd[i]; j<rowInd[i+1]; j++)
					val += data[j]*xVal[colInd[j]];
				msgPool.add(nextRow[i-nMyCols].chare, nextRow[i-nMyCols].row, val);
				belowNumDone++;
			}
			restBelowDone = true;
			msgPool.flushMsgPool();
		}
		
		for (; i<nMyCols; i++) {
			double val = 0;
			if (rowDep[i-indepRowNo]) {
				if (arrivedIs[i])
					val = arrivedData[i];
				else break;
			}
			for (int j=rowInd[i]; j<rowInd[i+1]-1; j++)
				val += data[j]*xVal[colInd[j]];
			xVal[i] = (rhs[i]-val)*(data[rowInd[i+1]-1]);
		}
		allDone = i;
		
		if (allDone==nMyCols && !finished) {
			
			if (!emptyNondiags) {
				emptyNondiags = true; // don't send again!
				// broadcast to other chares of column
				lowerSection.getXvals(xvalMsg);
			}
			if (belowNumDone==firstBelowRows+restBelowRows)
				finished = true;
		}
	}

	void nondiagCompute() {
		for (int i=0; i<nMyRows; i++) {
			if (rowDep[i])
				continue;
			double val=0;
			for (int j=rowInd[i]; j<rowInd[i+1]; j++)
				val += data[j]*xVal[colInd[j]];
			//			streamer->insertData(RowSum(nextRow[i].row, val), nextRow[i].chare);
			msgPool.add(nextRow[i].chare, nextRow[i].row, val);
			allDone++;			
		}
		msgPool.flushMsgPool();
		if (allDone==nMyRows && !finished)
			finished = true;
	}

	void diagReceiveData(int m_size, double m_data[], int m_rowInd[])
	{
		for (int i=0; i<m_size; i++) {
			double val = m_data[i];
			int row = m_rowInd[i];
			if (row==allDone && row<nMyCols) {
				for (int j=rowInd[row]; j<rowInd[row+1]-1; j++)
					val += data[j]*xVal[colInd[j]];
				xVal[row] = (rhs[row]-val)*(data[rowInd[row+1]-1]);
				// first depending row is done
				allDone++;
				diag_compute(allDone);
			}
			// if after diagonal 
			else if (row>=nMyCols && ( (row<nMyCols+firstBelowRows && allDone>firstBelowMaxCol)
									  || (row>=nMyCols+firstBelowRows && allDone>restBelowMaxCol))) {
				for (int j=rowInd[row]; j<rowInd[row+1]; j++)
					val += data[j]*xVal[colInd[j]];
				msgPool.add(nextRow[row-nMyCols].chare, nextRow[row-nMyCols].row, val);
				belowNumDone++;
			} 
			else {
				arrivedData[row] = val;
				arrivedIs[row] = true;
			}	
		}
		msgPool.flushMsgPool();
		diag_compute(allDone);
	}
	void nondiagReceiveData(int m_size, double m_data[], int m_rowInd[]){
		for (int i=0; i<m_size; i++) {
			double val = m_data[i];
			int row = m_rowInd[i];
			for (int j=rowInd[row]; j<rowInd[row+1]; j++)
				val += data[j]*xVal[colInd[j]];
			msgPool.add(nextRow[row].chare, nextRow[row].row, val);
			allDone++;
		}
		msgPool.flushMsgPool();
		if (allDone==nMyRows)
			finished = true;
	}

	void sendResults() {
		contribute(nMyCols*sizeof(double),xVal,CkReduction::concat, CkCallback(CkIndex_Main::validate(NULL), mainProxy));
	}
};
