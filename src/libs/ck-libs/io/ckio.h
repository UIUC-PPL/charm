#ifndef CK_IO_H
#define CK_IO_H

#include <ckcallback.h>
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
      : peStripe(0), writeStripe(0), activePEs(-1), basePE(-1), skipPEs(-1), numReaders(0)
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
  // How many IO buffers should there be
  size_t numReaders;

  void pup(PUP::er& p)
  {
    p | peStripe;
    p | writeStripe;
    p | activePEs;
    p | basePE;
    p | skipPEs;
    p | numReaders;
  }
};

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
/// callback. A session of 0 bytes is ready and complete at once; its two
/// callbacks may arrive in either order. One session per file at a time;
/// sessions on different files may be in progress together.
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
 * On starting the session, the buffer chares begin eagerly reading all requested data
 * into memory. The ready callback is invoked once all buffer chares have been created and
 * their reads have been initiated (but the reads are not guaranteed to be complete at
 * this point). One read session per file at a time; read sessions on different files
 * may be in progress together. A read session of 0 bytes aborts.
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
 * invoked, taking a ReadCompleteMsg* which points to a vector<char> buffer, the offset,
 * and the number of bytes of the read.
 * */
void read(Session session, size_t bytes, size_t offset, char* data,
          CkCallback after_read);

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

}  // namespace IO
}  // namespace Ck

#endif
