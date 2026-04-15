#include "zipc.h"
#include "zipc_utility.h"

#include <assert.h>
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

extern "C" ssize_t copy_file_range(int fd_in, off_t* off_in, int fd_out, off_t* off_out, size_t len, unsigned int flags);

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
	size_t data_offset;
	uint32_t local_offset;
	uint32_t crc;
};

struct zipc
{
	std::string filename;
	zipc_mode mode;
	FILE* fp = nullptr;
	std::unordered_map<std::string, filenode> files;
	size_t central_dir_offset = 0;
	bool central_dir_known = false;
	bool map_write_active = false;
	void* map_write_base = nullptr;
	void* map_write_data = nullptr;
	size_t map_write_length = 0;
	size_t map_write_max = 0;
	size_t map_write_data_offset = 0;
	uint32_t map_write_local_offset = 0;
	std::string map_write_path;
};

struct zipcstream
{
	zipc* parent = nullptr;
	FILE* stream = nullptr;
	std::string path;
	std::string temp_path;
	size_t size = 0;
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

static bool write_empty_archive(FILE* fp)
{
	static const unsigned char eocd[22] = {0x50, 0x4b, 0x05, 0x06,
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	if (fseek(fp, 0, SEEK_SET) != 0) return false;
	if (!write_all(fp, eocd, sizeof(eocd))) return false;
	return fflush(fp) == 0;
}

static bool compute_data_offset(FILE* fp, uint32_t local_offset, size_t* data_offset)
{
	if (fseek(fp, local_offset, SEEK_SET) != 0) return false;
	unsigned char local[30];
	if (!read_fully(fp, local, sizeof(local))) return false;
	if (read_le32(local) != 0x04034b50) return false;
	const uint16_t name_len = read_le16(local + 26);
	const uint16_t extra_len = read_le16(local + 28);
	*data_offset = static_cast<size_t>(local_offset) + 30U + name_len + extra_len;
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

	const long cd_start_long = ftell(handle->fp);
	if (cd_start_long < 0) return ZIPC_IO_FAILURE;
	if (cd_start_long > 0xFFFFFFFFL) return ZIPC_UNSUPPORTED_FEATURE;
	const uint32_t cd_start = (uint32_t)cd_start_long;

	for (const auto& name : names)
	{
		const filenode& n = handle->files.at(name);
		if (n.size > 0xFFFFFFFFULL) return ZIPC_UNSUPPORTED_FEATURE;
		if (name.size() > 0xFFFF) return ZIPC_UNSUPPORTED_FEATURE;

		unsigned char cd[46] = {0};
		cd[0] = 0x50; cd[1] = 0x4b; cd[2] = 0x01; cd[3] = 0x02;
		cd[4] = 20; cd[5] = 0; // version made by
		cd[6] = 20; cd[7] = 0; // version needed to extract
		// flags: 0
		// compression: 0
		// mod time/date: 0
		cd[16] = (unsigned char)(n.crc & 0xFF);
		cd[17] = (unsigned char)((n.crc >> 8) & 0xFF);
		cd[18] = (unsigned char)((n.crc >> 16) & 0xFF);
		cd[19] = (unsigned char)((n.crc >> 24) & 0xFF);

		const uint32_t sz32 = (uint32_t)n.size;
		cd[20] = (unsigned char)(sz32 & 0xFF);
		cd[21] = (unsigned char)((sz32 >> 8) & 0xFF);
		cd[22] = (unsigned char)((sz32 >> 16) & 0xFF);
		cd[23] = (unsigned char)((sz32 >> 24) & 0xFF);
		cd[24] = cd[20];
		cd[25] = cd[21];
		cd[26] = cd[22];
		cd[27] = cd[23];

		const uint16_t nlen = (uint16_t)name.size();
		cd[28] = (unsigned char)(nlen & 0xFF);
		cd[29] = (unsigned char)((nlen >> 8) & 0xFF);
		// extra len 0, comment len 0, disk start 0, int/ext attrs 0
		const uint32_t loc = n.local_offset;
		cd[42] = (unsigned char)(loc & 0xFF);
		cd[43] = (unsigned char)((loc >> 8) & 0xFF);
		cd[44] = (unsigned char)((loc >> 16) & 0xFF);
		cd[45] = (unsigned char)((loc >> 24) & 0xFF);

		if (!write_all(handle->fp, cd, sizeof(cd))) return ZIPC_IO_FAILURE;
		if (!write_all(handle->fp, name.data(), nlen)) return ZIPC_IO_FAILURE;
	}

	const long cd_end_long = ftell(handle->fp);
	if (cd_end_long < 0) return ZIPC_IO_FAILURE;
	const uint32_t cd_size = (uint32_t)(cd_end_long - (long)cd_start);
	if (names.size() > 0xFFFF) return ZIPC_UNSUPPORTED_FEATURE;
	const uint16_t entry_count = (uint16_t)names.size();

	unsigned char eocd[22] = {0};
	eocd[0] = 0x50; eocd[1] = 0x4b; eocd[2] = 0x05; eocd[3] = 0x06;
	// disk numbers 0
	eocd[8] = (unsigned char)(entry_count & 0xFF);
	eocd[9] = (unsigned char)((entry_count >> 8) & 0xFF);
	eocd[10] = eocd[8];
	eocd[11] = eocd[9];
	eocd[12] = (unsigned char)(cd_size & 0xFF);
	eocd[13] = (unsigned char)((cd_size >> 8) & 0xFF);
	eocd[14] = (unsigned char)((cd_size >> 16) & 0xFF);
	eocd[15] = (unsigned char)((cd_size >> 24) & 0xFF);
	eocd[16] = (unsigned char)(cd_start & 0xFF);
	eocd[17] = (unsigned char)((cd_start >> 8) & 0xFF);
	eocd[18] = (unsigned char)((cd_start >> 16) & 0xFF);
	eocd[19] = (unsigned char)((cd_start >> 24) & 0xFF);
	// comment length 0

	if (!write_all(handle->fp, eocd, sizeof(eocd))) return ZIPC_IO_FAILURE;
	if (fflush(handle->fp) != 0) return ZIPC_IO_FAILURE;

	handle->central_dir_offset = cd_start;
	handle->central_dir_known = true;
	return ZIPC_SUCCESS;
}

static enum zipc_status load_existing_archive(zipc* z)
{
	FILE* fp = z->fp;
	if (fseek(fp, 0, SEEK_END) != 0) return ZIPC_IO_FAILURE;
	const long file_size = ftell(fp);
	if (file_size < 0) return ZIPC_IO_FAILURE;
	if (file_size == 0) return ZIPC_SUCCESS;
	if (file_size < 22) return ZIPC_CORRUPT_ARCHIVE;

	const size_t max_search = 0xFFFF + 22;
	size_t search = static_cast<size_t>(file_size);
	if (search > max_search) search = max_search;

	std::vector<unsigned char> tail(search);
	if (fseek(fp, file_size - static_cast<long>(search), SEEK_SET) != 0) return ZIPC_IO_FAILURE;
	if (!read_fully(fp, tail.data(), search)) return ZIPC_IO_FAILURE;

	ssize_t eocd_idx = -1;
	for (size_t i = search - 22;; --i)
	{
		if (read_le32(&tail[i]) == 0x06054b50)
		{
			eocd_idx = static_cast<ssize_t>(i);
			break;
		}
		if (i == 0) break;
	}
	if (eocd_idx < 0) return ZIPC_CORRUPT_ARCHIVE;

	const long eocd_pos = file_size - static_cast<long>(search) + eocd_idx;
	const unsigned char* eocd = tail.data() + eocd_idx;
	const uint16_t entry_count = read_le16(eocd + 10);
	const uint32_t cd_size = read_le32(eocd + 12);
	const uint32_t cd_offset = read_le32(eocd + 16);
	if (cd_offset + cd_size > static_cast<uint32_t>(eocd_pos)) return ZIPC_CORRUPT_ARCHIVE;

	if (fseek(fp, cd_offset, SEEK_SET) != 0) return ZIPC_IO_FAILURE;
	std::vector<unsigned char> header(46);
	for (uint16_t i = 0; i < entry_count; ++i)
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
		const uint32_t local_offset = read_le32(header.data() + 42);

		if (method != 0) return ZIPC_UNSUPPORTED_FEATURE;
		if (flags & 0x08) return ZIPC_UNSUPPORTED_FEATURE;
		if (compressed_size != uncompressed_size) return ZIPC_UNSUPPORTED_FEATURE;

		std::string name(name_len, '\0');
		if (!read_fully(fp, name.data(), name_len)) return ZIPC_CORRUPT_ARCHIVE;
		if (fseek(fp, extra_len + comment_len, SEEK_CUR) != 0) return ZIPC_CORRUPT_ARCHIVE;

		size_t data_offset = 0;
		const long return_pos = ftell(fp);
		if (!compute_data_offset(fp, local_offset, &data_offset)) return ZIPC_CORRUPT_ARCHIVE;
		if (fseek(fp, return_pos, SEEK_SET) != 0) return ZIPC_IO_FAILURE;

		if (z->files.count(name) == 0)
		{
			z->files.emplace(std::move(name), filenode{static_cast<size_t>(uncompressed_size), data_offset, local_offset, crc});
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

	if (z->mode == ZIPC_APPEND) fseek(z->fp, 0, SEEK_END);
	return z;
}

enum zipc_status zipc_close(zipc* handle)
{
	enum zipc_status r = ZIPC_SUCCESS;
	if (!handle) return r;
	if (handle->map_write_active) r = ZIPC_SYNTAX_ERROR;
	if (handle->fp && fclose(handle->fp) != 0) r = ZIPC_IO_FAILURE;
	delete handle;
	return ZIPC_SUCCESS;
}

ssize_t zipc_filesize(zipc* handle, const char* path)
{
	assert(handle);
	assert(path);
	if (handle->files.count(path) == 0) return -1;
	const auto& v = handle->files.at(path);
	return v.size;
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
	const size_t page_offset = node.data_offset % pagesize;
	const size_t map_offset = node.data_offset - page_offset;
	const size_t map_length = node.size + page_offset;

	const int fd = fileno(handle->fp);
	if (fd < 0)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return m;
	}

	void* base = mmap(nullptr, map_length, PROT_READ, MAP_PRIVATE, fd, (off_t)map_offset);
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

zipc_mapping zipc_map_write(zipc* handle, const char* path, enum zipc_status* err, size_t max)
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
	if (max > 0xFFFFFFFFULL)
	{
		if (err) *err = ZIPC_UNSUPPORTED_FEATURE;
		return mapping;
	}
	if (max == 0)
	{
		const enum zipc_status st = zipc_write(handle, path, 0, nullptr);
		if (err) *err = st;
		if (st != ZIPC_SUCCESS) return mapping;
		static char empty = 0;
		mapping.data = &empty;
		mapping.size = 0;
		return mapping;
	}

	if (handle->central_dir_known)
	{
		if (fseek(handle->fp, (long)handle->central_dir_offset, SEEK_SET) != 0)
		{
			if (err) *err = ZIPC_IO_FAILURE;
			return mapping;
		}
		const int fd = fileno(handle->fp);
		if (fd >= 0 && ftruncate(fd, (off_t)handle->central_dir_offset) != 0)
		{
			if (err) *err = ZIPC_IO_FAILURE;
			return mapping;
		}
	}
	else if (fseek(handle->fp, 0, SEEK_END) != 0)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	const long local_offset_long = ftell(handle->fp);
	if (local_offset_long < 0 || local_offset_long > 0xFFFFFFFFL)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}
	const uint32_t local_offset = (uint32_t)local_offset_long;

	const uint32_t data_size32 = (uint32_t)max;
	unsigned char local[30] = {0};
	local[0] = 0x50; local[1] = 0x4b; local[2] = 0x03; local[3] = 0x04;
	local[4] = 20; // version needed to extract
	local[5] = 0;
	// flags: 0
	// compression: 0
	// mod time/date: 0
	// crc: 0 for now
	local[18] = (unsigned char)(data_size32 & 0xFF);
	local[19] = (unsigned char)((data_size32 >> 8) & 0xFF);
	local[20] = (unsigned char)((data_size32 >> 16) & 0xFF);
	local[21] = (unsigned char)((data_size32 >> 24) & 0xFF);
	local[22] = local[18];
	local[23] = local[19];
	local[24] = local[20];
	local[25] = local[21];
	local[26] = (unsigned char)(name_len & 0xFF);
	local[27] = (unsigned char)((name_len >> 8) & 0xFF);
	// extra length 0

	if (!write_all(handle->fp, local, sizeof(local)))
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}
	if (!write_all(handle->fp, path, name_len))
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	const size_t data_offset = (size_t)local_offset + 30U + name_len;
	const size_t end_offset = data_offset + max;
	const int fd = fileno(handle->fp);
	if (fd < 0 || ftruncate(fd, (off_t)end_offset) != 0)
	{
		if (err) *err = ZIPC_IO_FAILURE;
		return mapping;
	}

	const long pagesize_long = sysconf(_SC_PAGESIZE);
	const size_t pagesize = pagesize_long > 0 ? (size_t)pagesize_long : 4096;
	const size_t page_offset = data_offset % pagesize;
	const size_t map_offset = data_offset - page_offset;
	const size_t map_length = max + page_offset;

	void* base = mmap(nullptr, map_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)map_offset);
	if (base == MAP_FAILED)
	{
		ftruncate(fd, (off_t)local_offset);
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

enum zipc_status zipc_unmap_write(zipc* handle, zipc_mapping mapping, size_t size)
{
	assert(handle);
	if (!handle || !mapping.data) return ZIPC_SYNTAX_ERROR;
	if (!handle->map_write_active) return ZIPC_SYNTAX_ERROR;
	if (mapping.data != handle->map_write_data) return ZIPC_SYNTAX_ERROR;
	if (size > handle->map_write_max) return ZIPC_SYNTAX_ERROR;
	if (!handle->fp) return ZIPC_IO_FAILURE;

	enum zipc_status status = ZIPC_SUCCESS;
	const uint32_t crc = crc32_calc(mapping.data, size);
	if (munmap(handle->map_write_base, handle->map_write_length) != 0) status = ZIPC_IO_FAILURE;

	const size_t end_offset = handle->map_write_data_offset + size;
	if (status == ZIPC_SUCCESS && size < handle->map_write_max)
	{
		const int fd = fileno(handle->fp);
		if (fd >= 0 && ftruncate(fd, (off_t)end_offset) != 0) status = ZIPC_IO_FAILURE;
	}

	if (status == ZIPC_SUCCESS &&
		fseek(handle->fp, (long)handle->map_write_local_offset + 14, SEEK_SET) != 0) status = ZIPC_IO_FAILURE;
	unsigned char header[12];
	if (status == ZIPC_SUCCESS)
	{
		header[0] = (unsigned char)(crc & 0xFF);
		header[1] = (unsigned char)((crc >> 8) & 0xFF);
		header[2] = (unsigned char)((crc >> 16) & 0xFF);
		header[3] = (unsigned char)((crc >> 24) & 0xFF);
		const uint32_t size32 = (uint32_t)size;
		header[4] = (unsigned char)(size32 & 0xFF);
		header[5] = (unsigned char)((size32 >> 8) & 0xFF);
		header[6] = (unsigned char)((size32 >> 16) & 0xFF);
		header[7] = (unsigned char)((size32 >> 24) & 0xFF);
		header[8] = header[4];
		header[9] = header[5];
		header[10] = header[6];
		header[11] = header[7];
		if (!write_all(handle->fp, header, sizeof(header))) status = ZIPC_IO_FAILURE;
	}

	if (status == ZIPC_SUCCESS &&
		fseek(handle->fp, (long)end_offset, SEEK_SET) != 0) status = ZIPC_IO_FAILURE;
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

enum zipc_status zipc_write(zipc* handle, const char* path, size_t size, const void* ptr)
{
	assert(handle);
	assert(path);
	if (handle->mode == ZIPC_READ_ONLY) return ZIPC_PERMISSION_FAILURE;
	if (!ptr && size > 0) return ZIPC_SYNTAX_ERROR;
	if (handle->files.count(path) != 0) return ZIPC_PATH_ALREADY_EXISTS;
	if (!handle->fp) return ZIPC_IO_FAILURE;

	if (size > 0xFFFFFFFFULL) return ZIPC_UNSUPPORTED_FEATURE;
	const size_t name_len = strlen(path);
	if (name_len > 0xFFFF) return ZIPC_UNSUPPORTED_FEATURE;

	if (handle->central_dir_known)
	{
		if (fseek(handle->fp, (long)handle->central_dir_offset, SEEK_SET) != 0) return ZIPC_IO_FAILURE;
		const int fd = fileno(handle->fp);
		if (fd >= 0 && ftruncate(fd, (off_t)handle->central_dir_offset) != 0) return ZIPC_IO_FAILURE;
	}
	else
	{
		if (fseek(handle->fp, 0, SEEK_END) != 0) return ZIPC_IO_FAILURE;
	}

	const long local_offset_long = ftell(handle->fp);
	if (local_offset_long < 0 || local_offset_long > 0xFFFFFFFFL) return ZIPC_IO_FAILURE;
	const uint32_t local_offset = (uint32_t)local_offset_long;

	const uint32_t crc = crc32_calc(ptr, size);
	const uint32_t data_size32 = (uint32_t)size;

	unsigned char local[30] = {0};
	local[0] = 0x50; local[1] = 0x4b; local[2] = 0x03; local[3] = 0x04;
	local[4] = 20; // version needed to extract
	local[5] = 0;
	// flags: 0
	// compression: 0
	// mod time/date: 0
	local[14] = (unsigned char)(crc & 0xFF);
	local[15] = (unsigned char)((crc >> 8) & 0xFF);
	local[16] = (unsigned char)((crc >> 16) & 0xFF);
	local[17] = (unsigned char)((crc >> 24) & 0xFF);
	local[18] = (unsigned char)(data_size32 & 0xFF);
	local[19] = (unsigned char)((data_size32 >> 8) & 0xFF);
	local[20] = (unsigned char)((data_size32 >> 16) & 0xFF);
	local[21] = (unsigned char)((data_size32 >> 24) & 0xFF);
	local[22] = local[18];
	local[23] = local[19];
	local[24] = local[20];
	local[25] = local[21];
	local[26] = (unsigned char)(name_len & 0xFF);
	local[27] = (unsigned char)((name_len >> 8) & 0xFF);
	// extra length 0

	if (!write_all(handle->fp, local, sizeof(local))) return ZIPC_IO_FAILURE;
	if (!write_all(handle->fp, path, name_len)) return ZIPC_IO_FAILURE;
	if (size > 0 && !write_all(handle->fp, ptr, size)) return ZIPC_IO_FAILURE;

	filenode node;
	node.size = size;
	node.data_offset = (size_t)local_offset + 30U + name_len;
	node.local_offset = local_offset;
	node.crc = crc;
	handle->files.emplace(path, node);

	return write_central_directory(handle);
}

enum zipc_status zipc_read(zipc* handle, const char* path, size_t size, void* ptr)
{
	assert(handle);
	assert(path);
	if (!handle->fp) return ZIPC_IO_FAILURE;
	if (handle->files.count(path) == 0) return ZIPC_PATH_NOT_FOUND;
	if (handle->mode == ZIPC_WRITE_ONLY) return ZIPC_PERMISSION_FAILURE;
	if (!ptr && size > 0) return ZIPC_SYNTAX_ERROR;

	const filenode& node = handle->files.at(path);
	if (size > node.size) return ZIPC_SYNTAX_ERROR;
	if (fseek(handle->fp, (long)node.data_offset, SEEK_SET) != 0) return ZIPC_IO_FAILURE;
	if (size > 0 && !read_fully(handle->fp, ptr, size)) return ZIPC_IO_FAILURE;
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

	const int fd = mkstemp(buf.data());
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

enum zipc_status zipc_stream_write(const zipc* handle, zipcstream* stream, size_t size, const void* ptr)
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

	if (!write_all(stream->stream, ptr, size)) return ZIPC_IO_FAILURE;
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
	if (fseek(stream->stream, 0, SEEK_END) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	const long size_long = ftell(stream->stream);
	if (size_long < 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	const size_t data_size = (size_t)size_long;
	if (stream->size != data_size)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (data_size > 0xFFFFFFFFULL)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_UNSUPPORTED_FEATURE;
	}
	const size_t name_len = stream->path.size();
	if (name_len > 0xFFFF)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_UNSUPPORTED_FEATURE;
	}
	if (fseek(stream->stream, 0, SEEK_SET) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	uint32_t crc = 0xFFFFFFFFU;
	std::vector<unsigned char> buf(64 * 1024);
	size_t remaining = data_size;
	while (remaining > 0)
	{
		const size_t chunk = remaining < buf.size() ? remaining : buf.size();
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
	if (fseek(stream->stream, 0, SEEK_SET) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	if (handle->central_dir_known)
	{
		if (fseek(handle->fp, (long)handle->central_dir_offset, SEEK_SET) != 0)
		{
			fclose(stream->stream);
			if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
			delete stream;
			return ZIPC_IO_FAILURE;
		}
		const int fd = fileno(handle->fp);
		if (fd >= 0 && ftruncate(fd, (off_t)handle->central_dir_offset) != 0)
		{
			fclose(stream->stream);
			if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
			delete stream;
			return ZIPC_IO_FAILURE;
		}
	}
	else if (fseek(handle->fp, 0, SEEK_END) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	const long local_offset_long = ftell(handle->fp);
	if (local_offset_long < 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (local_offset_long > 0xFFFFFFFFL)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_UNSUPPORTED_FEATURE;
	}
	const uint32_t local_offset = (uint32_t)local_offset_long;

	unsigned char local[30] = {0};
	local[0] = 0x50; local[1] = 0x4b; local[2] = 0x03; local[3] = 0x04;
	local[4] = 20; // version needed to extract
	local[5] = 0;
	// flags: 0
	// compression: 0
	// mod time/date: 0
	local[14] = (unsigned char)(crc & 0xFF);
	local[15] = (unsigned char)((crc >> 8) & 0xFF);
	local[16] = (unsigned char)((crc >> 16) & 0xFF);
	local[17] = (unsigned char)((crc >> 24) & 0xFF);
	const uint32_t data_size32 = (uint32_t)data_size;
	local[18] = (unsigned char)(data_size32 & 0xFF);
	local[19] = (unsigned char)((data_size32 >> 8) & 0xFF);
	local[20] = (unsigned char)((data_size32 >> 16) & 0xFF);
	local[21] = (unsigned char)((data_size32 >> 24) & 0xFF);
	local[22] = local[18];
	local[23] = local[19];
	local[24] = local[20];
	local[25] = local[21];
	local[26] = (unsigned char)(name_len & 0xFF);
	local[27] = (unsigned char)((name_len >> 8) & 0xFF);
	// extra length 0

	if (!write_all(handle->fp, local, sizeof(local)))
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}
	if (!write_all(handle->fp, stream->path.data(), name_len))
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
		size_t chunk = remaining;
		if (chunk > (1U << 30)) chunk = (1U << 30);
		const off_t punch_offset = lseek(fd_in, 0, SEEK_CUR);
		if (punch_offset < 0)
		{
			fclose(stream->stream);
			if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
			delete stream;
			return ZIPC_IO_FAILURE;
		}
		const ssize_t moved = copy_file_range(fd_in, nullptr, fd_out, nullptr, chunk, 0);
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
	if (fseek(handle->fp, 0, SEEK_CUR) != 0)
	{
		fclose(stream->stream);
		if (!stream->temp_path.empty()) unlink(stream->temp_path.c_str());
		delete stream;
		return ZIPC_IO_FAILURE;
	}

	filenode node;
	node.size = data_size;
	node.data_offset = (size_t)local_offset + 30U + name_len;
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

	if (fseek(handle->fp, 0, SEEK_END) != 0) return ZIPC_IO_FAILURE;
	const long file_size_long = ftell(handle->fp);
	if (file_size_long < 0) return ZIPC_IO_FAILURE;
	const size_t file_size = (size_t)file_size_long;
	if (handle->central_dir_offset > file_size) return ZIPC_CORRUPT_ARCHIVE;

	std::vector<unsigned char> local(30);
	std::vector<unsigned char> buf(64 * 1024);

	for (const auto& kv : handle->files)
	{
		const std::string& name = kv.first;
		const filenode& node = kv.second;

		if (node.local_offset + local.size() > file_size) return ZIPC_CORRUPT_ARCHIVE;
		if (fseek(handle->fp, (long)node.local_offset, SEEK_SET) != 0) return ZIPC_IO_FAILURE;
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
		if (compressed_size != uncompressed_size) return ZIPC_UNSUPPORTED_FEATURE;
		if (name_len != name.size()) return ZIPC_CORRUPT_ARCHIVE;
		if (node.size != uncompressed_size) return ZIPC_CORRUPT_ARCHIVE;
		if (node.crc != crc) return ZIPC_CORRUPT_ARCHIVE;

		std::string name_on_disk(name_len, '\0');
		if (!read_fully(handle->fp, name_on_disk.data(), name_len)) return ZIPC_IO_FAILURE;
		if (name_on_disk != name) return ZIPC_CORRUPT_ARCHIVE;
		if (fseek(handle->fp, extra_len, SEEK_CUR) != 0) return ZIPC_IO_FAILURE;

		const size_t data_offset = (size_t)node.local_offset + 30U + name_len + extra_len;
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
			if (fseek(handle->fp, (long)data_offset, SEEK_SET) != 0) return ZIPC_IO_FAILURE;
			size_t remaining = node.size;
			while (remaining > 0)
			{
				const size_t chunk = remaining < buf.size() ? remaining : buf.size();
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
	size_t size_first = 0;
	size_t size_second = 0;
	size_t offset_first_diff = 0;
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
	size_t* offset_first_diff)
{
	assert(first);
	assert(second);
	assert(offset_first_diff);

	*offset_first_diff = 0;
	const size_t shared_size = std::min(first_node.size, second_node.size);
	if (fseek(first->fp, (long)first_node.data_offset, SEEK_SET) != 0) return ZIPC_IO_FAILURE;
	if (fseek(second->fp, (long)second_node.data_offset, SEEK_SET) != 0) return ZIPC_IO_FAILURE;

	size_t compared = 0;
	while (compared < shared_size)
	{
		const size_t chunk = std::min(shared_size - compared, first_buf.size());
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

		size_t offset_first_diff = 0;
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
				fprintf(out, "%s: %s at byte %zu (%zu vs %zu bytes)\n",
					name, zipc_diff_kind_string(diff.kind),
					diff.offset_first_diff, diff.size_first, diff.size_second);
				break;
			case ZIPC_DIFF_ONLY_IN_FIRST:
			case ZIPC_DIFF_ONLY_IN_SECOND:
				fprintf(out, "%s: %s (%zu vs %zu bytes)\n",
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
