#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif

#include "zipc.h"
#include "zipc_utility.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

#include <cstdint>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>
#include <dlfcn.h>

static ssize_t safe_copy_file_range(int fd_in, off_t* off_in, int fd_out, off_t* off_out, size_t len, unsigned int flags)
{
	typedef ssize_t (*cfr_func_t)(int, off_t*, int, off_t*, size_t, unsigned int);
	static cfr_func_t pfn_cfr = (cfr_func_t)dlsym(RTLD_DEFAULT, "copy_file_range");

	if (pfn_cfr)
	{
		ssize_t ret = pfn_cfr(fd_in, off_in, fd_out, off_out, len, flags);
		if (ret >= 0 || (errno != EXDEV && errno != ENOSYS && errno != EOPNOTSUPP && errno != EINVAL))
		{
			return ret;
		}
	}

	constexpr size_t BUF_SIZE = 64 * 1024;
	std::vector<char> buffer(BUF_SIZE);
	size_t remaining = len;

	off_t orig_in = 0, orig_out = 0;
	if (off_in && (orig_in = lseek(fd_in, *off_in, SEEK_SET)) < 0) return -1;
	if (off_out && (orig_out = lseek(fd_out, *off_out, SEEK_SET)) < 0) return -1;

	while (remaining > 0)
	{
		size_t chunk = (remaining < BUF_SIZE) ? remaining : BUF_SIZE;
		ssize_t bytes_read = read(fd_in, buffer.data(), chunk);
		if (bytes_read < 0) return -1;
		if (bytes_read == 0) break;

		size_t written = 0;
		while (written < (size_t)bytes_read)
		{
			ssize_t bytes_written = write(fd_out, buffer.data() + written, bytes_read - written);
			if (bytes_written <= 0) return -1;
			written += bytes_written;
		}
		remaining -= bytes_read;
	}

	if (off_in) { *off_in = lseek(fd_in, 0, SEEK_CUR); }
	if (off_out) { *off_out = lseek(fd_out, 0, SEEK_CUR); }

	return len - remaining;
}

// Private definitions

enum zipc_mode
{
	ZIPC_READ_ONLY,
	ZIPC_WRITE_ONLY,
	ZIPC_APPEND,
};

struct filenode
{
	uint64_t size;
	uint64_t data_offset;
	uint64_t local_offset;
	uint32_t crc;
};

struct zipc
{
	std::string filename;
	zipc_mode mode;
	FILE* fp = nullptr;
	std::unordered_map<std::string, filenode> files;
	uint64_t central_dir_offset = 0;
	bool central_dir_known = false;
	bool map_write_active = false;
	void* map_write_base = nullptr;
	void* map_write_data = nullptr;
	size_t map_write_length = 0;
	uint64_t map_write_max = 0;
	uint64_t map_write_data_offset = 0;
	uint64_t map_write_local_offset = 0;
	std::string map_write_path;
};

struct zipcstream
{
	zipc* parent = nullptr;
	FILE* stream = nullptr;
	std::string path;
	std::string temp_path;
	uint64_t size = 0;
};

// Utility functions

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

static bool to_off_t(uint64_t value, off_t* out)
{
	static_assert(sizeof(off_t) >= 8, "zipcontainer requires 64-bit off_t");
	if (value > (uint64_t)INT64_MAX) return false;
	*out = (off_t)value;
	return true;
}

static bool seek64(FILE* fp, uint64_t offset, int whence)
{
	off_t off = 0;
	if (whence == SEEK_SET && !to_off_t(offset, &off)) return false;
	if (whence != SEEK_SET) off = (off_t)offset;
	return fseeko(fp, off, whence) == 0;
}

static bool tell64(FILE* fp, uint64_t* out)
{
	const off_t pos = ftello(fp);
	if (pos < 0) return false;
	*out = (uint64_t)pos;
	return true;
}

static bool truncate64(FILE* fp, uint64_t size)
{
	const int fd = fileno(fp);
	off_t off = 0;
	return fd >= 0 && to_off_t(size, &off) && ftruncate(fd, off) == 0;
}

static const uint32_t* crc32_table()
{
	static uint32_t table[256];
	static bool table_init = false;
	if (!table_init)
	{
		for (uint32_t i = 0; i < 256; ++i)
		{
			uint32_t c = i;
			for (int j = 0; j < 8; ++j)
			{
				if (c & 1U) c = 0xEDB88320U ^ (c >> 1U);
				else c >>= 1U;
			}
			table[i] = c;
		}
		table_init = true;
	}
	return table;
}

static uint32_t crc32_update(uint32_t crc, const void* data, size_t size)
{
	const uint32_t* table = crc32_table();
	const unsigned char* p = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i)
	{
		crc = table[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8U);
	}
	return crc;
}

static uint32_t crc32_calc(const void* data, size_t size)
{
	uint32_t crc = 0xFFFFFFFFU;
	crc = crc32_update(crc, data, size);
	return crc ^ 0xFFFFFFFFU;
}

// Write all bytes even if fwrite returns short. Fail only on persistent error.
static bool write_all(FILE* fp, const void* buf, size_t size)
{
	const unsigned char* p = static_cast<const unsigned char*>(buf);
	size_t remaining = size;
	while (remaining > 0)
	{
		size_t w = fwrite(p, 1, remaining, fp);
		if (w == 0)
		{
			if (ferror(fp)) return false;
			continue;
		}
		p += w;
		remaining -= w;
	}
	return true;
}

static bool write_all64(FILE* fp, const void* buf, uint64_t size)
{
	if (size > (uint64_t)SIZE_MAX) return false;

	const unsigned char* p = static_cast<const unsigned char*>(buf);
	uint64_t remaining = size;
	while (remaining > 0)
	{
		size_t chunk = (size_t)remaining;
		if (chunk > (1U << 30)) chunk = (1U << 30);
		if (!write_all(fp, p, chunk)) return false;
		p += chunk;
		remaining -= chunk;
	}
	return true;
}

// Read exactly size bytes; fail on EOF or error.
static bool read_fully(FILE* fp, void* buf, size_t size)
{
	unsigned char* p = static_cast<unsigned char*>(buf);
	size_t remaining = size;
	while (remaining > 0)
	{
		size_t r = fread(p, 1, remaining, fp);
		if (r == 0)
		{
			if (ferror(fp) || feof(fp)) return false;
			continue;
		}
		p += r;
		remaining -= r;
	}
	return true;
}

static bool read_fully64(FILE* fp, void* buf, uint64_t size)
{
	if (size > (uint64_t)SIZE_MAX) return false;

	unsigned char* p = static_cast<unsigned char*>(buf);
	uint64_t remaining = size;
	while (remaining > 0)
	{
		size_t chunk = (size_t)remaining;
		if (chunk > (1U << 30)) chunk = (1U << 30);
		if (!read_fully(fp, p, chunk)) return false;
		p += chunk;
		remaining -= chunk;
	}
	return true;
}

struct zip64_values
{
	uint64_t uncompressed_size = 0;
	uint64_t compressed_size = 0;
	uint64_t local_offset = 0;
};

static bool add_u64(uint64_t a, uint64_t b, uint64_t* out)
{
	if (a > UINT64_MAX - b) return false;
	*out = a + b;
	return true;
}

static bool parse_zip64_extra(const unsigned char* extra, size_t extra_len,
	bool need_uncompressed, bool need_compressed, bool need_offset,
	zip64_values* values)
{
	assert(values);
	size_t pos = 0;
	while (pos + 4 <= extra_len)
	{
		const uint16_t id = read_le16(extra + pos);
		const uint16_t data_size = read_le16(extra + pos + 2);
		pos += 4;
		if (data_size > extra_len - pos) return false;
		if (id == 0x0001)
		{
			size_t zpos = pos;
			if (need_uncompressed)
			{
				if (data_size < zpos - pos + 8) return false;
				values->uncompressed_size = read_le64(extra + zpos);
				zpos += 8;
			}
			if (need_compressed)
			{
				if (data_size < zpos - pos + 8) return false;
				values->compressed_size = read_le64(extra + zpos);
				zpos += 8;
			}
			if (need_offset)
			{
				if (data_size < zpos - pos + 8) return false;
				values->local_offset = read_le64(extra + zpos);
			}
			return true;
		}
		pos += data_size;
	}
	return !need_uncompressed && !need_compressed && !need_offset;
}

static void write_zip64_local_extra(unsigned char* extra, uint64_t size)
{
	write_le16(extra, 0x0001);
	write_le16(extra + 2, 16);
	write_le64(extra + 4, size);
	write_le64(extra + 12, size);
}

static void write_zip64_central_extra(unsigned char* extra, uint64_t size, uint64_t local_offset)
{
	write_le16(extra, 0x0001);
	write_le16(extra + 2, 24);
	write_le64(extra + 4, size);
	write_le64(extra + 12, size);
	write_le64(extra + 20, local_offset);
}

static bool write_local_header_zip64(FILE* fp, const std::string& name, uint32_t crc, uint64_t size)
{
	if (name.size() > 0xFFFFU) return false;

	unsigned char local[30] = {0};
	write_le32(local, 0x04034b50);
	write_le16(local + 4, 45);
	write_le16(local + 8, 0);
	write_le32(local + 14, crc);
	write_le32(local + 18, 0xFFFFFFFFU);
	write_le32(local + 22, 0xFFFFFFFFU);
	write_le16(local + 26, (uint16_t)name.size());
	write_le16(local + 28, 20);

	unsigned char extra[20] = {0};
	write_zip64_local_extra(extra, size);

	return write_all(fp, local, sizeof(local)) &&
		write_all(fp, name.data(), name.size()) &&
		write_all(fp, extra, sizeof(extra));
}

static bool patch_local_header_zip64(FILE* fp, uint64_t local_offset, size_t name_len,
	uint32_t crc, uint64_t size)
{
	uint64_t crc_offset = 0;
	if (!add_u64(local_offset, 14, &crc_offset)) return false;
	if (!seek64(fp, crc_offset, SEEK_SET)) return false;
	unsigned char crc_buf[4];
	write_le32(crc_buf, crc);
	if (!write_all(fp, crc_buf, sizeof(crc_buf))) return false;

	uint64_t size_offset = 0;
	if (!add_u64(local_offset, 30U + (uint64_t)name_len + 4U, &size_offset)) return false;
	if (!seek64(fp, size_offset, SEEK_SET)) return false;
	unsigned char size_buf[16];
	write_le64(size_buf, size);
	write_le64(size_buf + 8, size);
	return write_all(fp, size_buf, sizeof(size_buf));
}

static bool write_zip64_end_records(FILE* fp, uint64_t entry_count, uint64_t cd_size, uint64_t cd_offset)
{
	uint64_t zip64_eocd_offset = 0;
	if (!tell64(fp, &zip64_eocd_offset)) return false;

	unsigned char zip64_eocd[56] = {0};
	write_le32(zip64_eocd, 0x06064b50);
	write_le64(zip64_eocd + 4, 44);
	write_le16(zip64_eocd + 12, 45);
	write_le16(zip64_eocd + 14, 45);
	write_le64(zip64_eocd + 24, entry_count);
	write_le64(zip64_eocd + 32, entry_count);
	write_le64(zip64_eocd + 40, cd_size);
	write_le64(zip64_eocd + 48, cd_offset);
	if (!write_all(fp, zip64_eocd, sizeof(zip64_eocd))) return false;

	unsigned char locator[20] = {0};
	write_le32(locator, 0x07064b50);
	write_le64(locator + 8, zip64_eocd_offset);
	write_le32(locator + 16, 1);
	if (!write_all(fp, locator, sizeof(locator))) return false;

	unsigned char eocd[22] = {0};
	write_le32(eocd, 0x06054b50);
	write_le16(eocd + 8, 0xFFFFU);
	write_le16(eocd + 10, 0xFFFFU);
	write_le32(eocd + 12, 0xFFFFFFFFU);
	write_le32(eocd + 16, 0xFFFFFFFFU);
	return write_all(fp, eocd, sizeof(eocd));
}

static bool write_empty_archive(FILE* fp)
{
	if (!seek64(fp, 0, SEEK_SET)) return false;
	if (!truncate64(fp, 0)) return false;
	if (!write_zip64_end_records(fp, 0, 0, 0)) return false;
	return fflush(fp) == 0;
}

static bool compute_data_offset(FILE* fp, uint64_t local_offset, uint64_t* data_offset)
{
	if (!seek64(fp, local_offset, SEEK_SET)) return false;
	unsigned char local[30];
	if (!read_fully(fp, local, sizeof(local))) return false;
	if (read_le32(local) != 0x04034b50) return false;
	const uint16_t name_len = read_le16(local + 26);
	const uint16_t extra_len = read_le16(local + 28);
	uint64_t offset = 0;
	if (!add_u64(local_offset, 30U + (uint64_t)name_len + (uint64_t)extra_len, &offset)) return false;
	*data_offset = offset;
	return true;
}

static enum zipc_status write_central_directory(zipc* handle)
{
	assert(handle);
	if (!handle->fp) return ZIPC_IO_FAILURE;

	std::vector<std::string> names;
	names.reserve(handle->files.size());
	for (const auto& kv : handle->files) names.push_back(kv.first);
	std::sort(names.begin(), names.end());

	uint64_t cd_start = 0;
	if (!tell64(handle->fp, &cd_start)) return ZIPC_IO_FAILURE;

	for (const auto& name : names)
	{
		const filenode& n = handle->files.at(name);
		if (name.size() > 0xFFFF) return ZIPC_UNSUPPORTED_FEATURE;

		unsigned char cd[46] = {0};
		write_le32(cd, 0x02014b50);
		write_le16(cd + 4, 45);
		write_le16(cd + 6, 45);
		// flags: 0
		// compression: 0
		// mod time/date: 0
		write_le32(cd + 16, n.crc);
		write_le32(cd + 20, 0xFFFFFFFFU);
		write_le32(cd + 24, 0xFFFFFFFFU);

		const uint16_t nlen = (uint16_t)name.size();
		write_le16(cd + 28, nlen);
		write_le16(cd + 30, 28);
		write_le32(cd + 42, 0xFFFFFFFFU);

		unsigned char extra[28] = {0};
		write_zip64_central_extra(extra, n.size, n.local_offset);

		if (!write_all(handle->fp, cd, sizeof(cd))) return ZIPC_IO_FAILURE;
		if (!write_all(handle->fp, name.data(), nlen)) return ZIPC_IO_FAILURE;
		if (!write_all(handle->fp, extra, sizeof(extra))) return ZIPC_IO_FAILURE;
	}

	uint64_t cd_end = 0;
	if (!tell64(handle->fp, &cd_end)) return ZIPC_IO_FAILURE;
	if (cd_end < cd_start) return ZIPC_IO_FAILURE;
	const uint64_t cd_size = cd_end - cd_start;
	const uint64_t entry_count = (uint64_t)names.size();

	if (!write_zip64_end_records(handle->fp, entry_count, cd_size, cd_start)) return ZIPC_IO_FAILURE;
	if (fflush(handle->fp) != 0) return ZIPC_IO_FAILURE;

	handle->central_dir_offset = cd_start;
	handle->central_dir_known = true;
	return ZIPC_SUCCESS;
}

static enum zipc_status load_existing_archive(zipc* z)
{
	FILE* fp = z->fp;
	if (!seek64(fp, 0, SEEK_END)) return ZIPC_IO_FAILURE;
	uint64_t file_size = 0;
	if (!tell64(fp, &file_size)) return ZIPC_IO_FAILURE;
	if (file_size == 0) return ZIPC_SUCCESS;
	if (file_size < 22) return ZIPC_CORRUPT_ARCHIVE;

	const size_t max_search = 0xFFFF + 22;
	size_t search = file_size > (uint64_t)max_search ? max_search : (size_t)file_size;

	std::vector<unsigned char> tail(search);
	if (!seek64(fp, file_size - search, SEEK_SET)) return ZIPC_IO_FAILURE;
	if (!read_fully(fp, tail.data(), search)) return ZIPC_IO_FAILURE;

	ssize_t eocd_idx = -1;
	for (size_t i = search - 22;; --i)
	{
		if (read_le32(&tail[i]) == 0x06054b50)
		{
			const uint16_t comment_len = read_le16(&tail[i] + 20);
			uint64_t eocd_end = 0;
			if (add_u64(file_size - search + (uint64_t)i, 22U + (uint64_t)comment_len, &eocd_end) &&
				eocd_end == file_size)
			{
				eocd_idx = static_cast<ssize_t>(i);
				break;
			}
		}
		if (i == 0) break;
	}
	if (eocd_idx < 0) return ZIPC_CORRUPT_ARCHIVE;

	const uint64_t eocd_pos = file_size - search + (uint64_t)eocd_idx;
	const unsigned char* eocd = tail.data() + eocd_idx;
	const uint16_t disk_number = read_le16(eocd + 4);
	const uint16_t cd_disk = read_le16(eocd + 6);
	const uint16_t entry_count16 = read_le16(eocd + 10);
	const uint32_t cd_size32 = read_le32(eocd + 12);
	const uint32_t cd_offset32 = read_le32(eocd + 16);
	if (disk_number != 0 || cd_disk != 0) return ZIPC_UNSUPPORTED_FEATURE;

	uint64_t entry_count = entry_count16;
	uint64_t cd_size = cd_size32;
	uint64_t cd_offset = cd_offset32;
	uint64_t cd_limit = eocd_pos;

	if (eocd_pos >= 20)
	{
		unsigned char locator[20];
		if (!seek64(fp, eocd_pos - 20, SEEK_SET)) return ZIPC_IO_FAILURE;
		if (!read_fully(fp, locator, sizeof(locator))) return ZIPC_IO_FAILURE;
		if (read_le32(locator) == 0x07064b50)
		{
			const uint32_t eocd64_disk = read_le32(locator + 4);
			const uint64_t eocd64_offset = read_le64(locator + 8);
			const uint32_t total_disks = read_le32(locator + 16);
			if (eocd64_disk != 0 || total_disks != 1) return ZIPC_UNSUPPORTED_FEATURE;
			if (eocd64_offset > eocd_pos - 20) return ZIPC_CORRUPT_ARCHIVE;

			unsigned char eocd64[56];
			if (!seek64(fp, eocd64_offset, SEEK_SET)) return ZIPC_IO_FAILURE;
			if (!read_fully(fp, eocd64, sizeof(eocd64))) return ZIPC_CORRUPT_ARCHIVE;
			if (read_le32(eocd64) != 0x06064b50) return ZIPC_CORRUPT_ARCHIVE;
			const uint64_t eocd64_size = read_le64(eocd64 + 4);
			if (eocd64_size < 44) return ZIPC_CORRUPT_ARCHIVE;
			uint64_t eocd64_end = 0;
			if (!add_u64(eocd64_offset, 12 + eocd64_size, &eocd64_end) ||
				eocd64_end > eocd_pos - 20) return ZIPC_CORRUPT_ARCHIVE;
			if (read_le32(eocd64 + 16) != 0 || read_le32(eocd64 + 20) != 0) return ZIPC_UNSUPPORTED_FEATURE;
			entry_count = read_le64(eocd64 + 32);
			if (read_le64(eocd64 + 24) != entry_count) return ZIPC_UNSUPPORTED_FEATURE;
			cd_size = read_le64(eocd64 + 40);
			cd_offset = read_le64(eocd64 + 48);
			cd_limit = eocd64_offset;
		}
	}

	uint64_t cd_end = 0;
	if (!add_u64(cd_offset, cd_size, &cd_end) || cd_end > cd_limit) return ZIPC_CORRUPT_ARCHIVE;

	if (!seek64(fp, cd_offset, SEEK_SET)) return ZIPC_IO_FAILURE;
	std::vector<unsigned char> header(46);
	for (uint64_t i = 0; i < entry_count; ++i)
	{
		if (!read_fully(fp, header.data(), header.size())) return ZIPC_CORRUPT_ARCHIVE;
		if (read_le32(header.data()) != 0x02014b50) return ZIPC_CORRUPT_ARCHIVE;

		const uint16_t flags = read_le16(header.data() + 8);
		const uint16_t method = read_le16(header.data() + 10);
		const uint32_t crc = read_le32(header.data() + 16);
		const uint32_t compressed_size = read_le32(header.data() + 20);
		const uint32_t uncompressed_size = read_le32(header.data() + 24);
		const uint16_t name_len = read_le16(header.data() + 28);
		const uint16_t extra_len = read_le16(header.data() + 30);
		const uint16_t comment_len = read_le16(header.data() + 32);
		const uint16_t disk_start = read_le16(header.data() + 34);
		const uint32_t local_offset = read_le32(header.data() + 42);

		if (method != 0) return ZIPC_UNSUPPORTED_FEATURE;
		if (flags & 0x08) return ZIPC_UNSUPPORTED_FEATURE;
		if (disk_start != 0) return ZIPC_UNSUPPORTED_FEATURE;

		std::string name(name_len, '\0');
		if (!read_fully(fp, name.data(), name_len)) return ZIPC_CORRUPT_ARCHIVE;
		std::vector<unsigned char> extra(extra_len);
		if (extra_len > 0 && !read_fully(fp, extra.data(), extra_len)) return ZIPC_CORRUPT_ARCHIVE;
		if (comment_len > 0 && !seek64(fp, comment_len, SEEK_CUR)) return ZIPC_CORRUPT_ARCHIVE;

		const bool need_uncompressed = uncompressed_size == 0xFFFFFFFFU;
		const bool need_compressed = compressed_size == 0xFFFFFFFFU;
		const bool need_offset = local_offset == 0xFFFFFFFFU;
		zip64_values values;
		if ((need_uncompressed || need_compressed || need_offset) &&
			!parse_zip64_extra(extra.data(), extra.size(), need_uncompressed, need_compressed, need_offset, &values))
		{
			return ZIPC_CORRUPT_ARCHIVE;
		}

		const uint64_t compressed_size64 = need_compressed ? values.compressed_size : compressed_size;
		const uint64_t uncompressed_size64 = need_uncompressed ? values.uncompressed_size : uncompressed_size;
		const uint64_t local_offset64 = need_offset ? values.local_offset : local_offset;
		if (compressed_size64 != uncompressed_size64) return ZIPC_UNSUPPORTED_FEATURE;

		uint64_t data_offset = 0;
		uint64_t return_pos = 0;
		if (!tell64(fp, &return_pos)) return ZIPC_IO_FAILURE;
		if (!compute_data_offset(fp, local_offset64, &data_offset)) return ZIPC_CORRUPT_ARCHIVE;
		if (!seek64(fp, return_pos, SEEK_SET)) return ZIPC_IO_FAILURE;

		if (z->files.count(name) == 0)
		{
			z->files.emplace(std::move(name), filenode{uncompressed_size64, data_offset, local_offset64, crc});
		}
	}
	z->central_dir_offset = cd_offset;
	z->central_dir_known = true;
	return ZIPC_SUCCESS;
}

// Implementations

const char* zipc_strerror(zipc_status err)
{
	switch (err)
	{
		case ZIPC_SUCCESS: return "Success";
		case ZIPC_SYNTAX_ERROR: return "Syntax error";
		case ZIPC_PERMISSION_FAILURE: return "Permission failure";
		case ZIPC_PATH_ALREADY_EXISTS: return "Path already exists";
		case ZIPC_IO_FAILURE: return "I/O failure";
		case ZIPC_CORRUPT_ARCHIVE: return "Corrupt archive";
		case ZIPC_UNSUPPORTED_FEATURE: return "Unsupported feature";
		case ZIPC_PATH_NOT_FOUND: return "Path not found";
		default: return "Unknown error";
	}
}

zipc* zipc_open(const char* filename, const char* mode, enum zipc_status* err)
{
	if (err) *err = ZIPC_SUCCESS;
	if (!filename || !mode || strlen(mode) != 1)
	{
		if (err) *err = ZIPC_SYNTAX_ERROR;
		return nullptr;
	}

	zipc* z = new zipc();
	z->filename = filename;
	if (mode[0] == 'r') z->mode = ZIPC_READ_ONLY;
	else if (mode[0] == 'w') z->mode = ZIPC_WRITE_ONLY;
	else if (mode[0] == 'a') z->mode = ZIPC_APPEND;
	else
	{
		if (err) *err = ZIPC_SYNTAX_ERROR;
		delete z;
		return nullptr;
	}

	const char* fopen_mode = (z->mode == ZIPC_READ_ONLY) ? "rb" : (z->mode == ZIPC_WRITE_ONLY ? "wb+" : "rb+");
	z->fp = fopen(filename, fopen_mode);
	if (!z->fp && z->mode == ZIPC_APPEND)
	{
		z->fp = fopen(filename, "wb+");
		if (z->fp && !write_empty_archive(z->fp))
		{
			fclose(z->fp);
			z->fp = nullptr;
		}
	}
	if (!z->fp)
	{
		if (err) *err = ZIPC_PERMISSION_FAILURE;
		delete z;
		return nullptr;
	}

	if (z->mode == ZIPC_WRITE_ONLY)
	{
		if (!write_empty_archive(z->fp))
		{
			if (err) *err = ZIPC_IO_FAILURE;
			fclose(z->fp);
			delete z;
			return nullptr;
		}
		z->central_dir_offset = 0;
		z->central_dir_known = true;
		return z;
	}

	const enum zipc_status status = load_existing_archive(z);
	if (status != ZIPC_SUCCESS)
	{
		if (err) *err = status;
		fclose(z->fp);
		delete z;
		return nullptr;
	}

	if (z->mode == ZIPC_APPEND) seek64(z->fp, 0, SEEK_END);
	return z;
}

zipc* zipc_open_fd(int fd, const char* mode, int close_fd, enum zipc_status* err)
{
	if (err) *err = ZIPC_SUCCESS;
	if (fd < 0 || !mode || strlen(mode) != 1)
	{
		if (err) *err = ZIPC_SYNTAX_ERROR;
		return nullptr;
	}

	int target_fd = fd;
	if (!close_fd)
	{
		target_fd = dup(fd);
		if (target_fd < 0)
		{
			if (err) *err = ZIPC_IO_FAILURE;
			return nullptr;
		}
	}

	zipc* z = new zipc();
	z->filename = "fd:" + std::to_string(fd);
	if (mode[0] == 'r') z->mode = ZIPC_READ_ONLY;
	else if (mode[0] == 'w') z->mode = ZIPC_WRITE_ONLY;
	else if (mode[0] == 'a') z->mode = ZIPC_APPEND;
	else
	{
		if (err) *err = ZIPC_SYNTAX_ERROR;
		if (!close_fd) close(target_fd);
		delete z;
		return nullptr;
	}

	const char* fopen_mode = (z->mode == ZIPC_READ_ONLY) ? "rb" : (z->mode == ZIPC_WRITE_ONLY ? "wb+" : "rb+");
	z->fp = fdopen(target_fd, fopen_mode);
	if (!z->fp)
	{
		if (err) *err = ZIPC_PERMISSION_FAILURE;
		if (!close_fd) close(target_fd);
		delete z;
		return nullptr;
	}

	if (z->mode == ZIPC_WRITE_ONLY)
	{
		if (!write_empty_archive(z->fp))
		{
			if (err) *err = ZIPC_IO_FAILURE;
			fclose(z->fp);
			delete z;
			return nullptr;
		}
		z->central_dir_offset = 0;
		z->central_dir_known = true;
		return z;
	}

	const enum zipc_status status = load_existing_archive(z);
	if (status != ZIPC_SUCCESS)
	{
		if (err) *err = status;
		fclose(z->fp);
		delete z;
		return nullptr;
	}

	if (z->mode == ZIPC_APPEND) seek64(z->fp, 0, SEEK_END);
	return z;
}

enum zipc_status zipc_close(zipc* handle)
{
	enum zipc_status r = ZIPC_SUCCESS;
	if (!handle) return r;
	if (handle->map_write_active) r = ZIPC_SYNTAX_ERROR;
	if (handle->fp && fclose(handle->fp) != 0) r = ZIPC_IO_FAILURE;
	delete handle;
	return r;
}

enum zipc_status zipc_filesize(zipc* handle, const char* path, uint64_t* size_out)
{
	assert(handle);
	assert(path);
	if (!handle || !path || !size_out) return ZIPC_SYNTAX_ERROR;
	if (handle->files.count(path) == 0) return ZIPC_PATH_NOT_FOUND;
	const auto& v = handle->files.at(path);
	*size_out = v.size;
	return ZIPC_SUCCESS;
}

zipc_mapping zipc_map_read(zipc* handle, const char* path, enum zipc_status* err)
{
	assert(handle);
	assert(path);
	zipc_mapping m{};
	if (!handle->fp)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return m;
	}
	if (handle->mode == ZIPC_WRITE_ONLY)
	{
		if (err) *err = ZIPC_PERMISSION_FAILURE;
		return m;
	}
	auto it = handle->files.find(path);
	if (it == handle->files.end())
	{
		if (err) *err = ZIPC_PATH_NOT_FOUND;
		return m;
	}

	const filenode& node = it->second;
	if (node.size == 0)
	{
		static char empty = 0;
		m.data = &empty;
		m.size = 0;
		m.map_base = nullptr;
		m.map_length = 0;
		if (err) *err = ZIPC_SUCCESS;
		return m;
	}

	const long pagesize_long = sysconf(_SC_PAGESIZE);
	const size_t pagesize = pagesize_long > 0 ? (size_t)pagesize_long : 4096;
	const size_t page_offset = (size_t)(node.data_offset % pagesize);
	const uint64_t map_offset64 = node.data_offset - page_offset;
	if (node.size > (uint64_t)SIZE_MAX - page_offset)
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return m;
	}
	const size_t map_length = (size_t)node.size + page_offset;

	const int fd = fileno(handle->fp);
	if (fd < 0)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return m;
	}

	off_t map_offset = 0;
	if (!to_off_t(map_offset64, &map_offset))
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return m;
	}
	void* base = mmap(nullptr, map_length, PROT_READ, MAP_PRIVATE, fd, map_offset);
	if (base == MAP_FAILED)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return m;
	}

	void* user_ptr = static_cast<unsigned char*>(base) + page_offset;
	m.data = user_ptr;
	m.size = node.size;
	m.map_base = base;
	m.map_length = map_length;

	if (err) *err = ZIPC_SUCCESS;
	return m;
}

zipc_mapping zipc_map_write(zipc* handle, const char* path, enum zipc_status* err, uint64_t max)
{
	assert(handle);
	assert(path);
	if (err) *err = ZIPC_SUCCESS;
	zipc_mapping mapping{};
	if (!handle || !path)
	{
		if (err) *err = ZIPC_SYNTAX_ERROR;
		return mapping;
	}
	if (handle->mode == ZIPC_READ_ONLY)
	{
		if (err) *err = ZIPC_PERMISSION_FAILURE;
		return mapping;
	}
	if (!handle->fp)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}
	if (handle->map_write_active)
	{
		if (err) *err = ZIPC_PERMISSION_FAILURE;
		return mapping;
	}
	if (handle->files.count(path) != 0)
	{
		if (err) *err = ZIPC_PATH_ALREADY_EXISTS;
		return mapping;
	}

	const size_t name_len = strlen(path);
	if (name_len > 0xFFFF)
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return mapping;
	}
	if (max > (uint64_t)SIZE_MAX)
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return mapping;
	}
	if (!seek64(handle->fp, 0, SEEK_END))
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	uint64_t local_offset = 0;
	if (!tell64(handle->fp, &local_offset))
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	if (!write_local_header_zip64(handle->fp, path, 0, max))
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	uint64_t data_offset = 0;
	uint64_t end_offset = 0;
	if (!add_u64(local_offset, 30U + name_len + 20U, &data_offset) ||
		!add_u64(data_offset, max, &end_offset))
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return mapping;
	}
	const int fd = fileno(handle->fp);
	off_t end_off = 0;
	if (fd < 0 || !to_off_t(end_offset, &end_off) || ftruncate(fd, end_off) != 0)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	if (max == 0)
	{
		static char empty = 0;
		handle->map_write_active = true;
		handle->map_write_base = nullptr;
		handle->map_write_data = &empty;
		handle->map_write_length = 0;
		handle->map_write_max = 0;
		handle->map_write_data_offset = data_offset;
		handle->map_write_local_offset = local_offset;
		handle->map_write_path = path;

		if (err) *err = ZIPC_SUCCESS;
		mapping.data = &empty;
		mapping.size = 0;
		mapping.map_base = nullptr;
		mapping.map_length = 0;
		return mapping;
	}

	const long pagesize_long = sysconf(_SC_PAGESIZE);
	const size_t pagesize = pagesize_long > 0 ? (size_t)pagesize_long : 4096;
	const size_t page_offset = (size_t)(data_offset % pagesize);
	const uint64_t map_offset64 = data_offset - page_offset;
	if (max > (uint64_t)SIZE_MAX - page_offset)
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return mapping;
	}
	const size_t map_length = (size_t)max + page_offset;

	off_t map_offset = 0;
	if (!to_off_t(map_offset64, &map_offset))
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return mapping;
	}
	void* base = mmap(nullptr, map_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map_offset);
	if (base == MAP_FAILED)
	{
		off_t local_off = 0;
		if (to_off_t(local_offset, &local_off) && ftruncate(fd, local_off) != 0)
		{
			// Preserve the original mmap failure status; this is only best-effort rollback.
		}
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	void* user_ptr = static_cast<unsigned char*>(base) + page_offset;
	handle->map_write_active = true;
	handle->map_write_base = base;
	handle->map_write_data = user_ptr;
	handle->map_write_length = map_length;
	handle->map_write_max = max;
	handle->map_write_data_offset = data_offset;
	handle->map_write_local_offset = local_offset;
	handle->map_write_path = path;

	if (err) *err = ZIPC_SUCCESS;
	mapping.data = user_ptr;
	mapping.size = max;
	mapping.map_base = base;
	mapping.map_length = map_length;
	return mapping;
}

void zipc_unmap_read(zipc* handle, zipc_mapping mapping)
{
	assert(handle);
	if (!mapping.map_base || mapping.map_length == 0) return;
	munmap(const_cast<void*>(mapping.map_base), mapping.map_length);
}

enum zipc_status zipc_unmap_write(zipc* handle, zipc_mapping mapping, uint64_t size)
{
	assert(handle);
	if (!handle || !mapping.data) return ZIPC_SYNTAX_ERROR;
	if (!handle->map_write_active) return ZIPC_SYNTAX_ERROR;
	if (mapping.data != handle->map_write_data) return ZIPC_SYNTAX_ERROR;
	if (size > handle->map_write_max) return ZIPC_SYNTAX_ERROR;
	if (size > (uint64_t)SIZE_MAX) return ZIPC_UNSUPPORTED_FEATURE;
	if (!handle->fp) return ZIPC_IO_FAILURE;

	enum zipc_status status = ZIPC_SUCCESS;
	const uint32_t crc = crc32_calc(mapping.data, (size_t)size);
	if (handle->map_write_length > 0 && munmap(handle->map_write_base, handle->map_write_length) != 0)
	{
		status = ZIPC_IO_FAILURE;
	}

	uint64_t end_offset = 0;
	if (!add_u64(handle->map_write_data_offset, size, &end_offset)) status = ZIPC_UNSUPPORTED_FEATURE;
	if (status == ZIPC_SUCCESS && size < handle->map_write_max)
	{
		if (!truncate64(handle->fp, end_offset)) status = ZIPC_IO_FAILURE;
	}

	if (status == ZIPC_SUCCESS && !patch_local_header_zip64(handle->fp, handle->map_write_local_offset,
		handle->map_write_path.size(), crc, size)) status = ZIPC_IO_FAILURE;

	if (status == ZIPC_SUCCESS &&
		!seek64(handle->fp, end_offset, SEEK_SET)) status = ZIPC_IO_FAILURE;
	if (status == ZIPC_SUCCESS)
	{
		filenode node;
		node.size = size;
		node.data_offset = handle->map_write_data_offset;
		node.local_offset = handle->map_write_local_offset;
		node.crc = crc;
		handle->files.emplace(handle->map_write_path, node);

		status = write_central_directory(handle);
		if (status != ZIPC_SUCCESS) handle->files.erase(handle->map_write_path);
	}

	handle->map_write_active = false;
	handle->map_write_base = nullptr;
	handle->map_write_data = nullptr;
	handle->map_write_length = 0;
	handle->map_write_max = 0;
	handle->map_write_data_offset = 0;
	handle->map_write_local_offset = 0;
	handle->map_write_path.clear();

	return status;
}

enum zipc_status zipc_write(zipc* handle, const char* path, uint64_t size, const void* ptr)
{
	assert(handle);
	assert(path);
	if (handle->mode == ZIPC_READ_ONLY) return ZIPC_PERMISSION_FAILURE;
	if (!ptr && size > 0) return ZIPC_SYNTAX_ERROR;
	if (handle->files.count(path) != 0) return ZIPC_PATH_ALREADY_EXISTS;
	if (!handle->fp) return ZIPC_IO_FAILURE;

	if (size > (uint64_t)SIZE_MAX) return ZIPC_UNSUPPORTED_FEATURE;
	const size_t name_len = strlen(path);
	if (name_len > 0xFFFF) return ZIPC_UNSUPPORTED_FEATURE;

	if (!seek64(handle->fp, 0, SEEK_END)) return ZIPC_IO_FAILURE;

	uint64_t local_offset = 0;
	if (!tell64(handle->fp, &local_offset)) return ZIPC_IO_FAILURE;

	const uint32_t crc = crc32_calc(ptr, (size_t)size);

	if (!write_local_header_zip64(handle->fp, path, crc, size)) return ZIPC_IO_FAILURE;
	if (size > 0 && !write_all64(handle->fp, ptr, size)) return ZIPC_IO_FAILURE;

	filenode node;
	node.size = size;
	if (!add_u64(local_offset, 30U + name_len + 20U, &node.data_offset)) return ZIPC_UNSUPPORTED_FEATURE;
	node.local_offset = local_offset;
	node.crc = crc;
	handle->files.emplace(path, node);

	const enum zipc_status status = write_central_directory(handle);
	if (status != ZIPC_SUCCESS) handle->files.erase(path);
	return status;
}

enum zipc_status zipc_read(zipc* handle, const char* path, uint64_t size, void* ptr)
{
	assert(handle);
	assert(path);
	if (!handle->fp) return ZIPC_IO_FAILURE;
	if (handle->files.count(path) == 0) return ZIPC_PATH_NOT_FOUND;
	if (handle->mode == ZIPC_WRITE_ONLY) return ZIPC_PERMISSION_FAILURE;
	if (!ptr && size > 0) return ZIPC_SYNTAX_ERROR;

	const filenode& node = handle->files.at(path);
	if (size > node.size) return ZIPC_SYNTAX_ERROR;
	if (size > (uint64_t)SIZE_MAX) return ZIPC_UNSUPPORTED_FEATURE;
	if (!seek64(handle->fp, node.data_offset, SEEK_SET)) return ZIPC_IO_FAILURE;
	if (size > 0 && !read_fully64(handle->fp, ptr, size)) return ZIPC_IO_FAILURE;
	return ZIPC_SUCCESS;
}

zipcstream* zipc_stream_open(zipc* handle, const char* path, const char* mode, enum zipc_status* err)
{
	assert(handle);
	assert(path);
	if (err) *err = ZIPC_SUCCESS;
	if (!handle || !path || !mode)
	{
		if (err) *err = ZIPC_SYNTAX_ERROR;
		return nullptr;
	}
	if (mode[0] != '\0')
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return nullptr;
	}
	if (handle->mode == ZIPC_READ_ONLY)
	{
		if (err) *err = ZIPC_PERMISSION_FAILURE;
		return nullptr;
	}
	if (!handle->fp)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return nullptr;
	}
	if (handle->files.count(path) != 0)
	{
		if (err) *err = ZIPC_PATH_ALREADY_EXISTS;
		return nullptr;
	}

	const std::string& filename = handle->filename;
	const size_t slash = filename.find_last_of('/');
	std::string dir;
	if (slash == std::string::npos)
	{
		dir = ".";
	}
	else if (slash == 0)
	{
		dir = "/";
	}
	else
	{
		dir = filename.substr(0, slash);
	}

	std::string tmpl;
	if (dir == "/") tmpl = "/.zipc_tmp_XXXXXX";
	else tmpl = dir + "/.zipc_tmp_XXXXXX";

	std::vector<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');

	int fd = mkstemp(buf.data());
	if (fd < 0)
	{
		const char* env_tmp = getenv("TMPDIR");
		if (!env_tmp) env_tmp = "/tmp";

		tmpl = std::string(env_tmp) + "/.zipc_tmp_XXXXXX";
		buf = std::vector<char>(tmpl.begin(), tmpl.end());
		buf.push_back('\0');
		fd = mkstemp(buf.data());
		if (fd < 0 && strcmp(env_tmp, "/tmp") != 0)
		{
			tmpl = "/tmp/.zipc_tmp_XXXXXX";
			buf = std::vector<char>(tmpl.begin(), tmpl.end());
			buf.push_back('\0');
			fd = mkstemp(buf.data());
		}
	}
	if (fd < 0)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return nullptr;
	}

	FILE* fp = fdopen(fd, "wb+");
	if (!fp)
	{
		close(fd);
		unlink(buf.data());
		if (err) *err = ZIPC_IO_FAILURE;
		return nullptr;
	}

	zipcstream* c = new zipcstream();
	c->parent = handle;
	c->stream = fp;
	c->path = path;
	c->temp_path = buf.data();
	c->size = 0;
	return c;
}

enum zipc_status zipc_stream_write(const zipc* handle, zipcstream* stream, uint64_t size, const void* ptr)
{
	assert(handle);
	assert(stream);
	if (!handle || !stream) return ZIPC_SYNTAX_ERROR;
	if (stream->parent != handle)
	{
		if (stream->stream) fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_SYNTAX_ERROR;
	}
	if (handle->mode == ZIPC_READ_ONLY) return ZIPC_PERMISSION_FAILURE;
	if (!stream->stream) return ZIPC_IO_FAILURE;
	if (!ptr && size > 0) return ZIPC_SYNTAX_ERROR;
	if (stream->path.empty()) return ZIPC_SYNTAX_ERROR;
	if (size == 0) return ZIPC_SUCCESS;
	if (size > (uint64_t)SIZE_MAX) return ZIPC_UNSUPPORTED_FEATURE;
	if (stream->size > UINT64_MAX - size) return ZIPC_UNSUPPORTED_FEATURE;

	if (!write_all64(stream->stream, ptr, size)) return ZIPC_IO_FAILURE;
	stream->size += size;
	return ZIPC_SUCCESS;
}

enum zipc_status zipc_stream_close(zipc* handle, zipcstream* stream)
{
	assert(handle);
	assert(stream);

	if (!handle || !stream) return ZIPC_SYNTAX_ERROR;
	if (handle->mode == ZIPC_READ_ONLY)
	{
		if (stream->stream) fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_PERMISSION_FAILURE;
	}
	if (!handle->fp || !stream->stream)
	{
		if (stream->stream) fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (stream->path.empty())
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_SYNTAX_ERROR;
	}
	if (handle->files.count(stream->path) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_PATH_ALREADY_EXISTS;
	}

	if (fflush(stream->stream) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (!seek64(stream->stream, 0, SEEK_END))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	uint64_t data_size = 0;
	if (!tell64(stream->stream, &data_size))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (stream->size != data_size)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	const size_t name_len = stream->path.size();
	if (name_len > 0xFFFF)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_UNSUPPORTED_FEATURE;
	}
	if (!seek64(stream->stream, 0, SEEK_SET))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	uint32_t crc = 0xFFFFFFFFU;
	std::vector<unsigned char> buf(64 * 1024);
	uint64_t remaining = data_size;
	while (remaining > 0)
	{
		const size_t chunk = remaining < (uint64_t)buf.size() ? (size_t)remaining : buf.size();
		if (!read_fully(stream->stream, buf.data(), chunk))
		{
			fclose(stream->stream);
			if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
			delete stream;
			return ZIPC_IO_FAILURE;
		}
		crc = crc32_update(crc, buf.data(), chunk);
		remaining -= chunk;
	}
	crc ^= 0xFFFFFFFFU;
	if (!seek64(stream->stream, 0, SEEK_SET))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	if (!seek64(handle->fp, 0, SEEK_END))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	uint64_t local_offset = 0;
	if (!tell64(handle->fp, &local_offset))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	if (!write_local_header_zip64(handle->fp, stream->path, crc, data_size))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (fflush(handle->fp) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	const int fd_in = fileno(stream->stream);
	const int fd_out = fileno(handle->fp);
	if (fd_in < 0 || fd_out < 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (lseek(fd_in, 0, SEEK_SET) < 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	remaining = data_size;
#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
	bool punch_hole_enabled = true;
#endif
	while (remaining > 0)
	{
		size_t chunk = (size_t)remaining;
		if (chunk > (1U << 30)) chunk = (1U << 30);
		const off_t punch_offset = lseek(fd_in, 0, SEEK_CUR);
		if (punch_offset < 0)
		{
			fclose(stream->stream);
			if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
			delete stream;
			return ZIPC_IO_FAILURE;
		}
		const ssize_t moved = safe_copy_file_range(fd_in, nullptr, fd_out, nullptr, chunk, 0);
		if (moved <= 0)
		{
			fclose(stream->stream);
			if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
			delete stream;
			return ZIPC_IO_FAILURE;
		}
#if defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
		if (punch_hole_enabled)
		{
			const int falloc_rc = fallocate(fd_in, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
				punch_offset, (off_t)moved);
			if (falloc_rc != 0)
			{
				if (errno == EOPNOTSUPP || errno == ENOSYS || errno == ENOTTY || errno == EINVAL)
				{
					punch_hole_enabled = false;
				}
				else
				{
					fclose(stream->stream);
					if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
					delete stream;
					return ZIPC_IO_FAILURE;
				}
			}
		}
#endif
		remaining -= (size_t)moved;
	}
	if (!seek64(handle->fp, 0, SEEK_CUR))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	filenode node;
	node.size = data_size;
	if (!add_u64(local_offset, 30U + name_len + 20U, &node.data_offset))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_UNSUPPORTED_FEATURE;
	}
	node.local_offset = local_offset;
	node.crc = crc;
	handle->files.emplace(stream->path, node);

	const enum zipc_status status = write_central_directory(handle);
	if (status != ZIPC_SUCCESS) handle->files.erase(stream->path);

	fclose(stream->stream);
	if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
	delete stream;
	return status;
}

enum zipc_status zipc_validate(zipc* handle)
{
	assert(handle);
	if (!handle) return ZIPC_SYNTAX_ERROR;
	if (!handle->fp) return ZIPC_IO_FAILURE;
	if (!handle->central_dir_known) return ZIPC_CORRUPT_ARCHIVE;

	zipc disk_state;
	disk_state.fp = handle->fp;
	const enum zipc_status load_status = load_existing_archive(&disk_state);
	if (load_status != ZIPC_SUCCESS) return load_status;
	if (!disk_state.central_dir_known) return ZIPC_CORRUPT_ARCHIVE;
	if (disk_state.central_dir_offset != handle->central_dir_offset) return ZIPC_CORRUPT_ARCHIVE;
	if (disk_state.files.size() != handle->files.size()) return ZIPC_CORRUPT_ARCHIVE;
	for (const auto& kv : handle->files)
	{
		const auto it = disk_state.files.find(kv.first);
		if (it == disk_state.files.end()) return ZIPC_CORRUPT_ARCHIVE;
		const filenode& expected = kv.second;
		const filenode& actual = it->second;
		if (expected.size != actual.size ||
			expected.data_offset != actual.data_offset ||
			expected.local_offset != actual.local_offset ||
			expected.crc != actual.crc)
		{
			return ZIPC_CORRUPT_ARCHIVE;
		}
	}

	if (!seek64(handle->fp, 0, SEEK_END)) return ZIPC_IO_FAILURE;
	uint64_t file_size = 0;
	if (!tell64(handle->fp, &file_size)) return ZIPC_IO_FAILURE;
	if (handle->central_dir_offset > file_size) return ZIPC_CORRUPT_ARCHIVE;

	std::vector<unsigned char> local(30);
	std::vector<unsigned char> buf(64 * 1024);

	for (const auto& kv : handle->files)
	{
		const std::string& name = kv.first;
		const filenode& node = kv.second;

		uint64_t local_header_end = 0;
		if (!add_u64(node.local_offset, local.size(), &local_header_end) ||
			local_header_end > file_size) return ZIPC_CORRUPT_ARCHIVE;
		if (!seek64(handle->fp, node.local_offset, SEEK_SET)) return ZIPC_IO_FAILURE;
		if (!read_fully(handle->fp, local.data(), local.size())) return ZIPC_IO_FAILURE;
		if (read_le32(local.data()) != 0x04034b50) return ZIPC_CORRUPT_ARCHIVE;

		const uint16_t flags = read_le16(local.data() + 6);
		const uint16_t method = read_le16(local.data() + 8);
		const uint32_t crc = read_le32(local.data() + 14);
		const uint32_t compressed_size = read_le32(local.data() + 18);
		const uint32_t uncompressed_size = read_le32(local.data() + 22);
		const uint16_t name_len = read_le16(local.data() + 26);
		const uint16_t extra_len = read_le16(local.data() + 28);

		if (method != 0) return ZIPC_UNSUPPORTED_FEATURE;
		if (flags & 0x08) return ZIPC_UNSUPPORTED_FEATURE;
		if (name_len != name.size()) return ZIPC_CORRUPT_ARCHIVE;
		if (node.crc != crc) return ZIPC_CORRUPT_ARCHIVE;

		std::string name_on_disk(name_len, '\0');
		if (!read_fully(handle->fp, name_on_disk.data(), name_len)) return ZIPC_IO_FAILURE;
		if (name_on_disk != name) return ZIPC_CORRUPT_ARCHIVE;
		std::vector<unsigned char> extra(extra_len);
		if (extra_len > 0 && !read_fully(handle->fp, extra.data(), extra_len)) return ZIPC_IO_FAILURE;

		const bool need_uncompressed = uncompressed_size == 0xFFFFFFFFU;
		const bool need_compressed = compressed_size == 0xFFFFFFFFU;
		zip64_values values;
		if ((need_uncompressed || need_compressed) &&
			!parse_zip64_extra(extra.data(), extra.size(), need_uncompressed, need_compressed, false, &values))
		{
			return ZIPC_CORRUPT_ARCHIVE;
		}
		const uint64_t compressed_size64 = need_compressed ? values.compressed_size : compressed_size;
		const uint64_t uncompressed_size64 = need_uncompressed ? values.uncompressed_size : uncompressed_size;
		if (compressed_size64 != uncompressed_size64) return ZIPC_UNSUPPORTED_FEATURE;
		if (node.size != uncompressed_size64) return ZIPC_CORRUPT_ARCHIVE;

		uint64_t data_offset = 0;
		if (!add_u64(node.local_offset, 30U + (uint64_t)name_len + (uint64_t)extra_len, &data_offset))
		{
			return ZIPC_CORRUPT_ARCHIVE;
		}
		if (data_offset != node.data_offset) return ZIPC_CORRUPT_ARCHIVE;
		if (node.size > file_size) return ZIPC_CORRUPT_ARCHIVE;
		if (data_offset > file_size - node.size) return ZIPC_CORRUPT_ARCHIVE;
		if (handle->central_dir_offset > 0)
		{
			if (node.size > handle->central_dir_offset) return ZIPC_CORRUPT_ARCHIVE;
			if (data_offset > handle->central_dir_offset - node.size) return ZIPC_CORRUPT_ARCHIVE;
		}

		uint32_t crc_calc = 0xFFFFFFFFU;
		if (node.size > 0)
		{
			if (!seek64(handle->fp, data_offset, SEEK_SET)) return ZIPC_IO_FAILURE;
			uint64_t remaining = node.size;
			while (remaining > 0)
			{
				const size_t chunk = remaining < (uint64_t)buf.size() ? (size_t)remaining : buf.size();
				if (!read_fully(handle->fp, buf.data(), chunk)) return ZIPC_IO_FAILURE;
				crc_calc = crc32_update(crc_calc, buf.data(), chunk);
				remaining -= chunk;
			}
		}
		crc_calc ^= 0xFFFFFFFFU;
		if (crc_calc != node.crc) return ZIPC_CORRUPT_ARCHIVE;
	}

	return ZIPC_SUCCESS;
}

struct compare_diff
{
	std::string name;
	enum zipc_diff_kind kind;
	uint64_t size_first = 0;
	uint64_t size_second = 0;
	uint64_t offset_first_diff = 0;
};

static const zipc_comparison* zipc_compare_status(enum zipc_status status)
{
	zipc_comparison* c = (zipc_comparison*)malloc(sizeof(zipc_comparison));
	if (!c) return nullptr;
	c->status = status;
	c->count = 0;
	c->differences = nullptr;
	return c;
}

static enum zipc_status zipc_find_first_difference(zipc* first, const filenode& first_node,
	zipc* second, const filenode& second_node,
	std::vector<unsigned char>& first_buf, std::vector<unsigned char>& second_buf,
	uint64_t* offset_first_diff)
{
	assert(first);
	assert(second);
	assert(offset_first_diff);

	*offset_first_diff = 0;
	const uint64_t shared_size = std::min(first_node.size, second_node.size);
	if (!seek64(first->fp, first_node.data_offset, SEEK_SET)) return ZIPC_IO_FAILURE;
	if (!seek64(second->fp, second_node.data_offset, SEEK_SET)) return ZIPC_IO_FAILURE;

	uint64_t compared = 0;
	while (compared < shared_size)
	{
		const size_t chunk = shared_size - compared < (uint64_t)first_buf.size() ?
			(size_t)(shared_size - compared) : first_buf.size();
		if (!read_fully(first->fp, first_buf.data(), chunk)) return ZIPC_IO_FAILURE;
		if (!read_fully(second->fp, second_buf.data(), chunk)) return ZIPC_IO_FAILURE;
		if (memcmp(first_buf.data(), second_buf.data(), chunk) != 0)
		{
			for (size_t i = 0; i < chunk; ++i)
			{
				if (first_buf[i] != second_buf[i])
				{
					*offset_first_diff = compared + i;
					return ZIPC_SUCCESS;
				}
			}
		}
		compared += chunk;
	}

	*offset_first_diff = shared_size;
	return ZIPC_SUCCESS;
}

static const zipc_comparison* zipc_build_comparison(const std::vector<compare_diff>& differences)
{
	if (differences.size() > (size_t)INT_MAX) return zipc_compare_status(ZIPC_UNSUPPORTED_FEATURE);

	size_t names_size = 0;
	for (const compare_diff& diff : differences)
	{
		if (names_size > SIZE_MAX - diff.name.size() - 1) return zipc_compare_status(ZIPC_UNSUPPORTED_FEATURE);
		names_size += diff.name.size() + 1;
	}

	const size_t count = differences.size();
	const size_t array_align = alignof(zipc_file_diff);
	size_t array_offset = sizeof(zipc_comparison);
	if (array_offset % array_align != 0) array_offset += array_align - (array_offset % array_align);
	if (count > 0 && count > (SIZE_MAX - array_offset) / sizeof(zipc_file_diff))
	{
		return zipc_compare_status(ZIPC_UNSUPPORTED_FEATURE);
	}
	const size_t names_offset = array_offset + count * sizeof(zipc_file_diff);
	if (names_size > SIZE_MAX - names_offset) return zipc_compare_status(ZIPC_UNSUPPORTED_FEATURE);
	const size_t total_size = names_offset + names_size;

	zipc_comparison* comparison = (zipc_comparison*)malloc(total_size);
	if (!comparison) return nullptr;

	comparison->status = ZIPC_SUCCESS;
	comparison->count = (int)count;
	comparison->differences = nullptr;
	if (count == 0) return comparison;

	zipc_file_diff* diff_array = (zipc_file_diff*)((unsigned char*)comparison + array_offset);
	char* names = (char*)comparison + names_offset;
	comparison->differences = diff_array;

	for (size_t i = 0; i < count; ++i)
	{
		const compare_diff& src = differences[i];
		zipc_file_diff& dst = diff_array[i];
		memcpy(names, src.name.c_str(), src.name.size() + 1);
		dst.name = names;
		dst.kind = src.kind;
		dst.size_first = src.size_first;
		dst.size_second = src.size_second;
		dst.offset_first_diff = src.offset_first_diff;
		names += src.name.size() + 1;
	}

	return comparison;
}

static const char* zipc_diff_kind_string(enum zipc_diff_kind kind)
{
	switch (kind)
	{
		case ZIPC_DIFF_CONTENT: return "content differs";
		case ZIPC_DIFF_ONLY_IN_FIRST: return "only in first";
		case ZIPC_DIFF_ONLY_IN_SECOND: return "only in second";
		default: return "unknown difference";
	}
}

/// Compare two zip files and list differences. You must call `zipc_compare_free()` on the
/// return pointer once you are done with the data.
const zipc_comparison* zipc_compare(const char* first, const char* second)
{
	if (!first || !second) return zipc_compare_status(ZIPC_SYNTAX_ERROR);

	enum zipc_status status = ZIPC_SUCCESS;
	zipc* first_zip = zipc_open(first, "r", &status);
	if (!first_zip) return zipc_compare_status(status);

	enum zipc_status second_status = ZIPC_SUCCESS;
	zipc* second_zip = zipc_open(second, "r", &second_status);
	if (!second_zip)
	{
		const enum zipc_status close_status = zipc_close(first_zip);
		(void)close_status;
		return zipc_compare_status(second_status);
	}

	std::vector<std::string> names;
	names.reserve(first_zip->files.size() + second_zip->files.size());
	for (const auto& kv : first_zip->files) names.push_back(kv.first);
	for (const auto& kv : second_zip->files) names.push_back(kv.first);
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());

	std::vector<compare_diff> differences;
	differences.reserve(names.size());
	std::vector<unsigned char> first_buf(64 * 1024);
	std::vector<unsigned char> second_buf(64 * 1024);

	for (const std::string& name : names)
	{
		const auto first_it = first_zip->files.find(name);
		const auto second_it = second_zip->files.find(name);
		if (first_it == first_zip->files.end())
		{
			differences.push_back(compare_diff{name, ZIPC_DIFF_ONLY_IN_SECOND, 0, second_it->second.size, 0});
			continue;
		}
		if (second_it == second_zip->files.end())
		{
			differences.push_back(compare_diff{name, ZIPC_DIFF_ONLY_IN_FIRST, first_it->second.size, 0, 0});
			continue;
		}

		const filenode& first_node = first_it->second;
		const filenode& second_node = second_it->second;
		if (first_node.size == second_node.size && first_node.crc == second_node.crc) continue;

		uint64_t offset_first_diff = 0;
		status = zipc_find_first_difference(first_zip, first_node, second_zip, second_node,
			first_buf, second_buf, &offset_first_diff);
		if (status != ZIPC_SUCCESS) break;
		if (first_node.size == second_node.size && offset_first_diff == first_node.size) continue;

		differences.push_back(compare_diff{name, ZIPC_DIFF_CONTENT,
			first_node.size, second_node.size, offset_first_diff});
	}

	const enum zipc_status first_close_status = zipc_close(first_zip);
	const enum zipc_status second_close_status = zipc_close(second_zip);
	if (status == ZIPC_SUCCESS && first_close_status != ZIPC_SUCCESS) status = first_close_status;
	if (status == ZIPC_SUCCESS && second_close_status != ZIPC_SUCCESS) status = second_close_status;

	if (status != ZIPC_SUCCESS) return zipc_compare_status(status);
	return zipc_build_comparison(differences);
}

/// Convenience helper function to pretty print the contents of the `zipc_file_diff` array.
void zipc_print_zipc_file_diff(const zipc_comparison* differences, FILE* out)
{
	if (!differences || !out) return;
	if (differences->status != ZIPC_SUCCESS)
	{
		fprintf(out, "comparison failed: %s\n", zipc_strerror(differences->status));
		return;
	}
	if (differences->count == 0)
	{
		fprintf(out, "no differences\n");
		return;
	}

	for (int i = 0; i < differences->count; ++i)
	{
		const zipc_file_diff& diff = differences->differences[i];
		const char* name = diff.name ? diff.name : "(null)";
		switch (diff.kind)
		{
			case ZIPC_DIFF_CONTENT:
				fprintf(out, "%s: %s at byte %" PRIu64 " (%" PRIu64 " vs %" PRIu64 " bytes)\n",
					name, zipc_diff_kind_string(diff.kind),
					diff.offset_first_diff, diff.size_first, diff.size_second);
				break;
			case ZIPC_DIFF_ONLY_IN_FIRST:
			case ZIPC_DIFF_ONLY_IN_SECOND:
				fprintf(out, "%s: %s (%" PRIu64 " vs %" PRIu64 " bytes)\n",
					name, zipc_diff_kind_string(diff.kind),
					diff.size_first, diff.size_second);
				break;
			default:
				fprintf(out, "%s: %s\n", name, zipc_diff_kind_string(diff.kind));
				break;
		}
	}
}

void zipc_compare_free(const zipc_comparison* differences)
{
	free((void*)differences);
}

std::vector<std::string> zipc_files(const std::string& zipname, const std::string& startsWith)
{
	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(zipname.c_str(), "r", &status);
	if (!z) return {};

	std::vector<std::string> files;
	files.reserve(z->files.size());
	for (const auto& kv : z->files)
	{
		if (!startsWith.empty() && kv.first.rfind(startsWith, 0) != 0) continue;
		files.push_back(kv.first);
	}
	std::sort(files.begin(), files.end());

	const enum zipc_status close_status = zipc_close(z);
	if (close_status != ZIPC_SUCCESS) return {};
	return files;
}

static void zipc_stream_abort(zipcstream* stream)
{
	if (!stream) return;
	if (stream->stream) fclose(stream->stream);
	if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
	delete stream;
}

static enum zipc_status zipc_close_preserving_status(zipc* z, enum zipc_status status)
{
	const enum zipc_status close_status = zipc_close(z);
	if (status == ZIPC_SUCCESS && close_status != ZIPC_SUCCESS) return close_status;
	return status;
}

static enum zipc_status zipc_file_open_status()
{
	if (errno == ENOENT || errno == ENOTDIR) return ZIPC_PATH_NOT_FOUND;
	if (errno == EACCES || errno == EPERM) return ZIPC_PERMISSION_FAILURE;
	return ZIPC_IO_FAILURE;
}

static bool is_path_safe(const std::string& path)
{
	if (path.empty()) return false;
	if (path[0] == '/') return false;
	if (path.find('\\') != std::string::npos) return false;

	if (path == ".." ||
		path.find("../") == 0 ||
		path.find("/../") != std::string::npos ||
		(path.length() >= 3 && path.substr(path.length() - 3) == "/.."))
	{
		return false;
	}

	return true;
}

static std::string zipc_temp_path_for(const std::string& path)
{
	const size_t slash = path.find_last_of('/');
	std::string dir;
	std::string base;
	if (slash == std::string::npos)
	{
		dir = ".";
		base = path;
	}
	else if (slash == 0)
	{
		dir = "/";
		base = path.substr(1);
	}
	else
	{
		dir = path.substr(0, slash);
		base = path.substr(slash + 1);
	}
	if (base.empty()) base = "zipc";
	if (dir == "/") return "/." + base + ".zipc_tmp_XXXXXX";
	return dir + "/." + base + ".zipc_tmp_XXXXXX";
}

enum zipc_status zipc_add_file(const std::string& zipname, const std::string& targetFile)
{
	if (zipname.empty() || targetFile.empty()) return ZIPC_SYNTAX_ERROR;
	if (!is_path_safe(targetFile)) return ZIPC_SYNTAX_ERROR;

	FILE* input = fopen(targetFile.c_str(), "rb");
	if (!input) return zipc_file_open_status();

	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(zipname.c_str(), "a", &status);
	if (!z)
	{
		fclose(input);
		return status;
	}

	zipcstream* stream = zipc_stream_open(z, targetFile.c_str(), "", &status);
	if (!stream)
	{
		fclose(input);
		return zipc_close_preserving_status(z, status);
	}

	std::vector<unsigned char> buf(64 * 1024);
	for (;;)
	{
		const size_t bytes = fread(buf.data(), 1, buf.size(), input);
		if (bytes > 0)
		{
			status = zipc_stream_write(z, stream, bytes, buf.data());
			if (status != ZIPC_SUCCESS)
			{
				zipc_stream_abort(stream);
				fclose(input);
				return zipc_close_preserving_status(z, status);
			}
		}
		if (bytes < buf.size())
		{
			if (ferror(input))
			{
				zipc_stream_abort(stream);
				fclose(input);
				return zipc_close_preserving_status(z, ZIPC_IO_FAILURE);
			}
			break;
		}
	}

	if (fclose(input) != 0)
	{
		zipc_stream_abort(stream);
		return zipc_close_preserving_status(z, ZIPC_IO_FAILURE);
	}

	status = zipc_stream_close(z, stream);
	return zipc_close_preserving_status(z, status);
}

enum zipc_status zipc_extract_file(const std::string& zipname, const std::string& targetFile)
{
	if (zipname.empty() || targetFile.empty()) return ZIPC_SYNTAX_ERROR;
	if (!is_path_safe(targetFile)) return ZIPC_SYNTAX_ERROR;
	if (zipname == targetFile) return ZIPC_PATH_NOT_FOUND;

	enum zipc_status status = ZIPC_SUCCESS;
	zipc* z = zipc_open(zipname.c_str(), "r", &status);
	if (!z) return status;

	const auto it = z->files.find(targetFile);
	if (it == z->files.end()) return zipc_close_preserving_status(z, ZIPC_PATH_NOT_FOUND);
	const filenode& node = it->second;

	std::string temp_template = zipc_temp_path_for(targetFile);
	std::vector<char> temp_path(temp_template.begin(), temp_template.end());
	temp_path.push_back('\0');
	const int output_fd = mkstemp(temp_path.data());
	if (output_fd < 0) return zipc_close_preserving_status(z, zipc_file_open_status());

	FILE* output = fdopen(output_fd, "wb");
	if (!output)
	{
		close(output_fd);
		unlink(temp_path.data());
		return zipc_close_preserving_status(z, ZIPC_IO_FAILURE);
	}

	if (!seek64(z->fp, node.data_offset, SEEK_SET))
	{
		fclose(output);
		unlink(temp_path.data());
		return zipc_close_preserving_status(z, ZIPC_IO_FAILURE);
	}

	std::vector<unsigned char> buf(64 * 1024);
	uint64_t remaining = node.size;
	while (remaining > 0)
	{
		const size_t chunk = remaining < (uint64_t)buf.size() ? (size_t)remaining : buf.size();
		if (!read_fully(z->fp, buf.data(), chunk) || !write_all(output, buf.data(), chunk))
		{
			fclose(output);
			unlink(temp_path.data());
			return zipc_close_preserving_status(z, ZIPC_IO_FAILURE);
		}
		remaining -= chunk;
	}

	if (fclose(output) != 0)
	{
		unlink(temp_path.data());
		return zipc_close_preserving_status(z, ZIPC_IO_FAILURE);
	}
	if (rename(temp_path.data(), targetFile.c_str()) != 0)
	{
		unlink(temp_path.data());
		return zipc_close_preserving_status(z, ZIPC_IO_FAILURE);
	}
	return zipc_close_preserving_status(z, ZIPC_SUCCESS);
}
