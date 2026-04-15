#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct zipc;
typedef struct zipc zipc;
struct zipcstream;
typedef struct zipcstream zipcstream;

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
};
typedef enum zipc_status zipc_status;

/// Return a string explaining one of our error codes.
const char* zipc_strerror(zipc_status err);

/// Open an uncompressed ZIP file. Mode is 'r' for reading, 'w' for writing (replacing
/// any existing file), or 'a' for appending. You are responsible for synchronizing
/// thread access to the returned handle unless otherwise specified below. You cannot
/// mix modes. If `err` is not null, we will write status to it. Returns null on failure.
zipc* zipc_open(const char* filename, const char* mode, enum zipc_status* err);

/// Close an open ZIP file handle. Returns ZIPC_SUCCESS on clean close.
enum zipc_status zipc_close(zipc* handle);

/// Obtain the filesize of a given file inside the ZIP container. Return -1 if the
/// file is not found. This function is thread-safe.
ssize_t zipc_filesize(zipc* handle, const char* path);

/// Validate the zip container and its contents. Returns ZIPC_SUCCESS if the container and its
/// contents look undamaged, otherwise a reason for failure.
enum zipc_status zipc_validate(zipc* handle);

struct zipc_mapping
{
	void* data;            // Pointer to file contents (read-only for map_read).
	size_t size;           // Size of the file data.
	const void* map_base;  // Base address of the mapping.
	size_t map_length;     // Length passed to mmap().
};
typedef struct zipc_mapping zipc_mapping;

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
/// do not use the full UINT64_MAX. Returns a struct describing the mapping; `data` will
/// be null on failure.
zipc_mapping zipc_map_write(zipc* handle, const char* path, enum zipc_status* err, size_t max);

/// Unmap the memory mapped by `zipc_map_read`. This function is thread-safe.
void zipc_unmap_read(zipc* handle, zipc_mapping mapping);

/// Finalize and unmap the memory mapped by `zipc_map_write`. `size` is the actual
/// number of bytes written (must be <= max). Returns a status code.
enum zipc_status zipc_unmap_write(zipc* handle, zipc_mapping mapping, size_t size);

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
