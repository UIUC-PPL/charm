#include "iotest.decl.h"
#include <assert.h>
#include <iostream>
#include <time.h>
#include <vector>
CProxy_Main mainproxy;
std::string global_fname;
class Main : public CBase_Main
{
  Main_SDAG_CODE

      CProxy_Test testers;
  int n;

  Ck::IO::Session session;
  Ck::IO::File f;

  size_t fileSize;

  double start_time;
  int numBufChares;
  std::string filename;

public:
  Main(CkArgMsg* m)
  {
    numBufChares = atoi(m->argv[1]);  // arg 1 = number of buffer chares

    fileSize = atoi(m->argv[2]);  // file size = arg 2

    n = atoi(m->argv[3]);  // arg 3 = number of readers

    std::string fn(m->argv[4]);  // arg 4 = filename
    filename = fn;
    global_fname = fn;

    CkPrintf("Parsed args.\n");

    mainproxy = thisProxy;
    thisProxy.run();  // open files
    delete m;
  }

  void iterDone() { CkExit(); }
};

class Test : public CBase_Test
{
  char* dataBuffer;
  char* file_reader_buffer;
  int size;
  std::string _fname;

public:
  Test(Ck::IO::Session token, size_t bytesToRead, std::string filename)
  {
    CkPrintf("Inside the constructor of tester %d\n", thisIndex);
    _fname = filename;
    thisProxy[thisIndex].testMethod(token, bytesToRead);
  }

  void testMethod(Ck::IO::Session token, size_t bytesToRead)
  {
    CkCallback sessionEnd(CkIndex_Test::readDone(0), thisProxy[thisIndex]);
    try
    {
      dataBuffer = new char[bytesToRead];
      file_reader_buffer = new char[bytesToRead];
    }
    catch (const std::bad_alloc& e)
    {
      CkPrintf("ERROR: Data buffer malloc of %zu bytes in Test chare %d failed.\n",
               bytesToRead, thisIndex);
      CkExit();
    }

    // setup and read using Ck::IO::FileReader
    size = bytesToRead;
    Ck::IO::FileReader fr(token);
    fr.seekg(bytesToRead * thisIndex);  // seek to the correct place in the file
    fr.read(file_reader_buffer,
            size);  // hopefully this will return the same data as Ck::IO::read
    CkAssert(fr.gcount() == size);  // makes sure that the gcount is correct
    CkAssert(fr.tellg() ==
             (size + bytesToRead * thisIndex));  // make sure that the tellg points to the
                                                 // correct place in the stream
    CkPrintf(
        "the FileReader::read function on tester[%d] is done with first character=%c\n",
        thisIndex, file_reader_buffer[0]);

    testFileReader(fr);

    // read using plain Ck::IO::Read
    Ck::IO::read(token, bytesToRead, bytesToRead * thisIndex, dataBuffer, sessionEnd);
  }

  void testFileReader(Ck::IO::FileReader& fr)
  {
    size_t og_pos = fr.tellg();
    fr.seekg(
        100000000000000);  // way beyond the bounds of read session, should trigger eof
    CkAssert(fr.eof());
    fr.seekg(5);
    CkAssert(fr.eof() == false);
    fr.seekg(1, std::ios_base::cur);
    CkAssert(fr.tellg() == 6);  // test that the seekg with different offset worked
    fr.seekg(0, std::ios_base::end);
    CkAssert(fr.eof());  // seeked to the end of file, make sure that the flag is on
    fr.seekg(6, std::ios_base::beg);
    CkAssert(fr.tellg() == 6);
    fr.seekg(og_pos);
  }

  Test(CkMigrateMessage* m) {}

  void readDone(Ck::IO::ReadCompleteMsg* m)
  {
    CkCallback done(CkIndex_Main::test_read(0), mainproxy);
    FILE* fp = fopen(_fname.c_str(), "r");
    if (!fp)
    {
      CkPrintf("FILE* is null on %d for file %s\n", thisIndex, _fname.c_str());
    }
    // CkPrintf("On reader[%d], Tryin to seek to %zu\n", thisIndex, (m->offset));

    int ret = fseek(fp, m->offset, SEEK_SET);

    if (ret)
    {
      CkPrintf("Something didn't return correctly in the fseek\n");
    }

    char* verify_buffer = new char[m->bytes];
    fread(verify_buffer, 1, m->bytes, fp);
    for (int i = 0; i < size; ++i)
    {
      if (verify_buffer[i] != dataBuffer[i])
      {
        CkPrintf(
            "From reader %d, offset=%zu, bytes=%zu, verify_buuffer[%d]=%c, "
            "dataBuffer[%d]=%c\n",
            thisIndex, (m->offset), (m->bytes), i, verify_buffer[i], i, dataBuffer[i]);
      }
      assert(verify_buffer[i] == dataBuffer[i]);
      assert(verify_buffer[i] == file_reader_buffer[i]);
    }
    delete[] verify_buffer;
    delete[] dataBuffer;
    delete[] file_reader_buffer;
    CkPrintf("Index %d is now done with the reads...\n", thisIndex);
    contribute(done);
  }
};

#include "iotest.def.h"
