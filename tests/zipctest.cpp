#include "zipc.h"
#include "zipc_utility.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <string>
#include <vector>

#ifdef NDEBUG
#if __clang__
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#else
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#endif
#endif

static uint16_t read_le16(const unsigned char* ptr)
{
	return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8U);
}

static uint32_t read_le32(const unsigned char* ptr)
{
	return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8U) | ((uint32_t)ptr[2] << 16U) | ((uint32_t)ptr[3] << 24U);
}

static uint64_t read_le64(const unsigned char* ptr)
{
	return (uint64_t)ptr[0] |
		((uint64_t)ptr[1] << 8U) |
		((uint64_t)ptr[2] << 16U) |
		((uint64_t)ptr[3] << 24U) |
		((uint64_t)ptr[4] << 32U) |
		((uint64_t)ptr[5] << 40U) |
		((uint64_t)ptr[6] << 48U) |
		((uint64_t)ptr[7] << 56U);
}

static void write_le16(unsigned char* ptr, uint16_t value)
{
	ptr[0] = (unsigned char)(value & 0xFFU);
	ptr[1] = (unsigned char)((value >> 8U) & 0xFFU);
}

static void write_le32(unsigned char* ptr, uint32_t value)
{
	ptr[0] = (unsigned char)(value & 0xFFU);
	ptr[1] = (unsigned char)((value >> 8U) & 0xFFU);
	ptr[2] = (unsigned char)((value >> 16U) & 0xFFU);
	ptr[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void write_le64(unsigned char* ptr, uint64_t value)
{
	ptr[0] = (unsigned char)(value & 0xFFU);
	ptr[1] = (unsigned char)((value >> 8U) & 0xFFU);
	ptr[2] = (unsigned char)((value >> 16U) & 0xFFU);
	ptr[3] = (unsigned char)((value >> 24U) & 0xFFU);
	ptr[4] = (unsigned char)((value >> 32U) & 0xFFU);
	ptr[5] = (unsigned char)((value >> 40U) & 0xFFU);
	ptr[6] = (unsigned char)((value >> 48U) & 0xFFU);
	ptr[7] = (unsigned char)((value >> 56U) & 0xFFU);
}

static bool write_all_test(FILE* fp, const void* data, size_t size)
{
	return fwrite(data, 1, size, fp) == size;
}

static void write_text_file(const char* filename, const char* content)
{
	FILE* fp = fopen(filename, "wb");
	assert(fp);
	assert(write_all_test(fp, content, strlen(content)));
	assert(fclose(fp) == 0);
}

static std::string read_text_file(const char* filename)
{
	FILE* fp = fopen(filename, "rb");
	assert(fp);
	std::string result;
	unsigned char buf[4096];
	for (;;)
	{
		const size_t bytes = fread(buf, 1, sizeof(buf), fp);
		result.append((const char*)buf, bytes);
		if (bytes < sizeof(buf)) break;
	}
	assert(!ferror(fp));
	assert(fclose(fp) == 0);
	return result;
}

static std::vector<unsigned char> read_binary_file(const char* filename)
{
	FILE* fp = fopen(filename, "rb");
	assert(fp);
	std::vector<unsigned char> result;
	unsigned char buf[4096];
	for (;;)
	{
		const size_t bytes = fread(buf, 1, sizeof(buf), fp);
		result.insert(result.end(), buf, buf + bytes);
		if (bytes < sizeof(buf)) break;
	}
	assert(!ferror(fp));
	assert(fclose(fp) == 0);
	return result;
}

static void write_binary_file(const char* filename, const std::vector<unsigned char>& data)
{
	FILE* fp = fopen(filename, "wb");
	assert(fp);
	const bool wrote = data.empty() || write_all_test(fp, data.data(), data.size());
	assert(wrote);
	assert(fclose(fp) == 0);
}

static off_t file_size_on_disk(const char* filename)
{
	struct stat st{};
	assert(stat(filename, &st) == 0);
	return st.st_size;
}

static uint32_t crc32_test(const void* data, size_t size)
{
	uint32_t crc = 0xFFFFFFFFU;
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < size; ++i)
	{
		crc ^= p[i];
		for (int bit = 0; bit < 8; ++bit)
		{
			if (crc & 1U) crc = 0xEDB88320U ^ (crc >> 1U);
			else crc >>= 1U;
		}
	}
	return crc ^ 0xFFFFFFFFU;
}

static bool file_contains_signature(const char* filename, uint32_t signature)
{
	FILE* fp = fopen(filename, "rb");
	assert(fp);
	std::vector<unsigned char> data;
	unsigned char buf[4096];
	for (;;)
	{
		const size_t n = fread(buf, 1, sizeof(buf), fp);
		data.insert(data.end(), buf, buf + n);
		if (n < sizeof(buf)) break;
	}
	fclose(fp);

	for (size_t i = 0; i + 4 <= data.size(); ++i)
	{
		const uint32_t value = (uint32_t)data[i] |
			((uint32_t)data[i + 1] << 8U) |
			((uint32_t)data[i + 2] << 16U) |
			((uint32_t)data[i + 3] << 24U);
		if (value == signature) return true;
	}
	return false;
}

static size_t count_file_signature(const char* filename, uint32_t signature)
{
	const std::vector<unsigned char> data = read_binary_file(filename);
	size_t count = 0;
	for (size_t i = 0; i + 4 <= data.size(); ++i)
	{
		const uint32_t value = (uint32_t)data[i] |
			((uint32_t)data[i + 1] << 8U) |
			((uint32_t)data[i + 2] << 16U) |
			((uint32_t)data[i + 3] << 24U);
		if (value == signature) ++count;
	}
	return count;
}

static size_t terminal_eocd_offset(const std::vector<unsigned char>& data)
{
	assert(data.size() >= 22);
	for (size_t i = data.size() - 22;; --i)
	{
		if (read_le32(data.data() + i) == 0x06054b50)
		{
			const uint16_t comment_len = read_le16(data.data() + i + 20);
			if (i + 22U + (size_t)comment_len == data.size()) return i;
		}
		if (i == 0) break;
	}
	assert(false);
	return 0;
}

static size_t terminal_central_directory_offset(const std::vector<unsigned char>& data)
{
	const size_t eocd = terminal_eocd_offset(data);
	assert(eocd + 22 <= data.size());
	if (eocd >= 20 && read_le32(data.data() + eocd - 20) == 0x07064b50)
	{
		const uint64_t zip64_eocd = read_le64(data.data() + eocd - 20 + 8);
		assert(data.size() >= 56U && zip64_eocd <= (uint64_t)data.size() - 56U);
		assert(read_le32(data.data() + (size_t)zip64_eocd) == 0x06064b50);
		const uint64_t cd_offset = read_le64(data.data() + (size_t)zip64_eocd + 48);
		assert(cd_offset <= (uint64_t)data.size());
		return (size_t)cd_offset;
	}

	const uint32_t cd_offset = read_le32(data.data() + eocd + 16);
	assert(cd_offset <= data.size());
	return cd_offset;
}

static uint64_t checked_filesize(zipc* z, const char* path)
{
	uint64_t size = 0;
	const enum zipc_status status = zipc_filesize(z, path, &size);
	assert(status == ZIPC_SUCCESS);
	return size;
}

static void create_sparse_zip64_offset_archive(const char* filename)
{
	const char* name = "huge-offset.txt";
	const char* content = "zip64 offset sentinel";
	const size_t name_len = strlen(name);
	const size_t data_size = strlen(content);
	const uint64_t local_offset = 0x100000000ULL;
	const uint32_t crc = crc32_test(content, data_size);

	FILE* fp = fopen(filename, "wb+");
	assert(fp);
	assert(fseeko(fp, (off_t)local_offset, SEEK_SET) == 0);

	unsigned char local[30] = {0};
	write_le32(local, 0x04034b50);
	write_le16(local + 4, 45);
	write_le32(local + 14, crc);
	write_le32(local + 18, 0xFFFFFFFFU);
	write_le32(local + 22, 0xFFFFFFFFU);
	write_le16(local + 26, (uint16_t)name_len);
	write_le16(local + 28, 20);
	unsigned char local_extra[20] = {0};
	write_le16(local_extra, 0x0001);
	write_le16(local_extra + 2, 16);
	write_le64(local_extra + 4, data_size);
	write_le64(local_extra + 12, data_size);
	assert(write_all_test(fp, local, sizeof(local)));
	assert(write_all_test(fp, name, name_len));
	assert(write_all_test(fp, local_extra, sizeof(local_extra)));
	assert(write_all_test(fp, content, data_size));

	const uint64_t cd_offset = (uint64_t)ftello(fp);
	unsigned char cd[46] = {0};
	write_le32(cd, 0x02014b50);
	write_le16(cd + 4, 45);
	write_le16(cd + 6, 45);
	write_le32(cd + 16, crc);
	write_le32(cd + 20, 0xFFFFFFFFU);
	write_le32(cd + 24, 0xFFFFFFFFU);
	write_le16(cd + 28, (uint16_t)name_len);
	write_le16(cd + 30, 28);
	write_le32(cd + 42, 0xFFFFFFFFU);
	unsigned char cd_extra[28] = {0};
	write_le16(cd_extra, 0x0001);
	write_le16(cd_extra + 2, 24);
	write_le64(cd_extra + 4, data_size);
	write_le64(cd_extra + 12, data_size);
	write_le64(cd_extra + 20, local_offset);
	assert(write_all_test(fp, cd, sizeof(cd)));
	assert(write_all_test(fp, name, name_len));
	assert(write_all_test(fp, cd_extra, sizeof(cd_extra)));

	const uint64_t cd_size = (uint64_t)ftello(fp) - cd_offset;
	const uint64_t zip64_eocd_offset = (uint64_t)ftello(fp);
	unsigned char eocd64[56] = {0};
	write_le32(eocd64, 0x06064b50);
	write_le64(eocd64 + 4, 44);
	write_le16(eocd64 + 12, 45);
	write_le16(eocd64 + 14, 45);
	write_le64(eocd64 + 24, 1);
	write_le64(eocd64 + 32, 1);
	write_le64(eocd64 + 40, cd_size);
	write_le64(eocd64 + 48, cd_offset);
	assert(write_all_test(fp, eocd64, sizeof(eocd64)));

	unsigned char locator[20] = {0};
	write_le32(locator, 0x07064b50);
	write_le64(locator + 8, zip64_eocd_offset);
	write_le32(locator + 16, 1);
	assert(write_all_test(fp, locator, sizeof(locator)));

	unsigned char eocd[22] = {0};
	write_le32(eocd, 0x06054b50);
	write_le16(eocd + 8, 0xFFFFU);
	write_le16(eocd + 10, 0xFFFFU);
	write_le32(eocd + 12, 0xFFFFFFFFU);
	write_le32(eocd + 16, 0xFFFFFFFFU);
	assert(write_all_test(fp, eocd, sizeof(eocd)));
	assert(fclose(fp) == 0);
}

static void create_saturated_eocd_without_locator(const char* filename)
{
	FILE* fp = fopen(filename, "wb");
	assert(fp);
	unsigned char eocd[22] = {0};
	write_le32(eocd, 0x06054b50);
	write_le16(eocd + 8, 0xFFFFU);
	write_le16(eocd + 10, 0xFFFFU);
	write_le32(eocd + 12, 0xFFFFFFFFU);
	write_le32(eocd + 16, 0xFFFFFFFFU);
	assert(write_all_test(fp, eocd, sizeof(eocd)));
	assert(fclose(fp) == 0);
}

static void create_zip32_max_entries_archive(const char* filename)
{
	const uint32_t entry_count = 0xFFFFU;
	const size_t name_len = 6;

	FILE* fp = fopen(filename, "wb");
	assert(fp);

	for (uint32_t i = 0; i < entry_count; ++i)
	{
		char name[7];
		snprintf(name, sizeof(name), "e%05u", i);
		unsigned char local[30] = {0};
		write_le32(local, 0x04034b50);
		write_le16(local + 4, 20);
		write_le16(local + 26, (uint16_t)name_len);
		assert(write_all_test(fp, local, sizeof(local)));
		assert(write_all_test(fp, name, name_len));
	}

	const uint32_t cd_offset = entry_count * (uint32_t)(30 + name_len);
	for (uint32_t i = 0; i < entry_count; ++i)
	{
		char name[7];
		snprintf(name, sizeof(name), "e%05u", i);
		unsigned char cd[46] = {0};
		write_le32(cd, 0x02014b50);
		write_le16(cd + 4, 20);
		write_le16(cd + 6, 20);
		write_le16(cd + 28, (uint16_t)name_len);
		write_le32(cd + 42, i * (uint32_t)(30 + name_len));
		assert(write_all_test(fp, cd, sizeof(cd)));
		assert(write_all_test(fp, name, name_len));
	}

	const uint32_t cd_size = entry_count * (uint32_t)(46 + name_len);
	unsigned char eocd[22] = {0};
	write_le32(eocd, 0x06054b50);
	write_le16(eocd + 8, 0xFFFFU);
	write_le16(eocd + 10, 0xFFFFU);
	write_le32(eocd + 12, cd_size);
	write_le32(eocd + 16, cd_offset);
	assert(write_all_test(fp, eocd, sizeof(eocd)));
	assert(fclose(fp) == 0);
}

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

static void assert_archive_entries(const char* filename, const test_entry* entries, size_t count)
{
	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(filename, "r", &status);
	assert(status == ZIPC_SUCCESS);
	assert(z);
	for (size_t i = 0; i < count; ++i)
	{
		const uint64_t size = checked_filesize(z, entries[i].path);
		assert(size == strlen(entries[i].content));
		std::vector<char> readback((size_t)size + 1, '\0');
		status = zipc_read(z, entries[i].path, size, readback.data());
		assert(status == ZIPC_SUCCESS);
		assert(memcmp(readback.data(), entries[i].content, (size_t)size) == 0);
	}
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	status = zipc_close(z);
	assert(status == ZIPC_SUCCESS);
}

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

static void assert_corrupt_archive(const char* filename)
{
	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(filename, "r", &status);
	assert(z == nullptr);
	assert(status == ZIPC_CORRUPT_ARCHIVE);
}

static void test_validate_rechecks_terminal_directory()
{
	const char* filename = "validate-terminal-corrupt.zip";
	unlink(filename);

	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(filename, "w", &status);
	assert(status == ZIPC_SUCCESS);
	assert(z);
	status = zipc_write(z, "stable.txt", strlen("stable content"), "stable content");
	assert(status == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);

	std::vector<unsigned char> data = read_binary_file(filename);
	const size_t cd_offset = terminal_central_directory_offset(data);
	std::vector<unsigned char> missing_cd = data;
	missing_cd.resize(cd_offset);
	write_binary_file(filename, missing_cd);
	assert(zipc_validate(z) == ZIPC_CORRUPT_ARCHIVE);
	status = zipc_close(z);
	assert(status == ZIPC_SUCCESS);

	z = zipc_open(filename, "w", &status);
	assert(status == ZIPC_SUCCESS);
	assert(z);
	status = zipc_write(z, "stable.txt", strlen("stable content"), "stable content");
	assert(status == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);

	data = read_binary_file(filename);
	const size_t damaged_cd_offset = terminal_central_directory_offset(data);
	data[damaged_cd_offset] ^= 0xFFU;
	write_binary_file(filename, data);
	assert(zipc_validate(z) == ZIPC_CORRUPT_ARCHIVE);
	status = zipc_close(z);
	assert(status == ZIPC_SUCCESS);
}

static void test_stream_api_edge_cases()
{
	const char* stream_test_zip = "stream-edge-cases.zip";
	unlink(stream_test_zip);

	enum zipc_status r = ZIPC_SUCCESS;
	zipc* z = zipc_open(stream_test_zip, "w", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);

	// Invalid mode string
	zipcstream* stream = zipc_stream_open(z, "test.txt", "w", &r);
	assert(stream == nullptr);
	assert(r == ZIPC_UNSUPPORTED_FEATURE);

	// Valid stream open
	stream = zipc_stream_open(z, "test.txt", "", &r);
	assert(stream);
	assert(r == ZIPC_SUCCESS);

	// Write with null pointer but non-zero size
	r = zipc_stream_write(z, stream, 10, nullptr);
	assert(r == ZIPC_SYNTAX_ERROR);

	// Write with zero size (should succeed and do nothing)
	r = zipc_stream_write(z, stream, 0, nullptr);
	assert(r == ZIPC_SUCCESS);

	r = zipc_stream_close(z, stream);
	assert(r == ZIPC_SUCCESS);

	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	// Test opening stream in read-only mode
	z = zipc_open(stream_test_zip, "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);
	stream = zipc_stream_open(z, "readonly.txt", "", &r);
	assert(stream == nullptr);
	assert(r == ZIPC_PERMISSION_FAILURE);

	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);
	unlink(stream_test_zip);
}

static void test_append_only_safety()
{
	const char* filename = "append-only.zip";
	unlink(filename);

	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(filename, "w", &status);
	assert(status == ZIPC_SUCCESS);
	assert(z);

	status = zipc_write(z, "direct.txt", strlen("direct content"), "direct content");
	assert(status == ZIPC_SUCCESS);
	const off_t direct_size = file_size_on_disk(filename);
	assert(count_file_signature(filename, 0x02014b50) == 1);

	zipc_mapping mapping = zipc_map_write(z, "mapped.txt", &status, 256);
	assert(status == ZIPC_SUCCESS);
	assert(mapping.data);
	const char* mapped_content = "mapped content";
	memcpy(mapping.data, mapped_content, strlen(mapped_content));
	status = zipc_unmap_write(z, mapping, strlen(mapped_content));
	assert(status == ZIPC_SUCCESS);
	const off_t mapped_size = file_size_on_disk(filename);
	assert(mapped_size > direct_size);
	assert(count_file_signature(filename, 0x02014b50) == 3);
	assert(zipc_validate(z) == ZIPC_SUCCESS);

	zipcstream* stream = zipc_stream_open(z, "streamed.txt", "", &status);
	assert(status == ZIPC_SUCCESS);
	assert(stream);
	status = zipc_stream_write(z, stream, strlen("streamed "), "streamed ");
	assert(status == ZIPC_SUCCESS);
	status = zipc_stream_write(z, stream, strlen("content"), "content");
	assert(status == ZIPC_SUCCESS);
	status = zipc_stream_close(z, stream);
	assert(status == ZIPC_SUCCESS);
	const off_t streamed_size = file_size_on_disk(filename);
	assert(streamed_size > mapped_size);
	assert(count_file_signature(filename, 0x02014b50) == 6);
	assert(zipc_validate(z) == ZIPC_SUCCESS);

	status = zipc_close(z);
	assert(status == ZIPC_SUCCESS);

	z = zipc_open(filename, "a", &status);
	assert(status == ZIPC_SUCCESS);
	assert(z);
	status = zipc_write(z, "appended.txt", strlen("append mode"), "append mode");
	assert(status == ZIPC_SUCCESS);
	assert(file_size_on_disk(filename) > streamed_size);
	assert(count_file_signature(filename, 0x02014b50) == 10);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	status = zipc_close(z);
	assert(status == ZIPC_SUCCESS);

	const test_entry entries[] = {
		{"direct.txt", "direct content"},
		{"mapped.txt", "mapped content"},
		{"streamed.txt", "streamed content"},
		{"appended.txt", "append mode"},
	};
	assert_archive_entries(filename, entries, sizeof(entries) / sizeof(entries[0]));

	std::vector<unsigned char> damaged = read_binary_file(filename);
	const size_t cd_offset = terminal_central_directory_offset(damaged);
	std::vector<unsigned char> missing_cd = damaged;
	missing_cd.resize(cd_offset);
	write_binary_file("append-only-missing-cd.zip", missing_cd);
	assert_corrupt_archive("append-only-missing-cd.zip");

	damaged[cd_offset] ^= 0xFFU;
	write_binary_file("append-only-damaged-cd.zip", damaged);
	assert_corrupt_archive("append-only-damaged-cd.zip");

	// Simulate a mid-write crash: a complete valid archive with a partial
	// local file header appended after the EOCD (as if a new write started
	// but the process died before the new central directory was written).
	std::vector<unsigned char> partial_write = read_binary_file(filename);
	const unsigned char junk[] = { 0x50, 0x4B, 0x03, 0x04, 0xDE, 0xAD, 0xBE, 0xEF };
	partial_write.insert(partial_write.end(), junk, junk + sizeof(junk));
	write_binary_file("append-only-partial-write.zip", partial_write);
	assert_corrupt_archive("append-only-partial-write.zip");
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

	std::vector<std::string> packed = zipc_files("compare_first.zip");
	assert(packed.size() == 4);
	assert(packed[0] == "diff.txt");
	assert(packed[1] == "only-first.txt");
	assert(packed[2] == "prefix.txt");
	assert(packed[3] == "same.txt");

	packed = zipc_files("compare_first.zip", "only-");
	assert(packed.size() == 1);
	assert(packed[0] == "only-first.txt");

	packed = zipc_files("compare_first.zip", "missing/");
	assert(packed.empty());
	packed = zipc_files("does-not-exist.zip");
	assert(packed.empty());

	unlink("utility.zip");
	unlink("utility-source.txt");
	write_text_file("utility-source.txt", "utility file contents");
	assert(zipc_add_file("utility.zip", "utility-source.txt") == ZIPC_SUCCESS);
	assert(zipc_add_file("utility.zip", "utility-source.txt") == ZIPC_PATH_ALREADY_EXISTS);
	packed = zipc_files("utility.zip");
	assert(packed.size() == 1);
	assert(packed[0] == "utility-source.txt");
	write_text_file("utility-source.txt", "overwritten before extract");
	assert(zipc_extract_file("utility.zip", "utility-source.txt") == ZIPC_SUCCESS);
	assert(read_text_file("utility-source.txt") == "utility file contents");
	assert(zipc_extract_file("utility.zip", "missing-utility.txt") == ZIPC_PATH_NOT_FOUND);
	assert(zipc_add_file("utility.zip", "missing-utility.txt") == ZIPC_PATH_NOT_FOUND);
	const off_t utility_zip_size = file_size_on_disk("utility.zip");
	assert(zipc_extract_file("utility.zip", "utility.zip") == ZIPC_PATH_NOT_FOUND);
	assert(file_size_on_disk("utility.zip") == utility_zip_size);

	// Test path safety in zipc_extract_file and zipc_add_file
	assert(zipc_extract_file("utility.zip", "../evil.txt") == ZIPC_SYNTAX_ERROR);
	assert(zipc_extract_file("utility.zip", "/etc/passwd") == ZIPC_SYNTAX_ERROR);
	assert(zipc_add_file("utility.zip", "../evil.txt") == ZIPC_SYNTAX_ERROR);
	assert(zipc_add_file("utility.zip", "/etc/passwd") == ZIPC_SYNTAX_ERROR);

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

static void test_open_fd()
{
	enum zipc_status r = ZIPC_SUCCESS;
	const char* filename = "fdtest.zip";
	unlink(filename);

	// 1. Create a zip archive using a file descriptor (with ownership)
	int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0666);
	assert(fd >= 0);

	zipc* z = zipc_open_fd(fd, "w", 1, &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);

	const char* content = "FD open content test";
	r = zipc_write(z, "fd_file.txt", strlen(content), content);
	assert(r == ZIPC_SUCCESS);

	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	// 2. Open it back up for reading using a file descriptor (without ownership)
	fd = open(filename, O_RDONLY);
	assert(fd >= 0);

	z = zipc_open_fd(fd, "r", 0, &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);

	uint64_t size = 0;
	r = zipc_filesize(z, "fd_file.txt", &size);
	assert(r == ZIPC_SUCCESS);
	assert(size == strlen(content));

	char readback[128];
	memset(readback, 0, sizeof(readback));
	r = zipc_read(z, "fd_file.txt", size, readback);
	assert(r == ZIPC_SUCCESS);
	assert(strcmp(readback, content) == 0);

	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	// fd should still be open since close_fd was 0
	int flags = fcntl(fd, F_GETFL);
	assert(flags >= 0);
	close(fd);

	unlink(filename);
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
	uint64_t size = 0;
	assert(zipc_filesize(z, "nonexistent.file", &size) == ZIPC_PATH_NOT_FOUND);
	const char* internal_filename = "testcontent.txt";
	const char* content = "This is a piece of content";
	r = zipc_write(z, internal_filename, strlen(content), content);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_filesize(z, internal_filename, &size) == ZIPC_SUCCESS);
	assert(size == strlen(content));
	assert(checked_filesize(z, internal_filename) == strlen(content));
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);
	assert(file_contains_signature(zip_filename, 0x06064b50));
	assert(file_contains_signature(zip_filename, 0x07064b50));

	// Read zip file just created
	z = zipc_open("testfile.zip", "r", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	assert(checked_filesize(z, internal_filename) == strlen(content));
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
	test_open_fd();
	test_append_only_safety();
	test_validate_rechecks_terminal_directory();

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
	zipc_mapping empty_map_write = zipc_map_write(z, "empty-map.txt", &r, 0);
	assert(r == ZIPC_SUCCESS);
	assert(empty_map_write.data);
	assert(empty_map_write.size == 0);
	r = zipc_unmap_write(z, empty_map_write, 0);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	z = zipc_open(map_zip_filename, "r", &r);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	assert(checked_filesize(z, "map.txt") == strlen(map_content));
	char map_readback[128];
	memset(map_readback, 0, sizeof(map_readback));
	r = zipc_read(z, "map.txt", strlen(map_content), map_readback);
	assert(r == ZIPC_SUCCESS);
	assert(strncmp(map_content, map_readback, strlen(map_content)) == 0);
	assert(checked_filesize(z, "empty-map.txt") == 0);
	r = zipc_read(z, "empty-map.txt", 0, nullptr);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	// Test existing zip files
	z = zipc_open(TEXT_FILES_ZIP, "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_filesize(z, "first.txt", &size) == ZIPC_SUCCESS);
	assert(zipc_filesize(z, "second.txt", &size) == ZIPC_SUCCESS);
	assert(zipc_filesize(z, "third.txt", &size) == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	// This one was created slightly different and has a comment
	z = zipc_open(TEXT_FILES2_ZIP, "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);
	assert(zipc_filesize(z, "first.txt", &size) == ZIPC_SUCCESS);
	assert(zipc_filesize(z, "second.txt", &size) == ZIPC_SUCCESS);
	assert(zipc_filesize(z, "third.txt", &size) == ZIPC_SUCCESS);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	assert(r == ZIPC_SUCCESS);
	assert(z);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	create_sparse_zip64_offset_archive("sparse-offset.zip");
	z = zipc_open("sparse-offset.zip", "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);
	assert(checked_filesize(z, "huge-offset.txt") == strlen("zip64 offset sentinel"));
	char sparse_readback[64];
	memset(sparse_readback, 0, sizeof(sparse_readback));
	r = zipc_read(z, "huge-offset.txt", strlen("zip64 offset sentinel"), sparse_readback);
	assert(r == ZIPC_SUCCESS);
	assert(strcmp(sparse_readback, "zip64 offset sentinel") == 0);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	create_zip32_max_entries_archive("zip32-max-entries.zip");
	z = zipc_open("zip32-max-entries.zip", "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);
	assert(checked_filesize(z, "e00000") == 0);
	assert(checked_filesize(z, "e65534") == 0);
	assert(zipc_validate(z) == ZIPC_SUCCESS);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	create_saturated_eocd_without_locator("bad-zip64.zip");
	z = zipc_open("bad-zip64.zip", "r", &r);
	assert(z == nullptr);
	assert(r == ZIPC_CORRUPT_ARCHIVE);

	// Compressed files are expected to fail
	z = zipc_open(COMPRESSED_ZIP, "r", &r);
	assert(z == nullptr);
	assert(r == ZIPC_UNSUPPORTED_FEATURE);

	// Encrypted files are expected to fail
	z = zipc_open(ENCRYPTED_ZIP, "r", &r);
	assert(z == nullptr);
	assert(r == ZIPC_UNSUPPORTED_FEATURE);

	test_stream_api_edge_cases();
}
