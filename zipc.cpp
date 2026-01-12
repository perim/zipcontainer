#include "zipc.h"

#include <assert.h>
#include <string.h>

#include <string>
#include <unordered_map>

// Private definitions

enum zipc_mode
{
	ZIPC_READ_ONLY,
	ZIPC_WRITE_ONLY,
	ZIPC_APPEND,
};

struct filenode
{
	size_t size;
	size_t offset;
};

struct zipc
{
	std::string filename;
	zipc_mode mode;
	std::unordered_map<std::string, filenode> files;
};

struct zipcstream
{
	zipc* parent = nullptr;
};

// Implementations

zipc* zipc_open(const char* filename, const char* mode, enum zipc_status* err)
{
	assert(filename);
	assert(mode);
	zipc* z = new zipc();
	z->filename = filename;
	if (!mode || strlen(mode) != 1) { if (err) *err = ZIPC_SYNTAX_ERROR; return nullptr; }
	if (mode[0] == 'r') z->mode = ZIPC_READ_ONLY;
	else if (mode[0] == 'w') z->mode = ZIPC_WRITE_ONLY;
	else if (mode[0] == 'a') z->mode = ZIPC_APPEND;
	else { if (err) *err = ZIPC_SYNTAX_ERROR; return nullptr; }

	// TBD open zip file
	// TBD verify that it is uncompressed
	// TBD read out its file dictionary and store in z->files

	if (err) *err = ZIPC_SUCCESS;
	return z;
}

void zipc_close(zipc* handle)
{
	if (handle) delete handle;
}

ssize_t zipc_filesize(zipc* handle, const char* path)
{
	assert(handle);
	assert(path);
	if (handle->files.count(path) == 0) return -1;
	const auto& v = handle->files.at(path);
	return v.size;
}

const void* zipc_map_read(zipc* handle, const char* path, enum zipc_status* err)
{
	assert(handle);
	assert(path);
	// TBD
	if (err) *err = ZIPC_SUCCESS;
	return nullptr;
}

void* zipc_map_write(zipc* handle, const char* path, enum zipc_status* err, size_t max)
{
	assert(handle);
	assert(path);
	// TBD
	if (err) *err = ZIPC_SUCCESS;
	return nullptr;
}

void zipc_unmap(zipc* handle, void* ptr)
{
	assert(handle);
	// TBD
}

enum zipc_status zipc_write(zipc* handle, const char* path, size_t size, const void* ptr)
{
	assert(handle);
	assert(path);
	// TBD
	return ZIPC_SUCCESS;
}

enum zipc_status zipc_read(zipc* handle, const char* path, size_t size, void* ptr)
{
	assert(handle);
	assert(path);
	// TBD
	return ZIPC_SUCCESS;
}

zipcstream* zipc_stream_open(zipc* handle, const char* path, const char* mode, enum zipc_status* err)
{
	assert(handle);
	assert(path);
	zipcstream* c = new zipcstream();
	// TBD
	if (err) *err = ZIPC_SUCCESS;
	return c;
}

enum zipc_status zipc_stream_write(const zipc* handle, zipcstream* stream, const char* path, size_t size, const void* ptr)
{
	assert(handle);
	assert(stream);
	// TBD
	return ZIPC_SUCCESS;
}

enum zipc_status zipc_stream_close(zipc* handle, zipcstream* stream)
{
	assert(handle);
	assert(stream);
	// TBD use sendfile() to efficiently move temporary file into ZIP
	delete stream;
	return ZIPC_SUCCESS;
}
