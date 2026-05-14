# ZIPC - an uncompressed zip file container library

This is a very simple, high-performance library for reading and writing
to uncompressed ZIP files, using them as containers for other files that
may themselves be compressed. We support streaming directly both to and
from such ZIP containers.

As long as you only write one file at a time, you can start writing files
into the container before you now how much you intend to write without
any extra overhead. If you need to write multiple files at once before
you know their sizes, this is also supported, but in this case there will
be some copying overhead as they are closed, although we use whatever OS
tricks we can do reduce this overhead and reduce the chance of running
out of disk space.

A quick example of how it can be used:

```cpp
	enum zipc_status result;
	zipc* fp = zipc_open("myfile.zip", "r", &result);
	assert(result == ZIPC_SUCCESS);
	uint64_t filesize = 0;
	result = zipc_filesize(fp, "assets/image.png", &filesize);
	assert(result == ZIPC_SUCCESS);
	char* ptr = malloc(filesize);
	result = zipc_read(fp, "assets/image.png", filesize, ptr);
	assert(result == ZIPC_SUCCESS);
	zipc_close(fp);
```

## To be done

- Mutating writes are not transactional: `zipc_write`, `zipc_map_write`
  / `zipc_unmap_write`, and `zipc_stream_close` may leave an existing
  archive corrupted if an I/O failure occurs after the old central
  directory has been truncated.
