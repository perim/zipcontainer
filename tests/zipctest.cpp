#include "zipc.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <vector>

#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
static void test_punch_hole_support()
{
	char tmpl[] = "zipc_punch_XXXXXX";
	const int fd = mkstemp(tmpl);
	assert(fd >= 0);

	const size_t data_size = 2 * 1024 * 1024;
	std::vector<unsigned char> buf(4096, 0xAB);
	size_t remaining = data_size;
	while (remaining > 0)
	{
		const size_t chunk = remaining < buf.size() ? remaining : buf.size();
		const ssize_t w = write(fd, buf.data(), chunk);
		assert(w > 0);
		remaining -= (size_t)w;
	}
	assert(fsync(fd) == 0);

	struct stat st_before{};
	assert(fstat(fd, &st_before) == 0);
	if (st_before.st_blocks == 0)
	{
		fprintf(stderr, "punch-hole test: st_blocks is 0, skipping\n");
		close(fd);
		unlink(tmpl);
		return;
	}

	const int rc = fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, (off_t)data_size);
	if (rc != 0)
	{
		if (errno == EOPNOTSUPP || errno == ENOSYS || errno == ENOTTY || errno == EINVAL)
		{
			fprintf(stderr, "punch-hole test: unsupported, skipping\n");
			close(fd);
			unlink(tmpl);
			return;
		}
		assert(false);
	}

	struct stat st_after{};
	assert(fstat(fd, &st_after) == 0);
	assert(st_after.st_blocks < st_before.st_blocks);
	fprintf(stderr, "punch-hole test: supported (blocks %ld -> %ld)\n",
		(long)st_before.st_blocks, (long)st_after.st_blocks);

	close(fd);
	unlink(tmpl);
}
#endif

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
	zipc_unmap_read(z, map);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	zipc_close(z);

	// Stream write zip file
	const char* stream_zip_filename = "streamfile.zip";
	z = zipc_open(stream_zip_filename, "w", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	zipcstream* stream = zipc_stream_open(z, "stream.txt", "", &r);
	assert(r == ZIPC_SUCCESS);
	assert(stream);
	const char* part1 = "Stream ";
	const char* part2 = "write test";
	r = zipc_stream_write(z, stream, strlen(part1), part1);
	assert(r == ZIPC_SUCCESS);
	r = zipc_stream_write(z, stream, strlen(part2), part2);
	assert(r == ZIPC_SUCCESS);
	r = zipc_stream_close(z, stream);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	stream = zipc_stream_open(z, "stream.txt", "", &r);
	assert(stream == nullptr);
	assert(r == ZIPC_PATH_ALREADY_EXISTS);
	zipc_close(z);

	z = zipc_open(stream_zip_filename, "r", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	const char* full_stream = "Stream write test";
	char stream_readback[128];
	memset(stream_readback, 0, sizeof(stream_readback));
	r = zipc_read(z, "stream.txt", strlen(full_stream), stream_readback);
	assert(r == ZIPC_SUCCESS);
	assert(strncmp(full_stream, stream_readback, strlen(full_stream)) == 0);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	zipc_close(z);

#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
	test_punch_hole_support();
#endif

	// Map write zip file
	const char* map_zip_filename = "mapwrite.zip";
	z = zipc_open(map_zip_filename, "w", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	const char* map_content = "Map write content";
	const size_t map_max = 1024;
	zipc_mapping map_write = zipc_map_write(z, "map.txt", &r, map_max);
	assert(r == ZIPC_SUCCESS);
	assert(map_write.data);
	memcpy(map_write.data, map_content, strlen(map_content));
	map_write.size = strlen(map_content);
	r = zipc_unmap_write(z, map_write, map_write.size);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	zipc_close(z);

	z = zipc_open(map_zip_filename, "r", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	assert(zipc_filesize(z, "map.txt") == (ssize_t)strlen(map_content));
	char map_readback[128];
	memset(map_readback, 0, sizeof(map_readback));
	r = zipc_read(z, "map.txt", strlen(map_content), map_readback);
	assert(r == ZIPC_SUCCESS);
	assert(strncmp(map_content, map_readback, strlen(map_content)) == 0);
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

	// TBD test zipc_stream_*() calls
	//zipcstream* zipc_stream_open(zipc* handle, const char* path, const char* mode);
	//int zipc_stream_write(zipcstream* handle, const char* path, size_t size, void* ptr);
	//int zipc_stream_close(zipcstream* handle);
}
