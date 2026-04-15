#include "zipc.h"
#include "zipc_utility.h"

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

struct test_entry
{
	const char* path;
	const char* content;
};

static void create_test_archive(const char* filename, const test_entry* entries, size_t count)
{
	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(filename, "w", &status);
	assert(status == ZIPC_SUCCESS);
	assert(z);
	for (size_t i = 0; i < count; ++i)
	{
		status = zipc_write(z, entries[i].path, strlen(entries[i].content), entries[i].content);
		assert(status == ZIPC_SUCCESS);
	}
	status = zipc_close(z);
	assert(status == ZIPC_SUCCESS);
}

static const zipc_file_diff* find_diff(const zipc_comparison* comparison, const char* name)
{
	assert(comparison);
	for (int i = 0; i < comparison->count; ++i)
	{
		if (strcmp(comparison->differences[i].name, name) == 0) return &comparison->differences[i];
	}
	return nullptr;
}

static void test_compare_utility()
{
	const test_entry first_entries[] = {
		{"same.txt", "same content"},
		{"diff.txt", "abcXef"},
		{"only-first.txt", "first side"},
		{"prefix.txt", "prefix"},
	};
	const test_entry second_entries[] = {
		{"same.txt", "same content"},
		{"diff.txt", "abcYef"},
		{"only-second.txt", "second side"},
		{"prefix.txt", "prefix and more"},
	};

	create_test_archive("compare_first.zip", first_entries, sizeof(first_entries) / sizeof(first_entries[0]));
	create_test_archive("compare_second.zip", second_entries, sizeof(second_entries) / sizeof(second_entries[0]));

	const zipc_comparison* comparison = zipc_compare("compare_first.zip", "compare_second.zip");
	assert(comparison);
	assert(comparison->status == ZIPC_SUCCESS);
	assert(comparison->count == 4);

	const zipc_file_diff* diff = find_diff(comparison, "diff.txt");
	assert(diff);
	assert(diff->kind == ZIPC_DIFF_CONTENT);
	assert(diff->size_first == strlen("abcXef"));
	assert(diff->size_second == strlen("abcYef"));
	assert(diff->offset_first_diff == 3);

	diff = find_diff(comparison, "only-first.txt");
	assert(diff);
	assert(diff->kind == ZIPC_DIFF_ONLY_IN_FIRST);
	assert(diff->size_first == strlen("first side"));
	assert(diff->size_second == 0);

	diff = find_diff(comparison, "only-second.txt");
	assert(diff);
	assert(diff->kind == ZIPC_DIFF_ONLY_IN_SECOND);
	assert(diff->size_first == 0);
	assert(diff->size_second == strlen("second side"));

	diff = find_diff(comparison, "prefix.txt");
	assert(diff);
	assert(diff->kind == ZIPC_DIFF_CONTENT);
	assert(diff->size_first == strlen("prefix"));
	assert(diff->size_second == strlen("prefix and more"));
	assert(diff->offset_first_diff == strlen("prefix"));

	assert(find_diff(comparison, "same.txt") == nullptr);
	zipc_compare_free(comparison);

	comparison = zipc_compare("compare_first.zip", "compare_first.zip");
	assert(comparison);
	assert(comparison->status == ZIPC_SUCCESS);
	assert(comparison->count == 0);
	assert(comparison->differences == nullptr);
	zipc_compare_free(comparison);

	comparison = zipc_compare(nullptr, "compare_second.zip");
	assert(comparison);
	assert(comparison->status == ZIPC_SYNTAX_ERROR);
	zipc_compare_free(comparison);
}

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
	test_punch_hole_support();
#endif

	test_compare_utility();

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

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
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

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
