#include "zipc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <cstdint>
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

// Private definitions

enum zipc_mode
{
	ZIPC_READ_ONLY,
	ZIPC_WRITE_ONLY,
	ZIPC_APPEND,
};

static uint16_t read_le16(const unsigned char* ptr)
{
	return (uint16_t)ptr[0] | ((uint16_t)ptr[1] << 8U);
}

static uint32_t read_le32(const unsigned char* ptr)
{
	return (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8U) | ((uint32_t)ptr[2] << 16U) | ((uint32_t)ptr[3] << 24U);
}

static uint32_t crc32_calc(const void* data, size_t size)
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

	uint32_t crc = 0xFFFFFFFFU;
	const unsigned char* p = static_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; ++i)
	{
		crc = table[(crc ^ p[i]) & 0xFFU] ^ (crc >> 8U);
	}
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
};

struct zipcstream
{
	zipc* parent = nullptr;
};

// Implementations

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

void zipc_close(zipc* handle)
{
	if (!handle) return;
	if (handle->fp) fclose(handle->fp);
	delete handle;
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

	// Rewrite central directory with all entries
	std::vector<std::string> names;
	names.reserve(handle->files.size());
	for (const auto& kv : handle->files) names.push_back(kv.first);
	std::sort(names.begin(), names.end());

	const long cd_start_long = ftell(handle->fp);
	if (cd_start_long < 0) return ZIPC_IO_FAILURE;
	uint32_t cd_start = (uint32_t)cd_start_long;

	for (const auto& name : names)
	{
		const filenode& n = handle->files.at(name);
		if (n.size > 0xFFFFFFFFULL) return ZIPC_UNSUPPORTED_FEATURE;
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
