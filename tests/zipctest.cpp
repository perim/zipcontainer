#include "zipc.h"

#include <assert.h>
#include <string.h>

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
	//TBD assert(zipc_filesize(z, internal_filename) == (ssize_t)strlen(content));
	zipc_close(z);

	// Read zip file just created
	z = zipc_open("testfile.zip", "r", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	//TBD assert(zipc_filesize(z, internal_filename) == (ssize_t)strlen(content));
	char readback[128];
	memset(readback, 0, sizeof(readback));
	r = zipc_read(z, internal_filename, strlen(content), readback);
	assert(r == ZIPC_SUCCESS);
	//TBD assert(strncmp(content, readback, strlen(content)) == 0);
	char* m = (char*)zipc_map_read(z, internal_filename, &r);
	assert(r == ZIPC_SUCCESS);
	//TBD assert(strncmp(content, m, strlen(content)) == 0);
	zipc_unmap(z, m);
	zipc_close(z);

	// TBD test zipc_map with write mode

	// TBD test zipc_stream_*() calls
	//zipcstream* zipc_stream_open(zipc* handle, const char* path, const char* mode);
	//int zipc_stream_write(zipcstream* handle, const char* path, size_t size, void* ptr);
	//int zipc_stream_close(zipcstream* handle);
}
