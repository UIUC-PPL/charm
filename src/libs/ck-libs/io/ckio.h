#ifndef CK_IO_H
#define CK_IO_H

#include <ckcallback.h>
#include <cstring>
#include <iostream>
#include <pup.h>
#include <string>
#include <vector>

#include "CkIO.decl.h"

namespace Ck
{
namespace IO
{
class Session;
}
}  // namespace Ck

namespace Ck
{
namespace IO
{
/// Note: The values in options are not currently a stable or working interface.
/// Users should not set anything in them.
struct Options
{
  Options()
      : peStripe(0),
        writeStripe(0),
        activePEs(-1),
        basePE(-1),
        skipPEs(-1),
        read_stride(0),
        numReaders(0)
  {
  }

  /// How much contiguous data (in bytes) should be assigned to each active PE
  size_t peStripe;
  /// How much contiguous data (in bytes) should a PE gather before writing it out
  size_t writeStripe;
  /// How many PEs should participate in this activity
  int activePEs;
  /// Which PE should be the first to participate in this activity
  int basePE;
  /// How should active PEs be spaced out?
  int skipPEs;
  // How many bytes each Read Session should hold
  size_t read_stride;
  // How many IO buffers should there be
  size_t numReaders;

  void pup(PUP::er& p)
  {
    p | peStripe;
    p | writeStripe;
    p | activePEs;
    p | basePE;
    p | skipPEs;
    p | read_stride;
    p | numReaders;
  }
};

class FileReader;

class File;
// class ReadAssembler;
/// Open the named file on the selected subset of PEs, and send a
/// FileReadyMsg to the opened callback when the system is ready to accept
/// session requests on that file.
/// Note: The values in options are not currently a stable or working interface.
/// Users should not set anything in them.
void open(std::string name, CkCallback opened, Options opts);

/// Prepare to write data into the file described by token, in the window
/// defined by the offset and byte length. When the session is set up, a
/// SessionReadyMsg will be sent to the ready callback. When all of the data
/// has been written and synced, a message will be sent to the complete
/// callback.
void startSession(File file, size_t bytes, size_t offset, CkCallback ready,
                  CkCallback complete);

/// Prepare to write data into @arg file, in the window defined by the @arg
/// offset and length in @arg bytes. When the session is set up, a
/// SessionReadyMsg will be sent to the @arg ready callback. When all of the
/// data has been written and synced, an additional write will be made to the
/// file to `commit' the session's work. When that write has completed, a
/// message will be sent to the @arg complete callback.
void startSession(File file, size_t bytes, size_t offset, CkCallback ready,
                  const char* commitData, size_t commitBytes, size_t commitOffset,
                  CkCallback complete);

/// Write the given data into the file to which session is attached. The
/// offset is relative to the file as a whole, not to the session's offset.
void write(Session session, const char* data, size_t bytes, size_t offset);

/// Close a previously-opened file. All sessions on that file must have
/// already signalled that they are complete.
void close(File file, CkCallback closed);

/**
 * Prepare to read data from @arg file section specified by @arg bytes and @arg offset.
 * This method will proceed to eagerly read all of the data in that window into memory
 * for future read calls. After all the data is read in, the ready callback will be
 * invoked. The ready callback will take in a SessionReadyMessage* that will contain the
 * offset, the amount of bytes , and the buffer in the form of a vector<char>.
 */
void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready);

/**
 * Same as the above start session in function. However, there is an extra @arg
 * pes_to_map. pes_to_map will contain a sequence of numbers representing pes. CkIO will
 * map the IO Buffer chares to those pes specified in pes_to_map in a round_robin fashion.
 */
void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready,
                      std::vector<int> pes_to_map);

/**
 * Used to end the current read session and will then invoke the after_end callback that
 * takes a CkReductionMsg* with nothing in it Will effectively call ckDestroy() on the
 * CProxy_Reader of the associated FileInfo
 */

void closeReadSession(Session read_session, CkCallback after_end);
/**
 * Is a method that reads data from the @arg session of length @arg bytes at offset
 * @arg offset (in file). After this read finishes, the @arg after_read callback is
 * invoked, taking a ReadCompleteMsg* which points to a char* buffer, the offset,
 * and the number of bytes of the read.
 * */
void read(Session session, size_t bytes, size_t offset, char* data,
          CkCallback after_read);
void read(Session session, size_t bytes, size_t offset, CkCallback after_read,
          size_t tag);

// ZERO COPY READ;
void read(Session session, size_t bytes, size_t offset, CkCallback after_read, size_t tag,
          char* user_buffer);

class File
{
  int token;
  friend void startSession(File file, size_t bytes, size_t offset, CkCallback ready,
                           CkCallback complete);

  friend void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready);
  friend void startReadSession(File file, size_t bytes, size_t offset, CkCallback ready,
                               std::vector<int> pes_to_map);

  friend void startSession(File file, size_t bytes, size_t offset, CkCallback ready,
                           const char* commitData, size_t commitBytes,
                           size_t commitOffset, CkCallback complete);
  friend void close(File file, CkCallback closed);
  friend class FileReadyMsg;

public:
  File(int token_) : token(token_) {}
  File() : token(-1) {}
  void pup(PUP::er& p) { p | token; }
};

class FileReadyMsg : public CMessage_FileReadyMsg
{
public:
  File file;
  FileReadyMsg(const File& tok) : file(tok) {}
};

namespace impl
{
class Manager;
int getRDMATag();
class Director;  // forward declare Director class as impl
class ReadAssembler;
}  // namespace impl

class Session
{
  int file;
  size_t bytes, offset;
  CkArrayID sessionID;
  friend class Ck::IO::impl::Manager;
  friend class Ck::IO::impl::Director;
  friend class Ck::IO::impl::ReadAssembler;
  friend void read(Session session, size_t bytes, size_t offset, char* data,
                   CkCallback after_read);
  friend struct std::hash<Ck::IO::Session>;
  friend class FileReader;

public:
  Session(int file_, size_t bytes_, size_t offset_, CkArrayID sessionID_)
      : file(file_), bytes(bytes_), offset(offset_), sessionID(sessionID_)
  {
  }
  Session() {}
  void pup(PUP::er& p)
  {
    p | file;
    p | bytes;
    p | offset;
    p | sessionID;
  }

  int getFile() const { return file; }

  size_t getBytes() const { return bytes; }
  size_t getOffset() const { return offset; }
  CkArrayID getSessionID() const { return sessionID; }
  bool operator==(const Ck::IO::Session& other) const
  {
    return ((file == other.file) && (bytes == other.bytes) && (offset == other.offset) &&
            (sessionID == other.sessionID));
  }
};

class SessionReadyMsg : public CMessage_SessionReadyMsg
{
public:
  Session session;
  SessionReadyMsg(Session session_) : session(session_) {}
};

class ReadCompleteMsg : public CMessage_ReadCompleteMsg
{
public:
  size_t read_tag;
  size_t offset;
  size_t bytes;
  ReadCompleteMsg() {}
  ReadCompleteMsg(size_t in_tag, size_t in_offset, size_t in_bytes)
      : read_tag(in_tag), offset(in_offset), bytes(in_bytes)
  {
  }
};
/**
 * This is used by the FileReader in order to try to minimize
 * the number of networks calls made during a read. Instead
 * of calling Ck::IO::read repeatedly and each call has only a
 * small amount of data, the FileReader will make a Ck::IO::read
 * call with a larger amount of data and store that data in the
 * FileReaderBuffer. This way, if the FileReader is making small
 * read calls, the data will hopefully already be in the buffer,
 * which prevents superfluous messages. This is NOT a user facing
 * class and should not be used by the user.
 */
class FileReaderBuffer
{
  size_t _buff_capacity = 4096;  // the size of the buffer array
  size_t _buff_size = 0;         // the number of valid elements in the array
  size_t _offset = 0;            // the offset byte
  char* _buffer;
  bool is_dirty = true;

public:
  FileReaderBuffer();
  FileReaderBuffer(size_t buff_capacity);
  ~FileReaderBuffer();
  /**
   * Copies the @arg data into the head of _buffer
   * until the _buffer is full or data has been fully copied.
   * Will also set _buff_size to the number of bytes that was
   * copied.
   *
   * @arg offset: the offset in the file the @arg data arguments are from.
   * @arg num_bytes: the length of @arg data
   * @arg data: the array with the data to be put into the FileReaderBuffer
   */
  void setBuffer(size_t offset, size_t num_bytes,
                 char* data);  // writes the data to the buffer
  /**
   * This data checks whether, given a request specified by @arg offset
   * and @arg num_bytes, can use some of its cached data to fulfill the
   * request. This method changes the @arg buffer.
   *
   * @arg offset: the offset in the session the read request is.
   * @arg num_bytes: the number of bytes of the read request.
   * @arg buffer: the address of the buffer the read will go;
   * 			  this method will write to that address.
   */
  size_t getFromBuffer(size_t offset, size_t num_bytes, char* buffer);
  /**
   * Returns the capacity of the internal buffer.
   * @return size_t: the total capacity of _buffer.
   */
  size_t capacity();
};
/**
 * The Ck:IO equivalent to std::ifstream. If the user
 * doesn't want to write callbacks after a lot of reads,
 * or the user is making a series of very small sequential
 * reads, this abstraction will make it very easy. FileReader
 * uses caching in order to try and minimize the number of
 * extraneous network calls made during a series of read requests.
 * This class should be used in threaded entry methods.
 */
class FileReader
{
  Session _session_token;
  size_t _curr_pos = 0;
  size_t _offset, _num_bytes;
  bool _eofbit = false;
  size_t _gcount = 0;
  FileReaderBuffer _data_cache;
  bool _status = true;

public:
  std::ios_base::seekdir end = std::ios_base::end;
  std::ios_base::seekdir cur = std::ios_base::cur;
  std::ios_base::seekdir beg = std::ios_base::beg;
  /**
   * @arg Session: the session token the FileReader will use
   */
  FileReader(Ck::IO::Session session);
  /**
   * Perform a request of size @arg num_bytes_to_read, with
   * an offset of wherever the FileReader is in the stream.
   * It will write the result to @arg buffer.
   *
   * @arg buffer: the location where the read will be written to
   * @arg num_bytes_to_read: the number of bytes to read
   */
  FileReader& read(char* buffer, size_t num_bytes_to_read);
  /**
   * Returns the current position in the file the FileReader
   * is i.e te next byte the read will start.
   *
   * @return size_t: the position the FileReader is at in the
   * 				 file
   */
  size_t tellg();
  /**
   * Seeks to a position in the file for the FileReader from the
   * beginning of the read session. If the seek goes beyond the end of
   * the read session, it will set the internal position to be one byte
   * further than the end of session and the eof flag will be set.
   *
   * @arg pos: the position in the session wrt the beginning of
   * 		   the session to seek to.
   */
  FileReader& seekg(size_t pos);
  /**
   * Seeks to a position in the file for the FileReader wrt the
   * @arg dir specifies. If the seek goes beyond the end of the
   * read session, it will set the internal position to be one byte
   * further than the end of session and the eof flag will be set.
   *
   * @arg pos: the position in the session wrt what @arg dir
   * 		   the session to seek to.
   * @arg dir: Where to seek with respect to. If dir=std::ios_base::beg,
   * 		   then it is with respect to the beginning of the file. If
   * 		   dir=std::ios_base::cur, it is with respect to the current
   * 		   position of the FileReader. If dir=std::ios_base_end, then
   * 		   it is with respect to the end of the stream.
   */
  FileReader& seekg(size_t pos, std::ios_base::seekdir dir);
  /**
   * Returns whether the FileReader is at the end of the session.
   *
   * @return bool: whether the FileReader is at end of session.
   */
  bool eof();
  /**
   * Returns the number of bytes the last read did.
   * @return size_t: the number of bytes the last read call did.
   */
  size_t gcount();
  /**
   * Will return true if the FileReader is on a bad file.
   * Currently this always returns false because we assume
   * that the Session points to a good file.
   *
   * @return bool: false
   */
  bool operator!() const;
};

}  // namespace IO
}  // namespace Ck

#endif
