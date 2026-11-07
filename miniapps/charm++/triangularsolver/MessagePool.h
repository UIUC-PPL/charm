/*
 author: Ehsan Totoni, totoni2@illinois.edu
 December, 2011
 */

#define NO_MSG_SEGS 5
#define MSG_SEG_SIZE 100000

class MessagePool {
	CProxy_ColumnsSolve thisProxy;
	
	// message pool data structures
	double* out_data; // store data to be sent out
	// pre-allocated segments of size MSG_SEG_SIZE for NO_MSG_SEGS PEs
	int* out_rowInd; // row index of each element
	int *out_colsInd; // column index of each element in data array
	int* out_size;    // data size of each processor
	int* index_out; // mapping of processor number to out data structure's index
	// last chare accessed, for faster search, mapped index, number of segments occupied
	int lastChare, lastChare_ind, no_pe_segs;

public:
	MessagePool(CProxy_ColumnsSolve p) {
		thisProxy = p;
		out_data = new double[NO_MSG_SEGS*MSG_SEG_SIZE];
		out_rowInd = new int[NO_MSG_SEGS*MSG_SEG_SIZE];
		out_size = new int[NO_MSG_SEGS];
		memset(out_size, 0, NO_MSG_SEGS*sizeof(int));
		index_out = new int[NO_MSG_SEGS];
		for (int i=0; i<NO_MSG_SEGS; i++)
			index_out[i] = -1;
		lastChare = lastChare_ind = -1;
		no_pe_segs = 0;
	}
	
	void add(int chare_no, int row, double val) {
		
		int chare_index, out_ind;
		// find PE
		if (chare_no==lastChare && no_pe_segs) { // last chare and not flushed
			chare_index = lastChare_ind;
		} else {
			int i;
			for (i=0; i<no_pe_segs; i++)
				if (index_out[i]==chare_no) {
					chare_index = i;
					break;
				}
			// if PE is not allocated
			if (i==no_pe_segs) {
				chare_index = allocMsgSeg();
				index_out[chare_index] = chare_no;
			}
		}
		lastChare = chare_no;
		lastChare_ind = chare_index;
		out_ind = out_size[chare_index]++;
		out_ind += chare_index*MSG_SEG_SIZE;
		out_data[out_ind] = val;
		out_rowInd[out_ind] = row;
		// check for full buffer
		if (out_size[lastChare_ind]==MSG_SEG_SIZE)
			flushMsgPool();
	}
	int allocMsgSeg() {
		int PE_index = -1;
		if (no_pe_segs!=NO_MSG_SEGS) {
			/// initialize---------
			PE_index = no_pe_segs++;
		} else {
			flushMsgPool();
			PE_index = 0;
			no_pe_segs=1;
		}
		out_size[PE_index] = 0;
		return PE_index;
	}
	void flushMsgPool() {
		for (int i=0; i<no_pe_segs; i++) {
			CkEntryOptions opts;
			opts.setQueueing(CK_QUEUEING_IFIFO);
			opts.setPriority(0);
			thisProxy[index_out[i]].receiveData(out_size[i], &out_data[i*MSG_SEG_SIZE], &out_rowInd[i*MSG_SEG_SIZE], &opts);
		}
		no_pe_segs=0;
	}
};
