#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <time.h>
#include <unordered_set>

typedef int FileToken;

#include "CkIO.decl.h"
#include "CkIO_impl.decl.h"

#include <errno.h>
#include <fcntl.h>
#include <pup_stl.h>
#include <sys/stat.h>

#if defined(_WIN32)
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include <chrono>
#include <future>  // used for async
#include <map>
#include <string>

#include "fs_parameters.h"

#if _cplusplus
#  warn("we are using C++\n")
#endif

using std::map;
using std::max;
using std::min;
using std::string;
using namespace std::chrono;

// FROM STACKEXCHANGE:
// https://stackoverflow.com/questions/19195183/how-to-properly-hash-the-custom-struct
template <class T>
inline void hash_combine(std::size_t& s, const T& v)
{
  std::hash<T> h;
  s ^= h(v) + 0x9e3779b9 + (s << 6) + (s >> 2);
}

// HASH FOR SESSION
template <>
struct std::hash<Ck::IO::Session>
{
  size_t operator()(const Ck::IO::Session& s) const
  {
    size_t res = 0;
    hash_combine(res, s.getFile());
    hash_combine(res, s.getBytes());
    hash_combine(res, s.getOffset());
    return res;
  }
};

namespace Ck
{
namespace IO
{
namespace impl
{
CProxy_Director director;
CkpvDeclare(Manager*, manager);
clock_t read_session_start;
clock_t read_session_end;
}  // namespace impl

namespace impl
{
struct FileInfo
{
  string name;
  CkCallback opened;
  Options opts;
  int fd;
  int sessionID;
  CProxy_WriteSession session;
  CProxy_BufferChares read_session;
  CkCallback complete;  // used for the write session complete callback?

  FileInfo(string name_, CkCallback opened_, Options opts_)
      : name(name_), opened(opened_), opts(opts_), fd(-1)
  {
  }
  FileInfo(string name_, Options opts_) : name(name_), opened(), opts(opts_), fd(-1) {}
  FileInfo() : fd(-1) {}
};

void fatalError(string desc, string file)
{
  CkAbort("FATAL ERROR on PE %d working on file '%s': %s; system reported %s\n", CkMyPe(),
          file.c_str(), desc.c_str(), strerror(errno));
}

class Director : public CBase_Director
{
  int filesOpened;
  map<FileToken, impl::FileInfo> files;
  CProxy_Manager managers;
  int opnum, sessionID;
  Director_SDAG_CODE

      public : Director(CkArgMsg* m)
      : filesOpened(0), opnum(0), sessionID(0)
  {
    delete m;
    director = thisProxy;
    managers = CProxy_Manager::ckNew();
  }

  Director(CkMigrateMessage* m) : CBase_Director(m) {}

  ~Director() {}

  void pup(PUP::er& p)
  {
    // TODO: All files must be closed across checkpoint/restart
    if (files.size() != 0)
      CkAbort("CkIO: All files must be closed across checkpoint/restart");

    p | filesOpened;
    p | managers;
    p | opnum;
    p | sessionID;
  }

  void openFile(string name, CkCallback opened, Options opts)
  {
    if (0 == opts.writeStripe)
      opts.writeStripe = CkGetFileStripeSize(name.c_str());
    if (0 == opts.peStripe)
      opts.peStripe = 4 * opts.writeStripe;
    if (-1 == opts.activePEs)
      opts.activePEs = min(CkNumPes(), 32);
    if (-1 == opts.basePE)
      opts.basePE = 0;
    if (-1 == opts.skipPEs)
      opts.skipPEs = CkMyNodeSize();
    if (opts.numReaders == 0)
    {
      opts.numReaders = std::min(CmiNumNodes(), CkNumPes());
    }
    files[filesOpened] = FileInfo(name, opened, opts);
    managers.openFile(opnum++, filesOpened++, name, opts);
  }

  void fileOpened(FileToken file) { files[file].opened.send(new FileReadyMsg(file)); }

  // method called by the closeReadSession function from user
  void closeReadSession(Session read_session, CkCallback after_end)
  {
    CProxy_BufferChares(read_session.sessionID).ckDestroy();

    after_end.send(
        CkReductionMsg::buildNew(0, NULL, CkReduction::nop));  // invoke a callback
  }

  void prepareWriteSession_helper(FileToken file, size_t bytes, size_t offset,
                                  CkCallback ready, CkCallback complete)
  {
    Options& opts = files[file].opts;
    files[file].sessionID = sessionID;

    int numStripes = 0;
    size_t bytesLeft = bytes, delta = opts.peStripe - offset % opts.peStripe;

    // Align to stripe boundary
    if (offset % opts.peStripe != 0 && delta < bytesLeft)
    {
      bytesLeft -= delta;
      numStripes++;
    }
    numStripes += bytesLeft / opts.peStripe;
    if (bytesLeft % opts.peStripe != 0)
      numStripes++;

    CkArrayOptions sessionOpts(numStripes);
    sessionOpts.setStaticInsertion(true);
    sessionOpts.setAnytimeMigration(false);
	      

    CkCallback sessionInitDone(CkIndex_Director::sessionReady(NULL), thisProxy);
    sessionInitDone.setRefnum(sessionID);
    sessionOpts.setInitCallback(sessionInitDone);

    // sessionOpts.setMap(managers);
    files[file].session = CProxy_WriteSession::ckNew(file, offset, bytes, sessionOpts);
    CkAssert(files[file].complete.isInvalid());
    files[file].complete = complete;
  }

  /**
   * prepareReadSessionHelper does all of the heavy lifting when trying to create the read
   * session it is responsible for creating the BufferChares, who then proceed to read in
   * their data asynchronously the pes_to_map could be empty or with items; if it's empty,
   * the user didn't provide a desired mapping, so the RTS will map how it likes
   * otherwise, it will round-robin on the pes in the pes_to_map vector when assigning
   * BufferChares to pes
   */
  void prepareReadSessionHelper(FileToken file, size_t bytes, size_t offset,
                                CkCallback ready, std::vector<int> pes_to_map)
  {
    if (!bytes)
    {
      CkAbort("You're tryna read 0 bytes. Oops.\n");
    }
    size_t session_bytes = bytes;  // amount of bytes in the session
    Options& opts = files[file].opts;
    files[file].sessionID = sessionID;
    // determine the number of reader sessions required, depending on the session size and
    // the number of bytes per reader
    int num_readers = opts.numReaders;

    CkArrayOptions sessionOpts(
        num_readers);  // set the number of elements in the chare array
    // if there is a non-empty mapping provided, do the mapping
    if (!pes_to_map.empty())
    {
      CProxy_BufferNodeMap bnm = CProxy_BufferNodeMap::ckNew(pes_to_map);
      sessionOpts.setMap(bnm);
    }
    CkCallback sessionInitDone(CkIndex_Director::sessionReady(0), thisProxy);
    sessionInitDone.setRefnum(sessionID);
    sessionOpts.setInitCallback(
        sessionInitDone);  // invoke the sessionInitDone callback after all the elements
                           // of the chare array are created
    files[file].read_session = CProxy_BufferChares::ckNew(
        file, offset, bytes, num_readers, sessionOpts);  // create the readers
  }

  void sessionComplete(FileToken token)
  {
    CProxy_CkArray(files[token].session.ckGetArrayID()).ckDestroy();
    files[token].complete.send(CkReductionMsg::buildNew(0, NULL, CkReduction::nop));
    files[token].complete = CkCallback(CkCallback::invalid);
  }

  void close(FileToken token, CkCallback closed)
  {
    managers.close(opnum++, token, closed);
    files.erase(token);
  }
};

/**
 * struct that keeps track of meta information of a particular read request
 * is used for the zero copy and the callback to be invoked by CkIO after read is complete
 * */
struct ReadInfo
{
  size_t bytes_left;  // the number of bytes the user wants for a particular read
  size_t read_bytes;
  size_t read_offset;  // the offset they specify for their read
  size_t read_size;
  CkCallback after_read;  // the callback to invoke after the read is complete
  ReadCompleteMsg* msg;
  size_t read_tag = -1;
  char* data = 0;
};
// class that is used to aggregate the data for a specific read call made by the user
// is responsible for collecting data for a specific read and correctly ordering it to
// return to the user
class ReadAssembler : public CBase_ReadAssembler
{
private:
  Session _session;
  std::unordered_map<int, ReadInfo>
      _read_info_buffer;  // matches an assigned tag for a read to the read info
  size_t _curr_read_tag = 0;
  size_t _curr_RDMA_tag = 0;

public:
  ReadAssembler(Session session) { _session = session; }

  ReadAssembler(CkMigrateMessage* m) : CBase_ReadAssembler(m) {}

  /*
   * This function adds the read request to the _read_info_buffer table
   * which maps a tag to a ReadInfo struct
   */
  size_t addReadToTable(size_t read_bytes, size_t read_offset, char* data,
                        CkCallback after_read)
  {
    // do the initialization of the read struct
    ReadInfo ri;
    ri.bytes_left = read_bytes;
    ri.read_offset = read_offset;
    ri.read_bytes = read_bytes;
    ri.after_read = after_read;
    ri.data = data;
    ri.msg = new ReadCompleteMsg();
    ri.msg->offset = read_offset;
    ri.msg->bytes = read_bytes;
    if (_read_info_buffer.count(_curr_read_tag) != 0)
    {
      CkPrintf("Something is wrong, a read tag is being overwritten on pe=%d!\n",
               CkMyPe());
      CkExit();
    }
    _read_info_buffer[_curr_read_tag] = ri;  // put the readinfo struct in the table
    _curr_read_tag++;
    return _curr_read_tag - 1;
  }

  void pup(PUP::er& p)
  {
    // TODO: All files must be closed across checkpoint/restart
  }

  void removeEntryFromReadTable(int tag) { _read_info_buffer.erase(tag); }

  /**
   * This is the entry method used in order to
   * send the data from the BufferChares to the ReadAssembler;
   * Called by the BufferChares
   * First one is the registration method called for Zero-copy, second method is the
   * actual logic
   * */
  void shareData(int read_tag, int buffer_tag, size_t read_chare_offset, size_t num_bytes,
                 char* data, CkNcpyBufferPost* ncpyPost)
  {
    ncpyPost[0].regMode = CK_BUFFER_REG;
    ncpyPost[0].deregMode = CK_BUFFER_DEREG;
    CkMatchBuffer(ncpyPost, 0, buffer_tag);
  }

  void shareData(int read_tag, int buffer_tag, size_t read_chare_offset, size_t num_bytes,
                 char* data)
  {
    ReadInfo& info = _read_info_buffer[read_tag];  // get the struct from the buffer tag
    info.bytes_left -= num_bytes;  // decrement the number of remaining bytes to read
    if (info.bytes_left)
      return;  // if there are bytes still to read, just return
    info.after_read.send(info.msg);
    removeEntryFromReadTable(read_tag);  // the read is complete; remove it from the table
  }

  /**
   * function used by the manager::read in order to take care of
   * requesting data from BufferChares, storing reads in the table,
   * as well as other read infrastructure
   */
  void serveRead(size_t read_bytes, size_t read_offset, char* data, CkCallback after_read,
                 size_t read_stride, size_t num_readers)
  {
    int read_tag = addReadToTable(read_bytes, read_offset, data,
                                  after_read);  // create a tag for the actual read
    // get the necessary info
    size_t bytes = read_bytes;
    size_t start_idx = (read_offset - _session.offset) /
                       read_stride;  // the first index that has the relevant data
    ReadInfo& info = _read_info_buffer[read_tag];  // get the ReadInfo object
    ReadCompleteMsg* msg = info.msg;

    // the entire read falls in the "extra" bytes that the last BufferChares owns
    if (start_idx == num_readers)
    {
      int tag = getRDMATag();
      CkPostBuffer(info.data, bytes, tag);
      CProxy_BufferChares(_session.sessionID)[start_idx - 1].sendData(
          read_tag, tag, read_offset, bytes, thisProxy, CkMyPe());
      return;
    }
    // make sure to account for the last BufferChares holding potentially more data than
    // the rest
    for (size_t i = start_idx;
         (i < num_readers) && (i * read_stride) < (read_offset + bytes); ++i)
    {
      size_t data_idx;
      size_t data_len;
      if (i == start_idx)
      {
        data_idx = 0;  // at the start of read
        // if intrabuffer, just take read size; o/w go from offset to end of buffer chare
        data_len =
            std::min((read_stride * (i + 1) + _session.offset - read_offset), bytes);
        if (i == num_readers - 1)
        {  // the read is contained entirely in data of the last buffer chare; make sure
           // to account for the extra bytes if there are any!
          data_len = read_bytes;
        }
      }
      else
      {
        data_idx = (read_stride * i + _session.offset -
                    read_offset);  // first byte of fille in buffer chare, offset from the
                                   // read offset
        data_len = std::min(read_stride,
                            read_offset + bytes - (read_stride * i + _session.offset));
        // the length is gonna be the entire chare's readstripe, or what's remainig of the
        // read
        //
        if (i == num_readers - 1)
        {  // we are searching the last buffer chare; make sure to account for the extra
           // bytes if there are any!
          data_len = read_offset + read_bytes - (read_stride * i + _session.offset);
        }
      }
      // do the CkPost call
      int tag = getRDMATag();
      CkPostBuffer(info.data + data_idx, data_len, tag);
      CProxy_BufferChares(_session.sessionID)[i].sendData(
          read_tag, tag, read_offset, read_bytes, thisProxy, CkMyPe());
    }
  }
};

class Manager : public CBase_Manager
{
  Manager_SDAG_CODE int opnum;
  std::unordered_map<Session, CProxy_ReadAssembler>
      _session_to_read_assembler;  // map used to get the read assembler for a specific
                                   // session
  int _curr_tag = 0;

public:
  Manager() : opnum(0)
  {
    CkpvInitialize(Manager*, manager);
    CkpvAccess(manager) = this;
    thisProxy[CkMyPe()].run();
  }

  Manager(CkMigrateMessage* m) : CBase_Manager(m)
  {
    CkpvInitialize(Manager*, manager);
    CkpvAccess(manager) = this;
  }

  // invoked to insert the readassembler for a specific session
  void addSessionReadAssemblerMapping(Session session, CProxy_ReadAssembler ra,
                                      CkCallback ready)
  {
    _session_to_read_assembler[session] = ra;
    CkCallback cb(CkIndex_Director::addSessionReadAssemblerFinished(0), director);
    int temp = 0;
    contribute(
        sizeof(temp), &temp, CkReduction::nop,
        cb);  // effectively a barrier, makes sure every PE is done with adding session
  }

  int getTag() { return _curr_tag++; }

  void pup(PUP::er& p)
  {
    p | opnum;

    // TODO: All files must be closed across checkpoint/restart
    if (files.size() != 0)
      CkAbort("CkIO: All files must be closed across checkpoint/restart");
  }

  void prepareFile(FileToken token, string name, Options opts)
  {
    CkAssert(files.end() == files.find(token));
    // CkAssert(lastActivePE(opts) < CkNumPes());
    CkAssert(opts.writeStripe <= opts.peStripe);
    files[token] = impl::FileInfo(name, opts);

    contribute(sizeof(FileToken), &token, CkReduction::max_int,
               CkCallback(CkReductionTarget(Director, fileOpened), director));
  }

  impl::FileInfo* get(FileToken token)
  {
    CkAssert(files.find(token) != files.end());

    // Open file if we're one of the active PEs
    // XXX: Or maybe wait until the first write-out, to smooth the metadata load?
    if (files[token].fd == -1)
    {
      string& name = files[token].name;
#if defined(_WIN32)
      int fd = CmiOpen(name.c_str(), _O_WRONLY | _O_CREAT, _S_IREAD | _S_IWRITE);
#else
      int fd = CmiOpen(name.c_str(), O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
#endif
      if (-1 == fd)
        fatalError("Failed to open a file for parallel output", name);

      files[token].fd = fd;
    }

    return &(files[token]);
  }

  // used by manager to handle a read request from its own PE
  void read(Session session, size_t bytes, size_t offset, char* data,
            CkCallback after_read)
  {
    if (!_session_to_read_assembler.count(session))
    {
      CkPrintf("Why is there no session associated with read on manager %d\n", CkMyPe());
      CkExit();
    }
    CProxy_ReadAssembler ra = _session_to_read_assembler[session];
    Options& opt = files[session.file].opts;
    size_t num_readers = opt.numReaders;
    // the number of bytes each BufferChare owns, exlcuding the bytes that aren't
    // available
    size_t read_stride = session.getBytes() / num_readers;
    // get the readassembler on this PE
    ReadAssembler* grp_ptr = ra.ckLocalBranch();
    if (!grp_ptr)
    {
      CkPrintf("The pointer to the local branch is null on pe=%d\n", CkMyPe());
      CkExit();
    }
    grp_ptr->serveRead(bytes, offset, data, after_read, read_stride, num_readers);
  }

  void write(Session session, const char* data, size_t bytes, size_t offset)
  {
    Options& opts = files[session.file].opts;
    size_t stripe = opts.peStripe;

    CkAssert(offset >= session.offset);
    CkAssert(offset + bytes <= session.offset + session.bytes);

    size_t sessionStripeBase = (session.offset / stripe) * stripe;

    while (bytes > 0)
    {
      size_t stripeIndex = (offset - sessionStripeBase) / stripe;
      size_t bytesToSend = min(bytes, stripe - offset % stripe);

      CProxy_WriteSession(session.sessionID)[stripeIndex].forwardData(data, bytesToSend,
                                                                      offset);

      data += bytesToSend;
      offset += bytesToSend;
      bytes -= bytesToSend;
    }
  }

  void doClose(FileToken token, CkCallback closed)
  {
    int fd = files[token].fd;
    if (fd != -1)
    {
      int ret;
      do
      {
#if defined(_WIN32)
        ret = _close(fd);
#else
        ret = ::close(fd);
#endif
      } while (ret < 0 && errno == EINTR);
      if (ret < 0)
        fatalError("close failed", files[token].name);
    }
    files.erase(token);
    contribute(closed);
  }

  int procNum(int arrayHdl, const CkArrayIndex& element)
  {
#if 0
          int peIndex = stripeIndex % opts.activePEs;
          int pe = opts.basePE + peIndex * opts.skipPEs;
#endif
    return 0;
  }

private:
  map<FileToken, impl::FileInfo> files;

  int lastActivePE(const Options& opts)
  {
    return opts.basePE + (opts.activePEs - 1) * opts.skipPEs;
  }
};

int getRDMATag()
{  // function to allow ReadAssembler to get the current RDMA tag on the PE
  return CkpvAccess(manager)->getTag();
}

class WriteSession : public CBase_WriteSession
{
  const FileInfo* file;
  size_t sessionOffset, myOffset;
  size_t sessionBytes, myBytes, myBytesWritten;
  FileToken token;

  struct buffer
  {
    std::vector<char> array;
    int bytes_filled_so_far;

    buffer() { bytes_filled_so_far = 0; }

    void expect(size_t bytes) { array.resize(bytes); }

    void insertData(const char* data, size_t length, size_t offset)
    {
      char* dest = &array[offset];
      memcpy(dest, data, length);

      bytes_filled_so_far += length;
    }

    bool isFull() { return bytes_filled_so_far == array.size(); }
  };
  map<size_t, struct buffer> bufferMap;

public:
  WriteSession(FileToken file_, size_t offset_, size_t bytes_)
      : file(CkpvAccess(manager)->get(file_)),
        sessionOffset(offset_),
        myOffset((sessionOffset / file->opts.peStripe + thisIndex) * file->opts.peStripe),
        sessionBytes(bytes_),
        myBytes(min(file->opts.peStripe, sessionOffset + sessionBytes - myOffset)),
        myBytesWritten(0),
        token(file_)
  {
    CkAssert(file->fd != -1);
    CkAssert(myOffset >= sessionOffset);
    CkAssert(myOffset + myBytes <= sessionOffset + sessionBytes);
  }

  WriteSession(CkMigrateMessage* m) {}

  void forwardData(const char* data, size_t bytes, size_t offset)
  {
    CkAssert(offset >= myOffset);
    CkAssert(offset + bytes <= myOffset + myBytes);

    size_t stripeSize = file->opts.writeStripe;

    while (bytes > 0)
    {
      size_t stripeBase = (offset / stripeSize) * stripeSize;
      size_t stripeOffset = max(stripeBase, myOffset);
      size_t nextStripe = stripeBase + stripeSize;
      size_t expectedBufferSize = min(nextStripe, myOffset + myBytes) - stripeOffset;
      size_t bytesInCurrentStripe = min(nextStripe - offset, bytes);

      buffer& currentBuffer = bufferMap[stripeOffset];
      currentBuffer.expect(expectedBufferSize);

      currentBuffer.insertData(data, bytesInCurrentStripe, offset - stripeOffset);

      if (currentBuffer.isFull())
      {
        flushBuffer(currentBuffer, stripeOffset);
        bufferMap.erase(stripeOffset);
      }

      bytes -= bytesInCurrentStripe;
      data += bytesInCurrentStripe;
      offset += bytesInCurrentStripe;
    }

    if (myBytesWritten == myBytes)
      contribute(CkCallback(CkIndex_WriteSession::syncData(), thisProxy));
  }

  void syncData()
  {
    int status;
    CkAssert(bufferMap.size() == 0);
#if CMK_HAS_FDATASYNC_FUNC
    while ((status = fdatasync(file->fd)) < 0)
    {
      if (errno != EINTR)
      {
        fatalError("fdatasync failed", file->name);
      }
    }
#elif CMK_HAS_FSYNC_FUNC
    while ((status = fsync(file->fd)) < 0)
    {
      if (errno != EINTR)
      {
        fatalError("fsync failed", file->name);
      }
    }
#elif defined(_WIN32)
    intptr_t hFile = _get_osfhandle(file->fd);
    if (FlushFileBuffers((HANDLE)hFile) == 0)
      fatalError("FlushFileBuffers failed", file->name);
#elif CMK_HAS_SYNC_FUNC
#  warning "Will call sync() for every completed write"
    sync();  // No error reporting from sync()
#else
#  warning "No file synchronization function available!"
#endif

    contribute(sizeof(FileToken), &token, CkReduction::max_int,
               CkCallback(CkReductionTarget(Director, sessionComplete), director));
  }

  void flushBuffer(buffer& buf, size_t bufferOffset)
  {
    int l = buf.bytes_filled_so_far;
    char* d = &(buf.array[0]);

    CmiInt8 ret = CmiPwrite(file->fd, d, l, bufferOffset);
    if (ret < 0)
      fatalError("Call to pwrite failed", file->name);

    CkAssert(ret == l);
    myBytesWritten += l;
  }
};

/**
 * These are the designated readers that go to disk
 * and get the data. They are also responsible for holding on
 * to the data and are who give copies of the data to
 * the ReadAssemblers who need to satisfy read requests.
 * The number of BufferChares is configurable in the Options struct
 * when the user sets up their read session.
 */
class BufferChares : public CBase_BufferChares
{
  BufferChares_SDAG_CODE private : FileToken _token;  // the token of the given file
  const FileInfo* _file;                              // the pointer to the FileInfo
  size_t _session_bytes;                              // number of bytes in the session
  size_t _session_offset;                             // the offset of the session
  size_t _my_offset;
  size_t _my_bytes;
  std::shared_future<char*> _buffer;

  size_t _num_readers;
  size_t _read_stride;

public:
  BufferChares(FileToken file, size_t offset, size_t bytes, size_t num_readers)
      : _token(file),
        _file(CkpvAccess(manager)->get(file)),
        _session_bytes(bytes),
        _session_offset(offset)
  {
    _num_readers = num_readers;
    _read_stride = bytes / num_readers;
    _my_offset = thisIndex * (_read_stride) + _session_offset;
    _my_bytes = min(_read_stride,
                    _session_offset + _session_bytes -
                        _my_offset);  // get the number of bytes owned by the session
    // last BufferChares array; read the remaining stuff
    if (thisIndex == _num_readers - 1)
      _my_bytes = _session_offset + _session_bytes - _my_offset;

    CkAssert(_file->fd != -1);
    CkAssert(_my_offset >= _session_offset);
    CkAssert(_my_offset + _my_bytes <= _session_offset + _session_bytes);
    double disk_read_start_ck = CkWallTimer();  // get the before disk_read

    std::future<char*> temp_buffer =
        std::async(std::launch::async, &BufferChares::readData, this);

    _buffer = temp_buffer.share();

    double disk_read_end_ck = CkWallTimer();
    double total_time_ms_ck = (disk_read_end_ck - disk_read_start_ck) * 1000;

    thisProxy[thisIndex].monitorRead();
  }

  ~BufferChares() { delete[] _buffer.get(); }

  void monitorRead()
  {
    while (_buffer.wait_for(std::chrono::microseconds(0)) != std::future_status::ready)
    {
      // "Call after" implementation
      // CcdCallFnAfter((CcdVoidFn)CthAwaken, CthSelf(),
      //                BUFFER_TIMEOUT_MS);  // timeout in ms
      // CthSuspend();

      CthYield();
    }

    thisProxy[thisIndex].bufferReady();
  }

  // can be used for debugging
  void printTime(double time_taken)
  {
    clock_t buffer_read_done = clock();
    double total =
        (double(buffer_read_done - read_session_start) / CLOCKS_PER_SEC * 1000);
    CkPrintf(
        "The time to disk took %fms on the max chare and %f since read session start.\n",
        time_taken, total);
  }

#if defined(_WIN32)
  char* readDataWIN32()
  {
    char* buffer = new char[_my_bytes];
    //    CkPrintf("Allocating buffer on chare %d at addr %p.\n", thisIndex, buffer);

    int fd = _open(_file->name.c_str(), O_RDONLY | O_BINARY, NULL);

    if (fd == -1)
    {
      CkAbort("Opening of the file %s went wrong\n", _file->name.c_str());
    }

    if (_lseek(fd, _my_offset, SEEK_SET) == -1)
    {
      CkAbort("Lseek buffer chare failed.\n");
    }

    size_t num_bytes_read = _read(fd, buffer, (int)_my_bytes);

    if (num_bytes_read != _my_bytes)
    {
      CkAbort("CKIO Reader: supposed to read %zu bytes, but only read %zu bytes\n",
              _my_bytes, num_bytes_read);
    }

    _close(fd);

    return buffer;
  }
#endif  // if defined(_WIN32)
  char* readDataPOSIX()
  {
    char* buffer = new char[_my_bytes];

    int fd = ::open(_file->name.c_str(), O_RDONLY, NULL);

    if (fd == -1)
    {
      CkAbort("Opening of the file %s went wrong\n", _file->name.c_str());
    }

    if (lseek(fd, _my_offset, SEEK_SET) == -1)
    {
      CkAbort("Lseek buffer chare failed.\n");
    }

    size_t num_bytes_read = ::read(fd, buffer, (int)_my_bytes);

    if (num_bytes_read != _my_bytes)
    {
      CkAbort("CKIO Reader: supposed to read %zu bytes, but only read %zu bytes\n",
              _my_bytes, num_bytes_read);
    }

    ::close(fd);

    return buffer;
  }
  /**
   * This function is launched in a separate thread
   * in order to allow the reads to disk to be parallelized
   * which allows other work to be done. This also stores the
   * segment read in memory. In the future, could Potentially explore not storing in
   * memory, and instead going to disk on-demand (what MPI does)
   */

  char* readData()
  {
#if defined(_WIN32)
    return readDataWIN32();
#else
    return readDataPOSIX();
#endif
  }

  /**
   * Method invoked by the ReadAssembler in order to request from the
   * BufferChare data.. Note that offset and bytes are with respect to the overall file
   * itself
   */
  void sendDataHandler(int read_tag, int buffer_tag, size_t offset, size_t bytes,
                       CProxy_ReadAssembler ra, int pe)
  {
    size_t chare_offset;
    size_t chare_bytes;

    if (offset >= (_my_offset + _my_bytes))
      return;  // read call starts to the right of this chare

    else if (offset + bytes <= _my_offset)
      return;  // the read call starts to the left of this chare

    if (offset < _my_offset)
      chare_offset =
          _my_offset;  // the start of the read is below this chare, so we should read
                       // in the current data from start of what it owns
    else
      chare_offset = offset;  // read offset is in the middle

    size_t end_byte_chare =
        min(offset + bytes,
            _my_offset + _my_bytes);  // the last byte, exclusive, this chare should read
    size_t bytes_to_read = end_byte_chare - chare_offset;

    char* buffer = _buffer.get();  // future call to get
    CProxy_ReadAssembler(ra)[pe].shareData(
        read_tag, buffer_tag, chare_offset, bytes_to_read,
        CkSendBuffer(buffer +
                     (chare_offset -
                      _my_offset) /*, cb*/));  // send this data to the ReadAssembler
  }
};

class Map : public CBase_Map
{
public:
  Map() {}

  int procNum(int arrayHdl, const CkArrayIndex& element) { return 0; }
};

}  // namespace impl

void open(string name, CkCallback opened, Options opts)
{
  impl::director.openFile(name, opened, opts);
}

void startSession(File file, size_t bytes, size_t offset, CkCallback ready,
                  CkCallback complete)
{
  impl::director.prepareWriteSession(file.token, bytes, offset, ready, complete);
}

void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready)
{
  impl::director.prepareReadSession(file.token, bytes, offset, ready);
}

void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready,
                      std::vector<int> pes_to_map)
{
  impl::director.prepareReadSession(file.token, bytes, offset, ready, pes_to_map);
}

void closeReadSession(Session read_session, CkCallback after_end)
{
  impl::director.closeReadSession(read_session, after_end);  // call the director helper
}

void startSession(File file, size_t bytes, size_t offset, CkCallback ready,
                  const char* commitData, size_t commitBytes, size_t commitOffset,
                  CkCallback complete)
{
  impl::director.prepareWriteSession(file.token, bytes, offset, ready, commitData,
                                     commitBytes, commitOffset, complete);
}

void write(Session session, const char* data, size_t bytes, size_t offset)
{
  using namespace impl;
  CkpvAccess(manager)->write(session, data, bytes, offset);
}

void read(Session session, size_t bytes, size_t offset, char* data, CkCallback after_read)
{
  CkAssert(bytes <= session.bytes);
  CkAssert(offset + bytes <= session.offset + session.bytes);
  using namespace impl;

  CkpvAccess(manager)->read(session, bytes, offset, data, after_read);
}

void close(File file, CkCallback closed) { impl::director.close(file.token, closed); }

class SessionCommitMsg : public CMessage_SessionCommitMsg
{
};
// used to specify which PEs to map the IO chares to
class BufferNodeMap : public CkArrayMap
{
  std::vector<int> _processors;

public:
  BufferNodeMap(void) : CkArrayMap() {}

  BufferNodeMap(CkMigrateMessage* m) {}

  BufferNodeMap(std::vector<int> processors) : _processors(processors) {}

  int procNum(int arrayHd1, const CkArrayIndex& element)
  {
    int elem = *(int*)(element.data());
    int idx = elem % _processors.size();
    return _processors.at(idx);
  }

  int registerArray(CkArrayIndex& numElements, CkArrayID aid) { return 0; }
};
FileReader::FileReader(Ck::IO::Session session)
    : _session_token(session), _offset(session.offset), _num_bytes(session.bytes)
{
}

FileReader& FileReader::read(char* buffer, size_t num_bytes_to_read)
{
  if (_eofbit)
  {  // no more bytes to read
    _gcount = 0;
    return *this;
  }
  size_t amt_from_cache = _data_cache.getFromBuffer(
      _curr_pos, num_bytes_to_read, buffer);  // get whatever data the cache has for us
  _curr_pos += amt_from_cache;
  if (amt_from_cache == num_bytes_to_read)
  {
    return *this;
  }
  size_t bytes_to_read_left = num_bytes_to_read - amt_from_cache;
  size_t bytes_to_read = std::min(
      std::max(bytes_to_read_left, _data_cache.capacity()),
      (_offset + _num_bytes -
       _curr_pos));  // if the read is too small, get more data to store in the buffer
  if (!bytes_to_read)
  {
    return *this;
  }
  char* tmp_data_buff =
      new char[bytes_to_read];  // temporary buffer that will hold all the data
  ReadCompleteMsg* read_msg;
  Ck::IO::read(_session_token, bytes_to_read, _curr_pos, tmp_data_buff,
               CkCallbackResumeThread((void*&)read_msg));
  // below will not get executed until the read is done
  size_t bytes_read = read_msg->bytes;
  if (bytes_read > bytes_to_read_left)
  {
    // if I read more bytes than what was left to read, that means I have extra bytes that
    // the buffer can use
    _data_cache.setBuffer(_curr_pos, bytes_read, tmp_data_buff);
    _curr_pos += bytes_to_read_left;
    std::memcpy(buffer + amt_from_cache, tmp_data_buff, bytes_to_read_left);
  }
  else
  {
    // too many bytes, nothing to actually cache
    _curr_pos += bytes_read;
    std::memcpy(buffer + amt_from_cache, tmp_data_buff, bytes_read);
  }
  delete[] tmp_data_buff;
  if (_curr_pos >= (_offset + _num_bytes))
  {
    _eofbit = true;  // ran out of data to read
    _curr_pos = _offset + _num_bytes;
  }
  _gcount = std::min(bytes_read, bytes_to_read_left);
  return *this;
}

// overload ! operator on filereader object
bool FileReader::operator!() const
{
  // CkPrintf("In overwritten operator\n");
  return false;
}

size_t FileReader::tellg() { return _curr_pos; }

FileReader& FileReader::seekg(size_t pos, std::ios_base::seekdir dir)
{
  if (dir == std::ios_base::beg)
  {
    _curr_pos = pos + _offset;
  }
  else if (dir == std::ios_base::cur)
  {
    _curr_pos += pos;
  }
  else if (dir == std::ios_base::end)
  {
    _curr_pos = _offset + _num_bytes - pos;
  }

  _eofbit = false;
  if (_curr_pos < _offset)
  {
    _curr_pos = _offset;
    _eofbit = false;
  }
  else if (_curr_pos >= (_offset + _num_bytes))
  {
    _curr_pos = _offset + _num_bytes;
    _eofbit = true;
  }
  return *this;
}

FileReader& FileReader::seekg(size_t pos)
{
  _curr_pos = pos;
  if (_curr_pos < _offset)
  {
    _curr_pos = _offset;
    _eofbit = false;
  }
  else if (_curr_pos >= (_offset + _num_bytes))
  {
    _curr_pos = _offset + _num_bytes;
    _eofbit = true;
	return *this;
  }
  _eofbit = false;
  return *this;
}

bool FileReader::eof() { return _eofbit; }

size_t FileReader::gcount() { return _gcount; }

FileReaderBuffer::FileReaderBuffer() { _buffer = new char[_buff_capacity]; }

FileReaderBuffer::FileReaderBuffer(size_t buff_capacity)
{
  _buff_capacity = buff_capacity;
  _buffer = new char[_buff_capacity];
}

void FileReaderBuffer::setBuffer(size_t offset, size_t num_bytes, char* data)
{
  is_dirty = false;
  _offset = offset;
  _buff_size = std::min(_buff_capacity, num_bytes);
  std::memcpy(_buffer, data, _buff_size);  // copy the first section of bytes
}

size_t FileReaderBuffer::getFromBuffer(size_t offset, size_t num_bytes, char* buffer)
{
  if (is_dirty || offset < _offset || offset >= (_offset + _buff_size))
  {
    return 0;  // the buffer has nothing of relevance
  }
  size_t cached_len = std::min(offset + num_bytes, _offset + _buff_size) - offset;
  std::memcpy(buffer, _buffer + (offset - _offset), cached_len);
  return cached_len;
}

size_t FileReaderBuffer::capacity() {
	return _buff_capacity;
}

FileReaderBuffer::~FileReaderBuffer() { delete[] _buffer; }

}  // namespace IO
}  // namespace Ck

#include "CkIO.def.h"
#include "CkIO_impl.def.h"
