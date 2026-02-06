ZIPC - an uncompressed zip file container library
=================================================

This is a very simple, high-performance library that only supports
reading and writing to uncompressed ZIP files, using them as containers
for other files that may themselves be compressed. We support streaming
directly both to and from such ZIP containers. As long as you only write
one file at a time, you can start writing files into the container before
you now how much you intend to write without any extra overhead. If you
need to write multiple files at once before you know their sizes, this is
also supported, but in this case there will be some copying overhead as
they are closed, although we use whatever OS tricks we can do reduce this
overhead.
