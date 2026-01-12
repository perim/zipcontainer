#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zipc;
struct zipcstream;

enum zipc_status
{
	ZIPC_SUCCESS = 0,
	ZIPC_SYNTAX_ERROR,
	ZIPC_PERMISSION_FAILURE,
	ZIPC_PATH_ALREADY_EXISTS,
	ZIPC_IO_FAILURE,
	ZIPC_CORRUPT_ARCHIVE,
	ZIPC_UNSUPPORTED_FEATURE,
	ZIPC_PATH_NOT_FOUND,
	// TBD many more here
};

/// Open an uncompressed ZIP file. Mode is 'r' for reading, 'w' for writing (replacing
/// any existing file), or 'a' for appending. You are responsible for synchronizing
/// thread access to the returned handle unless otherwise specified below. You cannot
/// mix modes. If `err` is not null, we will write status to it. Returns null on failure.
zipc* zipc_open(const char* filename, const char* mode, enum zipc_status* err);

/// Close an open ZIP file handle.
void zipc_close(zipc* handle);

/// Obtain the filesize of a given file inside the ZIP container. Return -1 if the
/// file is not found. This function is thread-safe.
ssize_t zipc_filesize(zipc* handle, const char* path);

struct zipc_mapping
{
	const void* data;      // Pointer to file contents (read-only).
	size_t size;           // Size of the file data.
	const void* map_base;  // Base address of the mapping.
	size_t map_length;     // Length passed to mmap().
};

/// Memory map a file inside the ZIP container for streaming read. Read once and then
/// unmap it. The whole file will be mapped, get the size of the file with `zipc_filesize`.
/// If `err` is not null, we will write status to it. This function is thread-safe.
/// Returns a struct describing the mapping; `data` will be null on failure.
zipc_mapping zipc_map_read(zipc* handle, const char* path, enum zipc_status* err);

/// Memory map to a new file inside the ZIP container for streaming write. `path` must
/// not already exist in the container. You must not write to the ZIP file in any other
/// way while having this memory map open. If `err` is not null, we will write status to
/// it. `max` is the maximum size of the file - it is safe to set this to some outlandishly
/// large value, but it needs to be a value that the virtual memory system can handle, so
/// do not use the full UINT64_MAX. Returns null on failure.
void* zipc_map_write(zipc* handle, const char* path, enum zipc_status* err, size_t max);

/// Unmap the memory mapped by `zipc_map_<type>`. Pass the mapping returned from
/// `zipc_map_read`. This function is thread-safe.
void zipc_unmap(zipc* handle, zipc_mapping mapping);

/// Create and write a new file inside the ZIP container.
enum zipc_status zipc_write(zipc* handle, const char* path, size_t size, const void* ptr);

/// Read the given number of bytes from a file from inside the ZIP container into
/// the given pointer.
enum zipc_status zipc_read(zipc* handle, const char* path, size_t size, void* ptr);

/// Open a stream for continous writing to a file inside a ZIP container that can be
/// used simultaneously with other read and write operations. Unlike all other calls
/// here, this is not guaranteed to be a zero-copy operation as we likely must use
/// temporary intermediate files. `mode` is reserved for future use. This function is
/// thread-safe. You are responsible for synchronizing thread access to the returned
/// handle. If `err` is not null, we will write status to it. Returns null on failure.
zipcstream* zipc_stream_open(zipc* handle, const char* path, const char* mode, enum zipc_status* err);

/// Write to our stream.
enum zipc_status zipc_stream_write(const zipc* handle, zipcstream* stream, size_t size, const void* ptr);

/// Close the write stream.
enum zipc_status zipc_stream_close(zipc* handle, zipcstream* stream);

#ifdef __cplusplus
}
#endif
