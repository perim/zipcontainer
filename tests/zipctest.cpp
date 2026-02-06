#include "zipc.h"

#include <assert.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;
	enum zipc_status r = ZIPC_SUCCESS;
	(void)r;

	// Create zip file
	const char* zip_filename = "testfile.zip";
	zipc* z = zipc_open(zip_filename, "w", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	ssize_t size = zipc_filesize(z, "nonexistent.file");
	assert(size == -1);
	const char* internal_filename = "testcontent.txt";
	const char* content = "This is a piece of content";
	r = zipc_write(z, internal_filename, strlen(content), content);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_filesize(z, internal_filename) == (ssize_t)strlen(content));
	zipc_close(z);

	// Read zip file just created
	z = zipc_open("testfile.zip", "r", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	assert(zipc_filesize(z, internal_filename) == (ssize_t)strlen(content));
	char readback[128];
	memset(readback, 0, sizeof(readback));
	r = zipc_read(z, internal_filename, strlen(content), readback);
	assert(r == ZIPC_SUCCESS);
	assert(strncmp(content, readback, strlen(content)) == 0);
	zipc_mapping map = zipc_map_read(z, internal_filename, &r);
	assert(r == ZIPC_SUCCESS);
	assert(map.data);
	assert(map.size == strlen(content));
	assert(strncmp(content, (const char*)map.data, map.size) == 0);
	assert(((const char*)map.data)[map.size - 1] == content[strlen(content) - 1]);
	zipc_unmap(z, map);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	zipc_close(z);

	// Test existing zip files
	z = zipc_open(TEXT_FILES_ZIP, "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);
	size = zipc_filesize(z, "first.txt");
	assert(size != -1);
	size = zipc_filesize(z, "second.txt");
	assert(size != -1);
	size = zipc_filesize(z, "third.txt");
	assert(size != -1);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	zipc_close(z);

	// This one was created slightly different and has a comment
	z = zipc_open(TEXT_FILES2_ZIP, "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);
	size = zipc_filesize(z, "first.txt");
	assert(size != -1);
	size = zipc_filesize(z, "second.txt");
	assert(size != -1);
	size = zipc_filesize(z, "third.txt");
	assert(size != -1);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	zipc_close(z);

	// Compressed files are expected to fail
	z = zipc_open(COMPRESSED_ZIP, "r", &r);
	assert(z == nullptr);
	assert(r == ZIPC_UNSUPPORTED_FEATURE);

	// Encrypted files are expected to fail
	z = zipc_open(ENCRYPTED_ZIP, "r", &r);
	assert(z == nullptr);
	assert(r == ZIPC_UNSUPPORTED_FEATURE);

	// TBD test zipc_map with write mode

	// TBD test zipc_stream_*() calls
	//zipcstream* zipc_stream_open(zipc* handle, const char* path, const char* mode);
	//int zipc_stream_write(zipcstream* handle, const char* path, size_t size, void* ptr);
	//int zipc_stream_close(zipcstream* handle);
}
