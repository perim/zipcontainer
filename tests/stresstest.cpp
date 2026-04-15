#include "zipc.h"

#include <assert.h>
#include <string.h>

#include <algorithm>
#include <random>
#include <string>
#include <unordered_map>
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

static std::vector<unsigned char> make_data(std::mt19937& rng, size_t size)
{
	std::vector<unsigned char> data(size);
	std::uniform_int_distribution<int> byte_dist(0, 255);
	for (size_t i = 0; i < size; ++i)
	{
		data[i] = (unsigned char)byte_dist(rng);
	}
	return data;
}

static void read_and_check(zipc* z, const std::string& name, const std::vector<unsigned char>& expected)
{
	assert(z);
	const ssize_t size = zipc_filesize(z, name.c_str());
	assert(size == (ssize_t)expected.size());

	std::vector<unsigned char> buf(expected.size());
	enum zipc_status r = zipc_read(z, name.c_str(), expected.size(), buf.data());
	assert(r == ZIPC_SUCCESS);
	assert(memcmp(buf.data(), expected.data(), expected.size()) == 0);
}

int main(int argc, char** argv)
{
	(void)argc;
	(void)argv;

	std::mt19937 rng(12345);
	std::uniform_int_distribution<int> size_dist(0, 4096);

	const char* zip_filename = "stress.zip";
	enum zipc_status r = ZIPC_SUCCESS;
	zipc* z = zipc_open(zip_filename, "w", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);

	assert(zipc_filesize(z, "missing.bin") == -1);

	std::unordered_map<std::string, std::vector<unsigned char>> files;
	files.reserve(128);

	// Write a bunch of regular files.
	for (int i = 0; i < 60; ++i)
	{
		const std::string name = "file_" + std::to_string(i) + ".bin";
		const size_t size = (size_t)size_dist(rng);
		std::vector<unsigned char> data = make_data(rng, size);
		r = zipc_write(z, name.c_str(), data.size(), data.data());
		assert(r == ZIPC_SUCCESS);
		files.emplace(name, std::move(data));
	}

	// Map-write some files.
	for (int i = 0; i < 10; ++i)
	{
		const std::string name = "map_" + std::to_string(i) + ".bin";
		const size_t max_size = 8192;
		zipc_mapping mapping = zipc_map_write(z, name.c_str(), &r, max_size);
		assert(r == ZIPC_SUCCESS);
		assert(mapping.data);

		const size_t actual_size = (size_t)(size_dist(rng) % (max_size + 1));
		std::vector<unsigned char> data = make_data(rng, actual_size);
		if (actual_size > 0)
		{
			memcpy(mapping.data, data.data(), actual_size);
		}
		r = zipc_unmap_write(z, mapping, actual_size);
		assert(r == ZIPC_SUCCESS);
		files.emplace(name, std::move(data));
	}

	// Stream-write some files.
	for (int i = 0; i < 10; ++i)
	{
		const std::string name = "stream_" + std::to_string(i) + ".bin";
		zipcstream* s = zipc_stream_open(z, name.c_str(), "", &r);
		assert(r == ZIPC_SUCCESS);
		assert(s);

		std::vector<unsigned char> data;
		for (int part = 0; part < 5; ++part)
		{
			const size_t part_size = (size_t)(size_dist(rng) % 1024);
			std::vector<unsigned char> chunk = make_data(rng, part_size);
			r = zipc_stream_write(z, s, chunk.size(), chunk.data());
			assert(r == ZIPC_SUCCESS);
			data.insert(data.end(), chunk.begin(), chunk.end());
		}
		r = zipc_stream_close(z, s);
		assert(r == ZIPC_SUCCESS);
		files.emplace(name, std::move(data));
	}

	// Reject duplicates.
	r = zipc_write(z, "file_0.bin", 1, "x");
	assert(r == ZIPC_PATH_ALREADY_EXISTS);

	assert(zipc_validate(z) == ZIPC_SUCCESS);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);

	// Reopen and validate contents.
	z = zipc_open(zip_filename, "r", &r);
	assert(z);
	assert(r == ZIPC_SUCCESS);

	for (const auto& kv : files)
	{
		read_and_check(z, kv.first, kv.second);
	}

	// Map-read a few random files.
	for (int i = 0; i < 5; ++i)
	{
		const std::string name = "file_" + std::to_string(i) + ".bin";
		auto it = files.find(name);
		assert(it != files.end());

		zipc_mapping mapping = zipc_map_read(z, name.c_str(), &r);
		assert(r == ZIPC_SUCCESS);
		assert(mapping.data);
		assert(mapping.size == it->second.size());
		assert(memcmp(mapping.data, it->second.data(), it->second.size()) == 0);
		zipc_unmap_read(z, mapping);
	}

	assert(zipc_validate(z) == ZIPC_SUCCESS);
	r = zipc_close(z);
	assert(r == ZIPC_SUCCESS);
	return 0;
}
