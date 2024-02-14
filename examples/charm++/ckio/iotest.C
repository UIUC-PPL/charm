#include "iotest.decl.h"
#include <vector>

class Main : public CBase_Main{
	Main_SDAG_CODE;
	std::vector<Ck::IO::File> _files; // holds all of the CkIO File objects
	int _num_writers;
	int _num_iterations;
	CProxy_Writer writers;
public:
	Main(CkArgMsg* msg){
		// make sure the example program is being used correctly
		if(msg -> argc != 3){
			ckout << "Usage: ./<program_name> <number_writers> <number_of_files>" << endl;
			CkExit();
		}
		_num_writers = atoi(msg -> argv[1]); // assign the number of writers
		_num_iterations = atoi(msg -> argv[2]); // assign the number of files to write
		int num_files = _num_iterations; // save the number of files
		_files.resize(_num_iterations);
		for(int i = 0; i < num_files; ++i){
			thisProxy.startWritingCycle(i); // start writing to the file numbered i
		}
		delete msg;

	}
	// standard bookkeeping for how many iterations we need to go through
	void decrementRemaining(){
		_num_iterations--;
		if(!_num_iterations){
			ckout << "Successfully completed parallel output!" << endl;
			CkExit();
		}
	}	

};


class Writer : public CBase_Writer {
	
public:
	/**
	 * Takes in a Session object to the current writing session. The constructor
	 * will actually write data to the file in the incoming_session object.
	 */
	Writer(Ck::IO::Session incoming_session){
		char out[11]; // 10 bytes for the message, 1 for the nullbyte
		sprintf(out, "Writer[%d]\n", thisIndex);
		Ck::IO::write(incoming_session, out, 10, 10*thisIndex); // writing 10 bytes starting at 10*thisIndex from the beginning of the file
	}
	
	Writer(CkMigrateMessage* m){
	
	}
};

#include "iotest.def.h"
